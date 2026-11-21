# Native cabinet I/O and IIDX legacy subscreen

IIDX and SDVX use the standard **Buttons** page for binding inputs.
In IIDX, the separate **BIO2 Buttons** and **BIO2 Lights** pages configure
the physical board independently of the game's I/O interface and LDJ/TDJ mode.
The physical board can be **BI2X**, **BI2A**, or **IO2 / EZUSB FX2**.

| Physical cabinet | Game-side interface | Translation path |
| --- | --- | --- |
| BI2X | Legacy EZUSB / IO2 or BI2A | Native BI2X → input routes → game; game lights → output routes → BI2X |
| BI2A | EZUSB / IO2 or BI2X | Native ACIO BI2A → common game inputs; game lights → legacy lamp routes |
| IO2 (FX2) | BI2A or BI2X | Native CyUSB3 endpoints → common game inputs; game lights → legacy lamp routes |

Spice's existing game-side EZUSB and BIO2 adapters provide the game interfaces;
the native backend communicates with the selected physical board. This preserves
the game's existing device discovery and startup path.

## Setup

1. Select Beatmania IIDX in SpiceCfg and open **BIO2 Buttons**.
2. Enable **Native I/O hardware**, then select **Physical board**.
3. Open **I/O connection** to set the serial port or USB path. Empty means auto.
4. Click **Connect I/O**. BI2X normally resets for eight seconds; BI2A resets for
   about three seconds and discovers its ACIO node. Neither mode forces TDJ.
5. **Auto-bind matching inputs** assigns matching unbound keys, turntables, and
   supported effect sliders. To redirect a key, click **Bind** on its row and
   press a physical BIO2 button. The standard **Bind (many)**, **Add alternate**,
   **Unbind** and **Edit properties** actions are available on the same page.
6. Open **BIO2 Lights**. **Fill unbound outputs** maps matching available outputs.
   Hold **Test** to preview, then release to restore the latest game state.

Available physical channels depend on the board. BI2A and IO2 provide fourteen
key lamps, four panel lamps, eight spotlights, a neon lamp and a nine-character
Ticker. They do not provide Lightning Model RGB tapes or headphone detection.
Their two turntables and five 4-bit effect sliders are exposed as analog inputs.
Map a BI2X game's RGB channel to a traditional lamp to reduce it to on/off. A
traditional game lamp can instead drive a BI2X lamp or a uniform tape RGB channel.
Unavailable saved channels are labelled and do not drive nonexistent hardware.

### BI2X hardware

An empty port searches for USB `1CCF:8050`. Supply your own `bi2x_tdj.bin` and
`bi2x_sci.bin`, or `libaio-iob2_video.dll` and `libaio-iob.dll`. DLLs are parsed as
data, not executed. Modules are uploaded into volatile board memory; no runtime
modules or game files are distributed with this project.

### BI2A hardware

An empty port searches for USB `1CCF:804C` or `1CCF:8040`; an explicit port such
as `COM5` is also supported. The board must already have the appropriate IIDX
BI2A firmware installed. This backend does not flash persistent firmware. It
uses 115200-baud ACIO, discovers a BI2A node and initializes the sub-I/O relay.

### IO2 / EZUSB FX2 hardware

Requires the **CyUSB3** Windows driver and the correct IIDX FX2 firmware already
loaded on the board. The old CyUSB driver and the earlier FX1/C02 board are not
the native IO2 backend. An empty path selects the first device with Cypress's
FX2 interface GUID; if multiple boards are connected, enter the full real USB
device path. Do not use the game's virtual `\\.\Ezusb-0` path. The backend exchanges
64-byte packets on endpoints `0x01` and `0x81`. It does not distribute firmware
or perform a firmware upload to the physical IO2 board.

## Configuration and routing

All settings use the existing per-game XML configuration. The historical native
route identifier `BIO2:BI2X:TDJ` is retained across physical board selections, so
existing mappings keep their channel numbers. BI2X's original channels 0–83 stay
unchanged; legacy spotlights and neon occupy channels 84–92. Bindings remain
editable while disconnected. The backend is disabled until enabled. Examples:

```
-bi2x -bio2board bi2x -bi2xport COM5 -bi2xmodules C:\BI2XRuntime
-bi2x -bio2board bi2a -bi2xport COM6
-bi2x -bio2board io2
```

The original `-bi2x` enable flag and `-bi2xport` are kept for compatibility and
now apply to the selected physical board. `-bi2xskipreset` skips a serial board
reset. An invalid `-bio2board` value disables native startup and logs a warning.

`-bi2xtapemap` stores seventeen comma-separated physical destinations for full
BI2X RGB tape sources. Pixel data is preserved and different strip lengths are
scaled. For legacy games, map their individual light channels instead. The
BI2X driver's normal tape brightness limit remains in effect. A game's RGB tape
extraction option controls its scalar RGB channels; full BI2X tape routing does
not depend on scalar extraction being enabled.

