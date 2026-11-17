#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

// Reason-style CV-over-audio modulation source. Emits a bipolar sine LFO
// on output pins (1 = phase, 2 = amplitude).
class CvLfoPlugin : public te::Plugin
{
public:
    explicit CvLfoPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        rateValue.referTo (state, juce::Identifier ("rate"), nullptr, 2.5f);
        depthValue.referTo (state, juce::Identifier ("depth"), nullptr, 0.8f);

        rateParam = addParam ("rate", TRANS("Rate (Hz)"), { 0.05f, 30.0f });
        depthParam = addParam ("depth", TRANS("Depth"), { 0.0f, 1.0f });
        rateParam->attachToCurrentValue (rateValue);
        depthParam->attachToCurrentValue (depthValue);
    }

    ~CvLfoPlugin() override
    {
        if (rateParam != nullptr)  rateParam->detachFromCurrentValue();
        if (depthParam != nullptr) depthParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool takesAudioInput() override { return false; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins)   ins->add ("CV Out 1");
        if (outs) outs->add ("CV Out 2");
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::mono(),
                                                   te::ChannelConfiguration::mono());
    }

    // No audio inputs, but one CV output: without this the engine's default
    // (numInputs) would allocate a zero-channel render buffer.
    int getNumOutputChannelsGivenInputs (int) override { return 1; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = info.sampleRate;
        phase = 0.0;
    }

    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr
            || fc.destBuffer->getNumChannels() < 1
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
        const float rate = (float) rateParam->getCurrentValue();
        const float depth = (float) depthParam->getCurrentValue();
        const float phaseInc = rate / (float) std::max (1.0, lastSampleRate);

        for (int i = 0; i < n; ++i)
        {
            phase += phaseInc;
            if (phase >= 1.0f) phase -= 1.0f;
            buf->setSample (0, base + i,
                            (float) std::sin (phase * juce::MathConstants<double>::twoPi) * depth);
        }
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, rateValue, depthValue);
    }

    static inline const char* xmlTypeName = "cv_lfo_plugin";
    juce::String getName() const override { return "CV LFO"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    juce::CachedValue<float> rateValue, depthValue;
    te::AutomatableParameter::Ptr rateParam, depthParam;
    double lastSampleRate = 44100.0;
    float phase = 0.0f;
};

// Combines two input channels (L/R) with a depth-controlled blend, used for
// phase-vocoder style stereo wobble.
class CvWobbleFilterPlugin : public te::Plugin
{
public:
    explicit CvWobbleFilterPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        depthValue.referTo (state, juce::Identifier ("wobbleDepth"), nullptr, 0.35f);
        depthParam = addParam ("wobbleDepth", TRANS("Wobble Depth"), { 0.0f, 1.0f });
        depthParam->attachToCurrentValue (depthValue);
    }

    ~CvWobbleFilterPlugin() override
    {
        if (depthParam != nullptr) depthParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins)   { ins->add ("L"); ins->add ("R"); }
        if (outs) { outs->add ("L"); outs->add ("R"); }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::stereo(),
                                                   te::ChannelConfiguration::stereo());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 2; }

    void initialise (const te::PluginInitialisationInfo&) override {}
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr
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
        const float depth = (float) depthParam->getCurrentValue();

        for (int i = 0; i < n; ++i)
        {
            const float l = buf->getSample (0, base + i);
            const float r = buf->getSample (1, base + i);
            buf->setSample (0, base + i, r * depth + l * (1.0f - depth));
            buf->setSample (1, base + i, l * depth + r * (1.0f - depth));
        }
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, depthValue);
    }

    static inline const char* xmlTypeName = "cv_wobble_filter";
    juce::String getName() const override { return "CV Wobble Filter"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    juce::CachedValue<float> depthValue;
    te::AutomatableParameter::Ptr depthParam;
};
