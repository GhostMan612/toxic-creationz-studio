#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace te = tracktion::engine;

// Dynamics processor with a dedicated stereo sidechain. On a 4-channel rack
// buffer the detection signal comes from pins 3/4 (channels 2/3) and those
// channels are cleared after reading; on a plain 2-channel AudioTrack the
// main L/R signal is used for self-sidechaining. Every DSP state value is a
// fixed-size scalar, so applyToBuffer() performs zero heap allocations.
class SidechainCompressorPlugin : public te::Plugin
{
public:
    explicit SidechainCompressorPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        thresholdValue.referTo (state, juce::Identifier ("thresholdDb"), nullptr, -18.0f);
        ratioValue.referTo     (state, juce::Identifier ("ratio"),       nullptr, 6.0f);
        attackValue.referTo    (state, juce::Identifier ("attackMs"),    nullptr, 5.0f);
        releaseValue.referTo   (state, juce::Identifier ("releaseMs"),   nullptr, 120.0f);
        makeupValue.referTo    (state, juce::Identifier ("makeupDb"),    nullptr, 0.0f);
        mixValue.referTo       (state, juce::Identifier ("mix"),         nullptr, 1.0f);

        thresholdParam = addParam ("thresholdDb", TRANS("Threshold (dB)"), { -48.0f, 0.0f });
        ratioParam     = addParam ("ratio",        TRANS("Ratio"),          { 1.0f, 20.0f });
        attackParam    = addParam ("attackMs",     TRANS("Attack (ms)"),    { 0.5f, 100.0f });
        releaseParam   = addParam ("releaseMs",    TRANS("Release (ms)"),   { 10.0f, 1000.0f });
        makeupParam    = addParam ("makeupDb",     TRANS("Makeup (dB)"),    { -12.0f, 24.0f });
        mixParam       = addParam ("mix",          TRANS("Mix"),            { 0.0f, 1.0f });

