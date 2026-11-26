#include "PluginPresetManager.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
juce::String sanitizeTypeFolderName (const juce::String& pluginType)
{
    juce::String cleaned = juce::File::createLegalFileName (pluginType.trim());
    return cleaned.isNotEmpty() ? cleaned : "default";
}

void setParamIfPresent (te::Plugin& plugin, const juce::String& paramID, float value)
{
    if (auto p = plugin.getAutomatableParameterByID (paramID))
        p->setParameter (value, juce::sendNotification);
}

void refreshAllAttachedParams (te::Plugin& plugin)
{
    for (auto* p : plugin.getAutomatableParameters())
        p->updateFromAttachedValue();
}

void resetFourOscToBaseline (te::FourOscPlugin& synth, juce::UndoManager* um)
{
    for (int i = 0; i < synth.oscParams.size(); ++i)
    {
        auto* osc = synth.oscParams.getUnchecked (i);
        osc->waveShapeValue.setValue (i == 0 ? 1 : 0, um);
        osc->tuneValue.setValue (0.0f, um);
        osc->fineTuneValue.setValue (0.0f, um);
        osc->levelValue.setValue (0.0f, um);
        osc->pulseWidthValue.setValue (0.5f, um);
        osc->voicesValue.setValue (1, um);
        osc->detuneValue.setValue (0.0f, um);
        osc->spreadValue.setValue (0.0f, um);
        osc->panValue.setValue (0.0f, um);
    }

    for (int i = 0; i < synth.lfoParams.size(); ++i)
    {
        auto* lfo = synth.lfoParams.getUnchecked (i);
        lfo->waveShapeValue.setValue (i == 0 ? 1 : 0, um);
        lfo->syncValue.setValue (false, um);
        lfo->rateValue.setValue (1.0f, um);
        lfo->depthValue.setValue (1.0f, um);
        lfo->beatValue.setValue (1.0f, um);
    }

    for (int i = 0; i < synth.modEnvParams.size(); ++i)
    {
        auto* env = synth.modEnvParams.getUnchecked (i);
        env->modAttackValue.setValue (0.1f, um);
        env->modDecayValue.setValue (0.1f, um);
        env->modSustainValue.setValue (80.0f, um);
        env->modReleaseValue.setValue (0.1f, um);
    }

    synth.ampAttackValue.setValue (0.005f, um);
    synth.ampDecayValue.setValue (0.20f, um);
    synth.ampSustainValue.setValue (85.0f, um);
    synth.ampReleaseValue.setValue (0.15f, um);
    synth.ampVelocityValue.setValue (100.0f, um);
    synth.ampAnalogValue.setValue (true, um);

    synth.filterAttackValue.setValue (0.005f, um);
    synth.filterDecayValue.setValue (0.25f, um);
    synth.filterSustainValue.setValue (50.0f, um);
    synth.filterReleaseValue.setValue (0.20f, um);
    synth.filterFreqValue.setValue (100.0f, um);
    synth.filterResonanceValue.setValue (10.0f, um);
    synth.filterAmountValue.setValue (0.0f, um);
    synth.filterKeyValue.setValue (0.0f, um);
    synth.filterVelocityValue.setValue (0.0f, um);
    synth.filterTypeValue.setValue (0, um);
    synth.filterSlopeValue.setValue (12, um);

    synth.distortionOnValue.setValue (false, um);
    synth.distortionValue.setValue (0.0f, um);

    synth.reverbOnValue.setValue (false, um);
    synth.reverbSizeValue.setValue (0.3f, um);
    synth.reverbDampingValue.setValue (0.5f, um);
    synth.reverbWidthValue.setValue (0.8f, um);
    synth.reverbMixValue.setValue (0.0f, um);

    synth.delayOnValue.setValue (false, um);
    synth.delayValue.setValue (0.5f, um);
    synth.delayFeedbackValue.setValue (-10.0f, um);
    synth.delayCrossfeedValue.setValue (-100.0f, um);
    synth.delayMixValue.setValue (0.0f, um);

    synth.chorusOnValue.setValue (false, um);
    synth.chorusSpeedValue.setValue (1.0f, um);
    synth.chorusDepthValue.setValue (3.0f, um);
    synth.chorusWidthValue.setValue (0.5f, um);
    synth.chorusMixValue.setValue (0.0f, um);

    synth.voiceModeValue.setValue (2, um);
    synth.voicesValue.setValue (16, um);
    synth.legatoValue.setValue (0.0f, um);
    synth.masterLevelValue.setValue (-3.0f, um);
}

