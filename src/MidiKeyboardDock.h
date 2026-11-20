#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <functional>

namespace te = tracktion::engine;

class MidiKeyboardDock : public juce::Component,
                         private juce::MidiKeyboardStateListener
{
public:
    MidiKeyboardDock();
    ~MidiKeyboardDock() override;

    void setEdit (te::Edit* newEdit);
    void syncFromEdit();

    // Deterministic helper used by Step Rec and --smoke-test.
    bool recordStepNote (int midiNoteNumber, int velocity = 105, double stepBeats = 0.5);

    std::function<void()> onNotesRecorded;
    std::function<double()> getStepBeatsCallback;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void handleNoteOn (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;
    void handleNoteOff (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;

    te::AudioTrack* resolveTargetTrack() const;

    static constexpr int controlStripWidth = 178;

    te::Edit* edit = nullptr;
    int selectedTrackIndex = 0;

    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboardComponent {
        keyboardState,
        juce::MidiKeyboardComponent::horizontalKeyboard
    };

    juce::Label titleLabel;
    juce::ComboBox trackSelector;
    juce::ToggleButton stepRecToggle { "Step Rec" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiKeyboardDock)
};
