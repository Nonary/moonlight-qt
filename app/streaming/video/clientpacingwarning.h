#pragma once

#include <cstdint>
#include <string>

// Decoder-thread observer. Counts describe the latest reporting interval,
// never a lifetime total or a retained quality score.
class ClientPacingWarning {
public:
    enum class Reason { None, BufferLimit, ProcessingOverload };

    Reason observe(uint64_t nowUs, bool active, bool qualified, bool atLimit,
                   bool overloaded, bool freshLateOrDrop, double qualityPercent)
    {
        if (!active || (m_Last && (nowUs <= m_Last || nowUs - m_Last > 2500000))) {
            *this = {};
        }
        m_Last = nowUs;
        if (!active) return Reason::None;
        if (!m_Started) m_Started = nowUs;
        // Both conditions are required for either warning: a maxed-out buffer
        // and measured smoothness at or below 99%. Unknown/NaN scores cannot
        // authorize a warning. Hide immediately when either condition clears.
        if (!qualified || !atLimit || !(qualityPercent <= 99.0)) {
            m_Candidate = m_Visible = Reason::None;
            m_BadSince = m_CleanSince = 0;
            return Reason::None;
        }
        const Reason candidate = qualified && freshLateOrDrop ?
            (overloaded ? Reason::ProcessingOverload :
             atLimit ? Reason::BufferLimit : Reason::None) : Reason::None;
        if (candidate != Reason::None) {
            m_CleanSince = 0;
            if (candidate != m_Candidate) {
                m_Candidate = candidate;
                m_BadSince = nowUs;
            }
            if (nowUs - m_Started >= 3000000 && nowUs - m_BadSince >= 2000000 &&
                    (m_Visible != Reason::None || !m_LastShown || nowUs - m_LastShown >= 30000000)) {
                if (m_Visible == Reason::None) m_LastShown = nowUs;
                m_Visible = candidate;
            }
        }
        else {
            m_Candidate = Reason::None;
            m_BadSince = 0;
            if (!m_CleanSince) m_CleanSince = nowUs;
            if (nowUs - m_CleanSince >= 5000000) m_Visible = Reason::None;
        }
        return m_Visible;
    }

    static std::string message(Reason reason, bool av1, bool hevcSupported, bool smooth)
    {
        if (reason == Reason::None) return {};
        std::string text = reason == Reason::BufferLimit ?
            "VRR buffer limit reached\nClient frames are missing presentation deadlines." :
            "Client processing cannot keep up\nMore buffering will not resolve this workload.";
        text += "\nTry lowering bitrate.";
        if (av1 && hevcSupported) text += " Try HEVC instead of AV1.";
        if (!smooth && reason == Reason::BufferLimit)
            text += "\nTry Smooth for more buffering (adds latency).";
        return text;
    }

private:
    uint64_t m_Started = 0, m_Last = 0, m_BadSince = 0, m_CleanSince = 0, m_LastShown = 0;
    Reason m_Candidate = Reason::None, m_Visible = Reason::None;
};