bool writeStarterWav (const juce::File& wavFile, double durationSec, int kind)
{
    constexpr int sampleRate = 44100;
    const int numSamples = std::max (256, static_cast<int> (std::round (durationSec * sampleRate)));
    juce::AudioBuffer<float> buf (2, numSamples);
    buf.clear();

    juce::Random rng (42 + kind * 97);
    double phase = 0.0;
    double phaseB = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double t = static_cast<double> (i) / sampleRate;
        float outL = 0.0f;
        float outR = 0.0f;

        if (kind == 0)
        {
            // 808 Punch Kick (pitch drops quickly from 150 Hz -> 46 Hz)
            const double instHz = 46.0 + 104.0 * std::exp (-t * 28.0);
            phase += juce::MathConstants<double>::twoPi * instHz / sampleRate;
            const double env = std::exp (-t * 7.5);
            const float sample = static_cast<float> (std::tanh (std::sin (phase) * 1.35) * env * 0.90);
            outL = sample;
            outR = sample;
        }
        else if (kind == 1)
        {
            // Crisp Electronic Snare
            const double toneHz = 185.0 + 95.0 * std::exp (-t * 35.0);
            phase += juce::MathConstants<double>::twoPi * toneHz / sampleRate;
            const float body = static_cast<float> (std::sin (phase) * std::exp (-t * 22.0) * 0.55);
            const float white = rng.nextFloat() * 2.0f - 1.0f;
            const float snap = white * static_cast<float> (std::exp (-t * 16.0) * 0.55);
            outL = juce::jlimit (-0.98f, 0.98f, body + snap);
            outR = outL;
        }
        else if (kind == 2)
        {
            // Closed Hi-Hat
            const float white = rng.nextFloat() * 2.0f - 1.0f;
            const float metallic = static_cast<float> (std::sin (t * 6200.0) * 0.35 + white * 0.65);
            const float env = static_cast<float> (std::exp (-t * 38.0));
            outL = metallic * env * 0.72f;
            outR = outL;
        }
        else
        {
            // Stereo Detuned C4 Fifth Synth Stab
            const double f1 = 261.6256;
            const double f2 = 261.6256 * 1.498307; // Fifth (+7 st)
            phase  += f1 / sampleRate;
            phaseB += f2 / sampleRate;
            if (phase >= 1.0)  phase -= 1.0;
            if (phaseB >= 1.0) phaseB -= 1.0;

            const float saw1 = static_cast<float> (2.0 * phase - 1.0);
            const float saw2 = static_cast<float> (2.0 * phaseB - 1.0);
            const float env  = static_cast<float> (std::min (1.0, t / 0.008) * std::exp (-t * 5.2));
            outL = (saw1 * 0.55f + saw2 * 0.35f) * env * 0.65f;
            outR = (saw1 * 0.45f + saw2 * 0.45f) * env * 0.65f;
        }

        // Smooth fade-out tail on last 64 samples to guarantee click-free loop boundaries
        if (i >= numSamples - 64)
        {
            const float fade = static_cast<float> (numSamples - 1 - i) / 64.0f;
            outL *= fade;
            outR *= fade;
        }

        buf.setSample (0, i, outL);
        buf.setSample (1, i, outR);
    }

    wavFile.getParentDirectory().createDirectory();
    if (wavFile.existsAsFile())
        wavFile.deleteFile();

    auto fs = std::make_unique<juce::FileOutputStream> (wavFile);
    if (! fs->openedOk())
        return false;

    std::unique_ptr<juce::OutputStream> os (fs.release());
    juce::WavAudioFormat fmt;
    auto writer = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                               .withSampleRate (sampleRate)
                                               .withNumChannels (2)
                                               .withBitsPerSample (16));
    if (writer == nullptr)
        return false;

    return writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
}
} // namespace

juce::File PluginPresetManager::getStudioPresetsRootDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Toxic Creationz Studio")
                   .getChildFile ("Presets");
    dir.createDirectory();
    return dir;
}

juce::File PluginPresetManager::getUserPresetsDirectory (const juce::String& pluginType)
{
    auto dir = getStudioPresetsRootDir().getChildFile (sanitizeTypeFolderName (pluginType));
    dir.createDirectory();
    return dir;
}

juce::File PluginPresetManager::getDefaultSamplerKitDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Toxic Creationz Studio")
                   .getChildFile ("SamplerKits")
                   .getChildFile ("Neon808Kit");
    dir.createDirectory();
    return dir;
}

juce::StringArray PluginPresetManager::getFactoryPresetNames (te::Plugin& plugin)
{
    if (dynamic_cast<te::FourOscPlugin*> (&plugin) != nullptr)
    {
        return {
            "Init Sine",
            "Cyber Sub Bass",
            "Reese Neuro Bass",
            "Supersaw Anthem Lead",
            "Acid 303 Squelch",
            "Pluck Neon Arp",
            "Lush Cyberpunk Pad",
            "Crystal Bell Keys"
        };
    }

    if (dynamic_cast<te::SamplerPlugin*> (&plugin) != nullptr)
        return { "Neon 808 + Synth Kit" };

    if (dynamic_cast<Tr808DrumPlugin*> (&plugin) != nullptr)
        return { "Classic 808 Boom", "Sub Shaker 808", "Tight Electro 808" };

    if (dynamic_cast<VintageSamplerPlugin*> (&plugin) != nullptr)
        return { "SP-1200 Grit (12-Bit / 26 kHz)", "Lo-Fi Crunch (8-Bit / 12 kHz)", "Pure ZOH Staircase" };

    if (dynamic_cast<AnalogTapePlugin*> (&plugin) != nullptr)
        return { "Warm Master Glue", "Lo-Fi Cassette", "Hot Driven Reel" };

    if (dynamic_cast<CvWobbleFilterPlugin*> (&plugin) != nullptr)
        return { "Sub Wobble (450 Hz)", "Mid Growl (900 Hz)", "Screamer Sweep (1.8 kHz)" };

    if (dynamic_cast<CvLfoPlugin*> (&plugin) != nullptr)
        return { "Slow Sweep (0.25 Hz)", "Quarter Wobble (2 Hz)", "Fast Flutter (6 Hz)" };

    if (dynamic_cast<SidechainCompressorPlugin*> (&plugin) != nullptr)
        return { "EDM Pump", "Vocal Duck", "Glue Bus" };

    if (dynamic_cast<te::ReverbPlugin*> (&plugin) != nullptr)
        return { "Tight Room", "Cyber Hall", "Ambient Wash" };

    if (dynamic_cast<te::DelayPlugin*> (&plugin) != nullptr)
        return { "Slapback", "Dub Echo" };

    if (dynamic_cast<te::CompressorPlugin*> (&plugin) != nullptr)
        return { "Punchy Bus", "Tight Limiter" };

    if (dynamic_cast<te::ChorusPlugin*> (&plugin) != nullptr)
        return { "Wide Ensemble", "Subtle Shimmer" };

    if (dynamic_cast<te::PhaserPlugin*> (&plugin) != nullptr)
        return { "Slow Jet", "Deep Phase" };

    return {};
}

