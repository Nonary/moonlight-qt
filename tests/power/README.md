# Host sleep regression tests

These tests use loopback-only TLS servers with newly generated test certificates.
They never contact a configured host, invoke a native power API, or start a stream.
The `openssl` executable must be on PATH to generate the temporary certificates.

Build with Qt development tools and OpenSSL development libraries:

```sh
mkdir -p build/power build/power-view
cd build/power
qmake ../../tests/power/poweractions.pro
make
./tst_poweractions
cd ../power-view
qmake ../../tests/power/poweractionview.pro
make
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ./tst_poweractionview
```

The regular opt-in `tests/tests.pro CONFIG+=tests` tree includes both targets.
The view test needs the Qt Quick, Quick Controls 2, and Layouts QML modules.
It loads the actual AppView with a fake backend boundary and verifies explicit
confirmation, cancellation, duplicate-click suppression, error reporting, and no
stream/session creation or app-preference writes.

The transport tests compile production NvHTTP. They exercise exact method/path,
empty POST body, unsupported/busy/revoked hosts, malformed XML, missing pairing,
certificate mismatch even with trusted CAs, changed endpoint/pin mid-transaction,
fresh TLS connections instead of borrowing pooled connections, and redirects.
Lost-response tests verify that Qt's internal reconnect does not replay discovery
or sleep POST: a second TLS connection is aborted before it can send HTTP.
Ordinary serverinfo behavior remains independently covered.

These tests do not prove a complete application build, native Windows/Qt 6
compatibility, physical input interaction, or real sleep/wake/reconnect behavior.
A host acceptance response means acceptance, not successful OS sleep. The host's
separate default-off per-client permission and one-use token remain required.
