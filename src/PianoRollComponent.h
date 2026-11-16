#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <functional>

namespace te = tracktion::engine;

class PianoRollComponent : public juce::Component
{
public:
    PianoRollComponent();

    void setTarget (te::Edit* targetEdit, tracktion::EditItemID clipID);
    void setSnapDivision (double beatsPerStep); // 0.25 == 1/16th note

    std::function<void()> onSequenceChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    enum class DragMode
    {
        none,
        moveNote,
        resizeNote
    };

    te::MidiClip* resolveClip() const;
    double snapBeat (double rawBeat) const;
    int getPitchAtY (int y) const;
    double getBeatAtX (int x) const;
    juce::Rectangle<int> getNoteRect (const te::MidiNote& note) const;
    static bool isBlackKey (int midiPitch);
    static juce::String getNoteLabel (int midiPitch);

    static constexpr int topBarHeight   = 30;
    static constexpr int pianoKeyWidth  = 58;
    static constexpr int lowestPitch    = 36;  // C2
    static constexpr int highestPitch   = 84;  // C6
    static constexpr float pixelsPerBeat = 76.0f;

    te::Edit* edit = nullptr;
    tracktion::EditItemID targetClipID;

    double snapBeats = 0.25;          // 1/16th default
    double lastNoteLengthBeats = 0.5; // FL-style remembered note length

    DragMode dragMode = DragMode::none;
    int activeNoteIndex = -1;
    double dragStartBeatOffset = 0.0;
    double initialNoteStartBeat = 0.0;
    double initialNoteLengthBeats = 0.5;

    juce::ComboBox snapSelector;
    juce::Label headerLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};
