#include "BgmGeneratorComponent.h"
#include "MusicTheoryEngine.h"

BgmGeneratorComponent::BgmGeneratorComponent (RiffLibrary& targetLibrary)
    : library (targetLibrary)
{
    setOpaque (true);

    addAndMakeVisible (titleLabel);
    titleLabel.setText ("RIFF BGM GENERATOR", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff38bdf8));

    addAndMakeVisible (genreBox);
    int genreId = 1;
    for (const auto& genre : tcs::theory::MusicTheoryEngine::genres())
        genreBox.addItem (genre, genreId++);
    genreBox.setSelectedId (1, juce::dontSendNotification);

    addAndMakeVisible (keyBox);
    int keyId = 1;
    for (const auto& key : tcs::theory::MusicTheoryEngine::keys())
        keyBox.addItem ("Key: " + key, keyId++);
    keyBox.setSelectedId (1, juce::dontSendNotification);

    addAndMakeVisible (scaleBox);
    int scaleId = 1;
    for (const auto& scale : tcs::theory::MusicTheoryEngine::scales())
        scaleBox.addItem (scale, scaleId++);
    genreBox.onChange = [this]
    {
        const auto genre = genreBox.getText().trim();
        const auto signature = tcs::theory::MusicTheoryEngine::genreProfile (genre).signatureScale;
        const int index = tcs::theory::MusicTheoryEngine::scales().indexOf (signature);
        if (index >= 0)
            scaleBox.setSelectedId (index + 1, juce::dontSendNotification);
    };
    scaleBox.setSelectedId (tcs::theory::MusicTheoryEngine::scales().indexOf ("natural_minor") + 1,
                            juce::dontSendNotification);

    addAndMakeVisible (seedBox);
    seedBox.setText ("42");

    addAndMakeVisible (energyBox);
    energyBox.addItem ("Energy: Chill (2)", 2);
    energyBox.addItem ("Energy: Groove (3)", 3);
    energyBox.addItem ("Energy: Peak (4)", 4);
    energyBox.addItem ("Energy: Max (5)", 5);
    energyBox.setSelectedId (4, juce::dontSendNotification);

    addAndMakeVisible (barsBox);
    barsBox.addItem ("8 Bars", 8);
    barsBox.addItem ("16 Bars", 16);
    barsBox.addItem ("32 Bars", 32);
    barsBox.setSelectedId (8, juce::dontSendNotification);

    addAndMakeVisible (complexityBox);
    complexityBox.addItem ("Sparse", 1);
    complexityBox.addItem ("Standard", 2);
    complexityBox.addItem ("Dense", 3);
    complexityBox.addItem ("Polyrhythmic", 4);
    complexityBox.setSelectedId (2, juce::dontSendNotification);

    addAndMakeVisible (randomizeButton);
    randomizeButton.onClick = [this]
    {
        seedBox.setText (juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
    };

    addAndMakeVisible (generateButton);
    generateButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0284c7));
    generateButton.onClick = [this] { triggerGenerateBgm(); };

    addAndMakeVisible (rescanButton);
    rescanButton.onClick = [this]
    {
        library.ensureStarterPack (RiffLibrary::getDefaultLibraryDirectory());
    };
}

BgmGeneratorComponent::~BgmGeneratorComponent()
{
    genreBox.onChange = nullptr;
    randomizeButton.onClick = nullptr;
    generateButton.onClick = nullptr;
    rescanButton.onClick = nullptr;
}

BgmGeneratorComponent::AdvancedRequest BgmGeneratorComponent::getAdvancedRequest() const
{
    AdvancedRequest req;
    req.genre = genreBox.getText().trim();
    req.key = keyBox.getText().fromFirstOccurrenceOf ("Key: ", false, false).trim();
    req.scale = scaleBox.getText().trim();
    req.complexity = complexityBox.getText().trim().toLowerCase();
    req.bars = juce::jlimit (8, 32, barsBox.getSelectedId());
    req.seed = juce::jlimit (0, 1000000000, static_cast<int> (seedBox.getText().getLargeIntValue()));
    return req;
}

void BgmGeneratorComponent::triggerGenerateBgm()
{
    if (edit == nullptr)
        return;

    if (onGenerateAdvanced)
    {
        onGenerateAdvanced();
        return;
    }

    if (library.getRiffs().empty())
        library.ensureStarterPack (RiffLibrary::getDefaultLibraryDirectory());

    BgmRequest req;

    req.genre            = genreBox.getText().trim();
    req.targetKeyLabel   = keyBox.getText().fromFirstOccurrenceOf ("Key: ", false, false).trim();
    req.targetPitchClass = RiffLibrary::parseKeyPitchClass (req.targetKeyLabel);
    req.targetEnergy     = juce::jlimit (1, 5, energyBox.getSelectedId());
    req.totalBars        = juce::jlimit (8, 32, barsBox.getSelectedId());
    req.scale            = scaleBox.getText().trim();
    req.seed             = static_cast<std::uint64_t> (juce::jlimit (
                               0, 1000000000, static_cast<int> (seedBox.getText().getLargeIntValue())));
    req.complexity       = complexityBox.getText().trim().toLowerCase();
    req.timeSignature    = tcs::theory::MusicTheoryEngine::genreProfile (req.genre).timeSignature;

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

    titleLabel.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (5);
    genreBox.setBounds (row.removeFromLeft (104));
    row.removeFromLeft (4);
    keyBox.setBounds (row.removeFromLeft (68));
    row.removeFromLeft (4);
    scaleBox.setBounds (row.removeFromLeft (94));
    row.removeFromLeft (4);
    energyBox.setBounds (row.removeFromLeft (96));
    row.removeFromLeft (4);
    barsBox.setBounds (row.removeFromLeft (58));
    row.removeFromLeft (4);
    complexityBox.setBounds (row.removeFromLeft (82));
    row.removeFromLeft (4);
    seedBox.setBounds (row.removeFromLeft (44));
    row.removeFromLeft (4);
    randomizeButton.setBounds (row.removeFromLeft (62));
    row.removeFromLeft (6);
    generateButton.setBounds (row.removeFromLeft (96));
    row.removeFromLeft (4);
    rescanButton.setBounds (row.removeFromLeft (78));
}
