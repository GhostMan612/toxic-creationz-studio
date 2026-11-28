#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <tracktion_engine/tracktion_engine.h>

#include "VocalChannelPlugin.h"
#include "RpgSoundboardPlugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace te = tracktion::engine;

//==============================================================================
// Reason-style native hardware rack machines.
//
// All three plugins are allocation-free inside applyToBuffer(): every DSP
// state block is a fixed-size member, and the only buffers that ever grow
// (the sampler's sample buffer, the tape's ring delays) are allocated on
// construction or in initialise(), never on the audio thread.
//
// Rack pin convention: pin 0 is MIDI, audio pin N maps to channel N-1.
// Where an input pin and an output pin share a channel, the input is read
// into a local scalar *before* the output is written for that sample.
//==============================================================================

//------------------------------------------------------------------------------
// TR-808 Drum Synth: 6-voice analogue emulation with 5 discrete outputs and a
// mono "CV Accent In" on pin 1 (channel 0).
//------------------------------------------------------------------------------
class Tr808DrumPlugin : public te::Plugin
{
public:
    explicit Tr808DrumPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        kickDecayValue.referTo  (state, juce::Identifier ("kickDecay"),  getUndoManager(), 0.42f);
        kickToneValue.referTo   (state, juce::Identifier ("kickTone"),   getUndoManager(), 52.0f);
        snareSnappyValue.referTo(state, juce::Identifier ("snareSnappy"),getUndoManager(), 0.65f);
        hatDecayValue.referTo   (state, juce::Identifier ("hatDecay"),   getUndoManager(), 0.14f);
        accentValue.referTo     (state, juce::Identifier ("accent"),     getUndoManager(), 0.60f);
        kickLevelValue.referTo  (state, juce::Identifier ("kickLevel"),  getUndoManager(), 1.0f);
        snareLevelValue.referTo (state, juce::Identifier ("snareLevel"), getUndoManager(), 1.0f);
        lowTomLevelValue.referTo(state, juce::Identifier ("lowTomLevel"),getUndoManager(), 1.0f);
        midTomLevelValue.referTo(state, juce::Identifier ("midTomLevel"),getUndoManager(), 1.0f);
        hatLevelValue.referTo   (state, juce::Identifier ("hatLevel"),   getUndoManager(), 1.0f);

        kickDecayParam  = addParam ("kickDecay",  "Kick Decay (s)",  { 0.08f, 1.20f });
        kickToneParam   = addParam ("kickTone",   "Kick Tone (Hz)",  { 38.0f, 90.0f });
        snareSnappyParam= addParam ("snareSnappy","Snare Snappy",    { 0.00f, 1.00f });
        hatDecayParam   = addParam ("hatDecay",   "Hat Decay (s)",   { 0.03f, 0.60f });
        accentParam     = addParam ("accent",     "Accent Depth",    { 0.00f, 1.00f });
        kickLevelParam  = addParam ("kickLevel",  "Kick Level",      { 0.00f, 1.50f });
        snareLevelParam = addParam ("snareLevel", "Snare Level",     { 0.00f, 1.50f });
        lowTomLevelParam= addParam ("lowTomLevel","Low Tom Level",   { 0.00f, 1.50f });
        midTomLevelParam= addParam ("midTomLevel","Mid Tom Level",   { 0.00f, 1.50f });
        hatLevelParam   = addParam ("hatLevel",   "Hat Level",       { 0.00f, 1.50f });

