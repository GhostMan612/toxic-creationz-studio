#include "RiffLibrary.h"

#include <algorithm>
#include <cmath>

juce::File RiffLibrary::getDefaultLibraryDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
             .getChildFile ("Toxic Creationz Studio")
             .getChildFile ("RiffLibrary");
}

int RiffLibrary::parseKeyPitchClass (const juce::String& keyText)
{
    const auto upper = keyText.trim().toUpperCase();

    if (upper.startsWith ("C#") || upper.startsWith ("DB")) return 1;
    if (upper.startsWith ("C"))  return 0;
    if (upper.startsWith ("D#") || upper.startsWith ("EB")) return 3;
    if (upper.startsWith ("D"))  return 2;
    if (upper.startsWith ("E"))  return 4;
    if (upper.startsWith ("F#") || upper.startsWith ("GB")) return 6;
    if (upper.startsWith ("F"))  return 5;
    if (upper.startsWith ("G#") || upper.startsWith ("AB")) return 8;
    if (upper.startsWith ("G"))  return 7;
    if (upper.startsWith ("A#") || upper.startsWith ("BB")) return 10;
    if (upper.startsWith ("B"))  return 11;

    return 9; // Default A
}

int RiffLibrary::computeClosestSemitoneShift (int sourcePitchClass, int targetPitchClass)
{
    int diff = (targetPitchClass - sourcePitchClass) % 12;

    if (diff > 6)  diff -= 12;
    if (diff < -6) diff += 12;

    return diff;
}

juce::String RiffLibrary::roleToTrackName (RiffRole role)
{
    switch (role)
    {
        case RiffRole::drums: return "BGM Drums";
        case RiffRole::bass:  return "BGM Bass";
        case RiffRole::lead:  return "BGM Lead";
        case RiffRole::pad:   return "BGM Pad";
    }

    return "BGM Stem";
}

