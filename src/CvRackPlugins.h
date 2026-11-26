#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace te = tracktion::engine;

// Reason-style CV-over-audio modulation source. Emits a depth-scaled bipolar
// sine LFO on its single mono output pin; it takes no audio input.
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
        // No audio inputs: the only pin is the mono CV output. (RackType's
        // isConnectionLegal reads these lists, so a phantom input name here
        // would make a connection into the LFO look legal.)
        juce::ignoreUnused (ins);

        if (outs) outs->add ("CV Out");
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
        const float rate = static_cast<float> (rateParam->getCurrentValue());
        const float depth = static_cast<float> (depthParam->getCurrentValue());
        const float phaseInc = rate / static_cast<float> (std::max (1.0, lastSampleRate));

        for (int i = 0; i < n; ++i)
        {
            phase += phaseInc;
            if (phase >= 1.0f) phase -= 1.0f;
            buf->setSample (0, base + i,
                            static_cast<float> (std::sin (phase * juce::MathConstants<double>::twoPi)) * depth);
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

// Reason-style CV-modulated State-Variable TPT filter. Takes stereo audio (L/R)
// on pins 1/2 and an optional mono CV modulation source on pin 3 ("CV In"),
// sweeping filter cutoff exponentially (+/- 3 octaves scaled by Wobble Depth).
class CvWobbleFilterPlugin : public te::Plugin
{
public:
    explicit CvWobbleFilterPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        depthValue.referTo     (state, juce::Identifier ("wobbleDepth"), nullptr, 0.5f);
        cutoffValue.referTo    (state, juce::Identifier ("cutoffHz"),    nullptr, 900.0f);
        resonanceValue.referTo (state, juce::Identifier ("resonance"),   nullptr, 1.8f);

        depthParam     = addParam ("wobbleDepth", TRANS("Wobble Depth"), { 0.0f, 1.0f });
        cutoffParam    = addParam ("cutoffHz",    TRANS("Cutoff (Hz)"),  { 80.0f, 12000.0f });
        resonanceParam = addParam ("resonance",   TRANS("Resonance"),    { 0.5f, 6.0f });

        depthParam->attachToCurrentValue (depthValue);
        cutoffParam->attachToCurrentValue (cutoffValue);
        resonanceParam->attachToCurrentValue (resonanceValue);
    }

    ~CvWobbleFilterPlugin() override
    {
        if (depthParam != nullptr)     depthParam->detachFromCurrentValue();
        if (cutoffParam != nullptr)    cutoffParam->detachFromCurrentValue();
        if (resonanceParam != nullptr) resonanceParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins)  { ins->add ("L"); ins->add ("R"); ins->add ("CV In"); }
        if (outs) { outs->add ("L"); outs->add ("R"); }
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
        svfFilter.reset();
        svfFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);

        const juce::uint32 maxBlock = static_cast<juce::uint32> (std::max (16, info.blockSizeSamples));
        svfFilter.prepare ({ lastSampleRate, maxBlock, 2 });
    }

    void deinitialise() override
    {
        svfFilter.reset();
    }

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

        const float baseCutoff = cutoffParam != nullptr ? static_cast<float> (cutoffParam->getCurrentValue()) : 900.0f;
        const float reso       = resonanceParam != nullptr ? static_cast<float> (resonanceParam->getCurrentValue()) : 1.8f;
        const float depth      = depthParam != nullptr ? static_cast<float> (depthParam->getCurrentValue()) : 0.5f;
        const bool hasCvCh     = (buf->getNumChannels() >= 3);
        const float maxSafeHz  = static_cast<float> (lastSampleRate * 0.45);

        svfFilter.setResonance (juce::jlimit (0.5f, 6.0f, reso));

        for (int i = 0; i < n; ++i)
        {
            if ((i & 7) == 0)
            {
                const float cv = hasCvCh ? juce::jlimit (-1.0f, 1.0f, buf->getSample (2, base + i)) : 0.0f;
                const float modCutoff = juce::jlimit (60.0f, maxSafeHz,
                                                      baseCutoff * std::pow (2.0f, cv * depth * 3.0f));
                svfFilter.setCutoffFrequency (modCutoff);
            }

            const float l = buf->getSample (0, base + i);
            const float r = buf->getSample (1, base + i);
            buf->setSample (0, base + i, svfFilter.processSample (0, l));
            buf->setSample (1, base + i, svfFilter.processSample (1, r));
        }

        if (hasCvCh)
            buf->clear (2, base, n);
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, depthValue, cutoffValue, resonanceValue);

        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    static inline const char* xmlTypeName = "cv_wobble_filter";
    juce::String getName() const override { return "CV Wobble Filter"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    juce::CachedValue<float> depthValue, cutoffValue, resonanceValue;
    te::AutomatableParameter::Ptr depthParam, cutoffParam, resonanceParam;
    juce::dsp::StateVariableTPTFilter<float> svfFilter;
    double lastSampleRate = 44100.0;
};

