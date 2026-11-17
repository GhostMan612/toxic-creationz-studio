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
    void syncFromEdit();
    void selectRack (int rackIndex);

    juce::String getSelectedRackName() const;
    int getNumPluginsInSelectedRack() const;
    int getNumConnectionsInSelectedRack() const;

    std::function<void()> onMixerChanged;

private:
    struct RackNode
    {
        te::Plugin* plugin = nullptr;
        juce::Rectangle<float> rect;
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
    int refreshDepth = 0;

    void addCvPluginToRack (const char* xmlTypeName);
    void rebuildRackSelector();
    void rebuildFromRack();
    void addConnection (te::EditItemID source, int sourcePin, te::EditItemID dest, int destPin);
    void removeConnection (te::EditItemID source, int sourcePin, te::EditItemID dest, int destPin);
    void handleNodeClick (int nodeIndex, NodeView& view, const juce::MouseEvent& e);
    juce::Rectangle<float> getPinRect (int nodeIndex, int pinIndex) const;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override;
    void valueTreeRedirected (juce::ValueTree&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModularRackComponent)
};
