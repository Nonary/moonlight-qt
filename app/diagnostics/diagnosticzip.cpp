#include "diagnosticzip.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QVector>
#include <array>
#include <limits>

namespace {
quint32 crc32(const QByteArray& data, quint32 crc)
{
    static const auto table = [] {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < values.size(); ++i) {
            quint32 value = i;
            for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0);
            values[i] = value;
        }
        return values;
    }();
    for (unsigned char byte : data) crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return crc;
}
}

bool writeDiagnosticZip(const QDir& source, const QString& destination, QString& error)
{
    // Never sweep settings, keys, crash dumps, other captures, or symlinks into
    // a support bundle. Reconnected worker segments use this same prefix.
    const auto files = source.entryInfoList({"Moonlight*.vrrtrace", "Moonlight*.vrrtrace.gpu-*.csv", "Moonlight.log", "capture-info.json"},
        QDir::Files | QDir::NoSymLinks, QDir::Name);
    if (files.isEmpty() || QFileInfo::exists(destination)) {
        error = QCoreApplication::translate("DiagnosticCapture", "No capture files to export, or the destination already exists.");
        return false;
    }
    QSaveFile archive(destination);
    if (!archive.open(QIODevice::WriteOnly)) {
        error = archive.errorString();
        return false;
    }
    QDataStream out(&archive);
    out.setByteOrder(QDataStream::LittleEndian);
    struct Entry { QByteArray name; quint32 size, crc, offset; };
    QVector<Entry> entries;
    constexpr auto maximum = std::numeric_limits<quint32>::max();
    bool ok = files.size() <= std::numeric_limits<quint16>::max();
    for (const auto& info : files) {
        // This simple ZIP32 writer refuses oversized input instead of silently
        // wrapping offsets. The original capture remains available in its folder.
        if (!ok || info.size() < 0 || quint64(archive.pos()) + quint64(info.size()) + 65536 >= maximum) {
            ok = false;
            break;
        }
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) { ok = false; break; }
        Entry entry{info.fileName().toUtf8(), quint32(info.size()), 0, quint32(archive.pos())};
        if (entry.name.size() > 65535) { ok = false; break; }
        // UTF-8 names, stored data, trailing data descriptor. DOS date is
        // 1980-01-01; exact UTC timestamps live in the manifest.
        out << quint32(0x04034b50) << quint16(20) << quint16(0x0808) << quint16(0)
            << quint16(0) << quint16(33) << quint32(0) << quint32(0) << quint32(0)
            << quint16(entry.name.size()) << quint16(0);
        ok = archive.write(entry.name) == entry.name.size();
        quint64 bytes = 0;
        quint32 crc = 0xffffffffU;
        while (ok && !file.atEnd()) {
            const auto chunk = file.read(1024 * 1024);
            if (chunk.isEmpty()) { ok = false; break; }
            bytes += chunk.size();
            if (bytes > entry.size) { ok = false; break; }
            crc = crc32(chunk, crc);
            ok = archive.write(chunk) == chunk.size();
        }
        if (!ok || bytes != entry.size || file.error() != QFile::NoError) { ok = false; break; }
        entry.crc = crc ^ 0xffffffffU;
        out << quint32(0x08074b50) << entry.crc << entry.size << entry.size;
        entries.append(entry);
    }
    const auto centralOffset = archive.pos();
    for (const auto& entry : entries) {
        out << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0x0808) << quint16(0)
            << quint16(0) << quint16(33) << entry.crc << entry.size << entry.size
            << quint16(entry.name.size()) << quint16(0) << quint16(0) << quint16(0)
            << quint16(0) << quint32(0) << entry.offset;
        if (archive.write(entry.name) != entry.name.size()) ok = false;
    }
    const auto centralSize = archive.pos() - centralOffset;
    if (quint64(archive.pos()) + 22 >= maximum) ok = false;
    out << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(entries.size())
        << quint16(entries.size()) << quint32(centralSize) << quint32(centralOffset) << quint16(0);
    if (!ok || out.status() != QDataStream::Ok || !archive.commit()) {
        error = QCoreApplication::translate("DiagnosticCapture",
            "Export failed: unreadable or changing input, insufficient disk space, or the ZIP exceeds 4 GiB. The original capture files were kept.");
        return false;
    }
    return true;
}
