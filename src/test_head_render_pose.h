#pragma once
#include <cassert>
// Owned-memory native sampler for adapter tests. The usual source is mesh+40;
// tests that vary the render phase replace partRenderSampler with their own spy.
// Production still resolves and validates the body's actual vtable entry.
static float fixturePartRenderSampler(uintptr_t sampler, uintptr_t head, float) {
    assert(sampler == gameAt(fp::sig::headRenderSample));
    const uintptr_t mesh = *(uintptr_t*)(head + field::bodyPart::meshNode);
    const float* source = (const float*)(mesh + field::sceneNode::transform);
    float* rendered = (float*)(mesh + field::sceneNode::renderTransform);
    for (int row = 0; row < 4; ++row) {
        for (int axis = 0; axis < 3; ++axis) rendered[row * 4 + axis] = source[row * 3 + axis];
        rendered[row * 4 + 3] = row == 3 ? 1.f : 0.f;
    }
    return 0.f;
}
static void fixtureHeadRenderSampling(uintptr_t head) {
    const uintptr_t table = head + 0x800;
    *(uintptr_t*)head = table;
    *(uintptr_t*)(table + partRenderSampleSlot) = gameAt(fp::sig::headRenderSample);
    partRenderSampler = fixturePartRenderSampler;
}
