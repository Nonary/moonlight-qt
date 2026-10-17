# Steam Controller (2026) over USB and the wireless puck

Moonlight drives a 2026 Steam Controller ("Ibex") itself when it is connected to the client PC
by USB cable (`28de:1302`) or through its wireless puck (`28de:1304`, up to four controllers).
The host sees a Steam Controller (`LI_CTYPE_STEAM`) with both trackpads, gyro and accelerometer,
grip and stick touch sensors, battery state, the physical pad and trigger clicks, Steam's own
haptic reports relayed to the real pads, and the real unit's firmware attributes. The reference
implementation is the Android client's USB/puck path (`SteamControllerUsb.java` and the shared
`SteamControllerInput.java`); this port follows it byte for byte on the wire.

Code: `app/streaming/input/steamcontroller/` (decoder, attribute parser, HID link, SDL hidapi
transport, host bridge). Tests: `tests/steamcontroller/`.

## What works and what does not

| Transport | Status |
|---|---|
| USB cable (`28de:1302`, HID interface 0) | supported |
| Wireless puck (`28de:1304`, HID interfaces 2..5 = slots 0..3) | supported; interfaces 0/1 belong to the receiver and are never opened |
| Bluetooth LE (`28de:1303`) | not in this phase (the link layer is transport-agnostic; a BLE transport can plug into `SteamHidTransport` later) |
| Steam Link build | compiled out (its SDL predates `SDL_hid_*`) |
| 2015 Steam Controller and dongle (`28de:1102`, `28de:1142`) | unchanged (SDL) |

