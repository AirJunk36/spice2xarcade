"""Exercise the actual native backend over a simulated serial board.

The payloads below are arbitrary test bytes, never executable firmware.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for name, seed in (("tdj", 17), ("sci", 53)):
        (root / f"bi2x_{name}.bin").write_bytes(bytes((i * seed) % 256 for i in range(512)))
    for crc in ("lsb", "msb"):
        with subprocess.Popen(
            [sys.executable, str(here / "sim_bi2x.py"), str(root / "bi2x_tdj.bin"),
             str(root / "bi2x_sci.bin"), "--seconds", "13", "--blackout", "3,1.5"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**os.environ, "SIM_CRC": crc},
        ) as board:
            try:
                port = board.stdout.readline().strip()
                host = subprocess.run([sys.argv[1], port, directory], timeout=25)
                stdout, stderr = board.communicate(timeout=15)
                if host.returncode or board.returncode:
                    raise RuntimeError(f"{crc}: host={host.returncode}, board={board.returncode}: {stderr}")
                report = json.loads(stdout.strip().splitlines()[-1])
                assert report["error_count"] == 0, report
                if crc == "msb":
                    assert report["crc_probes"] > 0, report
                assert report["power_cycles"] >= 1, report
                assert report["loaded"] == ["tdj", "sci"], report
                assert report["host_encrypted_frames"] > 0, report
                outputs = [bytes.fromhex(value) for value in report["output_history"]]
                assert any(output[1] == 0x0A and output[5] == 0x01
                           and output[3:5] == bytes.fromhex("7c1f") for output in outputs), report
                assert bytes.fromhex(report["outputs"]) == bytes(17), report
                assert report["tape_written"] == 707 and report["commits"] > 0, report
                assert 0x7C00 in report["tape_seen"] and 0x03E0 in report["tape_seen"], report
                assert set(report["tape_sample"].values()) == {0}, report
                print(f"{crc}: native input, output, RGB routing and reconnection passed")
            finally:
                if board.poll() is None:
                    board.kill()
                    board.wait()