bool PluginPresetManager::applyFactoryPreset (te::Plugin& plugin, const juce::String& presetName)
{
    const auto trimmed = presetName.trim();
    if (trimmed.isEmpty())
        return false;

    auto& um = plugin.edit.getUndoManager();

    if (auto* synth = dynamic_cast<te::FourOscPlugin*> (&plugin))
    {
        const auto validNames = getFactoryPresetNames (plugin);
        if (! validNames.contains (trimmed))
            return false;

        um.beginNewTransaction ("Load Preset: " + trimmed);
        resetFourOscToBaseline (*synth, &um);

        if (trimmed == "Init Sine")
        {
            // Baseline already set
        }
        else if (trimmed == "Cyber Sub Bass")
        {
            synth->voiceModeValue.setValue (0, &um);
            synth->oscParams[0]->waveShapeValue.setValue (1, &um); // Sine
            synth->oscParams[0]->levelValue.setValue (0.0f, &um);
            synth->oscParams[1]->waveShapeValue.setValue (4, &um); // Triangle
            synth->oscParams[1]->tuneValue.setValue (-12.0f, &um);
            synth->oscParams[1]->levelValue.setValue (-6.0f, &um);
            synth->filterTypeValue.setValue (1, &um);
            synth->filterSlopeValue.setValue (24, &um);
            synth->filterFreqValue.setValue (48.0f, &um);
            synth->filterResonanceValue.setValue (18.0f, &um);
            synth->filterAmountValue.setValue (0.25f, &um);
            synth->filterDecayValue.setValue (0.22f, &um);
            synth->filterSustainValue.setValue (20.0f, &um);
            synth->ampReleaseValue.setValue (0.12f, &um);
            synth->distortionOnValue.setValue (true, &um);
            synth->distortionValue.setValue (0.22f, &um);
        }
        else if (trimmed == "Reese Neuro Bass")
        {
            synth->voiceModeValue.setValue (1, &um); // Legato
            synth->legatoValue.setValue (65.0f, &um);
            synth->oscParams[0]->waveShapeValue.setValue (3, &um); // Saw
            synth->oscParams[0]->voicesValue.setValue (4, &um);
            synth->oscParams[0]->detuneValue.setValue (0.22f, &um);
            synth->oscParams[0]->spreadValue.setValue (60.0f, &um);
            synth->oscParams[0]->levelValue.setValue (0.0f, &um);
            synth->oscParams[1]->waveShapeValue.setValue (3, &um); // Saw
            synth->oscParams[1]->tuneValue.setValue (-12.0f, &um);
            synth->oscParams[1]->voicesValue.setValue (2, &um);
            synth->oscParams[1]->detuneValue.setValue (0.14f, &um);
            synth->oscParams[1]->levelValue.setValue (-3.0f, &um);
            synth->filterTypeValue.setValue (1, &um);
            synth->filterSlopeValue.setValue (24, &um);
            synth->filterFreqValue.setValue (58.0f, &um);
            synth->filterResonanceValue.setValue (38.0f, &um);
            synth->filterAmountValue.setValue (0.35f, &um);
            synth->distortionOnValue.setValue (true, &um);
            synth->distortionValue.setValue (0.32f, &um);
            synth->chorusOnValue.setValue (true, &um);
            synth->chorusSpeedValue.setValue (0.8f, &um);
            synth->chorusDepthValue.setValue (4.5f, &um);
            synth->chorusWidthValue.setValue (0.8f, &um);
            synth->chorusMixValue.setValue (0.25f, &um);
        }
        else if (trimmed == "Supersaw Anthem Lead")
        {
            synth->voiceModeValue.setValue (2, &um); // Poly
            synth->oscParams[0]->waveShapeValue.setValue (3, &um); // Saw
            synth->oscParams[0]->voicesValue.setValue (6, &um);
            synth->oscParams[0]->detuneValue.setValue (0.26f, &um);
            synth->oscParams[0]->spreadValue.setValue (85.0f, &um);
            synth->oscParams[0]->levelValue.setValue (0.0f, &um);
            synth->oscParams[1]->waveShapeValue.setValue (3, &um); // Saw
            synth->oscParams[1]->tuneValue.setValue (12.0f, &um);
            synth->oscParams[1]->voicesValue.setValue (4, &um);
            synth->oscParams[1]->detuneValue.setValue (0.18f, &um);
            synth->oscParams[1]->spreadValue.setValue (70.0f, &um);
            synth->oscParams[1]->levelValue.setValue (-7.0f, &um);
            synth->filterTypeValue.setValue (1, &um);
            synth->filterSlopeValue.setValue (12, &um);
            synth->filterFreqValue.setValue (112.0f, &um);
            synth->filterResonanceValue.setValue (15.0f, &um);
            synth->delayOnValue.setValue (true, &um);
            synth->delayValue.setValue (0.75f, &um);
            synth->delayFeedbackValue.setValue (-9.0f, &um);
            synth->delayMixValue.setValue (0.22f, &um);
            synth->reverbOnValue.setValue (true, &um);
            synth->reverbSizeValue.setValue (0.60f, &um);
            synth->reverbDampingValue.setValue (0.35f, &um);
            synth->reverbWidthValue.setValue (0.90f, &um);
            synth->reverbMixValue.setValue (0.24f, &um);
        }
        else if (trimmed == "Acid 303 Squelch")
        {
            synth->voiceModeValue.setValue (0, &um); // Mono
            synth->legatoValue.setValue (35.0f, &um);
            synth->oscParams[0]->waveShapeValue.setValue (3, &um); // Saw
            synth->oscParams[0]->levelValue.setValue (0.0f, &um);
            synth->oscParams[1]->waveShapeValue.setValue (2, &um); // Square
            synth->oscParams[1]->tuneValue.setValue (-12.0f, &um);
            synth->oscParams[1]->pulseWidthValue.setValue (0.35f, &um);
            synth->oscParams[1]->levelValue.setValue (-8.0f, &um);
            synth->filterTypeValue.setValue (1, &um);
            synth->filterSlopeValue.setValue (24, &um);
            synth->filterFreqValue.setValue (52.0f, &um);
            synth->filterResonanceValue.setValue (72.0f, &um);
            synth->filterAmountValue.setValue (0.65f, &um);
            synth->filterAttackValue.setValue (0.002f, &um);
            synth->filterDecayValue.setValue (0.22f, &um);
            synth->filterSustainValue.setValue (10.0f, &um);
            synth->distortionOnValue.setValue (true, &um);
            synth->distortionValue.setValue (0.38f, &um);
        }
        else if (trimmed == "Pluck Neon Arp")
        {
            synth->voiceModeValue.setValue (2, &um);
            synth->oscParams[0]->waveShapeValue.setValue (2, &um); // Square
            synth->oscParams[0]->pulseWidthValue.setValue (0.28f, &um);
            synth->oscParams[0]->levelValue.setValue (0.0f, &um);
            synth->oscParams[1]->waveShapeValue.setValue (3, &um); // Saw
            synth->oscParams[1]->fineTuneValue.setValue (8.0f, &um);
            synth->oscParams[1]->levelValue.setValue (-4.0f, &um);
            synth->ampAttackValue.setValue (0.002f, &um);
            synth->ampDecayValue.setValue (0.28f, &um);
            synth->ampSustainValue.setValue (0.0f, &um);
            synth->ampReleaseValue.setValue (0.18f, &um);
            synth->filterTypeValue.setValue (1, &um);
            synth->filterSlopeValue.setValue (24, &um);
            synth->filterFreqValue.setValue (62.0f, &um);
            synth->filterResonanceValue.setValue (28.0f, &um);
            synth->filterAmountValue.setValue (0.55f, &um);
            synth->filterDecayValue.setValue (0.20f, &um);
            synth->filterSustainValue.setValue (0.0f, &um);
            synth->delayOnValue.setValue (true, &um);
            synth->delayValue.setValue (0.50f, &um);
            synth->delayFeedbackValue.setValue (-8.0f, &um);
            synth->delayMixValue.setValue (0.28f, &um);
            synth->reverbOnValue.setValue (true, &um);
            synth->reverbSizeValue.setValue (0.50f, &um);
            synth->reverbMixValue.setValue (0.20f, &um);
        }
        else if (trimmed == "Lush Cyberpunk Pad")
        {
            synth->voiceModeValue.setValue (2, &um);
            synth->oscParams[0]->waveShapeValue.setValue (3, &um); // Saw
            synth->oscParams[0]->voicesValue.setValue (4, &um);
            synth->oscParams[0]->detuneValue.setValue (0.18f, &um);
            synth->oscParams[0]->spreadValue.setValue (80.0f, &um);
            synth->oscParams[0]->levelValue.setValue (-2.0f, &um);
            synth->oscParams[1]->waveShapeValue.setValue (4, &um); // Triangle
            synth->oscParams[1]->tuneValue.setValue (12.0f, &um);
            synth->oscParams[1]->voicesValue.setValue (2, &um);
            synth->oscParams[1]->detuneValue.setValue (0.10f, &um);
            synth->oscParams[1]->levelValue.setValue (-6.0f, &um);
            synth->ampAttackValue.setValue (0.65f, &um);
            synth->ampDecayValue.setValue (0.80f, &um);
            synth->ampSustainValue.setValue (85.0f, &um);
            synth->ampReleaseValue.setValue (1.20f, &um);
            synth->filterTypeValue.setValue (1, &um);
            synth->filterSlopeValue.setValue (12, &um);
            synth->filterFreqValue.setValue (82.0f, &um);
            synth->filterResonanceValue.setValue (20.0f, &um);
            synth->chorusOnValue.setValue (true, &um);
            synth->chorusSpeedValue.setValue (0.60f, &um);
            synth->chorusDepthValue.setValue (6.0f, &um);
            synth->chorusWidthValue.setValue (0.90f, &um);
            synth->chorusMixValue.setValue (0.38f, &um);
            synth->reverbOnValue.setValue (true, &um);
            synth->reverbSizeValue.setValue (0.82f, &um);
            synth->reverbDampingValue.setValue (0.25f, &um);
            synth->reverbWidthValue.setValue (1.0f, &um);
            synth->reverbMixValue.setValue (0.42f, &um);
        }
        else if (trimmed == "Crystal Bell Keys")
        {
            synth->voiceModeValue.setValue (2, &um);
            synth->oscParams[0]->waveShapeValue.setValue (1, &um); // Sine
            synth->oscParams[0]->levelValue.setValue (0.0f, &um);
            synth->oscParams[1]->waveShapeValue.setValue (4, &um); // Triangle
            synth->oscParams[1]->tuneValue.setValue (19.0f, &um);
            synth->oscParams[1]->levelValue.setValue (-8.0f, &um);
            synth->oscParams[2]->waveShapeValue.setValue (1, &um); // Sine harmonic
            synth->oscParams[2]->tuneValue.setValue (24.0f, &um);
            synth->oscParams[2]->levelValue.setValue (-11.0f, &um);
            synth->ampAttackValue.setValue (0.002f, &um);
            synth->ampDecayValue.setValue (0.95f, &um);
            synth->ampSustainValue.setValue (15.0f, &um);
            synth->ampReleaseValue.setValue (0.65f, &um);
            synth->chorusOnValue.setValue (true, &um);
            synth->chorusMixValue.setValue (0.20f, &um);
            synth->reverbOnValue.setValue (true, &um);
            synth->reverbSizeValue.setValue (0.65f, &um);
            synth->reverbMixValue.setValue (0.30f, &um);
        }

        refreshAllAttachedParams (*synth);
        synth->edit.pluginChanged (*synth);
        return true;
    }

    if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (&plugin))
    {
        if (trimmed == "Neon 808 + Synth Kit")
            return seedStarterSamplerKit (*sampler);
        return false;
    }

    if (auto* wobble = dynamic_cast<CvWobbleFilterPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Sub Wobble (450 Hz)")
        {
            setParamIfPresent (*wobble, "cutoffHz", 450.0f);
            setParamIfPresent (*wobble, "resonance", 2.2f);
            setParamIfPresent (*wobble, "wobbleDepth", 0.65f);
            return true;
        }
        if (trimmed == "Mid Growl (900 Hz)")
        {
            setParamIfPresent (*wobble, "cutoffHz", 900.0f);
            setParamIfPresent (*wobble, "resonance", 3.5f);
            setParamIfPresent (*wobble, "wobbleDepth", 0.55f);
            return true;
        }
        if (trimmed == "Screamer Sweep (1.8 kHz)")
        {
            setParamIfPresent (*wobble, "cutoffHz", 1800.0f);
            setParamIfPresent (*wobble, "resonance", 4.8f);
            setParamIfPresent (*wobble, "wobbleDepth", 0.75f);
            return true;
        }
        return false;
    }

    if (auto* lfo = dynamic_cast<CvLfoPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Slow Sweep (0.25 Hz)")
        {
            setParamIfPresent (*lfo, "rate", 0.25f);
            setParamIfPresent (*lfo, "depth", 0.85f);
            return true;
        }
        if (trimmed == "Quarter Wobble (2 Hz)")
        {
            setParamIfPresent (*lfo, "rate", 2.0f);
            setParamIfPresent (*lfo, "depth", 0.90f);
            return true;
        }
        if (trimmed == "Fast Flutter (6 Hz)")
        {
            setParamIfPresent (*lfo, "rate", 6.0f);
            setParamIfPresent (*lfo, "depth", 0.75f);
            return true;
        }
        return false;
    }

    if (auto* rev = dynamic_cast<te::ReverbPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Tight Room")
        {
            setParamIfPresent (*rev, "room size", 0.28f);
            setParamIfPresent (*rev, "damping", 0.65f);
            setParamIfPresent (*rev, "wet level", 0.22f);
            setParamIfPresent (*rev, "dry level", 0.85f);
            setParamIfPresent (*rev, "width", 0.75f);
            return true;
        }
        if (trimmed == "Cyber Hall")
        {
            setParamIfPresent (*rev, "room size", 0.68f);
            setParamIfPresent (*rev, "damping", 0.35f);
            setParamIfPresent (*rev, "wet level", 0.36f);
            setParamIfPresent (*rev, "dry level", 0.75f);
            setParamIfPresent (*rev, "width", 0.95f);
            return true;
        }
        if (trimmed == "Ambient Wash")
        {
            setParamIfPresent (*rev, "room size", 0.92f);
            setParamIfPresent (*rev, "damping", 0.22f);
            setParamIfPresent (*rev, "wet level", 0.58f);
            setParamIfPresent (*rev, "dry level", 0.55f);
            setParamIfPresent (*rev, "width", 1.0f);
            return true;
        }
        return false;
    }

    if (auto* dly = dynamic_cast<te::DelayPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Slapback")
        {
            dly->lengthMs.setValue (110, &um);
            setParamIfPresent (*dly, "feedback", -14.0f);
            setParamIfPresent (*dly, "mix proportion", 0.28f);
            return true;
        }
        if (trimmed == "Dub Echo")
        {
            dly->lengthMs.setValue (360, &um);
            setParamIfPresent (*dly, "feedback", -5.5f);
            setParamIfPresent (*dly, "mix proportion", 0.42f);
            return true;
        }
        return false;
    }

    if (auto* comp = dynamic_cast<te::CompressorPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Punchy Bus")
        {
            setParamIfPresent (*comp, "threshold", 0.32f);
            setParamIfPresent (*comp, "ratio", 0.30f);
            setParamIfPresent (*comp, "attack", 18.0f);
            setParamIfPresent (*comp, "release", 110.0f);
            setParamIfPresent (*comp, "output gain", 2.0f);
            return true;
        }
        if (trimmed == "Tight Limiter")
        {
            setParamIfPresent (*comp, "threshold", 0.55f);
            setParamIfPresent (*comp, "ratio", 0.12f);
            setParamIfPresent (*comp, "attack", 2.0f);
            setParamIfPresent (*comp, "release", 60.0f);
            setParamIfPresent (*comp, "output gain", 3.5f);
            return true;
        }
        return false;
    }

    if (auto* chorus = dynamic_cast<te::ChorusPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Wide Ensemble")
        {
            chorus->depthMs.setValue (6.5f, &um);
            chorus->speedHz.setValue (0.85f, &um);
            chorus->width.setValue (0.90f, &um);
            chorus->mixProportion.setValue (0.45f, &um);
            return true;
        }
        if (trimmed == "Subtle Shimmer")
        {
            chorus->depthMs.setValue (2.8f, &um);
            chorus->speedHz.setValue (1.35f, &um);
            chorus->width.setValue (0.65f, &um);
            chorus->mixProportion.setValue (0.24f, &um);
            return true;
        }
        return false;
    }

    if (auto* phaser = dynamic_cast<te::PhaserPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Slow Jet")
        {
            phaser->depth.setValue (6.5f, &um);
            phaser->rate.setValue (0.25f, &um);
            phaser->feedbackGain.setValue (0.72f, &um);
            return true;
        }
        if (trimmed == "Deep Phase")
        {
            phaser->depth.setValue (9.0f, &um);
            phaser->rate.setValue (0.95f, &um);
            phaser->feedbackGain.setValue (0.85f, &um);
            return true;
        }
        return false;
    }

    if (auto* tr808 = dynamic_cast<Tr808DrumPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Classic 808 Boom")
        {
            setParamIfPresent (*tr808, "kickDecay", 0.48f);
            setParamIfPresent (*tr808, "kickTone", 50.0f);
            setParamIfPresent (*tr808, "snareSnappy", 0.65f);
            setParamIfPresent (*tr808, "hatDecay", 0.14f);
            setParamIfPresent (*tr808, "accent", 0.60f);
            return true;
        }
        if (trimmed == "Sub Shaker 808")
        {
            setParamIfPresent (*tr808, "kickDecay", 0.95f);
            setParamIfPresent (*tr808, "kickTone", 42.0f);
            setParamIfPresent (*tr808, "snareSnappy", 0.78f);
            setParamIfPresent (*tr808, "hatDecay", 0.10f);
            setParamIfPresent (*tr808, "accent", 0.80f);
            return true;
        }
        if (trimmed == "Tight Electro 808")
        {
            setParamIfPresent (*tr808, "kickDecay", 0.22f);
            setParamIfPresent (*tr808, "kickTone", 64.0f);
            setParamIfPresent (*tr808, "snareSnappy", 0.50f);
            setParamIfPresent (*tr808, "hatDecay", 0.08f);
            setParamIfPresent (*tr808, "accent", 0.45f);
            return true;
        }
        return false;
    }

    if (auto* vintage = dynamic_cast<VintageSamplerPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "SP-1200 Grit (12-Bit / 26 kHz)")
        {
            setParamIfPresent (*vintage, "bitDepth", 12.0f);
            setParamIfPresent (*vintage, "targetRate", 26040.0f);
            setParamIfPresent (*vintage, "filterCutoff", 12500.0f);
            setParamIfPresent (*vintage, "gain", 0.90f);
            return true;
        }
        if (trimmed == "Lo-Fi Crunch (8-Bit / 12 kHz)")
        {
            setParamIfPresent (*vintage, "bitDepth", 8.0f);
            setParamIfPresent (*vintage, "targetRate", 12000.0f);
            setParamIfPresent (*vintage, "filterCutoff", 6500.0f);
            setParamIfPresent (*vintage, "gain", 0.95f);
            return true;
        }
        if (trimmed == "Pure ZOH Staircase")
        {
            setParamIfPresent (*vintage, "bitDepth", 10.0f);
            setParamIfPresent (*vintage, "targetRate", 14000.0f);
            setParamIfPresent (*vintage, "filterCutoff", 18000.0f);
            setParamIfPresent (*vintage, "gain", 0.85f);
            return true;
        }
        return false;
    }

    if (auto* tape = dynamic_cast<AnalogTapePlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "Warm Master Glue")
        {
            setParamIfPresent (*tape, "drive", 1.8f);
            setParamIfPresent (*tape, "wow", 0.10f);
            setParamIfPresent (*tape, "flutter", 0.08f);
            setParamIfPresent (*tape, "bias", 0.08f);
            setParamIfPresent (*tape, "mix", 0.65f);
            return true;
        }
        if (trimmed == "Lo-Fi Cassette")
        {
            setParamIfPresent (*tape, "drive", 3.6f);
            setParamIfPresent (*tape, "wow", 0.55f);
            setParamIfPresent (*tape, "flutter", 0.38f);
            setParamIfPresent (*tape, "bias", 0.18f);
            setParamIfPresent (*tape, "mix", 1.0f);
            return true;
        }
        if (trimmed == "Hot Driven Reel")
        {
            setParamIfPresent (*tape, "drive", 6.2f);
            setParamIfPresent (*tape, "wow", 0.20f);
            setParamIfPresent (*tape, "flutter", 0.15f);
            setParamIfPresent (*tape, "bias", 0.22f);
            setParamIfPresent (*tape, "mix", 0.85f);
            return true;
        }
        return false;
    }

    if (auto* comp = dynamic_cast<SidechainCompressorPlugin*> (&plugin))
    {
        um.beginNewTransaction ("Load Preset: " + trimmed);
        if (trimmed == "EDM Pump")
        {
            setParamIfPresent (*comp, "thresholdDb", -22.0f);
            setParamIfPresent (*comp, "ratio", 8.0f);
            setParamIfPresent (*comp, "attackMs", 2.0f);
            setParamIfPresent (*comp, "releaseMs", 90.0f);
            setParamIfPresent (*comp, "makeupDb", 3.0f);
            setParamIfPresent (*comp, "mix", 1.0f);
            return true;
        }
        if (trimmed == "Vocal Duck")
        {
            setParamIfPresent (*comp, "thresholdDb", -30.0f);
            setParamIfPresent (*comp, "ratio", 4.0f);
            setParamIfPresent (*comp, "attackMs", 10.0f);
            setParamIfPresent (*comp, "releaseMs", 220.0f);
            setParamIfPresent (*comp, "makeupDb", 0.0f);
            setParamIfPresent (*comp, "mix", 1.0f);
            return true;
        }
        if (trimmed == "Glue Bus")
        {
            setParamIfPresent (*comp, "thresholdDb", -14.0f);
            setParamIfPresent (*comp, "ratio", 2.5f);
            setParamIfPresent (*comp, "attackMs", 20.0f);
            setParamIfPresent (*comp, "releaseMs", 180.0f);
            setParamIfPresent (*comp, "makeupDb", 1.5f);
            setParamIfPresent (*comp, "mix", 0.75f);
            return true;
        }
        return false;
    }

    return false;
}

