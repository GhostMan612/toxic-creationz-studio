#include "GameForgeComponent.h"

#include "SfxForgeEngine.h"

namespace tcs
{
namespace game
{
GameForgeComponent::GameForgeComponent()
{
    const auto bold = juce::Font (juce::FontOptions (13.0f).withStyle ("Bold"));
    const auto normal = juce::Font (juce::FontOptions (12.0f));

    titleLabel.setFont (bold);
    forgeLabel.setFont (bold);
    exportLabel.setFont (bold);
    for (auto* label : { &titleLabel, &forgeLabel, &exportLabel })
        label->setColour (juce::Label::textColourId, juce::Colour (0xff5ce6a8));

    for (auto c : { SfxCategory::gunshotTactical, SfxCategory::breachExplosion,
                     SfxCategory::footstepSurface, SfxCategory::uiClick,
                     SfxCategory::meleeImpact, SfxCategory::doorBreach,
                     SfxCategory::sciFiPulse, SfxCategory::ambientDrone })
        categoryCombo.addItem (SfxForgeEngine::categoryName (c), categoryCombo.getNumItems() + 1);
    categoryCombo.setSelectedItemIndex (0, juce::dontSendNotification);

    countSlider.setRange (1.0, 16.0, 1.0);
    countSlider.setValue (6.0, juce::dontSendNotification);
    countSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    countSlider.setTextBoxStyle (juce::Slider::TextEntryBoxPosition::TextBoxRight, false, 44, 20);

    seedEditor.setText ("42");
    dirEditor.setText (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                            .getChildFile ("ToxicCreationzGamePack").getFullPathName());

    for (auto* label : { &categoryLabel, &countLabel, &seedLabel, &dirLabel, &forgeStatus, &exportStatus })
    {
        label->setFont (normal);
        label->setColour (juce::Label::textColourId, juce::Colour (0xffc8d2dd));
    }

    addAndMakeVisible (titleLabel);
    addAndMakeVisible (forgeLabel);
    addAndMakeVisible (categoryLabel);
    addAndMakeVisible (categoryCombo);
    addAndMakeVisible (countLabel);
    addAndMakeVisible (countSlider);
    addAndMakeVisible (seedLabel);
    addAndMakeVisible (seedEditor);
    addAndMakeVisible (randomizeButton);
    addAndMakeVisible (forgeButton);
    addAndMakeVisible (forgeStatus);
    addAndMakeVisible (exportLabel);
    addAndMakeVisible (dirLabel);
    addAndMakeVisible (dirEditor);
    addAndMakeVisible (loopButton);
    addAndMakeVisible (stemsButton);
    addAndMakeVisible (packButton);
    addAndMakeVisible (exportStatus);

    randomizeButton.onClick = [this]
    {
        seedEditor.setText (juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
    };
    forgeButton.onClick     = [this] { forgeVariations(); };
    loopButton.onClick      = [this] { exportLoop(); };
    stemsButton.onClick     = [this] { exportStems(); };
    packButton.onClick      = [this] { exportPack(); };
}

GameForgeComponent::~GameForgeComponent()
{
    randomizeButton.onClick = nullptr;
    forgeButton.onClick     = nullptr;
    loopButton.onClick      = nullptr;
    stemsButton.onClick     = nullptr;
    packButton.onClick      = nullptr;
}

void GameForgeComponent::resized()
{
    auto row = getLocalBounds().reduced (8);
    titleLabel.setBounds (row.removeFromTop (20));
    row.removeFromTop (2);

    forgeLabel.setBounds (row.removeFromTop (18));
    auto forgeRow = row.removeFromTop (26);
    categoryLabel.setBounds (forgeRow.removeFromLeft (72));
    categoryCombo.setBounds (forgeRow.removeFromLeft (132));
    forgeRow.removeFromLeft (8);
    countLabel.setBounds (forgeRow.removeFromLeft (72));
    countSlider.setBounds (forgeRow.removeFromLeft (126));
    forgeRow.removeFromLeft (8);
    seedLabel.setBounds (forgeRow.removeFromLeft (36));
    seedEditor.setBounds (forgeRow.removeFromLeft (66));
    randomizeButton.setBounds (forgeRow.removeFromLeft (84));
    forgeRow.removeFromLeft (8);
    forgeButton.setBounds (forgeRow.removeFromLeft (130));
    forgeStatus.setBounds (forgeRow);
    row.removeFromTop (4);

    exportLabel.setBounds (row.removeFromTop (18));
    auto dirRow = row.removeFromTop (24);
    dirLabel.setBounds (dirRow.removeFromLeft (72));
    dirEditor.setBounds (dirRow);
    row.removeFromTop (4);

    auto buttonRow = row.removeFromTop (26);
    loopButton.setBounds (buttonRow.removeFromLeft (152));
    buttonRow.removeFromLeft (6);
    stemsButton.setBounds (buttonRow.removeFromLeft (158));
    buttonRow.removeFromLeft (6);
    packButton.setBounds (buttonRow.removeFromLeft (176));
    exportStatus.setBounds (buttonRow);
}

void GameForgeComponent::setStatus (const juce::String& text)
{
    forgeStatus.setText (text, juce::dontSendNotification);
}

void GameForgeComponent::setExportDirectory (const juce::File& dir)
{
    dirEditor.setText (dir.getFullPathName());
}

static juce::String makeParams (const std::initializer_list<std::pair<juce::String, juce::var>>& entries)
{
    auto* object = new juce::DynamicObject();
    for (const auto& entry : entries)
        object->setProperty (entry.first, entry.second);
    return juce::JSON::toString (juce::var (object), true);
}

void GameForgeComponent::forgeVariations()
{
    if (onAction == nullptr)
        return;
    const auto seed = juce::jlimit (0, 1000000000, static_cast<int> (seedEditor.getText().getLargeIntValue()));
    onAction ("game.forge_sfx_batch", makeParams ({
        { "category", categoryCombo.getText().trim() },
        { "count", static_cast<int> (countSlider.getValue()) },
        { "seed", seed },
        { "prefix", categoryCombo.getText().trim().replaceCharacter (' ', '_') },
        { "duration_sec", 0.5 },
        { "peak_db", -1.0 },
    }));
    setStatus ("Forged " + juce::String (static_cast<int> (countSlider.getValue())) + " takes");
}

void GameForgeComponent::exportLoop()
{
    if (onAction == nullptr)
        return;
    onAction ("audio.export_seamless_loop", makeParams ({
        { "start_beat", 0.0 },
        { "end_beat", 8.0 },
        { "output_path", dirEditor.getText().trim() + "/bgm/seamless_loop.wav" },
        { "tail_seconds", 2.0 },
        { "target_lufs", -14.0 },
    }));
    exportStatus.setText ("Loop exported", juce::dontSendNotification);
}

void GameForgeComponent::exportStems()
{
    if (onAction == nullptr)
        return;
    auto* layersObj = new juce::DynamicObject();
    layersObj->setProperty ("calm", juce::var (0));
    layersObj->setProperty ("combat", juce::var (1));
    onAction ("game.export_adaptive_stems", makeParams ({
        { "start_beat", 0.0 },
        { "end_beat", 8.0 },
        { "output_dir", dirEditor.getText().trim() },
        { "layers", juce::var (layersObj) },
        { "target_lufs", -14.0 },
    }));
    exportStatus.setText ("Stems exported", juce::dontSendNotification);
}

void GameForgeComponent::exportPack()
{
    if (onAction == nullptr)
        return;
    onAction ("game.export_godot_pack", makeParams ({
        { "output_dir", dirEditor.getText().trim() },
        { "include_manifest", true },
    }));
    exportStatus.setText ("Godot pack exported", juce::dontSendNotification);
}
} // namespace game
} // namespace tcs
