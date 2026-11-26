#pragma once

// Platform-neutral ITU-R BS.1770-style loudness estimator: K-weighting
// (high-shelf + high-pass), 100 ms mean-square blocks, 400 ms gated
// integration with absolute (-70 LUFS) and relative (-10 LU) gates.
// Header-only so it can be used by the desktop IPC server and by any future
// Android/Oboe client without pulling in desktop UI headers.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace tcs
{
namespace loudness
{
class KWeightedLoudnessMeter
{
public:
    explicit KWeightedLoudnessMeter (double sampleRate, int channels)
        : sr (std::max (8000.0, sampleRate)),
          numChannels (juce::jlimit (1, 2, channels))
    {
        const float fs = static_cast<float> (sr);
        // BS.1770-4 K-weighting: +4 dB shelf above 1681.97 Hz, RLB high-pass at 38 Hz.
        const float shelfGain = std::pow (10.0f, 4.0f / 20.0f);
        shelfCoefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (fs, 1681.974f, 0.70710678f, shelfGain);
        highPassCoefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (fs, 38.0f, 0.5f);

        shelf.resize (static_cast<size_t> (numChannels));
        highPass.resize (static_cast<size_t> (numChannels));
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& s = shelf[static_cast<size_t> (ch)];
            auto& h = highPass[static_cast<size_t> (ch)];
            s.coefficients = shelfCoefficients;
            h.coefficients = highPassCoefficients;
            s.reset();
            h.reset();
        }

        blockSamples = std::max (64, static_cast<int> (std::lround (sr * 0.1)));
    }

    // Feed one interleaved frame (channel 1 required, channel 2 optional).
    void processFrame (float ch1, float ch2)
    {
        const float in[2] { ch1, ch2 };
        double sum = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto idx = static_cast<size_t> (ch);
            float v = highPass[idx].processSample (in[idx]);
            v = shelf[idx].processSample (v);
            sum += static_cast<double> (v) * static_cast<double> (v);
        }

        blockSum += sum;
        if (++blockCount >= blockSamples)
            pushBlock (blockSum / static_cast<double> (blockCount));
    }

    // Convenience for planar buffers of equal length.
    void processPlanar (const float* ch1, const float* ch2, int num)
    {
        for (int i = 0; i < num; ++i)
            processFrame (ch1[i], numChannels > 1 && ch2 != nullptr ? ch2[i] : 0.0f);
    }

    // -0.691 LU offset converts mean square to LUFS.
    double getLoudnessLufs() const
    {
        if (blockMeans.size() < 4)
            return -70.0;

        std::vector<double> windows;
        windows.reserve (blockMeans.size());
        for (size_t i = 0; i + 4 <= blockMeans.size(); ++i)
        {
            double sum = 0.0;
            for (size_t j = 0; j < 4; ++j)
                sum += blockMeans[i + j];
            windows.push_back (toLufs (sum / 4.0));
        }

        if (windows.empty())
            return -70.0;

        double ungatedSum = 0.0;
        for (double w : windows)
            ungatedSum += std::pow (10.0, (w + 0.691) / 10.0);
        const double ungatedLufs = toLufs (ungatedSum / static_cast<double> (windows.size()));

        const double absoluteGate = -70.0;
        const double relativeGate = ungatedLufs - 10.0;
        double gatedSum = 0.0;
        int gatedCount = 0;
        for (double w : windows)
        {
            if (w < absoluteGate || w < relativeGate)
                continue;
            gatedSum += std::pow (10.0, (w + 0.691) / 10.0);
            ++gatedCount;
        }

        return gatedCount > 0 ? toLufs (gatedSum / static_cast<double> (gatedCount)) : -70.0;
    }

private:
    static double toLufs (double meanSquare)
    {
        return -0.691 + 10.0 * std::log10 (std::max (1.0e-12, meanSquare));
    }

    void pushBlock (double meanSquare)
    {
        blockMeans.push_back (meanSquare);
        blockSum = 0.0;
        blockCount = 0;
    }

    double sr = 44100.0;
    int numChannels = 1;
    int blockSamples = 4410;
    int blockCount = 0;
    double blockSum = 0.0;
    std::vector<double> blockMeans;
    juce::dsp::IIR::Coefficients<float>::Ptr shelfCoefficients, highPassCoefficients;
    std::vector<juce::dsp::IIR::Filter<float>> shelf, highPass;
};
} // namespace loudness
} // namespace tcs