        thresholdParam->attachToCurrentValue (thresholdValue);
        ratioParam->attachToCurrentValue (ratioValue);
        attackParam->attachToCurrentValue (attackValue);
        releaseParam->attachToCurrentValue (releaseValue);
        makeupParam->attachToCurrentValue (makeupValue);
        mixParam->attachToCurrentValue (mixValue);
    }

    ~SidechainCompressorPlugin() override
    {
        if (thresholdParam != nullptr) thresholdParam->detachFromCurrentValue();
        if (ratioParam != nullptr)     ratioParam->detachFromCurrentValue();
        if (attackParam != nullptr)    attackParam->detachFromCurrentValue();
        if (releaseParam != nullptr)   releaseParam->detachFromCurrentValue();
        if (makeupParam != nullptr)    makeupParam->detachFromCurrentValue();
        if (mixParam != nullptr)       mixParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins)
        {
            ins->add ("Audio In L");
            ins->add ("Audio In R");
            ins->add ("Sidechain In L");
            ins->add ("Sidechain In R");
        }
        if (outs)
        {
            outs->add ("Audio Out L");
            outs->add ("Audio Out R");
        }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::discreteChannels (4),
                                                   te::ChannelConfiguration::stereo());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 2; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = std::max (8000.0, info.sampleRate);
        envPeak = 0.0f;
        envMeanSquare = 0.0f;
        initialised = true;
        currentGainReductionDb.store (0.0f);
    }

    void deinitialise() override
    {
        initialised = false;
        envPeak = 0.0f;
        envMeanSquare = 0.0f;
        currentGainReductionDb.store (0.0f);
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
        const int numCh = buf->getNumChannels();
        const bool hasSidechainPins = numCh >= 4;
        const int scL = hasSidechainPins ? 2 : 0;
        const int scR = hasSidechainPins ? 3 : 1;

        const float threshold = thresholdParam != nullptr
            ? juce::jlimit (-48.0f, 0.0f, static_cast<float> (thresholdParam->getCurrentValue())) : -18.0f;
        const float ratio = ratioParam != nullptr
            ? juce::jlimit (1.0f, 20.0f, static_cast<float> (ratioParam->getCurrentValue())) : 6.0f;
        const float attackMs = attackParam != nullptr
            ? static_cast<float> (attackParam->getCurrentValue()) : 5.0f;
        const float releaseMs = releaseParam != nullptr
            ? static_cast<float> (releaseParam->getCurrentValue()) : 120.0f;
        const float makeupDb = makeupParam != nullptr
            ? static_cast<float> (makeupParam->getCurrentValue()) : 0.0f;
        const float mix = mixParam != nullptr
            ? juce::jlimit (0.0f, 1.0f, static_cast<float> (mixParam->getCurrentValue())) : 1.0f;

        const double sr = std::max (8000.0, lastSampleRate);
        const float attackCoef = static_cast<float> (
            std::exp (-1.0 / (std::max (0.5, static_cast<double> (attackMs)) * 0.001 * sr)));
        const float releaseCoef = static_cast<float> (
            std::exp (-1.0 / (std::max (10.0, static_cast<double> (releaseMs)) * 0.001 * sr)));
        const float slope = 1.0f - 1.0f / ratio;
        const float halfKnee = 3.0f; // fixed 6 dB soft knee
        const float makeupLin = juce::Decibels::decibelsToGain (juce::jlimit (-12.0f, 24.0f, makeupDb));
        const float dryMix = 1.0f - mix;

        for (int i = 0; i < n; ++i)
        {
            const int pos = base + i;
            const float sideL = buf->getSample (scL, pos);
            const float sideR = buf->getSample (scR, pos);
            const float sidePeak = std::max (std::abs (sideL), std::abs (sideR));
            const float sideMs = 0.5f * (sideL * sideL + sideR * sideR);

            const float peakCoef = sidePeak > envPeak ? attackCoef : releaseCoef;
            envPeak = sidePeak + peakCoef * (envPeak - sidePeak);
            const float msCoef = sideMs > envMeanSquare ? attackCoef : releaseCoef;
            envMeanSquare = sideMs + msCoef * (envMeanSquare - sideMs);

            const float detectDb = juce::Decibels::gainToDecibels (
                std::max (std::sqrt (std::max (0.0f, envMeanSquare)), 1.0e-6f));
            const float over = detectDb - threshold;

            float grDb = 0.0f;
            if (over > -halfKnee)
            {
                if (over >= halfKnee)
                    grDb = over * slope;
                else
                {
                    const float kneePos = over + halfKnee;
                    grDb = slope * kneePos * kneePos / (4.0f * halfKnee);
                }
            }
            grDb = juce::jlimit (0.0f, 48.0f, grDb);
            currentGainReductionDb.store (-grDb);

            // Peak safety: never let makeup push the linked output past 0 dBFS.
            const float peakLin = std::max (envPeak, 1.0e-6f);
            const float peakCeiling = 1.0f / (peakLin * makeupLin);
            const float wetGain = juce::jlimit (0.0f, 8.0f,
                std::min (juce::Decibels::decibelsToGain (makeupDb - grDb), peakCeiling));

            for (int ch = 0; ch < 2; ++ch)
            {
                const float dry = buf->getSample (ch, pos);
                buf->setSample (ch, pos, dry * dryMix + dry * wetGain * mix);
            }

            if (hasSidechainPins)
            {
                buf->setSample (2, pos, 0.0f);
                buf->setSample (3, pos, 0.0f);
            }
        }
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, thresholdValue, ratioValue, attackValue,
                                          releaseValue, makeupValue, mixValue);

        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    static inline const char* xmlTypeName = "tcs_sidechain_comp";
    juce::String getName() const override { return "Sidechain Compressor"; }
    juce::String getPluginType() override { return xmlTypeName; }

    std::atomic<float> currentGainReductionDb { 0.0f };

private:
    juce::CachedValue<float> thresholdValue, ratioValue, attackValue;
    juce::CachedValue<float> releaseValue, makeupValue, mixValue;
    te::AutomatableParameter::Ptr thresholdParam, ratioParam, attackParam;
    te::AutomatableParameter::Ptr releaseParam, makeupParam, mixParam;

    double lastSampleRate = 44100.0;
    float envPeak = 0.0f;
    float envMeanSquare = 0.0f;
    bool initialised = false;
};

inline void registerDynamicsPlugins (te::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<SidechainCompressorPlugin>();
}