Per controller: buttons, D-pad, analog triggers (with their end-stop clicks), both sticks with
capacitive touch, both trackpads with position, pressure and click, grip buttons (see the grip
preference), grip touch sensors, raw IMU at 250 Hz decimated to the host's requested rate,
battery state and percentage, host rumble (as the controller's 0x80 report) and the Steam haptic
reports 0x80..0x85 a Vibepollo host relays. Trigger rumble and LEDs do not exist on the device.
Native PCM haptics (0x86..0x89) are not forwarded.

The driver runs only while a stream is active. Between streams the controller is left in lizard
mode (keyboard/mouse emulation), so it keeps working on the desktop and in Moonlight's menus. At
stream end the driver stops the motors and both pad engines and switches lizard mode back on.

## Before the first stream

**A client PC running Steam owns the controller first.** Steam opens the same vendor HID
collection and would compete for its reports and settings. Close Steam on the client, or make
sure Steam Input is not managing the controller, before streaming with this driver.

**Linux (hidraw permissions).** The driver opens `/dev/hidrawN`. On most distributions that
needs a udev rule; the Flatpak has `--device=all` but still relies on the host's permissions.
Create `/etc/udev/rules.d/70-steam-controller-2026.rules`:

```
# Valve Steam Controller (2026), USB cable and wireless puck
KERNEL=="hidraw*", ATTRS{idVendor}=="28de", ATTRS{idProduct}=="1302", MODE="0660", TAG+="uaccess"
KERNEL=="hidraw*", ATTRS{idVendor}=="28de", ATTRS{idProduct}=="1304", MODE="0660", TAG+="uaccess"
```

then `sudo udevadm control --reload-rules && sudo udevadm trigger` and replug the device. If
`steam-devices` is installed it may already carry equivalent rules. A log line
`Steam Controller: cannot open /dev/hidrawN ... check the hidraw permissions (udev rule)` means
the rule is missing.

**Windows and macOS** need nothing: the vendor collection has no kernel service and is opened
through SDL's hidapi.

## Preferences (Settings, Gamepad Settings)

| Preference | Default | Setting key / CLI |
|---|---|---|
| Drive a USB or puck-connected Steam Controller (2026) directly | on | `steamcontrollerdriver`, `--steam-controller` / `--no-steam-controller` |
| Gyro and accelerometer | on | `steamcontrollermotion`, `--[no-]steam-controller-motion` |
| Trackpads as DualShock touchpad halves | on | `steamcontrollersplitpads`, `--[no-]steam-controller-split-pads` |
| Grip buttons: paddles / DualShock extras / off | paddles | `steamcontrollergrips` (0/1/2), `--steam-controller-grips paddles|ds4|off` |
| Hold host rumble for 250 ms / 500 ms / 1 s / 2 s / unlimited | 500 ms | `steamcontrollerrumblehold` (ms, 0 = unlimited), `--steam-controller-rumble-hold <ms>` |
| Stretch the sticks to the rim | off | `steamcontrollerstickstretch`, `--[no-]steam-controller-stick-stretch` |

- Split pads sends the left pad as finger 0 in the left half and the right pad as finger 1 in the
  right half of touchpad 0, which a DualShock 4 emulation splits back into "left pad"/"right pad".
  Off advertises `LI_CCAP_DUAL_TOUCHPAD` and sends each pad on its own index; only for a host that
  understands two touchpads.
- Paddles map R4, L4, R5, L5 to paddles 1..4 (Xbox Elite order). DualShock extras map L4 to
  Share, R4 to touchpad click, L5 to L3 and R5 to R3. Off drops the four grips.
- Motion is advertised and the firmware's raw IMU enabled only with the motion preference on; the
  host still decides when it wants reports (`ConnListenerSetMotionEventState`).
- Rumble hold: a host rumble is refreshed on the controller every 40 ms until the hold expires,
  then the motors are stopped. Choose unlimited only when the host reliably sends a stop.
- Stick stretch divides each axis by the largest deflection seen this session (floor 0.75) and
  clamps to the unit circle. It is off because Steam's own calibration expects the native range.

The preferences are global; per-controller settings are deferred.

**NEVER start a controller firmware update from Steam while streaming. The update would target
the virtual controller on the host, not the real controller; it cannot succeed and may leave
Steam confused. Update firmware with the controller connected directly to a PC running Steam.**

## Controller numbering

Steam slots share the sixteen player slots and the gamepad mask with SDL controllers, so the host
sees one mask. In multi-controller mode a Steam controller takes the next free slot; in
single-controller mode it is player 1 like every other pad, but its input is not merged with
other pads (as in the Android client).

## Host side

A Vibepollo host with the Steam Controller profile (`gamepad = vhf_steam`, Nonary PRs #6 and
#629; the virtual device is libvirtualgamepad's SC2026) emulates a wired Steam Controller and:

- forwards Steam's haptic output reports (0x80..0x85) to this client, which writes them to the
  real controller (`LI_CCAP_STEAM_HAPTIC`; such a client gets no plain rumble for those),
- answers Steam's firmware-attribute queries with what this client read from the real unit
  (`LI_CCAP_STEAM_ATTRIBUTES`), so Steam's "firmware update available" notice refers to the real
  controller and goes away when it is current,
- puts the grip and stick touch bits and the independent pad/trigger clicks back on the virtual
  device (`LI_CCAP_GRIP_SENSE`, `LI_CCAP_STICK_TOUCH`, `LI_CCAP_SC26_BUTTONS`).

Other Sunshine-family hosts emulate a DualShock 4 for a client with touchpads and motion (hence
the split-pads default) or an Xbox pad; they ignore the extension bits. Host settings writes
(`LI_CCAP_STEAM_SETTINGS`) are not advertised for USB and puck links: the link keeps its own
settings (lizard mode off, raw IMU, Steam watchdog disabled), as the Android client does.

## Log lines to look for

On every connection (first cable plug or puck slot waking up):

```
Steam Controller (2026) driver started: motion on, split pads on, grips 0, rumble hold 500 ms, stick stretch off
Steam Controller: opened <path> (product 0x1304, interface 2, usage page 0xff00; usage page 0xff00) as USB slot 2
Steam Controller USB slot 2: controller connected (puck, link generation 1)
Gamepad 0 (player 0) is: Steam Controller (2026) via wireless puck, USB slot 2 (VID/PID: 0x28de/0x1304) (caps 0x5e7b)
Sending controller arrival: player 0 type 4 caps 0x5e7b buttons 0x3ffff7ff (Steam Controller USB slot 2, mask 0x1)
Steam Controller USB slot 2: link settings (feature report 1, interface 2): lizard mode 0, IMU mode 0x18, Steam watchdog 0 -> accepted
Steam Controller puck (USB slot 2): firmware build 2026-07-07 23:04Z (0x6a4d85e3), bootloader 2025-09-23 19:46Z, board rev 74, capabilities 0x0, build id 26e1f7ef1955, serial FXA..., radio fw n/a, dongle fw n/a
Steam Controller puck (USB slot 2): NEVER start a controller firmware update from Steam while streaming. ...
Steam Controller USB slot 2: firmware attributes sent for player 0 (valid mask 0xcf, rc 0)
```

A cable-connected unit says `cable`, `usb (USB slot 0)` and `interface 0`. `usage page 0xff00`
is how the vendor collection was picked on Windows/macOS; on Linux hidraw the line says
`accepted a feature report 1 read` instead. Then, every 10 s while something happens:

```
Steam Controller USB slot 2: 250 reports/s, pads touched L yes R no, host haptics: rumble 3, pulse L/R/both 12/8/0, command L/R/both 1/1/0, other 0
```

Other lines: `no state report for 3000 ms; controller gone` (a puck slot went to sleep or out of
range), `output failed on interface N (1 of 3)` / `retiring the slot` (USB writes failing; the
slot is retried after a second), `link settings ... -> write failed` (the feature report was
refused; the controller still works, lizard mode may stay on), `no firmware attributes read (the
host keeps its defaults)`, `stop sequence sent (lizard mode 1, IMU mode 0 -> accepted)` at stream
end, and `ignoring host settings 0x87` if a host sends settings anyway.

Steam's own `SDL_HINT_JOYSTICK_HIDAPI_STEAM` hidapi driver (SDL 3.4.12's `SDL_hidapi_steam_triton.c`
claims `1302`, `1303`, `1304` and `1305`) and the generic joystick view of these PIDs are
disabled while the driver preference is on, so SDL never reports the controller as a gamepad of
its own during a stream.

## Tests

`tests/steamcontroller/tst_steamcontroller` runs the production decoder, attribute parser and
HID link against a fake transport with a virtual clock (142 checks): the canned reports of the
Android regression harness, the real unit's 0x83/0xF2/0xAE replies, the connect sequence,
keep-alive cadence, output queue rules (replace, coalesce, evict, strike, retire), rumble hold,
hot-plug 0x79/0x46, stale-state disconnect, device loss, the stop sequence and attribute
delivery per arrival. No hardware or SDL is needed:

```sh
mkdir -p build/tests-steamcontroller && cd build/tests-steamcontroller
qmake6 ../../tests/steamcontroller/steamcontroller.pro CONFIG+=release && make && ./tst_steamcontroller
```

It is also part of the opt-in `tests/tests.pro` tree, which Windows CI builds and runs; the
AppImage and Flatpak nightly lanes run it too. Firmware behaviour, USB scheduling and radio
timing still need a real controller and puck.
