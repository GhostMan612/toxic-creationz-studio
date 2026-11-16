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

    std::cout << "[SMOKE_TEST] PASS: persistence, non-destructive New, atomic Open rollback, "
                 "and VST3 scanner lifecycle verified." << std::endl;
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
