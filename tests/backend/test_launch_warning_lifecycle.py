"""Run Session's actual warning setup/expiry/cleanup with the real overlay manager.

Only the clock is substituted. No window, stream, device or native UI is opened.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'app/streaming/session.cpp').read_text()
start = source.index('    // The async thread only supplies this after RTSP connection success.')
setup_end = source.index('\n    for (;;) {', start)
setup = source[start:setup_end]
expire_start = source.index('        if (hostWarningVisible &&', setup_end)
expire_end = source.index('\n#if SDL_VERSION_ATLEAST', expire_start)
expire = source[expire_start:expire_end]
cleanup_start = source.index('\n    m_OverlayManager.setStatusMessage(Overlay::StatusSource::HostLaunch, "");', source.index('DispatchDeferredCleanup:'))
cleanup_end = source.index('\n    // Switch back', cleanup_start)
cleanup = source[cleanup_start:cleanup_end]
# Keep setup after async-success validation and publication after real RTSP success.
assert source.index('if (!m_AsyncConnectionSuccess)', source.index('void Session::exec()')) < start
assert source.index('m_HostLaunchWarning = LaunchWarning::text(launchWarning);') > source.index('    if (err != 0) {', source.index('int err = LiStartConnection'))
fragments = (setup + '\n auto expire = [&] {\n' + expire + '\n };\n' + r'''
 assert(m_HostLaunchWarning.isEmpty());
 const auto visible = m_OverlayManager.getOverlayText(Overlay::OverlayStatusUpdate);
 assert(hostWarningVisible == expected);
 assert(visible == (expected ? "network\n\nhost warning" : "network"));
 for (int i = 0; i < 3; ++i) expire();
 assert(m_OverlayManager.getOverlayText(Overlay::OverlayStatusUpdate) == visible);
 TestClock::time += std::chrono::seconds(9);
 expire();
 assert(m_OverlayManager.getOverlayText(Overlay::OverlayStatusUpdate) == visible);
 TestClock::time += std::chrono::seconds(2);
 expire();
 assert(!hostWarningVisible);
 assert(m_OverlayManager.getOverlayText(Overlay::OverlayStatusUpdate) == "network");
''' + cleanup + '\n assert(m_HostLaunchWarning.isEmpty());\n')
fragments = fragments.replace('std::chrono::steady_clock::now()', 'TestClock::now()')
program = r'''
#include "streaming/video/overlaymanager.h"
#include <QCoreApplication>
#include <QDir>
#include <cassert>
#include <chrono>
struct TestClock {
 static inline std::chrono::steady_clock::time_point time {};
 static auto now() { return time; }
};
void check(bool expected) {
 Overlay::OverlayManager m_OverlayManager;
 m_OverlayManager.setStatusMessage(Overlay::StatusSource::Network, "network");
 QString m_HostLaunchWarning = expected ? QStringLiteral("host warning") : QString {};
''' + fragments + r'''
}
int main(int argc, char **argv) {
 QCoreApplication app(argc, argv);
 assert(argc == 2 && QDir::setCurrent(argv[1]));
 assert(SDL_Init(SDL_INIT_TIMER) == 0);
 check(true); check(false); check(true); // No stale state across successive sessions.
 SDL_Quit();
}
'''
with tempfile.TemporaryDirectory(prefix='launch-warning-lifecycle-') as directory:
    cpp = Path(directory) / 'test.cpp'
    cpp.write_text(program)
    binary = Path(directory) / 'test'
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt6Core', 'sdl2', 'SDL2_ttf'], text=True))
    subprocess.run(['g++', '-std=c++17', '-fPIC', '-pthread', '-DSDL_MAIN_HANDLED', '-I', str(root / 'app'), str(cpp), str(root / 'app/streaming/video/overlaymanager.cpp'), str(root / 'app/path.cpp'), *flags, '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(root / 'app')], check=True)
print('Production warning lifecycle and overlay isolation passed')
