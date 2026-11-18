#include "BgmGeneratorComponent.h"

BgmGeneratorComponent::BgmGeneratorComponent (RiffLibrary& targetLibrary)
    : library (targetLibrary)
{
    setOpaque (true);

    addAndMakeVisible (titleLabel);
    titleLabel.setText ("RIFF BGM GENERATOR", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff38bdf8));

    addAndMakeVisible (genreBox);
    genreBox.addItem ("Genre: Cyberpunk", 1);
    genreBox.addItem ("Genre: Synthwave", 2);
    genreBox.setSelectedId (1, juce::dontSendNotification);

    addAndMakeVisible (keyBox);
    keyBox.addItem ("Key: Am", 1);
    keyBox.addItem ("Key: Cm", 2);
    keyBox.addItem ("Key: Dm", 3);
    keyBox.addItem ("Key: Em", 4);
    keyBox.addItem ("Key: Gm", 5);
    keyBox.setSelectedId (1, juce::dontSendNotification);

    addAndMakeVisible (energyBox);
    energyBox.addItem ("Energy: Chill (2)", 2);
    energyBox.addItem ("Energy: Groove (3)", 3);
    energyBox.addItem ("Energy: Peak (4)", 4);
    energyBox.addItem ("Energy: Max (5)", 5);
    energyBox.setSelectedId (4, juce::dontSendNotification);

    addAndMakeVisible (barsBox);
    barsBox.addItem ("8 Bars", 8);
    barsBox.addItem ("16 Bars", 16);
    barsBox.setSelectedId (8, juce::dontSendNotification);

    addAndMakeVisible (generateButton);
    generateButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0284c7));
    generateButton.onClick = [this] { triggerGenerateBgm(); };

    addAndMakeVisible (rescanButton);
    rescanButton.onClick = [this]
    {
        library.ensureStarterPack (RiffLibrary::getDefaultLibraryDirectory());
    };
}

void BgmGeneratorComponent::triggerGenerateBgm()
{
    if (edit == nullptr)
        return;

    if (library.getRiffs().empty())
        library.ensureStarterPack (RiffLibrary::getDefaultLibraryDirectory());

    BgmRequest req;

    req.genre            = (genreBox.getSelectedId() == 2) ? "Synthwave" : "Cyberpunk";
    req.targetKeyLabel   = keyBox.getText().fromFirstOccurrenceOf ("Key: ", false, false).trim();
    req.targetPitchClass = RiffLibrary::parseKeyPitchClass (req.targetKeyLabel);
    req.targetEnergy     = juce::jlimit (1, 5, energyBox.getSelectedId());
    req.totalBars        = (barsBox.getSelectedId() == 16) ? 16 : 8;

    edit->getUndoManager().beginNewTransaction ("Generate BGM Arrangement");

    const int created = BgmArrangementSolver::generateArrangement (*edit, library, req);

    if (onBgmGenerated)
        onBgmGenerated (created);
}

void BgmGeneratorComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff161922));

    g.setColour (juce::Colour (0xff2b313e));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, static_cast<float> (getWidth()));
}

void BgmGeneratorComponent::resized()
{
    auto row = getLocalBounds().reduced (10, 4);

    titleLabel.setBounds (row.removeFromLeft (142));
    row.removeFromLeft (8);

    genreBox.setBounds (row.removeFromLeft (145));
    row.removeFromLeft (6);

    keyBox.setBounds (row.removeFromLeft (98));
    row.removeFromLeft (6);

    energyBox.setBounds (row.removeFromLeft (140));
    row.removeFromLeft (6);

    barsBox.setBounds (row.removeFromLeft (88));
    row.removeFromLeft (10);

    generateButton.setBounds (row.removeFromLeft (118));
    row.removeFromLeft (6);

    rescanButton.setBounds (row.removeFromLeft (98));
}
