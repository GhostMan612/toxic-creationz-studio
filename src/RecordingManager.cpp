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

namespace
{
    const juce::Identifier kLanesTree { "TCS_TAKE_LANES" };
    const juce::Identifier kLaneTree  { "TAKE_LANE" };
    const juce::Identifier kTakeTree  { "TAKE" };
    const juce::Identifier kFileProp  { "file" };
    const juce::Identifier kNameProp  { "name" };
    const juce::Identifier kStartProp { "startBeat" };
    const juce::Identifier kEndProp   { "endBeat" };
    const juce::Identifier kActiveProp { "activeTake" };
    const juce::Identifier kSlateProp  { "slate" };

    juce::File takeFileDirForLanes (const juce::ValueTree& lanes)
    {
        for (const auto& lane : lanes)
        {
            for (const auto& take : lane)
            {
                const juce::File f (take.getProperty (kFileProp).toString());
                if (f.existsAsFile())
                    return f.getParentDirectory();
            }
        }
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("tcs_take_comps");
    }

    juce::ValueTree findLane (const juce::ValueTree& lanes, double startBeat, double endBeat)
    {
        for (const auto& lane : lanes)
        {
            if (std::abs (static_cast<double> (lane.getProperty (kStartProp, 0.0)) - startBeat) < 0.05
                && std::abs (static_cast<double> (lane.getProperty (kEndProp, 0.0)) - endBeat) < 0.05)
                return lane;
        }
        return {};
    }
}

juce::Array<RecordingManager::TakeInfo> RecordingManager::getTrackTakes (const te::AudioTrack& track)
{
    juce::Array<TakeInfo> result;
    const auto lanes = track.state.getChildWithName (kLanesTree);
    if (! lanes.isValid())
        return result;

    for (const auto& lane : lanes)
    {
        const int active = static_cast<int> (lane.getProperty (kActiveProp, 0));
        int takeIndex = 0;
        for (const auto& take : lane)
        {
            if (take.getType() != kTakeTree)
                continue;

            TakeInfo info;
            info.file = take.getProperty (kFileProp).toString();
            info.name = take.getProperty (kNameProp).toString();
            info.slate = take.getProperty (kSlateProp, "").toString();
            info.startBeat = static_cast<double> (take.getProperty (kStartProp, 0.0));
            info.endBeat = static_cast<double> (take.getProperty (kEndProp, 0.0));
            info.active = (takeIndex == active);
            result.add (info);
            ++takeIndex;
        }
    }
    return result;
}

bool RecordingManager::addRecordedTake (te::Edit& edit, te::AudioTrack& track,
                                        const juce::File& wavFile, double startBeat, double endBeat,
                                        bool startTransaction)
{
    if (! wavFile.existsAsFile() || ! std::isfinite (startBeat) || ! std::isfinite (endBeat)
        || endBeat <= startBeat)
        return false;

    auto& um = edit.getUndoManager();
    if (startTransaction)
        um.beginNewTransaction ("Add Recorded Take");

    auto lanes = track.state.getChildWithName (kLanesTree);
    if (! lanes.isValid())
    {
        lanes = juce::ValueTree (kLanesTree);
        track.state.addChild (lanes, -1, &um);
    }

    auto lane = findLane (lanes, startBeat, endBeat);
    if (! lane.isValid())
    {
        lane = juce::ValueTree (kLaneTree);
        lane.setProperty (kStartProp, startBeat, &um);
        lane.setProperty (kEndProp, endBeat, &um);
        lanes.addChild (lane, -1, &um);
    }

    juce::ValueTree take (kTakeTree);
    take.setProperty (kFileProp, wavFile.getFullPathName(), &um);
    take.setProperty (kNameProp, wavFile.getFileNameWithoutExtension(), &um);
    take.setProperty (kStartProp, startBeat, &um);
    take.setProperty (kEndProp, endBeat, &um);
    lane.addChild (take, -1, &um);
    lane.setProperty (kActiveProp, lane.getNumChildren() - 1, &um);
    return true;
}

bool RecordingManager::selectActiveTake (te::Edit& edit, te::AudioTrack& track,
                                         int laneIndex, int takeIndex)
{
    auto lanes = track.state.getChildWithName (kLanesTree);
    if (! lanes.isValid() || ! juce::isPositiveAndBelow (laneIndex, lanes.getNumChildren()))
        return false;

    auto lane = lanes.getChild (laneIndex);
    if (! juce::isPositiveAndBelow (takeIndex, lane.getNumChildren()))
        return false;

    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("Select Active Take");
    lane.setProperty (kActiveProp, takeIndex, &um);
    return true;
}

