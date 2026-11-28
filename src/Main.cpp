#include "MainComponent.h"
#include "AudioToMidiTranscriber.h"
#include "CoreIsolationCheck.h"
#include "AdrCueGenerator.h"
#include "HardwarePluginEditors.h"
#include "HardwareRackPlugins.h"
#include "LoudnessMeter.h"
#include "PlatformConfig.h"
#include "PluginPresetManager.h"
#include "SidechainCompressorPlugin.h"
#include "GameAssetExporter.h"
#include "MusicTheoryEngine.h"
#include "RpgSoundboardPlugin.h"
#include "SfxForgeEngine.h"
#include "StudioPluginWindow.h"
#include "VocalChannelPlugin.h"
#include "VocalSpecAnalyzer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

    //===========================================================================
    // WO-009 Regression: filePathResolver, MCP rack.patch/import/export & Mixdown WAV
    //===========================================================================
    {
        const auto wo009TempDir = tempDir.getChildFile ("tcs_wo009_smoke");
        wo009TempDir.deleteRecursively();
        wo009TempDir.createDirectory();

        // 1. Verify edit->filePathResolver resolves a relative audio path across save/reload
        const auto audioSubDir = wo009TempDir.getChildFile ("Audio");
        audioSubDir.createDirectory();
        const auto relWavFile = audioSubDir.getChildFile ("RelativeStem.wav");

        {
            constexpr int sr = 44100;
            juce::AudioBuffer<float> tone (2, sr);

            for (int i = 0; i < sr; ++i)
            {
                const float s = static_cast<float> (
                    0.35 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr));
                tone.setSample (0, i, s);
                tone.setSample (1, i, s);
            }

            auto fs = std::make_unique<juce::FileOutputStream> (relWavFile);
            fs->setPosition (0);
            fs->truncate();
            std::unique_ptr<juce::OutputStream> os (fs.release());

            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                             .withSampleRate (sr)
                                             .withNumChannels (2)
                                             .withBitsPerSample (16)));

            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (tone, 0, sr))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 could not write RelativeStem.wav" << std::endl;
                return 1;
            }
        }

        const auto relSessionFile = wo009TempDir.getChildFile ("PortableProject.tracktionedit");
        {
            auto tracks = te::getAudioTracks (*app.edit);

            if (tracks.isEmpty())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 expected audio tracks" << std::endl;
                return 1;
            }

            // Pick a clean unmuted track without the unconnected WO-005 RackInstance.
            auto* targetTrack = tracks.size() >= 3 ? tracks[2] : tracks[0];
            targetTrack->setMute (false);

            const tracktion::TimeRange r {
                tracktion::TimePosition::fromSeconds (0.0),
                tracktion::TimePosition::fromSeconds (1.0)
            };

            if (auto wc = targetTrack->insertWaveClip ("RelClip", relWavFile, te::ClipPosition { r, {} }, false))
                wc->state.setProperty (te::IDs::source, "Audio/RelativeStem.wav", nullptr);
        }

        app.currentEditFile = relSessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (relSessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-009 portable project reload failed" << std::endl;
            return 1;
        }

        bool resolvedRelativeClip = false;
        {
            for (auto* t : te::getAudioTracks (*app.edit))
            {
                for (auto* c : t->getClips())
                {
                    if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                    {
                        if (wc->getName() == "RelClip")
                        {
                            const auto resolved = wc->getSourceFileReference().getFile();

                            if (resolved.getFullPathName() == relWavFile.getFullPathName()
                                && resolved.existsAsFile())
                            {
                                resolvedRelativeClip = true;
                            }
                        }
                    }
                }
            }
        }

        if (! resolvedRelativeClip)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-009 filePathResolver failed to resolve Audio/RelativeStem.wav" << std::endl;
            return 1;
        }

        // 2. Verify MCP rack.get_state, rack.patch, audio.import_file & audio.export_wav over loopback TCP
        {
            if (app.edit->getRackList().getTypes().isEmpty())
            {
                auto rack = app.edit->getRackList().addNewRack();
                rack->rackName = "WO-009 Smoke Rack";
                auto lfoPtr    = app.edit->getPluginCache().createNewPlugin (CvLfoPlugin::xmlTypeName, {});
                auto wobblePtr = app.edit->getPluginCache().createNewPlugin (CvWobbleFilterPlugin::xmlTypeName, {});

                if (lfoPtr != nullptr && wobblePtr != nullptr)
                {
                    const auto lfoId    = lfoPtr->itemID;
                    const auto wobbleId = wobblePtr->itemID;
                    rack->addPlugin (lfoPtr,    { 0.2f, 0.25f }, false);
                    rack->addPlugin (wobblePtr, { 0.6f, 0.25f }, false);
                    rack->addConnection (lfoId, 0, wobbleId, 0);
                }
            }

            int boundTestPort = 0;

            for (int candidatePort = 19466; candidatePort <= 19485; ++candidatePort)
            {
                if (app.ipcServer.startListening (candidatePort))
                {
                    boundTestPort = candidatePort;
                    break;
                }
            }

            if (boundTestPort == 0 || ! app.ipcServer.isListening())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 DawIpcServer could not bind loopback test port" << std::endl;
                return 1;
            }

            struct IpcListenGuard
            {
                DawIpcServer& server;
                ~IpcListenGuard() { server.stopListening(); }
            } ipcGuard { app.ipcServer };

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

                    while (sock.waitUntilReady (true, 10000) == 1 && sock.read (&ch, 1, false) == 1)
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

            const auto rackState = sendTcpRpc (
                R"({"jsonrpc":"2.0","id":90,"method":"rack.get_state","params":{}})");
            const auto* racksArr = rackState["result"]["racks"].getArray();

            if (racksArr == nullptr || racksArr->isEmpty())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 rack.get_state returned no racks" << std::endl;
                return 1;
            }

            const auto* conns = (*racksArr)[0]["connections"].getArray();

            if (conns == nullptr || conns->isEmpty())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 rack.get_state returned no connections" << std::endl;
                return 1;
            }

            const int initialConnCount = conns->size();
            const auto c0 = (*conns)[0];
            const juce::String patchArgs =
                R"("rack_index":0,"source_id":)" + c0["source_id"].toString()
                + R"(,"source_pin":)" + c0["source_pin"].toString()
                + R"(,"dest_id":)" + c0["dest_id"].toString()
                + R"(,"dest_pin":)" + c0["dest_pin"].toString();

            const auto disconnResp = sendTcpRpc (
                R"({"jsonrpc":"2.0","id":91,"method":"rack.patch","params":{"action":"disconnect",)" + patchArgs + "}}");

            if (static_cast<int> (disconnResp["result"]["connection_count"]) != initialConnCount - 1)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 rack.patch disconnect did not decrement connection_count" << std::endl;
                return 1;
            }

            const auto reconnResp = sendTcpRpc (
                R"({"jsonrpc":"2.0","id":92,"method":"rack.patch","params":{"action":"connect",)" + patchArgs + "}}");

            if (static_cast<int> (reconnResp["result"]["connection_count"]) != initialConnCount)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 rack.patch connect did not restore connection_count" << std::endl;
                return 1;
            }

            auto importTracks = te::getAudioTracks (*app.edit);
            const int importTrackIdx = importTracks.size() >= 4 ? 3 : 0;
            importTracks[importTrackIdx]->setMute (false);

            const juce::String escapedWavPath = relWavFile.getFullPathName().replace ("\\", "\\\\");
            const auto importResp = sendTcpRpc (
                juce::String (R"({"jsonrpc":"2.0","id":93,"method":"audio.import_file","params":{"track_index":)")
                + juce::String (importTrackIdx)
                + R"(,"file_path":")" + escapedWavPath + R"(","start_beat":0.0,"clip_name":"AriosoImportedStem"}})");

            if (importResp["result"]["clip_name"].toString() != "AriosoImportedStem"
                || static_cast<juce::int64> (importResp["result"]["clip_id"]) <= 0)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 audio.import_file failed" << std::endl;
                return 1;
            }

            const auto rpcMixdownFile = wo009TempDir.getChildFile ("RpcMixdown.wav");
            const juce::String escapedOutPath = rpcMixdownFile.getFullPathName().replace ("\\", "\\\\");
            const auto exportRpcResp = sendTcpRpc (
                juce::String (R"({"jsonrpc":"2.0","id":94,"method":"audio.export_wav","params":{"output_path":")")
                + escapedOutPath + R"(","start_beat":0.0,"end_beat":4.0}})");

            if (static_cast<juce::int64> (exportRpcResp["result"]["file_size_bytes"]) <= 44
                || ! rpcMixdownFile.existsAsFile())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 audio.export_wav RPC failed: "
                          << juce::JSON::toString (exportRpcResp, true) << std::endl;
                return 1;
            }
        }

        // 3. Verify direct MixdownExporter::exportEditToWav renders 2 bars of non-silent stereo 44.1 kHz WAV
        const auto mixdownFile = wo009TempDir.getChildFile ("MasterMixdown.wav");
        const tracktion::TimeRange renderRange {
            tracktion::TimePosition::fromSeconds (0.0),
            app.edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (8.0))
        };

        if (! MixdownExporter::exportEditToWav (*app.edit, mixdownFile, renderRange, 44100.0, 16))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-009 MixdownExporter::exportEditToWav returned false" << std::endl;
            return 1;
        }

        {
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatReader> reader (
                fmt.createReaderFor (mixdownFile.createInputStream().release(), true));

            if (reader == nullptr
                || reader->numChannels != 2
                || std::abs (reader->sampleRate - 44100.0) > 0.1
                || reader->lengthInSamples < 44100)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 rendered MasterMixdown.wav is invalid or truncated" << std::endl;
                return 1;
            }

            const int checkLen = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples, 44100));
            juce::AudioBuffer<float> mixBuf (2, checkLen);
            reader->read (&mixBuf, 0, checkLen, 0, true, true);

            if (mixBuf.getMagnitude (0, checkLen) <= 1e-4f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-009 MasterMixdown.wav rendered pure silence" << std::endl;
                return 1;
            }
        }

        wo009TempDir.deleteRecursively();

        std::cout << "[SMOKE_TEST] WO-009 filePathResolver, MCP rack.patch/import_file & Master Mixdown WAV PASS" << std::endl;
    }

    //===========================================================================
    // WO-010 Regression: Metronome Click, Loop Transport, Step Rec & Header Layout
    //===========================================================================
    {
        // 1. Toggle click & loop range via transport.control RPC and verify via daw.get_state
        app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":100,"method":"transport.control","params":{"click":true,"loop":true,"loop_start_beat":0.0,"loop_end_beat":16.0}})");
        const auto stateResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":101,"method":"daw.get_state","params":{}})"));

        if (! static_cast<bool> (stateResp["result"]["click_enabled"])
            || ! static_cast<bool> (stateResp["result"]["looping"])
            || std::abs (static_cast<double> (stateResp["result"]["loop_start_beat"]) - 0.0) > 0.05
            || std::abs (static_cast<double> (stateResp["result"]["loop_end_beat"]) - 16.0) > 0.05)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-010 click/loop transport RPC state mismatch" << std::endl;
            return 1;
        }

        // 2. Verify MidiKeyboardDock::recordStepNote appends a MIDI note, advances playhead, and undoes cleanly
        {
            app.midiKeyboardDock.setEdit (app.edit.get());
            app.edit->getTransport().setPosition (tracktion::TimePosition::fromSeconds (0.0));

            auto countTrack0Notes = [&app]()
            {
                int total = 0;
                auto tracks = te::getAudioTracks (*app.edit);
                if (! tracks.isEmpty())
                    for (auto* c : tracks[0]->getClips())
                        if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                            total += mc->getSequence().getNumNotes();
                return total;
            };

            const int notesBefore = countTrack0Notes();

            if (! app.midiKeyboardDock.recordStepNote (64, 112, 0.5))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 recordStepNote returned false" << std::endl;
                return 1;
            }

            const double playheadAfter = app.edit->tempoSequence.toBeats (app.edit->getTransport().getPosition()).inBeats();
            const int notesAfter = countTrack0Notes();

            if (notesAfter != notesBefore + 1 || std::abs (playheadAfter - 0.5) > 0.01)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 recordStepNote did not add note or advance playhead" << std::endl;
                return 1;
            }

            app.edit->getUndoManager().undo();
            const int notesAfterUndo = countTrack0Notes();

            if (notesAfterUndo != notesBefore)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 Undo did not revert recordStepNote" << std::endl;
                return 1;
            }

            // 2b. Step Rec at a late playhead must start a fresh clip at the
            // playhead's bar — it must NOT stretch the seeded "Intro Riff"
            // clip (beats 0-4) across the timeline to hold the note.
            auto tracks = te::getAudioTracks (*app.edit);
            te::MidiClip* introClip = nullptr;

            if (! tracks.isEmpty())
                for (auto* c : tracks[0]->getClips())
                    if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                        if (mc->getName() == "Intro Riff")
                            introClip = mc;

            if (introClip == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 seeded Intro Riff clip missing" << std::endl;
                return 1;
            }

            const double introEndBefore = app.edit->tempoSequence.toBeats (introClip->getPosition().getEnd()).inBeats();
            const int notesBeforeLate = countTrack0Notes();

            app.edit->getTransport().setPosition (
                app.edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (32.0)));

            if (! app.midiKeyboardDock.recordStepNote (67, 100, 0.5))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 recordStepNote at beat 32 returned false" << std::endl;
                return 1;
            }

            const double introEndAfter = app.edit->tempoSequence.toBeats (introClip->getPosition().getEnd()).inBeats();
            bool freshClipAt32 = false;

            for (auto* c : tracks[0]->getClips())
                if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                    if (std::abs (app.edit->tempoSequence.toBeats (mc->getPosition().getStart()).inBeats() - 32.0) < 0.01)
                        freshClipAt32 = true;

            if (countTrack0Notes() != notesBeforeLate + 1
                || std::abs (introEndAfter - introEndBefore) > 0.01
                || ! freshClipAt32)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 Step Rec stretched the Intro Riff clip or did not start a fresh clip at beat 32" << std::endl;
                return 1;
            }

            app.edit->getUndoManager().undo();
        }

        // 3. Verify two-row header layout (zero overlap, statusLabel >= 180px) & keyboard dock height
        {
            app.setSize (1280, 820);
            app.resized();

            if (app.statusLabel.getWidth() < 180)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 statusLabel too narrow at 1280x820 ("
                          << app.statusLabel.getWidth() << "px)" << std::endl;
                return 1;
            }

            auto assertNoOverlap = [] (const std::vector<juce::Component*>& items, const char* rowLabel)
            {
                for (size_t i = 0; i < items.size(); ++i)
                {
                    if (items[i]->getWidth() <= 0 || items[i]->getHeight() <= 0)
                    {
                        std::cerr << "[SMOKE_TEST] FAIL: WO-010 zero-sized control in " << rowLabel << std::endl;
                        return false;
                    }

                    for (size_t j = i + 1; j < items.size(); ++j)
                    {
                        if (items[i]->getBounds().intersects (items[j]->getBounds()))
                        {
                            std::cerr << "[SMOKE_TEST] FAIL: WO-010 overlapping controls in " << rowLabel << std::endl;
                            return false;
                        }
                    }
                }

                return true;
            };

            const std::vector<juce::Component*> row1Items {
                &app.newButton, &app.openButton, &app.saveButton, &app.saveAsButton,
                &app.exportWavButton, &app.scanPluginsButton, &app.audioSettingsButton,
                &app.bgmToggleButton, &app.waveEditorToggleButton, &app.keysToggleButton,
                &app.rackToggleButton, &app.mixerToggleButton
            };

            const std::vector<juce::Component*> row2Items {
                &app.playButton, &app.stopButton, &app.recordButton,
                &app.loopToggleButton, &app.clickToggleButton, &app.countInButton,
                &app.autoToggleButton, &app.addTrackButton, &app.undoButton, &app.redoButton,
                &app.bpmSlider, &app.transportPosLabel, &app.statusLabel
            };

            if (! assertNoOverlap (row1Items, "Row 1") || ! assertNoOverlap (row2Items, "Row 2"))
                return 1;

            app.midiKeyboardDock.setVisible (true);
            app.resized();

            if (app.midiKeyboardDock.getHeight() != 74)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-010 midiKeyboardDock height mismatch, got "
                          << app.midiKeyboardDock.getHeight() << std::endl;
                return 1;
            }

            app.midiKeyboardDock.setVisible (false);
            app.resized();
        }

        // 4. Verify clickTrackEnabled, looping & loopRange persist across save/load round-trip
        const auto wo010TempDir = tempDir.getChildFile ("tcs_wo010_smoke");
        wo010TempDir.deleteRecursively();
        wo010TempDir.createDirectory();
        const auto wo010SessionFile = wo010TempDir.getChildFile ("ClickLoopRoundTrip.tracktionedit");

        app.currentEditFile = wo010SessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (wo010SessionFile)
            || ! app.edit->clickTrackEnabled.get()
            || ! app.edit->getTransport().looping.get()
            || std::abs (app.edit->tempoSequence.toBeats (app.edit->getTransport().getLoopRange().getEnd()).inBeats() - 16.0) > 0.05
            || ! app.clickToggleButton.getToggleState()
            || ! app.loopToggleButton.getToggleState())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-010 clickTrackEnabled or loop state did not persist across reload" << std::endl;
            return 1;
        }

        wo010TempDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-010 Metronome click, loop transport, Step Rec & two-row layout PASS" << std::endl;
    }

    //===========================================================================
    // WO-011 Regression: CvUtilityPlugin 1-to-4 Splitter, 3-Band EQ & Metering
    //===========================================================================
    {
        const auto wo011TempDir = tempDir.getChildFile ("tcs_wo011_smoke");
        wo011TempDir.deleteRecursively();
        wo011TempDir.createDirectory();

        // 1. Verify CvUtilityPlugin DSP (1-to-4 scalar capture, no ch-0 aliasing) & rack patching.
        // Scoped tightly so no Plugin::Ptr / RackType::Ptr outlives the reload below.
        {
            auto utilTestPlugin = app.edit->getPluginCache().createNewPlugin (CvUtilityPlugin::xmlTypeName, {});
            auto* util = dynamic_cast<CvUtilityPlugin*> (utilTestPlugin.get());

            if (util == nullptr || util->getNumOutputChannelsGivenInputs (1) != 4)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 CvUtilityPlugin instantiation or output count mismatch"
                          << std::endl;
                return 1;
            }

            juce::StringArray ins, outs;
            util->getChannelNames (&ins, &outs);

            if (ins.size() != 1 || ins[0] != "CV In"
                || outs.size() != 4 || outs[0] != "CV Out 1" || outs[3] != "CV Out 4")
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 CvUtilityPlugin pin names mismatch" << std::endl;
                return 1;
            }

            util->setOutputGain (0, -0.5f);
            util->setOutputOffset (0, 0.10f);
            util->setOutputGain (1, 1.0f);
            util->setOutputOffset (1, 0.30f);
            util->setOutputGain (2, 0.5f);
            util->setOutputOffset (2, -0.25f);
            util->setOutputGain (3, -1.0f);
            util->setOutputOffset (3, 0.0f);

            juce::AudioBuffer<float> cvBuf (4, 16);
            cvBuf.clear();

            for (int s = 0; s < 16; ++s)
                cvBuf.setSample (0, s, 0.5f);

            const te::PluginInitialisationInfo initInfo { tracktion::TimePosition::fromSeconds (0.0), 44100.0, 16 };
            util->initialise (initInfo);
            const te::PluginRenderContext rc (&cvBuf, 0, 16, nullptr, 0.0, {}, false, false, false, false);
            util->applyToBuffer (rc);
            util->deinitialise();

            // Input 0.5 -> Out0 = -0.15, Out1 = +0.80 (must NOT alias Out0!), Out2 = 0.00, Out3 = -0.50.
            if (std::abs (cvBuf.getSample (0, 0) - (-0.15f)) > 1e-4f
                || std::abs (cvBuf.getSample (1, 0) - 0.80f) > 1e-4f
                || std::abs (cvBuf.getSample (2, 0) - 0.0f) > 1e-4f
                || std::abs (cvBuf.getSample (3, 0) - (-0.50f)) > 1e-4f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 CvUtilityPlugin DSP aliasing or gain/offset mismatch"
                          << std::endl;
                return 1;
            }

            auto splitterRack = app.edit->getRackList().addNewRack();

            if (splitterRack == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 could not create RackType" << std::endl;
                return 1;
            }

            splitterRack->rackName = "WO-011 CV Splitter Rack";

            auto lfoP    = app.edit->getPluginCache().createNewPlugin (CvLfoPlugin::xmlTypeName, {});
            auto utilP   = app.edit->getPluginCache().createNewPlugin (CvUtilityPlugin::xmlTypeName, {});
            auto wobbleP = app.edit->getPluginCache().createNewPlugin (CvWobbleFilterPlugin::xmlTypeName, {});

            if (auto* rackUtil = dynamic_cast<CvUtilityPlugin*> (utilP.get()))
            {
                rackUtil->setOutputGain (0, -0.5f);
                rackUtil->setOutputOffset (1, 0.30f);
            }

            if (! splitterRack->addPlugin (lfoP, { 0.15f, 0.25f }, false)
                || ! splitterRack->addPlugin (utilP, { 0.45f, 0.25f }, false)
                || ! splitterRack->addPlugin (wobbleP, { 0.75f, 0.25f }, false))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 failed to add CV rack modules" << std::endl;
                return 1;
            }

            if (! splitterRack->addConnection (lfoP->itemID, 1, utilP->itemID, 1)
                || ! splitterRack->addConnection (utilP->itemID, 2, wobbleP->itemID, 1)
                || splitterRack->getConnections().size() != 2)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 failed to patch LFO -> Utility -> Wobble" << std::endl;
                return 1;
            }

            const int rackIdx = app.edit->getRackList().getTypes().size() - 1;
            app.modularRackComponent.syncFromEdit();
            app.modularRackComponent.selectRack (rackIdx);
            app.modularRackComponent.setSize (1000, 560);
            app.modularRackComponent.resized();

            if (! app.modularRackComponent.verifyNonOverlappingLayoutForSmokeTest())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 ModularRackComponent multi-pin layout check failed"
                          << std::endl;
                return 1;
            }
        }

        // 2. Verify mixer.set_eq RPC, daw.get_state readback, plugin chain ordering & Undo/Redo
        const auto eqResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":110,"method":"mixer.set_eq","params":{"track_index":0,"low_db":4.5,"mid_db":-3.0,"high_db":2.5}})"));

        if (! eqResp["error"].isVoid()
            || ! static_cast<bool> (eqResp["result"]["has_eq"])
            || std::abs (static_cast<double> (eqResp["result"]["low_db"]) - 4.5) > 0.05)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-011 mixer.set_eq RPC failed" << std::endl;
            return 1;
        }

        const auto stateAfterEq = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":111,"method":"daw.get_state","params":{}})"));

        const auto* tracksArr = stateAfterEq["result"]["tracks"].getArray();
        const auto t0State = (tracksArr != nullptr && ! tracksArr->isEmpty()) ? tracksArr->getUnchecked (0) : juce::var();

        if (! static_cast<bool> (t0State["has_eq"])
            || std::abs (static_cast<double> (t0State["eq_low_db"]) - 4.5) > 0.05
            || std::abs (static_cast<double> (t0State["eq_mid_db"]) - (-3.0)) > 0.05
            || std::abs (static_cast<double> (t0State["eq_high_db"]) - 2.5) > 0.05)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-011 daw.get_state did not report EQ bands" << std::endl;
            return 1;
        }

        // Verify EqualiserPlugin sits before VolumeAndPan / LevelMeter
        {
            auto tracks = te::getAudioTracks (*app.edit);
            int eqIdx = -1, volIdx = -1;
            const auto& list = tracks[0]->pluginList;

            for (int i = 0; i < list.size(); ++i)
            {
                if (dynamic_cast<te::EqualiserPlugin*> (list[i]) != nullptr)    eqIdx = i;
                if (dynamic_cast<te::VolumeAndPanPlugin*> (list[i]) != nullptr) volIdx = i;
            }

            if (eqIdx < 0 || (volIdx >= 0 && eqIdx >= volIdx))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 EqualiserPlugin insert order invalid" << std::endl;
                return 1;
            }
        }

        app.edit->getUndoManager().undo();
        const auto stateAfterUndo = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":112,"method":"daw.get_state","params":{}})"));
        const auto t0Undo = stateAfterUndo["result"]["tracks"][0];

        if (static_cast<bool> (t0Undo["has_eq"])
            || std::abs (static_cast<double> (t0Undo["eq_low_db"])) > 0.01)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-011 Undo did not revert mixer.set_eq" << std::endl;
            return 1;
        }

        app.edit->getUndoManager().redo();
        const auto stateAfterRedo = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":113,"method":"daw.get_state","params":{}})"));
        const auto t0Redo = stateAfterRedo["result"]["tracks"][0];

        if (! static_cast<bool> (t0Redo["has_eq"])
            || std::abs (static_cast<double> (t0Redo["eq_low_db"]) - 4.5) > 0.05
            || std::abs (static_cast<double> (t0Redo["eq_mid_db"]) - (-3.0)) > 0.05
            || std::abs (static_cast<double> (t0Redo["eq_high_db"]) - 2.5) > 0.05)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-011 Redo did not restore mixer.set_eq" << std::endl;
            return 1;
        }

        // 3. Save/load round-trip persistence for both EQ & CvUtilityPlugin + read-only meter polling
        const auto wo011SessionFile = wo011TempDir.getChildFile ("EqCvRoundTrip.tracktionedit");
        app.currentEditFile = wo011SessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (wo011SessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-011 save/load round-trip failed" << std::endl;
            return 1;
        }

        const auto stateReloaded = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":114,"method":"daw.get_state","params":{}})"));
        const auto t0Reloaded = stateReloaded["result"]["tracks"][0];

        if (! static_cast<bool> (t0Reloaded["has_eq"])
            || std::abs (static_cast<double> (t0Reloaded["eq_low_db"]) - 4.5) > 0.05
            || std::abs (static_cast<double> (t0Reloaded["eq_mid_db"]) - (-3.0)) > 0.05
            || std::abs (static_cast<double> (t0Reloaded["eq_high_db"]) - 2.5) > 0.05)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-011 EQ settings did not persist after reload" << std::endl;
            return 1;
        }

        {
            te::RackType* persistedSplitterRack = nullptr;
            for (auto* r : app.edit->getRackList().getTypes())
                if (r->rackName.get() == "WO-011 CV Splitter Rack")
                    persistedSplitterRack = r;

            CvUtilityPlugin* persistedUtil = nullptr;
            if (persistedSplitterRack != nullptr)
                for (auto* p : persistedSplitterRack->getPlugins())
                    if (auto* u = dynamic_cast<CvUtilityPlugin*> (p))
                        persistedUtil = u;

            if (persistedSplitterRack == nullptr
                || persistedSplitterRack->getConnections().size() != 2
                || persistedUtil == nullptr
                || std::abs (persistedUtil->getOutputGain (0) - (-0.5f)) > 1e-4f
                || std::abs (persistedUtil->getOutputOffset (1) - 0.30f) > 1e-4f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-011 CvUtilityPlugin state or rack connections did not persist"
                          << std::endl;
                return 1;
            }
        }

        app.mixerComponent.refreshMeters();
        if (app.hasUnsavedChanges())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-011 read-only mixer meter polling dirtied clean session" << std::endl;
            return 1;
        }

        wo011TempDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-011 CvUtilityPlugin 1-to-4 splitter, 3-band EQ RPC/Undo/persistence & meter polling PASS"
                  << std::endl;
    }

    //===========================================================================
    // WO-012 Regression: Live Audio/MIDI Recording, Track Arming & Count-In
    //===========================================================================
    {
        // 1. Verify recording.list_inputs, mixer.set_track (armed/input_device), and count_in_bars RPC
        const auto inputsResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":120,"method":"recording.list_inputs","params":{}})"));

        if (! inputsResp["result"].hasProperty ("audio_inputs")
            || ! inputsResp["result"].hasProperty ("midi_inputs"))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-012 recording.list_inputs response malformed" << std::endl;
            return 1;
        }

        app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":121,"method":"mixer.set_track","params":{"track_index":0,"armed":true,"input_device":"Virtual MIDI"}})");
        app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":122,"method":"transport.control","params":{"count_in_bars":1}})");

        const auto stateBefore = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":123,"method":"daw.get_state","params":{}})"));
        const auto* recTracksBefore = stateBefore["result"]["tracks"].getArray();

        if (static_cast<int> (stateBefore["result"]["count_in_bars"]) != 1
            || recTracksBefore == nullptr || recTracksBefore->isEmpty()
            || ! static_cast<bool> ((*recTracksBefore)[0]["armed"])
            || (*recTracksBefore)[0]["input_device"].toString() != "Virtual MIDI")
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-012 arm/input/count-in state mismatch" << std::endl;
            return 1;
        }

        // 2. Verify live virtual MIDI capture commits notes inside one Undo transaction & reverts cleanly
        {
            const int clipsBefore = te::getAudioTracks (*app.edit)[0]->getClips().size();
            app.recordingManager.startRecording (true);
            app.recordingManager.captureLiveMidiMessage (juce::MidiMessage::noteOn (1, 60, 0.85f), 8.0);
            app.recordingManager.captureLiveMidiMessage (juce::MidiMessage::noteOff (1, 60), 9.0);
            app.recordingManager.captureLiveMidiMessage (juce::MidiMessage::noteOn (1, 67, 0.90f), 9.0);
            app.recordingManager.captureLiveMidiMessage (juce::MidiMessage::noteOff (1, 67), 10.0);
            const int recordedCount = app.recordingManager.stopRecording();

            if (recordedCount != 2 || te::getAudioTracks (*app.edit)[0]->getClips().size() != clipsBefore + 1)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-012 virtual MIDI recording did not commit 2 notes" << std::endl;
                return 1;
            }

            app.edit->getUndoManager().undo();

            if (te::getAudioTracks (*app.edit)[0]->getClips().size() != clipsBefore)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-012 Undo did not revert recorded MIDI clip" << std::endl;
                return 1;
            }
        }

        // 3. Verify non-destructive audio take commit writes non-silent stereo WAV, Undo/Redo, & persists
        const auto recTempDir = tempDir.getChildFile ("tcs_wo012_rec_smoke");
        recTempDir.deleteRecursively();
        recTempDir.createDirectory();
        const auto takesDir = recTempDir.getChildFile ("Takes");

        juce::File committedTakeWav;
        {
            constexpr int sr = 44100;
            juce::AudioBuffer<float> micBuf (2, sr / 2); // 0.5s stereo take

            for (int i = 0; i < micBuf.getNumSamples(); ++i)
            {
                const float s = static_cast<float> (
                    0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * i / sr));
                micBuf.setSample (0, i, s);
                micBuf.setSample (1, i, s);
            }

            auto* track0 = te::getAudioTracks (*app.edit)[0];
            const int clipsBeforeTake = track0->getClips().size();

            if (! RecordingManager::commitRecordedAudioTake (*app.edit, *track0, micBuf, sr, 0.0, takesDir, &committedTakeWav)
                || ! committedTakeWav.existsAsFile()
                || track0->getClips().size() != clipsBeforeTake + 1)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-012 commitRecordedAudioTake failed" << std::endl;
                return 1;
            }

            {
                juce::WavAudioFormat wavFmt;
                std::unique_ptr<juce::AudioFormatReader> reader (
                    wavFmt.createReaderFor (committedTakeWav.createInputStream().release(), true));

                if (reader == nullptr
                    || reader->numChannels != 2
                    || std::abs (reader->sampleRate - 44100.0) > 0.1
                    || reader->lengthInSamples != sr / 2)
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-012 committed WAV format mismatch" << std::endl;
                    return 1;
                }

                juce::AudioBuffer<float> readBack (2, 1024);
                reader->read (&readBack, 0, 1024, 0, true, true);

                if (readBack.getMagnitude (0, 1024) < 0.1f)
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-012 committed WAV is unexpectedly silent" << std::endl;
                    return 1;
                }
            }

            app.edit->getUndoManager().undo();
            if (te::getAudioTracks (*app.edit)[0]->getClips().size() != clipsBeforeTake)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-012 Undo did not remove committed audio take" << std::endl;
                return 1;
            }

            app.edit->getUndoManager().redo();
            if (te::getAudioTracks (*app.edit)[0]->getClips().size() != clipsBeforeTake + 1)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-012 Redo did not restore committed audio take" << std::endl;
                return 1;
            }
        }

        // 4. Verify two-row header still leaves statusLabel >= 180px at 1280x820 with Rec + Count-In
        app.setSize (1280, 820);
        app.resized();

        if (app.statusLabel.getWidth() < 180)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-012 statusLabel narrowed below 180px ("
                      << app.statusLabel.getWidth() << "px)" << std::endl;
            return 1;
        }

        // 5. Save/reload persistence check for armed state, count-in, and committed audio take
        const auto recSessionFile = recTempDir.getChildFile ("RecordingRoundTrip.tracktionedit");
        app.currentEditFile = recSessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (recSessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-012 session reload failed" << std::endl;
            return 1;
        }

        bool foundTakeAfterReload = false;
        {
            auto reloadedTracks = te::getAudioTracks (*app.edit);

            if (reloadedTracks.isEmpty()
                || ! RecordingManager::isTrackArmed (*reloadedTracks[0])
                || RecordingManager::getTrackInputName (*reloadedTracks[0]) != "Virtual MIDI"
                || RecordingManager::getCountInBars (*app.edit) != 1)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-012 arm/input/count-in state did not persist" << std::endl;
                return 1;
            }

            for (auto* c : reloadedTracks[0]->getClips())
                if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                    if (wc->getSourceFileReference().getFile() == committedTakeWav)
                        foundTakeAfterReload = true;
        }

        if (! foundTakeAfterReload)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-012 recorded audio take clip did not persist across reload"
                      << std::endl;
            return 1;
        }

        recTempDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-012 Live audio/MIDI recording, arming & count-in PASS" << std::endl;
    }

    //===========================================================================
    // WO-013 Regression: Automation Curves, Aux FX Send Bus, Zoom & Theme
    //===========================================================================
    {
        // 1. Write 3 volume automation points via automation.set_points, read back, Undo & Redo
        const auto setAutoResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":130,"method":"automation.set_points","params":{"track_index":0,"parameter":"volume","clear_existing":true,"points":[{"beat":0.0,"value":-12.0,"curve":0.0},{"beat":4.0,"value":0.0,"curve":0.0},{"beat":8.0,"value":-6.0,"curve":0.0}]}})"));

        if (static_cast<int> (setAutoResp["result"]["point_count"]) != 3)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 automation.set_points did not write 3 points" << std::endl;
            return 1;
        }

        const auto getAutoResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":131,"method":"automation.get_points","params":{"track_index":0,"parameter":"volume"}})"));
        const auto* autoPts = getAutoResp["result"]["points"].getArray();

        if (autoPts == nullptr || autoPts->size() != 3)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 automation.get_points expected 3 points" << std::endl;
            return 1;
        }

        app.edit->getUndoManager().undo();
        const auto getAfterUndo = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":132,"method":"automation.get_points","params":{"track_index":0,"parameter":"volume"}})"));

        if (getAfterUndo["result"]["points"].getArray() == nullptr
            || getAfterUndo["result"]["points"].getArray()->size() != 0)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 Undo did not clear automation points" << std::endl;
            return 1;
        }

        app.edit->getUndoManager().redo();

        // 2. Configure Aux FX Send bus (Reverb) on Track 0 at -9.0 dB, wet 0.45
        const auto fxResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":133,"method":"mixer.set_fx_send","params":{"track_index":0,"bus_index":0,"send_db":-9.0,"effect":"reverb","wet_level":0.45}})"));
        const int fxReturnTrackIdx = static_cast<int> (fxResp["result"]["return_track_index"]);

        if (fxReturnTrackIdx < 0 || std::abs (static_cast<double> (fxResp["result"]["send_db"]) - (-9.0)) > 0.2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 mixer.set_fx_send did not configure bus 0 at -9.0 dB" << std::endl;
            return 1;
        }

        // 3. Verify Arrangement + Piano Roll zoom & clamping, plus 1280x820 layout with Auto toggle
        const float arrBaseZoom = app.arrangementView.getPixelsPerBeat();
        app.arrangementView.zoomIn();
        if (app.arrangementView.getPixelsPerBeat() <= arrBaseZoom)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 ArrangementView zoomIn did not increase pixelsPerBeat"
                      << std::endl;
            return 1;
        }
        app.arrangementView.zoomOut();
        app.arrangementView.setPixelsPerBeat (1.0f);
        if (std::abs (app.arrangementView.getPixelsPerBeat() - 16.0f) > 0.01f)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 ArrangementView min zoom clamp failed" << std::endl;
            return 1;
        }
        app.arrangementView.setPixelsPerBeat (arrBaseZoom);

        const float prBaseZoom = app.pianoRoll.getPixelsPerBeat();
        app.pianoRoll.zoomIn();
        if (app.pianoRoll.getPixelsPerBeat() <= prBaseZoom)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 PianoRollComponent zoomIn did not increase pixelsPerBeat"
                      << std::endl;
            return 1;
        }
        app.pianoRoll.zoomOut();
        app.pianoRoll.setPixelsPerBeat (999.0f);
        if (std::abs (app.pianoRoll.getPixelsPerBeat() - 160.0f) > 0.01f)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 PianoRollComponent max zoom clamp failed" << std::endl;
            return 1;
        }
        app.pianoRoll.setPixelsPerBeat (prBaseZoom);

        app.setSize (1280, 820);
        app.resized();

        if (app.statusLabel.getWidth() < 180)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 statusLabel too narrow at 1280x820 ("
                      << app.statusLabel.getWidth() << "px)" << std::endl;
            return 1;
        }

        // 4. Verify automation points and AuxSend/AuxReturn+Reverb survive save/reload
        const auto wo013TempDir = tempDir.getChildFile ("tcs_wo013_smoke");
        wo013TempDir.deleteRecursively();
        wo013TempDir.createDirectory();
        const auto wo013SessionFile = wo013TempDir.getChildFile ("AutoFxRoundTrip.tracktionedit");

        app.currentEditFile = wo013SessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (wo013SessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 session reload failed" << std::endl;
            return 1;
        }

        const auto reloadedAuto = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":134,"method":"automation.get_points","params":{"track_index":0,"parameter":"volume"}})"));

        bool foundAuxSend = false;
        bool foundAuxReturn = false;
        bool foundWetReverb = false;

        {
            auto reloadedTracks = te::getAudioTracks (*app.edit);

            for (auto* p : reloadedTracks[0]->pluginList)
                if (auto* s = dynamic_cast<te::AuxSendPlugin*> (p))
                    if (std::abs (s->getGainDb() - (-9.0f)) <= 0.2f)
                        foundAuxSend = true;

            if (juce::isPositiveAndBelow (fxReturnTrackIdx, reloadedTracks.size()))
            {
                for (auto* p : reloadedTracks[fxReturnTrackIdx]->pluginList)
                {
                    if (dynamic_cast<te::AuxReturnPlugin*> (p) != nullptr)
                        foundAuxReturn = true;
                    if (auto* r = dynamic_cast<te::ReverbPlugin*> (p))
                        if (std::abs (r->wetValue.get() - 0.45f) <= 0.02f)
                            foundWetReverb = true;
                }
            }
        }

        const auto* reloadedAutoPts = reloadedAuto["result"]["points"].getArray();

        if (reloadedAutoPts == nullptr || reloadedAutoPts->size() != 3
            || ! foundAuxSend || ! foundAuxReturn || ! foundWetReverb)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-013 automation curve or Aux Send/Return FX did not persist"
                      << std::endl;
            return 1;
        }

        wo013TempDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-013 Automation curves, Aux FX bus, zoom & studio theme PASS" << std::endl;
    }

    //===========================================================================
    // WO-014 Regression Test: YIN Audio-to-MIDI Pitch Transcription, Undo & Reload
    //===========================================================================
    {
        const auto wo014TempDir = tempDir.getChildFile ("tcs_wo014_smoke");
        wo014TempDir.deleteRecursively();
        wo014TempDir.createDirectory();
        const auto melodyWavFile = wo014TempDir.getChildFile ("TestMelody_ACEA.wav");

        // 1. Synthesize a 4-note monophonic melody (A4=69, C5=72, E5=76, A5=81), 1 beat each at session BPM
        const double sessionBpm  = app.edit->tempoSequence.getTempo (0)->getBpm();
        const double secPerBeat  = 60.0 / std::max (40.0, sessionBpm);
        constexpr int sr         = 44100;
        const int samplesPerNote = static_cast<int> (std::round (secPerBeat * sr));
        const int gapSamples     = static_cast<int> (std::round (0.02 * sr)); // 20ms articulation gap
        const int expectedMidi[4] = { 69, 72, 76, 81 };

        {
            juce::AudioBuffer<float> melBuf (2, samplesPerNote * 4);
            melBuf.clear();

            for (int n = 0; n < 4; ++n)
            {
                const double freq = 440.0 * std::pow (2.0, (static_cast<double> (expectedMidi[n]) - 69.0) / 12.0);
                const int startS = n * samplesPerNote;
                const int activeS = std::max (1, samplesPerNote - gapSamples);

                for (int i = 0; i < activeS; ++i)
                {
                    const float env = std::min (1.0f, std::min (static_cast<float> (i) / 200.0f,
                                                                static_cast<float> (activeS - 1 - i) / 200.0f));
                    const float sample = static_cast<float> (
                        0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr)) * env;
                    melBuf.setSample (0, startS + i, sample);
                    melBuf.setSample (1, startS + i, sample);
                }
            }

            auto fs = std::make_unique<juce::FileOutputStream> (melodyWavFile);
            fs->setPosition (0);
            fs->truncate();
            std::unique_ptr<juce::OutputStream> os (fs.release());
            juce::WavAudioFormat fmt;
            auto writer = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (sr)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (16));

            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (melBuf, 0, melBuf.getNumSamples()))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-014 could not write TestMelody_ACEA.wav" << std::endl;
                return 1;
            }
        }

        // 2. Insert melody WAV onto a dedicated source track so clip_index 0 is deterministic
        int sourceTrackIdx = -1;
        {
            auto tracks = te::getAudioTracks (*app.edit);
            app.edit->ensureNumberOfAudioTracks (tracks.size() + 1);
            tracks = te::getAudioTracks (*app.edit);
            sourceTrackIdx = tracks.size() - 1;
            auto* srcTrack = tracks[sourceTrackIdx];
            srcTrack->setName ("Vocal Sketch");

            const tracktion::TimeRange r {
                tracktion::TimePosition::fromSeconds (0.0),
                tracktion::TimePosition::fromSeconds (secPerBeat * 4.0)
            };

            if (auto wc = srcTrack->insertWaveClip ("MelodyTake", melodyWavFile, te::ClipPosition { r, {} }, false))
            {
                wc->getSourceFileReference().setToFile (
                    melodyWavFile, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
            }
        }

        // 3. Transcribe via audio.transcribe_to_midi RPC into a new 4OSC track
        const auto transResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            juce::String (R"({"jsonrpc":"2.0","id":140,"method":"audio.transcribe_to_midi","params":{"track_index":)")
            + juce::String (sourceTrackIdx)
            + R"(,"clip_index":0,"target_track_index":-1,"quantize_beats":0.25,"min_note_beats":0.25}})"));

        const int transcribedTrackIdx = static_cast<int> (transResp["result"]["target_track_index"]);
        const auto* transNotes = transResp["result"]["notes"].getArray();

        if (transcribedTrackIdx < 0 || transNotes == nullptr || transNotes->size() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-014 expected 4 transcribed MIDI notes, got "
                      << (transNotes != nullptr ? transNotes->size() : -1) << std::endl;
            return 1;
        }

        for (int i = 0; i < 4; ++i)
        {
            const int gotPitch = static_cast<int> ((*transNotes)[i]["pitch"]);

            if (gotPitch != expectedMidi[i])
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-014 note " << i << " pitch mismatch: expected "
                          << expectedMidi[i] << ", got " << gotPitch << std::endl;
                return 1;
            }
        }

        // 4. Verify Undo removes transcribed MIDI notes and Redo restores them
        app.edit->getUndoManager().undo();
        {
            auto tracks = te::getAudioTracks (*app.edit);

            if (juce::isPositiveAndBelow (transcribedTrackIdx, tracks.size()))
            {
                int remaining = 0;

                for (auto* c : tracks[transcribedTrackIdx]->getClips())
                    if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                        remaining += mc->getSequence().getNumNotes();

                if (remaining != 0)
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-014 Undo did not clear transcribed MIDI notes" << std::endl;
                    return 1;
                }
            }
        }

        app.edit->getUndoManager().redo();

        // 5. Verify transcribed MidiClip persists across save/load round-trip
        const auto wo014SessionFile = wo014TempDir.getChildFile ("TranscribeRoundTrip.tracktionedit");
        app.currentEditFile = wo014SessionFile;
        app.sessionUntitled = false;
        app.saveSession();

        if (! app.loadSessionFromFileSafely (wo014SessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-014 session reload failed" << std::endl;
            return 1;
        }

        const auto verifyNotesResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            juce::String (R"({"jsonrpc":"2.0","id":141,"method":"midi.get_notes","params":{"track_index":)")
            + juce::String (transcribedTrackIdx) + R"(,"start_beat":0.0,"end_beat":8.0}})"));
        const auto* reloadedTransNotes = verifyNotesResp["result"]["notes"].getArray();

        if (reloadedTransNotes == nullptr || reloadedTransNotes->size() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-014 transcribed MIDI notes did not persist across reload" << std::endl;
            return 1;
        }

        wo014TempDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-014 YIN Audio-to-MIDI transcription, Undo/Redo & persistence PASS" << std::endl;
    }

    //===========================================================================
    // WO-015 Regression Test: Plugin Windows, Timeline Vertical Scroll & Wave-to-MIDI UI
    //===========================================================================
    {
        // 1. StudioUIBehaviour plugin window open/close lifecycle
        {
            auto wo015Tracks = te::getAudioTracks (*app.edit);
            if (wo015Tracks.isEmpty())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 expected tracks in Edit" << std::endl;
                return 1;
            }

            if (wo015Tracks[0]->pluginList.getPlugins().isEmpty())
            {
                if (auto synth = app.edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
                    wo015Tracks[0]->pluginList.insertPlugin (synth, 0, nullptr);
            }

            auto* testPlugin = wo015Tracks[0]->pluginList.getPlugins().getFirst().get();
            if (testPlugin == nullptr || testPlugin->windowState == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 missing testable plugin/windowState" << std::endl;
                return 1;
            }

            testPlugin->showWindowExplicitly();
            if (! testPlugin->windowState->isWindowShowing())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 StudioUIBehaviour failed to open plugin window" << std::endl;
                return 1;
            }

            testPlugin->windowState->closeWindowExplicitly();
            if (testPlugin->windowState->isWindowShowing())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 closeWindowExplicitly did not close plugin window" << std::endl;
                return 1;
            }
        }

        // 2. ArrangementView vertical track scroll & bounds clamping
        {
            auto wo015Tracks = te::getAudioTracks (*app.edit);
            if (wo015Tracks.size() < 5)
            {
                app.edit->ensureNumberOfAudioTracks (5);
                wo015Tracks = te::getAudioTracks (*app.edit);
            }

            app.arrangementView.setSize (800, 180);
            app.arrangementView.scrollToTrack (wo015Tracks.size() - 1);
            if (app.arrangementView.getVerticalScrollOffsetPx() <= 0)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 ArrangementView did not scroll vertically to last track"
                          << std::endl;
                return 1;
            }

            app.arrangementView.scrollToTrack (0);
            if (app.arrangementView.getVerticalScrollOffsetPx() != 0)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 ArrangementView did not scroll back to top track"
                          << std::endl;
                return 1;
            }
        }

        // 3. Wave Edit -> To MIDI callback auto-switches middle pane to Piano Roll
        {
            app.setSize (1280, 820);
            app.waveEditorToggleButton.setToggleState (true, juce::dontSendNotification);
            app.resized();

            if (! app.waveformEditorComponent.isVisible() || app.pianoRoll.isVisible())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 Wave Edit toggle did not show WaveformEditorComponent"
                          << std::endl;
                return 1;
            }

            if (! app.waveformEditorComponent.onTranscribedToMidi)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 onTranscribedToMidi callback not wired" << std::endl;
                return 1;
            }

            app.waveformEditorComponent.onTranscribedToMidi (0, {}, 4);

            if (app.waveEditorToggleButton.getToggleState()
                || app.waveformEditorComponent.isVisible()
                || ! app.pianoRoll.isVisible())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-015 Wave-to-MIDI callback did not switch back to Piano Roll"
                          << std::endl;
                return 1;
            }
        }

        std::cout << "[SMOKE_TEST] WO-015 Plugin windows, timeline vertical scroll & Wave-to-MIDI UI polish PASS"
                  << std::endl;
    }

    //===========================================================================
    // WO-016 Regression Test: CV SVF Filter DSP, Direction Pins, Transpose & Undo
    //===========================================================================
    {
        const auto wo016TempDir = tempDir.getChildFile ("tcs_wo016_smoke");
        wo016TempDir.deleteRecursively();
        wo016TempDir.createDirectory();

        // 1. Fresh-session Undo history starts empty
        app.createNewSession (wo016TempDir.getChildFile ("FreshUndo.tracktionedit"), true, false);
        if (app.edit->getUndoManager().canUndo())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-016 fresh session started with non-empty undo history" << std::endl;
            return 1;
        }

        // 2. CvWobbleFilterPlugin: 3 inputs (L, R, CV In), real CV-modulated SVF attenuation & CV clear
        {
            auto wobbleTestP = app.edit->getPluginCache().createNewPlugin (CvWobbleFilterPlugin::xmlTypeName, {});
            auto* wobble = dynamic_cast<CvWobbleFilterPlugin*> (wobbleTestP.get());

            juce::StringArray ins, outs;
            if (wobble != nullptr)
                wobble->getChannelNames (&ins, &outs);

            if (wobble == nullptr || ins.size() != 3 || outs.size() != 2 || ins[2] != "CV In")
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 CvWobbleFilterPlugin channel pin layout mismatch" << std::endl;
                return 1;
            }

            constexpr int blockN = 256;
            constexpr double sr = 44100.0;
            auto makeTestBlock = [] (float cvVal)
            {
                juce::AudioBuffer<float> buf (3, blockN);
                for (int i = 0; i < blockN; ++i)
                {
                    // 1500 Hz sits inside the high-CV passband (~2545 Hz) and
                    // ~2.2 octaves above the low-CV cutoff (~318 Hz) at default depth 0.5.
                    const float sig = static_cast<float> (
                        0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1500.0 * i / sr));
                    buf.setSample (0, i, sig);
                    buf.setSample (1, i, sig);
                    buf.setSample (2, i, cvVal);
                }
                return buf;
            };

            auto bufLow  = makeTestBlock (-1.0f);
            auto bufHigh = makeTestBlock (+1.0f);

            const te::PluginInitialisationInfo initInfo { tracktion::TimePosition::fromSeconds (0.0), sr, blockN };
            wobble->initialise (initInfo);
            wobble->applyToBuffer (te::PluginRenderContext (&bufLow, 0, blockN, nullptr, 0.0, {}, false, false, false, false));
            wobble->deinitialise();

            wobble->initialise (initInfo);
            wobble->applyToBuffer (te::PluginRenderContext (&bufHigh, 0, blockN, nullptr, 0.0, {}, false, false, false, false));
            wobble->deinitialise();

            const float lowRms  = bufLow.getRMSLevel (0, 0, blockN);
            const float highRms = bufHigh.getRMSLevel (0, 0, blockN);

            if (highRms <= lowRms * 1.25f || std::abs (bufHigh.getSample (2, 0)) > 1e-6f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 CvWobbleFilterPlugin CV-modulated SVF filter check failed"
                          << std::endl;
                return 1;
            }
        }

        // 3. ModularRackComponent direction-aware input/output pins & Rack I/O rails
        {
            auto wo16Rack = app.edit->getRackList().addNewRack();
            wo16Rack->rackName = "WO-016 CV Rack";

            auto lfoP    = app.edit->getPluginCache().createNewPlugin (CvLfoPlugin::xmlTypeName, {});
            auto wobbleP = app.edit->getPluginCache().createNewPlugin (CvWobbleFilterPlugin::xmlTypeName, {});

            const auto lfoID    = lfoP->itemID;
            const auto wobbleID = wobbleP->itemID;

            if (! wo16Rack->addPlugin (lfoP, { 0.15f, 0.25f }, false)
                || ! wo16Rack->addPlugin (wobbleP, { 0.55f, 0.25f }, false)
                || ! wo16Rack->addConnection ({}, 1, wobbleID, 1)
                || ! wo16Rack->addConnection (lfoID, 1, wobbleID, 3)
                || ! wo16Rack->addConnection (wobbleID, 1, {}, 1))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 could not wire Rack I/O & LFO -> Wobble CV In" << std::endl;
                return 1;
            }

            const int rIdx = app.edit->getRackList().getTypes().size() - 1;
            app.modularRackComponent.syncFromEdit();
            app.modularRackComponent.selectRack (rIdx);
            app.modularRackComponent.setSize (1000, 560);
            app.modularRackComponent.resized();

            const auto inPos  = app.modularRackComponent.getPinCentreInRack (wobbleID, 1, true);
            const auto outPos = app.modularRackComponent.getPinCentreInRack (wobbleID, 1, false);

            if (inPos.x >= outPos.x || ! app.modularRackComponent.verifyNonOverlappingLayoutForSmokeTest())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 direction-aware pin geometry invalid" << std::endl;
                return 1;
            }
        }

        // 4. Pitch-shift-aware AudioToMidiTranscriber (A4 + 3 semitones -> C5 = 72)
        const auto pitchWav = wo016TempDir.getChildFile ("PitchShiftA4.wav");
        const double curBpm = app.edit->tempoSequence.getTempo (0)->getBpm();
        const double beatSec = 60.0 / std::max (40.0, curBpm);
        constexpr int sr = 44100;
        const int nSamples = static_cast<int> (std::round (beatSec * 2.0 * sr));

        {
            juce::AudioBuffer<float> a4Buf (2, nSamples);
            for (int i = 0; i < nSamples; ++i)
            {
                const float s = static_cast<float> (
                    0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr));
                a4Buf.setSample (0, i, s);
                a4Buf.setSample (1, i, s);
            }

            auto fs = std::make_unique<juce::FileOutputStream> (pitchWav);
            fs->setPosition (0);
            fs->truncate();
            std::unique_ptr<juce::OutputStream> os (fs.release());
            juce::WavAudioFormat fmt;
            auto writer = fmt.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (sr)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (16));
            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (a4Buf, 0, nSamples))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 could not write PitchShiftA4.wav" << std::endl;
                return 1;
            }
        }

        {
            auto tracks = te::getAudioTracks (*app.edit);
            const tracktion::TimeRange r {
                tracktion::TimePosition::fromSeconds (0.0),
                tracktion::TimePosition::fromSeconds (beatSec * 2.0)
            };

            if (auto wc = tracks[1]->insertWaveClip ("TransposedTake", pitchWav, te::ClipPosition { r, {} }, false))
            {
                wc->getSourceFileReference().setToFile (
                    pitchWav, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
                wc->setPitchChange (3.0f);
            }
        }

        const auto transShiftResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":160,"method":"audio.transcribe_to_midi","params":{"track_index":1,"clip_index":0,"target_track_index":-1,"quantize_beats":0.25,"min_note_beats":0.25}})"));
        const auto* shiftNotes = transShiftResp["result"]["notes"].getArray();

        if (shiftNotes == nullptr || shiftNotes->isEmpty() || static_cast<int> ((*shiftNotes)[0]["pitch"]) != 72)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-016 transposed clip transcription expected pitch 72, got "
                      << (shiftNotes != nullptr && ! shiftNotes->isEmpty()
                              ? static_cast<int> ((*shiftNotes)[0]["pitch"])
                              : -1)
                      << std::endl;
            return 1;
        }

        wo016TempDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-016 CV SVF filter, direction pins & transposed transcription PASS" << std::endl;
    }

    //===========================================================================
    // WO-017 Regression Test: Factory Presets, .trkpreset, Sampler & Multi-Machine
    //===========================================================================
    {
        const auto wo017TempDir = tempDir.getChildFile ("tcs_wo017_smoke");
        wo017TempDir.deleteRecursively();
        wo017TempDir.createDirectory();

        // 1. Fresh session + 4OSC Factory Presets + GenericTracktionPluginEditor + .trkpreset round-trip
        app.createNewSession (wo017TempDir.getChildFile ("Wo017Presets.tracktionedit"), true, false);
        auto tracks = te::getAudioTracks (*app.edit);
        te::FourOscPlugin* synth = nullptr;

        if (! tracks.isEmpty())
            synth = tracks[0]->pluginList.findFirstPluginOfType<te::FourOscPlugin>();

        if (synth == nullptr || PluginPresetManager::getFactoryPresetNames (*synth).size() < 8)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 FourOscPlugin or factory preset bank missing" << std::endl;
            return 1;
        }

        if (! PluginPresetManager::applyFactoryPreset (*synth, "Reese Neuro Bass")
            || synth->voiceModeValue.get() != 1
            || synth->oscParams[0]->waveShapeValue.get() != 3
            || synth->oscParams[0]->voicesValue.get() != 4
            || ! synth->distortionOnValue.get())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 Reese Neuro Bass preset properties mismatch" << std::endl;
            return 1;
        }

        {
            GenericTracktionPluginEditor editor (*synth);
            if (editor.getNumPresetItems() < 8)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-017 GenericTracktionPluginEditor preset combo empty" << std::endl;
                return 1;
            }
        }

        const auto origId = synth->itemID;
        const auto presetFile = wo017TempDir.getChildFile ("ReeseSaved.trkpreset");

        if (! PluginPresetManager::savePresetToFile (*synth, presetFile) || ! presetFile.existsAsFile())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 savePresetToFile failed" << std::endl;
            return 1;
        }

        if (! PluginPresetManager::applyFactoryPreset (*synth, "Init Sine")
            || synth->oscParams[0]->waveShapeValue.get() != 1
            || synth->distortionOnValue.get())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 Init Sine reset failed" << std::endl;
            return 1;
        }

        if (! PluginPresetManager::loadPresetFromFile (*synth, presetFile)
            || synth->itemID != origId
            || synth->oscParams[0]->waveShapeValue.get() != 3
            || ! synth->distortionOnValue.get())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 loadPresetFromFile restore or ID preservation failed" << std::endl;
            return 1;
        }

        // 2. MCP rack.add_machine ('sampler', 'compressor') + SamplerPlugin 808 kit audio render + plugin.set_preset
        const auto prevTakesCb = app.ipcServer.getTakesDirectoryCallback;
        app.ipcServer.getTakesDirectoryCallback = [wo017TempDir]
        {
            return wo017TempDir.getChildFile ("Takes");
        };

        const auto addSamplerResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":170,"method":"rack.add_machine","params":{"rack_index":0,"machine_type":"sampler"}})"));
        const auto addCompResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":171,"method":"rack.add_machine","params":{"rack_index":0,"machine_type":"compressor"}})"));

        app.ipcServer.getTakesDirectoryCallback = prevTakesCb;

        const juce::int64 samplerId = static_cast<juce::int64> (addSamplerResp["result"]["plugin_id"]);
        const juce::int64 compId    = static_cast<juce::int64> (addCompResp["result"]["plugin_id"]);

        if (samplerId <= 0 || compId <= 0)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 MCP rack.add_machine returned invalid plugin_id" << std::endl;
            return 1;
        }

        auto rack0 = app.edit->getRackList().getRackType (0);
        auto* sampler = (rack0 != nullptr)
                            ? dynamic_cast<te::SamplerPlugin*> (rack0->getPluginForID (
                                  tracktion::EditItemID::fromRawID (static_cast<uint64_t> (samplerId))))
                            : nullptr;

        if (sampler == nullptr || sampler->getNumSounds() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 SamplerPlugin starter kit expected 4 sounds" << std::endl;
            return 1;
        }

        constexpr int smpBlock = 512;
        constexpr double smpSr = 44100.0;
        juce::AudioBuffer<float> smpOut (2, smpBlock);
        smpOut.clear();
        te::MidiMessageArray midiArr;
        midiArr.addMidiMessage (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (115)),
                                0.0,
                                te::MPESourceID{});

        sampler->initialise (te::PluginInitialisationInfo { tracktion::TimePosition::fromSeconds (0.0), smpSr, smpBlock });
        sampler->applyToBuffer (te::PluginRenderContext (&smpOut, 0, smpBlock, &midiArr, 0.0, {}, false, false, false, false));
        sampler->deinitialise();

        if (smpOut.getRMSLevel (0, 0, smpBlock) <= 0.005f)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 SamplerPlugin 808 Kick rendered silence" << std::endl;
            return 1;
        }

        const auto setPresetResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":172,"method":"plugin.set_preset","params":{"track_index":0,"plugin_index":0,"preset_name":"Supersaw Anthem Lead"}})"));

        if (setPresetResp["result"]["applied_preset"].toString() != "Supersaw Anthem Lead"
            || synth->oscParams[0]->voicesValue.get() != 6
            || ! synth->reverbOnValue.get())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-017 MCP plugin.set_preset did not apply Supersaw Anthem Lead" << std::endl;
            return 1;
        }

        // Release the RackType::Ptr before createNewSession() destroys the Edit.
        rack0 = nullptr;
        app.createNewSession (sessionFile, true, false);
        wo017TempDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-017 Factory presets, .trkpreset round-trip, Sampler 808 kit & multi-machine rack PASS" << std::endl;
    }

    //===========================================================================
    // WO-016 Hardware Rack Suite: TR-808, 12-bit sampler, tape + RPC persistence
    //===========================================================================
    {
        const auto hwDir = tempDir.getChildFile ("tcs_wo016_hardware_smoke");
        hwDir.deleteRecursively();
        hwDir.createDirectory();
        app.createNewSession (hwDir.getChildFile ("HardwareRack.tracktionedit"), true, false);

        // 1. TR-808 multi-out + CV accent on pin 1 / channel 0.
        {
            constexpr int blockN = 512;
            juce::AudioBuffer<float> drumBuf (5, blockN);
            drumBuf.clear();

            for (int i = 0; i < blockN; ++i)
                drumBuf.setSample (0, i, 0.6f);

            te::MidiMessageArray drumMidi;
            drumMidi.addMidiMessage (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (120)), 0.0, te::MPESourceID{});
            drumMidi.addMidiMessage (juce::MidiMessage::noteOn (1, 38, static_cast<juce::uint8> (120)), 0.0, te::MPESourceID{});
            drumMidi.addMidiMessage (juce::MidiMessage::noteOn (1, 42, static_cast<juce::uint8> (120)), 0.0, te::MPESourceID{});

            auto drumPtr = app.edit->getPluginCache().createNewPlugin (Tr808DrumPlugin::xmlTypeName, {});
            auto* drum = dynamic_cast<Tr808DrumPlugin*> (drumPtr.get());

            if (drum == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 Tr808DrumPlugin could not be created" << std::endl;
                return 1;
            }

            drum->initialise (te::PluginInitialisationInfo {
                tracktion::TimePosition::fromSeconds (0.0), 44100.0, blockN });
            drum->applyToBuffer (te::PluginRenderContext (&drumBuf, 0, blockN, &drumMidi,
                                                          0.0, {}, false, false, false, false));

            for (int ch = 0; ch < 5; ++ch)
            {
                bool finite = true;

                for (int i = 0; i < blockN; ++i)
                    if (! std::isfinite (drumBuf.getSample (ch, i)))
                        finite = false;

                if (! finite || drumBuf.getRMSLevel (ch, 0, blockN) <= 1e-3f)
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-016 TR-808 output " << ch
                              << " silent or non-finite" << std::endl;
                    return 1;
                }
            }

            // Closed Hat (42) must choke Open Hat (46), and 2-ch track inserts must render stereo.
            te::MidiMessageArray chokeMidi;
            chokeMidi.addMidiMessage (juce::MidiMessage::noteOn (1, 46, static_cast<juce::uint8> (110)), 0.0, te::MPESourceID{});
            juce::AudioBuffer<float> stereoDrum (2, 128);
            stereoDrum.clear();
            drum->applyToBuffer (te::PluginRenderContext (&stereoDrum, 0, 128, &chokeMidi,
                                                          0.0, {}, false, false, false, false));
            if (! drum->isOHatActiveForSmokeTest() || stereoDrum.getRMSLevel (0, 0, 128) <= 1e-4f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 TR-808 2-ch OHat render failed" << std::endl;
                return 1;
            }
            chokeMidi.clear();
            chokeMidi.addMidiMessage (juce::MidiMessage::noteOn (1, 42, static_cast<juce::uint8> (110)), 0.0, te::MPESourceID{});
            drum->applyToBuffer (te::PluginRenderContext (&stereoDrum, 0, 128, &chokeMidi,
                                                          0.0, {}, false, false, false, false));
            if (drum->isOHatActiveForSmokeTest() || ! PluginPresetManager::applyFactoryPreset (*drum, "Sub Shaker 808"))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 TR-808 CHat choke or preset application failed" << std::endl;
                return 1;
            }

            drum->deinitialise();
            drumPtr = nullptr;
        }

        // 2. 12-bit sampler: ZOH decimation + reconstruction bypass.
        {
            constexpr int blockN = 1024;
            juce::AudioBuffer<float> sampleOut (2, blockN);
            sampleOut.clear();

            te::MidiMessageArray sampleMidi;
            sampleMidi.addMidiMessage (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (120)), 0.0, te::MPESourceID{});

            auto samplerPtr = app.edit->getPluginCache().createNewPlugin (VintageSamplerPlugin::xmlTypeName, {});
            auto* sampler = dynamic_cast<VintageSamplerPlugin*> (samplerPtr.get());

            if (sampler == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 VintageSamplerPlugin could not be created" << std::endl;
                return 1;
            }

            sampler->setTargetRate (12000.0f);
            sampler->setFilterCutoff (18000.0f);
            sampler->initialise (te::PluginInitialisationInfo {
                tracktion::TimePosition::fromSeconds (0.0), 44100.0, blockN });
            sampler->applyToBuffer (te::PluginRenderContext (&sampleOut, 0, blockN, &sampleMidi,
                                                             0.0, {}, false, false, false, false));
            sampler->deinitialise();

            int repeats = 0;

            for (int i = 1; i < 300; ++i)
                if (sampleOut.getSample (0, i) == sampleOut.getSample (0, i - 1))
                    ++repeats;

            if (sampleOut.getRMSLevel (0, 0, blockN) <= 1e-3f || repeats < 20
                || ! PluginPresetManager::applyFactoryPreset (*sampler, "Lo-Fi Crunch (8-Bit / 12 kHz)"))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 sampler ZOH staircase or preset failed (repeats="
                          << repeats << ")" << std::endl;
                return 1;
            }

            samplerPtr = nullptr;
        }

        // 3. Analog tape: asymmetric saturation + wow/flutter is bounded & non-linear.
        {
            constexpr int blockN = 512;
            constexpr double sr = 44100.0;
            juce::AudioBuffer<float> tapeBuf (3, blockN);

            for (int i = 0; i < blockN; ++i)
            {
                const float s = static_cast<float> (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / sr));
                tapeBuf.setSample (0, i, s);
                tapeBuf.setSample (1, i, s);
                tapeBuf.setSample (2, i, 0.0f);
            }

            const float inputRms = tapeBuf.getRMSLevel (0, 0, blockN);

            auto tapePtr = app.edit->getPluginCache().createNewPlugin (AnalogTapePlugin::xmlTypeName, {});
            auto* tape = dynamic_cast<AnalogTapePlugin*> (tapePtr.get());

            if (tape == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 AnalogTapePlugin could not be created" << std::endl;
                return 1;
            }

            tape->setDrive (4.0f);
            tape->setWow (0.4f);
            tape->setMix (1.0f);
            tape->initialise (te::PluginInitialisationInfo {
                tracktion::TimePosition::fromSeconds (0.0), sr, blockN });
            tape->applyToBuffer (te::PluginRenderContext (&tapeBuf, 0, blockN, nullptr,
                                                          0.0, {}, false, false, false, false));

            float diff = 0.0f;
            bool finite = true;

            for (int i = 0; i < blockN; ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float x = tapeBuf.getSample (ch, i);
                    if (! std::isfinite (x) || std::abs (x) >= 1.0f)
                        finite = false;
                    diff += std::abs (x - static_cast<float> (0.5 * std::sin (
                                        2.0 * juce::MathConstants<double>::pi * 220.0 * i / sr)));
                }
            }

            if (! finite || tapeBuf.getRMSLevel (0, 0, blockN) <= 1e-3f
                || diff / static_cast<float> (blockN * 2) < 1e-3f
                || std::abs (tapeBuf.getRMSLevel (0, 0, blockN) - inputRms) < 1e-4f
                || std::abs (tapeBuf.getSample (2, 0)) > 1e-6f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 tape saturation/wow invalid or CV not cleared" << std::endl;
                return 1;
            }

            // Also verify AnalogTapePlugin processes 2-channel stereo tracks cleanly while initialised.
            juce::AudioBuffer<float> stereoTape (2, 256);
            for (int i = 0; i < 256; ++i)
            {
                const float s = static_cast<float> (0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / sr));
                stereoTape.setSample (0, i, s);
                stereoTape.setSample (1, i, s);
            }
            tape->applyToBuffer (te::PluginRenderContext (&stereoTape, 0, 256, nullptr,
                                                          0.0, {}, false, false, false, false));
            if (stereoTape.getRMSLevel (0, 0, 256) <= 1e-3f
                || ! PluginPresetManager::applyFactoryPreset (*tape, "Lo-Fi Cassette"))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 2-ch track tape processing or preset failed" << std::endl;
                return 1;
            }

            // After deinitialise(), applyToBuffer must safely no-op without overflowing ring buffers.
            tape->deinitialise();
            tape->applyToBuffer (te::PluginRenderContext (&stereoTape, 0, 256, nullptr,
                                                          0.0, {}, false, false, false, false));

            tapePtr = nullptr;
        }

        // 4. rack.add_module + track.create + save/reload persistence.
        {
            const auto targetFile = hwDir.getChildFile ("HardwarePersist.tracktionedit");
            targetFile.deleteFile();
            app.createNewSession (targetFile, true, false);

            const char* moduleTypes[3] = { "tr808", "sampler", "tape" };

            for (int i = 0; i < 3; ++i)
            {
                const auto resp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
                    "{\"jsonrpc\":\"2.0\",\"id\":" + juce::String (180 + i)
                    + ",\"method\":\"rack.add_module\",\"params\":{\"module_type\":\""
                    + moduleTypes[i] + "\",\"x\":" + juce::String (0.2 + 0.2 * i)
                    + ",\"y\":0.5}}"));

                if (! resp["result"].isObject()
                    || resp["result"]["name"].toString().isEmpty())
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-016 rack.add_module " << moduleTypes[i]
                              << " returned invalid result" << std::endl;
                    return 1;
                }
            }

            for (int i = 0; i < 3; ++i)
            {
                const auto resp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
                    "{\"jsonrpc\":\"2.0\",\"id\":" + juce::String (190 + i)
                    + ",\"method\":\"track.create\",\"params\":{\"name\":\"HW "
                    + juce::String (i + 1) + "\",\"plugin_type\":\"" + moduleTypes[i] + "\"}}"));

                if (! resp["result"].isObject())
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-016 track.create " << moduleTypes[i]
                              << " returned invalid result" << std::endl;
                    return 1;
                }
            }

            app.currentEditFile = targetFile;
            app.sessionUntitled = false;
            app.saveSession();

            if (! targetFile.existsAsFile())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 hardware session was not saved" << std::endl;
                return 1;
            }

            juce::String loadError;

            if (! app.loadSessionFromFileSafely (targetFile, &loadError))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-016 hardware session reload: "
                          << loadError << std::endl;
                return 1;
            }

            int found[3] = { 0, 0, 0 };

            auto countPlugin = [&found] (te::Plugin* p)
            {
                if (dynamic_cast<Tr808DrumPlugin*> (p) != nullptr)       ++found[0];
                if (dynamic_cast<VintageSamplerPlugin*> (p) != nullptr)  ++found[1];
                if (dynamic_cast<AnalogTapePlugin*> (p) != nullptr)      ++found[2];
            };

            for (auto* t : te::getAudioTracks (*app.edit))
                for (auto* p : t->pluginList)
                    countPlugin (p);

            for (auto* r : app.edit->getRackList().getTypes())
                for (auto* p : r->getPlugins())
                    countPlugin (p);

            for (int i = 0; i < 3; ++i)
            {
                if (found[i] < 2)
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-016 " << moduleTypes[i]
                              << " did not persist (found " << found[i] << ")"
                              << std::endl;
                    return 1;
                }
            }

            app.createNewSession (sessionFile, true, false);
        }

        hwDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-016 TR-808 multi-out, 12-bit ZOH sampler, analog tape & rack.add_module persistence PASS" << std::endl;
    }

    //===========================================================================
    // WO-018 Regression Test: AI Co-Producer Scaling (Chords, Scale, LUFS, Dupe)
    //===========================================================================
    {
        const auto wo018Dir = tempDir.getChildFile ("tcs_wo018_smoke");
        wo018Dir.deleteRecursively();
        wo018Dir.createDirectory();

        const auto wo018File = wo018Dir.getChildFile ("Wo018Scaling.tracktionedit");
        app.createNewSession (wo018File, true, false);

        // 1. midi.generate_progression on track 0 (voice-led i - VI - III - VII in C minor)
        const auto progResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":201,"method":"midi.generate_progression","params":{"track_index":0,"key":"C","scale":"minor","progression":["i","VI","III","VII"],"start_beat":0.0,"beats_per_chord":2.0,"octave":4,"velocity":96,"voice_lead":true}})"));

        if (! progResp["result"].isObject()
            || static_cast<int> (progResp["result"]["chords_written"]) != 4
            || static_cast<int> (progResp["result"]["notes_written"]) != 12)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-018 midi.generate_progression result mismatch" << std::endl;
            return 1;
        }

        // 2. midi.quantize_scale on a dedicated MIDI track (out-of-scale pitches & off-grid beats)
        const auto qTrackResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":202,"method":"track.create","params":{"name":"Quantize Test","plugin_type":"4osc"}})"));
        const int qTrackIdx = static_cast<int> (qTrackResp["result"]["track_index"]);

        const auto addOffResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            "{\"jsonrpc\":\"2.0\",\"id\":203,\"method\":\"midi.add_notes\",\"params\":{\"track_index\":"
            + juce::String (qTrackIdx)
            + ",\"clip_name\":\"Quantize Clip\",\"notes\":[{\"pitch\":66,\"start_beat\":0.11,\"length_beats\":0.5,\"velocity\":100},{\"pitch\":64,\"start_beat\":1.39,\"length_beats\":0.5,\"velocity\":90}]}}"));
        juce::ignoreUnused (addOffResp);

        const auto qResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            "{\"jsonrpc\":\"2.0\",\"id\":204,\"method\":\"midi.quantize_scale\",\"params\":{\"track_index\":"
            + juce::String (qTrackIdx)
            + ",\"key\":\"C\",\"scale\":\"minor\",\"grid_beats\":0.25,\"snap_pitch\":true,\"snap_timing\":true}}"));

        if (! qResp["result"].isObject()
            || static_cast<int> (qResp["result"]["pitch_snapped"]) < 2
            || static_cast<int> (qResp["result"]["timing_snapped"]) < 2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-018 midi.quantize_scale snapped counts too low" << std::endl;
            return 1;
        }

        {
            auto tracks = te::getAudioTracks (*app.edit);
            int verifiedNotes = 0;
            for (auto* c : tracks[qTrackIdx]->getClips())
            {
                if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                {
                    for (auto* n : mc->getSequence().getNotes())
                    {
                        const int p = n->getNoteNumber();
                        const double sb = n->getStartBeat().inBeats();
                        const bool expectedMatch = (p == 65 && std::abs (sb - 0.0) < 1e-4)
                                                || (p == 63 && std::abs (sb - 1.5) < 1e-4);
                        if (expectedMatch)
                            ++verifiedNotes;
                    }
                }
            }

            if (verifiedNotes != 2)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-018 quantized pitch/timing values incorrect (matched "
                          << verifiedNotes << ")" << std::endl;
                return 1;
            }
        }

        // 3. mixer.auto_balance_lufs clamped finite adjustment
        const auto lufsResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":205,"method":"mixer.auto_balance_lufs","params":{"target_lufs":-14.0,"min_gain_db":-12.0,"max_gain_db":4.5,"headroom_db":-1.0}})"));
        const auto* adjArr = lufsResp["result"]["tracks_adjusted"].getArray();

        if (adjArr == nullptr || adjArr->isEmpty())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-018 mixer.auto_balance_lufs returned empty list" << std::endl;
            return 1;
        }

        for (auto* t : te::getAudioTracks (*app.edit))
        {
            if (auto* vol = t->getVolumePlugin())
            {
                const float db = vol->getVolumeDb();
                if (! std::isfinite (db) || db < -12.01f || db > 4.51f)
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-018 track gain out of bounds (" << db << ")" << std::endl;
                    return 1;
                }
            }
        }

        // 4. arrangement.duplicate_section + mute_drums + save/reload persistence
        const auto drumResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":206,"method":"track.create","params":{"name":"808 Drum Track","plugin_type":"tr808"}})"));
        const int drumTrackIdx = static_cast<int> (drumResp["result"]["track_index"]);
        app.ipcServer.executeJsonRpcOnMessageThread (
            "{\"jsonrpc\":\"2.0\",\"id\":207,\"method\":\"midi.add_notes\",\"params\":{\"track_index\":"
            + juce::String (drumTrackIdx)
            + ",\"clip_name\":\"Drum Groove\",\"notes\":[{\"pitch\":36,\"start_beat\":0.0,\"length_beats\":0.5,\"velocity\":120}]}}");

        const auto dupResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
            R"({"jsonrpc":"2.0","id":208,"method":"arrangement.duplicate_section","params":{"start_beat":0.0,"end_beat":4.0,"dest_beat":8.0,"mute_drums":true}})"));

        if (! dupResp["result"].isObject() || static_cast<int> (dupResp["result"]["clips_duplicated"]) < 2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-018 arrangement.duplicate_section duplicated too few clips" << std::endl;
            return 1;
        }

        app.currentEditFile = wo018File;
        app.sessionUntitled = false;
        app.saveSession();

        juce::String loadError;
        if (! app.loadSessionFromFileSafely (wo018File, &loadError))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-018 session reload failed: " << loadError << std::endl;
            return 1;
        }

        {
            auto tracks = te::getAudioTracks (*app.edit);
            bool foundDuplicatedOnQ = false;
            bool foundDuplicatedOnDrum = false;

            for (auto* c : tracks[qTrackIdx]->getClips())
            {
                const double sb = app.edit->tempoSequence.toBeats (c->getPosition().getStart()).inBeats();
                if (std::abs (sb - 8.0) < 0.05)
                    foundDuplicatedOnQ = true;
            }

            for (auto* c : tracks[drumTrackIdx]->getClips())
            {
                const double sb = app.edit->tempoSequence.toBeats (c->getPosition().getStart()).inBeats();
                if (sb >= 7.9)
                    foundDuplicatedOnDrum = true;
            }

            if (! foundDuplicatedOnQ || foundDuplicatedOnDrum)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-018 duplicate_section persistence or mute_drums failed" << std::endl;
                return 1;
            }
        }

        app.createNewSession (sessionFile, true, false);
        wo018Dir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-018 Harmonic progression, scale quantize, LUFS auto-balance & section duplicator PASS" << std::endl;
    }

    //===========================================================================
    // WO-019 Regression Test: Sidechain Pin 3/4, Take Comping & Poly Transcription
    //===========================================================================
    {
        const auto wo019Dir = tempDir.getChildFile ("tcs_wo019_smoke");
        wo019Dir.deleteRecursively();
        wo019Dir.createDirectory();
        const auto wo019File = wo019Dir.getChildFile ("Wo019Dsp.tracktionedit");
        app.createNewSession (wo019File, true, false);

        constexpr int wo019Sr = 44100;
        constexpr int wo019Frames = 4410;

        // 1. SidechainCompressorPlugin: 4-pin ducking, channel clearing & post-deinit no-op
        {
            auto compPtr = app.edit->getPluginCache().createNewPlugin (
                SidechainCompressorPlugin::xmlTypeName, {});
            auto* comp = dynamic_cast<SidechainCompressorPlugin*> (compPtr.get());
            if (comp == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 SidechainCompressorPlugin not created" << std::endl;
                return 1;
            }

            juce::StringArray ins, outs;
            comp->getChannelNames (&ins, &outs);
            if (ins.size() != 4 || outs.size() != 2
                || ins[2] != "Sidechain In L" || ins[3] != "Sidechain In R"
                || outs[0] != "Audio Out L" || outs[1] != "Audio Out R")
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 sidechain pinout mismatch" << std::endl;
                return 1;
            }

            juce::AudioBuffer<float> fourCh (4, wo019Frames);
            fourCh.clear();
            for (int i = 0; i < wo019Frames; ++i)
            {
                const float tone = static_cast<float> (
                    0.35 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / wo019Sr));
                fourCh.setSample (0, i, tone);
                fourCh.setSample (1, i, tone);
                fourCh.setSample (2, i, 0.95f);
                fourCh.setSample (3, i, 0.95f);
            }

            const float mainBefore = fourCh.getMagnitude (0, 0, wo019Frames);
            const te::PluginInitialisationInfo initInfo {
                tracktion::TimePosition::fromSeconds (0.0), wo019Sr, wo019Frames };
            comp->initialise (initInfo);
            comp->applyToBuffer (te::PluginRenderContext (
                &fourCh, 0, wo019Frames, nullptr, 0.0, {}, false, false, false, false));

            const float grDb = comp->currentGainReductionDb.load();
            const float mainAfter = fourCh.getMagnitude (0, 0, wo019Frames);
            if (grDb > -3.0f || mainAfter >= mainBefore
                || fourCh.getMagnitude (2, 0, wo019Frames) > 1.0e-6f
                || fourCh.getMagnitude (3, 0, wo019Frames) > 1.0e-6f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 sidechain ducking/clearing failed (gr="
                          << grDb << ", before=" << mainBefore << ", after=" << mainAfter << ")" << std::endl;
                return 1;
            }

            comp->deinitialise();
            const juce::AudioBuffer<float> frozen (fourCh);
            comp->applyToBuffer (te::PluginRenderContext (
                &fourCh, 0, wo019Frames, nullptr, 0.0, {}, false, false, false, false));
            if (! juce::isWithin (frozen.getMagnitude (0, 0, wo019Frames),
                                  fourCh.getMagnitude (0, 0, wo019Frames), 1.0e-7f))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 post-deinitialise applyToBuffer was not a no-op" << std::endl;
                return 1;
            }

            juce::AudioBuffer<float> twoCh (2, wo019Frames);
            for (int i = 0; i < wo019Frames; ++i)
            {
                const float v = static_cast<float> (
                    0.28 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / wo019Sr));
                twoCh.setSample (0, i, v);
                twoCh.setSample (1, i, v);
            }
            comp->initialise (initInfo);
            comp->applyToBuffer (te::PluginRenderContext (
                &twoCh, 0, wo019Frames, nullptr, 0.0, {}, false, false, false, false));
            const float twoMag = twoCh.getMagnitude (0, 0, wo019Frames);
            comp->deinitialise();
            if (! std::isfinite (twoMag) || twoMag <= 0.0f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 2-channel track-mode render was not finite" << std::endl;
                return 1;
            }
            compPtr = nullptr;
        }

        // 2. Multi-take loop comping: two WAV takes, select, flatten & persist
        {
            auto writeSineWav = [] (const juce::File& file, double freq) -> bool
            {
                juce::AudioBuffer<float> buf (2, 22050);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                {
                    const float v = static_cast<float> (
                        0.45 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / 44100.0));
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }
                auto fs = std::make_unique<juce::FileOutputStream> (file);
                if (! fs->openedOk())
                    return false;
                fs->setPosition (0);
                fs->truncate();
                std::unique_ptr<juce::OutputStream> os (fs.release());
                juce::WavAudioFormat wav;
                const auto opts = juce::AudioFormatWriterOptions{}
                    .withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16);
                std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (os, opts));
                return writer != nullptr
                    && writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
            };

            const auto takeA = wo019Dir.getChildFile ("TakeA.wav");
            const auto takeB = wo019Dir.getChildFile ("TakeB.wav");
            if (! writeSineWav (takeA, 220.0) || ! writeSineWav (takeB, 330.0))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 could not synthesise take WAVs" << std::endl;
                return 1;
            }

            const auto trackResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
                R"({"jsonrpc":"2.0","id":301,"method":"track.create","params":{"name":"Loop Comp","plugin_type":"4osc"}})"));
            const int compTrackIdx = static_cast<int> (trackResp["result"]["track_index"]);
            auto tracks = te::getAudioTracks (*app.edit);
            if (! juce::isPositiveAndBelow (compTrackIdx, tracks.size())
                || ! RecordingManager::addRecordedTake (*app.edit, *tracks[compTrackIdx], takeA, 0.0, 4.0)
                || ! RecordingManager::addRecordedTake (*app.edit, *tracks[compTrackIdx], takeB, 0.0, 4.0))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 could not register two takes" << std::endl;
                return 1;
            }

            const auto listResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
                "{\"jsonrpc\":\"2.0\",\"id\":302,\"method\":\"recording.comp_takes\",\"params\":{\"track_index\":"
                + juce::String (compTrackIdx) + ",\"action\":\"list\"}}"));
            const auto* takeArr = listResp["result"]["takes"].getArray();
            if (takeArr == nullptr || takeArr->size() != 2)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 recording.comp_takes list returned != 2 takes" << std::endl;
                return 1;
            }

            const auto selectResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
                "{\"jsonrpc\":\"2.0\",\"id\":303,\"method\":\"recording.comp_takes\",\"params\":{\"track_index\":"
                + juce::String (compTrackIdx) + ",\"action\":\"select\",\"take_index\":0}}"));
            if (! selectResp["result"]["selected"].isBool() || ! static_cast<bool> (selectResp["result"]["selected"]))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 recording.comp_takes select failed" << std::endl;
                return 1;
            }

            const auto flatResp = juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
                "{\"jsonrpc\":\"2.0\",\"id\":304,\"method\":\"recording.comp_takes\",\"params\":{\"track_index\":"
                + juce::String (compTrackIdx)
                + ",\"action\":\"flatten\",\"start_beat\":0.0,\"end_beat\":4.0,\"crossfade_ms\":10.0}}"));
            const juce::File compWav (flatResp["result"]["output_file"].toString());
            if (! compWav.existsAsFile())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 recording.comp_takes flatten produced no WAV" << std::endl;
                return 1;
            }

            juce::AudioFormatManager compFm;
            compFm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> compReader (compFm.createReaderFor (compWav));
            juce::AudioBuffer<float> compReadBack (2, 4096);
            if (compReader == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 could not read flattened comp WAV" << std::endl;
                return 1;
            }
            compReader->read (&compReadBack, 0, 4096, 0, true, true);
            if (compReadBack.getMagnitude (0, 0, 4096) < 0.05f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 flattened comp WAV was silent" << std::endl;
                return 1;
            }
            if (! app.edit->getUndoManager().canUndo())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 comp flatten did not open an undo transaction" << std::endl;
                return 1;
            }
            compReader.reset();

            app.currentEditFile = wo019File;
            app.sessionUntitled = false;
            app.saveSession();
            juce::String wo019LoadError;
            if (! app.loadSessionFromFileSafely (wo019File, &wo019LoadError))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 session reload failed: " << wo019LoadError << std::endl;
                return 1;
            }
            tracks = te::getAudioTracks (*app.edit);
            if (! juce::isPositiveAndBelow (compTrackIdx, tracks.size())
                || RecordingManager::getTrackTakes (*tracks[compTrackIdx]).size() != 2)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 take lanes did not survive save/reload" << std::endl;
                return 1;
            }
        }

        // 3. Polyphonic Constant-Q triad detection (C4 + Eb4 + G4)
        {
            juce::AudioBuffer<float> triad (2, 26460); // 0.6 s @ 44.1 kHz
            for (int i = 0; i < triad.getNumSamples(); ++i)
            {
                const double t = static_cast<double> (i) / 44100.0;
                const float v = static_cast<float> (0.30 * (
                    std::sin (juce::MathConstants<double>::twoPi * 261.626 * t)
                    + std::sin (juce::MathConstants<double>::twoPi * 311.127 * t)
                    + std::sin (juce::MathConstants<double>::twoPi * 392.000 * t)));
                triad.setSample (0, i, v);
                triad.setSample (1, i, v);
            }

            const auto triadWav = wo019Dir.getChildFile ("CminorTriad.wav");
            auto fs = std::make_unique<juce::FileOutputStream> (triadWav);
            if (! fs->openedOk())
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 could not open triad WAV" << std::endl;
                return 1;
            }
            fs->setPosition (0);
            fs->truncate();
            std::unique_ptr<juce::OutputStream> os (fs.release());
            juce::WavAudioFormat wav;
            const auto opts = juce::AudioFormatWriterOptions{}
                .withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16);
            std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (os, opts));
            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (triad, 0, triad.getNumSamples()))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 could not write triad WAV" << std::endl;
                return 1;
            }
            writer.reset();

            const auto polyNotes = AudioToMidiTranscriber::transcribePolyphonic (triadWav, 124.0, 4, -28.0f);
            int matched = 0;
            for (const auto& n : polyNotes)
            {
                const int pc = n.pitch % 12;
                if (pc == 0 || pc == 3 || pc == 7)
                    ++matched;
            }
            if (matched < 3)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-019 polyphonic triad detection matched " << matched
                          << "/3 (total notes " << polyNotes.size() << ")" << std::endl;
                return 1;
            }
        }

        app.createNewSession (sessionFile, true, false);
        wo019Dir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-019 Sidechain Pin 3/4 ducking, multi-take comping & polyphonic Audio-to-MIDI PASS" << std::endl;
    }

    //===========================================================================
    // WO-020 Regression Test: Cross-Platform Config & VST3-free Core Boundary
    //===========================================================================
    {
        const juce::String backendName (TCS_AUDIO_BACKEND_NAME);
        if (backendName.isEmpty())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-020 audio backend name is empty" << std::endl;
            return 1;
        }

