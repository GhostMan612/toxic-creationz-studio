#pragma once

// Platform-neutral, deterministic, zero-asset procedural game SFX engine.
// No GUI, no VST3, no external sample or image dependencies.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tcs
{
namespace game
{
enum class SfxCategory
{
    gunshotTactical,
    breachExplosion,
    footstepSurface,
    uiClick,
    meleeImpact,
    doorBreach,
    sciFiPulse,
    ambientDrone
};

struct SfxRenderParams
{
    double durationSec = 0.5;
    std::uint64_t seed = 42;
    double peakDb = -1.0;
    bool suppressed = false;
    juce::String surface { "concrete" };
};

class SfxForgeEngine
{
public:
    static SfxCategory parseCategory (const juce::String& raw)
    {
        const auto name = raw.trim().toLowerCase();
        if (name == "breach_explosion" || name == "explosion") return SfxCategory::breachExplosion;
        if (name == "footstep_surface" || name == "footstep")   return SfxCategory::footstepSurface;
        if (name == "ui_click" || name == "click")              return SfxCategory::uiClick;
        if (name == "melee_impact" || name == "melee")           return SfxCategory::meleeImpact;
        if (name == "door_breach" || name == "door")             return SfxCategory::doorBreach;
        if (name == "sci_fi_pulse" || name == "scifi")           return SfxCategory::sciFiPulse;
        if (name == "ambient_drone" || name == "drone")          return SfxCategory::ambientDrone;
        return SfxCategory::gunshotTactical;
    }

    static juce::String categoryName (SfxCategory category) noexcept
    {
        switch (category)
        {
            case SfxCategory::gunshotTactical: return "gunshot_tactical";
            case SfxCategory::breachExplosion: return "breach_explosion";
            case SfxCategory::footstepSurface: return "footstep_surface";
            case SfxCategory::uiClick:         return "ui_click";
            case SfxCategory::meleeImpact:     return "melee_impact";
            case SfxCategory::doorBreach:      return "door_breach";
            case SfxCategory::sciFiPulse:      return "sci_fi_pulse";
            case SfxCategory::ambientDrone:    return "ambient_drone";
        }
        return "gunshot_tactical";
    }

    static juce::AudioBuffer<float> renderSfx (SfxCategory category,
                                               const SfxRenderParams& params = {})
    {
        constexpr double sampleRate = 44100.0;
        const int frames = juce::jlimit (64, static_cast<int> (20.0 * sampleRate),
                                         static_cast<int> (std::lround (params.durationSec * sampleRate)));
        juce::AudioBuffer<float> buffer (1, frames);
        buffer.clear();

        std::uint64_t state = params.seed ^ 0x9e3779b97f4a7c15ULL;
        const auto noise = [&state]() -> float
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            const double v = static_cast<double> (static_cast<std::int64_t> (state % 2000001ULL)) / 1000000.0 - 1.0;
            return static_cast<float> (v);
        };

        const float surfaceLowpass = params.surface.equalsIgnoreCase ("metal") ? 0.82f
                                   : params.surface.equalsIgnoreCase ("gravel") ? 0.48f
                                   : params.surface.equalsIgnoreCase ("wood") ? 0.66f : 0.36f;
        const double pitchFactor = std::pow (2.0,
            1.5 * std::sin (static_cast<double> (params.seed) * 0.731) / 12.0);
        const double decayScale = 1.0 + 0.10 * std::sin (static_cast<double> (params.seed) * 1.193);
        float filteredNoise = 0.0f;
        double polyPhase = 0.0;

        for (int i = 0; i < frames; ++i)
        {
            const double t = static_cast<double> (i) / sampleRate;
            const double pt = t * pitchFactor;
            const double dt = t / decayScale;
            const float n = noise();
            filteredNoise += surfaceLowpass * (n - filteredNoise);
            float v = 0.0f;

            switch (category)
            {
                case SfxCategory::gunshotTactical:
                {
                    const float decay = params.suppressed ? 26.0f : 58.0f;
                    v = static_cast<float> (n * std::exp (-decay * dt) * 0.9
                        + filteredNoise * std::exp (-(params.suppressed ? 9.0 : 4.0) * dt) * 0.5
                        + std::sin (juce::MathConstants<double>::twoPi * 58.0 * pt)
                          * std::exp (-14.0 * dt) * 0.45);
                    break;
                }
                case SfxCategory::breachExplosion:
                {
                    const double fallingHz = 92.0 * std::exp (-2.4 * t) + 28.0;
                    polyPhase += fallingHz * pitchFactor / sampleRate;
                    v = static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * polyPhase) * 0.6
                        + filteredNoise * std::exp (-3.2 * dt) * 0.55
                        + n * std::exp (-18.0 * dt) * 0.35);
                    break;
                }
                case SfxCategory::footstepSurface:
                {
                    const double heel = std::exp (-70.0 * dt);
                    const double toe = t > 0.11 ? std::exp (-48.0 * (t - 0.11) / decayScale) : 0.0;
                    v = static_cast<float> ((n * heel + filteredNoise * toe) * 0.7);
                    break;
                }
                case SfxCategory::uiClick:
                {
                    const double hz = (1350.0 - 700.0 * juce::jmin (1.0, t * 12.0)) * pitchFactor;
                    v = static_cast<float> ((std::sin (juce::MathConstants<double>::twoPi * hz * t)
                        + 0.3 * std::sin (juce::MathConstants<double>::twoPi * hz * 2.0 * t))
                        * std::exp (-45.0 * dt) * 0.65);
                    break;
                }
                case SfxCategory::meleeImpact:
                {
                    const double ring = std::sin (juce::MathConstants<double>::twoPi * 3100.0 * pt) * 0.35
                                      + std::sin (juce::MathConstants<double>::twoPi * 4370.0 * pt) * 0.25;
                    v = static_cast<float> (ring * std::exp (-9.0 * dt)
                        + std::sin (juce::MathConstants<double>::twoPi * 120.0 * pt) * std::exp (-20.0 * dt) * 0.5);
                    break;
                }
                case SfxCategory::doorBreach:
                {
                    const double latch = std::exp (-160.0 * dt) * (t < 0.02 ? 1.0 : 0.0);
                    const double burst = t > 0.04 ? std::exp (-12.0 * (t - 0.04) / decayScale) : 0.0;
                    v = static_cast<float> (n * (latch + filteredNoise * burst * 0.8));
                    break;
                }
                case SfxCategory::sciFiPulse:
                {
                    const double hz = 280.0 + 1200.0 * juce::jmin (1.0, t * 2.0);
                    const double fm = 3.5 * std::sin (juce::MathConstants<double>::twoPi * 42.0 * pt);
                    v = static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * hz * t + fm)
                        * std::exp (-5.0 * dt) * 0.7);
                    break;
                }
                case SfxCategory::ambientDrone:
                {
                    const double a = std::sin (juce::MathConstants<double>::twoPi * 110.0 * pt);
                    const double b = std::sin (juce::MathConstants<double>::twoPi * 110.37 * pt);
                    const double c = std::sin (juce::MathConstants<double>::twoPi * 220.21 * pt) * 0.35;
                    v = static_cast<float> ((a + b + c) * 0.22 + filteredNoise * 0.18);
                    break;
                }
            }

            buffer.setSample (0, i, v);
        }

        const float peak = buffer.getMagnitude (0, frames);
        const float target = static_cast<float> (std::pow (10.0, juce::jlimit (-60.0, 0.0, params.peakDb) / 20.0));
        if (peak > 1.0e-6f)
            for (int i = 0; i < frames; ++i)
                buffer.setSample (0, i, juce::jlimit (-1.0f, 1.0f,
                    buffer.getSample (0, i) * target / peak));

        return buffer;
    }

    static int generateSfxVariations (SfxCategory category, const juce::File& outputDir,
                                      const juce::String& prefix, int count,
                                      std::uint64_t baseSeed, double durationSec,
                                      double peakDb, juce::Array<juce::File>* outFiles = nullptr)
    {
        const int takeCount = juce::jlimit (1, 16, count);
        outputDir.createDirectory();
        int written = 0;

        for (int take = 0; take < takeCount; ++take)
        {
            auto params = SfxRenderParams {};
            params.seed = baseSeed + static_cast<std::uint64_t> (take) * 7919ULL;
            params.durationSec = durationSec * (1.0 + 0.05 * std::sin (static_cast<double> (take)));
            params.peakDb = peakDb;
            params.surface = "concrete";
            params.suppressed = false;

            const auto buffer = renderSfx (category, params);
            const auto file = outputDir.getChildFile (prefix + "_"
                + juce::String (take + 1).paddedLeft ('0', 2) + ".wav");

            juce::WavAudioFormat wavFormat;
            auto stream = std::make_unique<juce::FileOutputStream> (file);
            if (! stream->openedOk())
                continue;
            stream->setPosition (0);
            stream->truncate();
            std::unique_ptr<juce::OutputStream> os (stream.release());
            const auto options = juce::AudioFormatWriterOptions{}
                                     .withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16);
            auto writer = std::unique_ptr<juce::AudioFormatWriter> (wavFormat.createWriterFor (os, options));
            if (writer == nullptr)
                continue;
            if (! writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()))
                continue;

            if (outFiles != nullptr)
                outFiles->add (file);
            ++written;
        }
        return written;
    }
};
} // namespace game
} // namespace tcs
