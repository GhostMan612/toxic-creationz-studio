#pragma once

// Platform-neutral, zero-allocation real-time procedural RPG soundboard.
// No GUI headers, no external samples, no heap use in applyToBuffer().

#include <juce_audio_processors/juce_audio_processors.h>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace te = tracktion::engine;

class RpgSoundboardPlugin : public te::Plugin
{
public:
    static inline const char* xmlTypeName = "tcs_rpg_soundboard";

    static const char* const* biomeNames() noexcept
    {
        static const char* const names[] {
            "drowned_stars", "rainy_tavern", "haunted_crypt",
            "stormy_sea", "ancient_forest", "boss_altar"
        };
        return names;
    }
    static const char* const* sfxNames() noexcept
    {
        static const char* const names[] {
            "sword_clash", "fireball_cast", "eldritch_whisper", "dragon_roar",
            "heavy_stone_door", "critical_hit", "healing_chime", "thunder_crack",
            "bow_release", "dice_roll"
        };
        return names;
    }
    static constexpr int numBiomes = 6;
    static constexpr int numSfx = 10;

    static int parseBiome (const juce::String& raw)
    {
        const auto name = raw.trim().toLowerCase();
        for (int i = 0; i < numBiomes; ++i)
            if (name == biomeNames()[i])
                return i;
        return 0;
    }

    static int parseSfx (const juce::String& raw)
    {
        const auto name = raw.trim().toLowerCase();
        for (int i = 0; i < numSfx; ++i)
            if (name == sfxNames()[i])
                return i;
        return 0;
    }

    explicit RpgSoundboardPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        ambientValue.referTo (state, juce::Identifier ("ambientLevel"), nullptr, 0.75f);
        sfxValue.referTo     (state, juce::Identifier ("sfxLevel"), nullptr, 0.85f);
        moodValue.referTo    (state, juce::Identifier ("moodIntensity"), nullptr, 0.35f);
        crossfadeValue.referTo (state, juce::Identifier ("crossfadeSec"), nullptr, 2.0f);
        ceilingValue.referTo (state, juce::Identifier ("masterCeilingDb"), nullptr, -1.0f);
        biomeValue.referTo   (state, juce::Identifier ("biome"), nullptr, "drowned_stars");

        ambientParam = addParam ("ambientLevel", TRANS("Ambient Level"), { 0.0f, 1.0f });
        sfxParam     = addParam ("sfxLevel", TRANS("SFX Level"), { 0.0f, 1.0f });
        moodParam    = addParam ("moodIntensity", TRANS("Mood Intensity"), { 0.0f, 1.0f });
        crossfadeParam = addParam ("crossfadeSec", TRANS("Crossfade (s)"), { 0.5f, 5.0f });
        ceilingParam = addParam ("masterCeilingDb", TRANS("Master Ceiling (dB)"), { -24.0f, 0.0f });

