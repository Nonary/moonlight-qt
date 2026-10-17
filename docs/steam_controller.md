# Steam Controller (2026) over USB, the wireless puck and Bluetooth LE

Moonlight drives a 2026 Steam Controller ("Ibex") itself when it is connected to the client PC
by USB cable (`28de:1302`), through its wireless puck (`28de:1304`, up to four controllers) or
paired over Bluetooth LE (`28de:1303`). The host sees a Steam Controller (`LI_CTYPE_STEAM`) with
both trackpads, gyro and accelerometer, grip and stick touch sensors, battery state, the physical
pad and trigger clicks, Steam's own haptic reports relayed to the real pads, and the real unit's
firmware attributes. The reference implementation is the Android client's USB/puck path
(`SteamControllerUsb.java` and the shared `SteamControllerInput.java`) and, for the Bluetooth LE
additions (host settings relay, haptic knobs), its BLE driver (`SteamControllerBle.java`); this
port follows them byte for byte on the wire.

Code: `app/streaming/input/steamcontroller/` (decoder, attribute parser, HID link, SDL hidapi
transport, host bridge). Tests: `tests/steamcontroller/`.

## What works and what does not

| Transport | Status |
|---|---|
| USB cable (`28de:1302`, HID interface 0) | supported |
| Wireless puck (`28de:1304`, HID interfaces 2..5 = slots 0..3) | supported; interfaces 0/1 belong to the receiver and are never opened |
| Bluetooth LE (`28de:1303`, HID-over-GATT, one HID device per paired controller) | supported; same protocol as USB through hidapi, plus the host settings relay and three haptic knobs (see [Bluetooth LE](#bluetooth-le)) |
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
# Valve Steam Controller (2026) over Bluetooth LE (HID-over-GATT, uhid)
KERNEL=="hidraw*", KERNELS=="*28DE:1303*", MODE="0660", TAG+="uaccess"
```

The Bluetooth rule cannot match `idVendor`/`idProduct`: a HID-over-GATT device is created by
`uhid` and has no USB attributes, so it matches the HID id string of the parent device
(`0005:28DE:1303.000N`) instead. Then `sudo udevadm control --reload-rules && sudo udevadm trigger`
and replug (or re-pair) the device. If `steam-devices` / `game-devices-udev` or Valve's
`60-steam-input.rules` is installed it may already cover `28DE` devices, including the Bluetooth
node. A log line `Steam Controller: cannot open /dev/hidrawN ... check the hidraw permissions
(udev rule)` means the rule is missing.

**Windows and macOS** need nothing: the vendor collection has no kernel service and is opened
through SDL's hidapi, over USB and over Bluetooth LE alike.

## Preferences (Settings, Gamepad Settings)

| Preference | Default | Setting key / CLI |
|---|---|---|
| Drive a USB or puck-connected Steam Controller (2026) directly | on | `steamcontrollerdriver`, `--steam-controller` / `--no-steam-controller` |
| Gyro and accelerometer | on | `steamcontrollermotion`, `--[no-]steam-controller-motion` |
| Trackpads as DualShock touchpad halves | on | `steamcontrollersplitpads`, `--[no-]steam-controller-split-pads` |
| Grip buttons: paddles / DualShock extras / off | paddles | `steamcontrollergrips` (0/1/2), `--steam-controller-grips paddles|ds4|off` |
| Hold host rumble for 250 ms / 500 ms / 1 s / 2 s / unlimited | 500 ms | `steamcontrollerrumblehold` (ms, 0 = unlimited), `--steam-controller-rumble-hold <ms>` |
| Stretch the sticks to the rim | off | `steamcontrollerstickstretch`, `--[no-]steam-controller-stick-stretch` |
| Over Bluetooth LE: pad click boost (off / +6 / +12 / +20 dB) | off | `steamcontrollerhapticboost` (dB), `--steam-controller-haptic-boost <dB>` |
| Over Bluetooth LE: pad slide pulse length (100 / 150 / 200 / 300 %) | 100 % | `steamcontrollerhapticpulsescale` (percent), `--steam-controller-haptic-pulse-scale <percent>` |
| Over Bluetooth LE: haptic master gain (off / +6 / +12 dB) | off | `steamcontrollerhapticmastergain` (dB, 0 = not written), `--steam-controller-haptic-master-gain <dB>` |

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
- The three Bluetooth LE knobs apply only to Bluetooth LE links and are off by default, so the
  default BLE path is byte-identical to USB; USB and puck links ignore them. They are the Android
  BLE driver's (`steam_controller_haptic_boost` / `_pulse_scale` / `_master_gain`): the
  firmware's haptic baseline over BLE is low, and these lift it without the host's involvement.
  Boost is added to the signed gain byte of Steam's 0x82 tick and click commands (clamped to the
  firmware's +6 dB ceiling; +6 keeps Steam's click/tick balance, +20 puts both at the ceiling;
  stops untouched). Pulse length scales the on-time of 0x81 pulses (Steam's slide texture is
  400 µs pulses with no gain field; capped at 0xFFFF; zero on-time and zero-repeat stops
  untouched). Master gain writes `SETTING_HAPTIC_MASTER_GAIN_DB` (76) as a fourth triple in the
  link's own settings (setup, keep-alive and after a host reset) and clears it at stream end;
  Steam never writes it, so whether the firmware honours it over BLE is for a tester to say. The
  values are clamped to 0..30 dB, 100..400 % and 0..24 dB.

The preferences are global; per-controller settings are deferred.

## Bluetooth LE

Over Bluetooth LE the controller is a HID-over-GATT device and this driver talks plain HID to
it through the same SDL hidapi transport as USB (no QtBluetooth, no new dependency); the OS
Bluetooth stack does the GATT work. What is the same as USB: feature report 1 (`01 <type> <len>
...`, 64 bytes) carries every command, including the link settings (lizard mode 9 = 0, IMU mode
48 = 0x18 or 0, Steam watchdog 71 = 0 at connect and every second) and the attribute queries
0x83/0xF2/0xAE; the haptic output reports 0x80..0x85 are HID output reports with the report id
first, exactly as hidapi takes them for USB (each id is its own GATT characteristic underneath);
the stop sequence restores lizard mode. What differs: input arrives as report 0x45 (45 bytes
after the id, same field offsets as 0x42/0x47) at about 133 reports/s (7.5 ms connection
interval) instead of 250/s; battery is report 0x43 when the firmware sends it; the firmware
refuses the Deck-era feature commands 0x8F and 0xEA over BLE, which this driver never uses; and
the controller boots with the firmware's default settings instead of the ones Steam writes over
USB, so a BLE link advertises `LI_CCAP_STEAM_SETTINGS` and relays the host's settings writes
(below). The attributes packet says transport `LI_SCATTR_TRANSPORT_BLE` (1).

**Pairing.** Pair the controller in the OS Bluetooth settings like a keyboard: with no cable and
no puck in reach, hold the Steam button until the controller advertises (it shows up as "Steam
Controller"); on Windows use Settings > Bluetooth & devices > Add device, on macOS System
Settings > Bluetooth, on Linux `bluetoothctl` (`scan on`, `pair`, `trust`, `connect`) or the
desktop's Bluetooth panel. Once paired, powering the controller on reconnects it and the HID
device appears; Moonlight's 2 s rescan picks it up during a stream, and a controller that goes to
sleep or out of range vanishes as a HID device and is dropped (or after 3 s without a state
report). Steam must not be running on the client: it opens the same HID device and sends its
own settings and haptics.

**Windows and macOS** list one HID path per top-level collection of the Bluetooth device, with
no USB interface number (`interface -1`); the vendor collection is the one on usage page
`0xff00`, as over USB, and the other collections (keyboard, mouse) are refused by the feature
report 1 probe. **Linux** exposes one `hidraw` node per Bluetooth controller through `uhid`
(interface 0 or -1), which needs the udev rule above.

**Host settings relay.** A Vibepollo host forwards the settings writes Steam makes to the
virtual controller (`ConnListenerSteamSettings`, command 0x87 with (id, u16) triples, or 0x8E
"load defaults"). A BLE link applies them to the real controller the way the Android BLE driver
does: 0x8E is sent as `01 8E 00` and immediately followed by the link's own settings, so lizard
mode and the watchdog do not come back; 0x87 is sent minus the reserved ids {9, 48, 71, 49, 52,
53} (9/48/71 belong to the link's keep-alive, 49 would change the state report format, 52/53
would make the firmware never report pad clicks), as `01 87 <3*kept> <id u8, value u16 LE>*`,
if anything is left. Settings travel through the same bounded output queue as the haptics, as a
class with 16 reserved slots that haptics never evict (only a settings flood evicts the oldest
settings write). USB and puck links keep ignoring host settings.

**Battery.** The battery state comes from report 0x43 when the firmware sends it over the HID
link; hidapi cannot read the GATT battery service, so if no 0x43 arrives the battery stays
unknown on the host.

**Multiple controllers.** Every BLE controller enumerates with the same interface number, so
links are named `Bluetooth LE slot 0`, `1`, ... by the lowest free slot; they share the 16
player slots with USB, puck and SDL controllers as before.

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
  device (`LI_CCAP_GRIP_SENSE`, `LI_CCAP_STICK_TOUCH`, `LI_CCAP_SC26_BUTTONS`),
- forwards Steam's settings writes (0x87/0x8E) to a client that advertises
  `LI_CCAP_STEAM_SETTINGS`: only Bluetooth LE links do, and apply them minus the reserved ids
  (see [Bluetooth LE](#bluetooth-le)).

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
`accepted a feature report 1 read` instead. A Bluetooth LE controller:

```
Steam Controller: opened <path> (product 0x1303, interface -1, usage page 0xff00; usage page 0xff00) as Bluetooth LE slot 0
Steam Controller Bluetooth LE slot 0: controller connected (Bluetooth LE, link generation 1)
Steam Controller Bluetooth LE slot 0: haptics: 0x82 gain boost +0 dB (ceiling +6 dB), 0x81 pulse on-time x1.0, master gain setting not written
Gamepad 0 (player 0) is: Steam Controller (2026) via Bluetooth LE, Bluetooth LE slot 0 (VID/PID: 0x28de/0x1303) (caps 0x7e7b)
Steam Controller Bluetooth LE slot 0: link settings (feature report 1, interface 0): lizard mode 0, IMU mode 0x18, Steam watchdog 0 -> accepted
Steam Controller ble (Bluetooth LE slot 0): firmware build 2026-07-07 23:04Z (0x6a4d85e3), ...
Steam Controller Bluetooth LE slot 0: host settings 0x87: 41=4660 70=5 (reserved, kept ours: 9 48 71)
Steam Controller Bluetooth LE slot 0: host settings 0x87: nothing to apply (reserved, kept ours: 49 52 53)
Steam Controller Bluetooth LE slot 0: host settings 0x8e (load defaults), re-sending this link's setup
```

(`caps 0x7e7b` is the USB set `0x5e7b` plus `LI_CCAP_STEAM_SETTINGS` 0x2000; with a build
against SDL3's hidapi the opened line also says `, bus bluetooth`.) Then, every 10 s while
something happens:

```
Steam Controller USB slot 2: 250 reports/s, pads touched L yes R no, host haptics: rumble 3, pulse L/R/both 12/8/0, command L/R/both 1/1/0, other 0
Steam Controller Bluetooth LE slot 0: 133 reports/s, pads touched L yes R yes, host haptics: rumble 0, pulse L/R/both 0/23/0, command L/R/both 48/0/0, other 0, host settings 3, adjusted 48
```

(`host settings` counts the relayed writes, `adjusted` the haptic reports rewritten by the
Bluetooth LE knobs.) Other lines: `no state report for 3000 ms; controller gone` (a puck slot or
a Bluetooth controller went to sleep or out of range), `output failed on interface N (1 of 3)` /
`output failed on Bluetooth LE slot N (1 of 3)` / `retiring the slot` (writes failing; the slot
is retried after a second), `link settings ... -> write failed` (the feature report was refused;
the controller still works, lizard mode may stay on), `no firmware attributes read (the host
keeps its defaults)`, `stop sequence sent (lizard mode 1, IMU mode 0 -> accepted)` at stream end,
and `ignoring host settings 0x87 ... only a Bluetooth LE link takes them` if a host sends
settings to a USB or puck link.

Steam's own `SDL_HINT_JOYSTICK_HIDAPI_STEAM` hidapi driver (SDL 3.4.12's `SDL_hidapi_steam_triton.c`
claims `1302`, `1303`, `1304` and `1305`) and the generic joystick view of `1302`, `1303` and
`1304` are disabled while the driver preference is on, so SDL never reports the controller as a
gamepad of its own during a stream.

## Tests

`tests/steamcontroller/tst_steamcontroller` runs the production decoder, attribute parser and
HID link against a fake transport with a virtual clock (279 checks): the canned reports of the
Android regression harness, the real unit's 0x83/0xF2/0xAE replies, the connect sequence,
keep-alive cadence, output queue rules (replace, coalesce, evict, strike, retire), rumble hold,
hot-plug 0x79/0x46, stale-state disconnect, device loss, the stop sequence, attribute delivery
per arrival, feedback generations, and the Bluetooth LE transport: enumeration of 0x1303 with
interface -1/0, 0x45 input, STEAM_SETTINGS in the capabilities and transport BLE in the
attributes, byte identity of the connect/keep-alive/attribute/stop frames with USB, the settings
relay (reserved ids, exact frames, 0x8E, the 16-slot settings class under a haptic burst, the
registry path) and the three haptic knobs' arithmetic and clamps. No hardware or SDL is needed:

```sh
mkdir -p build/tests-steamcontroller && cd build/tests-steamcontroller
qmake6 ../../tests/steamcontroller/steamcontroller.pro CONFIG+=release && make && ./tst_steamcontroller
```

It is also part of the opt-in `tests/tests.pro` tree, which Windows CI builds and runs; the
AppImage and Flatpak nightly lanes run it too. Firmware behaviour, USB scheduling and radio
timing still need a real controller and puck.
