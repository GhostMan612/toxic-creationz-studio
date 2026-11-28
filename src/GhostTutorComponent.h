#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "GhostBrain.h"
#include "GhostWalkthroughs.h"

#include <functional>

namespace te = tracktion::engine;

/** Desktop-only Ghost Tutor chat dock. The LLM brain is local-only; all
 *  mutating actions require an explicit chip tap through onAction.
 */
class GhostTutorComponent : public juce::Component
{
public:
    GhostTutorComponent();
    ~GhostTutorComponent() override;

    void setEdit (te::Edit* newEdit);
    void paint (juce::Graphics& g) override;
    void resized() override;

    void ask (const juce::String& question, bool useLlm);
    void setProactive (bool enabled);
    void persistState();

    std::function<void (const juce::String&, const juce::String&)> onAction;

private:
    void appendMessage (const juce::String& speaker, const juce::String& text);
    void showActionChip (const juce::String& label, const juce::String& method,
                         const juce::String& paramsJson);
    void startWalkthrough (int index);
    void advanceWalkthrough();
    juce::String sessionSnapshot() const;

    te::Edit* edit = nullptr;
    GhostBrain brain;
    juce::ValueTree state;
    juce::TextEditor transcript;
    juce::TextEditor input;
    juce::TextButton sendButton { "Send" };
    juce::TextButton actionChip;
    juce::TextButton walkthroughButton { "Guided Walkthrough" };
    juce::TextButton forgetButton { "Forget what you've learned" };
    juce::ToggleButton proactiveToggle { "Proactive (off by default)" };
    juce::TextButton copilotChips[3] {
        juce::TextButton ("Identify this clip"),
        juce::TextButton ("Clone this voice"),
        juce::TextButton ("Generate like this")
    };
    juce::String pendingMethod;
    juce::String pendingParams;
    int walkthroughIndex = -1;
    int walkthroughStep = 0;
    bool proactive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GhostTutorComponent)
};
