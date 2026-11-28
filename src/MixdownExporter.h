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

    /** Render a beat range with an overhang tail folded back for a click-free loop. */
    static bool exportSeamlessLoopWav (te::Edit& edit,
                                       double startBeat, double endBeat,
                                       const juce::File& outputWavFile,
                                       double targetLufs = -14.0,
                                       double tailSeconds = 2.0);
};
