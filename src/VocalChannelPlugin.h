#pragma once

// Platform-neutral voice-acting channel strip for the Toxic Creationz Studio
// core boundary. No desktop UI / VST3 headers; every DSP state value is a
// fixed-size member and applyToBuffer() performs no heap allocation.
//
// Signal path:
//   1. 2-pole HPF (plosive / rumble guard)
//   2. hysteresis downward expander (noise gate / breath tamer)
//   3. split-band de-esser (bandpass sidechain detector, high-band cut)
//   4. soft-knee leveler + true-peak ceiling limiter

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace te = tracktion::engine;

class VocalChannelPlugin : public te::Plugin
{
public:
    explicit VocalChannelPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        hpfValue.referTo         (state, juce::Identifier ("hpfHz"),          nullptr, 85.0f);
        gateValue.referTo        (state, juce::Identifier ("gateThresholdDb"),nullptr, -48.0f);
        breathValue.referTo      (state, juce::Identifier ("breathFloorDb"),  nullptr, -12.0f);
        deEssFreqValue.referTo   (state, juce::Identifier ("deEssFreqHz"),    nullptr, 6800.0f);
        deEssAmountValue.referTo (state, juce::Identifier ("deEssAmount"),    nullptr, 0.35f);
        ceilingValue.referTo     (state, juce::Identifier ("ceilingDb"),      nullptr, -3.0f);
        outputGainValue.referTo  (state, juce::Identifier ("outputGainDb"),   nullptr, 0.0f);

        hpfParam         = addParam ("hpfHz",           TRANS("HPF (Hz)"),        { 20.0f, 180.0f });
        gateParam        = addParam ("gateThresholdDb", TRANS("Gate (dB)"),       { -70.0f, -20.0f });
        breathParam      = addParam ("breathFloorDb",   TRANS("Breath Floor"),    { -40.0f, 0.0f });
        deEssFreqParam   = addParam ("deEssFreqHz",     TRANS("De-Ess Freq (Hz)"),{ 4000.0f, 9500.0f });
        deEssAmountParam = addParam ("deEssAmount",     TRANS("De-Ess Amount"),   { 0.0f, 1.0f });
        ceilingParam     = addParam ("ceilingDb",       TRANS("Ceiling (dBFS)"),  { -6.0f, -0.1f });
        outputGainParam  = addParam ("outputGainDb",    TRANS("Output (dB)"),     { -12.0f, 18.0f });