        kickDecayParam->attachToCurrentValue  (kickDecayValue);
        kickToneParam->attachToCurrentValue   (kickToneValue);
        snareSnappyParam->attachToCurrentValue(snareSnappyValue);
        hatDecayParam->attachToCurrentValue   (hatDecayValue);
        accentParam->attachToCurrentValue     (accentValue);
        kickLevelParam->attachToCurrentValue  (kickLevelValue);
        snareLevelParam->attachToCurrentValue (snareLevelValue);
        lowTomLevelParam->attachToCurrentValue(lowTomLevelValue);
        midTomLevelParam->attachToCurrentValue(midTomLevelValue);
        hatLevelParam->attachToCurrentValue   (hatLevelValue);
    }

    ~Tr808DrumPlugin() override
    {
        if (kickDecayParam != nullptr)   kickDecayParam->detachFromCurrentValue();
        if (kickToneParam != nullptr)    kickToneParam->detachFromCurrentValue();
        if (snareSnappyParam != nullptr) snareSnappyParam->detachFromCurrentValue();
        if (hatDecayParam != nullptr)    hatDecayParam->detachFromCurrentValue();
        if (accentParam != nullptr)      accentParam->detachFromCurrentValue();
        if (kickLevelParam != nullptr)  kickLevelParam->detachFromCurrentValue();
        if (snareLevelParam != nullptr) snareLevelParam->detachFromCurrentValue();
        if (lowTomLevelParam != nullptr)lowTomLevelParam->detachFromCurrentValue();
        if (midTomLevelParam != nullptr)midTomLevelParam->detachFromCurrentValue();
        if (hatLevelParam != nullptr)   hatLevelParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }
    bool isSynth() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins != nullptr)
            ins->add ("CV Accent In");

        if (outs != nullptr)
        {
            outs->add ("Master L");
            outs->add ("Master R");
            outs->add ("Kick Out");
            outs->add ("Snare Out");
            outs->add ("Hat/Tom Out");
        }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::mono(),
                                                   te::ChannelConfiguration::discreteChannels (5));
    }

    int getNumOutputChannelsGivenInputs (int) override { return 5; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = std::max (8000.0, info.sampleRate);
        rngSeed = 0x9e3779b9u;
        for (auto& v : voices)
            v = DrumVoice {};
    }

    void deinitialise() override {}

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        auto* buf = fc.destBuffer;

        if (buf == nullptr || buf->getNumChannels() < 2 || fc.bufferNumSamples <= 0
            || fc.bufferStartSample < 0
            || fc.bufferStartSample + fc.bufferNumSamples > buf->getNumSamples())
            return;

        const int numCh = buf->getNumChannels();
        const int n = fc.bufferNumSamples;
        const int base = fc.bufferStartSample;
        const float sampleInc = static_cast<float> (1.0 / lastSampleRate);
        const float tone  = juce::jlimit (38.0f, 90.0f, kickToneParam->getCurrentValue());
        const float kDec  = juce::jlimit (0.08f, 1.20f, kickDecayParam->getCurrentValue());
        const float snappy= juce::jlimit (0.0f, 1.0f, snareSnappyParam->getCurrentValue());
        const float hDec  = juce::jlimit (0.03f, 0.60f, hatDecayParam->getCurrentValue());
        const float accentDepth = juce::jlimit (0.0f, 1.0f, accentParam->getCurrentValue());
        const float kickLevel = juce::jlimit (0.0f, 1.5f, static_cast<float> (kickLevelParam->getCurrentValue()));
        const float snareLevel = juce::jlimit (0.0f, 1.5f, static_cast<float> (snareLevelParam->getCurrentValue()));
        const float lowTomLevel = juce::jlimit (0.0f, 1.5f, static_cast<float> (lowTomLevelParam->getCurrentValue()));
        const float midTomLevel = juce::jlimit (0.0f, 1.5f, static_cast<float> (midTomLevelParam->getCurrentValue()));
        const float hatLevel = juce::jlimit (0.0f, 1.5f, static_cast<float> (hatLevelParam->getCurrentValue()));

        if (fc.bufferForMidiMessages != nullptr)
        {
            for (const auto& msg : *fc.bufferForMidiMessages)
            {
                if (! msg.isNoteOn())
                    continue;

                const int note = msg.getNoteNumber();
                const float vel = juce::jlimit (0.0f, 1.0f, msg.getFloatVelocity());

                if (note == 36)        trigger (voiceKick,   vel);
                else if (note == 38)   trigger (voiceSnare,  vel);
                else if (note == 42)
                {
                    voices[voiceOHat].active = false; // Authentic 808 CHat chokes OHat
                    trigger (voiceCHat, vel);
                }
                else if (note == 46)   trigger (voiceOHat,   vel);
                else if (note == 45)   trigger (voiceLowTom, vel);
                else if (note == 48)   trigger (voiceMidTom, vel);
            }
        }

        const int auditionNote = pendingAuditionNote.exchange (-1);
        if (auditionNote == 36)        trigger (voiceKick, 0.9f);
        else if (auditionNote == 38)   trigger (voiceSnare, 0.9f);
        else if (auditionNote == 42)
        {
            voices[voiceOHat].active = false;
            trigger (voiceCHat, 0.9f);
        }
        else if (auditionNote == 46)   trigger (voiceOHat, 0.9f);
        else if (auditionNote == 45)   trigger (voiceLowTom, 0.9f);
        else if (auditionNote == 48)   trigger (voiceMidTom, 0.9f);

        for (int i = 0; i < n; ++i)
        {
            // Pin 1 / channel 0 is both "CV Accent In" and "Master L": read it
            // into a scalar before any output channel (including 0) is written.
            const float cv = juce::jlimit (-1.0f, 1.0f, buf->getSample (0, base + i));
            const float accent = juce::jlimit (0.25f, 2.0f,
                                               1.0f + cv * accentDepth * 0.8f);

            float kick = 0.0f, snare = 0.0f, hat = 0.0f, lowTom = 0.0f, midTom = 0.0f;

            // Bridged-T damped sine sweep: kick + two toms.
            renderDampedSine (voices[voiceKick],   kick,
                              tone * 3.2f, tone, kDec, sampleInc);
            renderDampedSine (voices[voiceLowTom], lowTom,
                              tone * 2.4f, tone * 1.30f, kDec * 0.70f, sampleInc);
            renderDampedSine (voices[voiceMidTom], midTom,
                              tone * 3.0f, tone * 1.65f, kDec * 0.55f, sampleInc);
            const float tom = lowTom * lowTomLevel + midTom * midTomLevel;

            // Snare: dual-tone body + 1-pole HP white noise.
            renderSnare (voices[voiceSnare], snare, snappy, sampleInc);

            // Hats: 6 inharmonic squares + HP + tanh diode clipping.
            renderHat (voices[voiceCHat], hat, hDec, sampleInc);
            renderHat (voices[voiceOHat], hat, hDec * 3.0f, sampleInc);

            const float master = (kick * kickLevel * 0.95f + snare * snareLevel * 0.62f
                                  + hat * hatLevel * 0.40f + tom * 0.55f) * accent;

            buf->setSample (0, base + i, master);
            buf->setSample (1, base + i, master);
            if (numCh > 2) buf->setSample (2, base + i, kick  * kickLevel * accent);
            if (numCh > 3) buf->setSample (3, base + i, snare * snareLevel * accent);
            if (numCh > 4) buf->setSample (4, base + i, (hat * hatLevel * 0.6f + tom) * accent);
        }
    }

    bool isOHatActiveForSmokeTest() const noexcept { return voices[voiceOHat].active; }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, kickDecayValue, kickToneValue,
                                          snareSnappyValue, hatDecayValue, accentValue,
                                          kickLevelValue, snareLevelValue, lowTomLevelValue,
                                          midTomLevelValue, hatLevelValue);

        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    static inline const char* xmlTypeName = "tcs_tr808_drum";
    juce::String getName() const override { return "TR-808 Drum Synth"; }
    juce::String getPluginType() override { return xmlTypeName; }

    /** Monitor-only audition trigger used by the bespoke faceplate pads. */
    void triggerAudition (int noteNumber) noexcept
    {
        if (noteNumber == 36 || noteNumber == 38 || noteNumber == 42
            || noteNumber == 45 || noteNumber == 46 || noteNumber == 48)
            pendingAuditionNote.store (noteNumber);
    }

