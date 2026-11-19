# Installing a system from the card

The Install page lists the systems on the card; **Enter** installs the
selected one into the system slot (0x060000-0x0FFFFF). `fw/install.c`.

## Steps

1. **Check.** The file must start like an ECP5 bitstream (header,
   preamble, Lattice TN1260), its VERIFY_ID must be this FPGA, and it
   must fit the slot (640 KB, up to the Zeitlos region). Otherwise
   nothing is written and the page says why: "the file is for another
   FPGA", "is not a bitstream", "is too big for the slot".
2. **Compare.** The file is read 4 KB at a time and compared with what
   the slot holds; its CRC-32 is computed on the way. If every sector is
   the same, **nothing is written** ("was already installed").
3. **Invalidate.** The slot's first sector is erased. From now on, until
   step 5, the slot does not start with a bitstream.
4. **Write** every other sector that differs, and read each back.
5. **Write the first sector last**, and read it back.
6. **Record** the name, version, CRC-32 and size in the settings
   (docs/settings.md).

## A power cut during an install

Because the first sector is written last, an install cut short leaves a
slot that machboot does not try to start -- never half a bitstream that
looks whole (which the FPGA would fail to load, with no way back but a
power cycle). The settings still name the system that was being
installed, so the next start shows:

```
Zeitlos did not finish installing.

► Install Zeitlos again                    from card

Nothing else was changed.
```

Enter installs it again. Tested with a simulated power cut part-way
through (tests/run_host.py, "power_cut" and "recover").

## Wear

Only sectors whose contents change are erased (step 2 finds them), and an
all-0xFF page is not programmed. Installing the same version again writes
nothing; an update writes the sectors that differ, plus the first one.

## When installing is not offered

- In **install mode** (machboot running from tinydfu's area at 0x040000)
  the system slot is inside machboot's own image.
- In the **JTAG test build** the flash is whatever the board had --
  usually tinydfu and Zeitlos, whose gateware covers 0x060000.

The page says so instead of installing.

## The flash controller's part

`rtl/zeitlos/spiflash.v` (from Zeitlos) erases 4 KB sectors and programs
256-byte pages, each after an ARM write, and refuses anything below
0x060000 except the settings window 0x05E000-0x05FFFF -- so no install,
whatever went wrong in the firmware, can touch the boot slot.
