#include "RpgSoundboardComponent.h"
#include "RpgSoundboardPlugin.h"

namespace
{
juce::String paramsJson (const std::initializer_list<std::pair<juce::String, juce::var>>& entries)
{
    auto* object = new juce::DynamicObject();
    for (const auto& entry : entries)
        object->setProperty (entry.first, entry.second);
    return juce::JSON::toString (juce::var (object), true);
}
}

RpgSoundboardComponent::RpgSoundboardComponent()
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    const auto heading = juce::Font (juce::FontOptions (12.0f).withStyle ("Bold"));
    titleLabel.setFont (heading);
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc084fc));
    biomeLabel.setFont (heading);
    moodLabel.setFont (heading);
    voiceLabel.setFont (heading);
    padsLabel.setFont (heading);

    const char* biomeTitles[6] = {
        "Drowned Stars", "Rainy Tavern", "Haunted Crypt",
        "Stormy Sea", "Ancient Forest", "Boss Altar"
    };
    for (int i = 0; i < 6; ++i)
    {
        biomeButtons[i].setButtonText (biomeTitles[i]);
        biomeButtons[i].onClick = [this, i] { setBiome (RpgSoundboardPlugin::biomeNames()[i]); };
        addAndMakeVisible (biomeButtons[i]);
    }

    moodSlider.setRange (0.0, 1.0, 0.01);
    moodSlider.setValue (0.35, juce::dontSendNotification);
    moodSlider.onValueChange = [this]
    {
        setBiome (RpgSoundboardPlugin::biomeNames()[activeBiome]);
    };
    ambientSlider.setRange (0.0, 1.0, 0.01);
    ambientSlider.setValue (0.75, juce::dontSendNotification);
    sfxSlider.setRange (0.0, 1.0, 0.01);
    sfxSlider.setValue (0.85, juce::dontSendNotification);

    const char* padTitles[10] = {
        "Sword Clash", "Fireball", "Eldritch Whisper", "Dragon Roar", "Stone Door",
        "Critical Hit", "Healing Chime", "Thunder", "Bow Release", "Dice Roll"
    };
    for (int i = 0; i < 10; ++i)
    {
        padButtons[i].setButtonText (padTitles[i]);
        padButtons[i].onClick = [this, i]
        {
            triggerPad (i, RpgSoundboardPlugin::sfxNames()[i]);
        };
        addAndMakeVisible (padButtons[i]);
    }

    for (int i = 0; i < 6; ++i)
    {
        customButtons[i].setButtonText ("Custom " + juce::String (i + 1));
        customButtons[i].onClick = [this, i] { chooseCustomPad (i); };
        addAndMakeVisible (customButtons[i]);
    }

    const char* voiceTitles[4] = {
        "Normal GM", "Eldritch / Monster", "Radio / Telepathy", "Heroic Narrator"
    };
    const char* voicePresets[4] = {
        "ACX Audiobook", "Deep Trailer / Villain", "Commercial Bright", "Anime / Cartoon Lead"
    };
    for (int i = 0; i < 4; ++i)
    {
        voiceButtons[i].setButtonText (voiceTitles[i]);
        voiceButtons[i].onClick = [this, voicePresets, i] { setVoice (voicePresets[i]); };
        addAndMakeVisible (voiceButtons[i]);
    }

    for (auto* label : { &biomeLabel, &moodLabel, &voiceLabel, &padsLabel, &statusLabel })
    {
        label->setFont (juce::Font (juce::FontOptions (11.0f)));
        label->setColour (juce::Label::textColourId, juce::Colour (0xffc8d2dd));
    }

    addAndMakeVisible (titleLabel);
    addAndMakeVisible (biomeLabel);
    addAndMakeVisible (moodSlider);
    addAndMakeVisible (moodLabel);
    addAndMakeVisible (ambientSlider);
    addAndMakeVisible (sfxSlider);
    addAndMakeVisible (voiceLabel);
    addAndMakeVisible (padsLabel);
    addAndMakeVisible (statusLabel);
}

RpgSoundboardComponent::~RpgSoundboardComponent()
{
    for (auto& button : biomeButtons) button.onClick = nullptr;
    for (auto& button : padButtons) button.onClick = nullptr;
    for (auto& button : customButtons) button.onClick = nullptr;
    for (auto& button : voiceButtons) button.onClick = nullptr;
    moodSlider.onValueChange = nullptr;
    onAction = nullptr;
    customRefs.clear();
    customPadsState = juce::ValueTree();
}

void RpgSoundboardComponent::setEdit (te::Edit* newEdit)
{
    customRefs.clear();
    edit = newEdit;
    if (edit == nullptr)
        return;

    customPadsState = edit->state.getOrCreateChildWithName ("RPG_CUSTOM_PADS", nullptr);
    for (int i = 0; i < 6; ++i)
    {
        auto slot = customPadsState.getOrCreateChildWithName ("PAD_" + juce::String (i + 1), nullptr);
        customRefs.push_back (std::make_unique<te::SourceFileReference> (*edit, slot, juce::Identifier ("source")));
        const auto file = customRefs.back()->getFile();
        if (file.existsAsFile())
            customButtons[i].setButtonText (file.getFileName());
    }
}

void RpgSoundboardComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff161922));
    g.setColour (juce::Colour (0xff2b313e));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, static_cast<float> (getWidth()));
}

void RpgSoundboardComponent::resized()
{
    auto area = getLocalBounds().reduced (8);
    titleLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);

    auto biomeRow = area.removeFromTop (24);
    biomeLabel.setBounds (biomeRow.removeFromLeft (52));
    const int buttonWidth = biomeRow.getWidth() / 6;
    for (auto& button : biomeButtons)
        button.setBounds (biomeRow.removeFromLeft (buttonWidth).reduced (2));

    auto sliderRow = area.removeFromTop (24);
    moodLabel.setBounds (sliderRow.removeFromLeft (92));
    moodSlider.setBounds (sliderRow.removeFromLeft (180));
    sliderRow.removeFromLeft (8);
    ambientSlider.setBounds (sliderRow.removeFromLeft (120));
    sliderRow.removeFromLeft (8);
    sfxSlider.setBounds (sliderRow.removeFromLeft (120));
    area.removeFromTop (2);

    auto voiceRow = area.removeFromTop (24);
    voiceLabel.setBounds (voiceRow.removeFromLeft (52));
    for (auto& button : voiceButtons)
        button.setBounds (voiceRow.removeFromLeft (130).reduced (2));
    area.removeFromTop (2);

    padsLabel.setBounds (area.removeFromTop (16));
    auto padRow = area.removeFromTop (26);
    const int padWidth = padRow.getWidth() / 16;
    for (int i = 0; i < 10; ++i)
        padButtons[i].setBounds (padRow.removeFromLeft (padWidth).reduced (2));
    for (int i = 0; i < 6; ++i)
        customButtons[i].setBounds (padRow.removeFromLeft (padWidth).reduced (2));
    statusLabel.setBounds (area.removeFromTop (16));
}

bool RpgSoundboardComponent::keyPressed (const juce::KeyPress& key)
{
    const int keyNum = key.getKeyCode() - juce::KeyPress::F1Key + 1;
    if (keyNum >= 1 && keyNum <= 10)
    {
        triggerPad (keyNum - 1, RpgSoundboardPlugin::sfxNames()[keyNum - 1]);
        return true;
    }
    return false;
}

void RpgSoundboardComponent::triggerPad (int pad, const juce::String& sfxName)
{
    if (onAction == nullptr)
        return;
    onAction ("rpg.trigger_sfx", paramsJson ({ { "sfx_name", sfxName }, { "velocity", 0.9 } }));
    statusLabel.setText ("Fired " + juce::String (pad + 1) + ": " + sfxName, juce::dontSendNotification);
}

void RpgSoundboardComponent::setBiome (const juce::String& biome)
{
    activeBiome = RpgSoundboardPlugin::parseBiome (biome);
    if (onAction == nullptr)
        return;
    onAction ("rpg.set_scene", paramsJson ({
        { "biome", biome },
        { "mood_intensity", static_cast<double> (moodSlider.getValue()) },
        { "crossfade_sec", 2.0 },
        { "ambient_level", static_cast<double> (ambientSlider.getValue()) },
        { "sfx_level", static_cast<double> (sfxSlider.getValue()) },
    }));
    for (int i = 0; i < 6; ++i)
        biomeButtons[i].setToggleState (i == activeBiome, juce::dontSendNotification);
    statusLabel.setText ("Biome: " + biome, juce::dontSendNotification);
}

void RpgSoundboardComponent::setVoice (const juce::String& preset)
{
    if (onAction == nullptr)
        return;
    onAction ("vo.apply_preset", paramsJson ({ { "preset", preset }, { "track_index", -1 } }));
    statusLabel.setText ("GM Voice: " + preset, juce::dontSendNotification);
}

void RpgSoundboardComponent::chooseCustomPad (int slot)
{
    if (edit == nullptr || slot < 0 || slot >= static_cast<int> (customRefs.size()))
        return;
    auto chooser = std::make_shared<juce::FileChooser> (
        "Choose custom WAV for pad " + juce::String (slot + 1),
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, slot, chooser] (const juce::FileChooser& result)
        {
            const auto file = result.getResult();
            if (file.existsAsFile() && slot < static_cast<int> (customRefs.size()))
            {
                customRefs[static_cast<size_t> (slot)]->setToFile (
                    file, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
                customButtons[slot].setButtonText (file.getFileName());
                statusLabel.setText ("Custom pad " + juce::String (slot + 1) + " assigned",
                                     juce::dontSendNotification);
            }
        });
}
