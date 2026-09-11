#include "gamescoperepaint.h"
#include "protocols/gamescope-private-client-protocol.h"

#include <QDebug>
#include <wayland-client.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>

GamescopeRepaint::GamescopeRepaint(const QString& displayName, int connectedFd)
{
    m_WakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_WakeFd < 0) {
        if (connectedFd >= 0) close(connectedFd);
        m_Disabled = true;
        qWarning("Gamescope repaint: disabled (eventfd failed)");
        return;
    }
    m_Thread = std::thread(&GamescopeRepaint::run, this, displayName, connectedFd);
}

GamescopeRepaint::~GamescopeRepaint()
{
    m_Stop = true;
    wake();
    if (m_Thread.joinable()) m_Thread.join();
    if (m_WakeFd >= 0) close(m_WakeFd);
    qInfo("Gamescope repaint: requested=%llu acknowledged=%llu (command replies, not display events)",
          static_cast<unsigned long long>(m_Requested.load()),
          static_cast<unsigned long long>(m_Acknowledged.load()));
}

void GamescopeRepaint::wake()
{
    if (m_WakeFd < 0) return;
    const uint64_t value = 1;
    // Nonblocking: EAGAIN means a wakeup is already pending.
    while (write(m_WakeFd, &value, sizeof(value)) < 0 && errno == EINTR) {}
}

void GamescopeRepaint::request()
{
    if (m_Disabled.load(std::memory_order_relaxed)) return;
    ++m_Requested;
    if (!m_Pending.exchange(true)) wake();
}

void GamescopeRepaint::run(QString displayName, int connectedFd)
{
    auto* display = connectedFd >= 0 ? wl_display_connect_to_fd(connectedFd)
                                    : wl_display_connect(displayName.toLocal8Bit().constData());
    if (!display) {
        m_Disabled = true;
        qWarning("Gamescope repaint: disabled (connection failed)");
        return;
    }
    struct State {
        GamescopeRepaint* owner;
        gamescope_private* control = nullptr;
        bool outstanding = false;
        bool failed = false;
    } state{this};
    static const gamescope_private_listener controlListener{
        [](void* data, gamescope_private*, const char* text) {
            // This command has no normal log output. Treat any server message
            // as a diagnostic failure rather than silently claiming success.
            auto* s = static_cast<State*>(data);
            qWarning() << "Gamescope repaint: disabled, server replied:" << text;
            s->failed = true;
        },
        [](void* data, gamescope_private*) {
            auto* s = static_cast<State*>(data);
            if (!s->outstanding) return;
            s->outstanding = false;
            if (++s->owner->m_Acknowledged == 1)
                qInfo("Gamescope repaint: first per-frame request acknowledged");
        }
    };
    static const wl_registry_listener registryListener{
        [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
            auto* s = static_cast<State*>(data);
            if (!s->control && version >= 1 && !strcmp(interface, "gamescope_private")) {
                s->control = static_cast<gamescope_private*>(
                    wl_registry_bind(registry, name, &gamescope_private_interface, 1));
                gamescope_private_add_listener(s->control, &controlListener, s);
            }
        },
        [](void*, wl_registry*, uint32_t) {}
    };
    auto* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, &state);
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!m_Stop && !state.failed) {
        if (wl_display_dispatch_pending(display) < 0) break;
        if (state.failed) break;
        if (state.control && !state.outstanding && m_Pending.exchange(false)) {
            gamescope_private_execute(state.control, "debug_force_repaint", "");
            state.outstanding = true;
            deadline = Clock::now() + std::chrono::seconds(2);
        }
        if ((!state.control || state.outstanding) && Clock::now() >= deadline) {
            qWarning("Gamescope repaint: disabled (protocol discovery or acknowledgement timed out)");
            break;
        }
        // Exactly one thread owns this connection. No synchronous roundtrips.
        const int flushed = wl_display_flush(display);
        if (flushed < 0 && errno != EAGAIN) break;
        pollfd fds[2]{{wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0},
                      {m_WakeFd, POLLIN, 0}};
        const int result = poll(fds, 2, 100);
        if (result < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents & POLLIN) {
            uint64_t value;
            while (read(m_WakeFd, &value, sizeof(value)) < 0 && errno == EINTR) {}
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if ((fds[0].revents & POLLIN) && wl_display_dispatch(display) < 0) break;
    }
    if (!m_Stop) qWarning("Gamescope repaint: helper disabled for this stream");
    m_Disabled = true;
    if (state.control) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(state.control));
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
}