juce::File RecordingManager::flattenCompToClip (te::Edit& edit, te::AudioTrack& track,
                                                double startBeat, double endBeat,
                                                double crossfadeMs, int laneIndex)
{
    if (! std::isfinite (startBeat) || ! std::isfinite (endBeat) || endBeat <= startBeat)
        return {};

    const auto lanes = track.state.getChildWithName (kLanesTree);
    if (! lanes.isValid())
        return {};

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    const auto startTime = edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat));
    const auto endTime = edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endBeat));
    const double regionSeconds = std::max (0.001, endTime.inSeconds() - startTime.inSeconds());

    double sampleRate = 44100.0;
    for (const auto& lane : lanes)
    {
        for (const auto& take : lane)
        {
            const juce::File f (take.getProperty (kFileProp).toString());
            if (! f.existsAsFile())
                continue;
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (f)))
            {
                sampleRate = reader->sampleRate;
                break;
            }
        }
    }

    const int totalSamples = juce::jlimit (1, 44100 * 60 * 10,
                                          static_cast<int> (std::ceil (regionSeconds * sampleRate)));
    juce::AudioBuffer<float> compBuffer (2, totalSamples);
    compBuffer.clear();

    int contributingLanes = 0;
    const int fadeSamples = juce::jlimit (0, totalSamples / 4,
                                          static_cast<int> (std::ceil (crossfadeMs * 0.001 * sampleRate)));

    for (int li = 0; li < lanes.getNumChildren(); ++li)
    {
        if (laneIndex >= 0 && li != laneIndex)
            continue;

        auto lane = lanes.getChild (li);
        if (lane.getNumChildren() == 0)
            continue;

        const int active = juce::jlimit (0, lane.getNumChildren() - 1,
                                         static_cast<int> (lane.getProperty (kActiveProp, 0)));
        auto take = lane.getChild (active);
        const juce::File takeFile (take.getProperty (kFileProp).toString());
        if (! takeFile.existsAsFile())
            continue;

        auto reader = std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (takeFile));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            continue;

        const double takeStart = static_cast<double> (take.getProperty (kStartProp, startBeat));
        const double takeEnd = static_cast<double> (take.getProperty (kEndProp, endBeat));
        const double takeStartSec = edit.tempoSequence.toTime (
            tracktion::BeatPosition::fromBeats (std::max (startBeat, takeStart))).inSeconds();
        const double takeEndSec = edit.tempoSequence.toTime (
            tracktion::BeatPosition::fromBeats (std::min (endBeat, takeEnd))).inSeconds();

        const int destOffset = juce::jlimit (0, totalSamples - 1,
            static_cast<int> (std::floor ((takeStartSec - startTime.inSeconds()) * sampleRate)));
        const int readLen = juce::jlimit (0, totalSamples - destOffset,
            static_cast<int> (std::floor ((takeEndSec - takeStartSec) * sampleRate)));
        if (readLen <= 0)
            continue;

        juce::AudioBuffer<float> takeBuffer (2, readLen);
        takeBuffer.clear();
        reader->read (&takeBuffer, 0, readLen, 0, true, true);

        for (int s = 0; s < readLen; ++s)
        {
            float fade = 1.0f;
            if (fadeSamples > 0 && contributingLanes > 0)
            {
                if (s < fadeSamples)
                    fade = std::sin (0.5f * juce::MathConstants<float>::pi
                                     * static_cast<float> (s) / static_cast<float> (fadeSamples));
                else if (s > readLen - fadeSamples)
                    fade = std::sin (0.5f * juce::MathConstants<float>::pi
                                     * static_cast<float> (readLen - s) / static_cast<float> (fadeSamples));
            }

            for (int ch = 0; ch < 2; ++ch)
                compBuffer.addSample (ch, destOffset + s, takeBuffer.getSample (ch, s) * fade);
        }
        ++contributingLanes;
    }

    if (contributingLanes == 0)
        return {};

    for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < totalSamples; ++s)
            compBuffer.setSample (ch, s, juce::jlimit (-1.0f, 1.0f, compBuffer.getSample (ch, s)));

    const juce::File takeDir = takeFileDirForLanes (lanes);
    takeDir.createDirectory();
    const juce::File compFile = takeDir.getNonexistentChildFile ("Comp", ".wav", false);
    {
        auto fs = std::make_unique<juce::FileOutputStream> (compFile);
        if (! fs->openedOk())
            return {};
        fs->setPosition (0);
        fs->truncate();
        std::unique_ptr<juce::OutputStream> os (fs.release());
        juce::WavAudioFormat wavFormat;
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16);
        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (os, options));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (compBuffer, 0, totalSamples))
            return {};
    }

    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("Flatten Take Comp");
    const tracktion::TimeRange clipRange {
        edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat)),
        edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endBeat))
    };
    const te::ClipPosition clipPos { clipRange, tracktion::TimeDuration::fromSeconds (0.0) };

    if (auto clip = track.insertWaveClip ("Take Comp", compFile, clipPos, false))
    {
        clip->getSourceFileReference().setToFile (
            compFile, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
        return compFile;
    }
    return {};
}