// Reason-style 1-to-4 CV attenuverter & polarity splitter. Captures the mono
// CV input scalar before overwriting channels 0..3 so in-place processing
// never aliases channel 0 onto channels 1..3.
class CvUtilityPlugin : public te::Plugin
{
public:
    static constexpr int numOutputs = 4;

    explicit CvUtilityPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        for (int i = 0; i < numOutputs; ++i)
        {
            const auto idx = static_cast<size_t> (i);
            const auto gainId   = "gain" + juce::String (i + 1);
            const auto offsetId = "offset" + juce::String (i + 1);

            gainValues[idx].referTo (state, juce::Identifier (gainId), getUndoManager(), 1.0f);
            offsetValues[idx].referTo (state, juce::Identifier (offsetId), getUndoManager(), 0.0f);

            gainParams[idx]   = addParam (gainId, "Out " + juce::String (i + 1) + " Gain", { -1.0f, 1.0f });
            offsetParams[idx] = addParam (offsetId, "Out " + juce::String (i + 1) + " Offset", { -1.0f, 1.0f });
            gainParams[idx]->attachToCurrentValue (gainValues[idx]);
            offsetParams[idx]->attachToCurrentValue (offsetValues[idx]);
        }
    }

    ~CvUtilityPlugin() override
    {
        for (size_t i = 0; i < static_cast<size_t> (numOutputs); ++i)
        {
            if (gainParams[i] != nullptr)   gainParams[i]->detachFromCurrentValue();
            if (offsetParams[i] != nullptr) offsetParams[i]->detachFromCurrentValue();
        }
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins != nullptr)
            ins->add ("CV In");

        if (outs != nullptr)
        {
            outs->add ("CV Out 1");
            outs->add ("CV Out 2");
            outs->add ("CV Out 3");
            outs->add ("CV Out 4");
        }
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::mono(),
                                                   te::ChannelConfiguration::discreteChannels (numOutputs));
    }

    int getNumOutputChannelsGivenInputs (int) override { return numOutputs; }

    void initialise (const te::PluginInitialisationInfo&) override {}
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
        const int outChans = std::min (numOutputs, buf->getNumChannels());

        std::array<float, numOutputs> gains {};
        std::array<float, numOutputs> offsets {};

        for (int c = 0; c < outChans; ++c)
        {
            const auto idx = static_cast<size_t> (c);
            gains[idx]   = static_cast<float> (gainParams[idx]->getCurrentValue());
            offsets[idx] = static_cast<float> (offsetParams[idx]->getCurrentValue());
        }

        for (int i = 0; i < n; ++i)
        {
            // Capture channel 0 before writing so out 1..3 never read modified out 0.
            const float inSample = buf->getSample (0, base + i);

            for (int c = 0; c < outChans; ++c)
            {
                const auto idx = static_cast<size_t> (c);
                buf->setSample (c, base + i, juce::jlimit (-1.0f, 1.0f, inSample * gains[idx] + offsets[idx]));
            }
        }
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v,
                                          gainValues[0], gainValues[1], gainValues[2], gainValues[3],
                                          offsetValues[0], offsetValues[1], offsetValues[2], offsetValues[3]);

        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    void setOutputGain (int idx, float g)
    {
        if (idx >= 0 && idx < numOutputs)
        {
            const auto uIdx = static_cast<size_t> (idx);
            gainValues[uIdx] = juce::jlimit (-1.0f, 1.0f, g);

            if (gainParams[uIdx] != nullptr)
                gainParams[uIdx]->updateFromAttachedValue();
        }
    }

    void setOutputOffset (int idx, float o)
    {
        if (idx >= 0 && idx < numOutputs)
        {
            const auto uIdx = static_cast<size_t> (idx);
            offsetValues[uIdx] = juce::jlimit (-1.0f, 1.0f, o);

            if (offsetParams[uIdx] != nullptr)
                offsetParams[uIdx]->updateFromAttachedValue();
        }
    }

    float getOutputGain (int idx) const
    {
        return (idx >= 0 && idx < numOutputs) ? gainValues[static_cast<size_t> (idx)].get() : 0.0f;
    }

    float getOutputOffset (int idx) const
    {
        return (idx >= 0 && idx < numOutputs) ? offsetValues[static_cast<size_t> (idx)].get() : 0.0f;
    }

    static inline const char* xmlTypeName = "cv_utility_plugin";
    juce::String getName() const override { return "CV Splitter / Attenuverter"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    std::array<juce::CachedValue<float>, numOutputs> gainValues, offsetValues;
    std::array<te::AutomatableParameter::Ptr, numOutputs> gainParams, offsetParams;
};

