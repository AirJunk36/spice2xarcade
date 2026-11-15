# Protocol references

The BI2A/IO2 packet layouts and initialization behavior are based on public
Unlicense sources from `djhackersdev/bemanitools`, pinned at
`5ae0b175170d9ae657b7634ca514b7848923401e`:

- `src/main/bio2/bi2a-iidx.h` and `src/main/bio2drv/bi2a-iidx.c`
- `src/main/acio/acio.h` and `src/main/aciodrv/device.c`
- `src/main/ezusb2-iidx/msg.h` and `src/main/iidxio-ezusb2/iidxio.c`
- `src/main/ezusb2/ezusb2.h` and `ezusb2.c`
- `doc/iidxhook/iidxio-ezusb2.md`

Source: https://github.com/djhackersdev/bemanitools/tree/5ae0b175170d9ae657b7634ca514b7848923401e

The project's Unlicense dedication is reproduced in `licenses.txt`, alongside
the separately vendored native BI2X driver. The CyUSB3 SINGLE_TRANSFER structure
and IOCTL number implement the Cypress driver ABI; the original definition is
documented in bemanitools' `src/main/ezusb2/cyioctl.h` (Cypress, 2011).

The new C++ backend adds bounded receive parsing, address/sequence/checksum
validation, cancellation, cached inputs, remapping and reconnection. No
Bemanitools DLL, kernel driver, game data or proprietary firmware is bundled.
BI2A persistent firmware and IO2 volatile firmware must be prepared by the user.

The legacy subscreen uses Spice2x's existing Ticker state and DSEG font resource.
It is an independent implementation, not a port of `iidx_btools_subscreen`.
DSEG retains the existing SIL Open Font License in `external/dseg/DSEG-LICENSE.txt`.
