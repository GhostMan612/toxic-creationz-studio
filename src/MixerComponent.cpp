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

    auto configureEqControl = [this] (juce::Label& lbl, juce::Slider& slider, int bandIndex, const juce::String& tooltip)
    {
        addAndMakeVisible (lbl);
        lbl.setFont (juce::Font (juce::FontOptions (10.5f)));
        lbl.setColour (juce::Label::textColourId, juce::Colour (0xff94a3b8));
        lbl.setJustificationType (juce::Justification::centredLeft);

        addAndMakeVisible (slider);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (-18.0, 18.0, 0.1);
        slider.setDoubleClickReturnValue (true, 0.0);
        slider.setTooltip (tooltip);
        slider.onValueChange = [this, bandIndex, &slider]
        {
            applyEqBandChange (bandIndex, static_cast<float> (slider.getValue()));
        };
    };

    configureEqControl (eqLowLabel, eqLowSlider, 0, "3-Band EQ Low Shelf Gain (-18 to +18 dB)");
    configureEqControl (eqMidLabel, eqMidSlider, 1, "3-Band EQ Mid Band Gain (-18 to +18 dB)");
    configureEqControl (eqHighLabel, eqHighSlider, 2, "3-Band EQ High Shelf Gain (-18 to +18 dB)");

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

MixerComponent::ChannelStrip::~ChannelStrip()
{
    detachMeterClient();
}

te::AudioTrack* MixerComponent::ChannelStrip::resolveTrack() const
{
    auto tracks = te::getAudioTracks (edit);

    if (juce::isPositiveAndBelow (trackIndex, tracks.size()))
        return tracks[trackIndex];

    return nullptr;
}

void MixerComponent::ChannelStrip::attachMeterClient (te::AudioTrack* t)
{
    te::LevelMeasurer* target = nullptr;

    if (t != nullptr)
        if (auto* meterPlugin = t->getLevelMeterPlugin())
            target = &meterPlugin->measurer;

    if (attachedMeasurer.get() == target)
        return;

    detachMeterClient();

    if (target != nullptr)
    {
        attachedMeasurer = target;
        target->addClient (meterClient);
    }
}

void MixerComponent::ChannelStrip::detachMeterClient()
{
    if (auto* m = attachedMeasurer.get())
        m->removeClient (meterClient);

    attachedMeasurer = nullptr;
}

void MixerComponent::ChannelStrip::applyEqBandChange (int bandIndex, float dbValue)
{
    auto* t = resolveTrack();
    if (t == nullptr)
        return;

    edit.getUndoManager().beginNewTransaction ("Adjust Track EQ");

    auto* eq = t->getEqualiserPlugin();
    bool inserted = false;

    if (eq == nullptr)
    {
        if (auto eqPlugin = edit.getPluginCache().createNewPlugin (te::EqualiserPlugin::xmlTypeName, {}))
            t->pluginList.insertPlugin (eqPlugin, MixerComponent::getInsertIndexForNewPlugin (*t), nullptr);

        eq = t->getEqualiserPlugin();
        inserted = (eq != nullptr);
    }

    if (eq == nullptr)
        return;

    const float clamped = juce::jlimit (-18.0f, 18.0f, dbValue);

    if (bandIndex == 0)
    {
        eq->loGainValue = clamped;
        if (eq->loGain != nullptr) eq->loGain->updateFromAttachedValue();
    }
    else if (bandIndex == 1)
    {
        eq->midGainValue1 = clamped;
        if (eq->midGain1 != nullptr) eq->midGain1->updateFromAttachedValue();
    }
    else if (bandIndex == 2)
    {
        eq->hiGainValue = clamped;
        if (eq->hiGain != nullptr) eq->hiGain->updateFromAttachedValue();
    }

    if (inserted)
    {
        rebuildPluginSlots();
        owner.resized();
    }

    if (owner.onMixerChanged != nullptr)
        owner.onMixerChanged();
}

void MixerComponent::ChannelStrip::syncState()
{
    auto* t = resolveTrack();
    if (t == nullptr)
        return;

    attachMeterClient (t);

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

    if (auto* eq = t->getEqualiserPlugin())
    {
        eqLowSlider.setValue (eq->loGainValue.get(), juce::dontSendNotification);
        eqMidSlider.setValue (eq->midGainValue1.get(), juce::dontSendNotification);
        eqHighSlider.setValue (eq->hiGainValue.get(), juce::dontSendNotification);
    }
    else
    {
        eqLowSlider.setValue (0.0, juce::dontSendNotification);
        eqMidSlider.setValue (0.0, juce::dontSendNotification);
        eqHighSlider.setValue (0.0, juce::dontSendNotification);
    }
}

void MixerComponent::ChannelStrip::pollMeter()
{
    if (attachedMeasurer == nullptr)
        attachMeterClient (resolveTrack());

    const float dBL = meterClient.getAndClearAudioLevel (0).dB;
    const float dBR = meterClient.getAndClearAudioLevel (1).dB;

    auto dbToNorm = [] (float dB)
    {
        if (dB <= -60.0f)
            return 0.0f;

        return juce::jlimit (0.0f, 1.0f, (dB + 60.0f) / 60.0f);
    };

    float nextL = std::max (dbToNorm (dBL), meterLevelL * 0.85f);
    float nextR = std::max (dbToNorm (dBR), meterLevelR * 0.85f);

    if (nextL < 0.005f) nextL = 0.0f;
    if (nextR < 0.005f) nextR = 0.0f;

    if (std::abs (nextL - meterLevelL) > 0.004f || std::abs (nextR - meterLevelR) > 0.004f)
    {
        meterLevelL = nextL;
        meterLevelR = nextR;

        if (! meterBounds.isEmpty())
            repaint (meterBounds);
    }
}

