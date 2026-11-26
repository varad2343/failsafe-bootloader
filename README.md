# Power-Fail-Safe A/B Bootloader

**Job profile: embedded firmware engineer**

Firmware update logic in C for a Cortex-M0+ (STM32G0B1) that always leaves a bootable image, even if power is lost during any flash operation.

> **Status: the update logic passes host tests; the STM32 port compiles but has not been tested on hardware. No UART transport is connected yet.**

## Design
- Two image slots and two record pages; each record carries a sequence number and CRC-32
- A new record is always written to the page that does not hold the newest one
- The image header is written only after the whole image passes its CRC check
- A new image runs on trial: if it does not call `boot_confirm()` within 3 boots, the old one returns
- Updates are refused while the running image is unconfirmed, so the fallback is never erased

## Verification
A flash model cuts power during every erase or program operation of a full update cycle (523 points), leaving the operation half done. After each cut the device must boot an intact image and accept another update. The model also rejects programming a location twice.

## Run
```bash
python run_tests.py        # needs GCC and Python
python build_target.py     # needs the Arm GNU Toolchain
```

## Limitations
A torn write on real STM32 flash can raise an ECC error on read; this port has no handler for that. Application images must be linked for the slot they are written to.
