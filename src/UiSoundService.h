#pragma once

// Desktop-only lock-free UI touch sound service. Never touches te::Edit and
// never writes session state; settings live in an app-level properties file.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>

namespace tcs
{
namespace ui
{
class UiSoundService
{
public:
    static constexpr int numSounds = 7;
    static constexpr int maxVoices = 8;

    UiSoundService();
    ~UiSoundService();

    void renderBank (int seed = 42);
    void play (const juce::String& name);

    void renderCallbackBlock (float* const* outputs, int numChannels, int numSamples) noexcept;

    void setEnabled (bool enabled) noexcept;
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_acquire); }
    void setVolumeDb (double db) noexcept;
    double getVolumeDb() const noexcept { return volumeDb.load (std::memory_order_relaxed); }
    void saveSettings() const;
    void loadSettings();

    static juce::StringArray soundNames();
    static int durationMs (const juce::String& name);
    static void setHeadless (bool value) noexcept { headless.store (value, std::memory_order_release); }
    static bool isHeadless() noexcept { return headless.load (std::memory_order_acquire); }
    static UiSoundService* instance() noexcept { return currentInstance; }

private:
    struct Voice
    {
        std::atomic<int> sound { -1 };
        std::atomic<int> position { 0 };
        std::atomic<float> velocity { 0.0f };
    };

    static std::atomic<bool> headless;
    static UiSoundService* currentInstance;
    std::array<juce::AudioBuffer<float>, numSounds> bank;
    std::array<Voice, maxVoices> voices {};
    std::atomic<bool> enabled { true };
    std::atomic<double> volumeDb { -18.0 };
    std::atomic<int> nextVoice { 0 };
    int lastSeed = 42;
};
} // namespace ui
} // namespace tcs
