#include "HardwarePluginEditors.h"

#include "HardwareRackPlugins.h"
#include "PluginPresetManager.h"
#include "SidechainCompressorPlugin.h"
#include "VocalChannelPlugin.h"

#include <algorithm>
#include <cmath>

namespace
{
const juce::Colour chassis (0xff111317);
const juce::Colour bezel (0xff1a1e24);
const juce::Colour edge (0xff2b313c);
const juce::Colour emerald (0xff2dff8f);
const juce::Colour amber (0xffffb020);
const juce::Colour crimson (0xffff3b5c);
const juce::Colour silkscreen (0xffe6edf3);
const juce::Colour muted (0xff8b949e);
}

HardwareEditorBase::HardwareEditorBase (te::Plugin& targetPlugin, juce::String faceplateTitle,
                                        juce::Colour accent, int width, int height)
    : plugin (targetPlugin),
      title (std::move (faceplateTitle)),
      accentColour (accent),
      panelWidth (width), panelHeight (height)
{
    setOpaque (true);
    titleLabel.setText (title, juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, accentColour);
    addAndMakeVisible (titleLabel);
    startTimerHz (30);
    setSize (width, height);
}

HardwareEditorBase::~HardwareEditorBase()
{
    stopTimer();
    for (auto& knob : knobs)
        if (knob.slider != nullptr) knob.slider->onValueChange = nullptr;
    for (auto& pad : pads)
        if (pad != nullptr) pad->onClick = nullptr;
}

void HardwareEditorBase::addKnob (const juce::String& paramId, const juce::String& caption,
                                  int x, int y, int size)
{
    auto p = plugin.getAutomatableParameterByID (paramId);
    if (p == nullptr)
        return;

    auto slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag,
                                                  juce::Slider::NoTextBox);
    const auto range = p->getValueRange();
    slider->setRange (range.getStart(), range.getEnd(),
                      juce::jmax (0.01, range.getLength() / 200.0));
    slider->setValue (p->getCurrentValue(), juce::dontSendNotification);
    slider->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                 juce::MathConstants<float>::pi * 2.75f, true);
    slider->setColour (juce::Slider::trackColourId, accentColour);
    slider->setColour (juce::Slider::thumbColourId, silkscreen);
    slider->setBounds (x, y, size, size);
    slider->onValueChange = [s = slider.get(), param = p]()
    {
        param->setParameter (static_cast<float> (s->getValue()), juce::sendNotification);
    };
    addAndMakeVisible (slider.get());

    auto label = std::make_unique<juce::Label>();
    label->setText (caption, juce::dontSendNotification);
    label->setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    label->setColour (juce::Label::textColourId, muted);
    label->setJustificationType (juce::Justification::centred);
    label->setBounds (x - 8, y + size + 2, size + 16, 18);
    addAndMakeVisible (label.get());

    knobs.push_back ({ std::move (slider), std::move (label), p });
}

juce::TextButton& HardwareEditorBase::addPad (const juce::String& name, int x, int y,
                                              int w, int h, juce::Colour colour,
                                              std::function<void()> action)
{
    auto pad = std::make_unique<juce::TextButton> (name);
    pad->setBounds (x, y, w, h);
    pad->setColour (juce::TextButton::buttonColourId, colour.withAlpha (0.65f));
    pad->setColour (juce::TextButton::buttonOnColourId, colour);
    pad->setClickingTogglesState (false);
    pad->onClick = std::move (action);
    auto& ref = *pad;
    addAndMakeVisible (pad.get());
    pads.push_back (std::move (pad));
    return ref;
}

void HardwareEditorBase::addMeterLabel (const juce::String& prefix, int x, int y, int w)
{
    auto label = std::make_unique<juce::Label>();
    label->setText (prefix, juce::dontSendNotification);
    label->setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    label->setColour (juce::Label::textColourId, emerald);
    label->setBounds (x, y, w, 20);
    addAndMakeVisible (label.get());
    meterLabels.push_back (std::move (label));
}

void HardwareEditorBase::setSectionDividers (const std::vector<juce::Colour>& colours)
{
    dividerColours = colours;
}

void HardwareEditorBase::paint (juce::Graphics& g)
{
    g.fillAll (chassis);
    g.setColour (bezel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f);
    g.setColour (edge);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f, 1.0f);

    g.setColour (accentColour.withAlpha (0.18f));
    g.fillRect (0, 0, getWidth(), 2);

    int x = 0;
    for (const auto& colour : dividerColours)
    {
        const int width = juce::jmax (4, getWidth() / juce::jmax (1, static_cast<int> (dividerColours.size())));
        g.setColour (colour.withAlpha (0.8f));
        g.fillRect (x, 28, width, 3);
        x += width;
    }

    g.setColour (muted);
    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    g.drawText ("TOXIC CREATIONZ STUDIO  /  " + title.toUpperCase(),
                4, getHeight() - 16, getWidth() - 8, 12, juce::Justification::centredLeft, false);
}