void RecordingManager::configurePunchAndRoll (double preRollSeconds, double crossfadeMs)
{
    punchPreRollSeconds = juce::jlimit (1.0, 10.0, preRollSeconds);
    punchCrossfadeMs = juce::jlimit (5.0, 100.0, crossfadeMs);
}

juce::File RecordingManager::executePunchAndRollSplice (te::Edit& edit, te::AudioTrack& track,
                                                        const juce::File& priorWav, const juce::File& newTakeWav,
                                                        double punchTimeSeconds, double crossfadeMs,
                                                        const juce::File& outWav)
{
    if (! priorWav.existsAsFile() || ! newTakeWav.existsAsFile()
        || ! std::isfinite (punchTimeSeconds) || punchTimeSeconds <= 0.0
        || ! std::isfinite (crossfadeMs) || crossfadeMs < 0.0)
        return {};

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> priorReader (fm.createReaderFor (priorWav));
    std::unique_ptr<juce::AudioFormatReader> newReader (fm.createReaderFor (newTakeWav));
    if (priorReader == nullptr || newReader == nullptr
        || priorReader->sampleRate <= 0.0 || newReader->lengthInSamples <= 0)
        return {};

    const double sampleRate = priorReader->sampleRate;
    const int channels = static_cast<int> (std::min<unsigned int> (
        2u, std::max<unsigned int> (1u, std::max (priorReader->numChannels, newReader->numChannels))));
    const int punchSamples = static_cast<int> (std::min<juce::int64> (
        priorReader->lengthInSamples, static_cast<juce::int64> (std::lround (punchTimeSeconds * sampleRate))));
    const int totalSamples = static_cast<int> (std::min<juce::int64> (
        newReader->lengthInSamples, static_cast<juce::int64> (60.0 * sampleRate)));
    if (punchSamples <= 0 || totalSamples <= 0)
        return {};

    const int xfadeSamples = juce::jlimit (0, punchSamples - 1,
        static_cast<int> (std::lround (crossfadeMs * 0.001 * sampleRate)));

    juce::AudioBuffer<float> priorBuffer (channels, punchSamples);
    juce::AudioBuffer<float> newBuffer (channels, totalSamples);
    priorBuffer.clear();
    newBuffer.clear();
    priorReader->read (&priorBuffer, 0, punchSamples, 0, true, channels > 1);
    newReader->read (&newBuffer, 0, totalSamples, 0, true, channels > 1);

    juce::AudioBuffer<float> stitched (channels, totalSamples);
    stitched.clear();
    const int fadeStart = punchSamples - xfadeSamples;
    for (int ch = 0; ch < channels; ++ch)
    {
        for (int s = 0; s < punchSamples; ++s)
            stitched.setSample (ch, s, priorBuffer.getSample (ch, s));
        for (int s = fadeStart; s < punchSamples; ++s)
        {
            const float t = xfadeSamples > 0
                ? static_cast<float> (s - fadeStart) / static_cast<float> (xfadeSamples) : 1.0f;
            const float fadeOut = std::cos (t * juce::MathConstants<float>::halfPi);
            const float fadeIn  = std::sin (t * juce::MathConstants<float>::halfPi);
            stitched.setSample (ch, s, priorBuffer.getSample (ch, s) * fadeOut
                                        + newBuffer.getSample (ch, s) * fadeIn);
        }
        for (int s = punchSamples; s < totalSamples; ++s)
            stitched.setSample (ch, s, newBuffer.getSample (ch, s));
    }

    outWav.getParentDirectory().createDirectory();
    {
        auto fs = std::make_unique<juce::FileOutputStream> (outWav);
        if (! fs->openedOk())
            return {};
        fs->setPosition (0);
        fs->truncate();
        std::unique_ptr<juce::OutputStream> os (fs.release());
        juce::WavAudioFormat wavFormat;
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate (sampleRate).withNumChannels (channels).withBitsPerSample (16);
        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (os, options));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (stitched, 0, totalSamples))
            return {};
    }

    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("Punch and Roll Splice");
    const double durationSec = static_cast<double> (totalSamples) / sampleRate;
    const double endBeat = edit.tempoSequence
        .toBeats (tracktion::TimePosition::fromSeconds (durationSec)).inBeats();
    addRecordedTake (edit, track, outWav, 0.0, endBeat, false);

    const tracktion::TimeRange clipRange {
        tracktion::TimePosition::fromSeconds (0.0),
        tracktion::TimePosition::fromSeconds (durationSec)
    };
    const te::ClipPosition clipPos { clipRange, tracktion::TimeDuration::fromSeconds (0.0) };
    if (auto clip = track.insertWaveClip ("Punch Splice", outWav, clipPos, false))
        clip->getSourceFileReference().setToFile (
            outWav, te::SourceFileReference::PathStyle::alwaysAbsolute, false);

    return outWav;
}