juce::Array<juce::File> PluginPresetManager::listUserPresetFiles (te::Plugin& plugin)
{
    const auto dir = getUserPresetsDirectory (plugin.getPluginType());
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.trkpreset");

    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileNameWithoutExtension().compareIgnoreCase (b.getFileNameWithoutExtension()) < 0;
    });

    return files;
}

bool PluginPresetManager::savePresetToFile (te::Plugin& plugin, const juce::File& targetFile)
{
    if (targetFile == juce::File() || targetFile.isDirectory())
        return false;

    plugin.flushPluginStateToValueTree();
    auto snapshot = plugin.state.createCopy();

    // Strip instance-specific properties so presets are portable across tracks/racks.
    snapshot.removeProperty (te::IDs::id, nullptr);
    snapshot.removeProperty ("windowX", nullptr);
    snapshot.removeProperty ("windowY", nullptr);
    snapshot.removeProperty ("windowLocked", nullptr);
    snapshot.setProperty (te::IDs::type, plugin.getPluginType(), nullptr);

    const juce::Identifier paramsTag ("STUDIO_PARAMS");
    if (auto existingParams = snapshot.getChildWithName (paramsTag); existingParams.isValid())
        snapshot.removeChild (existingParams, nullptr);

    juce::ValueTree paramsChild (paramsTag);
    for (auto* p : plugin.getAutomatableParameters())
    {
        juce::ValueTree paramNode ("PARAM");
        paramNode.setProperty ("id", p->paramID, nullptr);
        paramNode.setProperty ("value", p->getCurrentValue(), nullptr);
        paramsChild.addChild (paramNode, -1, nullptr);
    }
    snapshot.addChild (paramsChild, -1, nullptr);

    auto xml = snapshot.createXml();
    if (xml == nullptr)
        return false;

    targetFile.getParentDirectory().createDirectory();
    return xml->writeTo (targetFile);
}

