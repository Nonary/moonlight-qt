#include "clipboardsync.h"

#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"

#include <QThreadPool>

#include "SDL_compat.h"

#include <QtGlobal>

namespace {

// Base timeout plus allowance for larger transfers, so a big size limit isn't
// defeated by the timeout on a slower link.
constexpr int k_BaseRequestTimeoutMs = 5000;
constexpr int k_RequestTimeoutPerMBMs = 2000;

}

ClipboardSync::ClipboardSync(NvComputer* computer, bool autoSendToHost, bool autoReceiveFromHost, int maxSizeMB)
    : m_Shared(std::make_shared<Shared>()),
      m_AutoSendToHost(autoSendToHost),
      m_AutoReceiveFromHost(autoReceiveFromHost)
{
    maxSizeMB = qBound(k_MinSizeMB, maxSizeMB, k_MaxSizeMB);
    m_MaxTextBytes = (size_t)maxSizeMB * 1024 * 1024;
    m_Shared->requestTimeoutMs = k_BaseRequestTimeoutMs + maxSizeMB * k_RequestTimeoutPerMBMs;

    // Snapshot connection details here so the worker never touches NvComputer
    m_Shared->address = computer->activeAddress;
    m_Shared->httpsPort = computer->activeHttpsPort;
    m_Shared->serverCert = computer->serverCert;
    m_Shared->useTrueUid = !computer->isNvidiaServerSoftware;
}

ClipboardSync::~ClipboardSync()
{
    // A request still in flight finishes on its own (bounded by the request
    // timeout); its result is dropped and no further wakeups are posted.
    m_Shared->alive = false;

    std::lock_guard<std::mutex> lock(m_Shared->lock);
    m_Shared->pending.clear();
    m_Shared->results.clear();
}

void ClipboardSync::onStreamStarted()
{
    if (m_AutoSendToHost) {
        sendToHost(false);
    }
}

void ClipboardSync::onFocusGained()
{
    // The user may have copied something locally while the stream was in the
    // background. sendToHost() skips the request if nothing changed.
    if (m_AutoSendToHost) {
        sendToHost(false);
    }
}

void ClipboardSync::onFocusLost()
{
    // The user is switching to a local app, likely to paste what they copied
    // on the host.
    if (m_AutoReceiveFromHost) {
        receiveFromHost(false);
    }
}

void ClipboardSync::sendToHostNow()
{
    sendToHost(true);
}

void ClipboardSync::receiveFromHostNow()
{
    receiveFromHost(true);
}

void ClipboardSync::sendToHost(bool manual)
{
    if (m_SendDenied && !manual) {
        return;
    }

    std::string text = readLocalClipboard();
    if (text.empty()) {
        if (manual) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard sync: local clipboard has no text to send");
        }
        return;
    }

    if (text.size() > m_MaxTextBytes) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: local clipboard text is too large to send (%zu bytes, limit %zu)",
                    text.size(), m_MaxTextBytes);
        return;
    }

    if (!manual && text == m_LastSyncedText) {
        return;
    }

    Request request;
    request.direction = Direction::ToHost;
    request.manual = manual;
    request.text = std::move(text);
    enqueue(std::move(request));
}

void ClipboardSync::receiveFromHost(bool manual)
{
    if (m_ReceiveDenied && !manual) {
        return;
    }

    Request request;
    request.direction = Direction::FromHost;
    request.manual = manual;
    request.localSnapshot = readLocalClipboard();
    enqueue(std::move(request));
}