Inputs are released on disconnect; the backend retries the connection. Closing
the connection requests a blackout before releasing the device. Hardware I/O
runs on a dedicated worker; game input reads use cached snapshots. ACIO framing
uses fixed buffers, its reply storage is reused, and IO2 reuses its completion
event. Normal BI2X tape updates do not allocate temporary vectors.

## Sound Voltex cabinet I/O

Select **Sound Voltex** in SpiceCfg. **BIO2 Buttons** provides **Auto-bind
matching inputs**, **Bind**, and **Bind (many)**. Auto-bind fills matching
unbound keys and both knobs. **BIO2 Lights** provides remapping and output tests.
The regular **Buttons** page remains available for keyboard and controller bindings.

| Physical board | Game-side adapters | Native controls |
| --- | --- | --- |
| SDVX BI2A | KFCA in SDVX 1–4, BI2A, or UFC/BI2X in Valkyrie mode | Keys, two knobs, key lamps, paired RGB wing lamps, woofer, controller, generator, pop and title lamps |
| SDVX KFCA | KFCA, BI2A, or UFC/BI2X in Valkyrie mode | Keys, two knobs, key lamps and original RGB outputs |
| IIDX Lightning BI2X | KFCA, BI2A, or UFC/BI2X in Valkyrie mode | Physical IIDX buttons, two turntable inputs, and remappable BI2X lamps |
| IIDX Standard BI2A | KFCA, BI2A, or UFC/BI2X in Valkyrie mode | Physical IIDX buttons, two turntable inputs, key lamps and traditional cabinet lights |

Check **Native I/O hardware**, select the board, and set **Port / USB path**
to the physical COM port. KFCA uses COM3 by default at 57600 baud. BI2A uses
115200 baud and an installed SDVX BI2A firmware. Keep the game's virtual
I/O enabled. Example launch options:

```
-sdvxnativeio -sdvxioboard bi2a -sdvxioport COM5
-sdvxnativeio -sdvxioboard kfca -sdvxioport COM3
-sdvxnativeio -sdvxioboard bi2x -sdvxioport COM5 -sdvxbi2xmodules C:\BI2XRuntime
-sdvxnativeio -sdvxioboard iidx-bi2a -sdvxioport COM5
```

Choose **BI2X / Lightning Model** for IIDX TDJ firmware and use the same TDJ
and SCI runtime files as the IIDX driver. Choose **BI2A / IIDX Standard** for
the IIDX LDJ firmware. The **SDVX BI2A** option selects SDVX cabinet firmware.
With either IIDX firmware, **Apply SDVX layout** in **BIO2 Buttons** assigns
BT-A through FX-R to P1 keys 1 through 6, Start to P1 Start,
and VOL-L/R to the P1/P2 turntable inputs. It fills unbound inputs and can
replace existing BIO2 primary bindings. **Bind** lets you choose a different
physical button. **Fill unbound outputs** pairs SDVX key lamps with the same
physical keys. RGB game lights can be routed to individual physical channels.

The BI2A and KFCA backends have synthetic serial tests. Physical-board and
per-version game testing is pending. The Valkyrie UFC game adapter is available
as a destination for mapped legacy hardware. Support for the physical
Valkyrie BI2X/UFC board is pending its native communication and initialization
protocol. Only the physical board's lamp channels are available when remapping.

## IIDX legacy subscreen

With native BI2X, Spice opens a legacy subscreen in non-TDJ mode on Windows 8
or newer. Cannon Ballers and Rootage can also open it if the cabinet PC is
detected as a Lightning Model. Use `-iidxnoautolegacysub` to disable automatic
startup, or `-iidxlegacysub` to request it explicitly. Set
`-iidxlegacymonitor \\.\DISPLAY2` to choose the secondary display.
Spice keeps the subscreen above other windows without taking game focus.

