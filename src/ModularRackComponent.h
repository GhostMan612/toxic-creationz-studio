#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "CvRackPlugins.h"

#include <functional>
#include <tuple>

namespace te = tracktion::engine;

//==============================================================================
// Reason-style modular CV/audio patchbay panel.
//
// A RackType is a node graph: each node is a Plugin (a synth, an effect, one
// of the built-in CV utilities, ...) and each connection routes one of its
// output pins to one of another node's input pins. RackTypeList owns the
// racks in the Edit and RackInstance wraps one into a track's plugin list.
//
// Coordinate spaces: RackType stores plugin positions as NORMALISED 0..1
// values, while the node cards live inside a scrollable Viewport. Layout maps
// normalised -> canvas pixels, and every cable endpoint is converted with
// getLocalArea() so pins, nodes and wires always share one space.
//==============================================================================
class ModularRackComponent : public juce::Component,
                             private juce::ValueTree::Listener
{
public:
    ModularRackComponent (te::Engine& engineRef);
    ~ModularRackComponent() override;

    void setEdit (te::Edit* editToAttach);
    void resized() override;
    void paint (juce::Graphics& g) override;
    void refresh();

    // Read-only UI synchronisation. Must never touch the UndoManager.
    void syncFromEdit();
    void selectRack (int rackIndex);

    juce::String getSelectedRackName() const;
    int getNumPluginsInSelectedRack() const;
    int getNumConnectionsInSelectedRack() const;

    // Layout introspection, in THIS component's coordinate space (the same
    // space the cable renderer uses). Exposed for --smoke-test assertions.
    juce::Rectangle<int> getModuleBoundsInRack (int index) const;
    juce::Point<float> getPinCentreInRack (te::EditItemID pluginID, int pinIndex, bool isInput) const;
    bool verifyNonOverlappingLayoutForSmokeTest() const;

private:
    struct RackNode
    {
        te::Plugin* plugin = nullptr;
        juce::Rectangle<float> rect;   // canvas-local (nodesContainer) space
        juce::String name;
        bool isCvSource = false;
    };

    class NodeView : public juce::Component
    {
    public:
        NodeView (te::Plugin& pluginToView, const juce::String& displayName, bool cvSource);
        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        juce::Rectangle<float> getPinRect (int pinIndex) const;
        te::Plugin& plugin;

        std::function<void (NodeView&, const juce::MouseEvent&)> onClick;

    private:
        juce::String name;
        bool isCv;
    };

    te::Engine& engine;
    te::Edit* edit = nullptr;
    te::RackType::Ptr selectedRack;

    juce::ComboBox rackSelector;
    juce::TextButton addLfoButton      { "+ CV LFO" };
    juce::TextButton addWobbleButton   { "+ Wobble" };
    juce::TextButton connectModeButton { "Connect: Off" };
    juce::TextButton clearRackButton   { "Clear Rack" };
    juce::Label rackInfoLabel          { {}, "No Rack" };

    juce::Viewport nodesViewport;
    juce::Component nodesContainer;

    juce::Array<RackNode> nodes;
    juce::Array<std::tuple<te::EditItemID, int, te::EditItemID, int>> connections;

    bool connectMode = false;
    bool awaitingSecondNode = false;
    int firstNodeIndex = -1;
    int firstPinIndex = -1;

    void addCvPluginToRack (const char* xmlTypeName);
    void rebuildRackSelector();
    void rebuildFromRack();
    void addConnection (te::EditItemID source, int sourcePin, te::EditItemID dest, int destPin);
    void removeConnection (te::EditItemID source, int sourcePin, te::EditItemID dest, int destPin);
    void handleNodeClick (int nodeIndex, NodeView& view, const juce::MouseEvent& e);

    juce::Rectangle<int> getCanvasArea() const;
    juce::Rectangle<int> computeGridSlot (int pluginIndex,
                                          juce::Rectangle<int> canvasArea) const;
    juce::Rectangle<int> computeModuleSlotBounds (int pluginIndex, int totalPlugins,
                                                  juce::Point<float> storedPos,
                                                  juce::Rectangle<int> canvasArea) const;
    int findNodeIndex (te::EditItemID pluginID) const;
    juce::Rectangle<float> getPinRectInCanvas (int nodeIndex, int pinIndex) const;
    juce::Rectangle<float> getPinRectInRack (int nodeIndex, int pinIndex) const;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override;
    void valueTreeRedirected (juce::ValueTree&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModularRackComponent)
};
