#pragma once

#include <QString>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

class AsyncLogWriter
{
public:
    using WriteMessage = void (*)(const QString&, bool normalOutput);

    explicit AsyncLogWriter(WriteMessage writeMessage) : m_WriteMessage(writeMessage) {}

    ~AsyncLogWriter()
    {
        if (m_Thread.joinable()) {
            {
                std::lock_guard lock(m_Mutex);
                m_Accepting = false;
            }
            m_Changed.notify_all();
            m_Thread.join();
        }
    }

    // Called once during application startup, before installing log handlers.
    bool start()
    {
        std::lock_guard lock(m_Mutex);
        try {
            m_Thread = std::thread(&AsyncLogWriter::run, this);
        } catch (const std::system_error&) {
            return false;
        }
        m_Accepting = true;
        return true;
    }

    bool enqueue(const QString& message, bool normalOutput)
    {
        {
            std::lock_guard lock(m_Mutex);
            if (!m_Accepting) {
                return false;
            }
            m_Pending.push_back({message, normalOutput});
        }
        m_Changed.notify_all();
        return true;
    }

    void drain()
    {
        std::unique_lock lock(m_Mutex);
        m_Changed.wait(lock, [this] { return m_Pending.empty() && !m_Writing; });
    }

private:
    struct Message
    {
        QString text;
        bool normalOutput;
    };

    void run()
    {
        std::unique_lock lock(m_Mutex);
        for (;;) {
            m_Changed.wait(lock, [this] { return !m_Pending.empty() || !m_Accepting; });
            if (m_Pending.empty()) {
                return;
            }

            Message message = std::move(m_Pending.front());
            m_Pending.pop_front();
            m_Writing = true;
            lock.unlock();
            m_WriteMessage(message.text, message.normalOutput);
            lock.lock();
            m_Writing = false;
            if (m_Pending.empty()) {
                m_Changed.notify_all();
            }
        }
    }

    const WriteMessage m_WriteMessage;
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    std::deque<Message> m_Pending;
    std::thread m_Thread;
    bool m_Accepting = false;
    bool m_Writing = false;
};
