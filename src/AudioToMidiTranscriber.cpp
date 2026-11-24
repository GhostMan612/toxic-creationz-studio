#include "AudioToMidiTranscriber.h"
#include "AutomationManager.h"

#include <algorithm>
#include <cmath>

float AudioToMidiTranscriber::estimateFrameFrequencyYin (const float* frame,
                                                         int windowSize,
                                                         double sampleRate,
                                                         float threshold,
                                                         int minMidi,
                                                         int maxMidi)
{
    const int halfW = windowSize / 2;

    if (halfW < 8 || sampleRate <= 0.0)
        return 0.0f;

    const double maxFreq = 440.0 * std::pow (2.0, (static_cast<double> (maxMidi) - 69.0) / 12.0);
    const double minFreq = 440.0 * std::pow (2.0, (static_cast<double> (minMidi) - 69.0) / 12.0);

    const int tauMin   = juce::jlimit (2, halfW - 2, static_cast<int> (std::floor (sampleRate / maxFreq)));
    const int tauMax   = juce::jlimit (tauMin + 1, halfW - 1, static_cast<int> (std::ceil (sampleRate / minFreq)));
    const int tauLimit = std::min (halfW - 1, tauMax + 1);

    std::vector<float> diff (static_cast<size_t> (tauLimit + 1), 0.0f);

    for (int tau = 1; tau <= tauLimit; ++tau)
    {
        double sum = 0.0;

        for (int j = 0; j < halfW; ++j)
        {
            const double d = static_cast<double> (frame[j]) - static_cast<double> (frame[j + tau]);
            sum += d * d;
        }

        diff[static_cast<size_t> (tau)] = static_cast<float> (sum);
    }

    std::vector<float> cmndf (static_cast<size_t> (tauLimit + 1), 1.0f);
    double runningSum = 0.0;

    for (int tau = 1; tau <= tauLimit; ++tau)
    {
        runningSum += static_cast<double> (diff[static_cast<size_t> (tau)]);

        if (runningSum > 1e-12)
        {
            cmndf[static_cast<size_t> (tau)] = static_cast<float> (
                (static_cast<double> (diff[static_cast<size_t> (tau)]) * static_cast<double> (tau)) / runningSum);
        }
    }

    int bestTau = -1;

    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (cmndf[static_cast<size_t> (tau)] < threshold)
        {
            while (tau + 1 <= tauMax && cmndf[static_cast<size_t> (tau + 1)] < cmndf[static_cast<size_t> (tau)])
                ++tau;

            bestTau = tau;
            break;
        }
    }

    if (bestTau < tauMin)
        return 0.0f;

    // Parabolic interpolation around bestTau
    double refinedTau = static_cast<double> (bestTau);

    if (bestTau > 1 && bestTau + 1 <= tauLimit)
    {
        const double s0 = static_cast<double> (cmndf[static_cast<size_t> (bestTau - 1)]);
        const double s1 = static_cast<double> (cmndf[static_cast<size_t> (bestTau)]);
        const double s2 = static_cast<double> (cmndf[static_cast<size_t> (bestTau + 1)]);
        const double denom = 2.0 * (s0 - 2.0 * s1 + s2);

        if (std::abs (denom) > 1e-12)
            refinedTau += juce::jlimit (-1.0, 1.0, (s0 - s2) / denom);
    }

    if (refinedTau <= 0.0)
        return 0.0f;

    return static_cast<float> (sampleRate / refinedTau);
}

