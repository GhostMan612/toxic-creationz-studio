#include "MixerComponent.h"

//==============================================================================
// ChannelStrip
//==============================================================================
MixerComponent::ChannelStrip::ChannelStrip (MixerComponent& ownerMixer,
                                            te::Engine& eng,
                                            te::Edit& ed,
                                            int index)
    : owner (ownerMixer),
      engine (eng),
      edit (ed),
      trackIndex (index)
{
    addAndMakeVisible (titleLabel);
    titleLabel.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
    addAndMakeVisible (addPluginButton);

    muteButton.setClickingTogglesState (true);
    soloButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd9534f));
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xfff0ad4e));
    soloButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);

    muteButton.onClick = [this]
    {
        if (auto* t = resolveTrack())
        {
            t->setMute (muteButton.getToggleState());

            if (owner.onMixerChanged != nullptr)
                owner.onMixerChanged();
        }
    };

    soloButton.onClick = [this]
    {
        if (auto* t = resolveTrack())
        {
            t->setSolo (soloButton.getToggleState());

            if (owner.onMixerChanged != nullptr)
                owner.onMixerChanged();
        }
    };

    addPluginButton.onClick = [this] { showAddPluginMenu(); };

    // Pan (-1.0 L .. +1.0 R)
    addAndMakeVisible (panSlider);
    panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    panSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    panSlider.setRange (-1.0, 1.0, 0.01);
    panSlider.setDoubleClickReturnValue (true, 0.0);
    panSlider.setTooltip ("Stereo Pan (-1.0 L to +1.0 R)");
    panSlider.onValueChange = [this]
    {
        if (auto* t = resolveTrack())
            if (auto* vol = t->getVolumePlugin())
                vol->setPan (static_cast<float> (panSlider.getValue()));
    };

    // Vertical volume fader (-60 dB .. +6 dB)
    addAndMakeVisible (volumeSlider);
    volumeSlider.setSliderStyle (juce::Slider::LinearVertical);
    volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange (-60.0, 6.0, 0.1);
    volumeSlider.setSkewFactorFromMidPoint (-12.0);
    volumeSlider.setDoubleClickReturnValue (true, 0.0);
    volumeSlider.onValueChange = [this]
    {
        if (auto* t = resolveTrack())
        {
            if (auto* vol = t->getVolumePlugin())
            {
                const float db = static_cast<float> (volumeSlider.getValue());
                vol->setVolumeDb (db);
                dbLabel.setText (juce::String (db, 1) + " dB", juce::dontSendNotification);

                if (owner.onMixerChanged != nullptr)
                    owner.onMixerChanged();
            }
        }
    };

    addAndMakeVisible (dbLabel);
    dbLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    dbLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcbd5e1));
    dbLabel.setJustificationType (juce::Justification::centred);

    syncState();
    rebuildPluginSlots();
}

te::AudioTrack* MixerComponent::ChannelStrip::resolveTrack() const
{
    auto tracks = te::getAudioTracks (edit);

    if (juce::isPositiveAndBelow (trackIndex, tracks.size()))
        return tracks[trackIndex];

    return nullptr;
}

void MixerComponent::ChannelStrip::syncState()
{
    auto* t = resolveTrack();
    if (t == nullptr)
        return;

    titleLabel.setText (juce::String (trackIndex + 1) + ". " + t->getName(), juce::dontSendNotification);
    muteButton.setToggleState (t->isMuted (false), juce::dontSendNotification);
    soloButton.setToggleState (t->isSolo (false), juce::dontSendNotification);

    if (auto* vol = t->getVolumePlugin())
    {
        const float db = vol->getVolumeDb();
        volumeSlider.setValue (db, juce::dontSendNotification);
        panSlider.setValue (vol->getPan(), juce::dontSendNotification);
        dbLabel.setText (juce::String (db, 1) + " dB", juce::dontSendNotification);
    }
}

void MixerComponent::ChannelStrip::rebuildPluginSlots()
{
    pluginSlots.clear();

    auto* t = resolveTrack();
    if (t == nullptr)
        return;

    for (auto* plugin : t->pluginList)
    {
        // Hide the built-in terminal volume/meter utilities from the insert rack.
        if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) != nullptr
            || dynamic_cast<te::LevelMeterPlugin*> (plugin) != nullptr)
        {
            continue;
        }

        PluginSlotRow row;
        row.pluginID = plugin->itemID;

        row.openButton = std::make_unique<juce::TextButton> (plugin->getName());
        row.openButton->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff263147));
        row.openButton->setTooltip ("Open plugin editor window");

        const auto targetPluginID = plugin->itemID;
        row.openButton->onClick = [this, targetPluginID]
        {
            if (auto* tr = resolveTrack())
                for (auto* p : tr->pluginList)
                    if (p->itemID == targetPluginID)
                    {
                        p->showWindowExplicitly();
                        break;
                    }
        };

        row.removeButton = std::make_unique<juce::TextButton> ("x");
        row.removeButton->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4b2630));
        row.removeButton->setTooltip ("Remove plugin from track");
        row.removeButton->onClick = [this, targetPluginID]
        {
            if (auto* tr = resolveTrack())
            {
                for (auto* p : tr->pluginList)
                {
                    if (p->itemID == targetPluginID)
                    {
                        p->deleteFromParent();
                        break;
                    }
                }
            }

            rebuildPluginSlots();
            resized();

            if (owner.onMixerChanged != nullptr)
                owner.onMixerChanged();
        };

        addAndMakeVisible (*row.openButton);
        addAndMakeVisible (*row.removeButton);
        pluginSlots.push_back (std::move (row));
    }
}

