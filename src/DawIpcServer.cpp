#include "DawIpcServer.h"

#include "AudioToMidiTranscriber.h"
#include "AutomationManager.h"
#include "HardwareRackPlugins.h"
#include "MixdownExporter.h"
#include "PluginPresetManager.h"
#include "WaveformEditorComponent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace
{
std::vector<int> getScaleIntervals (const juce::String& rawScale, bool allowPentatonic)
{
    const auto s = rawScale.trim().toLowerCase();
    if (s == "major" || s == "ionian")
        return { 0, 2, 4, 5, 7, 9, 11 };
    if (s == "dorian")
        return { 0, 2, 3, 5, 7, 9, 10 };
    if (s == "phrygian")
        return { 0, 1, 3, 5, 7, 8, 10 };
    if (allowPentatonic && (s == "pentatonic_minor" || s == "minor_pentatonic"))
        return { 0, 3, 5, 7, 10 };
    if (s == "minor" || s == "natural_minor" || s == "aeolian")
        return { 0, 2, 3, 5, 7, 8, 10 };
    return {};
}

bool parseRomanChordToken (const juce::String& rawToken,
                           int keyPc,
                           const std::vector<int>& scale,
                           std::vector<int>& outPitchClasses)
{
    outPitchClasses.clear();
    if (scale.size() < 7)
        return false;

    juce::String t = rawToken.trim();
    if (t.isEmpty())
        return false;

    int accidental = 0;
    while (t.startsWithChar ('b') || t.startsWithChar ('#'))
    {
        accidental += (t[0] == 'b') ? -1 : 1;
        t = t.substring (1);
    }

    int len = 0;
    while (len < t.length())
    {
        const auto ch = juce::CharacterFunctions::toUpperCase (t[len]);
        if (ch == 'I' || ch == 'V')
            ++len;
        else
            break;
    }

    if (len == 0)
        return false;

    const auto romanPart = t.substring (0, len);
    const auto romanUpper = romanPart.toUpperCase();
    int degree = -1;
    if (romanUpper == "I")        degree = 0;
    else if (romanUpper == "II")  degree = 1;
    else if (romanUpper == "III") degree = 2;
    else if (romanUpper == "IV")  degree = 3;
    else if (romanUpper == "V")   degree = 4;
    else if (romanUpper == "VI")  degree = 5;
    else if (romanUpper == "VII") degree = 6;
    else                          return false;

    const auto suffix = t.substring (len).trim().toLowerCase();
    const bool isLower = (romanPart == romanPart.toLowerCase());

    const int rootPc = (keyPc + scale[static_cast<size_t> (degree)] + accidental + 120) % 12;
    int thirdInterval = ((scale[static_cast<size_t> ((degree + 2) % 7)] + (degree + 2 >= 7 ? 12 : 0))
                         - scale[static_cast<size_t> (degree)]);
    int fifthInterval = ((scale[static_cast<size_t> ((degree + 4) % 7)] + (degree + 4 >= 7 ? 12 : 0))
                         - scale[static_cast<size_t> (degree)]);

    if (isLower && thirdInterval > 3)
        thirdInterval = 3;
    else if (! isLower && thirdInterval < 4 && ! suffix.contains ("dim"))
        thirdInterval = 4;

    if (suffix.contains ("dim"))
    {
        thirdInterval = 3;
        fifthInterval = 6;
    }

    outPitchClasses.push_back (rootPc);
    outPitchClasses.push_back ((rootPc + thirdInterval) % 12);
    outPitchClasses.push_back ((rootPc + fifthInterval) % 12);

    if (suffix.contains ("maj7"))
        outPitchClasses.push_back ((rootPc + 11) % 12);
    else if (suffix.contains ("m7") || suffix.contains ("7"))
    {
        if (suffix.contains ("m7"))
            outPitchClasses[1] = (rootPc + 3) % 12;
        outPitchClasses.push_back ((rootPc + 10) % 12);
    }

    return true;
}

std::vector<int> buildVoicedChord (const std::vector<int>& pcs,
                                   int baseOctaveMidi,
                                   const std::vector<int>& prevNotes,
                                   bool voiceLead)
{
    std::vector<int> baseNotes;
    baseNotes.reserve (pcs.size());

    for (size_t i = 0; i < pcs.size(); ++i)
    {
        int midi = baseOctaveMidi + pcs[i];
        while (midi < baseOctaveMidi)
            midi += 12;
        if (i > 0 && midi <= baseNotes.back())
            midi += 12;
        baseNotes.push_back (juce::jlimit (24, 96, midi));
    }

    if (! voiceLead || prevNotes.empty() || baseNotes.empty())
        return baseNotes;

    int prevSum = 0;
    for (int n : prevNotes)
        prevSum += n;
    const double prevCentroid = static_cast<double> (prevSum) / static_cast<double> (prevNotes.size());

    // Generate compact inversions by octave-shifting each tone around prevCentroid.
    std::vector<int> best = baseNotes;
    double bestCost = 1.0e9;

    const int nTones = static_cast<int> (baseNotes.size());
    std::vector<std::vector<int>> candidates (static_cast<size_t> (nTones));
    for (int t = 0; t < nTones; ++t)
    {
        const int pc = baseNotes[static_cast<size_t> (t)] % 12;
        for (int oct = 2; oct <= 6; ++oct)
        {
            const int note = oct * 12 + pc;
            if (note >= 36 && note <= 88)
                candidates[static_cast<size_t> (t)].push_back (note);
        }
    }

    std::vector<int> cur (static_cast<size_t> (nTones), 60);
    auto search = [&] (auto& self, int idx) -> void
    {
        if (idx == nTones)
        {
            auto sorted = cur;
            std::sort (sorted.begin(), sorted.end());
            if (sorted.back() - sorted.front() > 16)
                return;

            double motion = 0.0;
            double curSum = 0.0;
            for (int i = 0; i < nTones; ++i)
            {
                const int ref = prevNotes[static_cast<size_t> (std::min (i, static_cast<int> (prevNotes.size()) - 1))];
                motion += std::abs (sorted[static_cast<size_t> (i)] - ref);
                curSum += sorted[static_cast<size_t> (i)];
            }
            const double centroidDiff = std::abs ((curSum / nTones) - prevCentroid);
            const double totalCost = motion + 0.35 * centroidDiff;
            if (totalCost < bestCost)
            {
                bestCost = totalCost;
                best = sorted;
            }
            return;
        }

        for (int note : candidates[static_cast<size_t> (idx)])
        {
            cur[static_cast<size_t> (idx)] = note;
            self (self, idx + 1);
        }
    };

    search (search, 0);
    return best;
}

int snapMidiPitchToScale (int pitch, int keyPc, const std::vector<int>& scale)
{
    auto isInScale = [&] (int p)
    {
        const int rel = ((p - keyPc) % 12 + 12) % 12;
        return std::find (scale.begin(), scale.end(), rel) != scale.end();
    };

    if (isInScale (pitch))
        return pitch;

    for (int d = 1; d <= 6; ++d)
    {
        if (pitch - d >= 0 && isInScale (pitch - d))
            return pitch - d; // tie-break downward
        if (pitch + d <= 127 && isInScale (pitch + d))
            return pitch + d;
    }

    return pitch;
}
} // namespace

DawIpcServer::DawIpcServer (RiffLibrary& targetRiffLibrary,
                            RecordingManager& targetRecordingManager)
    : juce::Thread ("DawIpcServer"),
      riffLibrary (targetRiffLibrary),
      recordingManager (targetRecordingManager)
{
}

DawIpcServer::~DawIpcServer()
{
    stopListening();
    setEdit (nullptr);
}

void DawIpcServer::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
}

bool DawIpcServer::startListening (int listenPort)
{
    stopListening();

    if (! serverSocket.createListener (listenPort, "127.0.0.1"))
        return false;

    aliveFlag = std::make_shared<std::atomic<bool>> (true);
    boundPort.store (listenPort, std::memory_order_release);
    listening.store (true, std::memory_order_release);

    startThread();

    return true;
}

void DawIpcServer::stopListening()
{
    if (aliveFlag != nullptr)
        aliveFlag->store (false, std::memory_order_release);

    signalThreadShouldExit();
    listening.store (false, std::memory_order_release);
    serverSocket.close();

    {
        std::lock_guard<std::mutex> lock (queueMutex);

        while (! pendingQueue.empty())
        {
            pendingQueue.front().responsePromise->set_value (
                makeErrorResponse (juce::var(), -32001, "IPC server shutting down"));
            pendingQueue.pop_front();
        }
    }

    stopThread (2000);

    boundPort.store (0, std::memory_order_release);
}

void DawIpcServer::run()
{
    while (! threadShouldExit())
    {
        const int ready = serverSocket.waitUntilReady (true, 200);

        if (threadShouldExit())
            break;

        if (ready <= 0)
            continue;

        std::unique_ptr<juce::StreamingSocket> client (serverSocket.waitForNextConnection());

        if (client != nullptr)
            handleClient (*client);
    }
}

void DawIpcServer::handleClient (juce::StreamingSocket& client)
{
    juce::MemoryBlock lineBuffer;
    char ch = 0;

    while (! threadShouldExit() && client.isConnected())
    {
        const int ready = client.waitUntilReady (true, 200);

        if (threadShouldExit())
            break;

        if (ready < 0)
            break;

        if (ready == 0)
            continue;

        const int bytesRead = client.read (&ch, 1, false);

        if (bytesRead <= 0)
            break;

        if (ch == '\n')
        {
            const auto rawJson = juce::String::fromUTF8 (
                static_cast<const char*> (lineBuffer.getData()),
                static_cast<int> (lineBuffer.getSize())).trim();

            lineBuffer.reset();

            if (rawJson.isNotEmpty())
            {
                const auto responseJson = enqueueAndWaitForMessageThread (rawJson) + "\n";

                client.write (responseJson.toRawUTF8(),
                              static_cast<int> (responseJson.getNumBytesAsUTF8()));
            }
        }
        else if (ch != '\r')
        {
            if (lineBuffer.getSize() >= maxJsonFrameBytes)
            {
                const auto errJson = makeErrorResponse (juce::var(), -32600, "JSON-RPC frame exceeds 4 MiB limit") + "\n";
                client.write (errJson.toRawUTF8(), static_cast<int> (errJson.getNumBytesAsUTF8()));
                lineBuffer.reset();
                break;
            }

            lineBuffer.append (&ch, 1);
        }
    }
}

