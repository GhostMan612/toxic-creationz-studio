#include "GhostTutorComponent.h"
#include "GhostKnowledge.h"

GhostTutorComponent::GhostTutorComponent()
{
    setOpaque (true);
    transcript.setReadOnly (true);
    transcript.setMultiLine (true);
    transcript.setScrollbarsShown (true);
    input.setMultiLine (false);
    input.setReturnKeyStartsNewLine (false);
    proactiveToggle.setToggleState (false, juce::dontSendNotification);

    appendMessage ("GHOST", "Boo. Ask me how to do something, or take a guided walkthrough.");

    sendButton.onClick = [this] { ask (input.getText().trim(), true); };
    input.onTextChange = [this] { /* input is intentionally stateless */ };

    walkthroughButton.onClick = [this]
    {
        walkthroughIndex = (walkthroughIndex + 1) % 4;
        walkthroughStep = 0;
        appendMessage ("GHOST", juce::String ("Walkthrough: ")
            + ghost::walkthroughFor ("x").title + " — step 1 is proposed below.");
        startWalkthrough (walkthroughIndex);
    };

    forgetButton.onClick = [this]
    {
        transcript.setText ("");
        state.removeProperty ("memory", nullptr);
        appendMessage ("GHOST", "Memory cleared. Boo.");
    };

    proactiveToggle.onClick = [this]
    {
        proactive = proactiveToggle.getToggleState();
        persistState();
    };

    actionChip.onClick = [this]
    {
        if (pendingMethod.isNotEmpty() && onAction != nullptr)
            onAction (pendingMethod, pendingParams);
        pendingMethod = "";
        actionChip.setVisible (false);
    };

    const char* copilotQuestions[3] = {
        "Identify the selected clip", "Clone this voice locally", "Generate like this clip"
    };
    for (int i = 0; i < 3; ++i)
    {
        copilotChips[i].onClick = [this, copilotQuestions, i] { ask (copilotQuestions[i], false); };
        addAndMakeVisible (copilotChips[i]);
    }

    addAndMakeVisible (transcript);
    addAndMakeVisible (input);
    addAndMakeVisible (sendButton);
    addAndMakeVisible (actionChip);
    addAndMakeVisible (walkthroughButton);
    addAndMakeVisible (forgetButton);
    addAndMakeVisible (proactiveToggle);
    actionChip.setVisible (false);
}

GhostTutorComponent::~GhostTutorComponent()
{
    brain.cancel();
    sendButton.onClick = nullptr;
    input.onTextChange = nullptr;
    walkthroughButton.onClick = nullptr;
    forgetButton.onClick = nullptr;
    proactiveToggle.onClick = nullptr;
    actionChip.onClick = nullptr;
    for (auto& chip : copilotChips) chip.onClick = nullptr;
    onAction = nullptr;
}

void GhostTutorComponent::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    if (edit == nullptr)
        return;
    state = edit->state.getChildWithName ("GHOST_TUTOR");
    proactive = static_cast<bool> (state.getProperty ("proactive", false));
    proactiveToggle.setToggleState (proactive, juce::dontSendNotification);
    walkthroughIndex = static_cast<int> (state.getProperty ("walkthrough_id", -1));
    walkthroughStep = static_cast<int> (state.getProperty ("walkthrough_step", 0));
    persistState();
}

void GhostTutorComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff161922));

    // Tiny vector-drawn monochrome ghost glyph — never an emoji codepoint.
    juce::Path ghost;
    ghost.addEllipse (12.0f, 8.0f, 16.0f, 18.0f);
    ghost.lineTo (28.0f, 24.0f);
    ghost.lineTo (24.0f, 21.0f);
    ghost.lineTo (20.0f, 24.0f);
    ghost.lineTo (16.0f, 21.0f);
    ghost.lineTo (12.0f, 24.0f);
    ghost.closeSubPath();
    g.setColour (juce::Colour (0xff94a3b8));
    g.fillPath (ghost);
}