        hpfParam->attachToCurrentValue (hpfValue);
        gateParam->attachToCurrentValue (gateValue);
        breathParam->attachToCurrentValue (breathValue);
        deEssFreqParam->attachToCurrentValue (deEssFreqValue);
        deEssAmountParam->attachToCurrentValue (deEssAmountValue);
        ceilingParam->attachToCurrentValue (ceilingValue);
        outputGainParam->attachToCurrentValue (outputGainValue);
    }

    ~VocalChannelPlugin() override
    {
        for (auto p : { hpfParam, gateParam, breathParam, deEssFreqParam,
                        deEssAmountParam, ceilingParam, outputGainParam })
            if (p != nullptr) p->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins != nullptr)  { ins->add ("Audio In L");  ins->add ("Audio In R"); }
        if (outs != nullptr) { outs->add ("Audio Out L"); outs->add ("Audio Out R"); }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::discreteChannels (2),
                                                   te::ChannelConfiguration::stereo());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 2; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = std::max (8000.0, info.sampleRate);
        const auto maxBlock = static_cast<juce::uint32> (std::max (16, info.blockSizeSamples));

        hpfFilter.reset();
        hpfFilter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
        hpfFilter.setResonance (0.70710678f);
        hpfFilter.setCutoffFrequency (hpfValue.get());
        hpfFilter.prepare ({ lastSampleRate, maxBlock, 2 });

        deEssDetector.reset();
        deEssDetector.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        deEssDetector.setResonance (0.70710678f);
        deEssDetector.setCutoffFrequency (deEssFreqValue.get());
        deEssDetector.prepare ({ lastSampleRate, maxBlock, 1 });

        gateEnvelope  = 0.0f;
        deEssEnvelope = 0.0f;
        gateSmooth    = 1.0f;
        lowpassState[0] = lowpassState[1] = 0.0f;
        initialised = true;
        currentLimiterGrDb.store (0.0f);
    }

    void deinitialise() override
    {
        initialised = false;
        hpfFilter.reset();
        deEssDetector.reset();
        gateEnvelope  = 0.0f;
        deEssEnvelope = 0.0f;
        gateSmooth    = 1.0f;
        lowpassState[0] = lowpassState[1] = 0.0f;
        currentLimiterGrDb.store (0.0f);
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (! initialised
            || fc.destBuffer == nullptr
            || fc.destBuffer->getNumChannels() < 2
            || fc.destBuffer->getNumSamples() <= 0
            || fc.bufferNumSamples <= 0
            || fc.bufferStartSample < 0
            || fc.bufferStartSample + fc.bufferNumSamples > fc.destBuffer->getNumSamples())
        {
            return;
        }

        auto* buf = fc.destBuffer;
        const int n = fc.bufferNumSamples;
        const int base = fc.bufferStartSample;

        const float hpfHz      = juce::jlimit (20.0f, 180.0f, static_cast<float> (hpfParam->getCurrentValue()));
        const float gateDb     = juce::jlimit (-70.0f, -20.0f, static_cast<float> (gateParam->getCurrentValue()));
        const float breathDb   = juce::jlimit (-40.0f, 0.0f, static_cast<float> (breathParam->getCurrentValue()));
        const float deEssHz    = juce::jlimit (4000.0f, 9500.0f, static_cast<float> (deEssFreqParam->getCurrentValue()));
        const float deEssAmt   = juce::jlimit (0.0f, 1.0f, static_cast<float> (deEssAmountParam->getCurrentValue()));
        const float ceilingDb  = juce::jlimit (-6.0f, -0.1f, static_cast<float> (ceilingParam->getCurrentValue()));
        const float outGainDb  = juce::jlimit (-12.0f, 18.0f, static_cast<float> (outputGainParam->getCurrentValue()));

        hpfFilter.setCutoffFrequency (hpfHz);
        deEssDetector.setCutoffFrequency (deEssHz);

        const float sr = static_cast<float> (lastSampleRate);
        const float gateCoeff = 1.0f - std::exp (-1.0f / (0.010f * sr));
        const float deEssCoeff = 1.0f - std::exp (-1.0f / (0.020f * sr));
        const float lpCoeff = juce::jlimit (0.05f, 0.8f, deEssHz / (sr * 3.0f));
        const float ceiling = std::pow (10.0f, ceilingDb / 20.0f);
        const float makeup = std::pow (10.0f, outGainDb / 20.0f);
        const float floorAtten = std::abs (breathDb);
        float maxGrDb = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const int pos = base + i;
            float l = buf->getSample (0, pos);
            float r = buf->getSample (1, pos);

            // 1. Plosive / rumble guard.
            l = hpfFilter.processSample (0, l);
            r = hpfFilter.processSample (1, r);

            // 2. Hysteresis downward expander with a soft breath floor.
            const float framePeak = std::max (std::abs (l), std::abs (r));
            gateEnvelope += gateCoeff * (framePeak - gateEnvelope);
            const float envDb = juce::Decibels::gainToDecibels (std::max (1.0e-5f, gateEnvelope));
            float targetGate = 1.0f;
            if (envDb < gateDb)
            {
                const float depth = juce::jlimit (0.0f, 1.0f, (gateDb - envDb) / 36.0f);
                targetGate = std::pow (10.0f, -depth * floorAtten / 20.0f);
            }
            gateSmooth += gateCoeff * (targetGate - gateSmooth);
            l *= gateSmooth;
            r *= gateSmooth;

            // 3. Split-band de-esser: bandpass detector drives the high band only.
            const float detectorIn = std::max (std::abs (l), std::abs (r));
            const float detectorOut = std::abs (deEssDetector.processSample (0, detectorIn));
            deEssEnvelope += deEssCoeff * (detectorOut - deEssEnvelope);
            const float detectorDb = juce::Decibels::gainToDecibels (std::max (1.0e-5f, deEssEnvelope));
            const float essDepth = juce::jlimit (0.0f, 1.0f, (detectorDb + 30.0f) / 30.0f);
            const float highBandGain = 1.0f - deEssAmt * essDepth;

            for (int ch = 0; ch < 2; ++ch)
            {
                const float x = ch == 0 ? l : r;
                lowpassState[ch] += lpCoeff * (x - lowpassState[ch]);
                const float highBand = x - lowpassState[ch];
                const float shaped = lowpassState[ch] + highBand * highBandGain;
                if (ch == 0) l = shaped; else r = shaped;
            }

            // 4. Soft-knee leveler, output gain, and brickwall ceiling.
            const float preLimitPeak = std::max (std::abs (l), std::abs (r)) * makeup;
            const float processedL = l * makeup;
            const float processedR = r * makeup;
            const float limitedL = ceiling * std::tanh (processedL / ceiling);
            const float limitedR = ceiling * std::tanh (processedR / ceiling);
            buf->setSample (0, pos, juce::jlimit (-ceiling, ceiling, limitedL));
            buf->setSample (1, pos, juce::jlimit (-ceiling, ceiling, limitedR));

            if (preLimitPeak > ceiling)
                maxGrDb = std::max (maxGrDb, juce::Decibels::gainToDecibels (preLimitPeak / ceiling));
        }

        currentLimiterGrDb.store (maxGrDb);
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, hpfValue, gateValue, breathValue, deEssFreqValue,
                                          deEssAmountValue, ceilingValue, outputGainValue);
        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    static inline const char* xmlTypeName = "tcs_vocal_channel";
    juce::String getName() const override { return "Vocal Booth Strip"; }
    juce::String getPluginType() override { return xmlTypeName; }

    /** Peak limiter gain reduction over the most recent render, in dB (0 = none). */
    std::atomic<float> currentLimiterGrDb { 0.0f };

private:
    juce::CachedValue<float> hpfValue, gateValue, breathValue, deEssFreqValue,
                             deEssAmountValue, ceilingValue, outputGainValue;
    te::AutomatableParameter::Ptr hpfParam, gateParam, breathParam, deEssFreqParam,
                                  deEssAmountParam, ceilingParam, outputGainParam;

    juce::dsp::StateVariableTPTFilter<float> hpfFilter, deEssDetector;
    float lowpassState[2] { 0.0f, 0.0f };
    float gateEnvelope = 0.0f, deEssEnvelope = 0.0f, gateSmooth = 1.0f;
    double lastSampleRate = 44100.0;
    bool initialised = false;
};
