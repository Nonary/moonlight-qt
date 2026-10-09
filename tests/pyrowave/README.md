# PyroWave client regression tests

`pyrowave.pro` builds framing, independent-compression, negotiation, receive,
and link-policy regressions. The opt-in round-trip executable additionally
requires Vulkan hardware. Run it with `--require-gpu` when a missing GPU must
fail validation rather than skip. `--dequant-only` and `--compression-only`
select focused GPU checks and can be combined with `--require-gpu`.

`tst_pyrowaveudpaddress` exercises the actual calibration receiver with IPv4,
IPv6 and a DNS hostname, checking ephemeral ports and real loopback datagram
delivery. It also checks cancellation and invalid input. An optional hostname
argument checks resolution and receiver binding against a local discovered
host, without sending it a bandwidth probe.
Handshake cases verify that the token originates from the actual receiving
socket for IPv4 and IPv6, a lost first token is retried, measured reception
stops retries, cancellation sends no token, and invalid or unannounced source
ports are rejected. A progressive HTTP fixture verifies that Qt delivers the
port header while the body is still pending and completes that body only after
the UDP token arrives. These loopback checks do not replace a live-host test
through Windows Firewall.
The Mac delayed-reader cases hold packets for 160 ms before reading, check
that kernel timestamps retain the original spacing, and verify Qt's next
datagram notification still works for IPv4 and IPv6. The link-policy suite
checks that failed searches retain their measured floor probe and identify
lossless timing failures without classifying UDP as blocked.
Frame-shaped policy regressions cover repeated 12-packet tail loss below 2%
overall, holes in the final send group whose last packet arrived, a single
frame over 10% loss hidden by intact frames, the exact 10% boundary, absent
final frames, uneven sequence partitions, the 2% tail-frequency boundary,
and two fresh confirmations. Small interior holes may qualify under the
aggregate limits, including the measured 21/240 damaged frames at 0.142%
aggregate loss with no frame over 10% and no missing tail groups. Search cases
cover a passing intermediate speed despite failed endpoints, exhaustive
50 Mbps alternatives before fallback, fallback ranking by tail/severe damage,
failed confirmations, invalid measurements and cancellation.

`tst_PacketSpeed.qml` exercises the production packet-speed settings control:
numeric editing, controller arrow steps, Automatic reset, updates from an
applied calibration value, and a compact layout. Run it with Qt's
`qmltestrunner -input tests/pyrowave/tst_PacketSpeed.qml`. For an offscreen
Windows run, set `QT_QUICK_CONTROLS_STYLE=Material`, `QT_QUICK_BACKEND=software`
and `QT_QPA_FONTDIR=C:\Windows\Fonts`, then add `-platform offscreen`.

## macOS native GPU checks

The Mac round trip uses the bundled MoltenVK driver and the client's actual
Vulkan decoder. It checks 4:2:0 and 4:4:4 at 8 and 10 bits, both framing
contracts, lost-detail recovery, and sixteen consecutive compressed partial
frames followed by intact recovery. The coefficient-store test compares the
two shader store implementations byte for byte.

The shared-output checks create the production `PyroWaveMetalPool` on a native
Metal device, including 4K and clipped-tile 3024x1964 output. All three output textures are read through native Metal blits
and compared byte for byte with synchronous Vulkan readback. They retain eight
frames to exercise pool exhaustion, poison and recycle each shared surface,
and check its per-frame decode-completion timeline. The synchronous fallback
also checks reconstruction quality and negotiated pixel formats. A cloned frame
retains readable native textures after the decoder and pool are destroyed.
The production shared-output requirement is enabled for native decode and must
reject initialization when no sharing pool is available.

From the repository root, after the Mac codec library has been built:

```sh
mkdir -p build/mac-gpu-roundtrip
cd build/mac-gpu-roundtrip
../vrr-hybrid/qt/6.11.1/macos/bin/qmake ../../tests/pyrowave/roundtrip.pro \
    CONFIG+=release \
    PYROWAVE_STATIC_LIBRARY="$PWD/../mac-client/pyrowave/libpyrowave.a"
make -j8
GRANITE_VULKAN_LIBRARY="$PWD/../../libs/mac/lib/libMoltenVK.dylib" \
DYLD_LIBRARY_PATH="$PWD/../../libs/mac/lib" \
    ./tst_pyrowaveroundtrip --require-gpu
```

Replace the qmake path with another installed Qt when needed. Omit
`PYROWAVE_STATIC_LIBRARY` to compile the vendored codec into the test itself.
The dynamic-library path supplies the bundled FFmpeg dependency whose install
name normally resolves inside the application bundle.

These tests establish reconstruction and shared GPU ownership/synchronization.
They do not establish live stream throughput, compositor cadence, physical
scanout timing, or variable refresh behavior on an external display.
