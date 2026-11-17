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

    constexpr int nodeWidth  = 120;
    constexpr int nodeHeight = 92;
    constexpr int pinSize    = 8;
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
        // itself; refresh() then provisions a fresh empty one.
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
    // addNewRack() fires valueTreeChildAdded synchronously, which re-enters the
    // listeners below. Never let a notification re-enter refresh(): the rack
    // list has not been updated yet at that point, so a nested call would see
    // an empty list and provision yet another rack (unbounded recursion).
    if (refreshDepth > 0)
        return;

    ++refreshDepth;

    if (edit != nullptr)
    {
        // Read-only: never create or destroy racks from here. Auto-provisioning
        // a rack on every attach would mutate (and dirty) sessions that were
        // just loaded, and addNewRack() re-enters the ValueTree listeners.
        rebuildRackSelector();
        rebuildFromRack();
        resized();
    }

    --refreshDepth;
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
    if (edit != nullptr)
        edit->getUndoManager().undoCurrentTransactionOnly();
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

    const auto viewArea = nodesViewport.getLocalBounds();
    nodesContainer.setSize (juce::jmax (viewArea.getWidth(), 700),
                            juce::jmax (viewArea.getHeight(), 500));
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

        auto srcPin = getPinRect (srcIdx, conn->sourcePin.get());
        auto dstPin = getPinRect (dstIdx, conn->destPin.get());

        g.setColour (wireColour);
        g.drawLine (juce::Line<float> (srcPin.getCentre(), dstPin.getCentre()), 2.0f);
    }

    if (awaitingSecondNode && firstNodeIndex >= 0 && firstPinIndex >= 0)
    {
        g.setColour (pendingColour);
        g.drawEllipse (getPinRect (firstNodeIndex, firstPinIndex).expanded (4.0f), 2.0f);
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

    for (int i = 0; i < plugins.size(); ++i)
    {
        auto* plugin = plugins.getUnchecked (i);
        const auto pos = selectedRack->getPluginPosition (i);

        RackNode node;
        node.plugin = plugin;
        node.rect = juce::Rectangle<float> (pos.x, pos.y, (float) nodeWidth, (float) nodeHeight);
        node.name = plugin->getName();
        node.isCvSource = dynamic_cast<CvLfoPlugin*> (plugin) != nullptr;
        nodes.add (node);

        auto view = std::make_unique<NodeView> (*plugin, node.name, node.isCvSource);
        view->setBounds (node.rect.toNearestInt());
        view->onClick = [this, i] (NodeView& nv, const juce::MouseEvent& e)
        {
            handleNodeClick (i, nv, e);
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

juce::Rectangle<float> ModularRackComponent::getPinRect (int nodeIndex, int pinIndex) const
{
    if (nodeIndex < 0 || nodeIndex >= nodes.size())
        return {};

    const auto& n = nodes.getReference (nodeIndex);
    const float px = n.rect.getX() + (pinIndex == 0 ? 6.0f : (float) nodeWidth - 14.0f);
    return juce::Rectangle<float> (px, n.rect.getY() + 26.0f, (float) pinSize, (float) pinSize);
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
    g.fillEllipse ((float) getPinRect (0).getX(), 26.0f, (float) pinSize, (float) pinSize);
    g.fillEllipse ((float) getPinRect (1).getX(), 26.0f, (float) pinSize, (float) pinSize);
}

void ModularRackComponent::NodeView::mouseDown (const juce::MouseEvent& e)
{
    if (onClick)
        onClick (*this, e);
}

juce::Rectangle<float> ModularRackComponent::NodeView::getPinRect (int pinIndex) const
{
    const float px = (pinIndex == 0) ? 6.0f : (float) getWidth() - 14.0f;
    return juce::Rectangle<float> (px, 26.0f, (float) pinSize, (float) pinSize);
}