bool PluginPresetManager::loadPresetFromFile (te::Plugin& plugin, const juce::File& sourceFile)
{
    if (! sourceFile.existsAsFile())
        return false;

    auto xml = juce::XmlDocument::parse (sourceFile);
    if (xml == nullptr)
        return false;

    const auto vt = juce::ValueTree::fromXml (*xml);
    if (! vt.isValid())
        return false;

    const auto presetType = vt.getProperty (te::IDs::type).toString();
    if (presetType.isNotEmpty() && presetType != plugin.getPluginType())
        return false;

    auto& um = plugin.edit.getUndoManager();
    um.beginNewTransaction ("Load Preset File: " + sourceFile.getFileNameWithoutExtension());

    const juce::var preservedID = plugin.state.getProperty (te::IDs::id);
    const juce::Identifier paramsTag ("STUDIO_PARAMS");
    auto vtClean = vt.createCopy();
    const auto paramsChild = vtClean.getChildWithName (paramsTag).createCopy();
    if (auto toRemove = vtClean.getChildWithName (paramsTag); toRemove.isValid())
        vtClean.removeChild (toRemove, nullptr);

    for (int i = 0; i < vtClean.getNumProperties(); ++i)
    {
        const auto propName = vtClean.getPropertyName (i);
        if (propName == te::IDs::id || propName == juce::Identifier ("windowX")
            || propName == juce::Identifier ("windowY") || propName == juce::Identifier ("windowLocked"))
            continue;

        plugin.state.setProperty (propName, vtClean.getProperty (propName), &um);
    }

    plugin.restorePluginStateFromValueTree (vtClean);
    if (preservedID.isVoid() == false)
        plugin.state.setProperty (te::IDs::id, preservedID, nullptr);

    if (paramsChild.isValid())
    {
        for (int i = 0; i < paramsChild.getNumChildren(); ++i)
        {
            const auto node = paramsChild.getChild (i);
            const auto pId  = node.getProperty ("id").toString();
            const float val = static_cast<float> (static_cast<double> (node.getProperty ("value", 0.0)));
            if (auto p = plugin.getAutomatableParameterByID (pId))
                p->setParameter (val, juce::sendNotification);
        }
    }

    refreshAllAttachedParams (plugin);
    plugin.edit.pluginChanged (plugin);
    return true;
}

