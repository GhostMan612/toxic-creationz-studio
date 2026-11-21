#include "RecordingManager.h"

#include <algorithm>
#include <cmath>

RecordingManager::RecordingManager (te::Engine& targetEngine)
    : engine (targetEngine)
{
}

RecordingManager::~RecordingManager()
{
    onRecordingStateChanged = nullptr;
    setEdit (nullptr);
}

void RecordingManager::setEdit (te::Edit* newEdit)
{
    if (recordingActive && edit != nullptr)
        stopRecording (false);

    edit = newEdit;
    recordingActive = false;
    activeNotesByPitch.clear();
    completedNotes.clear();
}

juce::StringArray RecordingManager::getAvailableAudioInputNames() const
{
    juce::StringArray names;
    auto& dm = engine.getDeviceManager();

    for (auto* dev : dm.getWaveInputDevices())
        if (dev != nullptr && dev->getName().isNotEmpty())
            names.addIfNotAlreadyThere (dev->getName());

    return names;
}

juce::StringArray RecordingManager::getAvailableMidiInputNames() const
{
    juce::StringArray names;
    auto& dm = engine.getDeviceManager();

    for (const auto& dev : dm.getMidiInDevices())
        if (dev != nullptr && dev->getName().isNotEmpty())
            names.addIfNotAlreadyThere (dev->getName());

    return names;
}

juce::StringArray RecordingManager::getAllInputOptions() const
{
    juce::StringArray options;
    options.add ("None");
    options.add ("Virtual MIDI");

    for (const auto& name : getAvailableAudioInputNames())
        options.add ("Audio: " + name);

    for (const auto& name : getAvailableMidiInputNames())
        options.add ("MIDI: " + name);

    return options;
}

bool RecordingManager::isTrackArmed (const te::AudioTrack& track)
{
    return static_cast<bool> (track.state.getProperty (armedProp, false));
}

void RecordingManager::setTrackArmed (te::Edit& targetEdit,
                                      te::AudioTrack& track,
                                      bool armed,
                                      bool startTransaction)
{
    auto& um = targetEdit.getUndoManager();

    if (startTransaction)
        um.beginNewTransaction (armed ? "Arm Track" : "Disarm Track");

    track.state.setProperty (armedProp, armed, &um);

    // Default newly armed tracks with no input to Virtual MIDI so QWERTY/mouse keys work immediately.
    if (armed && getTrackInputName (track) == "None")
        track.state.setProperty (inputDeviceProp, "Virtual MIDI", &um);

    if (armed && ! track.state.hasProperty (inputMonitorProp))
        track.state.setProperty (inputMonitorProp, true, &um);

    if (auto* ctx = targetEdit.getCurrentPlaybackContext())
    {
        for (auto* inst : ctx->getAllInputs())
        {
            if (inst != nullptr && inst->getTargets().contains (track.itemID))
                inst->setRecordingEnabled (track.itemID, armed);
        }
    }
}

bool RecordingManager::isInputMonitorEnabled (const te::AudioTrack& track)
{
    return static_cast<bool> (track.state.getProperty (inputMonitorProp, true));
}

void RecordingManager::setInputMonitorEnabled (te::Edit& targetEdit,
                                               te::AudioTrack& track,
                                               bool enabled,
                                               bool startTransaction)
{
    auto& um = targetEdit.getUndoManager();

    if (startTransaction)
        um.beginNewTransaction ("Toggle Input Monitoring");

    track.state.setProperty (inputMonitorProp, enabled, &um);

    if (auto* ctx = targetEdit.getCurrentPlaybackContext())
    {
        for (auto* inst : ctx->getAllInputs())
        {
            if (inst != nullptr && inst->getTargets().contains (track.itemID))
            {
                inst->getInputDevice().setMonitorMode (enabled
                                                           ? te::InputDevice::MonitorMode::automatic
                                                           : te::InputDevice::MonitorMode::off);
            }
        }
    }
}

juce::String RecordingManager::getTrackInputName (const te::AudioTrack& track)
{
    const auto val = track.state.getProperty (inputDeviceProp, "None").toString();
    return val.isEmpty() ? "None" : val;
}

void RecordingManager::setTrackInputName (te::Edit& targetEdit,
                                          te::AudioTrack& track,
                                          const juce::String& inputName,
                                          bool startTransaction)
{
    const juce::String cleaned = inputName.trim().isEmpty() ? "None" : inputName.trim();
    auto& um = targetEdit.getUndoManager();

    if (startTransaction)
        um.beginNewTransaction ("Set Track Input");

    track.state.setProperty (inputDeviceProp, cleaned, &um);
    syncHardwareInputAssignments (targetEdit);
}