private:
    std::atomic<int> pendingAuditionNote { -1 };
    static constexpr int voiceKick = 0, voiceSnare = 1, voiceCHat = 2;
    static constexpr int voiceOHat = 3, voiceLowTom = 4, voiceMidTom = 5;
    static constexpr int numVoices = 6;

    struct DrumVoice
    {
        double phase   = 0.0;
        double age     = 0.0;
        double env     = 0.0;
        double noiseLp = 0.0;
        double oscPhase[6] {};
        std::uint32_t noiseState = 0x9e3779b9u;
        bool active = false;
    };

    void trigger (int index, float velocity)
    {
        auto& v = voices[static_cast<size_t> (index)];
        v = DrumVoice {};
        rngSeed = rngSeed * 1664525u + 1013904223u;
        v.noiseState = (rngSeed != 0u) ? rngSeed : 0x9e3779b9u;
        v.env = juce::jlimit (0.0f, 1.0f, velocity);
        v.active = v.env > 0.0f;
    }

    static float nextNoise (DrumVoice& v) noexcept
    {
        v.noiseState ^= v.noiseState << 13;
        v.noiseState ^= v.noiseState >> 17;
        v.noiseState ^= v.noiseState << 5;
        return static_cast<float> (static_cast<int32_t> (v.noiseState)) / 2147483648.0f;
    }

    static float highPass (DrumVoice& v, float x, float coeff) noexcept
    {
        v.noiseLp += static_cast<double> (coeff) * (static_cast<double> (x) - v.noiseLp);
        return x - static_cast<float> (v.noiseLp);
    }

    void renderDampedSine (DrumVoice& v, float& out, float startHz, float endHz,
                          float decaySec, float sampleInc) noexcept
    {
        if (! v.active)
            return;

        const float ratio = std::exp (-static_cast<float> (v.age) * 40.0f);
        const float hz = endHz + (startHz - endHz) * ratio;
        v.phase += static_cast<double> (hz * sampleInc);
        if (v.phase >= juce::MathConstants<double>::twoPi)
            v.phase -= juce::MathConstants<double>::twoPi;

        const float env = static_cast<float> (std::exp (-3.0 * v.age / std::max (0.01, static_cast<double> (decaySec))));
        out = static_cast<float> (std::sin (v.phase)) * env * static_cast<float> (v.env);
        advance (v, decaySec, sampleInc);
    }

    void renderSnare (DrumVoice& v, float& out, float snappy, float sampleInc) noexcept
    {
        if (! v.active)
            return;

        v.phase += juce::MathConstants<double>::twoPi * (180.0 * sampleInc);
        if (v.phase >= juce::MathConstants<double>::twoPi)
            v.phase -= juce::MathConstants<double>::twoPi;

        const float body = static_cast<float> (std::sin (v.phase)) * 0.65f
                         + static_cast<float> (std::sin (v.phase * 1.8333)) * 0.35f;
        const float noise = highPass (v, nextNoise (v), 0.04f);
        const float env = static_cast<float> (std::exp (-14.0 * v.age));
        out = (body * (1.0f - 0.35f * snappy) + noise * snappy * 0.85f) * env * static_cast<float> (v.env);
        advance (v, 0.42f, sampleInc);
    }

    void renderHat (DrumVoice& v, float& out, float decaySec, float sampleInc) noexcept
    {
        if (! v.active)
            return;

        static constexpr float oscHz[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };

        float sum = 0.0f;

        for (int k = 0; k < 6; ++k)
        {
            v.oscPhase[k] += juce::MathConstants<double>::twoPi * (static_cast<double> (oscHz[k]) * sampleInc);
            if (v.oscPhase[k] >= juce::MathConstants<double>::twoPi)
                v.oscPhase[k] -= juce::MathConstants<double>::twoPi;
            sum += v.oscPhase[k] < juce::MathConstants<double>::pi ? 1.0f : -1.0f;
        }

        const float hp = highPass (v, sum * (1.0f / 6.0f), 0.28f);
        const float env = static_cast<float> (std::exp (-3.0 * v.age / std::max (0.01, static_cast<double> (decaySec))));
        out = std::tanh (2.6f * hp) * 0.72f * env * static_cast<float> (v.env);
        advance (v, decaySec, sampleInc);
    }

    static void advance (DrumVoice& v, float decaySec, float sampleInc) noexcept
    {
        v.age += sampleInc;
        const double lifetime = std::max (0.02, static_cast<double> (decaySec) * 3.0);
        if (v.age > lifetime || v.env < 0.0001)
            v.active = false;
    }

    juce::CachedValue<float> kickDecayValue, kickToneValue, snareSnappyValue, hatDecayValue, accentValue;
    juce::CachedValue<float> kickLevelValue, snareLevelValue, lowTomLevelValue, midTomLevelValue, hatLevelValue;
    te::AutomatableParameter::Ptr kickDecayParam, kickToneParam, snareSnappyParam, hatDecayParam, accentParam;
    te::AutomatableParameter::Ptr kickLevelParam, snareLevelParam, lowTomLevelParam, midTomLevelParam, hatLevelParam;
    std::array<DrumVoice, numVoices> voices {};
    std::uint32_t rngSeed = 0x9e3779b9u;
    double lastSampleRate = 44100.0;
};

