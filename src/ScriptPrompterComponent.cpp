#include "ScriptPrompterComponent.h"

namespace
{
const juce::Identifier voScriptTree { "VOSCRIPT" };
const juce::Identifier propTitle { "title" };
const juce::Identifier propText { "text" };
const juce::Identifier propWpm { "wpm" };
const juce::Identifier propFontSize { "fontSize" };
const juce::Identifier propAdrEnabled { "adrEnabled" };
const juce::Identifier propAdrBeep { "adrBeepIntervalSec" };
const juce::Identifier propAdrLine { "adrLineSec" };
}

ScriptPrompterComponent::ScriptPrompterComponent()
{
    scriptEditor.setMultiLine (true);
    scriptEditor.setReturnKeyStartsNewLine (true);
    scriptEditor.setScrollbarsShown (true);
    scriptEditor.setFont (juce::Font (juce::FontOptions (state.fontSize)));
    scriptEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff12151c));
    scriptEditor.setColour (juce::TextEditor::textColourId, juce::Colour (0xffe2e8f0));
    scriptEditor.setText ("Load a script from the AI Voice Director or drop a .txt / .md file here.", false);
    addAndMakeVisible (scriptEditor);

    fontSizeSlider.setRange (14.0, 36.0, 1.0);
    fontSizeSlider.setValue (static_cast<double> (state.fontSize), juce::dontSendNotification);
    fontSizeSlider.onValueChange = [this]
    {
        state.fontSize = static_cast<float> (fontSizeSlider.getValue());
        scriptEditor.setFont (juce::Font (juce::FontOptions (state.fontSize)));
        persistToEdit();
    };
    addAndMakeVisible (fontSizeSlider);

    wpmSlider.setRange (80.0, 220.0, 1.0);
    wpmSlider.setValue (static_cast<double> (state.wpm), juce::dontSendNotification);
    wpmSlider.onValueChange = [this]
    {
        state.wpm = static_cast<int> (wpmSlider.getValue());
        persistToEdit();
    };
    addAndMakeVisible (wpmSlider);

    adrToggleButton.setClickingTogglesState (true);
    adrToggleButton.setToggleState (state.adrEnabled, juce::dontSendNotification);
    adrToggleButton.onClick = [this]
    {
        state.adrEnabled = adrToggleButton.getToggleState();
        persistToEdit();
        updateStatus();
    };
    addAndMakeVisible (adrToggleButton);

    dropCueButton.onClick = [this] { dropCueMarker(); };
    addAndMakeVisible (dropCueButton);

    warmupButton.setClickingTogglesState (true);
    warmupButton.onClick = [this]
    {
        warmupActive = warmupButton.getToggleState();
        warmupStartedSec = 0.001 * juce::Time::getMillisecondCounterHiRes();
        updateStatus();
    };
    addAndMakeVisible (warmupButton);

    fontSizeLabel.setText ("Font", juce::dontSendNotification);
    fontSizeLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    wpmLabel.setText ("WPM", juce::dontSendNotification);
    wpmLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    addAndMakeVisible (fontSizeLabel);
    addAndMakeVisible (wpmLabel);
    addAndMakeVisible (statusLabel);

    startTimerHz (20);
}

ScriptPrompterComponent::~ScriptPrompterComponent()
{
    stopTimer();
    fontSizeSlider.onValueChange = nullptr;
    wpmSlider.onValueChange = nullptr;
    adrToggleButton.onClick = nullptr;
    dropCueButton.onClick = nullptr;
    warmupButton.onClick = nullptr;
    setEdit (nullptr);
}

void ScriptPrompterComponent::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    if (edit != nullptr)
        syncFromEdit();
}

void ScriptPrompterComponent::setScript (const ScriptState& newState, bool persist)
{
    state = newState;
    state.fontSize = juce::jlimit (14.0f, 36.0f, state.fontSize);
    state.wpm = juce::jlimit (80, 220, state.wpm);
    state.adrLineSec = juce::jlimit (1.0, 15.0, state.adrLineSec);
    state.adrBeepIntervalSec = juce::jlimit (0.5, 1.0, state.adrBeepIntervalSec);

    scriptEditor.setText (state.text, false);
    scriptEditor.setFont (juce::Font (juce::FontOptions (state.fontSize)));
    fontSizeSlider.setValue (static_cast<double> (state.fontSize), juce::dontSendNotification);
    wpmSlider.setValue (static_cast<double> (state.wpm), juce::dontSendNotification);
    adrToggleButton.setToggleState (state.adrEnabled, juce::dontSendNotification);
    scrollLine = 0;

    if (persist)
        persistToEdit();
    updateStatus();
}

ScriptPrompterComponent::ScriptState ScriptPrompterComponent::getScriptState() const
{
    auto out = state;
    out.text = scriptEditor.getText();
    out.fontSize = static_cast<float> (fontSizeSlider.getValue());
    out.wpm = static_cast<int> (wpmSlider.getValue());
    out.adrEnabled = adrToggleButton.getToggleState();
    return out;
}

void ScriptPrompterComponent::persistToEdit()
{
    if (edit == nullptr)
        return;

    auto& um = edit->getUndoManager();
    um.beginNewTransaction ("VO Booth Script Update");
    auto tree = edit->state.getOrCreateChildWithName (voScriptTree, &um);
    tree.setProperty (propTitle, state.title, &um);
    tree.setProperty (propText, getScriptState().text, &um);
    tree.setProperty (propWpm, state.wpm, &um);
    tree.setProperty (propFontSize, state.fontSize, &um);
    tree.setProperty (propAdrEnabled, state.adrEnabled, &um);
    tree.setProperty (propAdrBeep, state.adrBeepIntervalSec, &um);
    tree.setProperty (propAdrLine, state.adrLineSec, &um);
}