//==============================================================================
// Procedural riff synthesis: 2 bars / 8 beats of stereo 16-bit PCM at 44.1 kHz.
//==============================================================================
bool RiffLibrary::writeProceduralRiffWav (const juce::File& file,
                                          RiffRole role,
                                          double bpm,
                                          int bars,
                                          int rootMidiNote,
                                          int variantSeed)
{
    if (file.existsAsFile() && file.getSize() > 512)
        return true;

    file.getParentDirectory().createDirectory();

    constexpr double sampleRate = 44100.0;
    const double secondsPerBeat = 60.0 / std::max (40.0, bpm);
    const int totalBeats = std::max (1, bars * 4);
    const int totalSamples = static_cast<int> (std::llround (totalBeats * secondsPerBeat * sampleRate));

    juce::AudioBuffer<float> buffer (2, totalSamples);
    buffer.clear();

    auto* left  = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);

    const auto midiToHz = [] (double note) -> double
    {
        return 440.0 * std::pow (2.0, (note - 69.0) / 12.0);
    };

    double phase1 = 0.0;
    double phase2 = 0.0;

    for (int i = 0; i < totalSamples; ++i)
    {
        const double tSec = static_cast<double> (i) / sampleRate;
        const double beatPos = tSec / secondsPerBeat;
        const double beatFrac = beatPos - std::floor (beatPos);
        const int step16 = static_cast<int> (beatPos * 4.0) % 16;

        float sampleL = 0.0f;
        float sampleR = 0.0f;

        if (role == RiffRole::drums)
        {
            const double kickFreq = 48.0 + 110.0 * std::exp (-beatFrac * 28.0);
            const double kickEnv  = std::exp (-beatFrac * 9.0);

            phase1 += kickFreq / sampleRate;

            const float kick = static_cast<float> (std::sin (phase1 * juce::MathConstants<double>::twoPi) * kickEnv * 0.55);

            const double subFrac = (beatPos * 2.0) - std::floor (beatPos * 2.0);
            const float hatEnv = static_cast<float> (std::exp (-subFrac * 35.0) * ((step16 % 2 == 1) ? 0.18 : 0.06));
            const float noise = (static_cast<float> ((i * 1103515245 + 12345) & 0x7fff) / 16384.0f - 1.0f) * hatEnv;

            sampleL = juce::jlimit (-0.9f, 0.9f, kick + noise);
            sampleR = juce::jlimit (-0.9f, 0.9f, kick - noise * 0.6f);
        }
        else if (role == RiffRole::bass)
        {
            static const int offsetsA[] = { 0, 0, 3, 0, 5, 0, 7, 10 };
            static const int offsetsB[] = { 0, 7, 3, 5, 0, 3, 10, 7 };

            const int idx = static_cast<int> (beatPos * 2.0) % 8;
            const int semi = (variantSeed % 2 == 0) ? offsetsA[idx] : offsetsB[idx];

            const double freq = midiToHz (rootMidiNote - 12 + semi);
            const double env = std::exp (-((beatPos * 2.0) - std::floor (beatPos * 2.0)) * 5.5);

            phase1 += freq / sampleRate;

            if (phase1 >= 1.0) phase1 -= 1.0;

            const float saw = static_cast<float> ((2.0 * phase1 - 1.0) * env * 0.38);
            const float sub = static_cast<float> (std::sin (phase1 * juce::MathConstants<double>::twoPi) * env * 0.35);

            sampleL = sampleR = saw + sub;
        }
        else if (role == RiffRole::lead)
        {
            static const int arpA[] = { 0, 3, 7, 12, 7, 3, 10, 7 };
            static const int arpB[] = { 12, 10, 7, 3, 0, 3, 7, 10 };

            const int semi = (variantSeed % 2 == 0) ? arpA[step16 % 8] : arpB[step16 % 8];

            const double freq = midiToHz (rootMidiNote + 12 + semi);
            const double stepFrac = (beatPos * 4.0) - std::floor (beatPos * 4.0);
            const double env = std::exp (-stepFrac * 6.5);

            phase1 += freq / sampleRate;
            phase2 += (freq * 1.004) / sampleRate;

            sampleL = static_cast<float> (std::sin (phase1 * juce::MathConstants<double>::twoPi) * env * 0.30);
            sampleR = static_cast<float> (std::sin (phase2 * juce::MathConstants<double>::twoPi) * env * 0.30);
        }
        else
        {
            const double f1 = midiToHz (rootMidiNote);
            const double f2 = midiToHz (rootMidiNote + 3);
            const double f3 = midiToHz (rootMidiNote + 7);

            const float swell = 0.55f + 0.35f * static_cast<float> (std::sin (tSec * 1.5));

            const float c1 = static_cast<float> (std::sin (tSec * f1 * juce::MathConstants<double>::twoPi));
            const float c2 = static_cast<float> (std::sin (tSec * f2 * juce::MathConstants<double>::twoPi));
            const float c3 = static_cast<float> (std::sin (tSec * f3 * juce::MathConstants<double>::twoPi));

            sampleL = (c1 + c2 * 0.8f + c3 * 0.6f) * 0.14f * swell;
            sampleR = (c1 * 0.8f + c2 + c3 * 0.9f) * 0.14f * swell;
        }

        left[i]  = sampleL;
        right[i] = sampleR;
    }

    juce::WavAudioFormat wavFormat;

    // JUCE 9: createWriterFor now takes AudioFormatWriterOptions and takes
    // ownership of the stream unique_ptr on success.
    std::unique_ptr<juce::FileOutputStream> fileStream (file.createOutputStream());

    if (fileStream == nullptr)
        return false;

    fileStream->setPosition (0);
    fileStream->truncate();

    std::unique_ptr<juce::OutputStream> outStream (fileStream.release());

    const juce::AudioFormatWriterOptions options =
        juce::AudioFormatWriterOptions{}
            .withSampleRate (sampleRate)
            .withNumChannels (2)
            .withBitsPerSample (16)
            .withQualityOptionIndex (0);

    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (outStream, options));

    if (writer == nullptr)
        return false;

    return writer->writeFromAudioSampleBuffer (buffer, 0, totalSamples);
}

