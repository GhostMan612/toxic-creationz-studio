#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "RecordingManager.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace te = tracktion::engine;

class MixerComponent : public juce::Component
{
public:
    explicit MixerComponent (te::Engine& targetEngine);
    ~MixerComponent() override;

    void setEdit (te::Edit* newEdit);
    void setRecordingManager (RecordingManager* manager);
    void rebuildStrips();
    void refreshMeters();

    // Insert position for a newly added plugin: just before the terminal
    // VolumeAndPan / LevelMeter utilities, i.e. at the end of the audible
    // insert chain. Inserting at index 0 would put an effect ahead of the
    // instrument it is meant to process.
    static int getInsertIndexForNewPlugin (te::AudioTrack& track);

    std::function<void()> onMixerChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class ChannelStrip : public juce::Component
    {
    public:
        ChannelStrip (MixerComponent& ownerMixer, te::Engine& eng, te::Edit& ed, int index);
        ~ChannelStrip() override;

        void syncState();
        void pollMeter();
        int getMinimumHeight() const;
        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        struct PluginSlotRow
        {
            tracktion::EditItemID pluginID;
            std::unique_ptr<juce::TextButton> openButton;
            std::unique_ptr<juce::TextButton> removeButton;
        };

        te::AudioTrack* resolveTrack() const;
        void attachMeterClient (te::AudioTrack* t);
        void detachMeterClient();
        void applyEqBandChange (int bandIndex, float dbValue);
        void rebuildPluginSlots();
        void showAddPluginMenu();

        MixerComponent& owner;
        te::Engine& engine;
        te::Edit& edit;
        int trackIndex = 0;

        juce::Label titleLabel;
        juce::TextButton muteButton      { "M" };
        juce::TextButton soloButton      { "S" };
        juce::TextButton armButton       { "R" };
        juce::TextButton addPluginButton { "+ Plugin" };
        juce::ComboBox inputSelector;
        juce::Label eqLowLabel           { {}, "Lo" };
        juce::Label eqMidLabel           { {}, "Mid" };
        juce::Label eqHighLabel          { {}, "Hi" };
        juce::Slider eqLowSlider;
        juce::Slider eqMidSlider;
        juce::Slider eqHighSlider;
        juce::Slider panSlider;
        juce::Slider volumeSlider;
        juce::Label dbLabel;

        te::LevelMeasurer::Client meterClient;
        juce::WeakReference<te::LevelMeasurer> attachedMeasurer;
        float meterLevelL = 0.0f;
        float meterLevelR = 0.0f;
        juce::Rectangle<int> meterBounds;

        std::vector<PluginSlotRow> pluginSlots;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
    };

    static constexpr int headerHeight = 26;
    static constexpr int stripWidth   = 168;

    te::Engine& engine;
    te::Edit* edit = nullptr;
    RecordingManager* recordingManager = nullptr;

    juce::Viewport viewport;
    juce::Component stripsContainer;
    std::vector<std::unique_ptr<ChannelStrip>> strips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};
