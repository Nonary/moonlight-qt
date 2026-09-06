#pragma once

#include "timing.h"
#include "SDL_compat.h"
#include <deque>
#include <list>
#include <wayland-client.h>
#include "../protocols/presentation-time-client-protocol.h"

namespace Vrr {
// All proxies live on a private event queue. Only the render thread dispatches
// callbacks after initialization; SDL retains ownership of the display/surface.
class WaylandFeedback {
public:
    ~WaylandFeedback();
    bool initialize(SDL_Window* window);
    void request(uint64_t id);
    bool poll(Feedback& result);
private:
    struct Pending { WaylandFeedback* owner; struct wp_presentation_feedback* proxy; uint64_t id; Ns requested; uint64_t output = 0; };
    static void global(void*, wl_registry*, uint32_t, const char*, uint32_t);
    static void removed(void*, wl_registry*, uint32_t);
    static void clockId(void*, wp_presentation*, uint32_t);
    static void syncOutput(void*, struct wp_presentation_feedback*, wl_output*);
    static void presented(void*, struct wp_presentation_feedback*, uint32_t, uint32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t, uint32_t);
    static void discarded(void*, struct wp_presentation_feedback*);
    void finish(Pending* pending, Feedback result);
    void pump();
    wl_display* m_Display = nullptr;
    wl_surface* m_Surface = nullptr;
    wl_event_queue* m_Queue = nullptr;
    wp_presentation* m_Presentation = nullptr;
    wl_registry* m_Registry = nullptr;
    int m_Clock = -1;
    uint32_t m_Global = 0;
    Ns m_Offset = 0;
    bool m_HaveOffset = false;
    std::list<Pending> m_Pending;
    std::deque<Feedback> m_Completed;
};
}
