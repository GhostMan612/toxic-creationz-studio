#include "ModularRackComponent.h"

namespace
{
    const juce::Colour rackBackground  (0xff1e2128);
    const juce::Colour rackLine        (0xff3a3f4b);
    const juce::Colour cvNodeColour    (0xff2b6f6f);
    const juce::Colour audioNodeColour (0xff44507a);
    const juce::Colour pinColour       (0xffc9a227);
    const juce::Colour wireColour      (0xff7fd1ae);
    const juce::Colour pendingColour   (0xffe0b13a);

    constexpr int nodeWidth  = 180;
    constexpr int nodeHeight = 110;
    constexpr int padX       = 24;
    constexpr int padY       = 20;
    constexpr int pinSize    = 8;
    constexpr int pinY       = 26;
    constexpr int minCanvasW = 760;
    constexpr int minCanvasH = 520;
}

ModularRackComponent::ModularRackComponent (te::Engine& engineRef)
    : engine (engineRef)
{
    addAndMakeVisible (rackSelector);
    addAndMakeVisible (addLfoButton);
    addAndMakeVisible (addWobbleButton);
    addAndMakeVisible (connectModeButton);
    addAndMakeVisible (clearRackButton);
    addAndMakeVisible (rackInfoLabel);
    addAndMakeVisible (nodesViewport);

    nodesViewport.setViewedComponent (&nodesContainer, false);
    nodesViewport.setScrollBarsShown (true, true);

    rackSelector.onChange = [this] { selectRack (rackSelector.getSelectedId() - 1); };

    addLfoButton.onClick = [this] { addCvPluginToRack (CvLfoPlugin::xmlTypeName); };
    addWobbleButton.onClick = [this] { addCvPluginToRack (CvWobbleFilterPlugin::xmlTypeName); };

    connectModeButton.onClick = [this]
    {
        connectMode = ! connectMode;
        awaitingSecondNode = false;
        firstNodeIndex = firstPinIndex = -1;
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
        awaitingSecondNode = false;
        firstNodeIndex = firstPinIndex = -1;
        refresh();
    };

    refresh();
}

ModularRackComponent::~ModularRackComponent()
{
    setEdit (nullptr);
}