bool PluginPresetManager::seedStarterSamplerKit (te::SamplerPlugin& sampler, const juce::File& kitDir)
{
    const auto targetDir = (kitDir != juce::File()) ? kitDir : getDefaultSamplerKitDirectory();
    targetDir.createDirectory();

    struct KitSampleSpec
    {
        const char* fileName;
        const char* soundName;
        double durationSec;
        int kind;
        int keyNote;
        int minNote;
        int maxNote;
        bool openEnded;
    };

    static constexpr KitSampleSpec specs[] = {
        { "Kick_808.wav",      "808 Kick",      0.45, 0, 36, 24, 37, true  },
        { "Snare_Crisp.wav",   "Crisp Snare",   0.30, 1, 38, 38, 41, true  },
        { "Hat_Closed.wav",    "Closed Hat",    0.16, 2, 42, 42, 47, true  },
        { "Synth_Stab_C4.wav", "Neon Synth C4", 0.60, 3, 60, 48, 96, false }
    };

    auto& um = sampler.edit.getUndoManager();
    um.beginNewTransaction ("Load Starter Sampler Kit");

    while (sampler.getNumSounds() > 0)
        sampler.removeSound (0);

    for (const auto& s : specs)
    {
        const auto wavFile = targetDir.getChildFile (s.fileName);
        if (! wavFile.existsAsFile() || wavFile.getSize() <= 44)
        {
            if (! writeStarterWav (wavFile, s.durationSec, s.kind))
                return false;
        }

        const auto err = sampler.addSound (wavFile.getFullPathName(), s.soundName, 0.0, s.durationSec, 0.0f);
        if (err.isNotEmpty())
            return false;

        const int idx = sampler.getNumSounds() - 1;
        sampler.setSoundParams (idx, s.keyNote, s.minNote, s.maxNote);
        sampler.setSoundGains (idx, 0.0f, 0.0f);
        sampler.setSoundOpenEnded (idx, s.openEnded);
    }

    sampler.flushPendingUpdates();
    sampler.sourceMediaChanged();
    sampler.edit.pluginChanged (sampler);
    return sampler.getNumSounds() == 4;
}

bool PluginPresetManager::addSampleFileToSampler (te::SamplerPlugin& sampler,
                                                  const juce::File& audioFile,
                                                  int rootNote,
                                                  int minNote,
                                                  int maxNote,
                                                  bool openEnded)
{
    if (! audioFile.existsAsFile())
        return false;

    double durationSec = 1.0;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    if (auto reader = std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (audioFile)))
    {
        if (reader->sampleRate > 0.0 && reader->lengthInSamples > 0)
            durationSec = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
    }

    auto& um = sampler.edit.getUndoManager();
    um.beginNewTransaction ("Add Sample to Sampler");

    const auto err = sampler.addSound (audioFile.getFullPathName(),
                                       audioFile.getFileNameWithoutExtension(),
                                       0.0,
                                       durationSec,
                                       0.0f);
    if (err.isNotEmpty())
        return false;

    const int idx = sampler.getNumSounds() - 1;
    sampler.setSoundParams (idx, rootNote, minNote, maxNote);
    sampler.setSoundOpenEnded (idx, openEnded);
    sampler.flushPendingUpdates();
    sampler.sourceMediaChanged();
    sampler.edit.pluginChanged (sampler);
    return true;
}
