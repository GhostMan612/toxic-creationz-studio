#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "RecordingManager.h"
#include "AutomationManager.h"

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

    void setPixelsPerBeat (float newPixelsPerBeat);
    float getPixelsPerBeat() const noexcept { return pixelsPerBeat; }
    void zoomIn();
    void zoomOut();

    void setAutomationOverlayEnabled (bool enabled);
    bool isAutomationOverlayEnabled() const noexcept { return automationOverlayEnabled; }
    void setAutomationParamName (const juce::String& paramName);
    juce::String getAutomationParamName() const { return automationParamName; }

    te::MidiClip* resolveSelectedClip() const;

    std::function<void (tracktion::EditItemID)> onClipSelected;
    std::function<void()> onLoopChanged;
    std::function<void()> onTrackArmChanged;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    static constexpr int headerWidth    = 190;
    static constexpr int rulerHeight    = 28;
    static constexpr int trackHeight    = 56;
    static constexpr int totalBars      = 16;

    int getTrackIndexAtY (int y) const;
    double getBeatAtX (int x) const;
    int getXForBeat (double beat) const;
    int getYForAutomationValue (float domainValue, int trackIdx) const;
    float getAutomationValueAtY (int y, int trackIdx) const;

    te::Edit* edit = nullptr;
    RecordingManager* recordingManager = nullptr;
    tracktion::EditItemID selectedClipID;

    float pixelsPerBeat = 34.0f;
    double horizontalScrollBeats = 0.0;
    bool automationOverlayEnabled = false;
    juce::String automationParamName { "volume" };

    int draggingAutoTrackIdx = -1;
    int draggingAutoPointIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangementView)
};
