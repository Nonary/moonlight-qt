#pragma once
#include "reserve.h"
#include <QByteArray>
#include <QString>

namespace Vrr13 {
// Identity follows the effective controller and presenter, not revision labels.
QString calibrationKey(const QString& context, const QByteArray& policy,
                       bool canLatch, bool allowTearing);
// Bounded, versioned model cache. No phase, native timestamps, or temporary
// boost survives a session. Empty profile key disables disk access (the lab/tests).
bool loadProfile(const QString& path, const QString& key, Reserve& reserve);
enum class PresentationValidation { Unavailable, Passed, Failed };
bool saveProfile(const QString& path, const QString& key, const Reserve& reserve,
                 PresentationValidation presentation = PresentationValidation::Unavailable);
// The playout delay a session with this key settled at, used to start the
// next session there instead of at a generic guess. Zero means none.
uint64_t loadStartDelay(const QString& path, const QString& key);
bool saveStartDelay(const QString& path, const QString& key, uint64_t delayUs);
}