void RiffLibrary::ensureStarterPack (const juce::File& targetDirectory)
{
    targetDirectory.createDirectory();

    struct StarterSpec
    {
        const char* fileName;
        RiffRole role;
        double bpm;
        int rootMidi;
        int seed;
    };

    static const StarterSpec specs[] = {
        { "Cyberpunk_Drums_124bpm_Am_E4_NeonPulse.wav",   RiffRole::drums, 124.0, 57, 0 },
        { "Cyberpunk_Bass_124bpm_Am_E4_SubDrive.wav",     RiffRole::bass,  124.0, 57, 0 },
        { "Cyberpunk_Lead_124bpm_Am_E4_GridArp.wav",      RiffRole::lead,  124.0, 57, 0 },
        { "Cyberpunk_Pad_124bpm_Am_E3_DarkHorizon.wav",   RiffRole::pad,   124.0, 57, 0 },
        { "Synthwave_Drums_120bpm_Am_E3_RetroGate.wav",   RiffRole::drums, 120.0, 57, 1 },
        { "Synthwave_Bass_120bpm_Am_E3_AnalogOctave.wav", RiffRole::bass,  120.0, 57, 1 },
        { "Synthwave_Lead_120bpm_Am_E4_Starlight.wav",    RiffRole::lead,  120.0, 57, 1 },
        { "Synthwave_Pad_120bpm_Am_E2_WarmTape.wav",      RiffRole::pad,   120.0, 57, 1 }
    };

    for (const auto& s : specs)
        writeProceduralRiffWav (targetDirectory.getChildFile (s.fileName),
                                s.role, s.bpm, 2, s.rootMidi, s.seed);

    scanDirectory (targetDirectory);
}

void RiffLibrary::scanDirectory (const juce::File& targetDirectory)
{
    riffs.clear();

    if (! targetDirectory.isDirectory())
        return;

    auto files = targetDirectory.findChildFiles (juce::File::findFiles, false, "*.wav");
    files.sort();

    for (const auto& f : files)
    {
        const auto tokens = juce::StringArray::fromTokens (f.getFileNameWithoutExtension(), "_", "");

        RiffMetadata meta;

        meta.id      = f.getFileNameWithoutExtension();
        meta.wavFile = f;
        meta.genre   = tokens.size() > 0 ? tokens[0] : "Cyberpunk";

        const auto roleStr = (tokens.size() > 1 ? tokens[1] : "Drums").toLowerCase();

        if (roleStr.contains ("bass"))      meta.role = RiffRole::bass;
        else if (roleStr.contains ("lead")) meta.role = RiffRole::lead;
        else if (roleStr.contains ("pad"))  meta.role = RiffRole::pad;
        else                                meta.role = RiffRole::drums;

        if (tokens.size() > 2 && tokens[2].endsWithIgnoreCase ("bpm"))
            meta.bpm = std::clamp (tokens[2].dropLastCharacters (3).getDoubleValue(), 40.0, 240.0);

        meta.keyLabel = tokens.size() > 3 ? tokens[3] : "Am";
        meta.rootPitchClass = parseKeyPitchClass (meta.keyLabel);

        if (tokens.size() > 4 && tokens[4].startsWithIgnoreCase ("E"))
            meta.energy = juce::jlimit (1, 5, tokens[4].substring (1).getIntValue());

        meta.name = tokens.size() > 5 ? tokens[5] : f.getFileNameWithoutExtension();

        // Read the real loop length from the WAV header instead of assuming
        // 2 bars: a 4-bar user loop treated as 2 bars would play double-speed
        // under autoTempo. Falls back to 2 bars if the file is unreadable.
        meta.bars = 2;

        juce::WavAudioFormat wavFormat;

        if (std::unique_ptr<juce::AudioFormatReader> reader (
                wavFormat.createReaderFor (f.createInputStream().release(), true)))
        {
            if (reader->sampleRate > 0.0 && reader->lengthInSamples > 0)
            {
                const double durationSec = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
                const double beats = durationSec * (meta.bpm / 60.0);
                meta.bars = std::max (1, static_cast<int> (std::round (beats / 4.0)));
            }
        }

        riffs.push_back (std::move (meta));
    }
}