//------------------------------------------------------------------------------
// 12-Bit Vintage Sampler: chromatic playback, file loading, linear DAC
// quantization, zero-order-hold decimation and a 1-pole reconstruction LPF.
//------------------------------------------------------------------------------
class VintageSamplerPlugin : public te::Plugin
{
public:
    explicit VintageSamplerPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        bitDepthValue.referTo    (state, juce::Identifier ("bitDepth"),    getUndoManager(), 12.0f);
        targetRateValue.referTo  (state, juce::Identifier ("targetRate"),  getUndoManager(), 26040.0f);
        cutoffValue.referTo      (state, juce::Identifier ("filterCutoff"),getUndoManager(), 14000.0f);
        cvPitchDepthValue.referTo(state, juce::Identifier ("cvPitchDepth"),getUndoManager(), 12.0f);
        gainValue.referTo        (state, juce::Identifier ("gain"),        getUndoManager(), 0.85f);

        bitDepthParam     = addParam ("bitDepth",     "DAC Bits",        { 4.0f, 16.0f });
        targetRateParam   = addParam ("targetRate",   "Sample Rate (Hz)",{ 8000.0f, 44100.0f });
        cutoffParam       = addParam ("filterCutoff","Recon. LPF (Hz)",  { 800.0f, 18000.0f });
        cvPitchDepthParam = addParam ("cvPitchDepth","CV Pitch Depth",   { 0.0f, 12.0f });
        gainParam         = addParam ("gain",         "Gain",            { 0.0f, 1.5f });

        bitDepthParam->attachToCurrentValue     (bitDepthValue);
        targetRateParam->attachToCurrentValue   (targetRateValue);
        cutoffParam->attachToCurrentValue       (cutoffValue);
        cvPitchDepthParam->attachToCurrentValue (cvPitchDepthValue);
        gainParam->attachToCurrentValue         (gainValue);

