#include "UiSoundService.h"

#include "SfxForgeEngine.h"

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <cmath>

namespace tcs
{
namespace ui
{
std::atomic<bool> UiSoundService::headless { false };
UiSoundService* UiSoundService::currentInstance = nullptr;

namespace
{
struct SoundSpec
{
    const char* name;
    tcs::game::SfxCategory category;
    double durationSec;
    int seed;
};

const SoundSpec specs[UiSoundService::numSounds] = {
    { "click",   tcs::game::SfxCategory::uiClick,     0.055, 101 },
    { "blip",    tcs::game::SfxCategory::sciFiPulse,  0.080, 202 },
    { "beep",    tcs::game::SfxCategory::sciFiPulse,  0.100, 303 },
    { "success", tcs::game::SfxCategory::sciFiPulse,  0.140, 404 },
    { "back",    tcs::game::SfxCategory::uiClick,     0.070, 505 },
    { "error",   tcs::game::SfxCategory::sciFiPulse,  0.120, 606 },
    { "toggle",  tcs::game::SfxCategory::uiClick,     0.045, 707 }
};

juce::File settingsFile()
{
    if (UiSoundService::isHeadless())
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("tcs_ui_sounds_headless.ini");
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Toxic Creationz Studio").getChildFile ("ui_sounds.ini");
}

int indexOf (const juce::String& name)
{
    for (int i = 0; i < UiSoundService::numSounds; ++i)
        if (name == specs[static_cast<size_t> (i)].name)
            return i;
    return -1;
}
}

UiSoundService::UiSoundService()
{
    currentInstance = this;
    loadSettings();
    renderBank (42);
}

UiSoundService::~UiSoundService()
{
    if (currentInstance == this)
        currentInstance = nullptr;

    for (auto& voice : voices)
    {
        voice.sound.store (-1, std::memory_order_release);
        voice.position.store (0, std::memory_order_release);
    }
}

juce::StringArray UiSoundService::soundNames()
{
    juce::StringArray out;
    for (const auto& spec : specs)
        out.add (spec.name);
    return out;
}

int UiSoundService::durationMs (const juce::String& name)
{
    const int index = indexOf (name);
    return index < 0 ? 0 : static_cast<int> (specs[static_cast<size_t> (index)].durationSec * 1000.0);
}

void UiSoundService::renderBank (int seed)
{
    lastSeed = seed;
    for (int i = 0; i < numSounds; ++i)
    {
        const auto& spec = specs[static_cast<size_t> (i)];
        auto params = tcs::game::SfxRenderParams {};
        params.seed = static_cast<std::uint64_t> (juce::jmax (0, seed)) + static_cast<std::uint64_t> (spec.seed);
        params.durationSec = spec.durationSec;
        params.peakDb = -1.0;
        bank[static_cast<size_t> (i)] = tcs::game::SfxForgeEngine::renderSfx (spec.category, params);
    }
}

void UiSoundService::play (const juce::String& name)
{
    if (! enabled.load (std::memory_order_acquire))
        return;
    const int index = indexOf (name);
    if (index < 0)
        return;
    const int slot = nextVoice.fetch_add (1, std::memory_order_relaxed) % maxVoices;
    auto& voice = voices[static_cast<size_t> (slot)];
    voice.sound.store (index, std::memory_order_release);
    voice.position.store (0, std::memory_order_release);
    voice.velocity.store (1.0f, std::memory_order_release);
}

void UiSoundService::renderCallbackBlock (float* const* outputs, int numChannels, int numSamples) noexcept
{
    if (outputs == nullptr || numChannels <= 0 || numSamples <= 0)
        return;
    for (int ch = 0; ch < numChannels; ++ch)
        if (outputs[ch] != nullptr)
            std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);

    if (! enabled.load (std::memory_order_acquire))
        return;

    const float gain = static_cast<float> (std::pow (10.0,
        juce::jlimit (-60.0, 0.0, volumeDb.load (std::memory_order_relaxed)) / 20.0));

    for (auto& voice : voices)
    {
        const int soundIndex = voice.sound.load (std::memory_order_acquire);
        if (soundIndex < 0 || soundIndex >= numSounds)
            continue;
        const auto& buffer = bank[static_cast<size_t> (soundIndex)];
        if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            const int pos = voice.position.load (std::memory_order_relaxed);
            if (pos >= buffer.getNumSamples())
            {
                voice.sound.store (-1, std::memory_order_release);
                break;
            }
            const float sample = buffer.getSample (0, pos) * gain
                               * voice.velocity.load (std::memory_order_relaxed);
            for (int ch = 0; ch < numChannels; ++ch)
                if (outputs[ch] != nullptr)
                    outputs[ch][i] += sample;
            voice.position.store (pos + 1, std::memory_order_relaxed);
        }
    }
}

void UiSoundService::setEnabled (bool value) noexcept
{
    enabled.store (value, std::memory_order_release);
    saveSettings();
}

void UiSoundService::setVolumeDb (double db) noexcept
{
    volumeDb.store (juce::jlimit (-60.0, 0.0, db), std::memory_order_release);
    saveSettings();
}

void UiSoundService::saveSettings() const
{
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    juce::PropertiesFile file (settingsFile(), options);
    file.setValue ("enabled", enabled.load (std::memory_order_acquire));
    file.setValue ("volume_db", volumeDb.load (std::memory_order_relaxed));
    file.setValue ("seed", lastSeed);
    file.save();
}

void UiSoundService::loadSettings()
{
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    juce::PropertiesFile file (settingsFile(), options);
    enabled.store (file.getBoolValue ("enabled", true), std::memory_order_release);
    volumeDb.store (juce::jlimit (-60.0, 0.0, file.getDoubleValue ("volume_db", -18.0)),
                    std::memory_order_release);
    lastSeed = file.getIntValue ("seed", 42);
}
} // namespace ui
} // namespace tcs