bool RecordingManager::tagTakeSlate (te::Edit& edit, te::AudioTrack& track,
                                     int takeIndex, const juce::String& slate)
{
    auto lanes = track.state.getChildWithName (kLanesTree);
    if (! lanes.isValid() || takeIndex < 0)
        return false;

    int flat = 0;
    for (auto lane : lanes)
    {
        if (lane.getType() != kLaneTree)
            continue;
        for (auto take : lane)
        {
            if (take.getType() != kTakeTree)
                continue;
            if (flat == takeIndex)
            {
                auto& um = edit.getUndoManager();
                um.beginNewTransaction ("Tag Take Slate");
                take.setProperty (kSlateProp, slate.trim(), &um);
                return true;
            }
            ++flat;
        }
    }
    return false;
}

bool RecordingManager::exportAbcAuditionString (te::AudioTrack& track,
                                                const std::vector<int>& takeIndices,
                                                double gapSeconds, const juce::File& outWav,
                                                bool monoMixdown)
{
    if (takeIndices.empty() || ! std::isfinite (gapSeconds) || gapSeconds < 0.0)
        return false;

    const auto allTakes = getTrackTakes (track);
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    juce::int64 totalSamples = 0;
    const double sampleRate = 44100.0;
    const int gapSamples = static_cast<int> (std::lround (gapSeconds * sampleRate));

    for (int idx : takeIndices)
    {
        if (! juce::isPositiveAndBelow (idx, allTakes.size()))
            return false;
        const juce::File file (allTakes[idx].file);
        if (! file.existsAsFile())
            return false;
        readers.push_back (std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (file)));
        if (readers.back() == nullptr || readers.back()->lengthInSamples <= 0)
            return false;
        totalSamples += readers.back()->lengthInSamples;
    }
    totalSamples += static_cast<juce::int64> (gapSamples) * static_cast<juce::int64> (readers.size() - 1);
    if (totalSamples <= 0 || totalSamples > static_cast<juce::int64> (10.0 * sampleRate))
        return false;

    const int channels = monoMixdown ? 1 : 2;
    juce::AudioBuffer<float> output (channels, static_cast<int> (totalSamples));
    output.clear();

    int dest = 0;
    for (size_t i = 0; i < readers.size(); ++i)
    {
        auto* reader = readers[i].get();
        const int len = static_cast<int> (reader->lengthInSamples);
        juce::AudioBuffer<float> takeBuffer (2, len);
        takeBuffer.clear();
        reader->read (&takeBuffer, 0, len, 0, true, reader->numChannels > 1);

        for (int s = 0; s < len; ++s)
        {
            const float l = takeBuffer.getSample (0, s);
            const float r = reader->numChannels > 1 ? takeBuffer.getSample (1, s) : l;
            output.setSample (0, dest + s, monoMixdown ? 0.5f * (l + r) : l);
            if (! monoMixdown)
                output.setSample (1, dest + s, r);
        }
        dest += len + gapSamples;
    }

    outWav.getParentDirectory().createDirectory();
    auto fs = std::make_unique<juce::FileOutputStream> (outWav);
    if (! fs->openedOk())
        return false;
    fs->setPosition (0);
    fs->truncate();
    std::unique_ptr<juce::OutputStream> os (fs.release());
    juce::WavAudioFormat wavFormat;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate).withNumChannels (channels).withBitsPerSample (16);
    std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (os, options));
    if (writer == nullptr)
        return false;

    return writer->writeFromAudioSampleBuffer (output, 0, output.getNumSamples());
}
