#include "StickSynth.h"

#include <cassert>
#include <cmath>

using namespace makcm::stick;

int main() {
    PhysicalPriorityOverride state;
    assert(!state.latched() && state.syntheticAllowed());
    assert(!state.observe({0.08f, 0.0f}, 0));
    assert(state.observe({0.11f, 0.0f}, 1));
    assert(state.latched() && !state.syntheticAllowed());
    // Hysteresis: values between off/on do not release the latch.
    assert(state.observe({0.08f, 0.0f}, 10));
    assert(state.observe({0.05f, 0.0f}, 20));
    assert(state.latched());
    assert(!state.observe({0.05f, 0.0f}, 40));
    assert(!state.latched() && state.awaitingFresh() && !state.syntheticAllowed());
    state.notifyFreshSequence(2);
    assert(state.syntheticAllowed());

    // The same helper can enforce the fresh-sequence barrier automatically.
    state.reset();
    assert(state.observe({0.2f, 0.0f}, 1, 0));
    assert(state.latched());
    assert(state.observe({0.0f, 0.0f}, 1, 40));
    assert(!state.observe({0.0f, 0.0f}, 1, 60));
    assert(state.awaitingFresh());
    assert(!state.observe({0.0f, 0.0f}, 2, 61));
    assert(state.syntheticAllowed());

    const StickVector synthetic{0.5f, -0.25f};
    const StickVector physical{0.12f, 0.0f};
    // Non-neutral physical input remains authoritative in the neutral-only
    // policy; centered physical input allows the fresh synthetic value.
    assert(std::fabs(state.merge(physical, synthetic).x - physical.x) < 0.0001f);
    const StickVector merged = state.merge({0.0f, 0.0f}, synthetic);
    assert(std::fabs(merged.x - synthetic.x) < 0.0001f &&
           std::fabs(merged.y - synthetic.y) < 0.0001f);
    const StickVector clamped = circleClamp({2.0f, 2.0f});
    assert(std::fabs(stickMagnitude(clamped) - 1.0f) < 0.0001f);
    assert(axisDeadzone(0.09f, 0.10f) == 0.0f);
    assert(std::fabs(axisDeadzone(0.55f, 0.10f) - 0.5f) < 0.0001f);
    assert(std::fabs(axisDeadzone(-0.55f, 0.10f) + 0.5f) < 0.0001f);
    const StickVector radial_zero = radialDeadzone({0.06f, 0.08f}, 0.10f);
    assert(radial_zero.x == 0.0f && radial_zero.y == 0.0f);
    const StickVector radial = radialDeadzone({0.3f, 0.4f}, 0.10f);
    assert(std::fabs(stickMagnitude(radial) - (0.5f - 0.1f) / 0.9f) < 0.0001f);
    return 0;
}
