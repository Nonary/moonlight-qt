#pragma once

#include <algorithm>
#include <cstdint>

namespace Vrr13 {
// One movement allowance for intentional extra delay. Recorded policy 9
// also charges input-buffer movement; policies 10 and 11 leave input
// adaptation to its own laws without accumulating extra-delay movement debt.
// Physical readiness/ownership floors are applied by the caller after
// this policy: a frame cannot be displayed before it exists or owns scanout.
class ExtraBufferSlew {
public:
    void reset() { *this = ExtraBufferSlew{}; }
    void begin(uint64_t frame, uint64_t nowUs, uint64_t inputUs, uint64_t toleranceUs,
               int64_t initialPadUs, bool layerChanged, bool layerDrain,
               bool shareInputMovement = true) {
        if (!m_Initialized) {
            m_Initialized = true;
            m_SmoothingUs = initialPadUs;
            m_LastUs = nowUs;
            m_InputUs = inputUs;
        }
        const uint64_t elapsed = nowUs > m_LastUs ? std::min<uint64_t>(nowUs - m_LastUs, 100000) : 0;
        m_LastUs = std::max(m_LastUs, nowUs);
        const uint64_t fraction = m_Fraction + elapsed * std::min<uint64_t>(toleranceUs, 1000000);
        m_OrdinaryRemainingUs = fraction / 1000000;
        m_Fraction = fraction % 1000000;
        if (!m_HaveFrame || frame != m_Frame) m_DrainRemainingUs = layerDrain ? toleranceUs : 0;
        m_HaveFrame = true; m_Frame = frame;
        m_LayerDrain = layerDrain;
        m_ToleranceUs = toleranceUs;
        if (!layerDrain) m_FastSmoothing = m_FastRecovery = false;
        const uint64_t change = std::max(inputUs, m_InputUs) - std::min(inputUs, m_InputUs);
        if (inputUs < m_InputUs && layerDrain) {
            m_DrainRemainingUs -= std::min(m_DrainRemainingUs, change);
            m_OrdinaryRemainingUs -= std::min(m_OrdinaryRemainingUs, change);
        }
        else if (shareInputMovement && !(layerChanged && inputUs > m_InputUs))
            m_OrdinaryDebtUs += change;
        const uint64_t paid = std::min(m_OrdinaryRemainingUs, m_OrdinaryDebtUs);
        m_OrdinaryRemainingUs -= paid;
        m_OrdinaryDebtUs -= paid;
        m_InputUs = inputUs;
    }
    int64_t smoothing(int64_t desiredUs) { return move(m_SmoothingUs, desiredUs, m_FastSmoothing); }
    uint64_t recovery(uint64_t desiredUs) {
        return uint64_t(move(m_RecoveryUs, int64_t(desiredUs), m_FastRecovery));
    }
    // A ready-successor replacement removes held recovery phase at once.
    void shedRecovery() { m_RecoveryUs = 0; m_FastRecovery = false; }
    bool draining(int64_t smoothingUs, uint64_t recoveryUs) const {
        return m_SmoothingUs > smoothingUs || m_RecoveryUs > int64_t(recoveryUs);
    }
private:
    int64_t move(int64_t& currentUs, int64_t desiredUs, bool& fast) {
        const bool down = desiredUs < currentUs;
        const uint64_t distance = uint64_t(std::max(currentUs, desiredUs)) -
            uint64_t(std::min(currentUs, desiredUs));
        if (down && m_LayerDrain && distance > m_ToleranceUs) fast = true;
        auto& remaining = down && m_LayerDrain && fast ? m_DrainRemainingUs : m_OrdinaryRemainingUs;
        const uint64_t step = std::min(distance, remaining);
        remaining -= step;
        // A downward layer allowance replaces the ordinary allowance; the
        // two cannot be added to exceed T on the same frame.
        if (down && m_LayerDrain && fast) m_OrdinaryRemainingUs -= std::min(m_OrdinaryRemainingUs, step);
        currentUs += down ? -int64_t(step) : int64_t(step);
        return currentUs;
    }
    uint64_t m_LastUs = 0, m_InputUs = 0, m_Fraction = 0, m_Frame = 0;
    uint64_t m_OrdinaryRemainingUs = 0, m_OrdinaryDebtUs = 0, m_DrainRemainingUs = 0;
    uint64_t m_ToleranceUs = 0;
    int64_t m_SmoothingUs = 0, m_RecoveryUs = 0;
    bool m_Initialized = false, m_LayerDrain = false, m_HaveFrame = false;
    bool m_FastSmoothing = false, m_FastRecovery = false;
};
}