// Original briefing blueprint residue: audio-rate envelope follower. Converts a
// mono audio/CV input into a unipolar control envelope with one-pole attack and
// release, scaled by Depth. Output is mono CV on pin 1.
class CvEnvelopeFollowerPlugin : public te::Plugin
{
public:
    explicit CvEnvelopeFollowerPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        attackValue.referTo  (state, juce::Identifier ("attackMs"), nullptr, 10.0f);
        releaseValue.referTo (state, juce::Identifier ("releaseMs"), nullptr, 180.0f);
        depthValue.referTo   (state, juce::Identifier ("envDepth"), nullptr, 1.0f);

        attackParam  = addParam ("attackMs",  TRANS("Attack (ms)"), { 1.0f, 200.0f });
        releaseParam = addParam ("releaseMs", TRANS("Release (ms)"), { 10.0f, 2000.0f });
        depthParam   = addParam ("envDepth",  TRANS("Depth"), { 0.0f, 1.0f });
        attackParam->attachToCurrentValue (attackValue);
        releaseParam->attachToCurrentValue (releaseValue);
        depthParam->attachToCurrentValue (depthValue);
    }

    ~CvEnvelopeFollowerPlugin() override
    {
        if (attackParam  != nullptr) attackParam->detachFromCurrentValue();
        if (releaseParam != nullptr) releaseParam->detachFromCurrentValue();
        if (depthParam   != nullptr) depthParam->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool takesAudioInput() override { return true; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins)  ins->add ("Audio/CV In");
        if (outs) outs->add ("CV Out");
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::mono(),
                                                   te::ChannelConfiguration::mono());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 1; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = std::max (8000.0, info.sampleRate);
        envelope = 0.0f;
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
        const float rateMs  = static_cast<float> (attackParam->getCurrentValue());
        const float fallMs  = static_cast<float> (releaseParam->getCurrentValue());
        const float depth   = static_cast<float> (depthParam->getCurrentValue());
        const float sr      = static_cast<float> (lastSampleRate);
        const float upCoeff = 1.0f - std::exp (-1.0f / (std::max (1.0f, rateMs) * 0.001f * sr));
        const float dnCoeff = 1.0f - std::exp (-1.0f / (std::max (1.0f, fallMs) * 0.001f * sr));

        for (int i = 0; i < fc.bufferNumSamples; ++i)
        {
            const float target = std::abs (buf->getSample (0, fc.bufferStartSample + i));
            const float coeff  = target > envelope ? upCoeff : dnCoeff;
            envelope += coeff * (target - envelope);
            buf->setSample (0, fc.bufferStartSample + i,
                            juce::jlimit (-1.0f, 1.0f, envelope * depth));
        }
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, attackValue, releaseValue, depthValue);
        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    static inline const char* xmlTypeName = "cv_envelope_follower";
    juce::String getName() const override { return "CV Envelope Follower"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    juce::CachedValue<float> attackValue, releaseValue, depthValue;
    te::AutomatableParameter::Ptr attackParam, releaseParam, depthParam;
    double lastSampleRate = 44100.0;
    float envelope = 0.0f;
};

