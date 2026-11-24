#include "ModularRackComponent.h"

namespace
{
    const juce::Colour rackBackground  (0xff1e2128);
    const juce::Colour rackLine        (0xff3a3f4b);
    const juce::Colour cvNodeColour    (0xff2b6f6f);
    const juce::Colour audioNodeColour (0xff44507a);
    const juce::Colour midiPinColour   (0xff818cf8);
    const juce::Colour cvAudioPinCol   (0xfffbbf24);
    const juce::Colour wireColour      (0xff7fd1ae);
    const juce::Colour pendingColour   (0xffe0b13a);

    constexpr int nodeWidth  = 180;
    constexpr int nodeHeight = 116;
    constexpr int padX       = 48;
    constexpr int padY       = 20;
    constexpr int pinSize    = 8;
    constexpr int minCanvasW = 760;
    constexpr int minCanvasH = 520;
}

ModularRackComponent::ModularRackComponent (te::Engine& engineRef)
    : engine (engineRef)
{
    addAndMakeVisible (rackSelector);
    addAndMakeVisible (addLfoButton);
    addAndMakeVisible (addWobbleButton);
    addAndMakeVisible (addSplitterButton);
    addAndMakeVisible (connectModeButton);
    addAndMakeVisible (clearRackButton);
    addAndMakeVisible (rackInfoLabel);
    addAndMakeVisible (nodesViewport);

    nodesViewport.setViewedComponent (&nodesContainer, false);
    nodesViewport.setScrollBarsShown (true, true);
    nodesContainer.addMouseListener (this, false);

    rackSelector.onChange = [this] { selectRack (rackSelector.getSelectedId() - 1); };

    addLfoButton.onClick = [this] { addCvPluginToRack (CvLfoPlugin::xmlTypeName); };
    addWobbleButton.onClick = [this] { addCvPluginToRack (CvWobbleFilterPlugin::xmlTypeName); };
    addSplitterButton.onClick = [this] { addCvPluginToRack (CvUtilityPlugin::xmlTypeName); };

    connectModeButton.onClick = [this]
    {
        connectMode = ! connectMode;
        awaitingSecondEndpoint = false;
        firstEndpointID = {};
        firstPinIndex = -1;
        connectModeButton.setButtonText (connectMode ? "Connect: On" : "Connect: Off");
        repaint();
    };

    clearRackButton.onClick = [this]
    {
        if (selectedRack == nullptr || edit == nullptr)
            return;

        // RackType exposes no per-plugin removal, so "Clear" removes the rack
        // itself; refresh() then rebuilds an empty canvas.
        edit->getRackList().removeRackType (selectedRack);
        selectedRack = nullptr;
        awaitingSecondEndpoint = false;
        firstEndpointID = {};
        firstPinIndex = -1;
        refresh();
    };

    refresh();
}

ModularRackComponent::~ModularRackComponent()
{
    nodesContainer.removeMouseListener (this);
    setEdit (nullptr);
}

void ModularRackComponent::setEdit (te::Edit* editToAttach)
{
    if (edit != nullptr)
        edit->state.removeListener (this);

    edit = editToAttach;
    selectedRack = nullptr;
    awaitingSecondEndpoint = false;
    firstEndpointID = {};
    firstPinIndex = -1;

    if (edit != nullptr)
    {
        edit->state.addListener (this);

        if (! edit->getRackList().getTypes().isEmpty())
            selectedRack = edit->getRackList().getRackType (0);
    }

    refresh();
}

void ModularRackComponent::refresh()
{
    if (edit == nullptr)
        return;

    rebuildRackSelector();
    rebuildFromRack();
    resized();
}

void ModularRackComponent::rebuildRackSelector()
{
    if (edit == nullptr)
        return;

    const auto& racks = edit->getRackList().getTypes();
    const int previousSelection = rackSelector.getSelectedId();

    rackSelector.clear (juce::dontSendNotification);

    for (int i = 0; i < racks.size(); ++i)
        rackSelector.addItem (racks.getUnchecked (i)->rackName.get(), i + 1);

    if (racks.isEmpty())
    {
        selectedRack = nullptr;
        return;
    }

    const int chosen = juce::jlimit (1, racks.size(), previousSelection == 0 ? 1 : previousSelection);
    rackSelector.setSelectedId (chosen, juce::dontSendNotification);
    selectedRack = edit->getRackList().getRackType (chosen - 1);
}

