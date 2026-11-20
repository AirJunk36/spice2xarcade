"""Exercise the real backend against synthetic BI2A, KFCA and IO2 peers.

The IO2 POSIX transport carries endpoint numbers and 64-byte packets over a pty;
it tests the polling/codec path, not the Windows kernel driver or USB hardware.
"""
import errno
import os
import pty
import select
import struct
import subprocess
import sys
import threading
import time


def frame(address, command, sequence, data):
    packet = bytes([address, command >> 8, command & 255, sequence, len(data)]) + data
    packet += bytes([sum(packet) & 255])
    return b'\xaa' + b''.join(bytes([255, b ^ 255]) if b in (170, 255) else bytes([b])
                              for b in packet)


def run(executable, kind):
    sdvx = kind.startswith('sdvx-')
    kfca = kind == 'sdvx-kfca'
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    stopped = threading.Event()
    history = []
    failures = []
    first_poll = None
    initialized = False
    poll_count = 0

    def serve():
        nonlocal first_poll, initialized, poll_count
        buffer = bytearray()
        packet = bytearray()
        started = escaped = False
        try:
            while not stopped.is_set():
                if not select.select([master], [], [], .05)[0]:
                    continue
                try:
                    chunk = os.read(master, 4096)
                except OSError as error:
                    if error.errno == errno.EIO:
                        continue
                    raise
                if kind == 'io2':
                    buffer.extend(chunk)
                    while buffer:
                        endpoint = buffer[0]
                        if endpoint == 1:
                            if len(buffer) < 65:
                                break
                            history.append(bytes(buffer[1:65]))
                            del buffer[:65]
                        elif endpoint == 129:
                            del buffer[0]
                            if first_poll is None:
                                first_poll = time.monotonic()
                            age = time.monotonic() - first_poll
                            if 1.2 < age < 2.0:
                                continue
                            data = bytearray(64)
                            data[8:12] = struct.pack('<I', ~(0x31 | (1 << 16)
                                | (1 << 29) | (1 << 30)) & 0xffffffff)
                            data[13:18] = bytes([255, 170, 0x8f, 0x16, 3])
                            os.write(master, data[:11])  # Fragmented transport read.
                            os.write(master, data[11:])
                            poll_count += 1
                        else:
                            raise AssertionError(f'unexpected endpoint: {endpoint:x}')
                    continue
                for value in chunk:
                    if value == 170 and not escaped:
                        if not started or not packet:
                            os.write(master, b'\xaa')
                        started = True
                        packet.clear()
                        continue
                    if not started:
                        continue
                    if escaped:
                        value ^= 255
                        escaped = False
                    elif value == 255:
                        escaped = True
                        continue
                    packet.append(value)
                    if len(packet) < 5 or len(packet) != 6 + packet[4]:
                        continue
                    request = bytes(packet)
                    packet.clear()
                    started = False
                    assert (sum(request[:-1]) & 255) == request[-1], 'request checksum'
                    address, hi, lo, sequence, size = request[:5]
                    command = (hi << 8) | lo
                    payload = request[5:-1]
                    if command == 1:
                        assert address == 0 and payload == b'\0'
                        response = b'\1'
                    elif command == 2:
                        response = bytes(8) + (b'KFCA' if kfca else b'BI2A') + bytes(32)
                    elif command == 3:
                        response = b'\0'
                    elif command == 0x100:
                        assert not kfca and payload == (b'\x3b' if sdvx else b'\x2d')
                        initialized = True
                        response = b'\0'
                    elif command == 0x120:
                        assert kfca and payload == struct.pack('>H', 6000)
                        initialized = True
                        response = b'\0'
                    elif command == 0x113:
                        assert sdvx and initialized and size == (24 if kfca else 40)
                        history.append(payload)
                        data = bytearray(16)
                        data[:4] = struct.pack('>HH', (257 << 6) | (0x34 if kfca else 0x0e),
                                               1023 << 6)
                        if kfca:
                            data[9], data[11] = 0x2a, 0x28
                        else:
                            data[9], data[10] = 0xab, 0x80
                        response = bytes(data)
                        poll_count += 1
                    elif command == 0x152:
                        assert initialized and size == 48
                        history.append(payload)
                        if first_poll is None:
                            first_poll = time.monotonic()
                        age = time.monotonic() - first_poll
                        if 1.2 < age < 2.0:
                            continue
                        data = bytearray(46)
                        data[1] = 14
                        data[9] = 128
                        data[16:18] = bytes([170, 255])  # Both reserved framing bytes.
                        data[18] = data[44] = 128
                        for offset, value in zip([0, 2, 4, 6, 7], [15, 8, 6, 1, 3]):
                            data[offset] = value << 4
                        response = bytes(data)
                        if poll_count == 0:
                            # Noise, an old sequence and a bad checksum must be ignored.
                            os.write(master, b'\x12\x34' + frame(address | 128,
                                command, (sequence - 1) & 255, response))
                            broken = bytearray(frame(address | 128, command, sequence, response))
                            broken[-1] ^= 1
                            os.write(master, broken)
                        poll_count += 1
                    else:
                        raise AssertionError(f'unexpected ACIO command {command:x}')
                    encoded = frame(address | 128, command, sequence, response)
                    os.write(master, encoded[:7])
                    os.write(master, encoded[7:])
        except BaseException as error:
            failures.append(error)

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    try:
        result = subprocess.run([executable, kind, path], timeout=15, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert not failures, failures
        if sdvx:
            assert poll_count > 30
            if kfca:
                lamp = lambda data, key: bool(data[2] & (0x20 << key))
                lit = lambda data: data[4] == 255 and data[23] == 128 and data[3] == 8
                dark = lambda data: data == b'\0\x40' + bytes(22)
            else:
                lamp = lambda data, key: data[12 + key] == 127
                lit = lambda data: data[31] == 127 and data[22] == 128 and data[23] == 255
                dark = lambda data: not any(data)
            assert any(lamp(data, 0) and lamp(data, 1) and lit(data) for data in history)
            assert any(lamp(data, 0) and not lamp(data, 1) and lit(data) for data in history)
            assert dark(history[-1]), 'SDVX shutdown blackout'
            print(f'{kind}: {poll_count} successful polls; keys, knobs, lamps and blackout passed')
            return
        assert poll_count > 30
        if kind == 'io2':
            lamp = lambda data, key: bool(data[11] & (1 << key))
            lit = lambda data: data[8] == 1 and data[14] == 1 and data[15] == 1
            ticker = lambda data: data[16:25]
            dark = lambda data: not any(data[8:16])
        else:
            lamp = lambda data, key: data[7 + key] == 128
            lit = lambda data: data[3] == 128 and data[32] == 128 and data[36] == 128
            ticker = lambda data: data[23:32]
            dark = lambda data: not any(data[3:21] + data[32:41])
        assert any(lamp(data, 0) and lamp(data, 1) and lit(data) for data in history), 'test overlay'
        assert any(lamp(data, 0) and not lamp(data, 1) and lit(data) for data in history), 'test restore'
        assert any(ticker(data) == b'IIDX TEST' for data in history), 'nine character ticker'
        assert dark(history[-1]), 'shutdown blackout'
        print(f'{kind}: {poll_count} successful polls; input, lamps, ticker, timeout, reconnect, blackout passed')
    finally:
        stopped.set()
        thread.join(timeout=1)
        os.close(slave)
        os.close(master)


for board in sys.argv[2:] or ('bi2a', 'io2'):
    run(sys.argv[1], board)