juce::String DawIpcServer::enqueueAndWaitForMessageThread (const juce::String& rawJson)
{
    auto promise = std::make_shared<std::promise<juce::String>>();
    auto future  = promise->get_future();

    {
        std::lock_guard<std::mutex> lock (queueMutex);
        pendingQueue.push_back ({ rawJson, promise });
    }

    const auto flag = aliveFlag;
    juce::MessageManager::callAsync ([this, flag]
    {
        if (flag != nullptr && flag->load (std::memory_order_acquire))
            servicePendingRequestsOnMessageThread();
    });

    // Per-method wait: long-running audio operations (mixdown rendering and
    // synchronous YIN transcription on large stems) get extended budgets,
    // while every other method keeps the 2.5 s budget so a hung message
    // thread fails fast.
    int timeoutMs = 2500;

    {
        juce::var parsed;

        if (juce::JSON::parse (rawJson, parsed).wasOk())
        {
            const auto reqMethod = parsed.getProperty ("method", "").toString();

            if (reqMethod == "audio.export_wav")
                timeoutMs = 30000;
            else if (reqMethod == "audio.transcribe_to_midi")
                timeoutMs = 15000;
            else if (reqMethod == "mixer.auto_balance_lufs" || reqMethod == "arrangement.duplicate_section")
                timeoutMs = 10000;
        }
    }

    if (future.wait_for (std::chrono::milliseconds (timeoutMs)) == std::future_status::ready)
        return future.get();

    return makeErrorResponse (juce::var(), -32000, "DAW message thread timeout");
}

void DawIpcServer::servicePendingRequestsOnMessageThread()
{
    for (;;)
    {
        PendingRpcRequest req;

        {
            std::lock_guard<std::mutex> lock (queueMutex);

            if (pendingQueue.empty())
                break;

            req = std::move (pendingQueue.front());
            pendingQueue.pop_front();
        }

        req.responsePromise->set_value (executeJsonRpcOnMessageThread (req.rawJson));
    }
}

juce::String DawIpcServer::executeJsonRpcOnMessageThread (const juce::String& rawJson)
{
    juce::var parsed;

    const auto parseRes = juce::JSON::parse (rawJson, parsed);

    if (parseRes.failed() || ! parsed.isObject())
        return makeErrorResponse (juce::var(), -32700, "Parse error");

    const juce::var id        = parsed.getProperty ("id", juce::var());
    const juce::String method = parsed.getProperty ("method", "").toString();
    const juce::var params    = parsed.getProperty ("params", juce::var());

    if (method.isEmpty())
        return makeErrorResponse (id, -32600, "Invalid Request: missing method");

    if (edit == nullptr)
        return makeErrorResponse (id, -32002, "No active Edit loaded");

    try
    {
        return dispatchMethod (id, method, params);
    }
    catch (const std::exception& e)
    {
        return makeErrorResponse (id, -32603, e.what());
    }
    catch (...)
    {
        return makeErrorResponse (id, -32603, "Internal RPC error");
    }
}

int DawIpcServer::getInsertIndexBeforeTerminalPlugins (te::AudioTrack& track)
{
    return AutomationManager::getInsertIndexBeforeTerminalPlugins (track);
}