        ambientParam->attachToCurrentValue (ambientValue);
        sfxParam->attachToCurrentValue (sfxValue);
        moodParam->attachToCurrentValue (moodValue);
        crossfadeParam->attachToCurrentValue (crossfadeValue);
        ceilingParam->attachToCurrentValue (ceilingValue);
    }

    ~RpgSoundboardPlugin() override
    {
        if (ambientParam != nullptr) ambientParam->detachFromCurrentValue();
        if (sfxParam != nullptr) sfxParam->detachFromCurrentValue();
        if (moodParam != nullptr) moodParam->detachFromCurrentValue();
        if (crossfadeParam != nullptr) crossfadeParam->detachFromCurrentValue();
        if (ceilingParam != nullptr) ceilingParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getName() const override { return "RPG Live Soundboard"; }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins != nullptr)
        {
            ins->add ("MIDI In");
            ins->add ("Audio In L");
            ins->add ("Audio In R");
        }
        if (outs != nullptr)
        {
            outs->add ("Audio Out L");
            outs->add ("Audio Out R");
        }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::stereo(),
                                                   te::ChannelConfiguration::stereo());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 2; }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, ambientValue, sfxValue, moodValue,
                                          crossfadeValue, ceilingValue, biomeValue);
        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        sampleRate = std::max (8000.0, info.sampleRate);
        phase = 0.0;
        rng = 0x9e3779b9u ^ static_cast<std::uint32_t> (static_cast<std::int64_t> (sampleRate));
        fadePos = 1.0;
        activeBiome = parseBiome (biomeValue.get());
        previousBiome = activeBiome;
        for (auto& voice : voices)
        {
            voice.active = false;
            voice.age = 0.0;
        }
        initialised = true;
    }

    void deinitialise() override
    {
        initialised = false;
        for (auto& voice : voices)
        {
            voice.active = false;
            voice.age = 0.0;
        }
    }

    /** Lock-free one-shot trigger (pad 0..15). */
    void triggerPad (int padIndex, float velocity)
    {
        const int pad = juce::jlimit (0, 15, padIndex);
        triggerVelocities[static_cast<size_t> (pad)].store (
            juce::jlimit (0.0f, 1.0f, velocity), std::memory_order_relaxed);
        triggerMask.fetch_or (1u << pad, std::memory_order_release);
    }

    void setBiome (const juce::String& biome) { biomeValue = biome.trim().toLowerCase(); }

    juce::String getActiveBiome() const { return biomeNames()[activeBiome]; }
    float getMoodIntensity() const { return juce::jlimit (0.0f, 1.0f, static_cast<float> (moodValue.get())); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (! initialised || fc.destBuffer == nullptr
            || fc.destBuffer->getNumChannels() < 2 || fc.bufferNumSamples <= 0)
            return;

        auto* buffer = fc.destBuffer;
        const int n = juce::jmin (fc.bufferNumSamples,
                                  buffer->getNumSamples() - fc.bufferStartSample);
        if (n <= 0 || fc.bufferStartSample < 0)
            return;

        if (fc.bufferForMidiMessages != nullptr)
            for (const auto& msg : *fc.bufferForMidiMessages)
                if (msg.isNoteOn())
                {
                    const int note = msg.getNoteNumber();
                    if (note >= 36 && note <= 51)
                        triggerPad (note - 36, juce::jlimit (0.0f, 1.0f, msg.getFloatVelocity()));
                }

        const int requestedBiome = parseBiome (biomeValue.get());
        if (requestedBiome != targetBiome)
        {
            previousBiome = activeBiome;
            targetBiome = requestedBiome;
            fadePos = 0.0;
        }

        const float ambientGain = juce::jlimit (0.0f, 1.0f, static_cast<float> (ambientValue.get()));
        const float sfxGain = juce::jlimit (0.0f, 1.0f, static_cast<float> (sfxValue.get()));
        const float mood = getMoodIntensity();
        const float crossfadeSec = juce::jlimit (0.5f, 5.0f, static_cast<float> (crossfadeValue.get()));
        const double fadeStep = 1.0 / (static_cast<double> (crossfadeSec) * sampleRate);
        const double dt = 1.0 / sampleRate;
        const float moodDrive = 0.35f + 0.85f * mood;
        const float ceiling = juce::Decibels::decibelsToGain (
            juce::jlimit (-24.0f, 0.0f, static_cast<float> (ceilingValue.get())));

        std::uint32_t pending = triggerMask.exchange (0, std::memory_order_acq_rel);
        while (pending != 0)
        {
            const int pad = trailingZeroBits (pending);
            pending &= pending - 1u;
            int freeVoice = -1;
            for (int v = 0; v < numVoices; ++v)
                if (! voices[static_cast<size_t> (v)].active)
                {
                    freeVoice = v;
                    break;
                }
            if (freeVoice < 0)
                continue;
            auto& voice = voices[static_cast<size_t> (freeVoice)];
            voice.active = true;
            voice.type = pad % numSfx;
            voice.age = 0.0;
            voice.life = voice.type == 3 ? 1.4 : (voice.type == 7 ? 1.0 : 0.75);
            voice.velocity = triggerVelocities[static_cast<size_t> (pad)].load (std::memory_order_relaxed);
            voice.pitch = pitchVariation();
            voice.phase = 0.0;
        }

        for (int i = 0; i < n; ++i)
        {
            fadePos = juce::jmin (1.0, fadePos + fadeStep);
            const float blend = static_cast<float> (fadePos * fadePos * (3.0 - 2.0 * fadePos));
            const float from = ambientSample (previousBiome, moodDrive);
            const float to = ambientSample (targetBiome, moodDrive);
            float sample = (from * (1.0f - blend) + to * blend) * ambientGain * 0.45f;

            for (auto& voice : voices)
            {
                if (! voice.active)
                    continue;
                const double progress = voice.age / voice.life;
                if (progress >= 1.0)
                {
                    voice.active = false;
                    continue;
                }
                sample += sfxSample (voice, dt) * sfxGain * voice.velocity * 0.55f;
                voice.age += dt;
            }

            phase += juce::MathConstants<double>::twoPi * 110.0 / sampleRate;
            sample += static_cast<float> (std::sin (phase)) * 0.015f * mood;
            const float limited = juce::jlimit (-ceiling, ceiling,
                ceiling * std::tanh (sample / std::max (0.05f, ceiling)));
            const int pos = fc.bufferStartSample + i;
            buffer->setSample (0, pos, limited);
            buffer->setSample (1, pos, limited * 0.94f);
        }
        activeBiome = targetBiome;
    }