        buildStarterStab();
    }

    ~VintageSamplerPlugin() override
    {
        if (bitDepthParam != nullptr)     bitDepthParam->detachFromCurrentValue();
        if (targetRateParam != nullptr)   targetRateParam->detachFromCurrentValue();
        if (cutoffParam != nullptr)       cutoffParam->detachFromCurrentValue();
        if (cvPitchDepthParam != nullptr) cvPitchDepthParam->detachFromCurrentValue();
        if (gainParam != nullptr)         gainParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }
    bool isSynth() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins != nullptr)
            ins->add ("CV Pitch In");

        if (outs != nullptr)
        {
            outs->add ("Audio Out L");
            outs->add ("Audio Out R");
        }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::mono(),
                                                   te::ChannelConfiguration::stereo());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 2; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = std::max (8000.0, info.sampleRate);
        reconstructionLpL = 0.0f;
        reconstructionLpR = 0.0f;
        for (auto& v : voices)
            v = Voice {};
    }

    void deinitialise() override {}

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        auto* buf = fc.destBuffer;

        if (buf == nullptr || buf->getNumChannels() < 2 || fc.bufferNumSamples <= 0
            || fc.bufferStartSample < 0
            || fc.bufferStartSample + fc.bufferNumSamples > buf->getNumSamples())
            return;

        const int n = fc.bufferNumSamples;
        const int base = fc.bufferStartSample;
        const int numFrames = sampleBuffer.getNumSamples();

        if (numFrames <= 0)
        {
            buf->clear (0, base, n);
            buf->clear (1, base, n);
            return;
        }

        const float bits      = juce::jlimit (4.0f, 16.0f, bitDepthParam->getCurrentValue());
        const float targetHz  = juce::jlimit (8000.0f, 44100.0f, targetRateParam->getCurrentValue());
        const float cutoffHz  = juce::jlimit (800.0f, 18000.0f, cutoffParam->getCurrentValue());
        const float cvDepth   = juce::jlimit (0.0f, 12.0f, cvPitchDepthParam->getCurrentValue());
        const float gain      = juce::jlimit (0.0f, 1.5f, gainParam->getCurrentValue());
        const bool  filterOn  = cutoffHz < 17900.0f;
        const float decimStep = juce::jlimit (0.01f, 8.0f,
                                              targetHz / static_cast<float> (lastSampleRate));
        const float quantSteps = std::pow (2.0f, bits - 1.0f) - 1.0f;
        const float lpCoeff = filterOn
                                ? juce::jlimit (0.0f, 1.0f,
                                    1.0f - std::exp (static_cast<float> (-2.0 * juce::MathConstants<double>::pi)
                                                     * cutoffHz / static_cast<float> (lastSampleRate)))
                                : 0.0f;
        const double rateScale = sourceSampleRate / lastSampleRate;
        const bool stereo = sampleBuffer.getNumChannels() > 1;

        if (fc.bufferForMidiMessages != nullptr)
        {
            for (const auto& msg : *fc.bufferForMidiMessages)
            {
                if (! msg.isNoteOn())
                    continue;

                const int note = msg.getNoteNumber();

                if (note < 24 || note > 96)
                    continue;

                Voice* targetVoice = nullptr;
                double maxPos = -1.0;

                for (auto& v : voices)
                {
                    if (! v.active)
                    {
                        targetVoice = &v;
                        break;
                    }

                    if (v.pos > maxPos)
                    {
                        maxPos = v.pos;
                        targetVoice = &v;
                    }
                }

                if (targetVoice != nullptr)
                {
                    *targetVoice = Voice {};
                    targetVoice->active = true;
                    targetVoice->note = note;
                    targetVoice->decimPhase = 1.0;
                }
            }
        }

        for (int i = 0; i < n; ++i)
        {
            // Pin 1 / channel 0 is "CV Pitch In" and also "Audio Out L".
            const float cv = juce::jlimit (-1.0f, 1.0f, buf->getSample (0, base + i));

            float mixedL = 0.0f, mixedR = 0.0f;

            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                const double semitones = static_cast<double> (v.note - 60)
                                       + static_cast<double> (cv * cvDepth);
                const double step = std::pow (2.0, semitones / 12.0) * rateScale;
                v.pos += step;

                if (v.pos >= static_cast<double> (numFrames))
                {
                    v.active = false;
                    continue;
                }

                v.decimPhase += decimStep;

                if (v.decimPhase >= 1.0)
                {
                    v.decimPhase -= 1.0;
                    v.holdFrame = static_cast<int> (v.pos);
                }

                const int frame = juce::jlimit (0, numFrames - 1, v.holdFrame);
                const float l = sampleBuffer.getSample (0, frame);
                const float r = stereo ? sampleBuffer.getSample (1, frame) : l;
                mixedL += l;
                mixedR += r;
            }

            if (filterOn)
            {
                reconstructionLpL += lpCoeff * (mixedL - reconstructionLpL);
                reconstructionLpR += lpCoeff * (mixedR - reconstructionLpR);
                mixedL = reconstructionLpL;
                mixedR = reconstructionLpR;
            }

            const float ql = std::round (juce::jlimit (-1.0f, 1.0f, mixedL * gain) * quantSteps) / quantSteps;
            const float qr = std::round (juce::jlimit (-1.0f, 1.0f, mixedR * gain) * quantSteps) / quantSteps;
            buf->setSample (0, base + i, juce::jlimit (-1.0f, 1.0f, ql));
            buf->setSample (1, base + i, juce::jlimit (-1.0f, 1.0f, qr));
        }
    }

    bool loadSampleFromFile (const juce::File& file)
    {
        return loadIntoBuffer (file, true);
    }

    void resetToStarterStab()
    {
        state.removeProperty ("samplePath", getUndoManager());
        buildStarterStab();
        edit.pluginChanged (*this);
    }

    juce::String getLoadedSampleName() const
    {
        const juce::String path = state.getProperty ("samplePath", juce::String());
        if (path.isNotEmpty())
            return juce::File (path).getFileName();
        return "Starter Chord Stab (0.65s)";
    }

    void setTargetRate (float hz) noexcept   { targetRateValue = juce::jlimit (8000.0f, 44100.0f, hz); targetRateParam->updateFromAttachedValue(); }
    void setFilterCutoff (float hz) noexcept { cutoffValue = juce::jlimit (800.0f, 18000.0f, hz); cutoffParam->updateFromAttachedValue(); }
    void setBitDepth (float bits) noexcept   { bitDepthValue = juce::jlimit (4.0f, 16.0f, bits); bitDepthParam->updateFromAttachedValue(); }
    void setCvPitchDepth (float depth) noexcept { cvPitchDepthValue = juce::jlimit (0.0f, 12.0f, depth); cvPitchDepthParam->updateFromAttachedValue(); }
    void setGain (float g) noexcept           { gainValue = juce::jlimit (0.0f, 1.5f, g); gainParam->updateFromAttachedValue(); }

    int getNumVoicesForSmokeTest() const noexcept { return static_cast<int> (voices.size()); }
    const juce::AudioBuffer<float>& getSampleBufferForPreview() const noexcept { return sampleBuffer; }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, bitDepthValue, targetRateValue,
                                          cutoffValue, cvPitchDepthValue, gainValue);

        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();

        const juce::String path = v.getProperty ("samplePath", juce::String());

        if (path.isNotEmpty())
            loadIntoBuffer (juce::File (path), false);
        else
            buildStarterStab();
    }

    static inline const char* xmlTypeName = "tcs_vintage_sampler";
    juce::String getName() const override { return "12-Bit Vintage Sampler"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    struct Voice
    {
        int note = 60;
        int holdFrame = 0;
        double pos = 0.0;
        double decimPhase = 0.0;
        bool active = false;
    };

    void buildStarterStab()
    {
        constexpr int frames = static_cast<int> (0.65 * 44100.0);
        sampleBuffer.setSize (2, frames);
        sampleBuffer.clear();

        static constexpr float chordHz[3] = { 261.63f, 329.63f, 392.00f };

        for (int i = 0; i < frames; ++i)
        {
            const double t = static_cast<double> (i) / 44100.0;
            const float env = static_cast<float> (std::exp (-t * 7.5) * (1.0 - std::exp (-t * 600.0)));
            float s = 0.0f;

            for (int c = 0; c < 3; ++c)
            {
                s += static_cast<float> (std::sin (juce::MathConstants<double>::twoPi
                                                  * static_cast<double> (chordHz[c]) * t));
                s += 0.18f * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi
                                                          * static_cast<double> (chordHz[c]) * 1.004 * t));
            }

            const float v = juce::jlimit (-1.0f, 1.0f, s * env * 0.11f);
            sampleBuffer.setSample (0, i, v * 0.97f);
            sampleBuffer.setSample (1, i, v);
        }

        sourceSampleRate = 44100.0;
        for (auto& v : voices)
            v = Voice {};
    }

    bool loadIntoBuffer (const juce::File& file, bool updateState)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            return false;

        const int numCh = std::min (2, static_cast<int> (reader->numChannels));
        const int frames = static_cast<int> (reader->lengthInSamples);

        if (numCh < 1 || frames < 16)
            return false;

        sampleBuffer.setSize (numCh, frames, false, true, true);

        if (! reader->read (&sampleBuffer, 0, frames, 0, true, numCh > 1))
            return false;

        sourceSampleRate = reader->sampleRate;

        for (auto& v : voices)
            v = Voice {};

        if (updateState)
            state.setProperty ("samplePath", juce::var (file.getFullPathName()), getUndoManager());

        return true;
    }

    juce::CachedValue<float> bitDepthValue, targetRateValue, cutoffValue, cvPitchDepthValue, gainValue;
    te::AutomatableParameter::Ptr bitDepthParam, targetRateParam, cutoffParam, cvPitchDepthParam, gainParam;
    juce::AudioBuffer<float> sampleBuffer;
    std::array<Voice, 8> voices {};
    double sourceSampleRate = 44100.0;
    double lastSampleRate = 44100.0;
    float reconstructionLpL = 0.0f;
    float reconstructionLpR = 0.0f;
};