void HardwareEditorBase::resized()
{
    titleLabel.setBounds (10, 4, getWidth() - 20, 22);
}

void HardwareEditorBase::timerCallback()
{
    for (auto& knob : knobs)
    {
        if (knob.slider == nullptr || knob.parameter == nullptr || knob.slider->isMouseButtonDown())
            continue;
        const double current = knob.parameter->getCurrentValue();
        if (std::abs (knob.slider->getValue() - current) > 1.0e-5)
            knob.slider->setValue (current, juce::dontSendNotification);
    }
    updateMeters();
    repaint();
}

//------------------------------------------------------------------------------
Tr808Editor::Tr808Editor (te::Plugin& p)
    : HardwareEditorBase (p, "TR-808 DRUM SYNTH", crimson, 640, 320)
{
    setSectionDividers ({ crimson, juce::Colour (0xffff8000), amber, silkscreen });
    addKnob ("kickDecay", "KICK DEC", 20, 52, 74);
    addKnob ("kickTone", "KICK TONE", 112, 52, 74);
    addKnob ("snareSnappy", "SNARE", 204, 52, 74);
    addKnob ("hatDecay", "HAT DEC", 296, 52, 74);
    addKnob ("accent", "ACCENT", 388, 52, 74);

    const int padNotes[6] = { 36, 38, 42, 46, 45, 48 };
    const char* names[6] = { "KICK", "SNARE", "CH", "OH", "LT", "MT" };
    for (int i = 0; i < 6; ++i)
    {
        addPad (names[i], 20 + i * 102, 172, 92, 56,
                i < 3 ? crimson : (i < 5 ? amber : silkscreen),
                [this, note = padNotes[i]]()
                {
                    if (auto* drum = dynamic_cast<Tr808DrumPlugin*> (&plugin))
                        drum->triggerAudition (note);
                });
    }
}

void Tr808Editor::updateMeters()
{
    // Trigger pads flash via their own button state; no meter telemetry needed.
}

//------------------------------------------------------------------------------
VintageSamplerEditor::VintageSamplerEditor (te::Plugin& p)
    : HardwareEditorBase (p, "SP-1200 VINTAGE SAMPLER", amber, 640, 320)
{
    setSectionDividers ({ amber, silkscreen, muted });
    addKnob ("bitDepth", "BIT DEPTH", 20, 52, 68);
    addKnob ("targetRate", "SAMPLE RATE", 104, 52, 68);
    addKnob ("filterCutoff", "RECON LPF", 188, 52, 68);
    addKnob ("cvPitchDepth", "CV PITCH", 272, 52, 68);
    addKnob ("gain", "GAIN", 356, 52, 68);

    addAndMakeVisible (loadButton);
    loadButton.setButtonText ("+ Load WAV...");
    loadButton.setBounds (460, 56, 160, 30);
    loadButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a3220));
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load sampler WAV", juce::File {}, "*.wav");
        auto safe = juce::Component::SafePointer<VintageSamplerEditor> (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& c)
        {
            if (safe == nullptr) return;
            const auto file = c.getResult();
            if (file.existsAsFile())
                if (auto* sampler = dynamic_cast<VintageSamplerPlugin*> (&safe->plugin))
                    sampler->loadSampleFromFile (file);
        });
    };

    addAndMakeVisible (resetButton);
    resetButton.setButtonText ("Reset Chord Stab");
    resetButton.setBounds (460, 92, 160, 30);
    resetButton.onClick = [this]
    {
        if (auto* sampler = dynamic_cast<VintageSamplerPlugin*> (&plugin))
            sampler->resetToStarterStab();
    };

    addAndMakeVisible (sampleLabel);
    sampleLabel.setBounds (460, 130, 160, 36);
    sampleLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    sampleLabel.setColour (juce::Label::textColourId, silkscreen);

    const char* names[8] = { "C3", "D3", "E3", "G3", "C4", "D4", "E4", "G4" };
    const int padNotes[8] = { 48, 50, 52, 55, 60, 62, 64, 67 };
    for (int i = 0; i < 8; ++i)
        addPad (names[i], 20 + (i % 4) * 102, 172 + (i / 4) * 56, 92, 48,
                amber.withAlpha (0.55f), [this, note = padNotes[i]]() { juce::ignoreUnused (note); });
}

VintageSamplerEditor::~VintageSamplerEditor()
{
    loadButton.onClick = nullptr;
    resetButton.onClick = nullptr;
    chooser.reset();
}

