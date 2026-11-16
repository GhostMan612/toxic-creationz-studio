#include "MainComponent.h"

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

        if (auto bassSynth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
            track2->pluginList.insertPlugin (bassSynth, 0, nullptr);

        app.mixerComponent.rebuildStrips();

        const auto mixerDir = tempDir.getChildFile ("tcs_wo004_mixer_smoke");
        mixerDir.deleteRecursively();
        mixerDir.createDirectory();

        const auto mixerTestFile = mixerDir.getChildFile ("MixerRoundTrip.tracktionedit");

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
        bool foundTrack2Synth = false;

        for (auto* p : reloadedTracks[1]->pluginList)
            if (dynamic_cast<te::FourOscPlugin*> (p) != nullptr)
                foundTrack2Synth = true;

        if (reloadedVol == nullptr
            || std::abs (reloadedVol->getVolumeDb() - (-6.5f)) > 0.05f
            || std::abs (reloadedVol->getPan() - 0.35f) > 0.02f
            || ! foundTrack2Synth)
        {
            std::cerr << "[SMOKE_TEST] FAIL: WO-004 mixer dB/pan or Track 2 plugin did not persist!" << std::endl;
            return 1;
        }

        mixerDir.deleteRecursively();
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
                 "persistence, and VST3 scanner lifecycle verified." << std::endl;
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
