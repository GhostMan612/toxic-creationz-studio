#include "DawIpcServer.h"

#include "MixdownExporter.h"
#include "WaveformEditorComponent.h"

#include <algorithm>
#include <cmath>

DawIpcServer::DawIpcServer (RiffLibrary& targetRiffLibrary)
    : juce::Thread ("DawIpcServer"),
      riffLibrary (targetRiffLibrary)
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

    boundPort.store (listenPort, std::memory_order_release);
    listening.store (true, std::memory_order_release);

    startThread();

    return true;
}

void DawIpcServer::stopListening()
{
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

    juce::MessageManager::callAsync ([this]
    {
        servicePendingRequestsOnMessageThread();
    });

    // Per-method wait: only a master mixdown renders synchronously on the
    // message thread, so only audio.export_wav gets the long budget. Every
    // other method keeps the 2.5 s budget, so a hung message thread fails
    // fast instead of blocking the reader thread (and the client) for 30 s.
    // The method name is read from the frame up front — the request itself
    // is still only dispatched on the message thread.
    int timeoutMs = 2500;

    {
        juce::var parsed;

        if (juce::JSON::parse (rawJson, parsed).wasOk()
            && parsed.getProperty ("method", "").toString() == "audio.export_wav")
        {
            timeoutMs = 30000;
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
    int idx = 0;

    for (auto* p : track.pluginList)
    {
        if (dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr
            || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr)
        {
            break;
        }

        ++idx;
    }

    return idx;
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
        root->setProperty ("position_seconds", posTime.inSeconds());
        root->setProperty ("position_beats", edit->tempoSequence.toBeats (posTime).inBeats());
        root->setProperty ("click_enabled", edit->clickTrackEnabled.get());
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
            tObj->setProperty ("clip_count", tracks[i]->getClips().size());

            if (auto* vol = tracks[i]->getVolumePlugin())
            {
                tObj->setProperty ("volume_db", vol->getVolumeDb());
                tObj->setProperty ("pan", vol->getPan());
            }

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

        if (pluginType == "4osc")
        {
            if (auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
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

        if (action == "play")
            edit->getTransport().play (false);
        else if (action == "stop")
            edit->getTransport().stop (false, false);

        if (onEditMutatedByRpc)
            onEditMutatedByRpc ("MCP transport update");

        auto* res = new juce::DynamicObject();
        const auto loopRange = edit->getTransport().getLoopRange();

        res->setProperty ("is_playing", edit->getTransport().isPlaying());
        res->setProperty ("bpm", edit->tempoSequence.getTempo (0)->getBpm());
        res->setProperty ("position_beats",
                          edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats());
        res->setProperty ("click_enabled", edit->clickTrackEnabled.get());
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