std::vector<TranscribedNote> AudioToMidiTranscriber::transcribeBuffer (const juce::AudioBuffer<float>& buffer,
                                                                       double sampleRate,
                                                                       double bpm,
                                                                       const TranscriptionOptions& opts)
{
    std::vector<TranscribedNote> result;
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (numSamples < 2048 || numChannels <= 0 || sampleRate <= 0.0)
        return result;

    const double safeBpm = std::clamp (bpm, 40.0, 240.0);
    const double beatsPerSecond = safeBpm / 60.0;

    // Mono mixdown for pitch estimation
    std::vector<float> mono (static_cast<size_t> (numSamples), 0.0f);
    const float chScale = 1.0f / static_cast<float> (numChannels);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = buffer.getReadPointer (ch);

        for (int i = 0; i < numSamples; ++i)
            mono[static_cast<size_t> (i)] += src[i] * chScale;
    }

    constexpr int windowSize = 2048;
    constexpr int hopSize    = 512;
    const int numFrames = 1 + (numSamples - windowSize) / hopSize;

    if (numFrames <= 0)
        return result;

    const float rmsThreshold = juce::Decibels::decibelsToGain (opts.silenceDb);
    std::vector<int> rawPitches (static_cast<size_t> (numFrames), -1);
    std::vector<float> frameRms (static_cast<size_t> (numFrames), 0.0f);

    for (int f = 0; f < numFrames; ++f)
    {
        const int offset = f * hopSize;
        const float* framePtr = mono.data() + offset;

        double sqSum = 0.0;

        for (int i = 0; i < windowSize; ++i)
            sqSum += static_cast<double> (framePtr[i]) * static_cast<double> (framePtr[i]);

        const float rms = static_cast<float> (std::sqrt (sqSum / static_cast<double> (windowSize)));
        frameRms[static_cast<size_t> (f)] = rms;

        if (rms < rmsThreshold)
            continue;

        const float freq = estimateFrameFrequencyYin (framePtr, windowSize, sampleRate,
                                                      opts.yinThreshold, opts.minMidiNote, opts.maxMidiNote);

        if (freq > 0.0f)
        {
            const int midiNote = static_cast<int> (
                std::round (69.0 + 12.0 * std::log2 (static_cast<double> (freq) / 440.0)));

            if (midiNote >= opts.minMidiNote && midiNote <= opts.maxMidiNote)
                rawPitches[static_cast<size_t> (f)] = midiNote;
        }
    }

    // 3-frame median smoothing on voiced frames to eliminate single-frame octave spikes
    std::vector<int> smoothed = rawPitches;

    for (int f = 1; f + 1 < numFrames; ++f)
    {
        const int p0 = rawPitches[static_cast<size_t> (f - 1)];
        const int p1 = rawPitches[static_cast<size_t> (f)];
        const int p2 = rawPitches[static_cast<size_t> (f + 1)];

        if (p0 >= 0 && p1 >= 0 && p2 >= 0)
        {
            int tri[3] = { p0, p1, p2 };
            std::sort (tri, tri + 3);
            smoothed[static_cast<size_t> (f)] = tri[1];
        }
    }

    // Segment contiguous frames with identical MIDI pitch
    int segStart = -1;
    int segPitch = -1;
    float segPeakRms = 0.0f;

    auto flushSegment = [&] (int segEndExclusive)
    {
        if (segStart < 0 || segPitch < 0 || segEndExclusive <= segStart)
            return;

        const double startSec = static_cast<double> (segStart * hopSize) / sampleRate;
        const double endSec   = static_cast<double> (segEndExclusive * hopSize + windowSize / 2) / sampleRate;
        double startBeat = startSec * beatsPerSecond;
        double lenBeats  = std::max (0.0, (endSec - startSec) * beatsPerSecond);

        if (lenBeats < opts.minNoteBeats)
            return;

        if (opts.quantizeBeats > 0.0)
        {
            startBeat = std::max (0.0, std::round (startBeat / opts.quantizeBeats) * opts.quantizeBeats);
            lenBeats  = std::max (opts.quantizeBeats, std::round (lenBeats / opts.quantizeBeats) * opts.quantizeBeats);
        }

        const int vel = juce::jlimit (40, 127,
                                      static_cast<int> (std::round (40.0f + std::min (1.0f, segPeakRms * 2.5f) * 87.0f)));
        result.push_back ({ segPitch, startBeat, lenBeats, vel });
    };

    for (int f = 0; f < numFrames; ++f)
    {
        const int p = smoothed[static_cast<size_t> (f)];

        if (p == segPitch && p >= 0)
        {
            segPeakRms = std::max (segPeakRms, frameRms[static_cast<size_t> (f)]);
        }
        else
        {
            flushSegment (f);
            segStart   = (p >= 0) ? f : -1;
            segPitch   = p;
            segPeakRms = (p >= 0) ? frameRms[static_cast<size_t> (f)] : 0.0f;
        }
    }

    flushSegment (numFrames);
    return result;
}

