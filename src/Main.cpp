#include "MainComponent.h"

#include <chrono>
#include <future>
#include <iostream>

//==============================================================================
// Headless regression harness (AI DAW.exe --smoke-test)
//==============================================================================
int MainComponent::runHeadlessSmokeTest()
{
    std::cout << "[SMOKE_TEST] Initializing Tracktion Engine..." << std::endl;

    // MainComponent owns the Engine and exercises the real session/scanner code.
    MainComponent app;

    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto sessionFile = tempDir.getChildFile ("aidaw_wo002_smoke.tracktionedit");
    sessionFile.deleteFile();

    //--- WO-001 regression: 4OSC + MIDI sequence round-trip ------------------
    app.createNewSession (sessionFile, true, false);
    app.saveSession();

    {
        auto reloaded = te::loadEditFromFile (app.engine, sessionFile);

        if (reloaded == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: loadEditFromFile returned nullptr" << std::endl;
            return 1;
        }

        auto tracks = te::getAudioTracks (*reloaded);

        if (tracks.size() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: reloaded track count mismatch, got " << tracks.size() << std::endl;
            return 1;
        }

        int totalNotes = 0;
        for (auto* c : tracks[0]->getClips())
            if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                totalNotes += mc->getSequence().getNumNotes();

        if (totalNotes != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected 4 reloaded MIDI notes, got " << totalNotes << std::endl;
            return 1;
        }
    }

    //--- WO-002 (1): createNewSession must not delete/modify an existing file -
    const auto savedFile = tempDir.getChildFile ("aidaw_wo002_saved.tracktionedit");
    savedFile.deleteFile();

    app.createNewSession (savedFile, true, false);
    app.saveSession();

    const auto savedSize = savedFile.getSize();
    const auto savedModified = savedFile.getLastModificationTime();

    const auto otherFile = tempDir.getChildFile ("aidaw_wo002_other.tracktionedit");
    otherFile.deleteFile();

    app.createNewSession (otherFile, false, false);

    if (! savedFile.existsAsFile()
        || savedFile.getSize() != savedSize
        || savedFile.getLastModificationTime() != savedModified)
    {
        std::cerr << "[SMOKE_TEST] FAIL: createNewSession modified an existing saved .tracktionedit" << std::endl;
        return 1;
    }

    //--- WO-002 (2): atomic load-then-swap open with rollback -----------------
    const int tracksBefore = static_cast<int> (te::getAudioTracks (*app.edit).size());
    const double bpmBefore = app.edit->tempoSequence.getTempo (0)->getBpm();

    if (app.loadSessionFromFileSafely (tempDir.getChildFile ("aidaw_wo002_missing.tracktionedit")))
    {
        std::cerr << "[SMOKE_TEST] FAIL: missing file unexpectedly loaded" << std::endl;
        return 1;
    }

    const auto corruptFile = tempDir.getChildFile ("aidaw_wo002_corrupt.tracktionedit");
    corruptFile.replaceWithText ("<NOTEDIT></NOTEDIT>");

    if (app.loadSessionFromFileSafely (corruptFile))
    {
        std::cerr << "[SMOKE_TEST] FAIL: corrupt file unexpectedly loaded" << std::endl;
        return 1;
    }

    // Truncated XML with the correct root tag: still rejected.
    // (A well-formed <EDIT> that fails inside loadEditFromFile would need the
    // Edit constructor itself to throw; no static file deterministically does
    // that, so the two rejected-at-the-gate cases above are the coverage.)
    const auto truncatedFile = tempDir.getChildFile ("aidaw_wo002_truncated.tracktionedit");
    truncatedFile.replaceWithText ("<EDIT><TRACK>");

    if (app.loadSessionFromFileSafely (truncatedFile))
    {
        std::cerr << "[SMOKE_TEST] FAIL: truncated file unexpectedly loaded" << std::endl;
        return 1;
    }

    if (static_cast<int> (te::getAudioTracks (*app.edit).size()) != tracksBefore
        || app.edit->tempoSequence.getTempo (0)->getBpm() != bpmBefore)
    {
        std::cerr << "[SMOKE_TEST] FAIL: failed open disturbed the active Edit" << std::endl;
        return 1;
    }

    if (! app.loadSessionFromFileSafely (savedFile))
    {
        std::cerr << "[SMOKE_TEST] FAIL: valid file did not load" << std::endl;
        return 1;
    }

    if (static_cast<int> (te::getAudioTracks (*app.edit).size()) != 4)
    {
        std::cerr << "[SMOKE_TEST] FAIL: loaded track count mismatch" << std::endl;
        return 1;
    }

    //--- WO-003: MidiList::removeNote + UndoManager undo/redo round-trip ------
    {
        auto* clip = [&]() -> te::MidiClip*
        {
            for (auto* c : te::getAudioTracks (*app.edit)[0]->getClips())
                if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                    return mc;
            return nullptr;
        }();

        if (clip == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: no MIDI clip available for undo check" << std::endl;
            return 1;
        }

        auto& seq = clip->getSequence();
        auto& um  = app.edit->getUndoManager();

        um.beginNewTransaction ("Smoke Delete Note");
        seq.removeNote (*seq.getNotes().getLast(), &um);

        if (seq.getNumNotes() != 3)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected 3 notes after removeNote, got "
                      << seq.getNumNotes() << std::endl;
            return 1;
        }

        um.undo();

        if (seq.getNumNotes() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected 4 notes after UndoManager::undo, got "
                      << seq.getNumNotes() << std::endl;
            return 1;
        }

        um.redo();

        if (seq.getNumNotes() != 3)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected 3 notes after UndoManager::redo, got "
                      << seq.getNumNotes() << std::endl;
            return 1;
        }

        um.undo();

        // The round-trip returns the Edit to its saved content.
        app.refreshDirtyFromSavedState();

        if (app.hasUnsavedChanges())
        {
            std::cerr << "[SMOKE_TEST] FAIL: undo round-trip did not return to a clean session" << std::endl;
            return 1;
        }
    }

    //--- WO-003 follow-up: dirty-state isolation + undo-to-saved -----------
    {
        // Transport movement must not dirty the session (PR #2 finding 1).
        app.edit->getTransport().setPosition (tracktion::TimePosition::fromSeconds (2.0));

        if (app.hasUnsavedChanges())
        {
            std::cerr << "[SMOKE_TEST] FAIL: transport setPosition marked the session dirty" << std::endl;
            return 1;
        }

        // A real content edit must dirty it...
        auto& um = app.edit->getUndoManager();
        um.beginNewTransaction ("Smoke BPM Change");
        app.edit->tempoSequence.getTempo (0)->setBpm (130.0);

        if (! app.hasUnsavedChanges())
        {
            std::cerr << "[SMOKE_TEST] FAIL: BPM edit did not mark the session dirty" << std::endl;
            return 1;
        }

        // ...and undoing that edit must return to clean (undo-to-saved).
        um.undo();
        app.refreshDirtyFromSavedState();

        if (app.hasUnsavedChanges())
        {
            std::cerr << "[SMOKE_TEST] FAIL: undo back to the saved state still shows dirty" << std::endl;
            return 1;
        }

        if (app.edit->tempoSequence.getTempo (0)->getBpm() != 124.0)
        {
            std::cerr << "[SMOKE_TEST] FAIL: BPM was not restored by undo, got "
                      << app.edit->tempoSequence.getTempo (0)->getBpm() << std::endl;
            return 1;
        }
    }

    //--- WO-003 follow-up: dragging past a neighbor must not transfer ---------
    {
        auto* clip = [&]() -> te::MidiClip*
        {
            for (auto* c : te::getAudioTracks (*app.edit)[0]->getClips())
                if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                    return mc;
            return nullptr;
        }();

        if (clip == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: no MIDI clip available for drag-reorder check" << std::endl;
            return 1;
        }

        auto& seq = clip->getSequence();
        auto& um  = app.edit->getUndoManager();

        auto findNoteAtBeat = [&seq] (double beat)
        {
            for (auto* n : seq.getNotes())
                if (std::abs (n->getStartBeat().inBeats() - beat) < 1e-4)
                    return n;
            return (te::MidiNote*) nullptr;
        };

        auto* dragged  = findNoteAtBeat (0.0);
        auto* neighbor = findNoteAtBeat (2.0);

        if (dragged == nullptr || neighbor == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: could not locate notes for drag-reorder check" << std::endl;
            return 1;
        }

        const juce::ValueTree draggedState = dragged->state;
        const double neighborStart = neighbor->getStartBeat().inBeats();
        const int neighborPitch = neighbor->getNoteNumber();

        um.beginNewTransaction ("Smoke Drag Note");
        dragged->setStartAndLength (tracktion::BeatPosition::fromBeats (2.5),
                                    tracktion::BeatDuration::fromBeats (1.0), &um);

        // Re-resolve the dragged note by its stable ValueTree identity: the
        // array re-sorts by startBeat, so an index would have moved.
        te::MidiNote* tracked = nullptr;
        for (auto* n : seq.getNotes())
            if (n->state == draggedState)
                tracked = n;

        if (tracked == nullptr || std::abs (tracked->getStartBeat().inBeats() - 2.5) > 1e-4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: dragged note did not land at 2.5 beats" << std::endl;
            return 1;
        }

        auto* neighborAfter = findNoteAtBeat (neighborStart);

        if (neighborAfter == nullptr || neighborAfter->getNoteNumber() != neighborPitch)
        {
            std::cerr << "[SMOKE_TEST] FAIL: neighboring note was disturbed by the drag" << std::endl;
            return 1;
        }

        if (seq.getNumNotes() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: note count changed during drag, got "
                      << seq.getNumNotes() << std::endl;
            return 1;
        }
    }

    //--- WO-004: mixer gain/pan + Track 2 instrument persistence --------------
    {
        auto* edit = app.edit.get();
        auto tracks = te::getAudioTracks (*edit);

        if (tracks.size() < 2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected at least 2 tracks for WO-004 mixer check" << std::endl;
            return 1;
        }

        auto* track2 = tracks[1];
        track2->setName ("Bass Line");

        if (auto* vol = track2->getVolumePlugin())
        {
            vol->setVolumeDb (-6.5f);
            vol->setPan (0.35f);
        }

        // Two inserts via the mixer's position helper: the second must land
        // after the first in the chain, ahead of the terminal volume plugin.
        tracktion::EditItemID firstSynthID, secondSynthID;

        if (auto bassSynth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
        {
            firstSynthID = bassSynth->itemID;
            track2->pluginList.insertPlugin (bassSynth, MixerComponent::getInsertIndexForNewPlugin (*track2), nullptr);
        }

        if (auto secondSynth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
        {
            secondSynthID = secondSynth->itemID;
            track2->pluginList.insertPlugin (secondSynth, MixerComponent::getInsertIndexForNewPlugin (*track2), nullptr);
        }

        app.mixerComponent.rebuildStrips();

        const auto mixerDir = tempDir.getChildFile ("tcs_wo004_mixer_smoke");
        mixerDir.deleteRecursively();
        mixerDir.createDirectory();

        const auto mixerTestFile = mixerDir.getChildFile ("MixerRoundTrip.tracktionedit");

        const auto prevEditFile = app.currentEditFile;
        const bool prevUntitled = app.sessionUntitled;

        app.currentEditFile = mixerTestFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (mixerTestFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-004 mixer round-trip save/load failed" << std::endl;
            return 1;
        }

        auto reloadedTracks = te::getAudioTracks (*app.edit);

        if (reloadedTracks.size() < 2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-004 reloaded track count mismatch" << std::endl;
            return 1;
        }

        auto* reloadedVol = reloadedTracks[1]->getVolumePlugin();
        std::vector<tracktion::EditItemID> reloadedSynthIDs;

        for (auto* p : reloadedTracks[1]->pluginList)
            if (dynamic_cast<te::FourOscPlugin*> (p) != nullptr)
                reloadedSynthIDs.push_back (p->itemID);

        if (reloadedVol == nullptr
            || std::abs (reloadedVol->getVolumeDb() - (-6.5f)) > 0.05f
            || std::abs (reloadedVol->getPan() - 0.35f) > 0.02f
            || reloadedSynthIDs.size() != 2
            || reloadedSynthIDs[0] != firstSynthID
            || reloadedSynthIDs[1] != secondSynthID)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-004 mixer dB/pan, Track 2 plugins, or insert order did not persist!" << std::endl;
            return 1;
        }

        // Restore the app's file pointer: the round-trip file is deleted below.
        app.currentEditFile = prevEditFile;
        app.sessionUntitled = prevUntitled;

        mixerDir.deleteRecursively();
    }

    //--- WO-005: Reason-style modular CV/audio rack persistence ---------------
    {
        auto* edit = app.edit.get();
        auto tracks = te::getAudioTracks (*edit);

        if (tracks.isEmpty())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 needs an audio track for the RackInstance" << std::endl;
            return 1;
        }

        auto rack = edit->getRackList().addNewRack();
        rack->rackName = "Reason CV Wobble Rack";

        auto lfoPtr    = edit->getPluginCache().createNewPlugin (CvLfoPlugin::xmlTypeName, {});
        auto wobblePtr = edit->getPluginCache().createNewPlugin (CvWobbleFilterPlugin::xmlTypeName, {});

        auto* lfo    = dynamic_cast<CvLfoPlugin*> (lfoPtr.get());
        auto* wobble = dynamic_cast<CvWobbleFilterPlugin*> (wobblePtr.get());

        if (lfo == nullptr || wobble == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 built-in CV plugins failed to instantiate" << std::endl;
            return 1;
        }

        if (! rack->addPlugin (lfoPtr,    { 0.22f, 0.24f }, false)
            || ! rack->addPlugin (wobblePtr, { 0.52f, 0.26f }, false))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 could not add plugins to the rack" << std::endl;
            return 1;
        }

        if (! rack->addConnection (lfo->itemID, 0, wobble->itemID, 0))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 could not add CV LFO -> Wobble connection" << std::endl;
            return 1;
        }

        const auto instanceTree = te::RackInstance::create (*rack);
        tracks.getFirst()->pluginList.insertPlugin (instanceTree, MixerComponent::getInsertIndexForNewPlugin (*tracks.getFirst()));

        const auto rackDir = tempDir.getChildFile ("tcs_wo005_rack_smoke");
        rackDir.deleteRecursively();
        rackDir.createDirectory();

        const auto rackTestFile = rackDir.getChildFile ("RackRoundTrip.tracktionedit");

        app.currentEditFile = rackTestFile;
        app.sessionUntitled = false;
        app.saveSession();

        //--- Regression (Muse finding 1): syncFromEdit() must be read-only ---
        {
            auto smokeTracks = te::getAudioTracks (*app.edit);

            if (! smokeTracks.isEmpty())
            {
                auto* vol = smokeTracks[0]->getVolumePlugin();

                if (vol != nullptr)
                {
                    app.edit->getUndoManager().beginNewTransaction ("Smoke Fader Move");
                    vol->setVolumeDb (-4.2f);

                    app.modularRackComponent.syncFromEdit();

                    if (std::abs (vol->getVolumeDb() - (-4.2f)) > 0.05f)
                    {
                        std::cerr << "[SMOKE_TEST] FAIL: syncFromEdit() reverted the last edit!" << std::endl;
                        return 1;
                    }
                }
            }
        }

        //--- Regression (Muse finding 2): no stacked nodes, aligned cables ---
        app.modularRackComponent.setSize (640, 320);
        app.modularRackComponent.resized();
        app.modularRackComponent.syncFromEdit();

        if (! app.modularRackComponent.verifyNonOverlappingLayoutForSmokeTest())
        {
            std::cerr << "[SMOKE_TEST] FAIL: ModularRackComponent nodes stacked or pin/cable coordinates misaligned!" << std::endl;
            return 1;
        }

        // Release every RackType/Plugin reference BEFORE the reload destroys the
        // Edit. ~RackType walks Edit-owned state, so a Ptr held past
        // edit.reset() is a use-after-free (ASAN: heap-use-after-free in
        // RackType::ConnectionList::deleteObject).
        rack = nullptr;
        lfoPtr = nullptr;
        wobblePtr = nullptr;

        if (! app.loadSessionFromFileSafely (rackTestFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 rack round-trip save/load failed" << std::endl;
            return 1;
        }

        const auto& reloadedRacks = app.edit->getRackList().getTypes();

        te::RackType::Ptr reloadedRack;

        for (auto* r : reloadedRacks)
            if (r->rackName.get() == "Reason CV Wobble Rack")
                reloadedRack = r;

        if (reloadedRack == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 named rack did not persist" << std::endl;
            return 1;
        }

        const auto plugins = reloadedRack->getPlugins();

        if (plugins.size() != 2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 reloaded rack plugin count mismatch" << std::endl;
            return 1;
        }

        bool foundLfo = false, foundWobble = false;

        for (auto* p : plugins)
        {
            if (dynamic_cast<CvLfoPlugin*> (p) != nullptr)          foundLfo = true;
            if (dynamic_cast<CvWobbleFilterPlugin*> (p) != nullptr) foundWobble = true;
        }

        if (! foundLfo || ! foundWobble)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 CV plugin types did not persist" << std::endl;
            return 1;
        }

        const auto reloadedConnections = reloadedRack->getConnections();

        if (reloadedConnections.size() != 1)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 connection count did not persist" << std::endl;
            return 1;
        }

        auto* conn = reloadedConnections.getFirst();

        if (reloadedRack->getPluginForID (conn->sourceID.get()) == nullptr
            || reloadedRack->getPluginForID (conn->destID.get()) == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-005 connection endpoints did not resolve after reload" << std::endl;
            return 1;
        }

        rackDir.deleteRecursively();
    }

    //===========================================================================
    // WO-006 Regression: Riff-Layering BGM Solver & WaveAudioClip Persistence
    //===========================================================================
    {
        const auto riffTempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("tcs_wo006_riff_smoke");

        riffTempDir.deleteRecursively();
        riffTempDir.createDirectory();

        RiffLibrary smokeLibrary;
        smokeLibrary.ensureStarterPack (riffTempDir);

        if (smokeLibrary.getRiffs().size() < 8)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected at least 8 starter WAV riffs, got "
                      << smokeLibrary.getRiffs().size() << std::endl;
            return 1;
        }

        // User WaveAudioClip preservation: a clip the solver did not create
        // must survive generateArrangement untouched.
        auto userTracks = te::getAudioTracks (*app.edit);

        const tracktion::TimeRange userClipRange {
            app.edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (0.0)),
            app.edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (8.0))
        };

        auto userClip = userTracks[0]->insertWaveClip (
            "User Imported Vocal",
            smokeLibrary.getRiffs()[0].wavFile,
            te::ClipPosition { userClipRange, tracktion::TimeDuration::fromSeconds (0.0) },
            false);

        if (userClip == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: could not insert the user WaveAudioClip for the preservation check" << std::endl;
            return 1;
        }

        BgmRequest req;

        req.genre            = "Cyberpunk";
        req.targetKeyLabel   = "Cm";
        req.targetPitchClass = RiffLibrary::parseKeyPitchClass ("Cm"); // 0 (+3 semitones from Am=9)
        req.targetEnergy     = 4;
        req.totalBars        = 8;

        const int createdClips = BgmArrangementSolver::generateArrangement (*app.edit, smokeLibrary, req);

        if (createdClips < 8)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected >= 8 layered BGM WaveAudioClips, got "
                      << createdClips << std::endl;
            return 1;
        }

        {
            auto postTracks = te::getAudioTracks (*app.edit);

            te::WaveAudioClip* userClipPtr = nullptr;

            for (auto* c : postTracks[0]->getClips())
                if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                    if (wc->getName() == "User Imported Vocal")
                        userClipPtr = wc;

            if (userClipPtr == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: generateArrangement deleted the user WaveAudioClip on Track 0" << std::endl;
                return 1;
            }

            // Remove it again so the persisted-clip count below stays exact.
            userClipPtr->removeFromParent();
        }

        // Release the last smart pointer to that clip before the reload below
        // destroys the Edit: ~WaveAudioClip walks Edit-owned state, so a Ptr
        // held past edit.reset() is a use-after-free.
        userClip = nullptr;

        // Empty-library no-op: with no riffs, generateArrangement must return 0
        // and leave the track name and clips untouched.
        {
            RiffLibrary emptyLib;

            auto preTracks = te::getAudioTracks (*app.edit);
            int preClipCount = 0;

            for (auto* t : preTracks)
                preClipCount += t->getClips().size();

            preTracks[0]->setName ("Custom User Track");

            if (BgmArrangementSolver::generateArrangement (*app.edit, emptyLib, req) != 0)
            {
                std::cerr << "[SMOKE_TEST] FAIL: empty-library generateArrangement should return 0" << std::endl;
                return 1;
            }

            auto postTracks = te::getAudioTracks (*app.edit);
            int postClipCount = 0;

            for (auto* t : postTracks)
                postClipCount += t->getClips().size();

            if (postTracks[0]->getName() != "Custom User Track" || postClipCount != preClipCount)
            {
                std::cerr << "[SMOKE_TEST] FAIL: empty-library generateArrangement renamed a track or removed clips" << std::endl;
                return 1;
            }
        }

        // Tight inner scope so no clip/track pointer outlives the reload.
        {
            auto smokeTracks = te::getAudioTracks (*app.edit);

            if (smokeTracks.size() < 2)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-006 expected the four BGM tracks" << std::endl;
                return 1;
            }

            te::WaveAudioClip* firstBassWave = nullptr;

            for (auto* c : smokeTracks[1]->getClips())
            {
                if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                {
                    firstBassWave = wc;
                    break;
                }
            }

            if (firstBassWave == nullptr
                || ! firstBassWave->getAutoTempo()
                || std::abs (firstBassWave->getLoopInfo().getNumBeats() - 8.0) > 0.01
                || std::abs (firstBassWave->getPitchChange() - 3.0f) > 0.01f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: BGM Bass WaveAudioClip missing autoTempo, 8-beat LoopInfo, or +3st pitch shift" << std::endl;
                return 1;
            }
        }

        const auto bgmSessionFile = riffTempDir.getChildFile ("BgmRoundTrip.tracktionedit");

        app.currentEditFile = bgmSessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (bgmSessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-006 BGM session save/load round-trip failed" << std::endl;
            return 1;
        }

        int persistedWaveClips = 0;
        bool persistedBassSettings = false;

        {
            auto reloadedTracks = te::getAudioTracks (*app.edit);

            for (auto* t : reloadedTracks)
                for (auto* c : t->getClips())
                    if (dynamic_cast<te::WaveAudioClip*> (c) != nullptr)
                        ++persistedWaveClips;

            if (reloadedTracks.size() >= 2)
            {
                for (auto* c : reloadedTracks[1]->getClips())
                {
                    if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                    {
                        if (wc->getAutoTempo() && std::abs (wc->getPitchChange() - 3.0f) <= 0.01f)
                            persistedBassSettings = true;
                    }
                }
            }
        }

        if (persistedWaveClips != createdClips || ! persistedBassSettings)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected " << createdClips
                      << " persisted WaveAudioClips with intact autoTempo/pitchChange after reload, got "
                      << persistedWaveClips << std::endl;
            return 1;
        }

        riffTempDir.deleteRecursively();

        std::cout << "[SMOKE_TEST] WO-006 riff-layering BGM solver & WaveAudioClip persistence PASS" << std::endl;
    }

    //===========================================================================
    // WO-007 Regression: Native TCP JSON-RPC 2.0 Server (DawIpcServer)
    //===========================================================================
    {
        int boundTestPort = 0;

        for (int candidatePort = 19450; candidatePort <= 19465; ++candidatePort)
        {
            if (app.ipcServer.startListening (candidatePort))
            {
                boundTestPort = candidatePort;
                break;
            }
        }

        if (boundTestPort == 0 || ! app.ipcServer.isListening())
        {
            std::cerr << "[SMOKE_TEST] FAIL: DawIpcServer could not bind a loopback test port" << std::endl;
            return 1;
        }

        // Sends a newline-delimited JSON-RPC request over a real TCP socket on a
        // worker thread while the calling (message) thread pumps the RPC queue.
        auto sendTcpRpc = [&app, boundTestPort] (const juce::String& jsonLine) -> juce::var
        {
            auto clientFuture = std::async (std::launch::async, [boundTestPort, jsonLine]() -> juce::String
            {
                juce::StreamingSocket sock;

                if (! sock.connect ("127.0.0.1", boundTestPort, 1500))
                    return {};

                const juce::String payload = jsonLine + "\n";

                if (sock.write (payload.toRawUTF8(),
                                static_cast<int> (payload.getNumBytesAsUTF8())) <= 0)
                    return {};

                juce::MemoryBlock respBuf;
                char ch = 0;

                while (sock.waitUntilReady (true, 2000) == 1 && sock.read (&ch, 1, false) == 1)
                {
                    if (ch == '\n')
                        break;

                    if (ch != '\r')
                        respBuf.append (&ch, 1);
                }

                return juce::String::fromUTF8 (static_cast<const char*> (respBuf.getData()),
                                              static_cast<int> (respBuf.getSize()));
            });

            while (clientFuture.wait_for (std::chrono::milliseconds (5)) != std::future_status::ready)
                app.ipcServer.servicePendingRequestsOnMessageThread();

            app.ipcServer.servicePendingRequestsOnMessageThread();

            return juce::JSON::parse (clientFuture.get());
        };

        // 1. Create an AI Lead Track via track.create
        const auto createResp = sendTcpRpc (
            R"({"jsonrpc":"2.0","id":1,"method":"track.create","params":{"name":"MCP AI Lead","plugin_type":"4osc"}})");

        const int aiTrackIdx = static_cast<int> (createResp["result"]["track_index"]);

        if (aiTrackIdx < 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: track.create returned unexpected track_index " << aiTrackIdx << std::endl;
            app.ipcServer.stopListening();
            return 1;
        }

        // 2. Write 3 MIDI notes via midi.add_notes
        const auto addNotesResp = sendTcpRpc (
            juce::String (R"({"jsonrpc":"2.0","id":2,"method":"midi.add_notes","params":{"track_index":)")
            + juce::String (aiTrackIdx)
            + R"(,"notes":[{"pitch":60,"start_beat":0.0,"length_beats":1.0,"velocity":110},{"pitch":64,"start_beat":1.0,"length_beats":1.0,"velocity":112},{"pitch":67,"start_beat":2.0,"length_beats":2.0,"velocity":118}]}})");

        if (static_cast<int> (addNotesResp["result"]["added"]) != 3)
        {
            std::cerr << "[SMOKE_TEST] FAIL: midi.add_notes did not add 3 notes" << std::endl;
            app.ipcServer.stopListening();
            return 1;
        }

        // 3. Read back notes via midi.get_notes
        const auto getNotesResp = sendTcpRpc (
            juce::String (R"({"jsonrpc":"2.0","id":3,"method":"midi.get_notes","params":{"track_index":)")
            + juce::String (aiTrackIdx) + R"(,"start_beat":0.0,"end_beat":8.0}})");

        const auto* notesArr = getNotesResp["result"]["notes"].getArray();

        if (notesArr == nullptr || notesArr->size() != 3)
        {
            std::cerr << "[SMOKE_TEST] FAIL: midi.get_notes expected 3 notes" << std::endl;
            app.ipcServer.stopListening();
            return 1;
        }

        // 4. Update mixer & transport, then verify via daw.get_state
        sendTcpRpc (
            juce::String (R"({"jsonrpc":"2.0","id":4,"method":"mixer.set_track","params":{"track_index":)")
            + juce::String (aiTrackIdx) + R"(,"volume_db":-3.5,"pan":-0.25,"mute":true}})");

        sendTcpRpc (R"({"jsonrpc":"2.0","id":5,"method":"transport.control","params":{"bpm":132.0,"beat":4.0}})");

        const auto stateResp = sendTcpRpc (R"({"jsonrpc":"2.0","id":6,"method":"daw.get_state","params":{}})");

        const double stateBpm = static_cast<double> (stateResp["result"]["bpm"]);
        const auto* stateTracks = stateResp["result"]["tracks"].getArray();
        const double statePositionBeats = static_cast<double> (stateResp["result"]["position_beats"]);

        app.ipcServer.stopListening();

        // Read the AI track's entry back: the mixer update (volume -3.5 dB,
        // pan -0.25, mute true) and the beat-4 seek must be visible in the
        // state response, not merely sent. A missing property reads as 0/false
        // through var conversion, so a dropped update fails loudly here.
        const auto aiTrackState = (stateTracks != nullptr && stateTracks->size() > aiTrackIdx)
                                      ? stateTracks->getUnchecked (aiTrackIdx)
                                      : juce::var();

        const double stateVolumeDb = static_cast<double> (aiTrackState["volume_db"]);
        const double statePan      = static_cast<double> (aiTrackState["pan"]);
        const bool stateMute       = static_cast<bool> (aiTrackState["mute"]);

        if (std::abs (stateBpm - 132.0) > 0.01 || stateTracks == nullptr || stateTracks->size() <= aiTrackIdx
            || std::abs (stateVolumeDb - (-3.5)) > 0.01 || std::abs (statePan - (-0.25)) > 0.01
            || ! stateMute || std::abs (statePositionBeats - 4.0) > 0.01)
        {
            std::cerr << "[SMOKE_TEST] FAIL: daw.get_state did not reflect RPC mutations" << std::endl;
            return 1;
        }

        // 5. Verify RPC-created track & notes survive save/load round-trip
        const auto ipcTempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("tcs_wo007_ipc_smoke");

        ipcTempDir.deleteRecursively();
        ipcTempDir.createDirectory();

        const auto ipcSessionFile = ipcTempDir.getChildFile ("IpcRoundTrip.tracktionedit");

        app.currentEditFile = ipcSessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! ipcSessionFile.existsAsFile() || ! app.loadSessionFromFileSafely (ipcSessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-007 IPC session save/load round-trip failed" << std::endl;
            return 1;
        }

        const auto reloadedVerify = app.ipcServer.executeJsonRpcOnMessageThread (
            juce::String (R"({"jsonrpc":"2.0","id":7,"method":"midi.get_notes","params":{"track_index":)")
            + juce::String (aiTrackIdx) + R"(,"start_beat":0.0,"end_beat":8.0}})");

        const auto parsedReload = juce::JSON::parse (reloadedVerify);
        const auto* reloadedNotes = parsedReload["result"]["notes"].getArray();

        if (reloadedNotes == nullptr || reloadedNotes->size() != 3)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-007 RPC-authored MIDI notes did not persist across reload" << std::endl;
            return 1;
        }

        ipcTempDir.deleteRecursively();

        std::cout << "[SMOKE_TEST] WO-007 native TCP JSON-RPC IPC server & persistence PASS" << std::endl;
    }

    //===========================================================================
    // WO-008 Regression: Non-Destructive Bounce-to-Take Waveform Editor & Undo
    //===========================================================================
    {
        const auto waveTempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("tcs_wo008_wave_smoke");

        waveTempDir.deleteRecursively();
        waveTempDir.createDirectory();

        const auto takesDir = waveTempDir.getChildFile ("Takes");

        tracktion::EditItemID targetWaveClipID;
        juce::File originalWavFile;
        juce::int64 originalWavSize = 0;

        // 1. Provision a source .wav in the temp dir, insert a WaveAudioClip
        //    referencing it, and capture that clip as the bounce target. The
        //    WO-006 riff clips point into a temp dir that block deletes, so
        //    WO-008 must own its source material.
        const auto sourceWav = waveTempDir.getChildFile ("source_take.wav");

        {
            juce::WavAudioFormat wavFormat;
            auto fileStream = std::make_unique<juce::FileOutputStream> (sourceWav);

            fileStream->setPosition (0);
            fileStream->truncate();

            std::unique_ptr<juce::OutputStream> outStream (fileStream.release());

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wavFormat.createWriterFor (outStream,
                                           juce::AudioFormatWriterOptions{}
                                               .withSampleRate (44100.0)
                                               .withNumChannels (2)
                                               .withBitsPerSample (16)));

            if (writer == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-008 could not create the source writer" << std::endl;
                return 1;
            }

            juce::AudioBuffer<float> sourceBuffer (2, 44100); // 1 second

            for (int ch = 0; ch < sourceBuffer.getNumChannels(); ++ch)
                for (int i = 0; i < sourceBuffer.getNumSamples(); ++i)
                    sourceBuffer.setSample (ch, i,
                        static_cast<float> (0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / 44100.0)));

            if (! writer->writeFromAudioSampleBuffer (sourceBuffer, 0, sourceBuffer.getNumSamples()))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-008 could not write the source WAV" << std::endl;
                return 1;
            }
        }

        auto waveTracks = te::getAudioTracks (*app.edit);

        if (waveTracks.isEmpty())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-008 no audio track to insert into" << std::endl;
            return 1;
        }

        const tracktion::TimeRange waveClipRange {
            tracktion::TimePosition::fromSeconds (0.0),
            tracktion::TimePosition::fromSeconds (1.0)
        };

        auto insertedClip = waveTracks.getFirst()->insertWaveClip (
            "WO-008 Source", sourceWav,
            te::ClipPosition { waveClipRange, tracktion::TimeDuration::fromSeconds (0.0) }, false);

        if (insertedClip == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-008 insertWaveClip failed" << std::endl;
            return 1;
        }

        // Pin the source to an absolute path. insertWaveClip stores a path
        // relative to the edit file when the engine's heuristics allow it,
        // but relative sources only resolve through a project/filePathResolver
        // (neither exists here, and WO-007 deleted its session dir), so the
        // test anchors its source material the same way the bounce pins takes.
        insertedClip->getSourceFileReference().setToFile (
            sourceWav, te::SourceFileReference::PathStyle::alwaysAbsolute, false);

        targetWaveClipID = insertedClip->itemID;
        originalWavFile  = insertedClip->getSourceFileReference().getFile();
        originalWavSize  = originalWavFile.getSize();

        if (! targetWaveClipID.isValid() || originalWavSize <= 0)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-008 could not bind a WaveAudioClip source" << std::endl;
            return 1;
        }

        // 2. Bounce-to-take: silence the first 25% [0.0, 0.25] of the clip.
        juce::File bouncedTakeFile;

        if (! WaveformEditorComponent::applyBounceToTakeDsp (
                *app.edit,
                targetWaveClipID,
                WaveformEditorComponent::DspOperation::silence,
                0.0,
                0.25,
                takesDir,
                &bouncedTakeFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-008 applyBounceToTakeDsp returned false" << std::endl;
            return 1;
        }

        // 3. Original .wav untouched, bounced take exists and is silenced up front.
        if (! originalWavFile.existsAsFile()
            || originalWavFile.getSize() != originalWavSize
            || ! bouncedTakeFile.existsAsFile()
            || bouncedTakeFile == originalWavFile)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-008 non-destructive file invariant violated" << std::endl;
            return 1;
        }

        {
            juce::WavAudioFormat wavFormat;

            std::unique_ptr<juce::AudioFormatReader> reader (
                wavFormat.createReaderFor (bouncedTakeFile.createInputStream().release(), true));

            if (reader == nullptr || reader->lengthInSamples < 100)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-008 could not read bounced take WAV" << std::endl;
                return 1;
            }

            const int checkSamples = static_cast<int> (reader->lengthInSamples / 5); // inside first 20%
            juce::AudioBuffer<float> checkBuf (1, checkSamples);

            reader->read (&checkBuf, 0, checkSamples, 0, true, false);

            if (checkBuf.getMagnitude (0, 0, checkSamples) > 1e-4f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-008 silence DSP did not zero leading samples" << std::endl;
                return 1;
            }
        }

        // 4. Undo restores the original .wav reference, redo restores the take.
        app.edit->getUndoManager().undo();

        {
            app.waveformEditorComponent.setTarget (app.edit.get(), targetWaveClipID);

            juce::File afterUndoFile;

            for (auto* t : te::getAudioTracks (*app.edit))
                for (auto* c : t->getClips())
                    if (c->itemID == targetWaveClipID)
                        if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                            afterUndoFile = wc->getSourceFileReference().getFile();

            if (afterUndoFile != originalWavFile)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-008 Undo did not restore the original WAV reference" << std::endl;
                return 1;
            }
        }

        app.edit->getUndoManager().redo();

        // 5. The bounced take reference survives a save/load round-trip.
        const auto waveSessionFile = waveTempDir.getChildFile ("WaveBounceRoundTrip.tracktionedit");

        // Release the clip smart pointer before the reload destroys its Edit.
        insertedClip = nullptr;

        app.currentEditFile = waveSessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! waveSessionFile.existsAsFile() || ! app.loadSessionFromFileSafely (waveSessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-008 session save/load round-trip failed" << std::endl;
            return 1;
        }

        bool foundPersistedBounce = false;

        for (auto* t : te::getAudioTracks (*app.edit))
            for (auto* c : t->getClips())
                if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                    if (wc->getSourceFileReference().getFile() == bouncedTakeFile)
                        foundPersistedBounce = true;

        if (! foundPersistedBounce)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-008 bounced take reference did not persist across reload" << std::endl;
            return 1;
        }

        waveTempDir.deleteRecursively();

        std::cout << "[SMOKE_TEST] WO-008 non-destructive waveform bounce-to-take & Undo/Redo PASS" << std::endl;
    }

    //--- WO-002 (3): cooperative VST3 scanner shutdown -------------------------
    app.startVst3Scan();
    app.stopVst3Scan (10000);

    if (app.isVst3ScanRunning())
    {
        std::cerr << "[SMOKE_TEST] FAIL: VST3 scanner did not join cleanly" << std::endl;
        return 1;
    }

    app.stopVst3Scan();  // idempotent no-op

    if (app.isVst3ScanRunning())
    {
        std::cerr << "[SMOKE_TEST] FAIL: repeated stopVst3Scan was not a no-op" << std::endl;
        return 1;
    }

    //--- cleanup --------------------------------------------------------------
    sessionFile.deleteFile();
    savedFile.deleteFile();
    otherFile.deleteFile();
    corruptFile.deleteFile();
    truncatedFile.deleteFile();

    std::cout << "[SMOKE_TEST] PASS: persistence, non-destructive New, atomic Open rollback, "
                 "Undo/Redo note editing, dirty-state isolation, mixer gain/pan + instrument "
                 "persistence & insert order, CV rack plugin/connection persistence, "
                 "BGM riff-layering WaveAudioClip time-stretch/pitch persistence, "
                 "native TCP JSON-RPC IPC round-trip, non-destructive waveform "
                 "bounce-to-take DSP, and VST3 scanner lifecycle verified." << std::endl;
    return 0;
}

//==============================================================================
// JUCE application shell
//==============================================================================
class AIDAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        if (commandLine.contains ("--smoke-test"))
        {
            setApplicationReturnValue (MainComponent::runHeadlessSmokeTest());
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (name, juce::Colour (0xff181a1f), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (AIDAWApplication)
