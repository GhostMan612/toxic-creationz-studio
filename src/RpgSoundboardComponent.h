#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <memory>
#include <vector>

namespace te = tracktion::engine;

/** Desktop-only live tabletop performance dock. All soundboard work is sent
 *  through onAction as JSON-RPC method + params; no plugin Ptrs are retained.
 */
class RpgSoundboardComponent : public juce::Component
{
public:
    RpgSoundboardComponent();
    ~RpgSoundboardComponent() override;

    void setEdit (te::Edit* newEdit);
    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

    std::function<void (const juce::String&, const juce::String&)> onAction;

private:
    void chooseCustomPad (int slot);
    void triggerPad (int pad, const juce::String& sfxName);
    void setBiome (const juce::String& biome);
    void setVoice (const juce::String& preset);

    te::Edit* edit = nullptr;
    int activeBiome = 0;
    juce::ValueTree customPadsState;
    std::vector<std::unique_ptr<te::SourceFileReference>> customRefs;

    juce::Label titleLabel { {}, "LIVE TABLETOP SOUNDBOARD" };
    juce::Label biomeLabel { {}, "Biome" };
    juce::Label moodLabel { {}, "Encounter Mood" };
    juce::Label voiceLabel { {}, "GM Voice" };
    juce::Label padsLabel { {}, "On-the-Spot Pads" };
    juce::Label statusLabel { {}, "Ready" };

    juce::TextButton biomeButtons[6];
    juce::Slider moodSlider { juce::Slider::LinearHorizontal, juce::Slider::TextEntryBoxPosition::NoTextBox };
    juce::Slider ambientSlider { juce::Slider::LinearHorizontal, juce::Slider::TextEntryBoxPosition::NoTextBox };
    juce::Slider sfxSlider { juce::Slider::LinearHorizontal, juce::Slider::TextEntryBoxPosition::NoTextBox };
    juce::TextButton padButtons[10];
    juce::TextButton customButtons[6];
    juce::TextButton voiceButtons[4];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RpgSoundboardComponent)
};