juce::String DawIpcServer::dispatchMethod (const juce::var& id,
                                           const juce::String& method,
                                           const juce::var& params)
{
    // 1. daw.get_state (read-only)
    if (method == "daw.get_state")
    {
        auto* root = new juce::DynamicObject();

        const auto posTime   = edit->getTransport().getPosition();
        const auto loopRange = edit->getTransport().getLoopRange();

        root->setProperty ("bpm", edit->tempoSequence.getTempo (0)->getBpm());
        root->setProperty ("is_playing", edit->getTransport().isPlaying());
        root->setProperty ("is_recording", recordingManager.isRecording() || edit->getTransport().isRecording());
        root->setProperty ("position_seconds", posTime.inSeconds());
        root->setProperty ("position_beats", edit->tempoSequence.toBeats (posTime).inBeats());
        root->setProperty ("click_enabled", edit->clickTrackEnabled.get());
        root->setProperty ("count_in_bars", RecordingManager::getCountInBars (*edit));
        root->setProperty ("looping", edit->getTransport().looping.get());
        root->setProperty ("loop_start_beat", edit->tempoSequence.toBeats (loopRange.getStart()).inBeats());
        root->setProperty ("loop_end_beat", edit->tempoSequence.toBeats (loopRange.getEnd()).inBeats());
        root->setProperty ("rack_count", edit->getRackList().getTypes().size());

        juce::Array<juce::var> trackList;

        auto tracks = te::getAudioTracks (*edit);

        for (int i = 0; i < tracks.size(); ++i)
        {
            auto* tObj = new juce::DynamicObject();

            tObj->setProperty ("index", i);
            tObj->setProperty ("name", tracks[i]->getName());
            tObj->setProperty ("mute", tracks[i]->isMuted (false));
            tObj->setProperty ("solo", tracks[i]->isSolo (false));
            tObj->setProperty ("armed", RecordingManager::isTrackArmed (*tracks[i]));
            tObj->setProperty ("input_device", RecordingManager::getTrackInputName (*tracks[i]));
            tObj->setProperty ("clip_count", tracks[i]->getClips().size());

            if (auto* vol = tracks[i]->getVolumePlugin())
            {
                tObj->setProperty ("volume_db", vol->getVolumeDb());
                tObj->setProperty ("pan", vol->getPan());
            }

            auto* eq = tracks[i]->getEqualiserPlugin();
            tObj->setProperty ("has_eq", eq != nullptr);
            tObj->setProperty ("eq_low_db",  eq != nullptr ? eq->loGainValue.get()   : 0.0f);
            tObj->setProperty ("eq_mid_db",  eq != nullptr ? eq->midGainValue1.get() : 0.0f);
            tObj->setProperty ("eq_high_db", eq != nullptr ? eq->hiGainValue.get()   : 0.0f);

            trackList.add (juce::var (tObj));
        }

        root->setProperty ("tracks", trackList);

        return makeResultResponse (id, juce::var (root));
    }

    // 2. track.create
    if (method == "track.create")
    {
        const juce::String name       = params.getProperty ("name", "AI Track").toString();
        const juce::String pluginType = params.getProperty ("plugin_type", "4osc").toString().toLowerCase();

        edit->getUndoManager().beginNewTransaction ("MCP Create Track");

        const int newCount = te::getAudioTracks (*edit).size() + 1;

        edit->ensureNumberOfAudioTracks (newCount);

        auto tracks = te::getAudioTracks (*edit);
        auto* newTrack = tracks.getLast();

        newTrack->setName (name);

        if (pluginType == "4osc" || pluginType == "tr808" || pluginType == "sampler"
            || pluginType == "tape")
        {
            const char* typeName = te::FourOscPlugin::xmlTypeName;

            if (pluginType == "tr808")   typeName = Tr808DrumPlugin::xmlTypeName;
            else if (pluginType == "sampler") typeName = VintageSamplerPlugin::xmlTypeName;
            else if (pluginType == "tape")    typeName = AnalogTapePlugin::xmlTypeName;

            if (auto synth = edit->getPluginCache().createNewPlugin (typeName, {}))
                newTrack->pluginList.insertPlugin (synth, getInsertIndexBeforeTerminalPlugins (*newTrack), nullptr);
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP created track: " + name);

        auto* res = new juce::DynamicObject();

        res->setProperty ("track_index", tracks.size() - 1);
        res->setProperty ("name", name);

        return makeResultResponse (id, juce::var (res));
    }

    // 3. midi.add_notes
    if (method == "midi.add_notes")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        const auto* notesArray = params.getProperty ("notes", juce::var()).getArray();

        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()) || notesArray == nullptr)
            return makeErrorResponse (id, -32602, "Invalid track_index or notes array");

        auto* track = tracks[trackIdx];
        const juce::String clipName = params.getProperty ("clip_name", "AI Pattern").toString();

        double maxEndBeat = 4.0;

        for (const auto& n : *notesArray)
        {
            const double endB = std::max (0.0, static_cast<double> (n.getProperty ("start_beat", 0.0)))
                              + std::max (0.0625, static_cast<double> (n.getProperty ("length_beats", 0.5)));

            maxEndBeat = std::max (maxEndBeat, std::ceil (endB / 4.0) * 4.0);
        }

        const tracktion::TimeRange clipRange {
            tracktion::TimePosition::fromSeconds (0.0),
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (maxEndBeat))
        };

        auto& um = edit->getUndoManager();

        um.beginNewTransaction ("MCP Add MIDI Notes");

        te::MidiClip* midiClip = nullptr;

        for (auto* c : track->getClips())
        {
            if (auto* mc = dynamic_cast<te::MidiClip*> (c))
            {
                midiClip = mc;
                break;
            }
        }

        if (midiClip == nullptr)
        {
            midiClip = dynamic_cast<te::MidiClip*> (
                track->insertNewClip (te::TrackItem::Type::midi, clipName, clipRange, nullptr));
        }

        int addedCount = 0;

        if (midiClip != nullptr)
        {
            if (midiClip->getPosition().getEnd() < clipRange.getEnd())
                midiClip->setEnd (clipRange.getEnd(), true);

            auto& seq = midiClip->getSequence();

            for (const auto& n : *notesArray)
            {
                const int pitch     = juce::jlimit (0, 127, static_cast<int> (n.getProperty ("pitch", 60)));
                const double startB = std::max (0.0, static_cast<double> (n.getProperty ("start_beat", 0.0)));
                const double lenB   = std::max (0.0625, static_cast<double> (n.getProperty ("length_beats", 0.5)));
                const int vel       = juce::jlimit (1, 127, static_cast<int> (n.getProperty ("velocity", 100)));

                seq.addNote (pitch,
                             tracktion::BeatPosition::fromBeats (startB),
                             tracktion::BeatDuration::fromBeats (lenB),
                             vel,
                             0,
                             &um);

                ++addedCount;
            }
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP added " + juce::String (addedCount) + " MIDI notes");

        auto* res = new juce::DynamicObject();

        res->setProperty ("added", addedCount);
        res->setProperty ("clip_name", midiClip != nullptr ? midiClip->getName() : clipName);

        return makeResultResponse (id, juce::var (res));
    }

    // 4. midi.get_notes (read-only)
    if (method == "midi.get_notes")
    {
        const int trackIdx   = static_cast<int> (params.getProperty ("track_index", 0));
        const double minBeat = static_cast<double> (params.getProperty ("start_beat", 0.0));
        const double maxBeat = static_cast<double> (params.getProperty ("end_beat", 99999.0));

        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        juce::Array<juce::var> notesOut;

        for (auto* c : tracks[trackIdx]->getClips())
        {
            if (auto* mc = dynamic_cast<te::MidiClip*> (c))
            {
                const double clipOffsetBeats = edit->tempoSequence.toBeats (mc->getPosition().getStart()).inBeats();

                for (auto* note : mc->getSequence().getNotes())
                {
                    const double absBeat = clipOffsetBeats + note->getStartBeat().inBeats();

                    if (absBeat >= minBeat && absBeat <= maxBeat)
                    {
                        auto* nObj = new juce::DynamicObject();

                        nObj->setProperty ("pitch", note->getNoteNumber());
                        nObj->setProperty ("start_beat", absBeat);
                        nObj->setProperty ("length_beats", note->getLengthBeats().inBeats());
                        nObj->setProperty ("velocity", note->getVelocity());

                        notesOut.add (juce::var (nObj));
                    }
                }
            }
        }

        auto* res = new juce::DynamicObject();

        res->setProperty ("track_index", trackIdx);
        res->setProperty ("notes", notesOut);

        return makeResultResponse (id, juce::var (res));
    }

    // 5. mixer.set_track
    if (method == "mixer.set_track")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));

        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        edit->getUndoManager().beginNewTransaction ("MCP Mixer Update");

        auto* track = tracks[trackIdx];

        if (params.hasProperty ("volume_db"))
        {
            const float db = juce::jlimit (-60.0f, 6.0f, static_cast<float> (params["volume_db"]));

            if (auto* vol = track->getVolumePlugin())
                vol->setVolumeDb (db);
        }

        if (params.hasProperty ("pan"))
        {
            const float pan = juce::jlimit (-1.0f, 1.0f, static_cast<float> (params["pan"]));

            if (auto* vol = track->getVolumePlugin())
                vol->setPan (pan);
        }

        if (params.hasProperty ("mute"))
            track->setMute (static_cast<bool> (params["mute"]));

        if (params.hasProperty ("solo"))
            track->setSolo (static_cast<bool> (params["solo"]));

        if (params.hasProperty ("input_device"))
            recordingManager.setTrackInputName (*edit, *track, params["input_device"].toString(), false);

        if (params.hasProperty ("armed"))
            RecordingManager::setTrackArmed (*edit, *track, static_cast<bool> (params["armed"]), false);

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP updated mixer on track " + juce::String (trackIdx + 1));

        auto* res = new juce::DynamicObject();

        res->setProperty ("updated_track", trackIdx);

        if (auto* vol = track->getVolumePlugin())
        {
            res->setProperty ("volume_db", vol->getVolumeDb());
            res->setProperty ("pan", vol->getPan());
        }

        res->setProperty ("mute", track->isMuted (false));
        res->setProperty ("solo", track->isSolo (false));
        res->setProperty ("armed", RecordingManager::isTrackArmed (*track));
        res->setProperty ("input_device", RecordingManager::getTrackInputName (*track));

        return makeResultResponse (id, juce::var (res));
    }

    // 6. transport.control
    if (method == "transport.control")
    {
        const juce::String action = params.getProperty ("action", "").toString().toLowerCase();

        if (params.hasProperty ("bpm"))
        {
            const double newBpm = juce::jlimit (40.0, 240.0, static_cast<double> (params["bpm"]));
            edit->tempoSequence.getTempo (0)->setBpm (newBpm);
        }

        if (params.hasProperty ("beat"))
        {
            const double targetBeat = std::max (0.0, static_cast<double> (params["beat"]));

            edit->getTransport().setPosition (
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (targetBeat)));
        }

        if (params.hasProperty ("click"))
        {
            edit->clickTrackGain = 0.7f;
            edit->clickTrackEmphasiseBars = true;
            edit->clickTrackEnabled = static_cast<bool> (params["click"]);
        }

        if (params.hasProperty ("loop_start_beat") || params.hasProperty ("loop_end_beat"))
        {
            const auto existingLoop = edit->getTransport().getLoopRange();
            const double curStartB = existingLoop.isEmpty()
                                         ? 0.0
                                         : edit->tempoSequence.toBeats (existingLoop.getStart()).inBeats();
            const double curEndB   = existingLoop.isEmpty()
                                         ? 16.0
                                         : edit->tempoSequence.toBeats (existingLoop.getEnd()).inBeats();
            const double startB = std::max (0.0, static_cast<double> (params.getProperty ("loop_start_beat", curStartB)));
            const double endB   = std::max (startB + 1.0,
                                            static_cast<double> (params.getProperty ("loop_end_beat",
                                                                                     std::max (startB + 4.0, curEndB))));

            edit->getTransport().setLoopRange ({
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startB)),
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endB))
            });
        }

        if (params.hasProperty ("loop"))
        {
            const bool loopOn = static_cast<bool> (params["loop"]);

            if (loopOn && edit->getTransport().getLoopRange().isEmpty())
            {
                edit->getTransport().setLoopRange ({
                    tracktion::TimePosition::fromSeconds (0.0),
                    edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (16.0))
                });
            }

            edit->getTransport().looping = loopOn;
        }

        if (params.hasProperty ("count_in_bars"))
            RecordingManager::setCountInBars (*edit, static_cast<int> (params["count_in_bars"]));

        if (action == "play")
        {
            edit->getTransport().play (false);
        }
        else if (action == "record")
        {
            const bool headless = juce::JUCEApplicationBase::getCommandLineParameters().contains ("--smoke-test");
            recordingManager.startRecording (headless);
        }
        else if (action == "stop")
        {
            if (recordingManager.isRecording())
                recordingManager.stopRecording();
            else
                edit->getTransport().stop (false, false);
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP transport update");

        auto* res = new juce::DynamicObject();
        const auto loopRange = edit->getTransport().getLoopRange();

        res->setProperty ("is_playing", edit->getTransport().isPlaying());
        res->setProperty ("is_recording", recordingManager.isRecording() || edit->getTransport().isRecording());
        res->setProperty ("bpm", edit->tempoSequence.getTempo (0)->getBpm());
        res->setProperty ("position_beats",
                          edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats());
        res->setProperty ("click_enabled", edit->clickTrackEnabled.get());
        res->setProperty ("count_in_bars", RecordingManager::getCountInBars (*edit));
        res->setProperty ("looping", edit->getTransport().looping.get());
        res->setProperty ("loop_start_beat", edit->tempoSequence.toBeats (loopRange.getStart()).inBeats());
        res->setProperty ("loop_end_beat", edit->tempoSequence.toBeats (loopRange.getEnd()).inBeats());

        return makeResultResponse (id, juce::var (res));
    }

    // 7. bgm.generate
    if (method == "bgm.generate")
    {
        if (riffLibrary.getRiffs().empty())
            riffLibrary.ensureStarterPack (RiffLibrary::getDefaultLibraryDirectory());

        BgmRequest req;

        req.genre            = params.getProperty ("genre", "Cyberpunk").toString();
        req.targetKeyLabel   = params.getProperty ("key", "Am").toString();
        req.targetPitchClass = RiffLibrary::parseKeyPitchClass (req.targetKeyLabel);
        req.targetEnergy     = juce::jlimit (1, 5, static_cast<int> (params.getProperty ("energy", 4)));
        req.totalBars        = juce::jlimit (4, 32, static_cast<int> (params.getProperty ("bars", 8)));

        edit->getUndoManager().beginNewTransaction ("MCP Generate BGM");

        const int clipsCreated = BgmArrangementSolver::generateArrangement (*edit, riffLibrary, req);

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP generated BGM (" + juce::String (clipsCreated) + " clips)");

        auto* res = new juce::DynamicObject();

        res->setProperty ("clips_created", clipsCreated);
        res->setProperty ("genre", req.genre);
        res->setProperty ("key", req.targetKeyLabel);

        return makeResultResponse (id, juce::var (res));
    }

    // 8. audio.process_clip (non-destructive bounce-to-take DSP)
    if (method == "audio.process_clip")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        const int clipIdx  = static_cast<int> (params.getProperty ("clip_index", 0));

        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        int waveSeen = 0;
        te::WaveAudioClip* target = nullptr;

        for (auto* c : tracks[trackIdx]->getClips())
        {
            if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
            {
                if (waveSeen == clipIdx)
                {
                    target = wc;
                    break;
                }

                ++waveSeen;
            }
        }

        if (target == nullptr)
            return makeErrorResponse (id, -32602, "Invalid clip_index (no such WaveAudioClip)");

        const juce::String opName = params.getProperty ("operation", "silence").toString().toLowerCase();
        WaveformEditorComponent::DspOperation op {};

        if      (opName == "normalize") op = WaveformEditorComponent::DspOperation::normalize;
        else if (opName == "reverse")   op = WaveformEditorComponent::DspOperation::reverse;
        else if (opName == "fade_in")   op = WaveformEditorComponent::DspOperation::fadeIn;
        else if (opName == "fade_out")  op = WaveformEditorComponent::DspOperation::fadeOut;
        else if (opName == "silence")   op = WaveformEditorComponent::DspOperation::silence;
        else return makeErrorResponse (id, -32602, "Invalid operation: " + opName);

        const double n0 = static_cast<double> (params.getProperty ("start_norm", 0.0));
        const double n1 = static_cast<double> (params.getProperty ("end_norm", 1.0));

        juce::File bouncedFile;

        // Takes go next to the session when the owner wired the callback
        // (MainComponent does); otherwise the Documents fallback applies.
        const juce::File takesDir = getTakesDirectoryCallback
                                      ? getTakesDirectoryCallback()
                                      : WaveformEditorComponent::getDefaultTakesDirectory (juce::File());

        if (! WaveformEditorComponent::applyBounceToTakeDsp (
                *edit, target->itemID, op, n0, n1,
                takesDir,
                &bouncedFile))
        {
            return makeErrorResponse (id, -32603, "Bounce-to-take DSP failed");
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP processed audio clip: " + opName);

        auto* res = new juce::DynamicObject();

        res->setProperty ("bounced_file", bouncedFile.getFullPathName());
        res->setProperty ("file_name", bouncedFile.getFileName());

        return makeResultResponse (id, juce::var (res));
    }

    // 9. rack.get_state (read-only)
    if (method == "rack.get_state")
    {
        juce::Array<juce::var> racksOut;
        const auto& racks = edit->getRackList().getTypes();

        for (int r = 0; r < racks.size(); ++r)
        {
            auto* rack = racks[r];
            auto* rObj = new juce::DynamicObject();
            rObj->setProperty ("index", r);
            rObj->setProperty ("name", rack->rackName.get());

            juce::Array<juce::var> pluginsOut;

            for (auto* p : rack->getPlugins())
            {
                juce::StringArray ins, outs;
                p->getChannelNames (&ins, &outs);

                auto* pObj = new juce::DynamicObject();
                pObj->setProperty ("id", static_cast<juce::int64> (p->itemID.getRawID()));
                pObj->setProperty ("name", p->getName());
                pObj->setProperty ("type", p->getPluginType());

                juce::Array<juce::var> inArr, outArr;
                for (const auto& s : ins)  inArr.add (s);
                for (const auto& s : outs) outArr.add (s);
                pObj->setProperty ("inputs", inArr);
                pObj->setProperty ("outputs", outArr);
                pluginsOut.add (juce::var (pObj));
            }

            rObj->setProperty ("plugins", pluginsOut);

            juce::Array<juce::var> connsOut;

            for (auto* conn : rack->getConnections())
            {
                auto* cObj = new juce::DynamicObject();
                cObj->setProperty ("source_id",  static_cast<juce::int64> (conn->sourceID.get().getRawID()));
                cObj->setProperty ("source_pin", conn->sourcePin.get());
                cObj->setProperty ("dest_id",    static_cast<juce::int64> (conn->destID.get().getRawID()));
                cObj->setProperty ("dest_pin",   conn->destPin.get());
                connsOut.add (juce::var (cObj));
            }

            rObj->setProperty ("connections", connsOut);
            racksOut.add (juce::var (rObj));
        }

        auto* res = new juce::DynamicObject();
        res->setProperty ("racks", racksOut);
        return makeResultResponse (id, juce::var (res));
    }

    // 10. rack.patch (connect / disconnect virtual Audio, MIDI & CV cables)
    if (method == "rack.patch")
    {
        const int rackIdx = static_cast<int> (params.getProperty ("rack_index", 0));
        const juce::String action = params.getProperty ("action", "connect").toString().toLowerCase();

        if (action != "connect" && action != "disconnect")
            return makeErrorResponse (id, -32602, "Invalid rack.patch action: " + action);

        const auto& racks = edit->getRackList().getTypes();
        if (! juce::isPositiveAndBelow (rackIdx, racks.size()))
            return makeErrorResponse (id, -32602, "Invalid rack_index");

        const juce::int64 rawSrc = static_cast<juce::int64> (params.getProperty ("source_id", 0));
        const juce::int64 rawDst = static_cast<juce::int64> (params.getProperty ("dest_id", 0));
        const int srcPin = static_cast<int> (params.getProperty ("source_pin", 0));
        const int dstPin = static_cast<int> (params.getProperty ("dest_pin", 0));

        if (rawSrc < 0 || rawDst < 0 || srcPin < 0 || dstPin < 0)
            return makeErrorResponse (id, -32602, "Invalid negative ID or pin index");

        auto* rack = racks[rackIdx];
        const tracktion::EditItemID srcId = rawSrc > 0
                                               ? tracktion::EditItemID::fromRawID (static_cast<uint64_t> (rawSrc))
                                               : tracktion::EditItemID{};
        const tracktion::EditItemID dstId = rawDst > 0
                                               ? tracktion::EditItemID::fromRawID (static_cast<uint64_t> (rawDst))
                                               : tracktion::EditItemID{};

        edit->getUndoManager().beginNewTransaction ("MCP Rack Patch");

        const bool patched = (action == "disconnect")
                                 ? rack->removeConnection (srcId, srcPin, dstId, dstPin)
                                 : rack->addConnection (srcId, srcPin, dstId, dstPin);

        if (! patched)
            return makeErrorResponse (id, -32602, "Rack patch failed or connection was illegal/missing");

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP rack " + action + " complete");

        auto* res = new juce::DynamicObject();
        res->setProperty ("rack_index", rackIdx);
        res->setProperty ("action", action);
        res->setProperty ("connection_count", rack->getConnections().size());
        return makeResultResponse (id, juce::var (res));
    }

    // 11. audio.import_file (for external Arioso / Suno / ElevenLabs Python AI workers)
    if (method == "audio.import_file")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        const juce::String rawPath = params.getProperty ("file_path", "").toString().trim();
        if (rawPath.isEmpty())
            return makeErrorResponse (id, -32602, "Missing file_path");

        const juce::File wavFile = juce::File::isAbsolutePath (rawPath)
                                       ? juce::File (rawPath)
                                       : juce::File::getCurrentWorkingDirectory().getChildFile (rawPath);

        if (! wavFile.existsAsFile())
            return makeErrorResponse (id, -32602, "Audio file does not exist: " + wavFile.getFullPathName());

        const double startBeat = std::max (0.0, static_cast<double> (params.getProperty ("start_beat", 0.0)));
        double lengthBeats = static_cast<double> (params.getProperty ("length_beats", 0.0));

        if (lengthBeats <= 0.0)
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();

            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wavFile));

            if (reader != nullptr)
            {
                if (reader->sampleRate > 0.0 && reader->lengthInSamples > 0)
                {
                    const double sec = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
                    const double bpm = edit->tempoSequence.getTempo (0)->getBpm();
                    lengthBeats = std::max (1.0, sec * (bpm / 60.0));
                }
            }

            if (lengthBeats <= 0.0)
                lengthBeats = 4.0;
        }

        juce::String clipName = params.getProperty ("clip_name", "").toString().trim();
        if (clipName.isEmpty())
            clipName = wavFile.getFileNameWithoutExtension();

        const bool autoTempo   = static_cast<bool> (params.getProperty ("auto_tempo", false));
        const float pitchShift = static_cast<float> (static_cast<double> (params.getProperty ("pitch_shift", 0.0)));

        const tracktion::TimeRange clipRange {
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat)),
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat + lengthBeats))
        };
        const te::ClipPosition clipPos { clipRange, tracktion::TimeDuration::fromSeconds (0.0) };

        edit->getUndoManager().beginNewTransaction ("MCP Import Audio Stem");
        tracktion::EditItemID createdId;
        {
            if (auto waveClip = tracks[trackIdx]->insertWaveClip (clipName, wavFile, clipPos, false))
            {
                waveClip->getSourceFileReference().setToFile (
                    wavFile, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
                waveClip->setAutoTempo (autoTempo);
                waveClip->setPitchChange (pitchShift);
                createdId = waveClip->itemID;
            }
        }

        if (! createdId.isValid())
            return makeErrorResponse (id, -32603, "Failed to insert WaveAudioClip");

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP imported stem: " + clipName);

        auto* res = new juce::DynamicObject();
        res->setProperty ("track_index", trackIdx);
        res->setProperty ("clip_id", static_cast<juce::int64> (createdId.getRawID()));
        res->setProperty ("clip_name", clipName);
        return makeResultResponse (id, juce::var (res));
    }

    // 12. audio.export_wav (master stereo mixdown via Tracktion Renderer)
    if (method == "audio.export_wav")
    {
        const juce::String rawOut = params.getProperty ("output_path", "").toString().trim();
        if (rawOut.isEmpty())
            return makeErrorResponse (id, -32602, "Missing output_path");

        const juce::File outFile = juce::File::isAbsolutePath (rawOut)
                                       ? juce::File (rawOut)
                                       : juce::File::getCurrentWorkingDirectory().getChildFile (rawOut);

        if (outFile == juce::File() || outFile.isDirectory())
            return makeErrorResponse (id, -32602, "Invalid output_path");

        auto range = MixdownExporter::getDefaultExportRange (*edit);
        if (params.hasProperty ("start_beat") || params.hasProperty ("end_beat"))
        {
            const double startB = std::max (0.0, static_cast<double> (params.getProperty ("start_beat", 0.0)));
            const double endB   = std::max (startB + 1.0, static_cast<double> (params.getProperty ("end_beat", startB + 8.0)));
            range = tracktion::TimeRange {
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startB)),
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endB))
            };
        }

        if (! MixdownExporter::exportEditToWav (*edit, outFile, range))
            return makeErrorResponse (id, -32603, "Failed to render master WAV");

        auto* res = new juce::DynamicObject();
        res->setProperty ("output_path", outFile.getFullPathName());
        res->setProperty ("file_size_bytes", outFile.getSize());
        res->setProperty ("duration_seconds", range.getLength().inSeconds());
        return makeResultResponse (id, juce::var (res));
    }

    // 13. mixer.set_eq (insert or update 3-band EQ on a track)
    if (method == "mixer.set_eq")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        auto* track = tracks[trackIdx];
        edit->getUndoManager().beginNewTransaction ("MCP Set Track EQ");

        auto* eq = track->getEqualiserPlugin();

        if (eq == nullptr)
        {
            if (auto eqPlugin = edit->getPluginCache().createNewPlugin (te::EqualiserPlugin::xmlTypeName, {}))
                track->pluginList.insertPlugin (eqPlugin, getInsertIndexBeforeTerminalPlugins (*track), nullptr);

            eq = track->getEqualiserPlugin();
        }

        if (eq == nullptr)
            return makeErrorResponse (id, -32603, "Failed to insert EqualiserPlugin");

        if (params.hasProperty ("low_db"))
        {
            eq->loGainValue = juce::jlimit (-18.0f, 18.0f, static_cast<float> (params["low_db"]));
            if (eq->loGain != nullptr) eq->loGain->updateFromAttachedValue();
        }

        if (params.hasProperty ("mid_db"))
        {
            eq->midGainValue1 = juce::jlimit (-18.0f, 18.0f, static_cast<float> (params["mid_db"]));
            if (eq->midGain1 != nullptr) eq->midGain1->updateFromAttachedValue();
        }

        if (params.hasProperty ("high_db"))
        {
            eq->hiGainValue = juce::jlimit (-18.0f, 18.0f, static_cast<float> (params["high_db"]));
            if (eq->hiGain != nullptr) eq->hiGain->updateFromAttachedValue();
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP updated 3-band EQ on track " + juce::String (trackIdx + 1));

        auto* res = new juce::DynamicObject();
        res->setProperty ("track_index", trackIdx);
        res->setProperty ("has_eq", true);
        res->setProperty ("low_db",  eq->loGainValue.get());
        res->setProperty ("mid_db",  eq->midGainValue1.get());
        res->setProperty ("high_db", eq->hiGainValue.get());
        return makeResultResponse (id, juce::var (res));
    }

    // 14. recording.list_inputs (read-only enumeration of audio/MIDI inputs)
    if (method == "recording.list_inputs")
    {
        juce::Array<juce::var> audioIns, midiIns;

        for (const auto& name : recordingManager.getAvailableAudioInputNames())
            audioIns.add (name);

        for (const auto& name : recordingManager.getAvailableMidiInputNames())
            midiIns.add (name);

        auto* res = new juce::DynamicObject();
        res->setProperty ("audio_inputs", audioIns);
        res->setProperty ("midi_inputs", midiIns);
        return makeResultResponse (id, juce::var (res));
    }

    // 15. automation.set_points (write track automation curve points)
    if (method == "automation.set_points")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        const juce::String paramName = params.getProperty ("parameter", "volume").toString().trim();
        const auto* ptsArray = params.getProperty ("points", juce::var()).getArray();

        if (ptsArray == nullptr)
            return makeErrorResponse (id, -32602, "Missing points array");

        auto* track = tracks[trackIdx];
        const juce::String lowerParam = paramName.toLowerCase();

        if ((lowerParam == "eq_low" || lowerParam == "eq_mid" || lowerParam == "eq_high")
            && track->getEqualiserPlugin() == nullptr)
        {
            edit->getUndoManager().beginNewTransaction ("Edit Track Automation");
            if (auto eqPlugin = edit->getPluginCache().createNewPlugin (te::EqualiserPlugin::xmlTypeName, {}))
                track->pluginList.insertPlugin (eqPlugin, getInsertIndexBeforeTerminalPlugins (*track), nullptr);
        }

        std::vector<AutomationPointSpec> specs;
        specs.reserve (static_cast<size_t> (ptsArray->size()));

        for (const auto& item : *ptsArray)
        {
            AutomationPointSpec pt;
            pt.beat  = std::max (0.0, static_cast<double> (item.getProperty ("beat", 0.0)));
            pt.value = static_cast<float> (static_cast<double> (item.getProperty ("value", 0.0)));
            pt.curve = juce::jlimit (-1.0f, 1.0f,
                                     static_cast<float> (static_cast<double> (item.getProperty ("curve", 0.0))));
            specs.push_back (pt);
        }

        const bool clearExisting = static_cast<bool> (params.getProperty ("clear_existing", true));

        if (! AutomationManager::setAutomationPoints (*edit, *track, paramName, specs, clearExisting))
            return makeErrorResponse (id, -32602, "Unsupported or unavailable automation parameter: " + paramName);

        const auto updated = AutomationManager::getAutomationPoints (*edit, *track, paramName);

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP edited " + paramName + " automation on track " + juce::String (trackIdx + 1));

        auto* res = new juce::DynamicObject();
        res->setProperty ("track_index", trackIdx);
        res->setProperty ("parameter", lowerParam);
        res->setProperty ("point_count", static_cast<int> (updated.size()));
        return makeResultResponse (id, juce::var (res));
    }

    // 16. automation.get_points (read-only)
    if (method == "automation.get_points")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        const juce::String paramName = params.getProperty ("parameter", "volume").toString().trim();

        if (AutomationManager::resolveTrackParameter (*tracks[trackIdx], paramName) == nullptr)
            return makeErrorResponse (id, -32602, "Unsupported or unavailable automation parameter: " + paramName);

        const auto pts = AutomationManager::getAutomationPoints (*edit, *tracks[trackIdx], paramName);
        juce::Array<juce::var> outArr;

        for (const auto& pt : pts)
        {
            auto* pObj = new juce::DynamicObject();
            pObj->setProperty ("beat", pt.beat);
            pObj->setProperty ("value", pt.value);
            pObj->setProperty ("curve", pt.curve);
            outArr.add (juce::var (pObj));
        }

        auto* res = new juce::DynamicObject();
        res->setProperty ("track_index", trackIdx);
        res->setProperty ("parameter", paramName.toLowerCase());
        res->setProperty ("points", outArr);
        return makeResultResponse (id, juce::var (res));
    }

    // 17. mixer.set_fx_send (configure Aux Send + shared FX Return Reverb/Delay bus)
    if (method == "mixer.set_fx_send")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        const int busIdx       = juce::jlimit (0, 7, static_cast<int> (params.getProperty ("bus_index", 0)));
        const float sendDb     = static_cast<float> (static_cast<double> (params.getProperty ("send_db", -6.0)));
        const juce::String fx  = params.getProperty ("effect", "reverb").toString().trim().toLowerCase();
        const float wetLevel   = static_cast<float> (static_cast<double> (params.getProperty ("wet_level", 0.35)));

        const auto fxRes = AutomationManager::configureFxSend (
            *edit, *tracks[trackIdx], busIdx, sendDb, fx, wetLevel);

        if (! fxRes.success)
            return makeErrorResponse (id, -32603, "Failed to configure Aux FX send");

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP routed track " + juce::String (trackIdx + 1) + " to FX bus " + juce::String (busIdx + 1));

        auto* res = new juce::DynamicObject();
        res->setProperty ("track_index", trackIdx);
        res->setProperty ("bus_index", fxRes.busIndex);
        res->setProperty ("return_track_index", fxRes.returnTrackIndex);
        res->setProperty ("send_db", fxRes.sendDb);
        res->setProperty ("effect", fx == "delay" ? "delay" : "reverb");
        res->setProperty ("wet_level", fxRes.wetLevel);
        return makeResultResponse (id, juce::var (res));
    }

    // 18. audio.transcribe_to_midi (YIN monophonic pitch tracking -> MidiClip)
    if (method == "audio.transcribe_to_midi")
    {
        const int trackIdx       = static_cast<int> (params.getProperty ("track_index", 0));
        const int clipIdx        = static_cast<int> (params.getProperty ("clip_index", 0));
        const int targetTrackIdx = static_cast<int> (params.getProperty ("target_track_index", -1));

        auto tracks = te::getAudioTracks (*edit);

        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        tracktion::EditItemID waveClipID;
        int waveIdx = 0;

        for (auto* c : tracks[trackIdx]->getClips())
        {
            if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
            {
                if (waveIdx == clipIdx)
                {
                    waveClipID = wc->itemID;
                    break;
                }

                ++waveIdx;
            }
        }

        if (! waveClipID.isValid())
            return makeErrorResponse (id, -32602, "No WaveAudioClip found at track_index/clip_index");

        TranscriptionOptions opts;

        if (params.hasProperty ("quantize_beats"))
            opts.quantizeBeats = std::clamp (static_cast<double> (params["quantize_beats"]), 0.0, 2.0);

        if (params.hasProperty ("min_note_beats"))
            opts.minNoteBeats = std::clamp (static_cast<double> (params["min_note_beats"]), 0.03125, 2.0);

        const auto res = AudioToMidiTranscriber::transcribeClipToMidi (*edit, waveClipID, targetTrackIdx, opts);

        if (! res.success)
            return makeErrorResponse (id, -32603, res.errorMessage);

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP transcribed " + juce::String (res.notes.size()) + " MIDI notes");

        juce::Array<juce::var> notesOut;

        for (const auto& n : res.notes)
        {
            auto* nObj = new juce::DynamicObject();
            nObj->setProperty ("pitch", n.pitch);
            nObj->setProperty ("start_beat", n.startBeat);
            nObj->setProperty ("length_beats", n.lengthBeats);
            nObj->setProperty ("velocity", n.velocity);
            notesOut.add (juce::var (nObj));
        }

        auto* out = new juce::DynamicObject();
        out->setProperty ("target_track_index", res.targetTrackIndex);
        out->setProperty ("clip_name", res.clipName);
        out->setProperty ("notes_transcribed", static_cast<int> (res.notes.size()));
        out->setProperty ("notes", notesOut);
        return makeResultResponse (id, juce::var (out));
    }

    // 19. plugin.set_preset (list or apply factory presets on non-terminal track plugins)
    if (method == "plugin.set_preset")
    {
        const int trackIdx  = static_cast<int> (params.getProperty ("track_index", 0));
        const int pluginIdx = static_cast<int> (params.getProperty ("plugin_index", 0));
        const juce::String presetName = params.getProperty ("preset_name", "").toString().trim();

        auto tracks = te::getAudioTracks (*edit);
        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        if (pluginIdx < 0)
            return makeErrorResponse (id, -32602, "Invalid plugin_index");

        juce::Array<te::Plugin*> nonTerminal;
        for (auto* p : tracks[trackIdx]->pluginList)
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*> (p) == nullptr
                && dynamic_cast<te::LevelMeterPlugin*> (p) == nullptr)
            {
                nonTerminal.add (p);
            }
        }

        if (! juce::isPositiveAndBelow (pluginIdx, nonTerminal.size()))
            return makeErrorResponse (id, -32602, "No plugin at track_index/plugin_index");

        auto* targetPlugin = nonTerminal.getUnchecked (pluginIdx);
        const auto available = PluginPresetManager::getFactoryPresetNames (*targetPlugin);

        juce::Array<juce::var> availArr;
        for (const auto& name : available)
            availArr.add (name);

        if (presetName.isNotEmpty())
        {
            if (! PluginPresetManager::applyFactoryPreset (*targetPlugin, presetName))
                return makeErrorResponse (id, -32602, "Unknown preset_name for plugin: " + presetName);

            if (onEditMutatedByRpc)
                onEditMutatedByRpc ("MCP applied preset " + presetName + " to " + targetPlugin->getName());
        }

        auto* out = new juce::DynamicObject();
        out->setProperty ("track_index", trackIdx);
        out->setProperty ("plugin_index", pluginIdx);
        out->setProperty ("plugin_name", targetPlugin->getName());
        out->setProperty ("applied_preset", presetName);
        out->setProperty ("available_presets", availArr);
        return makeResultResponse (id, juce::var (out));
    }

    // 20. rack.add_machine (insert instrument, effect, or CV utility into a modular rack)
    if (method == "rack.add_machine")
    {
        const int rackIdx = static_cast<int> (params.getProperty ("rack_index", 0));
        const juce::String rawType = params.getProperty ("machine_type", "4osc").toString().trim().toLowerCase();

        if (rackIdx < 0)
            return makeErrorResponse (id, -32602, "Invalid rack_index");

        if (edit->getRackList().getTypes().isEmpty() && rackIdx == 0)
        {
            if (auto newRack = edit->getRackList().addNewRack())
                newRack->rackName = "Modular Studio Rack";
        }

        const auto& racks = edit->getRackList().getTypes();
        if (! juce::isPositiveAndBelow (rackIdx, racks.size()))
            return makeErrorResponse (id, -32602, "Invalid rack_index");

        juce::String xmlType;
        if (rawType == "4osc") xmlType = te::FourOscPlugin::xmlTypeName;
        else if (rawType == "sampler") xmlType = te::SamplerPlugin::xmlTypeName;
        else if (rawType == "compressor") xmlType = te::CompressorPlugin::xmlTypeName;
        else if (rawType == "chorus") xmlType = te::ChorusPlugin::xmlTypeName;
        else if (rawType == "phaser") xmlType = te::PhaserPlugin::xmlTypeName;
        else if (rawType == "lowpass") xmlType = te::LowPassPlugin::xmlTypeName;
        else if (rawType == "pitchshift") xmlType = te::PitchShiftPlugin::xmlTypeName;
        else if (rawType == "reverb") xmlType = te::ReverbPlugin::xmlTypeName;
        else if (rawType == "delay") xmlType = te::DelayPlugin::xmlTypeName;
        else if (rawType == "eq" || rawType == "equaliser") xmlType = te::EqualiserPlugin::xmlTypeName;
        else if (rawType == "tr808") xmlType = Tr808DrumPlugin::xmlTypeName;
        else if (rawType == "vintage_sampler") xmlType = VintageSamplerPlugin::xmlTypeName;
        else if (rawType == "tape") xmlType = AnalogTapePlugin::xmlTypeName;
        else if (rawType == "cv_lfo") xmlType = CvLfoPlugin::xmlTypeName;
        else if (rawType == "cv_wobble") xmlType = CvWobbleFilterPlugin::xmlTypeName;
        else if (rawType == "cv_splitter") xmlType = CvUtilityPlugin::xmlTypeName;
        else
            return makeErrorResponse (id, -32602, "Unsupported machine_type: " + rawType);

        auto* rack = racks[rackIdx];
        edit->getUndoManager().beginNewTransaction ("MCP Add Rack Machine");

        auto plugin = edit->getPluginCache().createNewPlugin (xmlType, {});
        if (plugin == nullptr)
            return makeErrorResponse (id, -32603, "Failed to instantiate plugin: " + xmlType);

        if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (plugin.get()))
        {
            const auto kitDir = getTakesDirectoryCallback
                                    ? getTakesDirectoryCallback().getChildFile ("SamplerKit")
                                    : PluginPresetManager::getDefaultSamplerKitDirectory();
            PluginPresetManager::seedStarterSamplerKit (*sampler, kitDir);
        }

        const float count = static_cast<float> (rack->getPlugins().size());
        const float x = juce::jlimit (0.05f, 0.78f, 0.08f + 0.14f * std::fmod (count, 4.0f));
        const float y = juce::jlimit (0.08f, 0.72f, 0.12f + 0.22f * std::floor (count / 4.0f));

        if (! rack->addPlugin (plugin, { x, y }, false))
            return makeErrorResponse (id, -32603, "Failed to add machine into rack");

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP added " + plugin->getName() + " to rack " + juce::String (rackIdx));

        juce::Array<juce::var> availPresets;
        for (const auto& presetName : PluginPresetManager::getFactoryPresetNames (*plugin))
            availPresets.add (presetName);

        auto* out = new juce::DynamicObject();
        out->setProperty ("rack_index", rackIdx);
        out->setProperty ("plugin_id", static_cast<juce::int64> (plugin->itemID.getRawID()));
        out->setProperty ("plugin_name", plugin->getName());
        out->setProperty ("machine_type", rawType);
        out->setProperty ("available_presets", availPresets);
        return makeResultResponse (id, juce::var (out));
    }

    // 21. rack.add_module (Reason-style CV / hardware module into a modular rack)
    if (method == "rack.add_module")
    {
        const int rackIdx = static_cast<int> (params.getProperty ("rack_index", 0));
        const juce::String rawType = params.getProperty ("module_type", "cv_lfo").toString().trim().toLowerCase();

        if (rackIdx < 0)
            return makeErrorResponse (id, -32602, "Invalid rack_index");

        if (edit->getRackList().getTypes().isEmpty() && rackIdx == 0)
        {
            if (auto newRack = edit->getRackList().addNewRack())
                newRack->rackName = "Modular Studio Rack";
        }

        const auto& racks = edit->getRackList().getTypes();

        if (! juce::isPositiveAndBelow (rackIdx, racks.size()))
            return makeErrorResponse (id, -32602, "Invalid rack_index");

        juce::String xmlType;

        if (rawType == "cv_lfo")                        xmlType = CvLfoPlugin::xmlTypeName;
        else if (rawType == "cv_filter" || rawType == "cv_wobble") xmlType = CvWobbleFilterPlugin::xmlTypeName;
        else if (rawType == "cv_splitter")               xmlType = CvUtilityPlugin::xmlTypeName;
        else if (rawType == "tr808")                     xmlType = Tr808DrumPlugin::xmlTypeName;
        else if (rawType == "sampler")                   xmlType = VintageSamplerPlugin::xmlTypeName;
        else if (rawType == "tape")                      xmlType = AnalogTapePlugin::xmlTypeName;
        else if (rawType == "4osc")                      xmlType = te::FourOscPlugin::xmlTypeName;
        else
            return makeErrorResponse (id, -32602, "Unsupported module_type: " + rawType);

        auto* rack = racks[rackIdx];
        edit->getUndoManager().beginNewTransaction ("MCP Add Rack Module");

        auto plugin = edit->getPluginCache().createNewPlugin (xmlType, {});

        if (plugin == nullptr)
            return makeErrorResponse (id, -32603, "Failed to instantiate module: " + xmlType);

        const float x = juce::jlimit (0.02f, 0.92f, static_cast<float> (params.getProperty ("x", 0.25)));
        const float y = juce::jlimit (0.02f, 0.92f, static_cast<float> (params.getProperty ("y", 0.5)));

        if (! rack->addPlugin (plugin, { x, y }, false))
            return makeErrorResponse (id, -32603, "Failed to add module into rack");

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP added module " + plugin->getName() + " to rack " + juce::String (rackIdx));

        juce::StringArray ins, outs;
        plugin->getChannelNames (&ins, &outs);
        juce::Array<juce::var> inputs, outputs;

        for (const auto& name : ins)
            inputs.add (name);

        for (const auto& name : outs)
            outputs.add (name);

        auto* out = new juce::DynamicObject();
        out->setProperty ("rack_index", rackIdx);
        out->setProperty ("plugin_id", static_cast<juce::int64> (plugin->itemID.getRawID()));
        out->setProperty ("name", plugin->getName());
        out->setProperty ("inputs", inputs);
        out->setProperty ("outputs", outputs);
        return makeResultResponse (id, juce::var (out));
    }

    // 22. midi.generate_progression (Roman-numeral voice-led chord generator)
    if (method == "midi.generate_progression")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        auto tracks = te::getAudioTracks (*edit);
        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        const juce::String keyName   = params.getProperty ("key", "C").toString().trim();
        const juce::String scaleName = params.getProperty ("scale", "minor").toString().trim().toLowerCase();
        const auto scale = getScaleIntervals (scaleName, false);
        if (scale.size() < 7)
            return makeErrorResponse (id, -32602, "Unsupported scale for chord generation: " + scaleName);

        const int keyPc = RiffLibrary::parseKeyPitchClass (keyName);
        const double startBeat     = std::max (0.0, static_cast<double> (params.getProperty ("start_beat", 0.0)));
        const double beatsPerChord = std::clamp (static_cast<double> (params.getProperty ("beats_per_chord", 4.0)), 0.25, 32.0);
        const int octave           = juce::jlimit (1, 7, static_cast<int> (params.getProperty ("octave", 4)));
        const int velocity         = juce::jlimit (1, 127, static_cast<int> (params.getProperty ("velocity", 96)));
        const bool voiceLead       = static_cast<bool> (params.getProperty ("voice_lead", true));

        juce::StringArray tokens;
        if (const auto* arr = params.getProperty ("progression", juce::var()).getArray())
        {
            for (const auto& item : *arr)
            {
                const auto tok = item.toString().trim();
                if (tok.isNotEmpty())
                    tokens.add (tok);
            }
        }
        if (tokens.isEmpty())
            tokens = juce::StringArray { "i", "VI", "III", "VII" };

        std::vector<std::vector<int>> chordPcsList;
        chordPcsList.reserve (static_cast<size_t> (tokens.size()));
        for (const auto& tok : tokens)
        {
            std::vector<int> pcs;
            if (! parseRomanChordToken (tok, keyPc, scale, pcs))
                return makeErrorResponse (id, -32602, "Invalid Roman chord token: " + tok);
            chordPcsList.push_back (std::move (pcs));
        }

        const double endBeat = startBeat + beatsPerChord * static_cast<double> (chordPcsList.size());
        const tracktion::TimeRange targetRange {
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat)),
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endBeat))
        };

        auto* track = tracks[trackIdx];
        auto& um = edit->getUndoManager();
        um.beginNewTransaction ("MCP Generate Chord Progression");

        te::MidiClip* midiClip = nullptr;
        for (auto* c : track->getClips())
        {
            if (auto* mc = dynamic_cast<te::MidiClip*> (c))
            {
                const double cStart = edit->tempoSequence.toBeats (mc->getPosition().getStart()).inBeats();
                const double cEnd   = edit->tempoSequence.toBeats (mc->getPosition().getEnd()).inBeats();
                if (cEnd > startBeat - 1e-4 && cStart <= startBeat + 1e-4)
                {
                    midiClip = mc;
                    break;
                }
            }
        }

        if (midiClip == nullptr)
        {
            midiClip = dynamic_cast<te::MidiClip*> (
                track->insertNewClip (te::TrackItem::Type::midi, "AI Chords", targetRange, nullptr));
        }
        else if (midiClip->getPosition().getEnd() < targetRange.getEnd())
        {
            midiClip->setEnd (targetRange.getEnd(), true);
        }

        if (midiClip == nullptr)
            return makeErrorResponse (id, -32603, "Failed to create or resolve MidiClip");

        const double clipOriginBeat = edit->tempoSequence.toBeats (midiClip->getPosition().getStart()).inBeats();
        auto& seq = midiClip->getSequence();
        const int baseOctaveMidi = (octave + 1) * 12;

        std::vector<int> prevVoicing;
        int notesWritten = 0;

        for (size_t cIdx = 0; cIdx < chordPcsList.size(); ++cIdx)
        {
            const auto voiced = buildVoicedChord (chordPcsList[cIdx], baseOctaveMidi, prevVoicing, voiceLead);
            const double localStartBeat = std::max (0.0, (startBeat + static_cast<double> (cIdx) * beatsPerChord) - clipOriginBeat);
            const double noteLenBeats = std::max (0.125, beatsPerChord * 0.95);

            for (int pitch : voiced)
            {
                seq.addNote (pitch,
                             tracktion::BeatPosition::fromBeats (localStartBeat),
                             tracktion::BeatDuration::fromBeats (noteLenBeats),
                             velocity,
                             0,
                             &um);
                ++notesWritten;
            }

            prevVoicing = voiced;
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP generated " + juce::String (chordPcsList.size()) + " chords on track " + juce::String (trackIdx + 1));

        auto* out = new juce::DynamicObject();
        out->setProperty ("track_index", trackIdx);
        out->setProperty ("chords_written", static_cast<int> (chordPcsList.size()));
        out->setProperty ("notes_written", notesWritten);
        out->setProperty ("start_beat", startBeat);
        out->setProperty ("end_beat", endBeat);
        return makeResultResponse (id, juce::var (out));
    }

    // 23. midi.quantize_scale (snap MIDI pitch to key/scale and timing to beat grid)
    if (method == "midi.quantize_scale")
    {
        const int trackIdx = static_cast<int> (params.getProperty ("track_index", 0));
        auto tracks = te::getAudioTracks (*edit);
        if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
            return makeErrorResponse (id, -32602, "Invalid track_index");

        const juce::String keyName   = params.getProperty ("key", "C").toString().trim();
        const juce::String scaleName = params.getProperty ("scale", "minor").toString().trim().toLowerCase();
        const auto scale = getScaleIntervals (scaleName, true);
        if (scale.empty())
            return makeErrorResponse (id, -32602, "Unsupported scale: " + scaleName);

        const int keyPc        = RiffLibrary::parseKeyPitchClass (keyName);
        const double gridBeats = std::clamp (static_cast<double> (params.getProperty ("grid_beats", 0.25)), 0.03125, 4.0);
        const bool snapPitch   = static_cast<bool> (params.getProperty ("snap_pitch", true));
        const bool snapTiming  = static_cast<bool> (params.getProperty ("snap_timing", true));

        auto* track = tracks[trackIdx];
        auto& um = edit->getUndoManager();
        um.beginNewTransaction ("MCP Scale & Timing Quantize");

        int notesChecked = 0;
        int pitchSnapped = 0;
        int timingSnapped = 0;

        for (auto* c : track->getClips())
        {
            auto* mc = dynamic_cast<te::MidiClip*> (c);
            if (mc == nullptr)
                continue;

            const double clipStartBeat = edit->tempoSequence.toBeats (mc->getPosition().getStart()).inBeats();
            const double clipEndBeat   = edit->tempoSequence.toBeats (mc->getPosition().getEnd()).inBeats();
            double maxNeededEndBeat = clipEndBeat;

            juce::Array<te::MidiNote*> notesCopy;
            for (auto* n : mc->getSequence().getNotes())
                notesCopy.add (n);

            for (auto* note : notesCopy)
            {
                if (note == nullptr)
                    continue;

                ++notesChecked;

                if (snapPitch)
                {
                    const int origPitch = note->getNoteNumber();
                    const int snapped   = snapMidiPitchToScale (origPitch, keyPc, scale);
                    if (snapped != origPitch)
                    {
                        note->setNoteNumber (snapped, &um);
                        ++pitchSnapped;
                    }
                }

                if (snapTiming)
                {
                    const double origLocal = note->getStartBeat().inBeats();
                    const double absBeat   = clipStartBeat + origLocal;
                    const double quantAbs  = std::max (clipStartBeat, std::round (absBeat / gridBeats) * gridBeats);
                    const double newLocal  = std::max (0.0, quantAbs - clipStartBeat);
                    const double lenBeats  = std::max (0.03125, note->getLengthBeats().inBeats());

                    if (std::abs (newLocal - origLocal) > 1e-4)
                    {
                        note->setStartAndLength (tracktion::BeatPosition::fromBeats (newLocal),
                                                 tracktion::BeatDuration::fromBeats (lenBeats),
                                                 &um);
                        ++timingSnapped;
                        maxNeededEndBeat = std::max (maxNeededEndBeat, clipStartBeat + newLocal + lenBeats);
                    }
                }
            }

            if (maxNeededEndBeat > clipEndBeat + 1e-4)
                mc->setEnd (edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (maxNeededEndBeat)), true);
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP quantized " + juce::String (notesChecked) + " MIDI notes to " + keyName + " " + scaleName);

        auto* out = new juce::DynamicObject();
        out->setProperty ("track_index", trackIdx);
        out->setProperty ("notes_checked", notesChecked);
        out->setProperty ("pitch_snapped", pitchSnapped);
        out->setProperty ("timing_snapped", timingSnapped);
        return makeResultResponse (id, juce::var (out));
    }

    // 24. mixer.auto_balance_lufs (gain-match non-muted tracks toward target_lufs)
    if (method == "mixer.auto_balance_lufs")
    {
        const float targetLufs = juce::jlimit (-40.0f, -3.0f, static_cast<float> (params.getProperty ("target_lufs", -14.0)));
        const float minGainDb  = juce::jlimit (-60.0f, 6.0f, static_cast<float> (params.getProperty ("min_gain_db", -18.0)));
        const float maxGainDb  = juce::jlimit (minGainDb, 6.0f, static_cast<float> (params.getProperty ("max_gain_db", 6.0)));
        const float headroomDb = juce::jlimit (-12.0f, 0.0f, static_cast<float> (params.getProperty ("headroom_db", -1.0)));

        auto tracks = te::getAudioTracks (*edit);
        auto& um = edit->getUndoManager();
        um.beginNewTransaction ("MCP Auto-Balance LUFS");

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        juce::Array<juce::var> adjustedArr;

        for (int i = 0; i < tracks.size(); ++i)
        {
            auto* track = tracks[i];
            if (track == nullptr || track->isMuted (false))
                continue;

            auto* vol = track->getVolumePlugin();
            if (vol == nullptr)
                continue;

            float measuredRmsDb  = -18.0f;
            float measuredPeakDb = -6.0f;
            bool measuredFromAudio = false;

            double sumSquares = 0.0;
            juce::int64 totalFrames = 0;
            float maxAbsPeak = 0.0f;

            for (auto* c : track->getClips())
            {
                auto* wc = dynamic_cast<te::WaveAudioClip*> (c);
                if (wc == nullptr)
                    continue;

                const auto wavFile = wc->getSourceFileReference().getFile();
                if (! wavFile.existsAsFile())
                    continue;

                if (auto reader = std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (wavFile)))
                {
                    if (reader->lengthInSamples > 0 && reader->numChannels > 0)
                    {
                        const int toRead = static_cast<int> (std::min<juce::int64> (65536, reader->lengthInSamples));
                        const int numCh  = std::min (2, static_cast<int> (reader->numChannels));
                        juce::AudioBuffer<float> tmp (numCh, toRead);
                        if (reader->read (&tmp, 0, toRead, 0, true, numCh > 1))
                        {
                            const float clipGain = wc->getGain();
                            for (int ch = 0; ch < numCh; ++ch)
                            {
                                const float* data = tmp.getReadPointer (ch);
                                for (int s = 0; s < toRead; ++s)
                                {
                                    const float v = data[s] * clipGain;
                                    sumSquares += static_cast<double> (v) * static_cast<double> (v);
                                    maxAbsPeak = std::max (maxAbsPeak, std::abs (v));
                                }
                            }
                            totalFrames += static_cast<juce::int64> (toRead) * numCh;
                        }
                    }
                }
            }

            if (totalFrames > 0)
            {
                const float rms = static_cast<float> (std::sqrt (sumSquares / static_cast<double> (totalFrames)));
                measuredRmsDb  = juce::jlimit (-60.0f, 0.0f, juce::Decibels::gainToDecibels (std::max (1.0e-4f, rms)) - 0.691f);
                measuredPeakDb = juce::jlimit (-60.0f, 0.0f, juce::Decibels::gainToDecibels (std::max (1.0e-4f, maxAbsPeak)));
                measuredFromAudio = true;
            }
            else if (auto* meter = track->pluginList.findFirstPluginOfType<te::LevelMeterPlugin>())
            {
                te::LevelMeasurer::Client tmpClient;
                meter->measurer.addClient (tmpClient);
                const float lDb = tmpClient.getAndClearAudioLevel (0).dB;
                const float rDb = tmpClient.getAndClearAudioLevel (1).dB;
                meter->measurer.removeClient (tmpClient);
                const float activeDb = std::max (lDb, rDb);
                if (std::isfinite (activeDb) && activeDb > -55.0f)
                {
                    measuredRmsDb  = juce::jlimit (-60.0f, 0.0f, activeDb - 0.691f);
                    measuredPeakDb = juce::jlimit (-60.0f, 0.0f, activeDb);
                    measuredFromAudio = true;
                }
            }

            if (! measuredFromAudio)
            {
                // MIDI/synth velocity density proxy when transport is idle
                double velSum = 0.0;
                int noteCnt = 0;
                for (auto* c : track->getClips())
                {
                    if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                    {
                        for (auto* n : mc->getSequence().getNotes())
                        {
                            velSum += static_cast<double> (n->getVelocity()) / 127.0;
                            ++noteCnt;
                        }
                    }
                }

                if (noteCnt > 0)
                {
                    const float avgVel = juce::jlimit (0.1f, 1.0f, static_cast<float> (velSum / noteCnt));
                    measuredRmsDb  = juce::jlimit (-36.0f, -6.0f, -20.0f + 12.0f * avgVel);
                    measuredPeakDb = measuredRmsDb + 6.0f;
                }
            }

            const float curVolDb     = std::isfinite (vol->getVolumeDb()) ? vol->getVolumeDb() : 0.0f;
            const float desiredGain  = targetLufs - measuredRmsDb;
            const float peakCapGain  = headroomDb - measuredPeakDb;
            const float safeApplied  = juce::jlimit (minGainDb, maxGainDb, std::min (desiredGain, peakCapGain));
            juce::ignoreUnused (curVolDb);

            vol->setVolumeDb (safeApplied);

            auto* tObj = new juce::DynamicObject();
            tObj->setProperty ("track_index", i);
            tObj->setProperty ("name", track->getName());
            tObj->setProperty ("measured_db", measuredRmsDb);
            tObj->setProperty ("applied_db", safeApplied);
            adjustedArr.add (juce::var (tObj));
        }

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP auto-balanced " + juce::String (adjustedArr.size()) + " tracks toward " + juce::String (targetLufs, 1) + " LUFS");

        auto* out = new juce::DynamicObject();
        out->setProperty ("target_lufs", targetLufs);
        out->setProperty ("tracks_adjusted", adjustedArr);
        return makeResultResponse (id, juce::var (out));
    }

    // 25. arrangement.duplicate_section (copy trimmed MIDI & audio regions to dest_beat)
    if (method == "arrangement.duplicate_section")
    {
        const double startBeat = static_cast<double> (params.getProperty ("start_beat", 0.0));
        const double endBeat   = static_cast<double> (params.getProperty ("end_beat", 0.0));
        const double destBeat  = static_cast<double> (params.getProperty ("dest_beat", 0.0));
        const bool muteDrums   = static_cast<bool> (params.getProperty ("mute_drums", false));

        if (! std::isfinite (startBeat) || ! std::isfinite (endBeat) || ! std::isfinite (destBeat)
            || startBeat < 0.0 || endBeat <= startBeat || destBeat < 0.0)
        {
            return makeErrorResponse (id, -32602, "Invalid start_beat/end_beat/dest_beat range");
        }

        auto allTracks = te::getAudioTracks (*edit);
        std::vector<int> targetIndices;

        if (const auto* idxArr = params.getProperty ("track_indices", juce::var()).getArray())
        {
            for (const auto& item : *idxArr)
            {
                const int idx = static_cast<int> (item);
                if (! juce::isPositiveAndBelow (idx, allTracks.size()))
                    return makeErrorResponse (id, -32602, "Invalid track index in track_indices");
                if (std::find (targetIndices.begin(), targetIndices.end(), idx) == targetIndices.end())
                    targetIndices.push_back (idx);
            }
        }
        else
        {
            for (int i = 0; i < allTracks.size(); ++i)
                targetIndices.push_back (i);
        }

        struct CapturedNote
        {
            int pitch = 60;
            double startBeat = 0.0;
            double lengthBeats = 0.25;
            int velocity = 100;
            int colourIdx = 0;
        };

        struct CapturedClip
        {
            te::AudioTrack* track = nullptr;
            bool isMidi = true;
            juce::String name;
            double segStartBeat = 0.0;
            double segEndBeat = 0.0;
            double clipStartBeat = 0.0;
            std::vector<CapturedNote> notes;
            juce::File wavFile;
            double existingOffsetSec = 0.0;
            bool autoTempo = false;
            bool autoPitch = false;
            float pitchChange = 0.0f;
            float gainDb = 0.0f;
            double loopBeats = 0.0;
        };

        std::vector<CapturedClip> snapshots;

        for (int tIdx : targetIndices)
        {
            auto* track = allTracks[tIdx];
            if (track == nullptr)
                continue;

            if (muteDrums)
            {
                const auto lowerName = track->getName().toLowerCase();
                if (lowerName.contains ("drum") || lowerName.contains ("808")
                    || track->pluginList.findFirstPluginOfType<Tr808DrumPlugin>() != nullptr)
                {
                    continue;
                }
            }

            for (auto* c : track->getClips())
            {
                const double cStart = edit->tempoSequence.toBeats (c->getPosition().getStart()).inBeats();
                const double cEnd   = edit->tempoSequence.toBeats (c->getPosition().getEnd()).inBeats();
                const double segStart = std::max (startBeat, cStart);
                const double segEnd   = std::min (endBeat, cEnd);

                if (segEnd <= segStart + 1e-4)
                    continue;

                if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                {
                    CapturedClip snap;
                    snap.track = track;
                    snap.isMidi = true;
                    snap.name = mc->getName();
                    snap.segStartBeat = segStart;
                    snap.segEndBeat = segEnd;
                    snap.clipStartBeat = cStart;

                    for (auto* note : mc->getSequence().getNotes())
                    {
                        const double absNoteStart = cStart + note->getStartBeat().inBeats();
                        const double absNoteEnd   = absNoteStart + note->getLengthBeats().inBeats();
                        if (absNoteEnd <= segStart + 1e-4 || absNoteStart >= segEnd - 1e-4)
                            continue;

                        const double clampedStart = std::max (absNoteStart, segStart);
                        const double clampedEnd   = std::min (absNoteEnd, segEnd);
                        if (clampedEnd <= clampedStart + 1e-4)
                            continue;

                        CapturedNote cn;
                        cn.pitch = note->getNoteNumber();
                        cn.startBeat = clampedStart - segStart;
                        cn.lengthBeats = std::max (0.03125, clampedEnd - clampedStart);
                        cn.velocity = note->getVelocity();
                        cn.colourIdx = note->getColour();
                        snap.notes.push_back (cn);
                    }

                    snapshots.push_back (std::move (snap));
                }
                else if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                {
                    const auto file = wc->getSourceFileReference().getFile();
                    if (! file.existsAsFile())
                        continue;

                    CapturedClip snap;
                    snap.track = track;
                    snap.isMidi = false;
                    snap.name = wc->getName();
                    snap.segStartBeat = segStart;
                    snap.segEndBeat = segEnd;
                    snap.clipStartBeat = cStart;
                    snap.wavFile = file;
                    snap.existingOffsetSec = wc->getPosition().getOffset().inSeconds();
                    snap.autoTempo = wc->getAutoTempo();
                    snap.autoPitch = wc->getAutoPitch();
                    snap.pitchChange = wc->getPitchChange();
                    snap.gainDb = wc->getGainDB();
                    snap.loopBeats = wc->getLoopInfo().getNumBeats();
                    snapshots.push_back (std::move (snap));
                }
            }
        }

        auto& um = edit->getUndoManager();
        um.beginNewTransaction ("MCP Duplicate Arrangement Section");

        int clipsDuplicated = 0;

        for (const auto& snap : snapshots)
        {
            const double newStartBeat = destBeat + (snap.segStartBeat - startBeat);
            const double newEndBeat   = destBeat + (snap.segEndBeat - startBeat);
            const tracktion::TimeRange newRange {
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (newStartBeat)),
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (newEndBeat))
            };

            if (snap.isMidi)
            {
                if (auto* newMc = dynamic_cast<te::MidiClip*> (
                        snap.track->insertNewClip (te::TrackItem::Type::midi, snap.name, newRange, nullptr)))
                {
                    auto& seq = newMc->getSequence();
                    for (const auto& cn : snap.notes)
                    {
                        seq.addNote (cn.pitch,
                                     tracktion::BeatPosition::fromBeats (cn.startBeat),
                                     tracktion::BeatDuration::fromBeats (cn.lengthBeats),
                                     cn.velocity,
                                     cn.colourIdx,
                                     &um);
                    }
                    ++clipsDuplicated;
                }
            }
            else
            {
                const double trimDeltaSec = (snap.segStartBeat - snap.clipStartBeat)
                                          * (60.0 / std::max (40.0, edit->tempoSequence.getTempo (0)->getBpm()));
                const te::ClipPosition clipPos {
                    newRange,
                    tracktion::TimeDuration::fromSeconds (std::max (0.0, snap.existingOffsetSec + trimDeltaSec))
                };

                if (auto newWc = snap.track->insertWaveClip (snap.name, snap.wavFile, clipPos, false))
                {
                    newWc->getSourceFileReference().setToFile (
                        snap.wavFile, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
                    newWc->setAutoTempo (snap.autoTempo);
                    newWc->setAutoPitch (snap.autoPitch);
                    newWc->setPitchChange (snap.pitchChange);
                    newWc->setGainDB (snap.gainDb);
                    if (snap.loopBeats > 0.0)
                        newWc->getLoopInfo().setNumBeats (snap.loopBeats);
                    ++clipsDuplicated;
                }
            }
        }

        const double destEndBeat = destBeat + (endBeat - startBeat);

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP duplicated " + juce::String (clipsDuplicated) + " clips to beat " + juce::String (destBeat, 1));

        auto* out = new juce::DynamicObject();
        out->setProperty ("clips_duplicated", clipsDuplicated);
        out->setProperty ("dest_start_beat", destBeat);
        out->setProperty ("dest_end_beat", destEndBeat);
        return makeResultResponse (id, juce::var (out));
    }

    return makeErrorResponse (id, -32601, "Method not found: " + method);
}

juce::String DawIpcServer::makeResultResponse (const juce::var& id, const juce::var& result)
{
    auto* obj = new juce::DynamicObject();

    obj->setProperty ("jsonrpc", "2.0");
    obj->setProperty ("id", id);
    obj->setProperty ("result", result);

    return juce::JSON::toString (juce::var (obj), true);
}

juce::String DawIpcServer::makeErrorResponse (const juce::var& id, int code, const juce::String& msg)
{
    auto* err = new juce::DynamicObject();

    err->setProperty ("code", code);
    err->setProperty ("message", msg);

    auto* obj = new juce::DynamicObject();

    obj->setProperty ("jsonrpc", "2.0");
    obj->setProperty ("id", id);
    obj->setProperty ("error", juce::var (err));

    return juce::JSON::toString (juce::var (obj), true);
}
