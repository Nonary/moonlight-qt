#pragma once

#include <QByteArray>
#include <QList>

// Mesa's hardware low-latency request is independent of FFmpeg LOW_DELAY.
// Preserve user debug flags and make repeated initialization idempotent.
inline QByteArray withAmdLowLatencyDecode(QByteArray flags)
{
    for (const QByteArray& flag : flags.split(',')) {
        if (flag.trimmed() == "lowlatencydec") return flags;
    }
    if (!flags.isEmpty() && !flags.endsWith(',')) flags.append(',');
    flags.append("lowlatencydec");
    return flags;
}
