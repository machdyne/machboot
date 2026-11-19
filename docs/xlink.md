# The XA-XD link (Sergei, Mozart)

Four wires between the Sechzig module's FPGA and the carrier's RP2040,
the module's "extra link" (github.com/machdyne/sechzig, README, "Xx").
machboot uses them as an SPI bus with the RP2040 as master, so the RP2040
can ask machboot for things -- today, the SD card's sectors for the
card reader; next, settings and installs.

## The wires

| Signal | Role | RP2040 | FPGA, ML0/ML1 | FPGA, ML2 |
|---|---|---|---|---|
| XA | MOSI | GP11 (SPI1 TX) | A4 | N1 |
| XB | SCK | GP10 (SPI1 SCK) | A3 | P1 |
| XC | MISO | GP8 (SPI1 RX) | B3 | R1 |
| XD | CS | GP9 (a GPIO) | A2 | R2 |

The FPGA balls are the modules' own pad nets (`pcb/ml1_v2`, `pcb/ml2_v0`).
The roles are chosen so that the RP2040 never drives XC: Zeitlos's ML1
images put their UART's TX there (and RX on XD, an input to them).

The RP2040 drives XA, XB and XD only while machboot answers. Otherwise
all four are inputs, and it probes -- one 6-byte STATUS frame -- only in
a short window after something that may have put machboot in the FPGA:
its own load of the menu, or the end of a PC's JTAG session (12 probes
over 4 s, then quiet). If the first byte back is not 0x5A, it lets go
again. While machboot answers it polls STATUS every 20 ms; when a reset
comes (machboot's Start, a reboot) it lets go at once, so a system that
uses these balls -- Zeitlos's ML1 builds have their console on XC/XD --
never sees a frame (tested: 10 s of a running system, not one).
The FPGA side has pull-ups on XA, XB and XD, and drives XC only while
CS is low.

## The wire protocol (rtl/mb_xlink.v)

SPI mode 0, MSB first, **8.93 MHz** (the RP2040's 125 MHz / 14). The gateware samples the pins at its 48 MHz system
clock through two flip-flops. `tests/xlink_tb.cc` runs the RTL against a
master with real-time edges and a 6 ns pad delay: it works to 10.4 MHz
and fails at 12.5 MHz.

A frame is CS low ... CS high. Byte 0 is the command; the slave sends
0x5A meanwhile.

| Command | Then |
|---|---|
| 0x01 STATUS | the slave sends FLAGS, STATE (bit 0 its receive buffer is full, bit 1 a reply is ready), TXLEN low, TXLEN high (3 bits; 0 = 2048), RXSEQ |
| 0x02 SEND | the master's bytes fill the 2 KB receive buffer; it counts only if the buffer was empty when the frame began (RXSEQ + 1) |
| 0x03 RECV | the slave sends the 2 KB reply buffer from 0; the reply is taken once all of it went out |

FLAGS is a byte machboot sets for the RP2040 to see without a message:
bit 0 the card is shared; bit 1 machboot is about to start the system
(the RP2040 answers with START); bit 2 the settings changed (the RP2040
GETs them); bit 3 machboot asks for something (the RP2040 sends REQ,
docs/systems.md); bits 7:4 count the changes of bit 0.

One request at a time: STATUS until there is room, SEND, STATUS until a
reply is ready, RECV. A reply nobody collected (a call that gave up) is
drained before the next request.

## The messages (fw/xlink.c, rp2040/src/msc.c)

```
request   [op][tag][pc][0] then the op's arguments     pc bit 0: a PC has configured the RP2040's USB
reply     [op|0x80][tag][status][n] then the op's data  status 0 ok, 1 no medium, 2 I/O error, 3 bad request
```

| op | Request | Reply |
|---|---|---|
| 1 HELLO | -- | [shared][0][0][0][sectors, 4 bytes LE] |
| 2 READ | [lba, 4 LE][count 1-3][0][0][0] | n = count, count x 512 bytes |
| 3 WRITE | as READ, then count x 512 bytes | n = sectors written |
| 4 EJECT | -- (the PC ejected the drive) | -- |
| 5 START | -- (the RP2040 saw FLAGS bit 1: it lets the next reset boot the module's flash) | -- |
| 6 SET | the settings the RP2040 keeps, as text (docs/power-on.md) | -- |
| 7 GET | -- (FLAGS bit 2) | the settings as text |
| 8 REQ | -- (FLAGS bit 3) | [kind][0][0][0] and its arguments (below) |
| 9 CHUNK | [off, 4][n, 4], n up to 1536 | n bytes of the bitstream a PUT copies; status 4 when the user cancelled |
| 10 DONE | [kind][result][0][0] and the outcome's data | -- |
| 11 NOTE | text: a system the RP2040 could not start, and why | -- |

The kinds of request (machboot's, fw/xlink_proto.h, shared by both sides):

| kind | Arguments | DONE's data |
|---|---|---|
| 1 LIST | -- | [n][0][0][0][free][largest gap][size], then n items of 52 bytes: [crc][len][off][name 24][ver 16] |
| 2 PUT | [len][name 24][ver 16] | [crc]; meanwhile the RP2040 CHUNKs the bitstream in order |
| 3 RUN | [crc] | -- ; then the RP2040 loads that system into the FPGA over JTAG |
| 4 DROP | [crc] | -- |

Results: 0 ok, 1 no room, 2 error, 3 not found, 4 cancelled. The
RP2040 serves one request at a time and nothing else meanwhile; its card
reader reports NOT READY (becoming ready) for the few seconds a PUT
takes.

Three sectors per request because 3 x 512 plus the header fits the 2 KB
buffers. The RP2040's mass-storage buffer is 1536 bytes for the same
reason, so one USB callback is one request.

## The card reader

On Lakritz and Konfekt the FPGA is the USB device. On Sergei and Mozart
the USB-C socket is the RP2040's, so the card reader is split:

```
PC --USB--> RP2040: mass storage (rp2040/src/msc_tusb.c, msc.c)
              --XA-XD--> FPGA: rtl/mb_xlink.v
                           --> machboot: fw/xlink.c --> the SD card
```

The PC sees a card reader ("Machdyne SD card reader") as long as the
RP2040 runs `machboot-sv.uf2`. There is a card in it while machboot's
Card reader page shares the card; the menu is the same as on Lakritz,
and so is ejecting on the PC (the card is machboot's again, and its
files are read anew).

When something other than machboot is in the FPGA, the reader is empty.
If the RP2040 runs other firmware, the Card reader page says that it
needs `machboot-sv.uf2`.

**Speed.** Reading a run of sectors -- what the PC does with files and
with the FAT -- three stages work at once:

- machboot reads the next three sectors from the card while the RP2040
  takes the current reply (`fw/xlink.c`, read-ahead);
- the RP2040 fetches the next three over the link while its USB hardware
  sends the current ones to the PC (`rp2040/src/msc.c`, fetch-ahead,
  from the main loop);
- the SD card runs at 24 MHz, and sectors move a word at a time.

The whole-SoC simulation, with USB's own time (1.3 ms per 1.5 KB at full
speed) modelled alongside: **about 740 KB/s**; on a Sergei ML2 the PC
sees about 0.7 MB/s. The link's wire is the slowest stage (1.4 ms per 1.5 KB at 8.93 MHz); full-speed USB's
ceiling is about 1.1 MB/s. A write, an eject, or a change of sharing
makes both sides forget what they read ahead (tested: a write into
either side's read-ahead, then a read, gives the new data).

The Card reader page shows what has moved, and the rate while it moves.

## Tested

- `make xlink-test`: the RTL alone, 300 random exchanges, full-size
  messages both ways, a reply cut short, a SEND into a full buffer.
- `make sv-test`: the RP2040's `xlink.c` and `msc.c` against the FPGA
  firmware's own `fw/xlink.c`, through a byte-level model of the RTL:
  no machboot (pins let go), not shared, UNIT ATTENTION, reads across
  requests, writes, a slow card, a call that gives up and the one after,
  eject, the menu taking the card back.
- `make host`: the menu's side (`xl_*` cases in tests/run_host.py).
- `make sim`, `sim-xl`: **all of it together at gate level**, a Sergei
  ML2 SoC with the Sechzig firmware, the RP2040's own code driving
  XA-XD, the SD controller and card: shared from the menu over the
  serial port, read (also 120 sectors in sequence with USB's time
  modelled, timed, and where the time went), written, ejected.