void ModularRackComponent::syncFromEdit()
{
    refresh();
    repaint();
}

void ModularRackComponent::selectRack (int rackIndex)
{
    if (edit == nullptr)
        return;

    const auto& racks = edit->getRackList().getTypes();

    if (rackIndex >= 0 && rackIndex < racks.size())
        selectedRack = edit->getRackList().getRackType (rackIndex);

    awaitingSecondEndpoint = false;
    firstEndpointID = {};
    firstPinIndex = -1;
    rebuildFromRack();
    repaint();
}

juce::String ModularRackComponent::getSelectedRackName() const
{
    return selectedRack != nullptr ? selectedRack->rackName.get() : juce::String();
}

int ModularRackComponent::getNumPluginsInSelectedRack() const
{
    return selectedRack != nullptr ? selectedRack->getPlugins().size() : 0;
}

int ModularRackComponent::getNumConnectionsInSelectedRack() const
{
    return selectedRack != nullptr ? selectedRack->getConnections().size() : 0;
}

//==============================================================================
// Layout: RackType positions are normalised 0..1; the canvas is pixels.
//==============================================================================
juce::Rectangle<int> ModularRackComponent::getCanvasArea() const
{
    const auto view = nodesViewport.getLocalBounds();

    return { 0, 0,
             juce::jmax (view.getWidth(), minCanvasW),
             juce::jmax (view.getHeight(), minCanvasH) };
}

juce::Rectangle<int> ModularRackComponent::computeGridSlot (int pluginIndex,
                                                            juce::Rectangle<int> canvasArea) const
{
    const int innerW = juce::jmax (nodeWidth, canvasArea.getWidth() - padX);
    const int cols   = juce::jmax (1, innerW / (nodeWidth + 24));
    const int col    = pluginIndex % cols;
    const int row    = pluginIndex / cols;

    return { canvasArea.getX() + padX + col * (nodeWidth + 24),
             canvasArea.getY() + padY + row * (nodeHeight + padY),
             nodeWidth, nodeHeight };
}

juce::Rectangle<int> ModularRackComponent::computeModuleSlotBounds (int pluginIndex,
                                                                    int totalPlugins,
                                                                    juce::Point<float> storedPos,
                                                                    juce::Rectangle<int> canvasArea) const
{
    juce::ignoreUnused (totalPlugins);

    const int usableW = juce::jmax (nodeWidth,  canvasArea.getWidth()  - nodeWidth  - padX * 2);
    const int usableH = juce::jmax (nodeHeight, canvasArea.getHeight() - nodeHeight - padY * 2);

    auto slot = computeGridSlot (pluginIndex, canvasArea);

    if (storedPos.x > 0.01f || storedPos.y > 0.01f)
    {
        if (storedPos.x <= 1.0f && storedPos.y <= 1.0f)
        {
            slot.setX (canvasArea.getX() + padX + juce::roundToInt (storedPos.x * (float) usableW));
            slot.setY (canvasArea.getY() + padY + juce::roundToInt (storedPos.y * (float) usableH));
        }
        else
        {
            slot.setX (juce::jmax (padX, juce::roundToInt (storedPos.x)));
            slot.setY (juce::jmax (padY, juce::roundToInt (storedPos.y)));
        }
    }

    return slot;
}

int ModularRackComponent::findNodeIndex (te::EditItemID pluginID) const
{
    if (! pluginID.isValid())
        return -1;

    for (int i = 0; i < nodes.size(); ++i)
        if (nodes.getReference (i).plugin->itemID == pluginID)
            return i;

    return -1;
}

juce::Rectangle<float> ModularRackComponent::getPinRectInCanvas (int nodeIndex, int pinIndex, bool isInput) const
{
    if (nodeIndex < 0 || nodeIndex >= nodes.size())
        return {};

    const auto& n = nodes.getReference (nodeIndex);
    const float px = n.rect.getX() + (isInput ? 6.0f : static_cast<float> (nodeWidth) - 14.0f);
    const float py = 24.0f + static_cast<float> (juce::jlimit (0, 5, pinIndex) * 15);

    return juce::Rectangle<float> (px, n.rect.getY() + py, static_cast<float> (pinSize), static_cast<float> (pinSize));
}

