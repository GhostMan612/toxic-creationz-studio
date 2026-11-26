#pragma once

#include <juce_core/juce_core.h>

namespace tcs
{
namespace core
{
    struct CoreTypeIds
    {
        juce::String tr808;
        juce::String sampler;
        juce::String tape;
        juce::String sidechain;
        juce::String transcriber;

        bool complete() const noexcept
        {
            return tr808.isNotEmpty() && sampler.isNotEmpty() && tape.isNotEmpty()
                && sidechain.isNotEmpty() && transcriber.isNotEmpty();
        }
    };

    /** Stable, VST3-free identity of the platform-neutral DSP/engine boundary. */
    CoreTypeIds getCoreTypeIds();
}
}
