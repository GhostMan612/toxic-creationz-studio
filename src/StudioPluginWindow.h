#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "PluginPresetManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace te = tracktion::engine;

//==============================================================================
// Scrollable dark-themed parameter, preset & synth/sampler control editor for
// built-in Tracktion and custom CV plugins without a native VST3 UI.
//==============================================================================
class GenericTracktionPluginEditor : public te::Plugin::EditorComponent,
                                     private juce::Timer
{
public:
    explicit GenericTracktionPluginEditor (te::Plugin& targetPlugin)
        : plugin (targetPlugin)
    {
        setOpaque (true);

        const int numParams = plugin.getNumAutomatableParameters();

        addAndMakeVisible (headerLabel);
        headerLabel.setText (plugin.getName() + "  (" + juce::String (numParams) + " params)",
                             juce::dontSendNotification);
        headerLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        headerLabel.setColour (juce::Label::textColourId, juce::Colour (0xff38bdf8));

        addAndMakeVisible (presetLabel);
        presetLabel.setText ("Preset:", juce::dontSendNotification);
        presetLabel.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
        presetLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcbd5e1));

        addAndMakeVisible (presetCombo);
        presetCombo.setTextWhenNothingSelected ("Select Preset...");
        presetCombo.onChange = [this] { handlePresetSelection(); };

        addAndMakeVisible (savePresetButton);
        savePresetButton.onClick = [this] { savePresetDialog(); };

        addAndMakeVisible (loadPresetButton);
        loadPresetButton.onClick = [this] { loadPresetDialog(); };

        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&paramsContainer, false);
        viewport.setScrollBarsShown (true, false);

        rebuildPresetList();

        if (auto* synth = dynamic_cast<te::FourOscPlugin*> (&plugin))
            buildFourOscControls (*synth);
        else if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (&plugin))
            buildSamplerControls (*sampler);

        for (int i = 0; i < numParams; ++i)
        {
            auto p = plugin.getAutomatableParameter (i);
            if (p == nullptr)
                continue;

            ParamRow row;
            row.paramIndex = i;

            row.nameLabel = std::make_unique<juce::Label>();
            row.nameLabel->setText (p->getParameterName(), juce::dontSendNotification);
            row.nameLabel->setFont (juce::Font (juce::FontOptions (11.5f)));
            row.nameLabel->setColour (juce::Label::textColourId, juce::Colour (0xffe2e8f0));
            row.nameLabel->setJustificationType (juce::Justification::centredLeft);

            row.slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                         juce::Slider::NoTextBox);
            const auto range = p->getValueRange();
            const double minVal = range.getLength() > 0.0f ? static_cast<double> (range.getStart()) : 0.0;
            const double maxVal = range.getLength() > 0.0f ? static_cast<double> (range.getEnd()) : 1.0;
            const double step   = (p->isDiscrete() && range.getLength() >= 1.0f) ? 1.0 : 0.0;

            row.slider->setRange (minVal, maxVal, step);
            row.slider->setValue (static_cast<double> (p->getCurrentValue()), juce::dontSendNotification);
            row.slider->setColour (juce::Slider::trackColourId, juce::Colour (0xff38bdf8));
            row.slider->setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2a3242));
            row.slider->setColour (juce::Slider::thumbColourId, juce::Colour (0xfff1f5f9));

            row.valueLabel = std::make_unique<juce::Label>();
            row.valueLabel->setText (p->getCurrentValueAsStringWithLabel(), juce::dontSendNotification);
            row.valueLabel->setFont (juce::Font (juce::FontOptions (11.0f)));
            row.valueLabel->setColour (juce::Label::textColourId, juce::Colour (0xff94a3b8));
            row.valueLabel->setJustificationType (juce::Justification::centredRight);

            const size_t rowIdx = rows.size();
            row.slider->onValueChange = [this, rowIdx]
            {
                if (rowIdx >= rows.size())
                    return;

                auto& r = rows[rowIdx];
                if (auto param = plugin.getAutomatableParameter (r.paramIndex))
                {
                    param->setParameter (static_cast<float> (r.slider->getValue()), juce::sendNotification);
                    r.valueLabel->setText (param->getCurrentValueAsStringWithLabel(),
                                           juce::dontSendNotification);
                }
            };

            paramsContainer.addAndMakeVisible (*row.nameLabel);
            paramsContainer.addAndMakeVisible (*row.slider);
            paramsContainer.addAndMakeVisible (*row.valueLabel);
            rows.push_back (std::move (row));
        }

        const int extraH = getExtraControlsHeight();
        const int initialH = juce::jlimit (170, 520, 76 + extraH + static_cast<int> (rows.size()) * 28);
        setSize (480, initialH);
        startTimerHz (10);
    }

    ~GenericTracktionPluginEditor() override
    {
        stopTimer();
        activeChooser.reset();
    }

    bool allowWindowResizing() override { return true; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }

    int getNumPresetItems() const noexcept
    {
        return static_cast<int> (factoryPresetNames.size() + userPresetFiles.size());
    }

    bool selectFactoryPresetForTest (const juce::String& name)
    {
        const int idx = factoryPresetNames.indexOf (name);
        if (idx < 0)
            return false;

        presetCombo.setSelectedId (idx + 1, juce::dontSendNotification);
        const bool ok = PluginPresetManager::applyFactoryPreset (plugin, name);
        syncAllControlsFromPlugin();
        return ok;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141821));

        g.setColour (juce::Colour (0xff1b212d));
        g.fillRect (0, 0, getWidth(), 64);
        g.setColour (juce::Colour (0xff2b3344));
        g.drawHorizontalLine (31, 0.0f, static_cast<float> (getWidth()));
        g.drawHorizontalLine (63, 0.0f, static_cast<float> (getWidth()));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        headerLabel.setBounds (area.removeFromTop (32).reduced (10, 4));

        auto presetRow = area.removeFromTop (32).reduced (8, 4);
        presetLabel.setBounds (presetRow.removeFromLeft (48));
        loadPresetButton.setBounds (presetRow.removeFromRight (54));
        presetRow.removeFromRight (4);
        savePresetButton.setBounds (presetRow.removeFromRight (54));
        presetRow.removeFromRight (6);
        presetCombo.setBounds (presetRow);

        viewport.setBounds (area);

        const int rowH = 28;
        const int extraH = getExtraControlsHeight();
        const int containerW = std::max (260, viewport.getMaximumVisibleWidth());
        const int containerH = std::max (area.getHeight(), 10 + extraH + static_cast<int> (rows.size()) * rowH);
        paramsContainer.setSize (containerW, containerH);

        int y = 6;
        if (dynamic_cast<te::FourOscPlugin*> (&plugin) != nullptr)
        {
            for (int i = 0; i < 4; ++i)
            {
                juce::Rectangle<int> oscRow (8, y, containerW - 16, 24);
                oscRows[static_cast<size_t> (i)].label->setBounds (oscRow.removeFromLeft (46));
                oscRows[static_cast<size_t> (i)].voicesBox->setBounds (oscRow.removeFromRight (66));
                oscRow.removeFromRight (6);
                oscRows[static_cast<size_t> (i)].waveBox->setBounds (oscRow);
                y += 28;
            }

            {
                juce::Rectangle<int> vfRow (8, y, containerW - 16, 24);
                voiceModeBox.setBounds (vfRow.removeFromLeft (86));
                vfRow.removeFromLeft (6);
                filterSlopeToggle.setBounds (vfRow.removeFromRight (62));
                vfRow.removeFromRight (6);
                filterTypeBox.setBounds (vfRow);
                y += 28;
            }

            {
                juce::Rectangle<int> fxRow (8, y, containerW - 16, 24);
                const int bw = std::max (50, (fxRow.getWidth() - 18) / 4);
                distOnToggle.setBounds   (fxRow.removeFromLeft (bw)); fxRow.removeFromLeft (6);
                chorusOnToggle.setBounds (fxRow.removeFromLeft (bw)); fxRow.removeFromLeft (6);
                delayOnToggle.setBounds  (fxRow.removeFromLeft (bw)); fxRow.removeFromLeft (6);
                reverbOnToggle.setBounds (fxRow);
                y += 32;
            }
        }
        else if (dynamic_cast<te::SamplerPlugin*> (&plugin) != nullptr)
        {
            juce::Rectangle<int> btnRow (8, y, containerW - 16, 26);
            resetKitButton.setBounds (btnRow.removeFromLeft (120));
            btnRow.removeFromLeft (8);
            addSampleButton.setBounds (btnRow.removeFromLeft (115));
            y += 30;

            samplerZonesLabel.setBounds (8, y, containerW - 16, 48);
            y += 54;
        }

        for (auto& r : rows)
        {
            juce::Rectangle<int> rowBounds (8, y, containerW - 16, rowH - 2);
            const int nameW  = std::min (155, rowBounds.getWidth() / 3);
            const int valW   = 78;

            r.nameLabel->setBounds (rowBounds.removeFromLeft (nameW));
            r.valueLabel->setBounds (rowBounds.removeFromRight (valW));
            rowBounds.removeFromLeft (4);
            rowBounds.removeFromRight (4);
            r.slider->setBounds (rowBounds);
            y += rowH;
        }
    }

