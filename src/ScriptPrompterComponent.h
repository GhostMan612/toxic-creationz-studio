#pragma once

// Desktop-only VO booth dock: quiet-booth script teleprompter, ADR cue streamer
// and 4-2-15 warm-up pacer. Never include this header from tcs_core.

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "AdrCueGenerator.h"

namespace te = tracktion::engine;

class ScriptPrompterComponent : public juce::Component,
                                private juce::Timer
{
public:
    ScriptPrompterComponent();
    ~ScriptPrompterComponent() override;

    struct ScriptState
    {
        juce::String title;
        juce::String text;
        int wpm = 140;
        float fontSize = 18.0f;
        bool adrEnabled = false;
        double adrBeepIntervalSec = 0.65;
        double adrLineSec = 4.0;
    };

    void setEdit (te::Edit* newEdit);
    void setScript (const ScriptState& state, bool persist = true);
    ScriptState getScriptState() const;
    void syncFromEdit();

    void dropCueMarker();
    void paint (juce::Graphics& g) override;
    void resized() override;

    juce::String getStatusText() const { return statusLabel.getText(); }

private:
    void timerCallback() override;
    void persistToEdit();
    void updateStatus();
    juce::String currentLine() const;

    te::Edit* edit = nullptr;
    ScriptState state;
    bool warmupActive = false;
    double warmupStartedSec = 0.0;
    double lineStartSec = 0.0;
    int scrollLine = 0;

    juce::TextEditor scriptEditor;
    juce::TextButton dropCueButton   { "+ Drop Cue Marker" };
    juce::TextButton warmupButton    { "Warm-Up 4-2-15" };
    juce::TextButton adrToggleButton { "ADR Cue" };
    juce::Slider fontSizeSlider;
    juce::Slider wpmSlider;
    juce::Label statusLabel;
    juce::Label fontSizeLabel;
    juce::Label wpmLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScriptPrompterComponent)
};