juce::Rectangle<float> ModularRackComponent::getPinRectInRack (int nodeIndex, int pinIndex, bool isInput) const
{
    const auto canvasRect = getPinRectInCanvas (nodeIndex, pinIndex, isInput);

    if (canvasRect.isEmpty())
        return canvasRect;

    return getLocalArea (&nodesContainer, canvasRect.toNearestInt()).toFloat();
}

juce::Rectangle<float> ModularRackComponent::getRackIoPinRectInCanvas (int pinIndex, bool isRackInputSource) const
{
    const auto canvasArea = getCanvasArea();
    const int clampedPin  = juce::jlimit (0, 4, pinIndex);
    const float px = isRackInputSource ? 12.0f : static_cast<float> (canvasArea.getWidth() - 20);
    const float py = 46.0f + static_cast<float> (clampedPin * 22);

    return { px, py, static_cast<float> (pinSize), static_cast<float> (pinSize) };
}

juce::Rectangle<float> ModularRackComponent::getRackIoPinRectInRack (int pinIndex, bool isRackInputSource) const
{
    const auto canvasRect = getRackIoPinRectInCanvas (pinIndex, isRackInputSource);
    return getLocalArea (&nodesContainer, canvasRect.toNearestInt()).toFloat();
}

juce::Rectangle<int> ModularRackComponent::getModuleBoundsInRack (int index) const
{
    if (index < 0 || index >= nodes.size())
        return {};

    return getLocalArea (&nodesContainer, nodes.getReference (index).rect.toNearestInt());
}

juce::Point<float> ModularRackComponent::getPinCentreInRack (te::EditItemID pluginID,
                                                             int pinIndex,
                                                             bool isInput) const
{
    if (! pluginID.isValid())
    {
        // Outside RackType, sourceID={} means Rack Input rail and destID={} means Rack Output rail.
        const auto ioRect = getRackIoPinRectInRack (pinIndex, ! isInput);
        return ioRect.isEmpty() ? juce::Point<float>{} : ioRect.getCentre();
    }

    const int index = findNodeIndex (pluginID);

    if (index < 0)
        return {};

    const auto pinRect = getPinRectInRack (index, pinIndex, isInput);
    return pinRect.isEmpty() ? juce::Point<float>{} : pinRect.getCentre();
}

bool ModularRackComponent::verifyNonOverlappingLayoutForSmokeTest() const
{
    if (nodes.size() < 2)
        return true;

    for (int i = 0; i < nodes.size(); ++i)
    {
        const auto& canvasRect = nodes.getReference (i).rect;

        if (canvasRect.getX() <= 1.0f && canvasRect.getY() <= 1.0f)
            return false;

        const auto bi = getModuleBoundsInRack (i);

        for (int j = i + 1; j < nodes.size(); ++j)
            if (bi.intersects (getModuleBoundsInRack (j)))
                return false;
    }

    for (const auto& c : connections)
    {
        const auto [srcID, srcPin, dstID, dstPin] = c;
        const te::EditItemID ids[2] = { srcID, dstID };
        const int           pins[2] = { srcPin, dstPin };
        const bool       isInput[2] = { false, true };

        for (int k = 0; k < 2; ++k)
        {
            if (! ids[k].isValid())
            {
                const auto ioCanvas = getRackIoPinRectInCanvas (pins[k], k == 0);
                const auto ioRack   = getRackIoPinRectInRack (pins[k], k == 0);
                const auto ioCentre = getPinCentreInRack (ids[k], pins[k], isInput[k]);

                if (ioCanvas.isEmpty() || ioRack.isEmpty() || ioCentre.isOrigin())
                    return false;

                const auto expected = getLocalPoint (&nodesContainer, ioCanvas.getCentre());

                if (ioCentre.getDistanceFrom (expected) > 0.5f)
                    return false;

                continue;
            }

            const int index = findNodeIndex (ids[k]);

            if (index < 0)
                return false;

            const auto canvasPin  = getPinRectInCanvas (index, pins[k], isInput[k]);
            const auto rackPin    = getPinRectInRack (index, pins[k], isInput[k]);
            const auto rackCentre = getPinCentreInRack (ids[k], pins[k], isInput[k]);

            if (canvasPin.isEmpty() || rackPin.isEmpty() || rackCentre.isOrigin())
                return false;

            const auto expected = getLocalPoint (&nodesContainer, canvasPin.getCentre());

            if (rackCentre.getDistanceFrom (expected) > 0.5f)
                return false;
        }
    }

    return true;
}

