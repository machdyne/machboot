# USB host: keyboards, hubs and flash drives

machboot drives Zeitlos's USB host controller (`rtl/usb/`, Zeitlos
`docs/usb_host.md`) from `fw/usb.c`. It serves:

- **keyboards**, up to two: the menu works with them as with the serial
  console (`fw/console.c` turns their reports into the same key codes);
- **hubs**, up to two, one level deep: a hub on a host port, and what
  is plugged into it;
- **one flash drive** (USB mass storage, bulk-only, SCSI): its FAT32
  `\SYS\GATEWARE` is listed on Install next to the SD card's.

Everything runs from `usb_step()`, which `key_poll()` calls every time
round, as a set of small state machines that never block. Reading the
flash drive is the one exception: `usb_drive_read()` waits for its
sector, and keeps calling `usb_step()` while it does, so keyboards and
hubs carry on. No multitasking OS is needed. The controller has a single
transaction engine and the firmware has one owner for it at a time
(`xowner`): an enumerating device, a hub's status check, or the drive.

## Devices

Six entries: the two host ports, and four devices behind hubs. Each
device keeps one address for as long as it is plugged in (its index + 1),
including when it is tried again.

| Board page | |
|---|---|
| `USB 1` | on host port 1 |
| `  USB 1.3` | behind the hub on host port 1, its port 3 |

States: `-` (nothing), `starting ...`, `keyboard`, `hub`, `flash drive
NNN MB`, `not used` (a mouse, a printer, …), `not answering`, `no room
for it` (a third keyboard, a second drive, a third hub, or a hub behind
a hub). The serial console says the same as it happens:

```
usb 1: hub 05e3:0608, 4 ports
usb 1.2: keyboard 04c4:2001 (low speed, behind a hub)
usb 1.4: flash drive 0781:5581
usb: flash drive: 7633 MB
```

## Enumeration

| Step | |
|---|---|
| debounce | 100 ms after a connection is seen; then wait until no other device is on address 0 and the engine is free |
| reset | a host port: the controller resets it. Behind a hub: SET_PORT_FEATURE(PORT_RESET), poll GET_PORT_STATUS until C_PORT_RESET (500 ms at most), clear it, 10 ms recovery. The speed is read after the reset |
| GET_DESCRIPTOR (device, 8) | for bMaxPacketSize0 |
| SET_ADDRESS | the device's own address; 3 ms to settle |
| GET_DESCRIPTOR (device, 18), (configuration, 9, then all) | |
| classify | hub (class 9); keyboard (HID boot keyboard interface); flash drive (class 8, subclass 6, protocol 0x50, a bulk IN and a bulk OUT endpoint); otherwise not used |
| SET_CONFIGURATION | |
| a hub | GET_DESCRIPTOR (hub) for the port count; SET_PORT_FEATURE(PORT_POWER) on each port; wait bPwrOn2PwrGood |
| a keyboard | SET_PROTOCOL (boot), SET_IDLE (0); then the compat block's type is set, **then** a poll slot is enabled: the controller polls it by itself from then on |
| a flash drive | bound; it is first read when a list of files is wanted |

A failure retries three times, 200 ms apart. A device behind a hub that
still fails is given up, and **its hub port is disabled**, so that it
cannot answer on address 0 while another device is being set up there.

## Hubs

A hub's ports are read one after another with GET_PORT_STATUS, a full
round every 100 ms (the hub's
status-change endpoint is not polled; the engine is free most of the
time and this is simpler). Each change bit is cleared on its own
(C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_RESET, …). A connection starts
a new device; a disconnection tears down that device and everything
behind it. Unplugging the hub tears down all of its devices.

## Flash drives

Bulk-only transport, SCSI commands, 512-byte sectors:

| | |
|---|---|
| TEST UNIT READY | up to 30 tries; after each failure REQUEST SENSE (most drives say UNIT ATTENTION once after power-up) |
| READ CAPACITY (10) | the size; other sector sizes are not read |
| READ (10) | one sector at a time, two tries |

A command block wrapper, the data stage, and the status wrapper, whose
tag is checked. The data toggles are kept in software. A STALL clears
the halt (CLEAR_FEATURE(ENDPOINT_HALT)); a status that cannot be read
does the bulk-only reset recovery.

The drive is read only, by FAT32 (`fw/fat.c`, the same code as the SD
card) and only when Install or the card reader page lists files. On
Install, files from the drive say `(USB)`; installing from them is the
same as from the card. Nothing is ever written to a USB drive.

## The rules this follows

They come from Zeitlos's USB host driver, where each one was a bug found
on hardware (Zeitlos `docs/usb_host.md`, `sw/apps/usb/usbh.c`,
`usbh_hub.c`):

- one owner of the transaction engine at a time;
- only one device on address 0 at a time;
- one address per device, kept across retries;
- the hub does the port reset, and the host waits for C_PORT_RESET;
- a device that failed for good has its hub port disabled;
- a control transfer's data stage goes one packet per transaction; a
  NAK re-issues the same packet unchanged after 1 ms (up to 500 times);
- a short packet is success, not an error; the status stage's own
  result is checked;
- a low-speed device directly on a host port is LOWSPEED **and**
  INVERTED; behind a full-speed hub it is LOWSPEED **and** PRE; a
  full-speed device is neither;
- SET_PROTOCOL(boot) is sent explicitly;
- the compat block's type is written before the poll slot is enabled.

## Keys

The controller keeps each keyboard's current boot report in a compat
block: the modifier byte, and up to four held keys. A key that appears
is a press; the newest held key repeats after 400 ms, every 50 ms.

| USB key | menu key |
|---|---|
| arrows, Enter (both), Esc, Backspace, Tab, Delete, F1-F12 | the same |
| letters, digits, space | the character (Shift: upper case) |

**Keyboard layout** (Settings): US or Deutsch. For the menu the only
difference that matters is Y and Z, which German keyboards swap.

## Not yet

- Keyboard LEDs (Caps Lock etc.) are not set.
- Hubs behind hubs, and a hub's low-speed transaction translator for
  full-speed devices on a high-speed hub: a high-speed hub talks full
  speed to machboot, as USB 2.0 requires, so this works without one.
- Writing to a flash drive (not needed: machboot only installs from it).

## Testing

The PC build (`fw/host/usbh_model.inc`) models the controller with a
hub, low-speed keyboards and mice, and a flash drive holding a FAT32
image. It fails a test when two devices answer the same address, when a
data toggle is wrong, or when a device is addressed with the wrong
speed/polarity. See docs/testing.md.