const RiffMetadata* RiffLibrary::findBestMatch (RiffRole role,
                                                const juce::String& genre,
                                                int targetEnergy,
                                                int targetPitchClass) const
{
    const RiffMetadata* best = nullptr;
    int bestScore = -99999;

    for (const auto& r : riffs)
    {
        if (r.role != role || ! r.wavFile.existsAsFile())
            continue;

        int score = 0;

        if (r.genre.equalsIgnoreCase (genre))
            score += 50;

        score -= std::abs (r.energy - targetEnergy) * 8;

        if (r.role != RiffRole::drums)
            score -= std::abs (computeClosestSemitoneShift (r.rootPitchClass, targetPitchClass)) * 2;

        if (score > bestScore)
        {
            bestScore = score;
            best = &r;
        }
    }

    return best;
}

//==============================================================================
// BgmArrangementSolver
//==============================================================================
int BgmArrangementSolver::generateArrangement (te::Edit& edit,
                                               const RiffLibrary& library,
                                               const BgmRequest& request)
{
    static const juce::Identifier bgmSolverClipProp ("bgmSolverClip");

    const int totalBars = std::max (4, request.totalBars);
    edit.ensureNumberOfAudioTracks (4);
    auto tracks = te::getAudioTracks (edit);

    const RiffRole roles[4] = {
        RiffRole::drums,
        RiffRole::bass,
        RiffRole::lead,
        RiffRole::pad
    };

    int insertedClips = 0;

    for (int rIdx = 0; rIdx < 4; ++rIdx)
    {
        const auto role = roles[rIdx];

        // 1. Look up matching riff FIRST; if none exists, leave track name & clips untouched
        const auto* riff = library.findBestMatch (role,
                                                  request.genre,
                                                  request.targetEnergy,
                                                  request.targetPitchClass);
        if (riff == nullptr)
            continue;

        auto* track = tracks[rIdx];
        track->setName (RiffLibrary::roleToTrackName (role));

        // 2. Remove ONLY WaveAudioClips previously created by BgmArrangementSolver
        for (int c = track->getClips().size() - 1; c >= 0; --c)
        {
            auto* clip = track->getClips()[c];
            if (dynamic_cast<te::WaveAudioClip*> (clip) != nullptr
                && static_cast<bool> (clip->state.getProperty (bgmSolverClipProp, false)))
            {
                clip->removeFromParent();
            }
        }

        const int loopBars = std::max (1, riff->bars);
        const int semitoneShift = (role == RiffRole::drums)
                                      ? 0
                                      : RiffLibrary::computeClosestSemitoneShift (
                                            riff->rootPitchClass, request.targetPitchClass);

        for (int bar = 0; bar < totalBars; bar += loopBars)
        {
            // 4-stage Intro -> Groove -> Peak -> Outro arrangement gating
            const bool isIntro  = (bar < 2);
            const bool isGroove = (! isIntro && bar < totalBars / 2 && totalBars >= 8);
            const bool isOutro  = (bar >= totalBars - 2 && totalBars >= 8);

            if (isIntro && role == RiffRole::lead)
                continue;
            if (isIntro && role == RiffRole::drums && request.targetEnergy < 4)
                continue;
            if (isGroove && role == RiffRole::lead && request.targetEnergy < 4)
                continue;
            if (isOutro && (role == RiffRole::drums || role == RiffRole::bass) && request.targetEnergy < 5)
                continue;

            const double startBeat = static_cast<double> (bar * 4);
            const double endBeat   = static_cast<double> (std::min (totalBars, bar + loopBars) * 4);

            const tracktion::TimeRange clipRange {
                edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat)),
                edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endBeat))
            };

            const te::ClipPosition clipPos { clipRange, tracktion::TimeDuration::fromSeconds (0.0) };
            if (auto waveClip = track->insertWaveClip (riff->name, riff->wavFile, clipPos, false))
            {
                waveClip->state.setProperty (bgmSolverClipProp, true, nullptr);
                waveClip->getLoopInfo().setNumBeats (static_cast<double> (loopBars * 4));
                waveClip->setAutoTempo (true);
                waveClip->setAutoPitch (false);
                waveClip->setPitchChange (static_cast<float> (semitoneShift));
                ++insertedClips;
            }
        }
    }

    return insertedClips;
}