private:
    static constexpr int numVoices = 8;

    struct Voice
    {
        bool active = false;
        int type = 0;
        double age = 0.0;
        double life = 0.75;
        double phase = 0.0;
        float velocity = 0.0f;
        float pitch = 1.0f;
    };

    static int trailingZeroBits (std::uint32_t value) noexcept
    {
        int count = 0;
        while ((value & 1u) == 0u) { value >>= 1u; ++count; }
        return count;
    }

    std::uint32_t nextRandom() noexcept
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }

    float noise() noexcept
    {
        return static_cast<float> (static_cast<double> (nextRandom() % 2000001u) / 1000000.0 - 1.0);
    }

    float pitchVariation() noexcept
    {
        const double semitones = 1.5 * (static_cast<double> (nextRandom() % 2001u) / 1000.0 - 1.0);
        return static_cast<float> (std::pow (2.0, semitones / 12.0));
    }

    float ambientSample (int biome, float moodDrive) noexcept
    {
        phase += 0.0007;
        const double t = phase;
        const float n = noise();
        switch (biome)
        {
            case 0: return static_cast<float> (std::sin (t * 2.1) * 0.35) + n * (0.08f + 0.06f * moodDrive); // drowned_stars
            case 1: return n * 0.28f + static_cast<float> (std::sin (t * 5.3) * 0.04);                        // rainy_tavern
            case 2: return static_cast<float> (std::sin (t * 1.7) * 0.22) + n * (0.10f + 0.10f * moodDrive);  // haunted_crypt
            case 3: return n * static_cast<float> (0.24f + 0.12 * std::sin (t * 0.7)) + static_cast<float> (std::sin (t * 1.1) * 0.05);
            case 4: return n * 0.16f + static_cast<float> (std::sin (t * 7.1) * 0.06 + std::sin (t * 11.3) * 0.03);
            case 5: return static_cast<float> (std::sin (t * (8.0 + 4.0 * moodDrive)) * (0.18 + 0.30 * moodDrive)) + n * 0.05f;
            default: return n * 0.1f;
        }
    }

    float sfxSample (Voice& voice, double dt) noexcept
    {
        voice.phase += dt * (180.0 + 620.0 * static_cast<double> (voice.pitch));
        const double progress = voice.age / voice.life;
        const float envelope = static_cast<float> (std::exp (-3.0 * progress));
        const float n = noise();
        float v = 0.0f;
        switch (voice.type)
        {
            case 0: v = static_cast<float> (std::sin (voice.phase * 3.0)) * 0.9f + n * 0.4f; break;
            case 1: v = static_cast<float> (std::sin (voice.phase * 0.7)) * 0.6f + n * 0.5f; break;
            case 2: v = static_cast<float> (std::sin (voice.phase * 0.35)) * 0.5f + n * 0.35f; break;
            case 3: v = static_cast<float> (std::sin (voice.phase * 0.5)) * 0.7f + n * 0.4f; break;
            case 4: v = n * 0.7f + static_cast<float> (std::sin (voice.phase * 0.2)) * 0.3f; break;
            case 5: v = static_cast<float> (std::sin (voice.phase * 4.0)) * 0.8f + n * 0.5f; break;
            case 6: v = static_cast<float> (std::sin (voice.phase * 2.5)) * 0.8f; break;
            case 7: v = n * 0.9f + static_cast<float> (std::sin (voice.phase * 0.3)) * 0.4f; break;
            case 8: v = n * 0.5f + static_cast<float> (std::sin (voice.phase * 2.0)) * 0.3f; break;
            default: v = n * 0.6f + static_cast<float> (std::sin (voice.phase * 5.0)) * 0.2f; break;
        }
        return v * envelope;
    }

    juce::CachedValue<float> ambientValue, sfxValue, moodValue, crossfadeValue, ceilingValue;
    juce::CachedValue<juce::String> biomeValue;
    te::AutomatableParameter::Ptr ambientParam, sfxParam, moodParam, crossfadeParam, ceilingParam;

    std::array<std::atomic<float>, 16> triggerVelocities {};
    std::atomic<std::uint32_t> triggerMask { 0 };
    Voice voices[numVoices];
    double sampleRate = 44100.0;
    double phase = 0.0;
    double fadePos = 1.0;
    std::uint32_t rng = 0x9e3779b9u;
    int previousBiome = 0, targetBiome = 0, activeBiome = 0;
    bool initialised = false;
};