void VintageSamplerEditor::paint (juce::Graphics& g)
{
    HardwareEditorBase::paint (g);
    auto* sampler = dynamic_cast<VintageSamplerPlugin*> (&plugin);
    if (sampler == nullptr)
        return;

    const auto bounds = juce::Rectangle<float> (460.0f, 168.0f, 160.0f, 92.0f);
    g.setColour (juce::Colour (0xff0b0e13));
    g.fillRoundedRectangle (bounds, 3.0f);
    const auto& buffer = sampler->getSampleBufferForPreview();
    if (buffer.getNumSamples() <= 0)
        return;

    g.setColour (emerald.withAlpha (0.85f));
    const float midY = bounds.getCentreY();
    const int step = juce::jmax (1, buffer.getNumSamples() / 160);
    for (int x = 0; x < 160; ++x)
    {
        const int sampleIndex = juce::jmin (buffer.getNumSamples() - 1, x * step);
        const float v = buffer.getSample (0, sampleIndex);
        const float y = midY - v * 36.0f;
        g.drawLine (bounds.getX() + static_cast<float> (x), midY,
                    bounds.getX() + static_cast<float> (x), y, 1.0f);
    }
}

void VintageSamplerEditor::updateMeters()
{
    if (auto* sampler = dynamic_cast<VintageSamplerPlugin*> (&plugin))
        sampleLabel.setText (sampler->getLoadedSampleName(), juce::dontSendNotification);
}

//------------------------------------------------------------------------------
AnalogTapeEditor::AnalogTapeEditor (te::Plugin& p)
    : HardwareEditorBase (p, "ANALOG TAPE DECK", amber, 640, 320)
{
    setSectionDividers ({ amber, amber.brighter (0.3f), muted });
    addKnob ("drive", "DRIVE", 20, 52, 74);
    addKnob ("wow", "WOW", 110, 52, 74);
    addKnob ("flutter", "FLUTTER", 200, 52, 74);
    addKnob ("bias", "BIAS", 290, 52, 74);
    addKnob ("mix", "MIX", 380, 52, 74);
    addMeterLabel ("VU", 478, 56, 140);
}

void AnalogTapeEditor::paint (juce::Graphics& g)
{
    HardwareEditorBase::paint (g);
    const auto drawNeedle = [&] (juce::Rectangle<float> area, float db, const char* label)
    {
        g.setColour (juce::Colour (0xff0b0e13));
        g.fillRoundedRectangle (area, 3.0f);
        g.setColour (muted);
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText (label, area, juce::Justification::centredTop, false);
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        const float angle = juce::MathConstants<float>::pi * (0.75f + 1.5f * norm);
        const auto centre = area.getCentre();
        g.setColour (amber);
        g.drawLine (centre.x, centre.y,
                    centre.x + std::cos (angle) * area.getWidth() * 0.42f,
                    centre.y + std::sin (angle) * area.getHeight() * 0.72f, 2.0f);
    };
    drawNeedle (juce::Rectangle<float> (470.0f, 96.0f, 150.0f, 84.0f), leftNeedle, "L");
    drawNeedle (juce::Rectangle<float> (470.0f, 190.0f, 150.0f, 84.0f), rightNeedle, "R");
}

void AnalogTapeEditor::updateMeters()
{
    if (auto* tape = dynamic_cast<AnalogTapePlugin*> (&plugin))
    {
        const float db = tape->currentLevelDb.load();
        leftNeedle = db;
        rightNeedle = db - 0.5f;
        if (! meterLabels.empty())
            meterLabels[0]->setText ("VU " + juce::String (db, 1) + " dB",
                                     juce::dontSendNotification);
    }
}

//------------------------------------------------------------------------------
SidechainCompEditor::SidechainCompEditor (te::Plugin& p)
    : HardwareEditorBase (p, "SIDECHAIN BUS COMPRESSOR", emerald, 640, 320)
{
    setSectionDividers ({ emerald, amber, crimson });
    addKnob ("thresholdDb", "THRESH", 20, 52, 74);
    addKnob ("ratio", "RATIO", 110, 52, 74);
    addKnob ("attackMs", "ATTACK", 200, 52, 74);
    addKnob ("releaseMs", "RELEASE", 290, 52, 74);
    addKnob ("makeupDb", "MAKEUP", 380, 52, 74);
    addKnob ("mix", "MIX", 470, 52, 74);
    addMeterLabel ("GR", 20, 172, 200);
}