//==============================================================================
void ModularRackComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    auto toolbar = area.removeFromTop (30);

    rackSelector.setBounds (toolbar.removeFromLeft (190));
    toolbar.removeFromLeft (6);
    addLfoButton.setBounds (toolbar.removeFromLeft (80));
    addWobbleButton.setBounds (toolbar.removeFromLeft (90));
    addSplitterButton.setBounds (toolbar.removeFromLeft (94));
    connectModeButton.setBounds (toolbar.removeFromLeft (100));
    clearRackButton.setBounds (toolbar.removeFromLeft (90));
    rackInfoLabel.setBounds (toolbar);

    nodesViewport.setBounds (area);

    const auto canvasArea = getCanvasArea();
    nodesContainer.setSize (canvasArea.getWidth(), canvasArea.getHeight());
}

void ModularRackComponent::paint (juce::Graphics& g)
{
    g.fillAll (rackBackground);
    g.setColour (rackLine);
    g.drawRect (getLocalBounds().toFloat(), 1.0f);

    if (selectedRack == nullptr)
    {
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (15.0f)));
        g.drawText ("No rack selected", getLocalBounds(), juce::Justification::centred);
        return;
    }

    juce::Graphics::ScopedSaveState rackClip (g);
    g.reduceClipRegion (nodesViewport.getBounds());

    const auto inRailRect  = getLocalArea (&nodesContainer, juce::Rectangle<int> (2, 16, 36, 130)).toFloat();
    const auto canvasW     = getCanvasArea().getWidth();
    const auto outRailRect = getLocalArea (&nodesContainer, juce::Rectangle<int> (canvasW - 38, 16, 36, 130)).toFloat();

    g.setColour (juce::Colour (0xff242a38));
    g.fillRoundedRectangle (inRailRect, 4.0f);
    g.fillRoundedRectangle (outRailRect, 4.0f);
    g.setColour (juce::Colour (0xff3a4254));
    g.drawRoundedRectangle (inRailRect, 4.0f, 1.0f);
    g.drawRoundedRectangle (outRailRect, 4.0f, 1.0f);

    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.setColour (juce::Colour (0xffcbd5e1));
    g.drawText ("IN", inRailRect.withHeight (22.0f), juce::Justification::centred);
    g.drawText ("OUT", outRailRect.withHeight (22.0f), juce::Justification::centred);

    static const char* railLabels[3] = { "M", "L", "R" };

    for (int p = 0; p < 3; ++p)
    {
        const auto col = (p == 0) ? midiPinColour : cvAudioPinCol;
        const auto inPinR  = getRackIoPinRectInRack (p, true);
        const auto outPinR = getRackIoPinRectInRack (p, false);

        g.setColour (col);
        g.fillEllipse (inPinR);
        g.fillEllipse (outPinR);

        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.setColour (juce::Colour (0xffcbd5e1));
        g.drawText (railLabels[p],
                    juce::Rectangle<float> (inPinR.getRight() + 3.0f, inPinR.getY() - 2.0f, 14.0f, 12.0f),
                    juce::Justification::centredLeft);
        g.drawText (railLabels[p],
                    juce::Rectangle<float> (outPinR.getX() - 16.0f, outPinR.getY() - 2.0f, 14.0f, 12.0f),
                    juce::Justification::centredRight);
    }
}

