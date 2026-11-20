# Protocol references

The BI2A, IO2 and KFCA packet layouts and initialization behavior are based on public
Unlicense sources from `djhackersdev/bemanitools`, pinned at
`5ae0b175170d9ae657b7634ca514b7848923401e`:

- `src/main/bio2/bi2a-iidx.h` and `src/main/bio2drv/bi2a-iidx.c`
- `src/main/acio/acio.h` and `src/main/aciodrv/device.c`
- `src/main/ezusb2-iidx/msg.h` and `src/main/iidxio-ezusb2/iidxio.c`
- `src/main/ezusb2/ezusb2.h` and `ezusb2.c`
- `doc/iidxhook/iidxio-ezusb2.md`
- `src/main/bio2/bi2a-sdvx.h` and `src/main/bio2drv/bi2a-sdvx.c`
- `src/main/acio/kfca.h` and `src/main/aciodrv/kfca.c`
- `src/main/sdvxio-bio2/sdvxio.c` and `src/main/sdvxio-kfca/sdvxio.c`

Source: https://github.com/djhackersdev/bemanitools/tree/5ae0b175170d9ae657b7634ca514b7848923401e

The project's Unlicense dedication is reproduced in `licenses.txt`, alongside
the separately vendored native BI2X driver. The CyUSB3 SINGLE_TRANSFER structure
and IOCTL number implement the Cypress driver ABI; the original definition is
documented in bemanitools' `src/main/ezusb2/cyioctl.h` (Cypress, 2011).

The new C++ backend adds bounded receive parsing, address/sequence/checksum
validation, cancellation, cached inputs, remapping and reconnection. No
Bemanitools DLL, kernel driver, game data or proprietary firmware is bundled.
BI2A persistent firmware and IO2 volatile firmware must be prepared by the user.

The built-in IIDX subscreen uses window, touch and ticker code adapted from
[`iidx_btools_subscreen`](https://github.com/cloud11665/iidx_btools_subscreen/tree/300916a7b481be8ff204570fd1e695e6ea36a35c).
It uses Spice game I/O, the existing DX11 renderer and original Ticker segment
geometry. The widget textures and font are replaced by Spice's bundled ImGui
controls. Both its MIT notice and the LED-Segment-ASCII MIT notice are in
`licenses.txt`. An explicit backend path still supports the standalone DLL.