void MixerComponent::ChannelStrip::showAddPluginMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Built-In Instruments");
    menu.addItem (1, "4OSC Synthesizer");

    const auto vstTypes = engine.getPluginManager().knownPluginList.getTypes();

    if (! vstTypes.isEmpty())
    {
        menu.addSeparator();
        menu.addSectionHeader ("Scanned VST3 Plugins");

        for (int i = 0; i < vstTypes.size(); ++i)
            menu.addItem (100 + i, vstTypes[i].name + " (" + vstTypes[i].manufacturerName + ")");
    }
    else
    {
        menu.addSeparator();
        menu.addItem (99, "(No VST3 cached - click Scan VST3)", false);
    }

    juce::Component::SafePointer<ChannelStrip> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addPluginButton),
                        [safeThis, vstTypes] (int chosenId)
    {
        if (safeThis == nullptr || chosenId <= 0)
            return;

        auto* tr = safeThis->resolveTrack();
        if (tr == nullptr)
            return;

        if (chosenId == 1)
        {
            if (auto synth = safeThis->edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
                tr->pluginList.insertPlugin (synth, 0, nullptr);
        }
        else if (chosenId >= 100)
        {
            const int idx = chosenId - 100;

            if (juce::isPositiveAndBelow (idx, vstTypes.size()))
            {
                const auto pluginTree = te::ExternalPlugin::create (safeThis->engine, vstTypes[idx]);

                if (auto extPlugin = safeThis->edit.getPluginCache().createNewPlugin (pluginTree))
                    tr->pluginList.insertPlugin (extPlugin, 0, nullptr);
            }
        }

        safeThis->rebuildPluginSlots();
        safeThis->resized();

        if (safeThis->owner.onMixerChanged != nullptr)
            safeThis->owner.onMixerChanged();
    });
}

void MixerComponent::ChannelStrip::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.setColour (juce::Colour (0xff1b1f28));
    g.fillRoundedRectangle (bounds.reduced (2).toFloat(), 5.0f);

    g.setColour (juce::Colour (0xff2e3442));
    g.drawRoundedRectangle (bounds.reduced (2).toFloat(), 5.0f, 1.0f);
}

void MixerComponent::ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    titleLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (3);

    auto btnRow = area.removeFromTop (20);
    muteButton.setBounds (btnRow.removeFromLeft (24));
    btnRow.removeFromLeft (4);
    soloButton.setBounds (btnRow.removeFromLeft (24));
    btnRow.removeFromLeft (6);
    addPluginButton.setBounds (btnRow);

    area.removeFromTop (4);

    const int maxVisibleSlots = std::min (3, static_cast<int> (pluginSlots.size()));

    for (int i = 0; i < static_cast<int> (pluginSlots.size()); ++i)
    {
        if (i < maxVisibleSlots)
        {
            auto slotRow = area.removeFromTop (19);
            pluginSlots[i].removeButton->setVisible (true);
            pluginSlots[i].openButton->setVisible (true);
            pluginSlots[i].removeButton->setBounds (slotRow.removeFromRight (20));
            slotRow.removeFromRight (2);
            pluginSlots[i].openButton->setBounds (slotRow);
            area.removeFromTop (2);
        }
        else
        {
            pluginSlots[i].openButton->setVisible (false);
            pluginSlots[i].removeButton->setVisible (false);
        }
    }

    panSlider.setBounds (area.removeFromTop (18));
    dbLabel.setBounds (area.removeFromBottom (16));
    volumeSlider.setBounds (area.reduced (38, 2));
}

//==============================================================================
// MixerComponent
//==============================================================================
MixerComponent::MixerComponent (te::Engine& targetEngine)
    : engine (targetEngine)
{
    setOpaque (true);
    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&stripsContainer, false);
    viewport.setScrollBarsShown (false, true);
}

void MixerComponent::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    rebuildStrips();
}

void MixerComponent::rebuildStrips()
{
    strips.clear();

    if (edit != nullptr)
    {
        auto tracks = te::getAudioTracks (*edit);

        for (int i = 0; i < tracks.size(); ++i)
        {
            auto strip = std::make_unique<ChannelStrip> (*this, engine, *edit, i);
            stripsContainer.addAndMakeVisible (*strip);
            strips.push_back (std::move (strip));
        }
    }

    resized();
}

void MixerComponent::syncFromEdit()
{
    if (edit == nullptr)
        return;

    if (static_cast<int> (strips.size()) != te::getAudioTracks (*edit).size())
    {
        rebuildStrips();
        return;
    }

    for (auto& strip : strips)
        strip->syncState();
}

void MixerComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff13161c));

    g.setColour (juce::Colour (0xff1e222b));
    g.fillRect (0, 0, getWidth(), headerHeight);

    g.setColour (juce::Colour (0xff2e3442));
    g.drawHorizontalLine (headerHeight - 1, 0.0f, static_cast<float> (getWidth()));

    g.setColour (juce::Colour (0xffcbd5e1));
    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    g.drawText ("MIXER & VST3 RACK  |  Click Plugin Chip to Open UI",
                10, 0, getWidth() - 20, headerHeight,
                juce::Justification::centredLeft);
}

void MixerComponent::resized()
{
    auto body = getLocalBounds().withTrimmedTop (headerHeight);
    viewport.setBounds (body);

    const int contentHeight = std::max (120, body.getHeight() - 10);
    const int contentWidth  = std::max (body.getWidth(),
                                        static_cast<int> (strips.size()) * stripWidth);
    stripsContainer.setBounds (0, 0, contentWidth, contentHeight);

    for (int i = 0; i < static_cast<int> (strips.size()); ++i)
        strips[i]->setBounds (i * stripWidth, 0, stripWidth, contentHeight);
}
