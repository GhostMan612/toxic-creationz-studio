#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <tracktion_engine/tracktion_engine.h>
#include <vector>

namespace te = tracktion::engine;

struct TranscribedNote
{
    int pitch = 60;
    double startBeat = 0.0;
    double lengthBeats = 0.5;
    int velocity = 100;
};

struct TranscriptionOptions
{
    double quantizeBeats = 0.25;   // 0.0 disables quantization
    double minNoteBeats  = 0.125;  // Ignore micro-glitches shorter than 1/32
    float yinThreshold   = 0.15f;  // YIN CMNDF threshold
    float silenceDb      = -38.0f; // RMS energy gate in dBFS
    int minMidiNote      = 28;     // E1 (~41.2 Hz)
    int maxMidiNote      = 96;     // C7 (~2093 Hz)
};

struct TranscriptionCommitResult
{
    bool success = false;
    int targetTrackIndex = -1;
    tracktion::EditItemID createdClipID;
    juce::String clipName;
    std::vector<TranscribedNote> notes;
    juce::String errorMessage;
};

class AudioToMidiTranscriber
{
public:
    static std::vector<TranscribedNote> transcribeBuffer (const juce::AudioBuffer<float>& buffer,
                                                          double sampleRate,
                                                          double bpm,
                                                          const TranscriptionOptions& opts = {});

    static std::vector<TranscribedNote> transcribeFile (const juce::File& audioFile,
                                                        double bpm,
                                                        const TranscriptionOptions& opts = {});

    // Polyphonic Constant-Q-style multi-peak tracker. The monophonic YIN path
    // above is untouched; this only adds a second, independent detector.
    static std::vector<TranscribedNote> transcribePolyphonic (const juce::File& audioFile,
                                                              double bpm,
                                                              int maxVoices = 4,
                                                              float peakThresholdDb = -28.0f);

    static TranscriptionCommitResult transcribePolyphonicClipToMidi (
        te::Edit& edit,
        tracktion::EditItemID sourceWaveClipID,
        int targetTrackIndex = -1,
        int maxVoices = 4,
        float peakThresholdDb = -28.0f);

    static TranscriptionCommitResult transcribeClipToMidi (te::Edit& edit,
                                                           tracktion::EditItemID sourceWaveClipID,
                                                           int targetTrackIndex = -1,
                                                           const TranscriptionOptions& opts = {});

private:
    static float estimateFrameFrequencyYin (const float* frame,
                                            int windowSize,
                                            double sampleRate,
                                            float threshold,
                                            int minMidi,
                                            int maxMidi);
};
