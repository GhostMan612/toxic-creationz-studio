#pragma once

// Platform-neutral, deterministic music theory and song-form engine.
// No GUI headers, no external libraries, no file I/O: safe in tcs_core_isolation.

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tcs
{
namespace theory
{
struct ArrangementNote
{
    int track = 0;
    double startBeat = 0.0;
    double lengthBeats = 0.25;
    int midi = 60;
    int velocity = 96;
};

struct ArrangementPlan
{
    juce::String genre;
    juce::String key;
    juce::String scale;
    juce::String timeSignature { "4/4" };
    double bpm = 120.0;
    int bars = 16;
    int seed = 42;
    juce::String complexity { "standard" };
    juce::StringArray sections;
    juce::StringArray trackNames;
    std::vector<ArrangementNote> notes;
};

struct GenreProfile
{
    juce::String name;
    double minBpm;
    double maxBpm;
    juce::String signatureScale;
    juce::String signatureChord;
    juce::String timeSignature { "4/4" };
    double swing = 0.0;
    int density = 2; // 1 sparse .. 4 polyrhythmic
};

class MusicTheoryEngine
{
public:
    static const juce::StringArray& keys()
    {
        static const juce::StringArray list {
            "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
        };
        return list;
    }

    static const juce::StringArray& scales()
    {
        static const juce::StringArray list {
            "major", "natural_minor", "harmonic_minor", "melodic_minor", "dorian",
            "phrygian", "phrygian_dominant", "lydian", "mixolydian", "locrian",
            "pentatonic_major", "pentatonic_minor", "blues", "whole_tone",
            "hirajoshi", "diminished_hw"
        };
        return list;
    }

    static const juce::StringArray& genres()
    {
        static const juce::StringArray list {
            "hiphop_boombap", "trap_dark", "synthwave_80s", "cyberpunk_industrial",
            "lofi_chillhop", "dnb_jungle", "house_deep", "metal_heavy",
            "orchestral_epic", "cosmic_horror", "anime_action", "tactical_stealth",
            "fantasy_tavern", "ambient_cinematic"
        };
        return list;
    }

    static const juce::StringArray& timeSignatures()
    {
        static const juce::StringArray list { "4/4", "3/4", "6/8", "5/4", "7/8" };
        return list;
    }

    static int parseKey (const juce::String& raw)
    {
        const auto key = raw.trim().toLowerCase();
        if (key == "c#" || key == "db") return 1;
        if (key == "d")  return 2;
        if (key == "d#" || key == "eb") return 3;
        if (key == "e")  return 4;
        if (key == "f")  return 5;
        if (key == "f#" || key == "gb") return 6;
        if (key == "g")  return 7;
        if (key == "g#" || key == "ab") return 8;
        if (key == "a")  return 9;
        if (key == "a#" || key == "bb") return 10;
        if (key == "b")  return 11;
        return 0;
    }

    static std::vector<int> scaleIntervals (const juce::String& raw)
    {
        const auto name = raw.trim().toLowerCase();
        if (name == "major" || name == "ionian")            return { 0, 2, 4, 5, 7, 9, 11 };
        if (name == "natural_minor" || name == "aeolian" || name == "minor") return { 0, 2, 3, 5, 7, 8, 10 };
        if (name == "harmonic_minor")                        return { 0, 2, 3, 5, 7, 8, 11 };
        if (name == "melodic_minor")                         return { 0, 2, 3, 5, 7, 9, 11 };
        if (name == "dorian")                                return { 0, 2, 3, 5, 7, 9, 10 };
        if (name == "phrygian")                              return { 0, 1, 3, 5, 7, 8, 10 };
        if (name == "phrygian_dominant" || name == "phrygian dominant") return { 0, 1, 4, 5, 7, 8, 10 };
        if (name == "lydian")                                return { 0, 2, 4, 6, 7, 9, 11 };
        if (name == "mixolydian")                            return { 0, 2, 4, 5, 7, 9, 10 };
        if (name == "locrian")                               return { 0, 1, 3, 5, 6, 8, 10 };
        if (name == "pentatonic_major")                      return { 0, 2, 4, 7, 9 };
        if (name == "pentatonic_minor")                      return { 0, 3, 5, 7, 10 };
        if (name == "blues")                                 return { 0, 3, 5, 6, 7, 10 };
        if (name == "whole_tone")                            return { 0, 2, 4, 6, 8, 10 };
        if (name == "hirajoshi")                             return { 0, 2, 3, 7, 8 };
        if (name == "diminished_hw")                         return { 0, 2, 3, 5, 6, 8, 9, 11 };
        return scaleIntervals ("natural_minor");
    }

    static bool isInScale (int midi, int rootPc, const std::vector<int>& intervals)
    {
        const int pc = ((midi % 12) + 12) % 12;
        const int rel = ((pc - rootPc) % 12 + 12) % 12;
        return std::find (intervals.begin(), intervals.end(), rel) != intervals.end();
    }

    static std::vector<int> chordIntervals (const juce::String& raw)
    {
        const auto symbol = raw.trim().toLowerCase();
        if (symbol == "5")       return { 0, 7, 12 };
        if (symbol == "sus2")    return { 0, 2, 7 };
        if (symbol == "sus4")    return { 0, 5, 7 };
        if (symbol == "add9")    return { 0, 4, 7, 14 };
        if (symbol == "maj7")    return { 0, 4, 7, 11 };
        if (symbol == "m7")      return { 0, 3, 7, 10 };
        if (symbol == "dim7")    return { 0, 3, 6, 9 };
        if (symbol == "m7b5")    return { 0, 3, 6, 10 };
        if (symbol == "9")       return { 0, 4, 7, 10, 14 };
        if (symbol == "7")       return { 0, 4, 7, 10 };
        if (symbol == "m" || symbol == "min") return { 0, 3, 7 };
        if (symbol == "dim")     return { 0, 3, 6 };
        if (symbol == "aug")     return { 0, 4, 8 };
        return { 0, 4, 7 }; // major
    }

    static std::vector<int> chordTones (int rootPc, const juce::String& symbol, int octave = 4)
    {
        std::vector<int> notes;
        for (int interval : chordIntervals (symbol))
            notes.push_back ((octave * 12) + rootPc + interval);
        return notes;
    }

    // Bjorklund-style even distribution via the cumulative Euclidean method.
    static std::vector<int> generateEuclideanPattern (int steps, int pulses, int rotation)
    {
        const int n = juce::jlimit (1, 64, steps);
        const int p = juce::jlimit (0, n, pulses);
        std::vector<int> pattern (static_cast<size_t> (n), 0);
        int bucket = 0;
        for (int i = 0; i < n; ++i)
        {
            bucket += p;
            if (bucket >= n)
            {
                bucket -= n;
                pattern[static_cast<size_t> (i)] = 1;
            }
        }
        if (p == 0)
            return pattern;

        const int shift = ((rotation % n) + n) % n;
        std::vector<int> rotated (static_cast<size_t> (n), 0);
        for (int i = 0; i < n; ++i)
            rotated[static_cast<size_t> ((i + shift) % n)] = pattern[static_cast<size_t> (i)];
        return rotated;
    }

    static GenreProfile genreProfile (const juce::String& raw)
    {
        const auto name = raw.trim().toLowerCase();
        GenreProfile p;
        p.name = name;
        if (name == "trap_dark")            { p.minBpm = 130; p.maxBpm = 150; p.signatureScale = "phrygian"; p.signatureChord = "m7b5"; p.swing = 0.12; p.density = 3; }
        else if (name == "synthwave_80s")   { p.minBpm = 100; p.maxBpm = 118; p.signatureScale = "natural_minor"; p.signatureChord = "maj7"; p.density = 3; }
        else if (name == "cyberpunk_industrial") { p.minBpm = 115; p.maxBpm = 132; p.signatureScale = "locrian"; p.signatureChord = "5"; p.density = 4; }
        else if (name == "lofi_chillhop")   { p.minBpm = 72; p.maxBpm = 86; p.signatureScale = "dorian"; p.signatureChord = "maj7"; p.swing = 0.28; p.density = 1; }
        else if (name == "dnb_jungle")      { p.minBpm = 166; p.maxBpm = 176; p.signatureScale = "natural_minor"; p.signatureChord = "m7"; p.swing = 0.08; p.density = 4; }
        else if (name == "house_deep")      { p.minBpm = 120; p.maxBpm = 126; p.signatureScale = "dorian"; p.signatureChord = "m7"; p.swing = 0.06; p.density = 3; }
        else if (name == "metal_heavy")     { p.minBpm = 125; p.maxBpm = 165; p.signatureScale = "phrygian"; p.signatureChord = "5"; p.density = 4; }
        else if (name == "orchestral_epic") { p.minBpm = 80; p.maxBpm = 115; p.signatureScale = "harmonic_minor"; p.signatureChord = "maj7"; p.density = 2; }
        else if (name == "cosmic_horror")   { p.minBpm = 60; p.maxBpm = 88; p.signatureScale = "locrian"; p.signatureChord = "dim7"; p.density = 1; }
        else if (name == "anime_action")    { p.minBpm = 145; p.maxBpm = 175; p.signatureScale = "hirajoshi"; p.signatureChord = "maj7"; p.density = 4; }
        else if (name == "tactical_stealth"){ p.minBpm = 88; p.maxBpm = 108; p.signatureScale = "natural_minor"; p.signatureChord = "m7b5"; p.density = 1; }
        else if (name == "fantasy_tavern")  { p.minBpm = 105; p.maxBpm = 128; p.signatureScale = "mixolydian"; p.signatureChord = "add9"; p.timeSignature = "6/8"; p.swing = 0.18; p.density = 2; }
        else if (name == "ambient_cinematic") { p.minBpm = 60; p.maxBpm = 80; p.signatureScale = "lydian"; p.signatureChord = "add9"; p.density = 1; }
        else                                { p.minBpm = 84; p.maxBpm = 96; p.signatureScale = "dorian"; p.signatureChord = "m7"; p.swing = 0.22; p.density = 2; } // hiphop_boombap
        return p;
    }

    static ArrangementPlan generateStructuredArrangement (juce::String genre,
                                                          const juce::String& key,
                                                          const juce::String& scale,
                                                          int bars, double bpm,
                                                          std::uint64_t seed,
                                                          const juce::String& complexity,
                                                          bool includeCounterMelody)
    {
        ArrangementPlan plan;
        plan.genre = genre;
        plan.key = key;
        plan.scale = scale;
        plan.bars = juce::jlimit (8, 64, bars);
        plan.seed = static_cast<int> (seed);
        plan.complexity = complexity;
        auto profile = genreProfile (genre);
        plan.bpm = bpm > 0.0 ? bpm : (profile.minBpm + profile.maxBpm) / 2.0;
        plan.timeSignature = profile.timeSignature;
        plan.trackNames = { "Drums", "Bass", "Chords/Pad", "Lead/Arp" };
        if (includeCounterMelody)
            plan.trackNames.add ("Counter-Melody");

        const int rootPc = parseKey (key);
        const auto intervals = scaleIntervals (scale);
        std::uint64_t state = seed ^ 0x9e3779b97f4a7c15ULL;
        const auto rnd = [&state] (int range) -> int
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<int> ((state >> 11) % static_cast<std::uint64_t> (juce::jmax (1, range)));
        };

        const int sectionCount = 5;
        const int sectionBars = juce::jmax (1, plan.bars / sectionCount);
        plan.sections = { "Intro", "Verse", "Chorus/Drop", "Bridge", "Outro" };

        // Motif: four scale degrees, repeated and varied per section.
        std::vector<int> motif;
        for (int i = 0; i < 4; ++i)
        {
            const int degree = rnd (static_cast<int> (intervals.size()));
            motif.push_back (60 + rootPc + intervals[static_cast<size_t> (degree)]);
        }

        const int stepsPerBar = plan.timeSignature == "6/8" ? 12
                              : plan.timeSignature == "5/4" ? 10
                              : plan.timeSignature == "7/8" ? 14
                              : plan.timeSignature == "3/4" ? 12 : 16;
        const double stepBeats = 4.0 / static_cast<double> (stepsPerBar);
        const auto kick = generateEuclideanPattern (stepsPerBar, 4, 0);
        const auto snare = generateEuclideanPattern (stepsPerBar, 2, stepsPerBar / 4);
        const auto hat = generateEuclideanPattern (stepsPerBar, profile.density * 2, 1);
        const auto bassPattern = generateEuclideanPattern (stepsPerBar, 3 + profile.density, 0);
        const auto chordPattern = generateEuclideanPattern (stepsPerBar, 2, 2);

        for (int bar = 0; bar < plan.bars; ++bar)
        {
            const double barStart = static_cast<double> (bar) * 4.0;
            const int sectionIndex = juce::jmin (sectionCount - 1, bar / sectionBars);
            const bool isIntro = sectionIndex == 0;
            const bool isOutro = sectionIndex == 4;
            const bool isChorus = sectionIndex == 2;
            const double energy = isIntro || isOutro ? 0.45 : isChorus ? 1.0 : 0.72;

            // Roman-numeral root motion with a Royal-Road flourish for anime.
            static const int roadDegrees[] { 3, 4, 2, 5 };
            int degree = 0;
            if (genre == "anime_action")
                degree = roadDegrees[bar % 4] % static_cast<int> (intervals.size());
            else
                degree = (bar % 4) * 2 % static_cast<int> (intervals.size());
            const int chordRootPc = (rootPc + intervals[static_cast<size_t> (degree)]) % 12;

            for (int step = 0; step < stepsPerBar; ++step)
            {
                const double at = barStart + static_cast<double> (step) * stepBeats;
                const int idx = static_cast<size_t> (step);
                const int swingShift = step % 2 == 1 ? static_cast<int> (profile.swing * 100.0) : 0;
                const double jitter = static_cast<double> (rnd (7)) / 100.0 + static_cast<double> (swingShift) / 1000.0;
                const double t = at + jitter;

                if (kick[idx]) plan.notes.push_back ({ 0, t, 0.12, 36, isChorus ? 120 : 96 });
                if (snare[idx] && ! isIntro) plan.notes.push_back ({ 0, t, 0.10, 38, 104 });
                if (hat[idx]) plan.notes.push_back ({ 0, t, 0.06, 42, 72 + rnd (18) });

                if (bassPattern[idx] && ! isOutro)
                {
                    const int bassNote = 36 + chordRootPc + (rnd (3) == 0 ? 12 : 0);
                    plan.notes.push_back ({ 1, t, std::max (0.1, stepBeats * 0.9), bassNote, 92 });
                }

                if (chordPattern[idx])
                {
                    for (int tone = 0; tone < 3; ++tone)
                    {
                        const int degreeIndex = (degree + tone * 2) % static_cast<int> (intervals.size());
                        const int midi = 60 + rootPc + intervals[static_cast<size_t> (degreeIndex)];
                        const int clampMidi = juce::jlimit (48, 76, midi);
                        plan.notes.push_back ({ 2, t, 2.0 * energy + 0.5, clampMidi, 78 });
                    }
                }

                if (isChorus || (step % 4 == 0))
                {
                    const int motifNote = motif[static_cast<size_t> ((bar + step / 4) % motif.size())]
                                        + (rnd (5) == 0 ? 12 : 0);
                    const int leadMidi = juce::jlimit (48, 76, motifNote);
                    if (isInScale (leadMidi, rootPc, intervals))
                        plan.notes.push_back ({ 3, t, stepBeats * 1.5, leadMidi, 88 });
                }

                if (includeCounterMelody && (sectionIndex >= 2) && (step % 8 == 4))
                {
                    const int counterMidi = juce::jlimit (48, 76,
                        60 + rootPc + intervals[static_cast<size_t> ((degree + 1) % intervals.size())] + 12);
                    if (isInScale (counterMidi, rootPc, intervals))
                        plan.notes.push_back ({ 4, t, 1.5, counterMidi, 70 });
                }
            }
        }
        return plan;
    }
};
} // namespace theory
} // namespace tcs