void RecordingManager::syncHardwareInputAssignments (te::Edit& targetEdit)
{
    auto* ctx = targetEdit.getCurrentPlaybackContext();

    if (ctx == nullptr)
        return;

    auto tracks = te::getAudioTracks (targetEdit);

    for (auto* track : tracks)
    {
        const juce::String wanted = getTrackInputName (*track);
        const bool armed          = isTrackArmed (*track);
        const bool monitor        = isInputMonitorEnabled (*track);

        for (auto* inst : ctx->getAllInputs())
        {
            if (inst == nullptr)
                continue;

            auto& dev = inst->getInputDevice();
            const juce::String audioLabel = "Audio: " + dev.getName();
            const juce::String midiLabel  = "MIDI: " + dev.getName();
            const bool matches = (wanted == dev.getName() || wanted == audioLabel || wanted == midiLabel);

            if (matches)
            {
                const auto setRes = inst->setTarget (track->itemID, true, &targetEdit.getUndoManager(), 0);
                juce::ignoreUnused (setRes);
                inst->setRecordingEnabled (track->itemID, armed);
                dev.setMonitorMode (monitor ? te::InputDevice::MonitorMode::automatic
                                            : te::InputDevice::MonitorMode::off);
            }
            else if (inst->getTargets().contains (track->itemID))
            {
                inst->setRecordingEnabled (track->itemID, false);
                const auto remRes = inst->removeTarget (track->itemID, &targetEdit.getUndoManager());
                juce::ignoreUnused (remRes);
            }
        }
    }
}

int RecordingManager::getCountInBars (const te::Edit& targetEdit)
{
    const int stored = static_cast<int> (targetEdit.state.getProperty (countInProp, -1));

    if (stored >= 0)
        return stored > 0 ? 1 : 0;

    return targetEdit.getCountInMode() == te::Edit::CountIn::none ? 0 : 1;
}

void RecordingManager::setCountInBars (te::Edit& targetEdit, int bars)
{
    const int clamped = (bars > 0) ? 1 : 0;
    auto& um = targetEdit.getUndoManager();
    um.beginNewTransaction ("Set Count-In");
    targetEdit.state.setProperty (countInProp, clamped, &um);
    targetEdit.setCountInMode (clamped > 0 ? te::Edit::CountIn::oneBar : te::Edit::CountIn::none);
}

bool RecordingManager::startRecording (bool headlessVirtualOnly)
{
    if (edit == nullptr)
        return false;

    auto tracks = te::getAudioTracks (*edit);
    bool anyArmed = false;

    for (auto* t : tracks)
        if (isTrackArmed (*t))
            anyArmed = true;

    // Auto-arm Track 0 if user triggers Rec with no track explicitly armed.
    if (! anyArmed && ! tracks.isEmpty())
        setTrackArmed (*edit, *tracks[0], true);

    activeNotesByPitch.clear();
    completedNotes.clear();
    recordStartBeat = std::max (0.0, edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats());
    recordingActive = true;

    if (! headlessVirtualOnly)
    {
        edit->getTransport().ensureContextAllocated();
        syncHardwareInputAssignments (*edit);

        bool hasHardwareInput = false;

        if (auto* ctx = edit->getCurrentPlaybackContext())
            for (auto* inst : ctx->getAllInputs())
                if (inst != nullptr && inst->isRecordingActive())
                    hasHardwareInput = true;

        if (hasHardwareInput)
            edit->getTransport().record (false);
        else
            edit->getTransport().play (false);
    }

    if (onRecordingStateChanged)
        onRecordingStateChanged ("Recording...");

    return true;
}

void RecordingManager::captureLiveMidiMessage (const juce::MidiMessage& msg, double beatOverride)
{
    if (! recordingActive || edit == nullptr)
        return;

    const double nowBeat = (beatOverride >= 0.0)
                               ? beatOverride
                               : std::max (recordStartBeat,
                                           edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats());

    if (msg.isNoteOn())
    {
        PendingMidiNote note;
        note.pitch     = juce::jlimit (0, 127, msg.getNoteNumber());
        note.velocity  = juce::jlimit (1, 127, static_cast<int> (msg.getVelocity()));
        note.startBeat = nowBeat;
        note.endBeat   = nowBeat + 0.5;
        activeNotesByPitch[note.pitch] = note;
    }
    else if (msg.isNoteOff())
    {
        const int pitch = msg.getNoteNumber();
        auto it = activeNotesByPitch.find (pitch);

        if (it != activeNotesByPitch.end())
        {
            it->second.endBeat = std::max (it->second.startBeat + 0.125, nowBeat);
            completedNotes.push_back (it->second);
            activeNotesByPitch.erase (it);
        }
    }
}