void ModularRackComponent::paintOverChildren (juce::Graphics& g)
{
    if (selectedRack == nullptr)
        return;

    juce::Graphics::ScopedSaveState wireClip (g);
    g.reduceClipRegion (nodesViewport.getBounds());

    const auto plugins = selectedRack->getPlugins();

    for (auto* conn : selectedRack->getConnections())
    {
        const auto srcID  = conn->sourceID.get();
        const auto dstID  = conn->destID.get();
        const int srcIdx  = srcID.isValid() ? plugins.indexOf (selectedRack->getPluginForID (srcID)) : -1;
        const int dstIdx  = dstID.isValid() ? plugins.indexOf (selectedRack->getPluginForID (dstID)) : -1;

        if ((srcID.isValid() && srcIdx < 0) || (dstID.isValid() && dstIdx < 0))
            continue;

        const auto srcPin = srcID.isValid()
                                ? getPinRectInRack (srcIdx, conn->sourcePin.get(), false)
                                : getRackIoPinRectInRack (conn->sourcePin.get(), true);
        const auto dstPin = dstID.isValid()
                                ? getPinRectInRack (dstIdx, conn->destPin.get(), true)
                                : getRackIoPinRectInRack (conn->destPin.get(), false);

        if (srcPin.isEmpty() || dstPin.isEmpty())
            continue;

        const bool isMidiWire = (conn->sourcePin.get() == 0 && conn->destPin.get() == 0);
        g.setColour (isMidiWire ? midiPinColour : wireColour);
        g.drawLine (juce::Line<float> (srcPin.getCentre(), dstPin.getCentre()), 2.0f);
    }

    if (awaitingSecondEndpoint && firstPinIndex >= 0)
    {
        juce::Rectangle<float> pending;

        if (! firstEndpointID.isValid())
            pending = getRackIoPinRectInRack (firstPinIndex, ! firstIsInput);
        else
            pending = getPinRectInRack (findNodeIndex (firstEndpointID), firstPinIndex, firstIsInput);

        if (! pending.isEmpty())
        {
            g.setColour (pendingColour);
            g.drawEllipse (pending.expanded (4.0f), 2.0f);
        }
    }
}

void ModularRackComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent != &nodesContainer || ! connectMode || selectedRack == nullptr)
        return;

    const auto rel = e.getEventRelativeTo (this).position;

    for (int p = 0; p < 3; ++p)
    {
        if (getRackIoPinRectInRack (p, true).expanded (4.0f).contains (rel))
        {
            handleEndpointSelection ({}, p, false);
            return;
        }

        if (getRackIoPinRectInRack (p, false).expanded (4.0f).contains (rel))
        {
            handleEndpointSelection ({}, p, true);
            return;
        }
    }
}

void ModularRackComponent::rebuildFromRack()
{
    while (nodesContainer.getNumChildComponents() > 0)
        delete nodesContainer.removeChildComponent (0);

    nodes.clear();
    connections.clear();

    if (selectedRack == nullptr)
    {
        rackInfoLabel.setText ("No Rack", juce::dontSendNotification);
        return;
    }

    const auto plugins = selectedRack->getPlugins();
    const auto canvasArea = getCanvasArea();

    nodesContainer.setSize (canvasArea.getWidth(), canvasArea.getHeight());

    juce::Array<juce::Rectangle<int>> placed;

    for (int i = 0; i < plugins.size(); ++i)
    {
        auto* plugin = plugins.getUnchecked (i);
        const auto storedPos = selectedRack->getPluginPosition (i);

        auto slot = computeModuleSlotBounds (i, plugins.size(), storedPos, canvasArea);

        bool overlaps = false;

        for (auto& r : placed)
            if (r.intersects (slot))
            {
                overlaps = true;
                break;
            }

        if (overlaps)
        {
            const int innerW = juce::jmax (nodeWidth, canvasArea.getWidth() - padX);
            const int cols   = juce::jmax (1, innerW / (nodeWidth + 24));

            for (int candidate = 0; candidate < cols * 256; ++candidate)
            {
                auto gridSlot = computeGridSlot (candidate, canvasArea);
                bool hit = false;

                for (auto& r : placed)
                    if (r.intersects (gridSlot))
                    {
                        hit = true;
                        break;
                    }

                if (! hit)
                {
                    slot = gridSlot;
                    break;
                }
            }
        }

        placed.add (slot);

        RackNode node;
        node.plugin = plugin;
        node.rect = slot.toFloat();
        node.name = plugin->getName();
        node.isCvSource = (dynamic_cast<CvLfoPlugin*> (plugin) != nullptr
                           || dynamic_cast<CvUtilityPlugin*> (plugin) != nullptr);
        nodes.add (node);

        auto view = std::make_unique<NodeView> (*plugin, node.name, node.isCvSource);
        view->setBounds (slot);

        const auto itemID = plugin->itemID;
        view->onClick = [this, itemID] (NodeView& nv, const juce::MouseEvent& e)
        {
            const int idx = findNodeIndex (itemID);

            if (idx >= 0)
                handleNodeClick (idx, nv, e);
        };

        view->onDoubleClick = [this, itemID] (NodeView&)
        {
            if (connectMode || selectedRack == nullptr)
                return;

            if (auto* target = selectedRack->getPluginForID (itemID))
                target->showWindowExplicitly();
        };

        nodesContainer.addAndMakeVisible (view.release());
    }

    for (auto* conn : selectedRack->getConnections())
        connections.add (std::make_tuple (conn->sourceID.get(), conn->sourcePin.get(),
                                          conn->destID.get(), conn->destPin.get()));

    rackInfoLabel.setText (selectedRack->rackName.get()
                               + "  |  " + juce::String (plugins.size()) + " nodes, "
                               + juce::String (selectedRack->getConnections().size()) + " cables",
                           juce::dontSendNotification);
}

