#pragma once
#include "reserve.h"
#include <QString>

namespace Vrr {
// Bounded, versioned model cache. No phase, native timestamps, or temporary
// boost survives a session. Empty profile key disables disk access (the lab/tests).
bool loadProfile(const QString& path, const QString& key, Reserve& reserve);
enum class PresentationValidation { Unavailable, Passed, Failed };
bool saveProfile(const QString& path, const QString& key, const Reserve& reserve,
                 PresentationValidation presentation = PresentationValidation::Unavailable);
}