private:
    struct ParamRow
    {
        int paramIndex = 0;
        std::unique_ptr<juce::Label> nameLabel;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> valueLabel;
    };

    struct FourOscUiRow
    {
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::ComboBox> waveBox;
        std::unique_ptr<juce::ComboBox> voicesBox;
    };

    int getExtraControlsHeight() const noexcept
    {
        if (dynamic_cast<te::FourOscPlugin*> (&plugin) != nullptr)
            return 6 * 28 + 8;
        if (dynamic_cast<te::SamplerPlugin*> (&plugin) != nullptr)
            return 84;
        return 0;
    }

    void rebuildPresetList()
    {
        const int previousId = presetCombo.getSelectedId();
        presetCombo.clear (juce::dontSendNotification);

        factoryPresetNames = PluginPresetManager::getFactoryPresetNames (plugin);
        userPresetFiles    = PluginPresetManager::listUserPresetFiles (plugin);

        if (! factoryPresetNames.isEmpty())
        {
            presetCombo.addSectionHeading ("Factory Presets");
            for (int i = 0; i < factoryPresetNames.size(); ++i)
                presetCombo.addItem (factoryPresetNames[i], i + 1);
        }

        if (! userPresetFiles.isEmpty())
        {
            presetCombo.addSeparator();
            presetCombo.addSectionHeading ("User Presets (.trkpreset)");
            for (int i = 0; i < userPresetFiles.size(); ++i)
                presetCombo.addItem (userPresetFiles[i].getFileNameWithoutExtension(), 1000 + i);
        }

        if (previousId > 0)
            presetCombo.setSelectedId (previousId, juce::dontSendNotification);
    }

    void handlePresetSelection()
    {
        const int chosenId = presetCombo.getSelectedId();
        if (chosenId <= 0)
            return;

        if (chosenId >= 1 && chosenId <= factoryPresetNames.size())
        {
            if (PluginPresetManager::applyFactoryPreset (plugin, factoryPresetNames[chosenId - 1]))
                syncAllControlsFromPlugin();
            return;
        }

        const int userIdx = chosenId - 1000;
        if (juce::isPositiveAndBelow (userIdx, userPresetFiles.size()))
        {
            if (PluginPresetManager::loadPresetFromFile (plugin, userPresetFiles[userIdx]))
                syncAllControlsFromPlugin();
        }
    }

    void savePresetDialog()
    {
        const auto dir = PluginPresetManager::getUserPresetsDirectory (plugin.getPluginType());
        const auto defaultFile = dir.getChildFile (
            juce::File::createLegalFileName (plugin.getName() + "_Custom") + ".trkpreset");

        activeChooser = std::make_unique<juce::FileChooser> (
            "Save Plugin Preset", defaultFile, "*.trkpreset");

        juce::Component::SafePointer<GenericTracktionPluginEditor> safeThis (this);
        activeChooser->launchAsync (
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis] (const juce::FileChooser& chooser)
            {
                if (safeThis == nullptr)
                    return;

                auto target = chooser.getResult();
                safeThis->activeChooser.reset();

                if (target == juce::File())
                    return;

                if (! target.hasFileExtension (".trkpreset"))
                    target = target.withFileExtension (".trkpreset");

                if (PluginPresetManager::savePresetToFile (safeThis->plugin, target))
                    safeThis->rebuildPresetList();
            });
    }

    void loadPresetDialog()
    {
        const auto dir = PluginPresetManager::getUserPresetsDirectory (plugin.getPluginType());
        activeChooser = std::make_unique<juce::FileChooser> (
            "Load Plugin Preset", dir, "*.trkpreset");

        juce::Component::SafePointer<GenericTracktionPluginEditor> safeThis (this);
        activeChooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis] (const juce::FileChooser& chooser)
            {
                if (safeThis == nullptr)
                    return;

                const auto source = chooser.getResult();
                safeThis->activeChooser.reset();

                if (source.existsAsFile()
                    && PluginPresetManager::loadPresetFromFile (safeThis->plugin, source))
                {
                    safeThis->rebuildPresetList();
                    safeThis->syncAllControlsFromPlugin();
                }
            });
    }

    void buildFourOscControls (te::FourOscPlugin& synth)
    {
        static const char* waveNames[] = { "Off", "Sine", "Square", "Saw", "Triangle", "Noise" };

        for (int i = 0; i < 4; ++i)
        {
            auto& ui = oscRows[static_cast<size_t> (i)];
            ui.label = std::make_unique<juce::Label>();
            ui.label->setText ("Osc " + juce::String (i + 1), juce::dontSendNotification);
            ui.label->setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            ui.label->setColour (juce::Label::textColourId, juce::Colour (0xffe2e8f0));

            ui.waveBox = std::make_unique<juce::ComboBox>();
            for (int w = 0; w < 6; ++w)
                ui.waveBox->addItem (waveNames[w], w + 1);

            ui.waveBox->onChange = [&synth, i, box = ui.waveBox.get()]
            {
                if (juce::isPositiveAndBelow (i, synth.oscParams.size()))
                {
                    synth.oscParams[i]->waveShapeValue = std::max (0, box->getSelectedId() - 1);
                    synth.edit.pluginChanged (synth);
                }
            };

            ui.voicesBox = std::make_unique<juce::ComboBox>();
            for (int v = 1; v <= 8; ++v)
                ui.voicesBox->addItem (juce::String (v) + "v", v);

            ui.voicesBox->onChange = [&synth, i, box = ui.voicesBox.get()]
            {
                if (juce::isPositiveAndBelow (i, synth.oscParams.size()))
                {
                    synth.oscParams[i]->voicesValue = std::max (1, box->getSelectedId());
                    synth.edit.pluginChanged (synth);
                }
            };

            paramsContainer.addAndMakeVisible (*ui.label);
            paramsContainer.addAndMakeVisible (*ui.waveBox);
            paramsContainer.addAndMakeVisible (*ui.voicesBox);
        }

        voiceModeBox.addItem ("Mono", 1);
        voiceModeBox.addItem ("Legato", 2);
        voiceModeBox.addItem ("Poly", 3);
        voiceModeBox.onChange = [&synth, this]
        {
            synth.voiceModeValue = std::max (0, voiceModeBox.getSelectedId() - 1);
            synth.edit.pluginChanged (synth);
        };

        filterTypeBox.addItem ("Filter: Off", 1);
        filterTypeBox.addItem ("Filter: LowPass", 2);
        filterTypeBox.addItem ("Filter: HighPass", 3);
        filterTypeBox.addItem ("Filter: BandPass", 4);
        filterTypeBox.addItem ("Filter: Notch", 5);
        filterTypeBox.onChange = [&synth, this]
        {
            synth.filterTypeValue = std::max (0, filterTypeBox.getSelectedId() - 1);
            synth.edit.pluginChanged (synth);
        };

        filterSlopeToggle.onClick = [&synth, this]
        {
            synth.filterSlopeValue = filterSlopeToggle.getToggleState() ? 24 : 12;
            synth.edit.pluginChanged (synth);
        };

        distOnToggle.onClick   = [&synth, this] { synth.distortionOnValue = distOnToggle.getToggleState(); synth.edit.pluginChanged (synth); };
        chorusOnToggle.onClick = [&synth, this] { synth.chorusOnValue = chorusOnToggle.getToggleState(); synth.edit.pluginChanged (synth); };
        delayOnToggle.onClick  = [&synth, this] { synth.delayOnValue = delayOnToggle.getToggleState(); synth.edit.pluginChanged (synth); };
        reverbOnToggle.onClick = [&synth, this] { synth.reverbOnValue = reverbOnToggle.getToggleState(); synth.edit.pluginChanged (synth); };

        paramsContainer.addAndMakeVisible (voiceModeBox);
        paramsContainer.addAndMakeVisible (filterTypeBox);
        paramsContainer.addAndMakeVisible (filterSlopeToggle);
        paramsContainer.addAndMakeVisible (distOnToggle);
        paramsContainer.addAndMakeVisible (chorusOnToggle);
        paramsContainer.addAndMakeVisible (delayOnToggle);
        paramsContainer.addAndMakeVisible (reverbOnToggle);

        syncAllControlsFromPlugin();
    }

    void buildSamplerControls (te::SamplerPlugin& sampler)
    {
        paramsContainer.addAndMakeVisible (resetKitButton);
        paramsContainer.addAndMakeVisible (addSampleButton);
        paramsContainer.addAndMakeVisible (samplerZonesLabel);

        samplerZonesLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        samplerZonesLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcbd5e1));
        samplerZonesLabel.setJustificationType (juce::Justification::topLeft);

        resetKitButton.onClick = [this, &sampler]
        {
            PluginPresetManager::seedStarterSamplerKit (sampler);
            syncAllControlsFromPlugin();
        };

        addSampleButton.onClick = [this, &sampler]
        {
            activeChooser = std::make_unique<juce::FileChooser> (
                "Load WAV into Sampler",
                PluginPresetManager::getDefaultSamplerKitDirectory(),
                "*.wav;*.aif;*.aiff;*.flac");

            juce::Component::SafePointer<GenericTracktionPluginEditor> safeThis (this);
            activeChooser->launchAsync (
                juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safeThis, &sampler] (const juce::FileChooser& chooser)
                {
                    if (safeThis == nullptr)
                        return;

                    const auto file = chooser.getResult();
                    safeThis->activeChooser.reset();

                    if (file.existsAsFile())
                    {
                        PluginPresetManager::addSampleFileToSampler (sampler, file);
                        safeThis->syncAllControlsFromPlugin();
                    }
                });
        };

        syncAllControlsFromPlugin();
    }

    void syncAllControlsFromPlugin()
    {
        if (auto* synth = dynamic_cast<te::FourOscPlugin*> (&plugin))
        {
            for (int i = 0; i < 4 && i < synth->oscParams.size(); ++i)
            {
                auto& ui = oscRows[static_cast<size_t> (i)];
                auto* osc = synth->oscParams.getUnchecked (i);

                if (ui.waveBox != nullptr && ! ui.waveBox->isPopupActive())
                    ui.waveBox->setSelectedId (juce::jlimit (1, 6, osc->waveShapeValue.get() + 1), juce::dontSendNotification);

                if (ui.voicesBox != nullptr && ! ui.voicesBox->isPopupActive())
                    ui.voicesBox->setSelectedId (juce::jlimit (1, 8, osc->voicesValue.get()), juce::dontSendNotification);
            }

            if (! voiceModeBox.isPopupActive())
                voiceModeBox.setSelectedId (juce::jlimit (1, 3, synth->voiceModeValue.get() + 1), juce::dontSendNotification);

            if (! filterTypeBox.isPopupActive())
                filterTypeBox.setSelectedId (juce::jlimit (1, 5, synth->filterTypeValue.get() + 1), juce::dontSendNotification);

            filterSlopeToggle.setToggleState (synth->filterSlopeValue.get() >= 24, juce::dontSendNotification);
            distOnToggle.setToggleState (synth->distortionOnValue.get(), juce::dontSendNotification);
            chorusOnToggle.setToggleState (synth->chorusOnValue.get(), juce::dontSendNotification);
            delayOnToggle.setToggleState (synth->delayOnValue.get(), juce::dontSendNotification);
            reverbOnToggle.setToggleState (synth->reverbOnValue.get(), juce::dontSendNotification);
        }
        else if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (&plugin))
        {
            const int n = sampler->getNumSounds();
            juce::String summary = "Loaded zones: " + juce::String (n);
            for (int i = 0; i < std::min (4, n); ++i)
            {
                summary += "\n- " + sampler->getSoundName (i)
                         + " (Root MIDI " + juce::String (sampler->getKeyNote (i))
                         + ", " + juce::String (sampler->getMinKey (i))
                         + ".." + juce::String (sampler->getMaxKey (i)) + ")";
            }
            if (n == 0)
                summary += "  (Click 'Reset 808 Kit' or '+ Load WAV...')";

            samplerZonesLabel.setText (summary, juce::dontSendNotification);
        }

        for (auto& r : rows)
        {
            if (r.slider == nullptr || r.slider->isMouseButtonDown())
                continue;

            if (auto p = plugin.getAutomatableParameter (r.paramIndex))
            {
                const double current = static_cast<double> (p->getCurrentValue());
                if (std::abs (r.slider->getValue() - current) > 1e-5)
                    r.slider->setValue (current, juce::dontSendNotification);

                r.valueLabel->setText (p->getCurrentValueAsStringWithLabel(),
                                       juce::dontSendNotification);
            }
        }
    }

    void timerCallback() override
    {
        syncAllControlsFromPlugin();
    }

    te::Plugin& plugin;
    juce::Label headerLabel;
    juce::Label presetLabel;
    juce::ComboBox presetCombo;
    juce::TextButton savePresetButton { "Save" };
    juce::TextButton loadPresetButton { "Load" };
    juce::StringArray factoryPresetNames;
    juce::Array<juce::File> userPresetFiles;
    std::unique_ptr<juce::FileChooser> activeChooser;

    juce::Viewport viewport;
    juce::Component paramsContainer;

    std::array<FourOscUiRow, 4> oscRows;
    juce::ComboBox voiceModeBox;
    juce::ComboBox filterTypeBox;
    juce::ToggleButton filterSlopeToggle { "24 dB" };
    juce::ToggleButton distOnToggle      { "Dist" };
    juce::ToggleButton chorusOnToggle    { "Chorus" };
    juce::ToggleButton delayOnToggle     { "Delay" };
    juce::ToggleButton reverbOnToggle    { "Reverb" };

    juce::TextButton resetKitButton      { "Reset 808 Kit" };
    juce::TextButton addSampleButton     { "+ Load WAV..." };
    juce::Label samplerZonesLabel;

    std::vector<ParamRow> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenericTracktionPluginEditor)
};

