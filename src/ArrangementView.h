#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "RecordingManager.h"

#include <functional>

namespace te = tracktion::engine;

class ArrangementView : public juce::Component
{
public:
    ArrangementView();

    void setEdit (te::Edit* newEdit);
    void setRecordingManager (RecordingManager* manager);
    void setSelectedClipID (tracktion::EditItemID clipID);
    tracktion::EditItemID getSelectedClipID() const noexcept { return selectedClipID; }

    te::MidiClip* resolveSelectedClip() const;

    std::function<void (tracktion::EditItemID)> onClipSelected;
    std::function<void()> onLoopChanged;
    std::function<void()> onTrackArmChanged;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    static constexpr int headerWidth    = 190;
    static constexpr int rulerHeight    = 28;
    static constexpr int trackHeight    = 56;
    static constexpr int totalBars      = 16;
    static constexpr float pixelsPerBeat = 34.0f;

    int getTrackIndexAtY (int y) const;
    double getBeatAtX (int x) const;

    te::Edit* edit = nullptr;
    RecordingManager* recordingManager = nullptr;
    tracktion::EditItemID selectedClipID;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangementView)
};
