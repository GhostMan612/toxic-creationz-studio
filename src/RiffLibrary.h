#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <tracktion_engine/tracktion_engine.h>

#include <vector>

namespace te = tracktion::engine;

enum class RiffRole
{
    drums = 0,
    bass  = 1,
    lead  = 2,
    pad   = 3
};

struct RiffMetadata
{
    juce::String id;
    juce::String name;
    juce::String genre;       // "Cyberpunk", "Synthwave"
    RiffRole role = RiffRole::drums;
    double bpm = 124.0;
    int rootPitchClass = 9;   // 0 = C, 2 = D, 4 = E, 7 = G, 9 = A
    juce::String keyLabel;    // "Am", "Cm", etc.
    int energy = 3;           // 1..5
    int bars = 2;             // loop length in bars (4/4)
    juce::File wavFile;
};

struct BgmRequest
{
    juce::String genre = "Cyberpunk";
    int targetPitchClass = 9; // 9 = A minor default
    juce::String targetKeyLabel = "Am";
    int targetEnergy = 4;     // 1..5
    int totalBars = 8;        // 8 or 16 bars
};

//==============================================================================
// Indexes riff loops by genre / role / BPM / key / energy, and can synthesise a
// starter pack of real .wav files so sessions and headless CI runs never depend
// on external assets.
//==============================================================================
class RiffLibrary
{
public:
    RiffLibrary() = default;

    void ensureStarterPack (const juce::File& targetDirectory);
    void scanDirectory (const juce::File& targetDirectory);

    const std::vector<RiffMetadata>& getRiffs() const noexcept { return riffs; }

    const RiffMetadata* findBestMatch (RiffRole role,
                                      const juce::String& genre,
                                      int targetEnergy,
                                      int targetPitchClass) const;

    static juce::File getDefaultLibraryDirectory();
    static int parseKeyPitchClass (const juce::String& keyText);
    static int computeClosestSemitoneShift (int sourcePitchClass, int targetPitchClass);
    static juce::String roleToTrackName (RiffRole role);

private:
    static bool writeProceduralRiffWav (const juce::File& file,
                                       RiffRole role,
                                       double bpm,
                                       int bars,
                                       int rootMidiNote,
                                       int variantSeed);

    std::vector<RiffMetadata> riffs;
};

//==============================================================================
// MTV Music Generator-style layering: pick the best riff per stem role and lay
// an Intro -> Groove -> Peak -> Outro arc across the requested bars, with
// Tracktion autoTempo stretching and semitone transposition to the target key.
//==============================================================================
class BgmArrangementSolver
{
public:
    static int generateArrangement (te::Edit& edit,
                                    const RiffLibrary& library,
                                    const BgmRequest& request);
};