void ClipboardSync::enqueue(Request&& request)
{
    bool startWorker = false;
    {
        std::lock_guard<std::mutex> lock(m_Shared->lock);

        // Collapse redundant automatic requests in the same direction; only
        // the newest one matters.
        if (!request.manual) {
            for (auto it = m_Shared->pending.begin(); it != m_Shared->pending.end();) {
                if (!it->manual && it->direction == request.direction) {
                    it = m_Shared->pending.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        m_Shared->pending.push_back(std::move(request));
        if (!m_Shared->workerRunning) {
            m_Shared->workerRunning = true;
            startWorker = true;
        }
    }

    if (startWorker) {
        // QRunnable rather than a lambda: QThreadPool::start(std::function)
        // needs Qt 5.15, older than some supported targets (Steam Link).
        class Worker : public QRunnable
        {
        public:
            explicit Worker(std::shared_ptr<Shared> shared) : m_Shared(std::move(shared)) {}
            void run() override { runWorker(m_Shared); }
        private:
            std::shared_ptr<Shared> m_Shared;
        };

        QThreadPool::globalInstance()->start(new Worker(m_Shared));
    }
}

void ClipboardSync::runWorker(std::shared_ptr<Shared> shared)
{
    for (;;) {
        Request request;
        {
            std::lock_guard<std::mutex> lock(shared->lock);
            if (shared->pending.empty() || !shared->alive) {
                shared->workerRunning = false;
                return;
            }
            request = std::move(shared->pending.front());
            shared->pending.pop_front();
        }

        Result result = execute(*shared, request);

        {
            std::lock_guard<std::mutex> lock(shared->lock);
            if (!shared->alive) {
                shared->workerRunning = false;
                return;
            }
            shared->results.push_back(std::move(result));
        }

        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_CLIPBOARD_RESULT;
        SDL_PushEvent(&event);
    }
}

ClipboardSync::Result ClipboardSync::execute(Shared& shared, const Request& request)
{
    Result result;
    result.request = request;
    result.success = false;
    result.permissionDenied = false;

    // Created on this thread so its QNetworkAccessManager lives here too
    NvHTTP http(shared.address, shared.httpsPort, shared.serverCert, shared.useTrueUid);

    try {
        if (request.direction == Direction::ToHost) {
            http.setClipboardText(QByteArray::fromStdString(request.text), shared.requestTimeoutMs);
        }
        else {
            result.text = http.getClipboardText(shared.requestTimeoutMs).toStdString();
        }
        result.success = true;
    }
    catch (const GfeHttpResponseException& e) {
        result.permissionDenied = e.getStatusCode() == 401;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: host returned HTTP %d", e.getStatusCode());
    }
    catch (const QtNetworkReplyException& e) {
        // Apollo answers 401 when this client lacks the clipboard permission
        // and 403 when it isn't streaming. Hosts without the endpoint usually
        // answer 404.
        result.permissionDenied = e.getError() == QNetworkReply::AuthenticationRequiredError;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: request failed: %s", e.getErrorText());
    }
    catch (const std::exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: request failed: %s", e.what());
    }

    return result;
}

void ClipboardSync::processResults()
{
    std::deque<Result> results;
    {
        std::lock_guard<std::mutex> lock(m_Shared->lock);
        results.swap(m_Shared->results);
    }

    for (Result& result : results) {
        const Request& request = result.request;

        if (!result.success) {
            if (result.permissionDenied) {
                bool& denied = request.direction == Direction::ToHost ? m_SendDenied : m_ReceiveDenied;
                if (!denied) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Clipboard sync: host has not granted %s permission to this client; "
                                "automatic sync in this direction is paused for this session",
                                request.direction == Direction::ToHost ? "clipboard set" : "clipboard read");
                }
                denied = true;
            }
            continue;
        }

        if (request.direction == Direction::ToHost) {
            // A manual send that succeeds re-enables automatic sending
            m_SendDenied = false;
            m_LastSyncedText = request.text;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard sync: sent %zu bytes to host", request.text.size());
            continue;
        }

        m_ReceiveDenied = false;

        if (result.text.empty()) {
            continue;
        }

        if (result.text.size() > m_MaxTextBytes) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard sync: host clipboard text is too large to accept (%zu bytes, limit %zu)",
                        result.text.size(), m_MaxTextBytes);
            continue;
        }

        std::string current = readLocalClipboard();

        // If the user copied something locally while the request was in
        // flight, keep their new copy instead of overwriting it.
        if (!request.manual && current != request.localSnapshot) {
            continue;
        }

        if (current != result.text) {
            if (SDL_SetClipboardText(result.text.c_str()) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Clipboard sync: failed to set local clipboard: %s", SDL_GetError());
                continue;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard sync: received %zu bytes from host", result.text.size());
        }

        m_LastSyncedText = std::move(result.text);
    }
}

std::string ClipboardSync::readLocalClipboard()
{
    std::string text;

    if (SDL_HasClipboardText()) {
        char* raw = SDL_GetClipboardText();
        if (raw != nullptr) {
            text = raw;
            SDL_free(raw);
        }
    }

    return text;
}
