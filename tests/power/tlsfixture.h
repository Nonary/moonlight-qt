#pragma once

#include <QFile>
#include <QProcess>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <functional>

// Loopback-only test credentials are generated per run. No real host or power
// implementation is involved; every acceptance is synthetic XML.
struct TlsCredentials {
    QSslCertificate certificate;
    QSslKey key;
    static TlsCredentials generate(const QString& directory, const QString& name) {
        const auto certPath = directory + "/" + name + ".crt";
        const auto keyPath = directory + "/" + name + ".key";
        QProcess openssl;
        openssl.start("openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", keyPath, "-out", certPath, "-days", "2", "-subj", "/CN=127.0.0.1",
            "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"});
        if (!openssl.waitForFinished(30000) || openssl.exitCode() != 0)
            qFatal("Could not generate isolated TLS test credentials");
        QFile certFile(certPath), keyFile(keyPath);
        if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly))
            qFatal("Could not read isolated TLS test credentials");
        return {QSslCertificate(certFile.readAll()), QSslKey(keyFile.readAll(), QSsl::Rsa)};
    }
};

class TlsServer : public QTcpServer {
public:
    struct Result {
        QByteArray body;
        int status = 200;
        bool close = true;
        bool loseResponse = false;
        std::function<void()> after;
    };
    struct Request { QByteArray line, headers, body; int connection; };
    TlsCredentials credentials;
    QVector<Result> results;
    QVector<Request> requests;
    int connections = 0;
    explicit TlsServer(TlsCredentials value) : credentials(value) {
        if (!listen(QHostAddress::LocalHost, 0)) qFatal("Loopback TLS test listen failed");
    }
    int posts() const {
        int count = 0;
        for (const auto& request : requests) if (request.line.startsWith("POST ")) ++count;
        return count;
    }
protected:
    void incomingConnection(qintptr descriptor) override {
        auto* socket = new QSslSocket(this);
        const int connection = ++connections;
        socket->setSocketDescriptor(descriptor);
        socket->setLocalCertificate(credentials.certificate);
        socket->setPrivateKey(credentials.key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::readyRead, this, [this, socket, connection] {
            QByteArray data = socket->property("request").toByteArray() + socket->readAll();
            const int headerEnd = data.indexOf("\r\n\r\n");
            if (headerEnd < 0) { socket->setProperty("request", data); return; }
            const auto headers = data.left(headerEnd);
            int bodyLength = 0;
            for (const auto& header : headers.split('\n')) {
                if (header.toLower().startsWith("content-length:")) bodyLength = header.mid(15).trimmed().toInt();
            }
            if (data.size() < headerEnd + 4 + bodyLength) { socket->setProperty("request", data); return; }
            socket->setProperty("request", data.mid(headerEnd + 4 + bodyLength));
            requests.append({headers.split('\n').value(0).trimmed(), headers,
                             data.mid(headerEnd + 4, bodyLength), connection});
            const auto result = results.isEmpty() ? Result{{}, 500} : results.takeFirst();
            if (result.loseResponse) { socket->abort(); return; }
            QByteArray response = "HTTP/1.1 " + QByteArray::number(result.status) + " Test\r\nContent-Type: application/xml\r\n";
            if (result.status == 307) response += "Location: /redirect-must-never-follow\r\n";
            response += "Content-Length: " + QByteArray::number(result.body.size()) +
                (result.close ? "\r\nConnection: close\r\n\r\n" : "\r\nConnection: keep-alive\r\n\r\n");
            socket->write(response + result.body);
            if (result.after) result.after();
            if (result.close) socket->disconnectFromHost();
        });
        socket->startServerEncryption();
    }
};