void GhostTutorComponent::resized()
{
    auto area = getLocalBounds().reduced (8);
    area.removeFromLeft (34);
    transcript.setBounds (area.removeFromTop (area.getHeight() - 116));
    area.removeFromTop (4);
    actionChip.setBounds (area.removeFromTop (24));
    auto row = area.removeFromTop (28);
    input.setBounds (row.removeFromLeft (row.getWidth() - 76));
    sendButton.setBounds (row.removeFromLeft (72));
    auto bottom = area.removeFromTop (24);
    walkthroughButton.setBounds (bottom.removeFromLeft (150));
    forgetButton.setBounds (bottom.removeFromLeft (180));
    proactiveToggle.setBounds (bottom.removeFromLeft (170));
    auto chips = area.removeFromTop (22);
    for (auto& chip : copilotChips)
        chip.setBounds (chips.removeFromLeft (140).reduced (2));
}

void GhostTutorComponent::appendMessage (const juce::String& speaker, const juce::String& text)
{
    transcript.insertTextAtCaret (speaker + ": " + text + "\n");
    transcript.setCaretPosition (transcript.getText().length());
}

juce::String GhostTutorComponent::sessionSnapshot() const
{
    if (edit == nullptr)
        return "No session loaded.";
    juce::String names;
    for (auto* track : te::getAudioTracks (*edit))
    {
        if (names.isNotEmpty())
            names << ", ";
        names << track->getName();
    }
    const double bpm = edit->tempoSequence.getTempo (0)->getBpm();
    return "tracks=" + juce::String (te::getAudioTracks (*edit).size()) + " [" + names
         + "] bpm=" + juce::String (bpm, 1);
}

void GhostTutorComponent::ask (const juce::String& question, bool useLlm)
{
    if (question.isEmpty())
        return;
    input.setText ("");
    appendMessage ("YOU", question);

    const auto top = GhostKnowledge::topSectionIds (question, 2);
    const auto prompt = "You are Ghost, a dry but helpful tutor inside Toxic Creationz Studio. "
                        "Answer only from the built-in guide and the live session snapshot. "
                        "Never invent a button, menu or shortcut. Say 'Boo.' sparingly. "
                        "Relevant sections: " + top.joinIntoString (", ")
                        + ". Live session: " + sessionSnapshot();
    brain.requestAsync (question, prompt, useLlm,
        [safePtr = juce::Component::SafePointer<GhostTutorComponent> (this)] (const GhostBrain::Reply& reply)
        {
            if (auto* panel = safePtr.getComponent())
            {
                panel->appendMessage ("GHOST", reply.text);
                const auto knowledge = GhostKnowledge::answer (reply.fromOllama ? panel->input.getText() : reply.detail);
                if (knowledge.actionMethod.isNotEmpty())
                    panel->showActionChip ("Want me to do it for you? " + knowledge.actionLabel,
                                           knowledge.actionMethod, knowledge.actionParamsJson);
            }
        });
}

void GhostTutorComponent::showActionChip (const juce::String& label, const juce::String& method,
                                          const juce::String& paramsJson)
{
    pendingMethod = method;
    pendingParams = paramsJson;
    actionChip.setButtonText (label);
    actionChip.setVisible (true);
    resized();
}

void GhostTutorComponent::startWalkthrough (int index)
{
    const auto& item = [index] () -> const ghost::Walkthrough&
    {
        static const ghost::Walkthrough fallback { "record-first-take", "", nullptr, 0 };
        switch (index)
        {
            case 1: {
                static const ghost::Walkthrough w { "make-808-beat", "", ghost::beatSteps, 3 };
                return w;
            }
            case 2: {
                static const ghost::Walkthrough w { "mix-vocal-booth", "", ghost::vocalSteps, 3 };
                return w;
            }
            case 3: {
                static const ghost::Walkthrough w { "export-first-mixdown", "", ghost::exportSteps, 2 };
                return w;
            }
            default: return fallback;
        }
    }();
    walkthroughStep = 0;
    if (item.steps != nullptr && item.stepCount > 0)
        showActionChip (item.steps[0].label, item.steps[0].method, item.steps[0].paramsJson);
    persistState();
}

void GhostTutorComponent::advanceWalkthrough()
{
    ++walkthroughStep;
    persistState();
}

void GhostTutorComponent::setProactive (bool enabled)
{
    proactive = enabled;
    proactiveToggle.setToggleState (enabled, juce::dontSendNotification);
    persistState();
}

void GhostTutorComponent::persistState()
{
    if (! state.isValid())
        return;
    state.setProperty ("proactive", proactive, nullptr);
    state.setProperty ("walkthrough_id", walkthroughIndex, nullptr);
    state.setProperty ("walkthrough_step", walkthroughStep, nullptr);
}
