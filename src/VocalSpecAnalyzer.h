#pragma once

// Platform-neutral voice-acting audio spec analyzer. Reuses LoudnessMeter.h
// for true BS.1770 K-weighted gated LUFS and computes the remaining ACX /
// audition metrics from decoded WAV frames. No desktop UI headers.

#include "LoudnessMeter.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace tcs
{
namespace vo
{
struct SpecAnalysis
{
    bool passed = false;
    double lufs = -70.0;
    double rmsDb = -70.0;
    double peakDb = -70.0;
    double noiseFloorDb = -90.0;
    double headSilenceSec = 0.0;
    double tailSilenceSec = 0.0;
    double sampleRate = 44100.0;
    int numChannels = 1;
    double durationSec = 0.0;
    juce::String profile;
    juce::String measurement { "bs1770_k_weighted_gated" };
    juce::StringArray issues;
};

class VocalSpecAnalyzer
{
public:
    static SpecAnalysis analyzeWavFile (const juce::File& wavFile,
                                        const juce::String& profile = "acx")
    {
        SpecAnalysis out;
        out.profile = profile.trim().toLowerCase();
        if (out.profile.isEmpty())
            out.profile = "acx";

        if (! wavFile.existsAsFile())
        {
            out.issues.add ("Audio file does not exist: " + wavFile.getFullPathName());
            return out;
        }

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wavFile));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        {
            out.issues.add ("Could not decode WAV file: " + wavFile.getFullPathName());
            return out;
        }

        out.sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
        out.numChannels = static_cast<int> (std::min<unsigned int> (2u, reader->numChannels));
        out.durationSec = static_cast<double> (reader->lengthInSamples) / out.sampleRate;

        tcs::loudness::KWeightedLoudnessMeter meter (out.sampleRate, out.numChannels);
        const int blockSize = 4096;
        const int frameSize = std::max (64, static_cast<int> (std::lround (out.sampleRate * 0.05)));
        juce::AudioBuffer<float> buffer (out.numChannels, blockSize);

        double sumSquares = 0.0;
        double peak = 0.0;
        double frameSum = 0.0;
        int frameCount = 0;
        std::vector<float> frameRms;
        std::vector<float> frameDb;
        juce::int64 firstActiveSample = reader->lengthInSamples;
        juce::int64 lastActiveSample = 0;

        const auto commitFrame = [&]()
        {
            if (frameCount <= 0)
                return;

            const float rms = static_cast<float> (std::sqrt (frameSum / static_cast<double> (frameCount)));
            frameRms.push_back (rms);
            frameCount = 0;
            frameSum = 0.0;
        };

        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += blockSize)
        {
            const int num = static_cast<int> (std::min<juce::int64> (blockSize, reader->lengthInSamples - pos));
            if (! reader->read (&buffer, 0, num, pos, true, out.numChannels > 1))
                break;

            for (int s = 0; s < num; ++s)
            {
                float l = buffer.getSample (0, s);
                float r = out.numChannels > 1 ? buffer.getSample (1, s) : 0.0f;
                meter.processFrame (l, r);

                const float framePeak = std::max (std::abs (l), std::abs (r));
                peak = std::max (peak, static_cast<double> (framePeak));
                sumSquares += static_cast<double> (l) * l + static_cast<double> (r) * r;

                frameSum += static_cast<double> (framePeak) * framePeak;
                if (++frameCount >= frameSize)
                {
                    const float rms = static_cast<float> (std::sqrt (frameSum / static_cast<double> (frameCount)));
                    frameRms.push_back (rms);
                    frameDb.push_back (juce::Decibels::gainToDecibels (std::max (1.0e-7f, rms)));
                    frameCount = 0;
                    frameSum = 0.0;
                }
            }
        }
        commitFrame();

        const double totalSamples = std::max (1.0, static_cast<double> (reader->lengthInSamples) * out.numChannels);
        out.rmsDb = juce::jlimit (-90.0, 0.0,
                                  static_cast<double> (juce::Decibels::gainToDecibels (
                                      static_cast<float> (std::sqrt (sumSquares / totalSamples)))));
        out.peakDb = juce::jlimit (-90.0, 6.0,
                                   static_cast<double> (juce::Decibels::gainToDecibels (
                                       static_cast<float> (std::max (1.0e-7, peak)))));
        out.lufs = juce::jlimit (-70.0, 0.0, meter.getLoudnessLufs());

        if (! frameDb.empty())
        {
            auto sorted = frameDb;
            std::sort (sorted.begin(), sorted.end());
            const size_t idx = std::min (sorted.size() - 1, sorted.size() / 10);
            out.noiseFloorDb = juce::jlimit (-90.0, 0.0, static_cast<double> (sorted[idx]));

            const double gate = std::max (out.noiseFloorDb + 6.0, -60.0);
            for (size_t i = 0; i < frameDb.size(); ++i)
            {
                if (frameDb[i] < gate)
                    continue;
                const juce::int64 sampleAt = static_cast<juce::int64> (i) * frameSize;
                firstActiveSample = std::min (firstActiveSample, sampleAt);
                lastActiveSample = std::max (lastActiveSample, sampleAt + frameSize);
                break;
            }
            for (size_t i = frameDb.size(); i-- > 0;)
            {
                if (frameDb[i] < gate)
                    continue;
                lastActiveSample = std::max (lastActiveSample,
                                             static_cast<juce::int64> ((i + 1) * frameSize));
                break;
            }
        }

        out.headSilenceSec = static_cast<double> (firstActiveSample) / out.sampleRate;
        out.tailSilenceSec = std::max (0.0, static_cast<double> (reader->lengthInSamples - lastActiveSample)) / out.sampleRate;

        if (out.profile == "acx")
        {
            if (out.rmsDb < -23.0 || out.rmsDb > -18.0)
                out.issues.add ("ACX RMS must be between -23 and -18 dB (measured "
                                + juce::String (out.rmsDb, 1) + " dB)");
            if (out.peakDb > -3.0)
                out.issues.add ("ACX peak must not exceed -3 dBFS (measured "
                                + juce::String (out.peakDb, 1) + " dBFS)");
            if (out.noiseFloorDb > -60.0)
                out.issues.add ("ACX noise floor must be at or below -60 dB (measured "
                                + juce::String (out.noiseFloorDb, 1) + " dB)");
            if (out.headSilenceSec < 0.5 || out.headSilenceSec > 1.0)
                out.issues.add ("ACX head silence must be 0.5-1.0 s (measured "
                                + juce::String (out.headSilenceSec, 2) + " s)");
            if (out.tailSilenceSec < 1.0 || out.tailSilenceSec > 5.0)
                out.issues.add ("ACX tail silence must be 1.0-5.0 s (measured "
                                + juce::String (out.tailSilenceSec, 2) + " s)");
        }
        else
        {
            if (out.lufs < -22.0 || out.lufs > -12.0)
                out.issues.add ("Audition integrated loudness must be -22..-12 LUFS (measured "
                                + juce::String (out.lufs, 1) + " LUFS)");
            if (out.peakDb > -1.0)
                out.issues.add ("Audition peak must not exceed -1 dBFS (measured "
                                + juce::String (out.peakDb, 1) + " dBFS)");
            if (out.noiseFloorDb > -55.0)
                out.issues.add ("Audition noise floor must be at or below -55 dB (measured "
                                + juce::String (out.noiseFloorDb, 1) + " dB)");
            if (peak >= 1.0)
                out.issues.add ("Audition file is clipping (sample peak reached full scale)");
        }

        out.passed = out.issues.isEmpty();
        return out;
    }
};
} // namespace vo
} // namespace tcs