int MixerComponent::ChannelStrip::getMinimumHeight() const
{
    // Title + M/S/add row + 21px per plugin slot + 3 EQ rows + pan + minimum fader + dB label + insets.
    return 191 + 21 * static_cast<int> (pluginSlots.size());
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

            syncState();
            rebuildPluginSlots();
            owner.resized();

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
    menu.addSeparator();
    menu.addSectionHeader ("Built-In Effects");
    menu.addItem (2, "4-Band Equaliser (EQ)");

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
                tr->pluginList.insertPlugin (synth, MixerComponent::getInsertIndexForNewPlugin (*tr), nullptr);
        }
        else if (chosenId == 2)
        {
            if (tr->getEqualiserPlugin() == nullptr)
            {
                safeThis->edit.getUndoManager().beginNewTransaction ("Add Track EQ");
                if (auto eq = safeThis->edit.getPluginCache().createNewPlugin (te::EqualiserPlugin::xmlTypeName, {}))
                    tr->pluginList.insertPlugin (eq, MixerComponent::getInsertIndexForNewPlugin (*tr), nullptr);
            }
        }
        else if (chosenId >= 100)
        {
            const int idx = chosenId - 100;

            if (juce::isPositiveAndBelow (idx, vstTypes.size()))
            {
                const auto pluginTree = te::ExternalPlugin::create (safeThis->engine, vstTypes[idx]);

                if (auto extPlugin = safeThis->edit.getPluginCache().createNewPlugin (pluginTree))
                    tr->pluginList.insertPlugin (extPlugin, MixerComponent::getInsertIndexForNewPlugin (*tr), nullptr);
            }
        }

        safeThis->syncState();
        safeThis->rebuildPluginSlots();
        safeThis->owner.resized();

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

    if (! meterBounds.isEmpty())
    {
        g.setColour (juce::Colour (0xff0f131a));
        g.fillRoundedRectangle (meterBounds.toFloat(), 2.0f);
        g.setColour (juce::Colour (0xff2e3442));
        g.drawRoundedRectangle (meterBounds.toFloat(), 2.0f, 1.0f);

        const auto inner = meterBounds.reduced (2);
        const int barW = std::max (2, (inner.getWidth() - 2) / 2);
        const float levels[2] = { meterLevelL, meterLevelR };

        for (int ch = 0; ch < 2; ++ch)
        {
            const int bx = inner.getX() + ch * (barW + 2);
            const int fillH = juce::roundToInt (levels[ch] * static_cast<float> (inner.getHeight()));

            if (fillH > 0)
            {
                const juce::Colour meterCol = levels[ch] > 0.90f
                                                  ? juce::Colour (0xffef4444)
                                                  : (levels[ch] > 0.70f ? juce::Colour (0xfff59e0b)
                                                                        : juce::Colour (0xff22c55e));
                g.setColour (meterCol);
                g.fillRect (bx, inner.getBottom() - fillH, barW, fillH);
            }
        }
    }
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

    // All slots are laid out; if a strip needs more height than the pane
    // offers, the mixer's viewport scrolls vertically (see resized()).
    for (int i = 0; i < static_cast<int> (pluginSlots.size()); ++i)
    {
        auto slotRow = area.removeFromTop (19);
        pluginSlots[i].removeButton->setVisible (true);
        pluginSlots[i].openButton->setVisible (true);
        pluginSlots[i].removeButton->setBounds (slotRow.removeFromRight (20));
        slotRow.removeFromRight (2);
        pluginSlots[i].openButton->setBounds (slotRow);
        area.removeFromTop (2);
    }

    auto layoutEqRow = [&area] (juce::Label& lbl, juce::Slider& slider)
    {
        auto row = area.removeFromTop (16);
        lbl.setBounds (row.removeFromLeft (26));
        slider.setBounds (row);
        area.removeFromTop (2);
    };

    layoutEqRow (eqLowLabel, eqLowSlider);
    layoutEqRow (eqMidLabel, eqMidSlider);
    layoutEqRow (eqHighLabel, eqHighSlider);
    area.removeFromTop (4);

    panSlider.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);
    dbLabel.setBounds (area.removeFromBottom (16));

    meterBounds = area.removeFromRight (18).reduced (1, 2);
    volumeSlider.setBounds (area.reduced (26, 2));
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
    viewport.setScrollBarsShown (true, true);
}

MixerComponent::~MixerComponent()
{
    setEdit (nullptr);
}

void MixerComponent::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    rebuildStrips();
}

void MixerComponent::refreshMeters()
{
    if (edit == nullptr)
        return;

    for (auto& strip : strips)
        strip->pollMeter();
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

int MixerComponent::getInsertIndexForNewPlugin (te::AudioTrack& track)
{
    int index = 0;

    for (auto* plugin : track.pluginList)
    {
        // The terminal utilities sit at the end of the chain; a new insert
        // goes just before the first of them. If there are none, the index
        // is past the end and insertPlugin appends.
        if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) != nullptr
            || dynamic_cast<te::LevelMeterPlugin*> (plugin) != nullptr)
        {
            break;
        }

        ++index;
    }

    return index;
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

    int contentHeight = std::max (120, body.getHeight() - 10);

    for (auto& strip : strips)
        contentHeight = std::max (contentHeight, strip->getMinimumHeight());

    const int contentWidth  = std::max (body.getWidth(),
                                        static_cast<int> (strips.size()) * stripWidth);
    stripsContainer.setBounds (0, 0, contentWidth, contentHeight);

    for (int i = 0; i < static_cast<int> (strips.size()); ++i)
        strips[i]->setBounds (i * stripWidth, 0, stripWidth, contentHeight);
}