void ScriptPrompterComponent::syncFromEdit()
{
    if (edit == nullptr)
        return;

    const auto tree = edit->state.getChildWithName (voScriptTree);
    if (! tree.isValid())
        return;

    ScriptState restored;
    restored.title = tree.getProperty (propTitle, "").toString();
    restored.text = tree.getProperty (propText, "").toString();
    restored.wpm = static_cast<int> (tree.getProperty (propWpm, 140));
    restored.fontSize = static_cast<float> (tree.getProperty (propFontSize, 18.0));
    restored.adrEnabled = static_cast<bool> (tree.getProperty (propAdrEnabled, false));
    restored.adrBeepIntervalSec = static_cast<double> (tree.getProperty (propAdrBeep, 0.65));
    restored.adrLineSec = static_cast<double> (tree.getProperty (propAdrLine, 4.0));
    setScript (restored, false);
}

juce::String ScriptPrompterComponent::currentLine() const
{
    const auto lines = juce::StringArray::fromLines (scriptEditor.getText());
    return juce::isPositiveAndBelow (scrollLine, lines.size()) ? lines[scrollLine] : juce::String();
}

void ScriptPrompterComponent::dropCueMarker()
{
    if (edit == nullptr)
        return;

    auto& mm = edit->getMarkerManager();
    const auto pos = edit->getTransport().getPosition();
    if (auto marker = mm.createMarker (mm.getNextUniqueID(), pos, {}, nullptr))
        marker->setName (currentLine().isEmpty() ? "VO Cue" : currentLine().substring (0, 48));
}

void ScriptPrompterComponent::timerCallback()
{
    if (edit != nullptr && edit->getTransport().isPlaying() && ! scriptEditor.getText().isEmpty())
    {
        const double wordsPerMs = std::max (1, state.wpm) / 60000.0;
        const double elapsed = edit->getTransport().getPosition().inSeconds();
        const int targetLine = juce::jlimit (0,
            std::max (0, juce::StringArray::fromLines (scriptEditor.getText()).size() - 1),
            static_cast<int> (elapsed * wordsPerMs * 10.0));
        if (targetLine != scrollLine)
            scrollLine = targetLine;
    }
    updateStatus();
}

void ScriptPrompterComponent::updateStatus()
{
    if (warmupActive)
    {
        const double elapsed = 0.001 * juce::Time::getMillisecondCounterHiRes() - warmupStartedSec;
        double remaining = 0.0;
        const auto stage = tcs::vo::AdrCueGenerator::getPacerStage (elapsed, remaining);
        statusLabel.setText (juce::String (tcs::vo::AdrCueGenerator::pacerLabel (stage))
                             + "  " + juce::String (remaining, 1) + " s", juce::dontSendNotification);
        return;
    }

    juce::String text = state.title.isEmpty() ? juce::String ("VO Booth") : state.title;
    text += "  |  " + juce::String (state.wpm) + " WPM";
    text += state.adrEnabled ? "  |  ADR ON" : "  |  ADR OFF";
    statusLabel.setText (text, juce::dontSendNotification);
}

void ScriptPrompterComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff12151c));
    g.setColour (juce::Colour (0xff232937));
    g.drawRect (getLocalBounds(), 1);

    auto bar = getLocalBounds().removeFromBottom (26).reduced (6, 6);
    g.setColour (juce::Colour (0xff0b0e13));
    g.fillRect (bar);

    if (state.adrEnabled)
    {
        const int step = bar.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
        {
            const auto slot = bar.removeFromLeft (step);
            g.setColour (i < 3 ? juce::Colour (0xfffbbf24) : juce::Colour (0xff22c55e));
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (i < 3 ? juce::String (i + 1) : "GO!", slot, juce::Justification::centred, false);
        }

        if (edit != nullptr && edit->getTransport().isPlaying())
        {
            const double lineSec = juce::jlimit (1.0, 15.0, state.adrLineSec);
            const double playhead = edit->getTransport().getPosition().inSeconds();
            const double progress = std::fmod (std::max (0.0, playhead), lineSec) / lineSec;
            g.setColour (juce::Colour (0xff0ea5e9));
            g.fillRect (bar.removeFromLeft (static_cast<int> (bar.getWidth() * progress)));
        }
    }
    else
    {
        g.setColour (juce::Colour (0xff64748b));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("ADR streamer idle", bar, juce::Justification::centred, false);
    }
}

void ScriptPrompterComponent::resized()
{
    auto area = getLocalBounds().reduced (6);
    area.removeFromBottom (32);

    auto controls = area.removeFromTop (28);
    fontSizeLabel.setBounds (controls.removeFromLeft (38));
    fontSizeSlider.setBounds (controls.removeFromLeft (110));
    wpmLabel.setBounds (controls.removeFromLeft (38));
    wpmSlider.setBounds (controls.removeFromLeft (110));
    adrToggleButton.setBounds (controls.removeFromLeft (96));
    dropCueButton.setBounds (controls.removeFromLeft (130));
    warmupButton.setBounds (controls.removeFromLeft (110));
    statusLabel.setBounds (controls);

    scriptEditor.setBounds (area);
}