void ModularRackComponent::addCvPluginToRack (const char* xmlTypeName)
{
    if (edit == nullptr)
        return;

    if (selectedRack == nullptr)
    {
        if (edit->getRackList().getTypes().isEmpty())
        {
            if (auto newRack = edit->getRackList().addNewRack())
                newRack->rackName = "Default CV Rack";
        }

        rebuildRackSelector();
        rebuildFromRack();
        resized();
    }

    if (selectedRack == nullptr)
        return;

    auto plugin = edit->getPluginCache().createNewPlugin (xmlTypeName, {});
    if (plugin == nullptr)
        return;

    const float count = (float) selectedRack->getPlugins().size();
    const float x = 0.08f + 0.06f * count;
    const float y = 0.16f + 0.05f * count;

    if (selectedRack->addPlugin (plugin, { x, y }, false))
    {
        rebuildFromRack();
        repaint();
    }
}

void ModularRackComponent::addConnection (te::EditItemID source, int sourcePin,
                                          te::EditItemID dest, int destPin)
{
    if (selectedRack == nullptr)
        return;

    selectedRack->addConnection (source, sourcePin, dest, destPin);
    rebuildFromRack();
    repaint();
}

void ModularRackComponent::removeConnection (te::EditItemID source, int sourcePin,
                                             te::EditItemID dest, int destPin)
{
    if (selectedRack == nullptr)
        return;

    selectedRack->removeConnection (source, sourcePin, dest, destPin);
    rebuildFromRack();
    repaint();
}

void ModularRackComponent::handleEndpointSelection (te::EditItemID endpointID, int pinIndex, bool isInputPin)
{
    if (selectedRack == nullptr)
        return;

    if (! awaitingSecondEndpoint)
    {
        awaitingSecondEndpoint = true;
        firstEndpointID = endpointID;
        firstPinIndex   = pinIndex;
        firstIsInput    = isInputPin;
        repaint();
        return;
    }

    awaitingSecondEndpoint = false;

    if (firstIsInput == isInputPin
        || (firstEndpointID.isValid() && firstEndpointID == endpointID))
    {
        firstEndpointID = {};
        firstPinIndex = -1;
        repaint();
        return;
    }

    const te::EditItemID srcID = firstIsInput ? endpointID : firstEndpointID;
    const int srcPin           = firstIsInput ? pinIndex   : firstPinIndex;
    const te::EditItemID dstID = firstIsInput ? firstEndpointID : endpointID;
    const int dstPin           = firstIsInput ? firstPinIndex   : pinIndex;

    firstEndpointID = {};
    firstPinIndex = -1;

    bool exists = false;

    for (auto* conn : selectedRack->getConnections())
    {
        if (conn->sourceID.get() == srcID && conn->sourcePin.get() == srcPin
            && conn->destID.get() == dstID && conn->destPin.get() == dstPin)
        {
            exists = true;
            break;
        }
    }

    if (exists)
        removeConnection (srcID, srcPin, dstID, dstPin);
    else
        addConnection (srcID, srcPin, dstID, dstPin);
}

