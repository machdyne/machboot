# Zeitlos under machboot

With machboot, Zeitlos's gateware starts at 0x060000 instead of
0x040000 (docs/flash-layout.md). Zeitlos itself boots unchanged --
machboot starts it with PROGRAMN from the system slot, and Zeitlos's own
`reboot` comes back to machboot -- but two places in Zeitlos assumed the
tinydfu layout. `zeitlos-patches/` has the changes, against Zeitlos
`ede46a8`, as `git format-patch` files:

```
cd zeitlos
git am ../machboot/zeitlos-patches/*.patch
```

## 0001: the kernel and zfpga learn machboot's layout

**The write fence.** Zeitlos's flash controller locks 0x000000-0x03FFFF
(tinydfu). Under machboot, 0x040000-0x05FFFF is machboot's firmware and
settings, and Zeitlos could have written it. The controller cannot tell
the layouts apart (the gateware is the same either way), so the kernel
does: when the flash holds machboot's firmware header ("MBFW" at
0x050000), `Z_SYS_FLASH` refuses erases and programs below 0x060000, and
`Z_FLASH_INFO` reports 0x060000 as the lock end (`sw/os/flashapi.c`,
`k_flash_lock_end()`). On a tinydfu board, 0x050000 is inside Zeitlos's
own bitstream and never holds that header, so nothing changes there.

machboot's own fence still protects machboot's **bitstream** in
hardware whatever runs: Zeitlos's controller locks 0x000000-0x03FFFF, and
that is where it is.

**`zfpga flash`'s free space.** It put user gateware "after Zeitlos's
own gateware, up to 0x0F0000", finding Zeitlos's gateware at 0x040000 or
at 0. Under machboot it found machboot's bitstream at 0 and offered
0x040000 onwards -- machboot's firmware and Zeitlos's own gateware. It
now finds the gateware at 0x060000 when machboot's header is there
(`sw/apps/zfpga/boot.c`), and the kernel's lock end keeps it above
0x060000 anyway. `zfpga-simflash` models the same, and
`sw/apps/zfpga/tests/run.sh` has a machboot case.

## 0002: zfpga's flash tests

Unrelated to machboot, found on the way: `tests/mkflash.py` put the core
apps' archive at 0x140000 while `boot.c` reads it at 0x128000, so the
whole `flash / run` block of `tests/run.sh` failed (16 checks) at
`ede46a8`. With the fixture corrected, all 122 checks pass, the machboot
case included.

## Checked

- `sw/apps/zfpga/tests/run.sh`: 122 passed, 0 failed (4 skipped: tools
  not installed here).
- `sw/os/flashapi.c` compiles with the RISC-V toolchain. The kernel
  change itself has not run on hardware or in Zeitlos's simulator.

## Not needed

- Zeitlos's boot logo used to live at 0x0F0000, inside what is now
  machboot's system slot; Zeitlos no longer has one (`sw/bios/bios.c`),
  so nothing there is overwritten.
- No other Zeitlos app writes this flash: `zfpga` is the only user of
  `Z_SYS_FLASH` besides the kernel's key/value store, and `update` leaves
  the kernel and core apps in flash alone.