//==============================================================================
// Floating host window for both native VST3 UIs and GenericTracktionPluginEditor.
//==============================================================================
class StudioPluginWindow : public juce::DocumentWindow
{
public:
    explicit StudioPluginWindow (te::Plugin& plug)
        : DocumentWindow (plug.getName(),
                          juce::Colour (0xff141821),
                          juce::DocumentWindow::closeButton,
                          true),
          plugin (plug)
    {
        setUsingNativeTitleBar (true);
        getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
        setResizeLimits (240, 120, 4000, 4000);

        recreateEditor();

        if (plugin.windowState != nullptr)
            setBoundsConstrained (getLocalBounds() + plugin.windowState->choosePositionForPluginWindow());

        updateStoredBounds = true;
    }

    ~StudioPluginWindow() override
    {
        updateStoredBounds = false;
        plugin.edit.flushPluginStateIfNeeded (plugin);
        setEditor (nullptr);
    }

    static std::unique_ptr<juce::Component> create (te::Plugin& plugin)
    {
        if (auto* externalPlugin = dynamic_cast<te::ExternalPlugin*> (&plugin))
            if (externalPlugin->getAudioPluginInstance() == nullptr)
                return nullptr;

        auto w = std::make_unique<StudioPluginWindow> (plugin);

        if (w->getEditor() == nullptr)
            return {};

        w->show();
        return w;
    }