void ModularRackComponent::handleNodeClick (int nodeIndex, NodeView& view, const juce::MouseEvent& e)
{
    if (! connectMode || ! juce::isPositiveAndBelow (nodeIndex, nodes.size()))
        return;

    juce::StringArray ins, outs;
    view.plugin.getChannelNames (&ins, &outs);

    const int maxInPin  = juce::jlimit (0, 4, ins.size());
    const int maxOutPin = juce::jlimit (0, 4, outs.size());

    for (int p = 0; p <= maxInPin; ++p)
    {
        if (view.getPinRect (p, true).expanded (3.0f).contains (e.position))
        {
            handleEndpointSelection (view.plugin.itemID, p, true);
            return;
        }
    }

    for (int p = 0; p <= maxOutPin; ++p)
    {
        if (view.getPinRect (p, false).expanded (3.0f).contains (e.position))
        {
            handleEndpointSelection (view.plugin.itemID, p, false);
            return;
        }
    }
}

void ModularRackComponent::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    rebuildFromRack();
    repaint();
}

void ModularRackComponent::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&)
{
    rebuildRackSelector();
    rebuildFromRack();
    repaint();
}

void ModularRackComponent::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
    rebuildRackSelector();
    rebuildFromRack();
    repaint();
}

void ModularRackComponent::valueTreeChildOrderChanged (juce::ValueTree&, int, int) {}
void ModularRackComponent::valueTreeParentChanged (juce::ValueTree&) {}
void ModularRackComponent::valueTreeRedirected (juce::ValueTree&) {}

//==============================================================================
ModularRackComponent::NodeView::NodeView (te::Plugin& pluginToView,
                                          const juce::String& displayName,
                                          bool cvSource)
    : plugin (pluginToView),
      name (displayName),
      isCv (cvSource)
{
}

void ModularRackComponent::NodeView::paint (juce::Graphics& g)
{
    g.fillAll (isCv ? cvNodeColour : audioNodeColour);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawRect (getLocalBounds().toFloat(), 1.0f);

    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
    g.drawText (name, 6, 3, getWidth() - 12, 18, juce::Justification::centredLeft, true);

    juce::StringArray ins, outs;
    plugin.getChannelNames (&ins, &outs);

    const int maxInPin  = juce::jlimit (0, 4, ins.size());
    const int maxOutPin = juce::jlimit (0, 4, outs.size());

    g.setFont (juce::Font (juce::FontOptions (9.5f)));

    for (int p = 0; p <= maxInPin; ++p)
    {
        const auto r = getPinRect (p, true);
        g.setColour (p == 0 ? midiPinColour : cvAudioPinCol);
        g.fillEllipse (r);

        const juce::String lbl = (p == 0) ? "MIDI" : ins[p - 1];
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawText (lbl,
                    juce::Rectangle<float> (r.getRight() + 4.0f, r.getY() - 2.0f, 68.0f, 12.0f),
                    juce::Justification::centredLeft, true);
    }

    for (int p = 0; p <= maxOutPin; ++p)
    {
        const auto r = getPinRect (p, false);
        g.setColour (p == 0 ? midiPinColour : cvAudioPinCol);
        g.fillEllipse (r);

        const juce::String lbl = (p == 0) ? "MIDI" : outs[p - 1];
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawText (lbl,
                    juce::Rectangle<float> (r.getX() - 74.0f, r.getY() - 2.0f, 70.0f, 12.0f),
                    juce::Justification::centredRight, true);
    }
}

void ModularRackComponent::NodeView::mouseDown (const juce::MouseEvent& e)
{
    if (onClick)
        onClick (*this, e);
}

void ModularRackComponent::NodeView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick)
        onDoubleClick (*this);
}

juce::Rectangle<float> ModularRackComponent::NodeView::getPinRect (int pinIndex, bool isInput) const
{
    const float px = isInput ? 6.0f : static_cast<float> (getWidth()) - 14.0f;
    const float py = 24.0f + static_cast<float> (juce::jlimit (0, 5, pinIndex) * 15);
    return { px, py, static_cast<float> (pinSize), static_cast<float> (pinSize) };
}