int RecordingManager::stopRecording (bool notifyListeners)
{
    if (edit == nullptr)
    {
        recordingActive = false;
        return 0;
    }

    const double stopBeat = std::max (recordStartBeat + 0.5,
                                      edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats());

    if (edit->getTransport().isRecording() || edit->getTransport().isPlaying())
        edit->getTransport().stop (false, false);

    // Flush any held notes at Stop time.
    for (auto& [pitch, note] : activeNotesByPitch)
    {
        note.endBeat = std::max (note.startBeat + 0.125, stopBeat);
        completedNotes.push_back (note);
    }

    activeNotesByPitch.clear();

    int committedNotes = 0;

    if (! completedNotes.empty())
    {
        double minBeat = completedNotes[0].startBeat;
        double maxBeat = completedNotes[0].endBeat;

        for (const auto& n : completedNotes)
        {
            minBeat = std::min (minBeat, n.startBeat);
            maxBeat = std::max (maxBeat, n.endBeat);
        }

        const double clipStartBeat = std::floor (minBeat / 4.0) * 4.0;
        const double clipEndBeat   = std::max (clipStartBeat + 4.0, std::ceil (maxBeat / 4.0) * 4.0);
        const tracktion::TimeRange clipRange {
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (clipStartBeat)),
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (clipEndBeat))
        };

        auto& um = edit->getUndoManager();
        um.beginNewTransaction ("Record Live MIDI");

        for (auto* track : te::getAudioTracks (*edit))
        {
            if (! isTrackArmed (*track))
                continue;

            if (getTrackInputName (*track).startsWith ("Audio:"))
                continue;

            if (auto* clip = dynamic_cast<te::MidiClip*> (
                    track->insertNewClip (te::TrackItem::Type::midi, "Live Rec", clipRange, nullptr)))
            {
                auto& seq = clip->getSequence();

                for (const auto& n : completedNotes)
                {
                    seq.addNote (n.pitch,
                                 tracktion::BeatPosition::fromBeats (std::max (0.0, n.startBeat - clipStartBeat)),
                                 tracktion::BeatDuration::fromBeats (std::max (0.125, n.endBeat - n.startBeat)),
                                 n.velocity,
                                 0,
                                 &um);
                    ++committedNotes;
                }
            }
        }
    }

    completedNotes.clear();
    recordingActive = false;

    if (notifyListeners && onRecordingStateChanged)
    {
        onRecordingStateChanged (committedNotes > 0
                                     ? ("Recorded " + juce::String (committedNotes) + " MIDI notes")
                                     : "Recording stopped");
    }

    return committedNotes;
}

bool RecordingManager::commitRecordedAudioTake (te::Edit& targetEdit,
                                                te::AudioTrack& track,
                                                const juce::AudioBuffer<float>& recordedBuffer,
                                                double sampleRate,
                                                double startBeat,
                                                const juce::File& takesDirectory,
                                                juce::File* outTakeFile)
{
    if (recordedBuffer.getNumChannels() <= 0 || recordedBuffer.getNumSamples() <= 0 || sampleRate <= 0.0)
        return false;

    takesDirectory.createDirectory();
    const juce::File takeFile = takesDirectory.getNonexistentChildFile ("RecTake", ".wav", false);

    {
        auto fs = std::make_unique<juce::FileOutputStream> (takeFile);

        if (! fs->openedOk())
            return false;

        fs->setPosition (0);
        fs->truncate();

        std::unique_ptr<juce::OutputStream> os (fs.release());
        juce::WavAudioFormat wavFormat;
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate (sampleRate)
                                 .withNumChannels (recordedBuffer.getNumChannels())
                                 .withBitsPerSample (16);

        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (os, options));

        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (recordedBuffer, 0, recordedBuffer.getNumSamples()))
            return false;
    }

    const double durationSec = static_cast<double> (recordedBuffer.getNumSamples()) / sampleRate;
    const auto startTime = targetEdit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (std::max (0.0, startBeat)));
    const auto endTime   = tracktion::TimePosition::fromSeconds (startTime.inSeconds() + durationSec);

    const tracktion::TimeRange range { startTime, endTime };
    const te::ClipPosition clipPos { range, tracktion::TimeDuration::fromSeconds (0.0) };

    auto& um = targetEdit.getUndoManager();
    um.beginNewTransaction ("Commit Recorded Audio Take");

    bool inserted = false;

    if (auto waveClip = track.insertWaveClip (takeFile.getFileNameWithoutExtension(), takeFile, clipPos, false))
    {
        waveClip->getSourceFileReference().setToFile (
            takeFile, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
        inserted = true;
    }

    if (inserted && outTakeFile != nullptr)
        *outTakeFile = takeFile;

    return inserted;
}