    void show()
    {
        setVisible (true);
        toFront (false);
        setBoundsConstrained (getBounds());
    }

    void setEditor (std::unique_ptr<te::Plugin::EditorComponent> newEditor)
    {
        setConstrainer (nullptr);
        setContentNonOwned (nullptr, false);
        editor.reset();

        if (newEditor != nullptr)
        {
            editor = std::move (newEditor);
            setContentNonOwned (editor.get(), true);
        }

        setResizable (editor == nullptr || editor->allowWindowResizing(), false);

        if (editor != nullptr && editor->allowWindowResizing())
            setConstrainer (editor->getBoundsConstrainer());
    }

    te::Plugin::EditorComponent* getEditor() const noexcept { return editor.get(); }

    void recreateEditor()
    {
        setEditor (nullptr);
        auto ed = plugin.createEditor();

        if (ed == nullptr)
            ed = std::make_unique<GenericTracktionPluginEditor> (plugin);

        setEditor (std::move (ed));
    }

    void recreateEditorAsync()
    {
        setEditor (nullptr);

        juce::Timer::callAfterDelay (50, [safeThis = juce::Component::SafePointer<StudioPluginWindow> (this)]
        {
            if (safeThis != nullptr)
                safeThis->recreateEditor();
        });
    }

private:
    void moved() override
    {
        if (updateStoredBounds && plugin.windowState != nullptr)
        {
            plugin.windowState->lastWindowBounds = getBounds();
            plugin.edit.pluginChanged (plugin);
        }
    }

    void userTriedToCloseWindow() override
    {
        if (plugin.windowState != nullptr)
            plugin.windowState->closeWindowExplicitly();
    }

    void closeButtonPressed() override
    {
        userTriedToCloseWindow();
    }

    float getDesktopScaleFactor() const override { return 1.0f; }

    std::unique_ptr<te::Plugin::EditorComponent> editor;
    te::Plugin& plugin;
    bool updateStoredBounds = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StudioPluginWindow)
};

//==============================================================================
// Studio Tracktion UIBehaviour enabling native VST3 & built-in plugin windows.
//==============================================================================
class StudioUIBehaviour : public te::UIBehaviour
{
public:
    StudioUIBehaviour() = default;

    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& pws) override
    {
        if (auto* ws = dynamic_cast<te::Plugin::WindowState*> (&pws))
            return StudioPluginWindow::create (ws->plugin);

        return {};
    }

    void recreatePluginWindowContentAsync (te::Plugin& p) override
    {
        if (p.windowState != nullptr)
        {
            if (auto* w = dynamic_cast<StudioPluginWindow*> (p.windowState->pluginWindow.get()))
            {
                w->recreateEditorAsync();
                return;
            }
        }

        UIBehaviour::recreatePluginWindowContentAsync (p);
    }
};
