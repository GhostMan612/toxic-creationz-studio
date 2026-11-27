#pragma once

// Platform-neutral, monitor-only Anime ADR cue generator. Header-only: no GUI,
// no VST3, no allocation. The cue is intended strictly for the headphone /
// metronome preview path and must never be written into recorded WAV clips.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace tcs
{
namespace vo
{
struct AdrCueConfig
{
    bool enabled = true;
    double beepIntervalSec = 0.65;
    double lineDurationSec = 4.0;
};

enum class PacerStage { inhale, hold, hiss };

class AdrCueGenerator
{
public:
    static constexpr int pipCount = 3;
    static constexpr double pipDurationSec = 0.06;
    static constexpr double toneHz = 1000.0;
    static constexpr double inhaleSec = 4.0;
    static constexpr double holdSec = 2.0;
    static constexpr double hissSec = 15.0;

    /** Seconds before the live line window at which each pip sounds. */
    static double pipLeadSeconds (int index, const AdrCueConfig& cfg) noexcept
    {
        const double interval = juce::jlimit (0.5, 1.0, cfg.beepIntervalSec);
        return static_cast<double> (pipCount - index) * interval;
    }

    /** Monitor sample for the given transport cursor, or 0 during the live line. */
    static float getMonitorSample (double cursorSec, double lineStartSec, const AdrCueConfig& cfg) noexcept
    {
        if (! cfg.enabled || ! std::isfinite (cursorSec) || ! std::isfinite (lineStartSec))
            return 0.0f;

        const double remaining = lineStartSec - cursorSec;
        if (remaining <= 0.0 || remaining > pipLeadSeconds (0, cfg) + pipDurationSec)
            return 0.0f;

        for (int i = 0; i < pipCount; ++i)
        {
            const double lead = pipLeadSeconds (i, cfg);
            const double since = lead - remaining;
            if (since >= 0.0 && since < pipDurationSec)
                return 0.25f * static_cast<float> (
                    std::sin (juce::MathConstants<double>::twoPi * toneHz * since));
        }
        return 0.0f;
    }

    /** Fill a monitor buffer without allocating; used by the booth preview path. */
    static void renderMonitor (float* dest, int numSamples, double sampleRate,
                               double cursorSec, double lineStartSec,
                               const AdrCueConfig& cfg) noexcept
    {
        if (dest == nullptr || numSamples <= 0 || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
            return;
        const double step = 1.0 / sampleRate;
        double cursor = cursorSec;
        for (int i = 0; i < numSamples; ++i)
        {
            dest[i] = getMonitorSample (cursor, lineStartSec, cfg);
            cursor += step;
        }
    }

    /** 4 s inhale -> 2 s hold -> 15 s hiss, repeated. */
    static PacerStage getPacerStage (double elapsedSec, double& stageRemainingSec) noexcept
    {
        const double cycle = inhaleSec + holdSec + hissSec;
        double t = std::fmod (std::max (0.0, elapsedSec), cycle);
        if (t < inhaleSec)
        {
            stageRemainingSec = inhaleSec - t;
            return PacerStage::inhale;
        }
        t -= inhaleSec;
        if (t < holdSec)
        {
            stageRemainingSec = holdSec - t;
            return PacerStage::hold;
        }
        stageRemainingSec = hissSec - (t - holdSec);
        return PacerStage::hiss;
    }

    static const char* pacerLabel (PacerStage stage) noexcept
    {
        switch (stage)
        {
            case PacerStage::inhale: return "Inhale 4s";
            case PacerStage::hold:   return "Hold 2s";
            case PacerStage::hiss:   return "Hiss 15s";
        }
        return "";
    }
};
} // namespace vo
} // namespace tcs
