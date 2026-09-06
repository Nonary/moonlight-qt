#include "wayland.h"

#include <SDL_syswm.h>
#include <cstring>
#include <poll.h>
#include <time.h>

namespace Vrr {
WaylandFeedback::~WaylandFeedback()
{
    for (auto& p : m_Pending) wp_presentation_feedback_destroy(p.proxy);
    if (m_Presentation) wp_presentation_destroy(m_Presentation);
    if (m_Registry) wl_registry_destroy(m_Registry);
    if (m_Queue) wl_event_queue_destroy(m_Queue);
}

bool WaylandFeedback::initialize(SDL_Window* window)
{
#ifdef SDL_VIDEO_DRIVER_WAYLAND
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WAYLAND) return false;
    m_Display = info.info.wl.display;
    m_Surface = info.info.wl.surface;
    m_Queue = wl_display_create_queue(m_Display);
    if (!m_Queue) return false;
    // A wrapper assigns the child proxy to our queue before any other thread
    // can read an event for it. Setting the queue after construction races SDL.
    auto wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(m_Display));
    if (!wrapper) return false;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), m_Queue);
    m_Registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (!m_Registry) return false;
    static const wl_registry_listener listener{global, removed};
    wl_registry_add_listener(m_Registry, &listener, this);
    if (wl_display_roundtrip_queue(m_Display, m_Queue) < 0 || !m_Presentation ||
        wl_display_roundtrip_queue(m_Display, m_Queue) < 0) return false;
    // Reject wall clocks that can jump/slew. CLOCK_MONOTONIC is the common case;
    // RAW is accepted with an explicitly measured conversion at each event.
    return m_Clock == CLOCK_MONOTONIC || m_Clock == CLOCK_MONOTONIC_RAW;
#else
    (void)window;
    return false;
#endif
}

void WaylandFeedback::global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    auto* me = static_cast<WaylandFeedback*>(data);
    if (strcmp(interface, wp_presentation_interface.name) || me->m_Presentation) return;
    me->m_Global = name;
    // Version 1 suffices for measured presentation. Its refresh=0 on VRR is
    // intentionally accepted; a compositor prediction is not a mode range.
    me->m_Presentation = static_cast<wp_presentation*>(wl_registry_bind(registry, name, &wp_presentation_interface, std::min(version, 1u)));
    static const wp_presentation_listener listener{clockId};
    wp_presentation_add_listener(me->m_Presentation, &listener, me);
}

void WaylandFeedback::removed(void* data, wl_registry*, uint32_t name)
{
    auto* me = static_cast<WaylandFeedback*>(data);
    if (name == me->m_Global) {
        wp_presentation_destroy(me->m_Presentation);
        me->m_Presentation = nullptr;
        Feedback f; f.outcome = Outcome::Reset; f.observed = now();
        if (me->m_Completed.size() == 64) me->m_Completed.pop_front();
        me->m_Completed.push_back(f);
    }
}

void WaylandFeedback::clockId(void* data, wp_presentation*, uint32_t id)
{
    static_cast<WaylandFeedback*>(data)->m_Clock = int(id);
}

void WaylandFeedback::request(uint64_t id)
{
    pump();
    // Bound protocol objects even when a hidden window never receives feedback.
    if (m_Pending.size() == 32) {
        Feedback f; f.outcome = Outcome::Unavailable; f.observed = now();
        finish(&m_Pending.front(), f);
    }
    if (!m_Presentation) return;
    auto proxy = wp_presentation_feedback(m_Presentation, m_Surface);
    if (!proxy) return;
    m_Pending.push_back({this, proxy, id, now(), 0});
    static const wp_presentation_feedback_listener listener{syncOutput, presented, discarded};
    wp_presentation_feedback_add_listener(proxy, &listener, &m_Pending.back());
    // No empty wl_surface_commit here: the following native swap owns it.
}

void WaylandFeedback::syncOutput(void* data, struct wp_presentation_feedback*, wl_output* output)
{
    static_cast<Pending*>(data)->output = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(output));
}

void WaylandFeedback::presented(void* data, struct wp_presentation_feedback*, uint32_t hi, uint32_t lo,
                                 uint32_t ns, uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags)
{
    auto* p = static_cast<Pending*>(data);
    auto* me = p->owner;
    Feedback f;
    const Ns before = now();
    timespec clockTime{};
    const bool validClock = clock_gettime(me->m_Clock, &clockTime) == 0;
    f.observed = now();
    const Ns offset = before + (f.observed - before) / 2 -
        (Ns(clockTime.tv_sec) * Second + clockTime.tv_nsec);
    const uint64_t seconds = uint64_t(hi) << 32 | lo;
    f.outcome = Outcome::Unavailable;
    if (validClock && ns < Second && seconds < uint64_t(INT64_MAX / Second) &&
        (!me->m_HaveOffset || std::abs(offset - me->m_Offset) < Millisecond)) {
        f.presented = Ns(seconds) * Second + ns + offset;
        f.uncertainty = (f.observed - before + 1) / 2 + 1000;
        f.sequence = uint64_t(seqHi) << 32 | seqLo;
        f.refresh = refresh; f.flags = flags; f.output = p->output;
        f.quality = (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK) &&
                    (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION) &&
                    (flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC) ? Quality::Hardware : Quality::Compositor;
        f.outcome = Outcome::Presented;
    }
    else if (me->m_HaveOffset && std::abs(offset - me->m_Offset) >= Millisecond) {
        f.outcome = Outcome::Reset;
    }
    me->m_Offset = offset; me->m_HaveOffset = validClock;
    me->finish(p, f);
}

void WaylandFeedback::discarded(void* data, struct wp_presentation_feedback*)
{
    auto* p = static_cast<Pending*>(data);
    Feedback f; f.outcome = Outcome::Discarded; f.observed = now();
    p->owner->finish(p, f);
}

void WaylandFeedback::finish(Pending* p, Feedback f)
{
    f.id = p->id;
    wp_presentation_feedback_destroy(p->proxy);
    if (m_Completed.size() == 64) m_Completed.pop_front();
    m_Completed.push_back(f);
    for (auto it = m_Pending.begin(); it != m_Pending.end(); ++it) {
        if (&*it == p) { m_Pending.erase(it); break; }
    }
}

void WaylandFeedback::pump()
{
    if (!m_Display || !m_Queue) return;
    // Nonblocking shared-connection read protocol, interoperable with SDL's
    // event thread. Never dispatch SDL's default queue from the renderer.
    if (wl_display_prepare_read_queue(m_Display, m_Queue) == 0) {
        wl_display_flush(m_Display);
        pollfd fd{wl_display_get_fd(m_Display), POLLIN, 0};
        if (::poll(&fd, 1, 0) > 0 && (fd.revents & POLLIN)) wl_display_read_events(m_Display);
        else wl_display_cancel_read(m_Display);
    }
    wl_display_dispatch_queue_pending(m_Display, m_Queue);
    while (!m_Pending.empty() && now() - m_Pending.front().requested > 250 * Millisecond) {
        Feedback f; f.outcome = Outcome::Unavailable; f.observed = now();
        finish(&m_Pending.front(), f);
    }
}

bool WaylandFeedback::poll(Feedback& result)
{
    pump();
    if (m_Completed.empty()) return false;
    result = m_Completed.front(); m_Completed.pop_front();
    return true;
}
}