Spice also checks the game working directory for a standalone
[iidx_btools_subscreen](https://github.com/cloud11665/iidx_btools_subscreen)
DLL at launch. It tries `backend32.dll` with `spice.exe` or `backend64.dll`
with `spice64.exe`, then `backend.dll` if the first file is missing or fails
to load. The DLL must match the launcher's bitness. Finding a candidate can
start the subscreen even without native BI2X. If the original cannot start,
Spice opens the built-in screen. `-iidxnoautolegacysub` also disables this
automatic startup. `-iidxlegacysub` still requests startup explicitly.
The original DLL is supplied separately.

The built-in screen offers touch keypads for both players, the Ticker, five
effect faders, Concentration Mode, card insertion, settings and coin insertion.
Display changes cause the screen to reopen on the selected display.
Screen faders take over from the hardware individually when touched. The
IIDX IO Panel remains available through its own overlay toggle.

Existing launch scripts may still contain `-iidxlegacybackend`; Spice ignores
its path. Use `-iidxlegacysub` for explicit startup.

## IIDX 26 cabinet troubleshooting

- Leave **Enable Device Passthrough** off with native BIO2 routing. The game
  still needs Spice's virtual FM serial interface on COM2. If you see
  `ERROR (FM SERIALPORT OPEN)`, check for `-devicehookdisable` in launch arguments
  and saved options.
- The ICCC reader has its own ACIO serial connection. With the reader physically
  connected to COM1, set **NFC Card Readers → ICCA Reader Port** to `COM1`
  (`-reader COM1`). The reader and native BI2X board need distinct physical
  ports. With one configured reader port, IIDX checks for a second ACIO reader
  at node 2 and routes the two nodes to P1/P2. For separate physical ports, pass
  `-reader COM1 -reader COMn` in P1/P2 order. The log reports the assigned nodes
  and the player for each swipe.
- For crackling with WASAPI Exclusive, try `-exclusivebuffer 10`, then
  `-exclusivebuffer 20` if needed. A larger buffer adds audio latency.
  Force ASIO on 32-bit IIDX with Xonar now gives shared polling games a
  minimum 40 ms software queue. The driver keeps its own hardware period.
- To hide only the Spice swipe popup, turn on **General Overlay → Hide Card
  Inserted Notification** (`-hidecardtoast`).

## Tricoro FeliCa card handoff

Tricoro uses a 20-byte ICCA status and reads the card's technology flag alongside
the eight-byte UID. Spice keeps the UID available until the card is ejected or
the reader timeout expires. AIC e-amusement passes and Suica are delivered as
FeliCa cards using their original UID.

Some `LDJ-506163e5_c4122` executables force the older E004 card type before
contacting the card server. The separate **Restore FeliCa card detection** local
patch restores the game's FeliCa selection for the verified executable. Put
the matching JSON under `patches/local/`, enable it in **Patches**, and save
with automatic application. This supplements the existing patch list. The
card server must also support the corresponding card type.

## Optional ASIO for IIDX 18–26
Enable **IIDX Force ASIO** in the Game Options or pass `-iidxforceasio`.
Spice prefers the Xonar driver registered for the executable's bitness.
To select another driver, use **IIDX ASIO Driver**. A driver selected in the
existing **WASAPI Exclusive to ASIO Conversion** option takes precedence.

This uses the existing ASIO backend for WASAPI exclusive audio and shared
audio, including both event-driven and polling clients.
The flag overrides the shared/low-latency WASAPI switches only when a
matching ASIO driver can be selected. Look for `Forcing IIDX WASAPI audio`
and `audio::asio` in the startup log. The ASIO driver controls its own buffer
size. Shared-mode clients such as Tricoro retain their requested mixing
buffer, with at least two driver periods of headroom. The log records both sizes
and reports any callback underruns when the stream stops. Hardware playback and
round-trip latency still require cabinet testing.

## Validation

The portable tests use arbitrary synthetic module payloads and a serial-board
simulator. They require no game DLLs, real firmware or physical hardware:

```
cmake -S tests/bio2 -B cmake-build-bio2-tests
cmake --build cmake-build-bio2-tests
ctest --test-dir cmake-build-bio2-tests --output-on-failure
```

The BI2X simulator checks both CRC variants, module transfer, noisy/dropped replies,
lost execution replies, RGB routing and automatic reconnection. The BI2A serial
simulator checks ACIO framing, checksums, sequence validation, node discovery and
sub-I/O initialization. IO2 tests exercise the actual polling and packet codec
through a POSIX endpoint simulator; the CyUSB3 kernel driver is compile-checked,
not executed on macOS. All three test lamps, temporary testing, blackout and
input release.
Windows 32/64-bit driver builds and changed application translation units are
checked separately. The standard release build remains `./build_docker.sh`.

The protocol driver is vendored from `dumcatt/iidxio-bi2x` under the Unlicense;
its source revision and local changes are recorded in `external/bi2x/UPSTREAM`.

## macOS build environment

Docker CLI, Buildx and Colima are installed on the development machine. The
project's `spice-arcade` VM uses 4 CPUs, 6 GiB RAM and Rosetta for x86_64 Linux
containers. To start it and build from `src/spice2x`:

```
colima start --profile spice-arcade --activate=false --ssh-config=false
DOCKER_CONTEXT=colima-spice-arcade ./build_docker.sh
```

The build script selects `linux/amd64` for both images and the build container.
On Apple Silicon it enables pacman's emulation compatibility setting inside
the dependency image. `SPICE_DOCKER_EMULATED=1` selects the same setting on other
hosts using user-mode CPU emulation. Native x86_64 builds keep pacman's default.
Distribution source archives contain the actual working tree, including local
edits and new source files.
