#include "../../app/streaming/video/ffmpeg-renderers/compositionpresentpolicy.h"

#include <cstdio>
#include <limits>

int main()
{
    int failures = 0;
    const auto check = [&](bool valid, const char* description) {
        if (!valid) {
            std::fprintf(stderr, "FAIL: %s\n", description);
            ++failures;
        }
    };
    const auto target = CompositionPresentTarget::translate(3000, 100000, 1000, 1020);
    check(target.time100ns == 119900 && target.uncertaintyUs == 11,
          "future display deadline crosses clock domains without losing its lead");
    check(CompositionPresentTarget::translate(2000, 100000, 1000, 1020).time100ns == 109900 &&
          CompositionPresentTarget::translate(10000, 100000, 1000, 1020).time100ns == 189900,
          "2 ms and 10 ms deadlines reach native scheduling 8 ms apart");
    check(CompositionPresentTarget::translate(1000, 100000, 1000, 1020).time100ns == 100000 &&
          CompositionPresentTarget::translate(1010, 100000, 1000, 1020).time100ns == 100000,
          "past or current deadline requests immediate presentation with no extra frame");
    const auto widest = CompositionPresentTarget::translate(3000, 100000, 1000, 1500);
    check(widest.time100ns == 117500 && widest.uncertaintyUs == 251,
          "largest admitted clock bracket preserves conservative uncertainty");
    check(CompositionPresentTarget::translate(3000, 100000, 1000, 1501).time100ns == 0,
          "an excessively uncertain clock correlation is rejected");
    check(CompositionPresentTarget::translate(3000, 0, 1000, 1020).time100ns == 0 &&
          CompositionPresentTarget::translate(3000, 100000, 0, 1020).time100ns == 0 &&
          CompositionPresentTarget::translate(3000, 100000, 1020, 1000).time100ns == 0,
          "missing clocks and reversed brackets cannot fabricate native deadlines");
    constexpr auto max = (std::numeric_limits<uint64_t>::max)();
    check(CompositionPresentTarget::translate(max, 100000, 1000, 1020).time100ns == 0 &&
          CompositionPresentTarget::translate(1011, max - 9, 1000, 1020).time100ns == 0 &&
          CompositionPresentTarget::translate(1011, max - 10, 1000, 1020).time100ns == max,
          "native target arithmetic rejects overflow while retaining exact upper bound");
    check(CompositionPresentTarget::translate(1011, max, 1000, 1020).time100ns == 0 &&
          CompositionPresentTarget::translate(max, 100000, max - 20, max - 2).time100ns == 100110,
          "clock correlation near integer limit preserves the midpoint without overflow");

    CompositionPresentOwnership<5> ownership;
    check(ownership.outstanding() == 0, "ownership starts empty");
    for (size_t slot = 0; slot < 5; ++slot) {
        check(ownership.accept(slot, 100 + slot), "each displayable slot admits one accepted present");
    }
    check(ownership.outstanding() == 5 && !ownership.accept(0, 200) &&
          !ownership.accept(5, 200), "accepted ownership cannot exceed physical storage");
    ownership.available(2);
    check(ownership.outstanding() == 4 && !ownership.accept(2, 101),
          "native availability releases one slot but cannot duplicate a live present ID");
    check(ownership.accept(2, 200), "a natively available slot accepts its next present");
    ownership.terminal(999);
    check(ownership.outstanding() == 5, "unmatched status cannot release a different present");
    ownership.terminal(103);
    check(ownership.outstanding() == 4 && ownership.accept(3, 201),
          "skip or cancel releases exactly its accepted present");
    ownership.retireThrough(101);
    check(ownership.outstanding() == 3, "retiring fence releases only IDs through its completed value");
    ownership.terminal(200);
    check(ownership.outstanding() == 2, "out of order terminal evidence preserves newer accepted images");
    ownership.available(99);
    ownership.terminal(0);
    check(ownership.outstanding() == 2 && !ownership.accept(0, 0),
          "invalid availability and reserved ID cannot change ownership");
    ownership.reset();
    check(ownership.outstanding() == 0 && ownership.accept(0, 1),
          "lifecycle reset retires the old epoch before accepting a new one");
    return failures ? 1 : 0;
}