//------------------------------------------------------------------------------
// Analog Tape Deck: fractional wow/flutter delay, 85 Hz head bump, asymmetric
// tanh saturation and wet/dry mix. All ring/IIR state is pre-allocated in
// initialise(); applyToBuffer performs no allocation.
//------------------------------------------------------------------------------
class AnalogTapePlugin : public te::Plugin
{
public:
    explicit AnalogTapePlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        driveValue.referTo   (state, juce::Identifier ("drive"),   getUndoManager(), 2.5f);
        wowValue.referTo     (state, juce::Identifier ("wow"),     getUndoManager(), 0.25f);
        flutterValue.referTo (state, juce::Identifier ("flutter"), getUndoManager(), 0.15f);
        biasValue.referTo    (state, juce::Identifier ("bias"),    getUndoManager(), 0.12f);
        mixValue.referTo     (state, juce::Identifier ("mix"),     getUndoManager(), 1.0f);

        driveParam   = addParam ("drive",   "Tape Drive",   { 1.0f, 8.0f });
        wowParam     = addParam ("wow",     "Wow",          { 0.0f, 1.0f });
        flutterParam = addParam ("flutter", "Flutter",      { 0.0f, 1.0f });
        biasParam    = addParam ("bias",    "Bias",         { 0.0f, 0.4f });
        mixParam     = addParam ("mix",     "Wet / Dry",    { 0.0f, 1.0f });