void SidechainCompEditor::paint (juce::Graphics& g)
{
    HardwareEditorBase::paint (g);
    const auto bar = juce::Rectangle<float> (20.0f, 200.0f, 500.0f, 26.0f);
    g.setColour (juce::Colour (0xff0b0e13));
    g.fillRoundedRectangle (bar, 3.0f);
    const float norm = juce::jlimit (0.0f, 1.0f, std::abs (reductionDb) / 24.0f);
    for (int led = 0; led < 20; ++led)
    {
        const auto r = juce::Rectangle<float> (bar.getX() + 3.0f + static_cast<float> (led) * 24.0f,
                                               bar.getY() + 4.0f, 20.0f, bar.getHeight() - 8.0f);
        const bool lit = static_cast<float> (led) / 20.0f < norm;
        g.setColour (lit ? (led > 16 ? crimson : (led > 12 ? amber : emerald))
                         : juce::Colour (0xff26303a));
        g.fillRoundedRectangle (r, 2.0f);
    }
}

void SidechainCompEditor::updateMeters()
{
    if (auto* comp = dynamic_cast<SidechainCompressorPlugin*> (&plugin))
    {
        reductionDb = comp->currentGainReductionDb.load();
        if (! meterLabels.empty())
            meterLabels[0]->setText ("GR " + juce::String (reductionDb, 1) + " dB",
                                     juce::dontSendNotification);
    }
}

//------------------------------------------------------------------------------
VocalChannelEditor::VocalChannelEditor (te::Plugin& p)
    : HardwareEditorBase (p, "VOICE BOOTH STRIP", emerald, 640, 320)
{
    setSectionDividers ({ emerald, amber, crimson, silkscreen });
    addKnob ("hpfHz", "HPF", 20, 52, 68);
    addKnob ("gateThresholdDb", "GATE", 104, 52, 68);
    addKnob ("breathFloorDb", "BREATH", 188, 52, 68);
    addKnob ("deEssFreqHz", "DE-ESS FREQ", 272, 52, 68);
    addKnob ("deEssAmount", "DE-ESS AMT", 356, 52, 68);
    addKnob ("ceilingDb", "CEILING", 440, 52, 68);
    addKnob ("outputGainDb", "OUTPUT", 524, 52, 68);

    addAndMakeVisible (presetCombo);
    presetCombo.setBounds (20, 168, 260, 26);
    for (const auto& name : PluginPresetManager::getFactoryPresetNames (plugin))
        presetCombo.addItem (name, presetCombo.getNumItems() + 1);
    presetCombo.onChange = [this]
    {
        PluginPresetManager::applyFactoryPreset (plugin, presetCombo.getText());
    };
    addMeterLabel ("LIMITER GR", 300, 168, 300);
}

VocalChannelEditor::~VocalChannelEditor()
{
    presetCombo.onChange = nullptr;
}

void VocalChannelEditor::paint (juce::Graphics& g)
{
    HardwareEditorBase::paint (g);
    auto bar = juce::Rectangle<float> (300.0f, 198.0f, 320.0f, 22.0f);
    g.setColour (juce::Colour (0xff0b0e13));
    g.fillRoundedRectangle (bar, 3.0f);
    const float norm = juce::jlimit (0.0f, 1.0f, limiterGrDb / 12.0f);
    g.setColour (norm > 0.7f ? crimson : (norm > 0.3f ? amber : emerald));
    g.fillRoundedRectangle (bar.removeFromLeft (bar.getWidth() * norm), 3.0f);
}

void VocalChannelEditor::updateMeters()
{
    if (auto* vocal = dynamic_cast<VocalChannelPlugin*> (&plugin))
    {
        limiterGrDb = vocal->currentLimiterGrDb.load();
        if (! meterLabels.empty())
            meterLabels[0]->setText ("LIMITER GR " + juce::String (limiterGrDb, 1) + " dB",
                                     juce::dontSendNotification);
    }
}

//------------------------------------------------------------------------------
std::unique_ptr<te::Plugin::EditorComponent> createCustomEditorForPlugin (te::Plugin& plugin)
{
    if (dynamic_cast<Tr808DrumPlugin*> (&plugin) != nullptr)
        return std::make_unique<Tr808Editor> (plugin);
    if (dynamic_cast<VintageSamplerPlugin*> (&plugin) != nullptr)
        return std::make_unique<VintageSamplerEditor> (plugin);
    if (dynamic_cast<AnalogTapePlugin*> (&plugin) != nullptr)
        return std::make_unique<AnalogTapeEditor> (plugin);
    if (dynamic_cast<SidechainCompressorPlugin*> (&plugin) != nullptr)
        return std::make_unique<SidechainCompEditor> (plugin);
    if (dynamic_cast<VocalChannelPlugin*> (&plugin) != nullptr)
        return std::make_unique<VocalChannelEditor> (plugin);
    return {};
}
