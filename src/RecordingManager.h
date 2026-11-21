#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <map>
#include <vector>

namespace te = tracktion::engine;

class RecordingManager
{
public:
    explicit RecordingManager (te::Engine& targetEngine);
    ~RecordingManager();

    void setEdit (te::Edit* newEdit);

    // Input device enumeration
    juce::StringArray getAvailableAudioInputNames() const;
    juce::StringArray getAvailableMidiInputNames() const;
    juce::StringArray getAllInputOptions() const; // "None", "Virtual MIDI", Wave ins, MIDI ins

    // Track arming, monitoring & input routing (persisted on track->state)
    static bool isTrackArmed (const te::AudioTrack& track);
    static void setTrackArmed (te::Edit& edit, te::AudioTrack& track, bool armed, bool startTransaction = true);

    static bool isInputMonitorEnabled (const te::AudioTrack& track);
    static void setInputMonitorEnabled (te::Edit& edit, te::AudioTrack& track, bool enabled, bool startTransaction = true);

    static juce::String getTrackInputName (const te::AudioTrack& track);
    void setTrackInputName (te::Edit& edit, te::AudioTrack& track, const juce::String& inputName, bool startTransaction = true);
    void syncHardwareInputAssignments (te::Edit& edit);

    // Count-in (0 or 1 bar)
    static int getCountInBars (const te::Edit& edit);
    static void setCountInBars (te::Edit& edit, int bars);

    // Transport recording lifecycle
    bool startRecording (bool headlessVirtualOnly = false);
    int stopRecording (bool notifyListeners = true);
    bool isRecording() const noexcept { return recordingActive; }

    // Live MIDI capture for Virtual MIDI / MidiKeyboardDock during recording
    void captureLiveMidiMessage (const juce::MidiMessage& msg, double beatOverride = -1.0);

    // Non-destructive audio take writer & WaveAudioClip committer
    static bool commitRecordedAudioTake (te::Edit& edit,
                                         te::AudioTrack& track,
                                         const juce::AudioBuffer<float>& recordedBuffer,
                                         double sampleRate,
                                         double startBeat,
                                         const juce::File& takesDirectory,
                                         juce::File* outTakeFile = nullptr);

    std::function<void (const juce::String& /*status*/)> onRecordingStateChanged;

private:
    struct PendingMidiNote
    {
        int pitch = 60;
        int velocity = 100;
        double startBeat = 0.0;
        double endBeat = 0.5;
    };

    static inline const juce::Identifier armedProp        { "tcsRecordArmed" };
    static inline const juce::Identifier inputMonitorProp { "tcsInputMonitor" };
    static inline const juce::Identifier inputDeviceProp  { "tcsInputDevice" };
    static inline const juce::Identifier countInProp      { "tcsCountInBars" };

    te::Engine& engine;
    te::Edit* edit = nullptr;
    bool recordingActive = false;
    double recordStartBeat = 0.0;

    std::map<int, PendingMidiNote> activeNotesByPitch;
    std::vector<PendingMidiNote> completedNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordingManager)
};