#if TCS_HAS_DESKTOP_UI
        static_assert (TCS_HAS_DESKTOP_VST3 == 1,
                       "WO-020: desktop builds must have VST3 hosting enabled");
#endif

        const auto ids = tcs::core::getCoreTypeIds();
        if (! ids.complete()
            || ids.tr808 != "tcs_tr808_drum"
            || ids.sampler != "tcs_vintage_sampler"
            || ids.tape != "tcs_analog_tape"
            || ids.sidechain != "tcs_sidechain_comp"
            || ids.vocal != "tcs_vocal_channel"
            || ids.transcriber != "AudioToMidiTranscriber")
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-020 isolated core type-ID boundary mismatch" << std::endl;
            return 1;
        }

        std::cout << "[SMOKE_TEST] WO-020 " << backendName
                  << " backend, VST3=" << TCS_HAS_DESKTOP_VST3
                  << " and isolated core type IDs PASS" << std::endl;
    }

    //===========================================================================
    // BS.1770-style K-weighted gated loudness meter regression
    //===========================================================================
    {
        constexpr double sampleRate = 48000.0;
        constexpr double toneHz = 1000.0;
        tcs::loudness::KWeightedLoudnessMeter meter (sampleRate, 1);

        for (int i = 0; i < 96000; ++i)
        {
            const double t = static_cast<double> (i) / sampleRate;
            const float sample = static_cast<float> (0.5 * std::sin (juce::MathConstants<double>::twoPi * toneHz * t));
            meter.processFrame (sample, 0.0f);
        }

        const double lufs = meter.getLoudnessLufs();
        if (! std::isfinite (lufs) || lufs < -15.0 || lufs > -4.0)
        {
            std::cerr << "[SMOKE_TEST] FAIL: BS.1770 meter returned " << lufs << " LUFS for a -6 dBFS tone" << std::endl;
            return 1;
        }

        std::cout << "[SMOKE_TEST] BS.1770 K-weighted gated LUFS meter PASS (" << lufs << " LUFS)" << std::endl;
    }

    //===========================================================================
    // Blueprint Reconciliation: original briefing CV modules never implemented
    //===========================================================================
    {
        auto* edit = app.edit.get();
        auto blueprintRack = edit->getRackList().addNewRack();
        blueprintRack->rackName = "CV Blueprint Modules";

        auto envPtr  = edit->getPluginCache().createNewPlugin (CvEnvelopeFollowerPlugin::xmlTypeName, {});
        auto stepPtr = edit->getPluginCache().createNewPlugin (CvStepModulatorPlugin::xmlTypeName, {});

        if (envPtr == nullptr || stepPtr == nullptr
            || dynamic_cast<CvEnvelopeFollowerPlugin*> (envPtr.get()) == nullptr
            || dynamic_cast<CvStepModulatorPlugin*> (stepPtr.get()) == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: CV envelope follower / step modulator failed to instantiate" << std::endl;
            return 1;
        }

        if (! blueprintRack->addPlugin (envPtr,  { 0.22f, 0.30f }, false)
            || ! blueprintRack->addPlugin (stepPtr, { 0.52f, 0.30f }, false)
            || blueprintRack->getPlugins().size() != 2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: CV blueprint modules could not be added to the rack" << std::endl;
            return 1;
        }

        envPtr = nullptr;
        stepPtr = nullptr;
        blueprintRack = nullptr;

        std::cout << "[SMOKE_TEST] CV envelope follower & 8-step modulator blueprint modules PASS" << std::endl;
    }

    //===========================================================================
    // WO-021: Voice Acting Studio Suite
    //===========================================================================
    {
        const auto voDir = tempDir.getChildFile ("tcs_wo021_smoke");
        voDir.deleteRecursively();
        voDir.createDirectory();

        const auto writeTone = [&] (const juce::File& file, double freq, float amp,
                                    double seconds, double leadSilence, double tailSilence) -> bool
        {
            const double sr = 44100.0;
            const int total = static_cast<int> ((seconds + leadSilence + tailSilence) * sr);
            juce::AudioBuffer<float> buf (1, total);
            buf.clear();
            const int start = static_cast<int> (leadSilence * sr);
            const int len = static_cast<int> (seconds * sr);
            for (int i = 0; i < len && start + i < total; ++i)
                buf.setSample (0, start + i,
                    amp * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * freq * i / sr)));

            file.getParentDirectory().createDirectory();
            auto fs = std::make_unique<juce::FileOutputStream> (file);
            if (! fs->openedOk())
                return false;
            fs->setPosition (0);
            fs->truncate();
            std::unique_ptr<juce::OutputStream> os (fs.release());
            juce::WavAudioFormat wavFmt;
            const auto opts = juce::AudioFormatWriterOptions{}
                                  .withSampleRate (sr).withNumChannels (1).withBitsPerSample (16);
            std::unique_ptr<juce::AudioFormatWriter> writer (wavFmt.createWriterFor (os, opts));
            return writer != nullptr && writer->writeFromAudioSampleBuffer (buf, 0, total);
        };

        //---- 1. Vocal booth DSP, factory preset and post-deinitialise guard ----
        auto vocalPtr = app.edit->getPluginCache().createNewPlugin (VocalChannelPlugin::xmlTypeName, {});
        auto* vocal = dynamic_cast<VocalChannelPlugin*> (vocalPtr.get());
        if (vocal == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 VocalChannelPlugin failed to instantiate" << std::endl;
            return 1;
        }

        constexpr int renderSamples = 4410;
        const te::PluginInitialisationInfo initInfo {
            tracktion::TimePosition::fromSeconds (0.0), 44100.0, renderSamples };
        vocal->initialise (initInfo);

        const auto renderRms = [&] (double freq, float amp) -> double
        {
            juce::AudioBuffer<float> buf (2, renderSamples);
            for (int i = 0; i < renderSamples; ++i)
            {
                const float s = amp * static_cast<float> (
                    std::sin (juce::MathConstants<double>::twoPi * freq * i / 44100.0));
                buf.setSample (0, i, s);
                buf.setSample (1, i, s);
            }
            const te::PluginRenderContext rc (&buf, 0, renderSamples, nullptr, 0.0,
                tracktion::TimeRange { tracktion::TimePosition::fromSeconds (0.0),
                                       tracktion::TimePosition::fromSeconds (0.1) },
                false, false, false, false);
            vocal->applyToBuffer (rc);

            double sum = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < renderSamples; ++i)
                {
                    const double v = buf.getSample (ch, i);
                    sum += v * v;
                }
            return std::sqrt (sum / (2.0 * renderSamples));
        };

        const double rumbleRms = renderRms (40.0, 0.5f);
        const double midRms = renderRms (1000.0, 0.5f);
        if (! std::isfinite (rumbleRms) || ! std::isfinite (midRms) || rumbleRms >= midRms * 0.5)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 85 Hz HPF did not attenuate 40 Hz rumble ("
                      << rumbleRms << " vs " << midRms << ")" << std::endl;
            return 1;
        }

        {
            juce::AudioBuffer<float> burst (2, renderSamples);
            for (int i = 0; i < renderSamples; ++i)
            {
                const float s = static_cast<float> (
                    std::sin (juce::MathConstants<double>::twoPi * 6800.0 * i / 44100.0));
                burst.setSample (0, i, s);
                burst.setSample (1, i, s);
            }
            const te::PluginRenderContext rc (&burst, 0, renderSamples, nullptr, 0.0,
                tracktion::TimeRange { tracktion::TimePosition::fromSeconds (0.0),
                                       tracktion::TimePosition::fromSeconds (0.1) },
                false, false, false, false);
            vocal->applyToBuffer (rc);
            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < renderSamples; ++i)
                    peak = std::max (peak, std::abs (burst.getSample (ch, i)));
            const float ceiling = std::pow (10.0f, -2.9f / 20.0f);
            if (! std::isfinite (peak) || peak > ceiling || peak < 0.01f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-021 ceiling limiter peak " << peak << std::endl;
                return 1;
            }
        }

        if (! PluginPresetManager::getFactoryPresetNames (*vocal).contains ("ACX Audiobook")
            || ! PluginPresetManager::applyFactoryPreset (*vocal, "ACX Audiobook"))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 factory VO preset did not load" << std::endl;
            return 1;
        }

        vocal->deinitialise();
        {
            juce::AudioBuffer<float> guard (2, 128);
            for (int i = 0; i < 128; ++i)
            {
                guard.setSample (0, i, 0.25f);
                guard.setSample (1, i, -0.25f);
            }
            const te::PluginRenderContext rc (&guard, 0, 128, nullptr, 0.0,
                tracktion::TimeRange { tracktion::TimePosition::fromSeconds (0.0),
                                       tracktion::TimePosition::fromSeconds (0.01) },
                false, false, false, false);
            vocal->applyToBuffer (rc);
            if (std::abs (guard.getSample (0, 64) - 0.25f) > 1.0e-6f
                || std::abs (guard.getSample (1, 64) + 0.25f) > 1.0e-6f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-021 post-deinitialise applyToBuffer was not a no-op" << std::endl;
                return 1;
            }
        }
        vocalPtr = nullptr;
        vocal = nullptr;

        //---- 2. Punch-and-roll, slate tags, A/B/C audition string --------------
        const auto priorWav = voDir.getChildFile ("prior.wav");
        const auto newWav = voDir.getChildFile ("new_take.wav");
        const auto takeA = voDir.getChildFile ("take_a.wav");
        const auto takeB = voDir.getChildFile ("take_b.wav");
        const auto takeC = voDir.getChildFile ("take_c.wav");
        const auto splicedWav = voDir.getChildFile ("punch_splice.wav");
        const auto abcWav = voDir.getChildFile ("abc_audition.wav");

        if (! writeTone (priorWav, 220.0, 0.35f, 2.0, 0.0, 0.0)
            || ! writeTone (newWav, 330.0, 0.35f, 2.0, 0.0, 0.0)
            || ! writeTone (takeA, 220.0, 0.30f, 0.8, 0.0, 0.0)
            || ! writeTone (takeB, 330.0, 0.30f, 0.6, 0.0, 0.0)
            || ! writeTone (takeC, 440.0, 0.30f, 0.7, 0.0, 0.0))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 could not synthesize take WAVs" << std::endl;
            return 1;
        }

        app.edit->ensureNumberOfAudioTracks (te::getAudioTracks (*app.edit).size() + 1);
        auto* voTrack = te::getAudioTracks (*app.edit).getLast();
        if (voTrack == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 could not create a voice track" << std::endl;
            return 1;
        }

        for (const auto& f : { takeA, takeB, takeC })
            if (! RecordingManager::addRecordedTake (*app.edit, *voTrack, f, 0.0, 4.0))
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-021 could not register take " << f.getFileName() << std::endl;
                return 1;
            }

        if (! RecordingManager::tagTakeSlate (*app.edit, *voTrack, 0, "Take A - Intense"))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 slate tag was not stored" << std::endl;
            return 1;
        }

        const auto punched = RecordingManager::executePunchAndRollSplice (
            *app.edit, *voTrack, priorWav, newWav, 1.0, 15.0, splicedWav);
        if (! punched.existsAsFile())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 punch-and-roll splice file missing" << std::endl;
            return 1;
        }
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (punched));
            juce::AudioBuffer<float> check (1, static_cast<int> (std::min<juce::int64> (4096, reader->lengthInSamples)));
            reader->read (&check, 0, check.getNumSamples(), 0, true, false);
            if (check.getNumSamples() <= 0 || check.getMagnitude (0, check.getNumSamples()) <= 1.0e-4f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-021 spliced take is silent" << std::endl;
                return 1;
            }
        }

        if (! RecordingManager::exportAbcAuditionString (*voTrack, { 0, 1, 2 }, 1.5, abcWav, true))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 A/B/C audition export failed" << std::endl;
            return 1;
        }
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (abcWav));
            const double expectedSec = 0.8 + 0.6 + 0.7 + 2.0 * 1.5;
            const double actualSec = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
            if (std::abs (actualSec - expectedSec) > 0.05)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-021 A/B/C duration " << actualSec
                          << " != " << expectedSec << std::endl;
                return 1;
            }
        }

        const auto voSession = voDir.getChildFile ("wo021_vo.tracktionedit");
        app.currentEditFile = voSession;
        app.sessionUntitled = false;
        app.saveSession();
        if (! app.loadSessionFromFileSafely (voSession))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 voice session reload failed" << std::endl;
            return 1;
        }
        {
            const auto reloadedTakes = RecordingManager::getTrackTakes (*te::getAudioTracks (*app.edit).getLast());
            bool foundSlate = false;
            for (const auto& info : reloadedTakes)
                if (info.slate == "Take A - Intense")
                    foundSlate = true;
            if (! foundSlate)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-021 slate tag did not survive save/reload" << std::endl;
                return 1;
            }
        }

        //---- 3. ACX / BS.1770 pass-fail detection ------------------------------
        const auto acxWav = voDir.getChildFile ("acx_pass.wav");
        const auto clippedWav = voDir.getChildFile ("clipped.wav");
        if (! writeTone (acxWav, 1000.0, 0.20f, 2.0, 0.7, 1.5)
            || ! writeTone (clippedWav, 1000.0, 1.0f, 1.0, 0.0, 0.0))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 could not synthesize analyzer fixtures" << std::endl;
            return 1;
        }

        const auto acxResult = tcs::vo::VocalSpecAnalyzer::analyzeWavFile (acxWav, "acx");
        if (! acxResult.passed || ! std::isfinite (acxResult.lufs)
            || acxResult.measurement != "bs1770_k_weighted_gated")
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 ACX-compliant WAV did not pass: "
                      << acxResult.issues.joinIntoString (" | ") << std::endl;
            return 1;
        }

        const auto clipResult = tcs::vo::VocalSpecAnalyzer::analyzeWavFile (clippedWav, "acx");
        bool peakViolation = false;
        for (const auto& issue : clipResult.issues)
            if (issue.contains ("peak"))
                peakViolation = true;
        if (clipResult.passed || ! peakViolation)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-021 clipped WAV did not report a peak violation" << std::endl;
            return 1;
        }

        voDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-021 vocal DSP, punch-and-roll, A/B/C export & BS.1770/ACX analyzer PASS" << std::endl;
    }

    //===========================================================================
    // WO-022: Voice Acting Studio Suite Part 2
    //===========================================================================
    {
        const auto voDir = tempDir.getChildFile ("tcs_wo022_smoke");
        voDir.deleteRecursively();
        voDir.createDirectory();

        const auto callRpc = [&] (const juce::String& method, const juce::var& params) -> juce::var
        {
            auto* request = new juce::DynamicObject();
            request->setProperty ("jsonrpc", "2.0");
            request->setProperty ("id", 1);
            request->setProperty ("method", method);
            request->setProperty ("params", params);
            const auto response = app.ipcServer.executeJsonRpcOnMessageThread (
                juce::JSON::toString (juce::var (request), true));
            juce::var parsed;
            if (! juce::JSON::parse (response, parsed).wasOk() || parsed.hasProperty ("error"))
                return {};
            return parsed["result"];
        };

        auto* setParams = new juce::DynamicObject();
        setParams->setProperty ("title", "WO-022 Booth Script");
        setParams->setProperty ("text", "Line A for the booth.\nLine B for the booth.");
        setParams->setProperty ("wpm", 144);
        setParams->setProperty ("adr_enabled", true);
        setParams->setProperty ("adr_line_sec", 4.5);

        const auto setResult = callRpc ("vo.set_script", juce::var (setParams));
        if (! setResult.isObject() || static_cast<int> (setResult["wpm"]) != 144)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022 vo.set_script did not round-trip" << std::endl;
            return 1;
        }

        auto* mm = &app.edit->getMarkerManager();
        if (auto marker = mm->createMarker (mm->getNextUniqueID(),
                                            tracktion::TimePosition::fromSeconds (0.5),
                                            tracktion::TimeDuration(), nullptr))
            marker->setName ("Booth Cue 1");

        const auto getResult = callRpc ("vo.get_script", juce::var (new juce::DynamicObject()));
        const bool markerFound = getResult.isObject() && getResult["markers"].isArray()
            && ! getResult["markers"].getArray()->isEmpty();
        if (! getResult.isObject()
            || getResult["title"].toString() != "WO-022 Booth Script"
            || getResult["text"].toString() != "Line A for the booth.\nLine B for the booth."
            || ! markerFound)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022 vo.get_script state/markers mismatch" << std::endl;
            return 1;
        }

        // ADR cue pre-roll pips are finite; the live line window is silent.
        {
            tcs::vo::AdrCueConfig cfg;
            constexpr double lineStart = 5.0;
            for (int i = 0; i < tcs::vo::AdrCueGenerator::pipCount; ++i)
            {
                const double pipTime = lineStart
                    - tcs::vo::AdrCueGenerator::pipLeadSeconds (i, cfg) + 0.00025;
                const float v = tcs::vo::AdrCueGenerator::getMonitorSample (pipTime, lineStart, cfg);
                if (! std::isfinite (v) || std::abs (v) < 1.0e-6f)
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-022 ADR pip " << i << " was silent" << std::endl;
                    return 1;
                }
            }
            if (std::abs (tcs::vo::AdrCueGenerator::getMonitorSample (5.2, lineStart, cfg)) >= 1.0e-6f)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-022 ADR cue leaked into the live line window" << std::endl;
                return 1;
            }
            double remaining = 0.0;
            if (tcs::vo::AdrCueGenerator::getPacerStage (1.0, remaining) != tcs::vo::PacerStage::inhale
                || tcs::vo::AdrCueGenerator::getPacerStage (5.0, remaining) != tcs::vo::PacerStage::hold
                || tcs::vo::AdrCueGenerator::getPacerStage (8.0, remaining) != tcs::vo::PacerStage::hiss)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-022 4-2-15 warm-up pacer stage mismatch" << std::endl;
                return 1;
            }
        }

        // Demo reel layout: one transaction wires the vocal strip and duck bus.
        app.edit->ensureNumberOfAudioTracks (2);
        auto* layoutParams = new juce::DynamicObject();
        layoutParams->setProperty ("voice_track_index", 0);
        layoutParams->setProperty ("bed_track_index", 1);
        layoutParams->setProperty ("duck_threshold_db", -22.0);
        layoutParams->setProperty ("duck_ratio", 6.0);
        const auto layoutResult = callRpc ("vo.build_demo_reel_layout", juce::var (layoutParams));
        if (! layoutResult.isObject()
            || layoutResult["vocal_plugin"].toString() != "tcs_vocal_channel"
            || layoutResult["duck_plugin"].toString() != "tcs_sidechain_comp")
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022 demo reel layout RPC did not wire plugins" << std::endl;
            return 1;
        }

        const auto voSession = voDir.getChildFile ("wo022_booth.tracktionedit");
        app.currentEditFile = voSession;
        app.sessionUntitled = false;
        app.saveSession();
        if (! app.loadSessionFromFileSafely (voSession))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022 booth session reload failed" << std::endl;
            return 1;
        }

        const auto reloaded = callRpc ("vo.get_script", juce::var (new juce::DynamicObject()));
        if (! reloaded.isObject() || reloaded["title"].toString() != "WO-022 Booth Script")
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022 script did not survive save/reload" << std::endl;
            return 1;
        }

        {
            auto tracks = te::getAudioTracks (*app.edit);
            bool foundVocal = false, foundDuck = false;
            if (tracks.size() >= 2)
            {
                for (auto* p : tracks[0]->pluginList)
                    if (dynamic_cast<VocalChannelPlugin*> (p) != nullptr) foundVocal = true;
                for (auto* p : tracks[1]->pluginList)
                    if (dynamic_cast<SidechainCompressorPlugin*> (p) != nullptr) foundDuck = true;
            }
            if (! foundVocal || ! foundDuck)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-022 demo reel plugins did not survive reload" << std::endl;
                return 1;
            }

            if (PluginPresetManager::getCharacterFxMacroNames().size() != 4)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-022 four Character FX macros are missing" << std::endl;
                return 1;
            }
            auto fxRack = app.edit->getRackList().addNewRack();
            const bool macroApplied = PluginPresetManager::applyCharacterFxMacro (
                *app.edit, *fxRack, "Sci-Fi Holo / Droid");
            if (! macroApplied || fxRack->getPlugins().size() != 4)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-022 Character FX macro did not patch four modules" << std::endl;
                return 1;
            }
            fxRack = nullptr;
        }

        voDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-022 teleprompter, ADR cue, demo-reel layout & Character FX PASS" << std::endl;
    }

    //===========================================================================
    // WO-022B: boutique hardware faceplates & workspace suite switcher
    //===========================================================================
    {
        const char* faceplateTypes[] {
            Tr808DrumPlugin::xmlTypeName,
            VintageSamplerPlugin::xmlTypeName,
            AnalogTapePlugin::xmlTypeName,
            SidechainCompressorPlugin::xmlTypeName,
            VocalChannelPlugin::xmlTypeName
        };

        for (const char* typeName : faceplateTypes)
        {
            auto pluginPtr = app.edit->getPluginCache().createNewPlugin (typeName, {});
            if (pluginPtr == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-022B could not instantiate " << typeName << std::endl;
                return 1;
            }

            auto editor = createCustomEditorForPlugin (*pluginPtr);
            if (editor == nullptr)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-022B no bespoke editor for " << typeName << std::endl;
                return 1;
            }

            editor->setSize (640, 320);
            editor->resized();
            editor.reset();
            pluginPtr = nullptr;
        }

        app.setSize (1280, 840);
        app.resized();

        app.setWorkspaceMode (MainComponent::WorkspaceMode::modularRack);
        if (app.getWorkspaceMode() != MainComponent::WorkspaceMode::modularRack
            || ! app.modularRackComponent.isVisible())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022B Modular Rack workspace did not activate" << std::endl;
            return 1;
        }

        app.setWorkspaceMode (MainComponent::WorkspaceMode::voBooth);
        if (app.getWorkspaceMode() != MainComponent::WorkspaceMode::voBooth
            || ! app.scriptPrompter.isVisible())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022B VO Booth workspace did not activate" << std::endl;
            return 1;
        }

        app.setWorkspaceMode (MainComponent::WorkspaceMode::studio);
        if (app.getWorkspaceMode() != MainComponent::WorkspaceMode::studio
            || ! app.mixerComponent.isVisible())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-022B Studio workspace did not restore" << std::endl;
            return 1;
        }

        std::cout << "[SMOKE_TEST] WO-022B bespoke faceplates & workspace suite switcher PASS" << std::endl;
    }

    //===========================================================================
    // WO-023: Game Audio Forge & seamless loop renderer
    //===========================================================================
    {
        const auto forgeDir = tempDir.getChildFile ("tcs_wo023_smoke");
        forgeDir.deleteRecursively();
        forgeDir.createDirectory();

        const auto checkBatch = [&] (tcs::game::SfxCategory category, const juce::String& prefix) -> bool
        {
            juce::Array<juce::File> files;
            const int written = tcs::game::SfxForgeEngine::generateSfxVariations (
                category, forgeDir, prefix, 4, 42, 0.25, -1.0, &files);
            if (written != 4 || files.size() != 4)
                return false;

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            juce::AudioBuffer<float> first, second;
            for (int i = 0; i < files.size(); ++i)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (files[i]));
                if (reader == nullptr || reader->lengthInSamples <= 0)
                    return false;
                juce::AudioBuffer<float> buf (1, static_cast<int> (reader->lengthInSamples));
                buf.clear();
                reader->read (&buf, 0, buf.getNumSamples(), 0, true, false);
                const float peak = buf.getMagnitude (0, buf.getNumSamples());
                const float limit = std::pow (10.0f, -0.9f / 20.0f);
                if (! std::isfinite (peak) || peak <= 1.0e-5f || peak > limit)
                    return false;
                if (i == 0) first = buf;
                if (i == 1) second = buf;
            }

            float difference = 0.0f;
            for (int i = 0; i < juce::jmin (first.getNumSamples(), second.getNumSamples()); i += 8)
                difference += std::abs (first.getSample (0, i) - second.getSample (0, i));
            return difference > 1.0e-4f;
        };

        if (! checkBatch (tcs::game::SfxCategory::gunshotTactical, "gun")
            || ! checkBatch (tcs::game::SfxCategory::uiClick, "click"))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-023 SFX variation batch was silent or not diverse" << std::endl;
            return 1;
        }

        // A quiet drone clip gives the seamless-loop renderer real audio to wrap.
        juce::Array<juce::File> droneFiles;
        tcs::game::SfxForgeEngine::generateSfxVariations (
            tcs::game::SfxCategory::ambientDrone, forgeDir, "loopbed", 1, 7, 4.0, -6.0, &droneFiles);
        if (droneFiles.isEmpty())
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-023 loop bed was not rendered" << std::endl;
            return 1;
        }

        app.edit->ensureNumberOfAudioTracks (1);
        auto* loopTrack = te::getAudioTracks (*app.edit).getFirst();
        const tracktion::TimeRange loopRange {
            tracktion::TimePosition::fromSeconds (0.0),
            tracktion::TimePosition::fromSeconds (4.0)
        };
        const te::ClipPosition clipPos { loopRange, tracktion::TimeDuration::fromSeconds (0.0) };
        bool clipInserted = false;
        {
            auto clip = loopTrack->insertWaveClip ("WO-023 Loop Bed", droneFiles.getFirst(), clipPos, false);
            if (clip != nullptr)
            {
                clip->getSourceFileReference().setToFile (
                    droneFiles.getFirst(), te::SourceFileReference::PathStyle::alwaysAbsolute, false);
                clipInserted = true;
            }
        }
        if (! clipInserted)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-023 loop bed clip was not inserted" << std::endl;
            return 1;
        }

        const auto loopWav = forgeDir.getChildFile ("seamless_loop.wav");
        if (! MixdownExporter::exportSeamlessLoopWav (*app.edit, 0.0, 8.0, loopWav, -14.0, 2.0))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-023 seamless loop render failed" << std::endl;
            return 1;
        }

        juce::AudioFormatManager loopFm;
        loopFm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> loopReader (loopFm.createReaderFor (loopWav));
        if (loopReader == nullptr || loopReader->lengthInSamples < 512)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-023 loop WAV is unreadable" << std::endl;
            return 1;
        }
        juce::AudioBuffer<float> loopBuf (2, static_cast<int> (loopReader->lengthInSamples));
        loopBuf.clear();
        loopReader->read (&loopBuf, 0, loopBuf.getNumSamples(), 0, true, true);

        const float boundary = std::abs (loopBuf.getSample (0, 0)
                                        - loopBuf.getSample (0, loopBuf.getNumSamples() - 1));
        tcs::loudness::KWeightedLoudnessMeter checkMeter (loopReader->sampleRate, 2);
        for (int i = 0; i < loopBuf.getNumSamples(); ++i)
            checkMeter.processFrame (loopBuf.getSample (0, i), loopBuf.getSample (1, i));
        const double lufs = checkMeter.getLoudnessLufs();

        if (loopBuf.getMagnitude (0, loopBuf.getNumSamples()) <= 1.0e-5f
            || boundary >= 0.05f || std::abs (lufs + 14.0) > 1.5)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-023 loop boundary=" << boundary
                      << " lufs=" << lufs << std::endl;
            return 1;
        }

        // RPC round-trip for the forge batch.
        auto* forgeParams = new juce::DynamicObject();
        forgeParams->setProperty ("category", "ui_click");
        forgeParams->setProperty ("count", 2);
        forgeParams->setProperty ("seed", 99);
        forgeParams->setProperty ("output_dir", forgeDir.getChildFile ("rpc").getFullPathName());
        forgeParams->setProperty ("prefix", "rpc_click");
        forgeParams->setProperty ("duration_sec", 0.2);
        forgeParams->setProperty ("peak_db", -1.0);
        forgeParams->setProperty ("import_first_to_track", -1);
        auto* rpcRequest = new juce::DynamicObject();
        rpcRequest->setProperty ("jsonrpc", "2.0");
        rpcRequest->setProperty ("id", 1);
        rpcRequest->setProperty ("method", "game.forge_sfx_batch");
        rpcRequest->setProperty ("params", juce::var (forgeParams));
        const auto rpcResponse = app.ipcServer.executeJsonRpcOnMessageThread (
            juce::JSON::toString (juce::var (rpcRequest), true));
        juce::var rpcParsed;
        if (! juce::JSON::parse (rpcResponse, rpcParsed).wasOk()
            || rpcParsed.hasProperty ("error")
            || ! rpcParsed["result"].isObject()
            || static_cast<int> (rpcParsed["result"]["count"]) != 2)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-023 game.forge_sfx_batch RPC round-trip failed" << std::endl;
            return 1;
        }

        forgeDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-023 procedural SFX forge & seamless loop renderer PASS" << std::endl;
    }

    //===========================================================================
    // WO-024: Game Audio & Asset Forge Suite
    //===========================================================================
    {
        const auto packDir = tempDir.getChildFile ("tcs_wo024_smoke");
        packDir.deleteRecursively();
        packDir.createDirectory();
        app.edit->ensureNumberOfAudioTracks (3);
        for (int i = 0; i < 3; ++i)
            te::getAudioTracks (*app.edit)[i]->setName ("layer" + juce::String (i));

        juce::Array<tcs::game::StemLayer> layers;
        for (int i = 0; i < 2; ++i)
        {
            tcs::game::StemLayer layer;
            layer.name = i == 0 ? "calm" : "combat";
            layer.trackIndices.add (i);
            layers.add (layer);
        }
        juce::Array<juce::File> stemFiles;
        const int stems = tcs::game::GameAssetExporter::exportAdaptiveBgmStems (
            *app.edit, 0.0, 8.0, packDir, layers, -14.0, &stemFiles);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        int stemFrames = -1;
        bool stemsOk = stems == 2 && stemFiles.size() == 2;
        for (const auto& file : stemFiles)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            if (reader == nullptr || reader->lengthInSamples <= 0)
            {
                stemsOk = false;
                break;
            }
            if (stemFrames < 0)
                stemFrames = static_cast<int> (reader->lengthInSamples);
            else if (stemFrames != static_cast<int> (reader->lengthInSamples))
                stemsOk = false;
        }
        if (! stemsOk)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024 adaptive stems were not phase-locked" << std::endl;
            return 1;
        }

        // Bark fixture: 0.2 s silence + a click + 0.2 s silence.
        auto barkBuffer = tcs::game::SfxForgeEngine::renderSfx (
            tcs::game::SfxCategory::uiClick, tcs::game::SfxRenderParams {});
        const int silentFrames = static_cast<int> (barkBuffer.getNumSamples() * 0.8);
        juce::AudioBuffer<float> padded (1, barkBuffer.getNumSamples() + silentFrames * 2);
        padded.clear();
        for (int i = 0; i < barkBuffer.getNumSamples(); ++i)
            padded.setSample (0, silentFrames + i, barkBuffer.getSample (0, i));
        const auto barkInput = packDir.getChildFile ("raw_bark.wav");
        if (! tcs::game::GameAssetExporter::writeWav (padded, barkInput, 44100.0, 1))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024 bark fixture could not be written" << std::endl;
            return 1;
        }

        juce::Array<juce::File> barkInputs { barkInput };
        juce::Array<juce::File> barkOutputs;
        const int barks = tcs::game::GameAssetExporter::processVoxBarkBatch (
            barkInputs, packDir, "hero", "attack", -16.0, &barkOutputs);
        bool barkOk = barks == 1 && barkOutputs.size() == 1;
        if (barkOk)
        {
            std::unique_ptr<juce::AudioFormatReader> barkReader (fm.createReaderFor (barkOutputs.getFirst()));
            barkOk = barkReader != nullptr
                  && barkReader->lengthInSamples < static_cast<juce::int64> (padded.getNumSamples());
            if (barkOk)
            {
                juce::AudioBuffer<float> barkOut (1, static_cast<int> (barkReader->lengthInSamples));
                barkOut.clear();
                barkReader->read (&barkOut, 0, barkOut.getNumSamples(), 0, true, false);
                const float peak = barkOut.getMagnitude (0, barkOut.getNumSamples());
                barkOk = peak > 1.0e-5f && peak <= std::pow (10.0f, -0.9f / 20.0f);
            }
        }
        if (! barkOk)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024 bark trim/normalize failed" << std::endl;
            return 1;
        }

        auto* packParams = new juce::DynamicObject();
        packParams->setProperty ("output_dir", packDir.getFullPathName());
        packParams->setProperty ("include_manifest", true);
        packParams->setProperty ("bpm", 120.0);
        auto* packReq = new juce::DynamicObject();
        packReq->setProperty ("jsonrpc", "2.0");
        packReq->setProperty ("id", 301);
        packReq->setProperty ("method", "game.export_godot_pack");
        packReq->setProperty ("params", juce::var (packParams));
        const auto packResponse = app.ipcServer.executeJsonRpcOnMessageThread (
            juce::JSON::toString (juce::var (packReq), true));
        juce::var packParsed;
        bool manifestOk = juce::JSON::parse (packResponse, packParsed).wasOk()
                       && ! packParsed.hasProperty ("error")
                       && static_cast<int> (packParsed["result"]["asset_count"]) >= 3;
        const auto manifestText = packDir.getChildFile ("audio_manifest.json").loadFileAsString();
        juce::var manifestVar;
        manifestOk = manifestOk && juce::JSON::parse (manifestText, manifestVar).wasOk()
                  && manifestVar.hasProperty ("schema_version")
                  && manifestVar.hasProperty ("bpm")
                  && manifestVar["assets"].isArray();
        if (manifestOk)
        {
            for (const auto& asset : *manifestVar["assets"].getArray())
            {
                if (! asset.isObject() || ! asset.hasProperty ("path")
                    || ! asset.hasProperty ("category") || ! asset.hasProperty ("lufs")
                    || ! asset.hasProperty ("peak_db") || ! asset.hasProperty ("variation_group"))
                {
                    manifestOk = false;
                    break;
                }
            }
        }
        if (! manifestOk)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024 Godot manifest was incomplete" << std::endl;
            return 1;
        }

        packDir.deleteRecursively();
        std::cout << "[SMOKE_TEST] WO-024 adaptive stems, VOX barks & Godot manifest PASS" << std::endl;
    }

    //===========================================================================
    // WO-024B: Deep music theory & multi-section song-form engine
    //===========================================================================
    {
        namespace theory = tcs::theory;
        const auto catalogReq = juce::String (R"({"jsonrpc":"2.0","id":401,"method":"theory.get_catalog","params":{}})");
        juce::var catalogParsed;
        const auto catalogResponse = app.ipcServer.executeJsonRpcOnMessageThread (catalogReq);
        bool catalogOk = juce::JSON::parse (catalogResponse, catalogParsed).wasOk()
                      && ! catalogParsed.hasProperty ("error")
                      && catalogParsed["result"]["keys"].isArray()
                      && catalogParsed["result"]["scales"].isArray()
                      && catalogParsed["result"]["genres"].isArray()
                      && catalogParsed["result"]["keys"].getArray()->size() == 12
                      && catalogParsed["result"]["scales"].getArray()->size() == 16
                      && catalogParsed["result"]["genres"].getArray()->size() == 14;
        if (! catalogOk)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024B theory catalog counts were wrong" << std::endl;
            return 1;
        }

        int euclidPulses = 0;
        for (int hit : theory::MusicTheoryEngine::generateEuclideanPattern (16, 5, 0))
            euclidPulses += hit;
        if (euclidPulses != 5)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024B Euclidean(16,5) pulses=" << euclidPulses << std::endl;
            return 1;
        }

        struct TheoryCase { const char* genre; const char* key; const char* scale; };
        const TheoryCase cases[] {
            { "metal_heavy", "D", "phrygian" },
            { "anime_action", "F#", "hirajoshi" },
            { "cosmic_horror", "C", "locrian" }
        };

        juce::String firstSignature;
        juce::String secondSignature;
        for (const auto& testCase : cases)
        {
            const auto keyPc = theory::MusicTheoryEngine::parseKey (testCase.key);
            const auto intervals = theory::MusicTheoryEngine::scaleIntervals (testCase.scale);
            const auto plan = theory::MusicTheoryEngine::generateStructuredArrangement (
                testCase.genre, testCase.key, testCase.scale, 16, 0.0, 42, "standard", true);
            if (plan.notes.empty() || plan.sections.size() != 5 || plan.trackNames.size() < 5)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-024B arrangement for " << testCase.genre
                          << " was incomplete" << std::endl;
                return 1;
            }

            int melodicNotes = 0;
            juce::String signature;
            for (const auto& note : plan.notes)
            {
                if (note.track < 1 || note.midi < 48 || note.midi > 76)
                    continue;
                ++melodicNotes;
                signature += juce::String (note.midi) + ",";
                if (! theory::MusicTheoryEngine::isInScale (note.midi, keyPc, intervals))
                {
                    std::cerr << "[SMOKE_TEST] FAIL: WO-024B note " << note.midi
                              << " outside " << testCase.scale << std::endl;
                    return 1;
                }
            }
            if (melodicNotes < 8)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-024B no melodic notes for " << testCase.genre << std::endl;
                return 1;
            }
            if (firstSignature.isEmpty()) firstSignature = signature;
            else if (secondSignature.isEmpty()) secondSignature = signature;

            auto* params = new juce::DynamicObject();
            params->setProperty ("genre", testCase.genre);
            params->setProperty ("key", testCase.key);
            params->setProperty ("scale", testCase.scale);
            params->setProperty ("bars", 16);
            params->setProperty ("bpm", 0.0);
            params->setProperty ("seed", 42);
            params->setProperty ("complexity", "standard");
            params->setProperty ("include_counter_melody", true);
            auto* request = new juce::DynamicObject();
            request->setProperty ("jsonrpc", "2.0");
            request->setProperty ("id", 402);
            request->setProperty ("method", "midi.generate_song_arrangement");
            request->setProperty ("params", juce::var (params));
            const auto response = app.ipcServer.executeJsonRpcOnMessageThread (
                juce::JSON::toString (juce::var (request), true));
            juce::var parsed;
            if (! juce::JSON::parse (response, parsed).wasOk() || parsed.hasProperty ("error")
                || ! parsed["result"].isObject()
                || static_cast<int> (parsed["result"]["total_notes"]) < 8
                || parsed["result"]["tracks_created"].getArray() == nullptr
                || parsed["result"]["tracks_created"].getArray()->size() < 5
                || parsed["result"]["sections"].getArray() == nullptr
                || parsed["result"]["sections"].getArray()->size() != 5)
            {
                std::cerr << "[SMOKE_TEST] FAIL: WO-024B song arrangement RPC failed for "
                          << testCase.genre << std::endl;
                return 1;
            }
        }

        const auto other = theory::MusicTheoryEngine::generateStructuredArrangement (
            "anime_action", "F#", "hirajoshi", 16, 0.0, 1337, "standard", true);
        juce::String otherSignature;
        for (const auto& note : other.notes)
            if (note.track >= 1 && note.midi >= 48 && note.midi <= 76)
                otherSignature += juce::String (note.midi) + ",";
        if (firstSignature == secondSignature || secondSignature == otherSignature)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024B genres/seeds produced identical motifs" << std::endl;
            return 1;
        }

        const auto theorySessionFile = tempDir.getChildFile ("aidaw_wo024b_theory.tracktionedit");
        app.currentEditFile = theorySessionFile;
        app.sessionUntitled = false;
        app.saveSession();
        if (! theorySessionFile.existsAsFile() || ! app.loadSessionFromFileSafely (theorySessionFile))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024B arrangement session round-trip failed" << std::endl;
            return 1;
        }
        int reloadedMidiClips = 0;
        for (auto* track : te::getAudioTracks (*app.edit))
            for (auto* clip : track->getClips())
                if (dynamic_cast<te::MidiClip*> (clip) != nullptr)
                    ++reloadedMidiClips;
        theorySessionFile.deleteFile();
        if (reloadedMidiClips < 5)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-024B arrangement MIDI clips did not persist" << std::endl;
            return 1;
        }

        std::cout << "[SMOKE_TEST] WO-024B music theory catalog, scales, song forms & persistence PASS" << std::endl;
    }

    //===========================================================================
    // WO-025: Pathfinder God live RPG soundboard engine
    //===========================================================================
    {
        const auto renderBoard = [] (RpgSoundboardPlugin* board, juce::AudioBuffer<float>& out,
                                     int frames, float mood) -> float
        {
            out.setSize (2, frames);
            out.clear();
            if (auto p = board->getAutomatableParameterByID ("moodIntensity"))
                p->setParameter (mood, juce::sendNotification);
            board->applyToBuffer (te::PluginRenderContext (
                &out, 0, frames, nullptr, 0.0, {}, false, false, false, false));
            return out.getRMSLevel (0, 0, frames);
        };

        auto boardPtr = app.edit->getPluginCache().createNewPlugin (RpgSoundboardPlugin::xmlTypeName, {});
        auto* board = dynamic_cast<RpgSoundboardPlugin*> (boardPtr.get());
        if (board == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 RpgSoundboardPlugin could not be created" << std::endl;
            return 1;
        }
        board->setBiome ("drowned_stars");
        board->initialise (te::PluginInitialisationInfo {
            tracktion::TimePosition::fromSeconds (0.0), 44100.0, 1024 });
        juce::AudioBuffer<float> ambient (2, 4096);
        const float quietRms = renderBoard (board, ambient, 4096, 0.0f);
        if (! std::isfinite (quietRms) || quietRms <= 1.0e-5f)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 ambient bed was silent or non-finite" << std::endl;
            return 1;
        }

        board->setBiome ("boss_altar");
        juce::AudioBuffer<float> crossfade (2, 4096);
        const float bossRms = renderBoard (board, crossfade, 4096, 1.0f);
        if (! std::isfinite (bossRms) || bossRms <= 1.0e-5f || bossRms <= quietRms)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 biome crossfade/mood intensity did not increase energy"
                      << std::endl;
            return 1;
        }

        board->triggerPad (RpgSoundboardPlugin::parseSfx ("fireball_cast"), 0.9f);
        juce::AudioBuffer<float> hitA (2, 2048);
        renderBoard (board, hitA, 2048, 0.5f);
        board->triggerPad (RpgSoundboardPlugin::parseSfx ("sword_clash"), 0.9f);
        juce::AudioBuffer<float> hitB (2, 2048);
        renderBoard (board, hitB, 2048, 0.5f);
        float hitDifference = 0.0f;
        for (int i = 0; i < 2048; i += 16)
            hitDifference += std::abs (hitA.getSample (0, i) - hitB.getSample (0, i));
        if (hitA.getRMSLevel (0, 0, 2048) <= 1.0e-5f
            || hitB.getRMSLevel (0, 0, 2048) <= 1.0e-5f || hitDifference <= 1.0e-5f)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 one-shot pool was silent or not randomized" << std::endl;
            return 1;
        }

        board->deinitialise();
        juce::AudioBuffer<float> afterDeinit (2, 256);
        afterDeinit.clear();
        board->applyToBuffer (te::PluginRenderContext (
            &afterDeinit, 0, 256, nullptr, 0.0, {}, false, false, false, false));
        if (afterDeinit.getMagnitude (0, 256) > 0.0f)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 applyToBuffer was not a no-op after deinitialise" << std::endl;
            return 1;
        }
        boardPtr = nullptr;

        const auto sceneReq = juce::String (R"({"jsonrpc":"2.0","id":501,"method":"rpg.set_scene","params":{"biome":"haunted_crypt","mood_intensity":0.8,"crossfade_sec":0.5}})");
        juce::var sceneParsed;
        const auto sceneResponse = app.ipcServer.executeJsonRpcOnMessageThread (sceneReq);
        const auto sfxReq = juce::String (R"({"jsonrpc":"2.0","id":502,"method":"rpg.trigger_sfx","params":{"sfx_name":"dragon_roar","velocity":0.9}})");
        juce::var sfxParsed;
        const auto sfxResponse = app.ipcServer.executeJsonRpcOnMessageThread (sfxReq);
        if (! juce::JSON::parse (sceneResponse, sceneParsed).wasOk() || sceneParsed.hasProperty ("error")
            || ! juce::JSON::parse (sfxResponse, sfxParsed).wasOk() || sfxParsed.hasProperty ("error"))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 soundboard RPC round-trip failed" << std::endl;
            return 1;
        }

        const auto rpgSession = tempDir.getChildFile ("aidaw_wo025_rpg.tracktionedit");
        app.currentEditFile = rpgSession;
        app.sessionUntitled = false;
        app.saveSession();
        if (! app.loadSessionFromFileSafely (rpgSession))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 soundboard session round-trip failed" << std::endl;
            return 1;
        }
        bool persistedBoard = false;
        for (auto* track : te::getAudioTracks (*app.edit))
            if (track->pluginList.findFirstPluginOfType<RpgSoundboardPlugin>() != nullptr)
                persistedBoard = true;
        rpgSession.deleteFile();
        if (! persistedBoard)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-025 RpgSoundboardPlugin did not persist" << std::endl;
            return 1;
        }

        std::cout << "[SMOKE_TEST] WO-025 procedural RPG soundboard & biome crossfade PASS" << std::endl;
    }

    //===========================================================================
    // WO-026: Pathfinder God live encounter bridge & GM voice toggle
    //===========================================================================
    {
        app.edit->ensureNumberOfAudioTracks (6);
        auto tracks = te::getAudioTracks (*app.edit);
        auto* gmTrack = tracks[tracks.size() - 2];
        gmTrack->setName ("GM Voice");
        auto vocalPtr = app.edit->getPluginCache().createNewPlugin (VocalChannelPlugin::xmlTypeName, {});
        if (vocalPtr == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-026 VocalChannelPlugin could not be created" << std::endl;
            return 1;
        }
        gmTrack->pluginList.insertPlugin (vocalPtr, 0, nullptr);
        vocalPtr = nullptr;

        const auto cue = [] (const char* event) -> juce::var
        {
            auto* params = new juce::DynamicObject();
            params->setProperty ("event", event);
            auto* request = new juce::DynamicObject();
            request->setProperty ("jsonrpc", "2.0");
            request->setProperty ("id", 601);
            request->setProperty ("method", "rpg.apply_encounter_cue");
            request->setProperty ("params", juce::var (params));
            return juce::var (request);
        };

        juce::var bossParsed;
        const auto bossResponse = app.ipcServer.executeJsonRpcOnMessageThread (
            juce::JSON::toString (cue ("boss_reveal"), true));
        juce::var dreadParsed;
        const auto dreadResponse = app.ipcServer.executeJsonRpcOnMessageThread (
            juce::JSON::toString (cue ("cosmic_dread"), true));

        if (! juce::JSON::parse (bossResponse, bossParsed).wasOk() || bossParsed.hasProperty ("error")
            || bossParsed["result"]["biome"].toString() != "boss_altar"
            || static_cast<double> (bossParsed["result"]["mood_intensity"]) < 0.99
            || bossParsed["result"]["gm_voice_preset"].toString().isEmpty()
            || ! juce::JSON::parse (dreadResponse, dreadParsed).wasOk() || dreadParsed.hasProperty ("error")
            || dreadParsed["result"]["biome"].toString() != "drowned_stars"
            || dreadParsed["result"]["sfx_name"].toString() != "eldritch_whisper")
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-026 encounter cue mapping failed" << std::endl;
            return 1;
        }

        // Custom pad WAV + SourceFileReference must survive a session reload.
        const auto padWav = tempDir.getChildFile ("wo026_custom_pad.wav");
        const auto padBuffer = tcs::game::SfxForgeEngine::renderSfx (
            tcs::game::SfxCategory::uiClick, tcs::game::SfxRenderParams {});
        tcs::game::GameAssetExporter::writeWav (padBuffer, padWav, 44100.0, 1);
        {
            auto padsState = app.edit->state.getOrCreateChildWithName ("RPG_CUSTOM_PADS", nullptr);
            auto slot = padsState.getOrCreateChildWithName ("PAD_1", nullptr);
            te::SourceFileReference ref (*app.edit, slot, juce::Identifier ("source"));
            ref.setToFile (padWav, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
        }

        bool boardBeforeReload = false;
        for (auto* track : te::getAudioTracks (*app.edit))
            if (track->pluginList.findFirstPluginOfType<RpgSoundboardPlugin>() != nullptr)
                boardBeforeReload = true;
        if (! boardBeforeReload)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-026 soundboard missing before GM voice toggle" << std::endl;
            return 1;
        }

        const auto voiceParams = new juce::DynamicObject();
        voiceParams->setProperty ("preset", "Deep Trailer / Villain");
        voiceParams->setProperty ("track_index", -1);
        auto* voiceReq = new juce::DynamicObject();
        voiceReq->setProperty ("jsonrpc", "2.0");
        voiceReq->setProperty ("id", 602);
        voiceReq->setProperty ("method", "vo.apply_preset");
        voiceReq->setProperty ("params", juce::var (voiceParams));
        juce::var voiceParsed;
        if (! juce::JSON::parse (app.ipcServer.executeJsonRpcOnMessageThread (
                juce::JSON::toString (juce::var (voiceReq), true)), voiceParsed).wasOk()
            || voiceParsed.hasProperty ("error"))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-026 GM voice preset toggle failed" << std::endl;
            return 1;
        }

        const auto rpg26Session = tempDir.getChildFile ("aidaw_wo026_rpg.tracktionedit");
        app.currentEditFile = rpg26Session;
        app.sessionUntitled = false;
        app.saveSession();
        if (! app.loadSessionFromFileSafely (rpg26Session))
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-026 session round-trip failed" << std::endl;
            return 1;
        }

        bool boardAfterReload = false;
        bool padPathPersisted = false;
        for (auto* track : te::getAudioTracks (*app.edit))
            if (track->pluginList.findFirstPluginOfType<RpgSoundboardPlugin>() != nullptr)
                boardAfterReload = true;
        auto pads = app.edit->state.getChildWithName ("RPG_CUSTOM_PADS");
        if (pads.isValid())
        {
            auto slot = pads.getChildWithName ("PAD_1");
            if (slot.isValid())
                padPathPersisted = slot.getProperty ("source").toString().contains ("wo026_custom_pad");
        }
        rpg26Session.deleteFile();
        padWav.deleteFile();
        if (! boardAfterReload || ! padPathPersisted)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-026 board or custom pad did not persist" << std::endl;
            return 1;
        }

        std::cout << "[SMOKE_TEST] WO-026 live encounter cues, GM voice toggle & custom pad persistence PASS" << std::endl;
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
                 "bounce-to-take DSP, portable filePathResolver, MCP rack.patch/import_file, "
                 "master WAV mixdown export, metronome click/loop transport, virtual MIDI Step Rec, "
                 "two-row layout, CvUtilityPlugin 1-to-4 splitter, 3-band EQ RPC/Undo/persistence, "
                 "live mixer metering, live audio/MIDI recording, track arming, count-in, automation curves, "
                 "Aux FX send/return bus, zoomable views, dark studio theme, YIN audio-to-MIDI pitch transcription, "
                 "plugin editor windows, timeline vertical scroll, Wave-to-MIDI switch, "
                 "CV SVF filter DSP, direction-aware rack pins, clip-transpose transcription, "
                 "factory preset bank, .trkpreset round-trip, Sampler 808 kit, multi-machine rack, "
                 "TR-808, 12-bit ZOH sampler, analog tape saturation & rack.add_module persistence, "
                 "harmonic progression, scale quantize, LUFS auto-balance, section duplicator, "
                 "sidechain Pin 3/4 ducking, multi-take comping, polyphonic audio-to-MIDI, "
                 "cross-platform core isolation, VST3 scanner lifecycle, the original CV envelope/step "
                 "blueprint modules, the WO-021 voice acting suite, and WO-022 booth teleprompter "
                 "and ADR/demo-reel workflow, WO-022B hardware faceplates and workspace suites, "
                 "and the WO-023 game SFX forge / seamless loop renderer, plus WO-024 adaptive "
                 "stems, VOX bark batching and the Godot 4 audio manifest, and the WO-024B music theory "
                 "catalog, scale-pure song forms, seed variation and arrangement persistence, "
                 "and the WO-025 procedural RPG soundboard, biome crossfade and one-shot pool, "
                 "and the WO-026 encounter cues, GM voice toggle and custom pad persistence verified."
              << std::endl;
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
            if (auto* mc = dynamic_cast<MainComponent*> (getContentComponent()))
            {
                mc->guardUnsavedChanges ([] (bool proceed)
                {
                    if (proceed)
                        JUCEApplication::getInstance()->systemRequestedQuit();
                });
                return;
            }

            JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (AIDAWApplication)