// Original briefing blueprint residue: 8-step CV sequencer. Emits a bipolar
// step held for 1/rate seconds at a time on its mono CV output pin.
class CvStepModulatorPlugin : public te::Plugin
{
public:
    static constexpr int numSteps = 8;

    explicit CvStepModulatorPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        rateValue.referTo  (state, juce::Identifier ("stepRateHz"), nullptr, 4.0f);
        depthValue.referTo (state, juce::Identifier ("stepDepth"),  nullptr, 1.0f);

        rateParam  = addParam ("stepRateHz", TRANS("Step Rate (Hz)"), { 0.1f, 32.0f });
        depthParam = addParam ("stepDepth",  TRANS("Depth"), { 0.0f, 1.0f });
        rateParam->attachToCurrentValue (rateValue);
        depthParam->attachToCurrentValue (depthValue);

        for (int i = 0; i < numSteps; ++i)
        {
            const auto id = "step" + juce::String (i + 1);
            stepValues[i].referTo (state, juce::Identifier (id), nullptr, (i % 2 == 0) ? 1.0f : -1.0f);
            stepParams[i] = addParam (id, "Step " + juce::String (i + 1), { -1.0f, 1.0f });
            stepParams[i]->attachToCurrentValue (stepValues[i]);
        }
    }

    ~CvStepModulatorPlugin() override
    {
        if (rateParam  != nullptr) rateParam->detachFromCurrentValue();
        if (depthParam != nullptr) depthParam->detachFromCurrentValue();
        for (auto& p : stepParams)
            if (p != nullptr) p->detachFromCurrentValue();
        notifyListenersOfDeletion();
    }

    juce::String getSelectableDescription() override { return getName(); }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool takesAudioInput() override { return false; }
    bool canBeAddedToRack() override { return true; }

    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        juce::ignoreUnused (ins);
        if (outs) outs->add ("CV Out");
    }

    te::Plugin::BusLayout getBusses() const override
    {
        return te::Plugin::BusLayout::singleInOut (te::ChannelConfiguration::mono(),
                                                   te::ChannelConfiguration::mono());
    }

    int getNumOutputChannelsGivenInputs (int) override { return 1; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        lastSampleRate = std::max (8000.0, info.sampleRate);
        phase = 0.0f;
        stepIndex = 0;
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

        const float rate  = static_cast<float> (rateParam->getCurrentValue());
        const float depth = static_cast<float> (depthParam->getCurrentValue());
        const float inc   = rate / static_cast<float> (lastSampleRate);

        for (int i = 0; i < fc.bufferNumSamples; ++i)
        {
            phase += inc;
            if (phase >= 1.0f)
            {
                phase -= 1.0f;
                stepIndex = (stepIndex + 1) % numSteps;
            }

            const float value = static_cast<float> (stepParams[stepIndex]->getCurrentValue()) * depth;
            fc.destBuffer->setSample (0, fc.bufferStartSample + i, juce::jlimit (-1.0f, 1.0f, value));
        }
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyPropertiesToCachedValues (v, rateValue, depthValue);
        for (auto& cached : stepValues)
            te::copyPropertiesToCachedValues (v, cached);
        for (auto* p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    static inline const char* xmlTypeName = "cv_step_modulator";
    juce::String getName() const override { return "CV Step Modulator"; }
    juce::String getPluginType() override { return xmlTypeName; }

private:
    juce::CachedValue<float> rateValue, depthValue;
    std::array<juce::CachedValue<float>, numSteps> stepValues;
    te::AutomatableParameter::Ptr rateParam, depthParam;
    std::array<te::AutomatableParameter::Ptr, numSteps> stepParams;
    double lastSampleRate = 44100.0;
    float phase = 0.0f;
    int stepIndex = 0;
};

inline void registerCvRackPlugins (te::Engine& engine)
{
    auto& pm = engine.getPluginManager();
    pm.createBuiltInType<CvLfoPlugin>();
    pm.createBuiltInType<CvWobbleFilterPlugin>();
    pm.createBuiltInType<CvUtilityPlugin>();
    pm.createBuiltInType<CvEnvelopeFollowerPlugin>();
    pm.createBuiltInType<CvStepModulatorPlugin>();
}
