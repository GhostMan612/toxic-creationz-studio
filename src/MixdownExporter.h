#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

class MixdownExporter
{
public:
    static tracktion::TimeRange getDefaultExportRange (te::Edit& edit);

    static bool exportEditToWav (te::Edit& edit,
                                 const juce::File& outputWavFile,
                                 tracktion::TimeRange range,
                                 double sampleRate = 44100.0,
                                 int bitDepth = 16);
};