std::vector<TranscribedNote> AudioToMidiTranscriber::transcribeFile (const juce::File& audioFile,
                                                                     double bpm,
                                                                     const TranscriptionOptions& opts)
{
    if (! audioFile.existsAsFile())
        return {};

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (audioFile));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0.0)
        return {};

    // Cap single-pass transcription buffer to 60 seconds to bound memory on large stems
    const int maxSamples = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples,
                                                                    static_cast<juce::int64> (reader->sampleRate * 60.0)));

    if (maxSamples < 2048)
        return {};

    juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), maxSamples);

    if (! reader->read (&buffer, 0, maxSamples, 0, true, true))
        return {};

    return transcribeBuffer (buffer, reader->sampleRate, bpm, opts);
}

TranscriptionCommitResult AudioToMidiTranscriber::transcribeClipToMidi (te::Edit& edit,
                                                                        tracktion::EditItemID sourceWaveClipID,
                                                                        int targetTrackIndex,
                                                                        const TranscriptionOptions& opts)
{
    TranscriptionCommitResult res;

    te::WaveAudioClip* srcClip = nullptr;

    for (auto* track : te::getAudioTracks (edit))
    {
        for (auto* clip : track->getClips())
        {
            if (clip->itemID == sourceWaveClipID)
            {
                srcClip = dynamic_cast<te::WaveAudioClip*> (clip);
                break;
            }
        }

        if (srcClip != nullptr)
            break;
    }

    if (srcClip == nullptr)
    {
        res.errorMessage = "Source WaveAudioClip not found";
        return res;
    }

    const juce::File wavFile = srcClip->getSourceFileReference().getFile();

    if (! wavFile.existsAsFile())
    {
        res.errorMessage = "Source audio file missing: " + wavFile.getFullPathName();
        return res;
    }

    const auto* tempoSetting = edit.tempoSequence.getTempo (0);
    const double sessionBpm = tempoSetting != nullptr ? tempoSetting->getBpm() : 124.0;
    const auto clipPos = srcClip->getPosition();
    const double srcStartBeat = edit.tempoSequence.toBeats (clipPos.getStart()).inBeats();
    const double srcEndBeat   = edit.tempoSequence.toBeats (clipPos.getEnd()).inBeats();
    const double clipSpanBeats = std::max (0.0, srcEndBeat - srcStartBeat);
    const juce::String srcName = srcClip->getName();

    // Respect clip trim offset, auto-tempo stretch ratio, and pitchChange semitones
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wavFile));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0.0)
    {
        res.errorMessage = "Unreadable audio source: " + wavFile.getFileName();
        return res;
    }

    const juce::int64 offsetSamples = juce::jlimit<juce::int64> (
        0,
        std::max<juce::int64> (0, reader->lengthInSamples - 2048),
        static_cast<juce::int64> (std::llround (std::max (0.0, clipPos.getOffset().inSeconds()) * reader->sampleRate)));
    const juce::int64 availSamples = reader->lengthInSamples - offsetSamples;
    const int maxSamples = static_cast<int> (std::min<juce::int64> (
        availSamples, static_cast<juce::int64> (reader->sampleRate * 60.0)));

    if (maxSamples < 2048)
    {
        res.errorMessage = "Audio clip region too short for pitch analysis";
        return res;
    }

    juce::AudioBuffer<float> clipBuf (static_cast<int> (reader->numChannels), maxSamples);

    if (! reader->read (&clipBuf, 0, maxSamples, offsetSamples, true, true))
    {
        res.errorMessage = "Failed to read audio samples from clip";
        return res;
    }

    double effectiveBpm = sessionBpm;
    const double fileDurSec = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;

    if (srcClip->getAutoTempo() && srcClip->getLoopInfo().getNumBeats() > 0.0 && fileDurSec > 0.01)
        effectiveBpm = std::clamp (60.0 * (srcClip->getLoopInfo().getNumBeats() / fileDurSec), 40.0, 240.0);
    else if (srcClip->getSpeedRatio() > 0.05)
        effectiveBpm = std::clamp (sessionBpm / srcClip->getSpeedRatio(), 40.0, 240.0);

    auto notes = transcribeBuffer (clipBuf, reader->sampleRate, effectiveBpm, opts);
    reader.reset();

    const int pitchShiftSemis = static_cast<int> (std::round (srcClip->getPitchChange()));

    if (pitchShiftSemis != 0)
    {
        for (auto& n : notes)
            n.pitch = juce::jlimit (0, 127, n.pitch + pitchShiftSemis);
    }

    if (clipSpanBeats >= 0.25)
    {
        notes.erase (std::remove_if (notes.begin(), notes.end(),
                                     [clipSpanBeats] (const TranscribedNote& n)
                                     {
                                         return n.startBeat >= clipSpanBeats - 1e-4;
                                     }),
                     notes.end());

        for (auto& n : notes)
            if (n.startBeat + n.lengthBeats > clipSpanBeats)
                n.lengthBeats = std::max (0.125, clipSpanBeats - n.startBeat);
    }

    if (notes.empty())
    {
        res.errorMessage = "No pitched notes detected in audio clip";
        return res;
    }

    double maxNoteEnd = 4.0;

    for (const auto& n : notes)
        maxNoteEnd = std::max (maxNoteEnd, n.startBeat + n.lengthBeats);

    const double clipLengthBeats = std::max (4.0, std::ceil (maxNoteEnd / 4.0) * 4.0);
    const tracktion::TimeRange midiRange {
        edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (srcStartBeat)),
        edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (srcStartBeat + clipLengthBeats))
    };

    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("Transcribe Audio to MIDI");

    auto tracks = te::getAudioTracks (edit);
    te::AudioTrack* destTrack = nullptr;
    int resolvedTrackIdx = targetTrackIndex;

    if (juce::isPositiveAndBelow (targetTrackIndex, tracks.size()))
    {
        destTrack = tracks[targetTrackIndex];
    }
    else
    {
        edit.ensureNumberOfAudioTracks (tracks.size() + 1);
        tracks = te::getAudioTracks (edit);
        destTrack = tracks.getLast();
        resolvedTrackIdx = tracks.size() - 1;
        destTrack->setName (srcName + " (MIDI)");

        if (auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
            destTrack->pluginList.insertPlugin (synth,
                                                AutomationManager::getInsertIndexBeforeTerminalPlugins (*destTrack),
                                                nullptr);
    }

    const juce::String midiClipName = srcName + " MIDI";
    auto* midiClip = dynamic_cast<te::MidiClip*> (
        destTrack->insertNewClip (te::TrackItem::Type::midi, midiClipName, midiRange, nullptr));

    if (midiClip == nullptr)
    {
        res.errorMessage = "Failed to insert transcribed MidiClip";
        return res;
    }

    auto& seq = midiClip->getSequence();

    for (const auto& n : notes)
    {
        seq.addNote (n.pitch,
                     tracktion::BeatPosition::fromBeats (n.startBeat),
                     tracktion::BeatDuration::fromBeats (n.lengthBeats),
                     n.velocity,
                     0,
                     &um);
    }

    res.success          = true;
    res.targetTrackIndex = resolvedTrackIdx;
    res.createdClipID    = midiClip->itemID;
    res.clipName         = midiClipName;
    res.notes            = std::move (notes);
    return res;
}