void ModularRackComponent::setEdit (te::Edit* editToAttach)
{
    if (edit != nullptr)
        edit->state.removeListener (this);

    edit = editToAttach;
    selectedRack = nullptr;

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

    // Read-only: never create or destroy racks from here. Auto-provisioning
    // a rack on every attach would mutate (and dirty) sessions that were
    // just loaded, and addNewRack() re-enters the ValueTree listeners.
    // No re-entrancy guard is needed: nothing below mutates the tree.
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
    // Strictly read-only UI sync. Never touch UndoManager here: JUCE's
    // undoCurrentTransactionOnly() falls back to undo() when no new transaction
    // is open, so calling it after a Redo with an empty redo stack silently
    // reverts the user's last edit.
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
    const int cols = juce::jmax (1, canvasArea.getWidth() / (nodeWidth + padX));
    const int col  = pluginIndex % cols;
    const int row  = pluginIndex / cols;

    return { canvasArea.getX() + padX + col * (nodeWidth + padX),
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

    // Deterministic, guaranteed non-overlapping fallback slot.
    auto slot = computeGridSlot (pluginIndex, canvasArea);

    // A position near (0,0) means "unset" (or a normalised coord that would
    // stack the card in the corner), so keep the grid slot.
    if (storedPos.x > 0.01f || storedPos.y > 0.01f)
    {
        if (storedPos.x <= 1.0f && storedPos.y <= 1.0f)
        {
            slot.setX (canvasArea.getX() + padX + juce::roundToInt (storedPos.x * (float) usableW));
            slot.setY (canvasArea.getY() + padY + juce::roundToInt (storedPos.y * (float) usableH));
        }
        else
        {
            // Already pixel-valued.
            slot.setX (juce::roundToInt (storedPos.x));
            slot.setY (juce::roundToInt (storedPos.y));
        }
    }

    return slot;
}

int ModularRackComponent::findNodeIndex (te::EditItemID pluginID) const
{
    for (int i = 0; i < nodes.size(); ++i)
        if (nodes.getReference (i).plugin->itemID == pluginID)
            return i;

    return -1;
}

juce::Rectangle<float> ModularRackComponent::getPinRectInCanvas (int nodeIndex, int pinIndex) const
{
    if (nodeIndex < 0 || nodeIndex >= nodes.size())
        return {};

    const auto& n = nodes.getReference (nodeIndex);
    const float px = n.rect.getX() + (pinIndex == 0 ? 6.0f : (float) nodeWidth - 14.0f);

    return juce::Rectangle<float> (px, n.rect.getY() + (float) pinY, (float) pinSize, (float) pinSize);
}

juce::Rectangle<float> ModularRackComponent::getPinRectInRack (int nodeIndex, int pinIndex) const
{
    const auto canvasRect = getPinRectInCanvas (nodeIndex, pinIndex);

    if (canvasRect.isEmpty())
        return canvasRect;

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
    juce::ignoreUnused (isInput);

    const int index = findNodeIndex (pluginID);

    if (index < 0)
        return {};

    const auto pinRect = getPinRectInRack (index, pinIndex);

    if (pinRect.isEmpty())
        return {};

    return pinRect.getCentre();
}

bool ModularRackComponent::verifyNonOverlappingLayoutForSmokeTest() const
{
    if (nodes.size() < 2)
        return true;

    for (int i = 0; i < nodes.size(); ++i)
    {
        // Regression for the original bug: normalised 0..1 coordinates used as
        // pixels piled every card into the canvas top-left corner.
        const auto& canvasRect = nodes.getReference (i).rect;

        if (canvasRect.getX() <= 1.0f && canvasRect.getY() <= 1.0f)
            return false;

        const auto bi = getModuleBoundsInRack (i);

        for (int j = i + 1; j < nodes.size(); ++j)
            if (bi.intersects (getModuleBoundsInRack (j)))
                return false;
    }

    // Every wire endpoint must be the exact pin centre, and the canvas->rack
    // conversion used for cables must agree with getLocalPoint().
    for (const auto& c : connections)
    {
        const auto [srcID, srcPin, dstID, dstPin] = c;
        const te::EditItemID ids[2] = { srcID, dstID };
        const int           pins[2] = { srcPin, dstPin };

        for (int k = 0; k < 2; ++k)
        {
            const int index = findNodeIndex (ids[k]);

            if (index < 0)
                return false;

            const auto canvasPin   = getPinRectInCanvas (index, pins[k]);
            const auto rackPin     = getPinRectInRack (index, pins[k]);
            const auto rackCentre  = getPinCentreInRack (ids[k], pins[k], false);

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

    const auto plugins = selectedRack->getPlugins();

    for (auto* conn : selectedRack->getConnections())
    {
        const int srcIdx = plugins.indexOf (selectedRack->getPluginForID (conn->sourceID.get()));
        const int dstIdx = plugins.indexOf (selectedRack->getPluginForID (conn->destID.get()));

        if (srcIdx < 0 || dstIdx < 0)
            continue;

        // Cable endpoints and pin jacks share this component's coordinate space.
        const auto srcPin = getPinRectInRack (srcIdx, conn->sourcePin.get());
        const auto dstPin = getPinRectInRack (dstIdx, conn->destPin.get());

        if (srcPin.isEmpty() || dstPin.isEmpty())
            continue;

        g.setColour (wireColour);
        g.drawLine (juce::Line<float> (srcPin.getCentre(), dstPin.getCentre()), 2.0f);
    }

    if (awaitingSecondNode && firstNodeIndex >= 0 && firstPinIndex >= 0)
    {
        const auto pending = getPinRectInRack (firstNodeIndex, firstPinIndex);

        if (! pending.isEmpty())
        {
            g.setColour (pendingColour);
            g.drawEllipse (pending.expanded (4.0f), 2.0f);
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

        // Normalised positions can collide; never stack two modules. If the
        // converted slot hits an already-placed card, walk the grid until a
        // free slot is found.
        bool overlaps = false;

        for (auto& r : placed)
            if (r.intersects (slot))
            {
                overlaps = true;
                break;
            }

        if (overlaps)
        {
            const int cols = juce::jmax (1, canvasArea.getWidth() / (nodeWidth + padX));

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
        node.isCvSource = dynamic_cast<CvLfoPlugin*> (plugin) != nullptr;
        nodes.add (node);

        auto view = std::make_unique<NodeView> (*plugin, node.name, node.isCvSource);
        view->setBounds (slot);

        // Capture the stable plugin id, not the array index: rebuilds reorder.
        const auto itemID = plugin->itemID;
        view->onClick = [this, itemID] (NodeView& nv, const juce::MouseEvent& e)
        {
            const int idx = findNodeIndex (itemID);

            if (idx >= 0)
                handleNodeClick (idx, nv, e);
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
    const float x = 0.15f + 0.06f * count;
    const float y = 0.22f + 0.05f * count;

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

void ModularRackComponent::handleNodeClick (int nodeIndex, NodeView& view, const juce::MouseEvent& e)
{
    if (! connectMode)
        return;

    int pin = -1;
    if (view.getPinRect (0).contains (e.position))        pin = 0;
    else if (view.getPinRect (1).contains (e.position))   pin = 1;

    if (pin < 0)
        return;

    if (! awaitingSecondNode)
    {
        awaitingSecondNode = true;
        firstNodeIndex = nodeIndex;
        firstPinIndex = pin;
        repaint();
        return;
    }

    if (firstNodeIndex == nodeIndex)
    {
        awaitingSecondNode = false;
        firstNodeIndex = firstPinIndex = -1;
        repaint();
        return;
    }

    if (firstNodeIndex < 0 || firstNodeIndex >= nodes.size()
        || nodeIndex < 0 || nodeIndex >= nodes.size())
        return;

    const auto& src = nodes.getReference (firstNodeIndex);
    const auto& dst = nodes.getReference (nodeIndex);

    addConnection (src.plugin->itemID, firstPinIndex, dst.plugin->itemID, pin);

    awaitingSecondNode = false;
    firstNodeIndex = firstPinIndex = -1;
}

void ModularRackComponent::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    // Read-only rebuild: never mutate the tree from inside a notification.
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
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawText (name, getLocalBounds().reduced (6), juce::Justification::topLeft);

    g.setColour (pinColour);
    g.fillEllipse ((float) getPinRect (0).getX(), (float) pinY, (float) pinSize, (float) pinSize);
    g.fillEllipse ((float) getPinRect (1).getX(), (float) pinY, (float) pinSize, (float) pinSize);
}

void ModularRackComponent::NodeView::mouseDown (const juce::MouseEvent& e)
{
    if (onClick)
        onClick (*this, e);
}

juce::Rectangle<float> ModularRackComponent::NodeView::getPinRect (int pinIndex) const
{
    const float px = (pinIndex == 0) ? 6.0f : (float) getWidth() - 14.0f;
    return juce::Rectangle<float> (px, (float) pinY, (float) pinSize, (float) pinSize);
}
