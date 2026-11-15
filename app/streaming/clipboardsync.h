#pragma once

#include "backend/nvaddress.h"

#include <QSslCertificate>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

class NvComputer;

// Posted (with no payload) to wake Session::exec() when a clipboard request
// finishes on the worker thread. Must not collide with other SDL_CODE_* values.
#define SDL_CODE_CLIPBOARD_RESULT 107

// Text clipboard sync with hosts that implement Apollo's /actions/clipboard
// endpoint (GET reads the host clipboard, POST replaces it).
//
// Threading: every public method must be called on the SDL main thread (the
// thread running Session::exec()). The Qt event loop is not pumped while
// streaming, so the local clipboard is accessed through SDL, HTTP runs on a
// worker in QThreadPool::globalInstance(), and results come back via an
// SDL_USEREVENT with code SDL_CODE_CLIPBOARD_RESULT. Requests run one at a
// time, in the order they were made.
class ClipboardSync
{
public:
    ClipboardSync(NvComputer* computer, bool autoSendToHost, bool autoReceiveFromHost, int maxSizeMB);
    ~ClipboardSync();

    ClipboardSync(const ClipboardSync&) = delete;
    ClipboardSync& operator=(const ClipboardSync&) = delete;

    // Automatic triggers (no-ops unless enabled in preferences)
    void onStreamStarted();
    void onFocusGained();
    void onFocusLost();

    // Manual triggers (keyboard shortcuts). These ignore the preferences
    // and retry even if the host previously denied permission.
    void sendToHostNow();
    void receiveFromHostNow();

    // Call when an SDL_CODE_CLIPBOARD_RESULT event arrives
    void processResults();

    // Allowed range for the size limit (matches Artemis)
    static constexpr int k_MinSizeMB = 1;
    static constexpr int k_MaxSizeMB = 100;

private:
    enum class Direction { ToHost, FromHost };

    struct Request
    {
        Direction direction;
        bool manual;
        std::string text;          // ToHost: text to send
        std::string localSnapshot; // FromHost: local clipboard when requested
    };

    struct Result
    {
        Request request;
        bool success;
        bool permissionDenied;
        std::string text;          // FromHost: host clipboard
    };

    struct Shared
    {
        NvAddress address;
        uint16_t httpsPort;
        QSslCertificate serverCert;
        bool useTrueUid;

        std::mutex lock;
        std::deque<Request> pending;
        std::deque<Result> results;
        bool workerRunning = false;
        std::atomic<bool> alive { true };
        int requestTimeoutMs = 0;
    };

    void sendToHost(bool manual);
    void receiveFromHost(bool manual);
    void enqueue(Request&& request);

    static void runWorker(std::shared_ptr<Shared> shared);
    static Result execute(Shared& shared, const Request& request);

    static std::string readLocalClipboard();

    std::shared_ptr<Shared> m_Shared;
    bool m_AutoSendToHost;
    bool m_AutoReceiveFromHost;

    // Upper bound for text sent or accepted, in UTF-8 bytes
    size_t m_MaxTextBytes;

    // Set when the host answers 401 (clipboard permission not granted for this
    // client) so automatic sync stops retrying for the rest of the session.
    bool m_SendDenied = false;
    bool m_ReceiveDenied = false;

    // Last text known to be identical on both sides. Used to skip redundant
    // uploads and to avoid echoing the host's clipboard back to it.
    std::string m_LastSyncedText;
};
