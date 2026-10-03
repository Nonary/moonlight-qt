#pragma once

#include <QString>
#include <atomic>
#include <cstdint>
#include <thread>

// Owns a separate Wayland connection. The renderer only queues a coalesced
// notification; all compositor I/O and replies belong to the helper thread.
class GamescopeRepaint {
public:
    // connectedFd transfers ownership and is used by the isolated protocol test.
    explicit GamescopeRepaint(const QString& displayName, int connectedFd = -1);
    ~GamescopeRepaint();
    GamescopeRepaint(const GamescopeRepaint&) = delete;
    GamescopeRepaint& operator=(const GamescopeRepaint&) = delete;
    void request();
    uint64_t acknowledged() const { return m_Acknowledged.load(); }
    bool disabled() const { return m_Disabled.load(); }

private:
    void wake();
    void run(QString displayName, int connectedFd);
    int m_WakeFd = -1;
    std::atomic<bool> m_Stop{false}, m_Pending{false}, m_Disabled{false};
    std::atomic<uint64_t> m_Requested{0}, m_Acknowledged{0};
    std::thread m_Thread;
};