        driveParam->attachToCurrentValue  (driveValue);
        wowParam->attachToCurrentValue    (wowValue);
        flutterParam->attachToCurrentValue(flutterValue);
        biasParam->attachToCurrentValue   (biasValue);
        mixParam->attachToCurrentValue    (mixValue);
    }

    ~AnalogTapePlugin() override
    {
        if (driveParam != nullptr)   driveParam->detachFromCurrentValue();
        if (wowParam != nullptr)     wowParam->detachFromCurrentValue();
        if (flutterParam != nullptr) flutterParam->detachFromCurrentValue();
        if (biasParam != nullptr)    biasParam->detachFromCurrentValue();
        if (mixParam != nullptr)     mixParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins != nullptr)
        {
            ins->add ("Audio In L");
            ins->add ("Audio In R");
            ins->add ("CV Drive In");
        }

        if (outs != nullptr)
        {
            outs->add ("Audio Out L");
            outs->add ("Audio Out R");
        }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::discreteChannels (3),
                                                   te::ChannelConfiguration::stereo());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 2; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = std::max (8000.0, info.sampleRate);
        ringSize = static_cast<int> (std::ceil (0.05 * lastSampleRate)) + 4;
        ringL.assign (static_cast<size_t> (ringSize), 0.0f);
        ringR.assign (static_cast<size_t> (ringSize), 0.0f);
        writeIndex = 0;
        timeSamples = 0.0;
        headFilter.reset();

        // RBJ peak EQ: 85 Hz, +2.5 dB, Q 0.85.
        const double gainDb = 2.5;
        const double f0 = 85.0;
        const double q  = 0.85;
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * f0 / lastSampleRate;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double cw = std::cos (w0);
        const double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cw) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cw) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    void deinitialise() override
    {
        ringL.clear();
        ringR.clear();
        ringSize = 0;
        writeIndex = 0;
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        auto* buf = fc.destBuffer;

        if (buf == nullptr || buf->getNumChannels() < 2 || fc.bufferNumSamples <= 0
            || fc.bufferStartSample < 0
            || fc.bufferStartSample + fc.bufferNumSamples > buf->getNumSamples()
            || ringSize < 8
            || static_cast<int> (ringL.size()) < ringSize
            || static_cast<int> (ringR.size()) < ringSize)
            return;

        const bool hasCvCh = (buf->getNumChannels() >= 3);
        const int n = fc.bufferNumSamples;
        const int base = fc.bufferStartSample;
        const float sr = static_cast<float> (lastSampleRate);
        const float drive = juce::jlimit (1.0f, 8.0f, driveParam->getCurrentValue());
        const float wow = juce::jlimit (0.0f, 1.0f, wowParam->getCurrentValue());
        const float flutter = juce::jlimit (0.0f, 1.0f, flutterParam->getCurrentValue());
        const float bias = juce::jlimit (0.0f, 0.4f, biasParam->getCurrentValue());
        const float mix = juce::jlimit (0.0f, 1.0f, mixParam->getCurrentValue());
        const float wowPhaseInc = static_cast<float> (juce::MathConstants<double>::twoPi * 0.55 / lastSampleRate);
        const float flutterPhaseInc = static_cast<float> (juce::MathConstants<double>::twoPi * 6.8 / lastSampleRate);
        const float baseDelay = 0.005f * sr;
        const float wowDepth = wow * 0.0045f * sr;
        const float flutterDepth = flutter * 0.0012f * sr;
        float outPeak = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            // Pin 1/2/3 = channels 0/1/2 are also Audio Out L/R: read all
            // inputs into scalars before writing any output channel.
            const float inL = buf->getSample (0, base + i);
            const float inR = buf->getSample (1, base + i);
            const float cv  = hasCvCh ? juce::jlimit (-1.0f, 1.0f, buf->getSample (2, base + i)) : 0.0f;

            ringL[static_cast<size_t> (writeIndex)] = inL;
            ringR[static_cast<size_t> (writeIndex)] = inR;
            writeIndex++;
            if (writeIndex >= ringSize)
                writeIndex = 0;

            const float wowPhase = static_cast<float> (timeSamples) * wowPhaseInc;
            const float flutterPhase = static_cast<float> (timeSamples) * flutterPhaseInc;
            timeSamples += 1.0;

            float delay = baseDelay + std::sin (wowPhase) * wowDepth
                        + std::sin (flutterPhase) * flutterDepth;
            delay = juce::jlimit (1.0f, static_cast<float> (ringSize - 2), delay);

            const int i0 = readDelayIndex (delay, 0);
            const int i1 = readDelayIndex (delay, 1);
            const float frac = delay - std::floor (delay);

            const float delayedL = ringL[static_cast<size_t> (i0)] * (1.0f - frac)
                                 + ringL[static_cast<size_t> (i1)] * frac;
            const float delayedR = ringR[static_cast<size_t> (i0)] * (1.0f - frac)
                                 + ringR[static_cast<size_t> (i1)] * frac;

            const float bumpedL = static_cast<float> (headFilter.processSample (
                                     0, static_cast<double> (delayedL), b0, b1, b2, a1, a2));
            const float bumpedR = static_cast<float> (headFilter.processSample (
                                     1, static_cast<double> (delayedR), b0, b1, b2, a1, a2));

            const float effDrive = juce::jlimit (0.5f, 12.0f, drive * (1.0f + cv * 0.5f));
            const float biasOffset = std::tanh (effDrive * bias);

            // tanh() never reaches +/-1, so 0.5x keeps the output strictly < 1.
            const float wetL = 0.5f * (std::tanh (effDrive * (bumpedL + bias)) - biasOffset);
            const float wetR = 0.5f * (std::tanh (effDrive * (bumpedR + bias)) - biasOffset);

            const float outL = inL * (1.0f - mix) + wetL * mix;
            const float outR = inR * (1.0f - mix) + wetR * mix;
            outPeak = std::max (outPeak, std::max (std::abs (outL), std::abs (outR)));
            buf->setSample (0, base + i, outL);
            buf->setSample (1, base + i, outR);
        }

        currentLevelDb.store (juce::jlimit (-60.0f, 0.0f,
            juce::Decibels::gainToDecibels (std::max (1.0e-5f, outPeak))));
        if (hasCvCh)
            buf->clear (2, base, n);
    }

    void setDrive (float d) noexcept   { driveValue = juce::jlimit (1.0f, 8.0f, d); driveParam->updateFromAttachedValue(); }
    void setWow (float w) noexcept     { wowValue = juce::jlimit (0.0f, 1.0f, w); wowParam->updateFromAttachedValue(); }
    void setFlutter (float f) noexcept { flutterValue = juce::jlimit (0.0f, 1.0f, f); flutterParam->updateFromAttachedValue(); }
    void setMix (float m) noexcept     { mixValue = juce::jlimit (0.0f, 1.0f, m); mixParam->updateFromAttachedValue(); }

    /** Peak output level in dBFS, polled by the bespoke VU faceplate. */
    std::atomic<float> currentLevelDb { -60.0f };

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, driveValue, wowValue, flutterValue, biasValue, mixValue);

        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    static inline const char* xmlTypeName = "tcs_analog_tape";
    juce::String getName() const override { return "Analog Tape Deck"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    // Tiny 2-channel direct-form IIR used for the 85 Hz head bump. The
    // coefficients are set in initialise() and the state lives on the stack
    // of this member (no per-buffer allocation).
    struct StereoBiquad
    {
        void reset() { x1[0] = x2[0] = y1[0] = y2[0] = 0.0; x1[1] = x2[1] = y1[1] = y2[1] = 0.0; }

        double processSample (int ch, double x, double b0c, double b1c, double b2c,
                              double a1c, double a2c) noexcept
        {
            const auto c = static_cast<size_t> (ch & 1);
            const double y = b0c * x + b1c * x1[c] + b2c * x2[c] - a1c * y1[c] - a2c * y2[c];
            x2[c] = x1[c]; x1[c] = x;
            y2[c] = y1[c]; y1[c] = y;
            return y;
        }

        double x1[2] {}, x2[2] {}, y1[2] {}, y2[2] {};
    };

    int readDelayIndex (float delay, int ahead) const noexcept
    {
        int idx = writeIndex - static_cast<int> (std::floor (delay)) - ahead;
        idx %= ringSize;
        if (idx < 0)
            idx += ringSize;
        return idx;
    }

    juce::CachedValue<float> driveValue, wowValue, flutterValue, biasValue, mixValue;
    te::AutomatableParameter::Ptr driveParam, wowParam, flutterParam, biasParam, mixParam;
    std::vector<float> ringL, ringR;
    StereoBiquad headFilter;
    int ringSize = 0;
    int writeIndex = 0;
    double timeSamples = 0.0;
    double lastSampleRate = 44100.0;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

inline void registerHardwareRackPlugins (te::Engine& engine)
{
    auto& pm = engine.getPluginManager();
    pm.createBuiltInType<Tr808DrumPlugin>();
    pm.createBuiltInType<VintageSamplerPlugin>();
    pm.createBuiltInType<AnalogTapePlugin>();
    pm.createBuiltInType<VocalChannelPlugin>();
    pm.createBuiltInType<RpgSoundboardPlugin>();
}
