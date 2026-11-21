#include "MainComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

//==============================================================================
// Owned, Interruptible Background VST3 Scanner (Cooperative Cancellation)
//==============================================================================
class MainComponent::Vst3ScanThread : public juce::Thread
{
public:
    explicit Vst3ScanThread (MainComponent& ownerComponent, int generationToUse)
        : juce::Thread ("Vst3ScanThread"),
          safeOwner (&ownerComponent),
          generation (generationToUse)
    {
    }

    void run() override
    {
        struct FinishGuard
        {
            explicit FinishGuard (std::atomic<bool>& f) : flag (f) { flag.store (false); }
            ~FinishGuard() { flag.store (true); }
            std::atomic<bool>& flag;
        } finishGuard (scanFinished);

        juce::VST3PluginFormat vst3Format;
        const juce::FileSearchPath searchPaths = vst3Format.getDefaultLocationsToSearch();

        juce::KnownPluginList threadLocalList;

        juce::PluginDirectoryScanner scanner (
            threadLocalList,
            vst3Format,
            searchPaths,
            true,
            juce::File());

        juce::String currentPlugin;

        while (! threadShouldExit() && scanner.scanNextFile (true, currentPlugin))
        {
            auto safeTarget = safeOwner;
            const juce::String pluginNameCopy = currentPlugin;

            juce::MessageManager::callAsync ([safeTarget, pluginNameCopy]
            {
                if (safeTarget != nullptr)
                    safeTarget->statusLabel.setText ("Scanning: " + pluginNameCopy,
                                                     juce::dontSendNotification);
            });
        }

        // Cancelled: never publish a partial list or write the cache file.
        if (threadShouldExit())
            return;

        auto xmlSnapshot = threadLocalList.createXml();
        auto safeTarget = safeOwner;

        // Capture this scan's generation: a publish posted by a superseded scan
        // must not overwrite a newer scan's results or re-enable its button.
        juce::MessageManager::callAsync ([safeTarget, xml = std::move (xmlSnapshot), gen = generation]() mutable
        {
            if (safeTarget == nullptr || safeTarget->isScanAbortRequested()
                || gen != safeTarget->scanGeneration.load())
                return;

            auto& knownList = safeTarget->engine.getPluginManager().knownPluginList;
            knownList.clear();

            if (xml != nullptr)
                knownList.recreateFromXml (*xml);

            safeTarget->saveCachedPlugins();
            safeTarget->mixerComponent.rebuildStrips();
            safeTarget->modularRackComponent.refresh();

            safeTarget->scanPluginsButton.setEnabled (true);
            safeTarget->statusLabel.setText (
                "VST3 Scan Complete. Found " + juce::String (knownList.getNumTypes()) + " plugins.",
                juce::dontSendNotification);
        });
    }

    bool isFinished() const noexcept { return scanFinished.load(); }

private:
    juce::Component::SafePointer<MainComponent> safeOwner;
    const int generation;
    std::atomic<bool> scanFinished { false };
};

//==============================================================================
// MainComponent
//==============================================================================
MainComponent::MainComponent()
{
    setLookAndFeel (&studioLookAndFeel);
    setWantsKeyboardFocus (true);

    // A live WASAPI real-time callback thread races the rapid headless
    // Edit/RackType teardown that --smoke-test performs, so don't open audio
    // devices on that path.
    const bool headlessSmokeTest =
        juce::JUCEApplicationBase::getCommandLineParameters().contains ("--smoke-test");

    if (! headlessSmokeTest)
        engine.getDeviceManager().initialise (2, 2);

    registerCvRackPlugins (engine);
    loadCachedPlugins();

    // Provision the starter riff pack once for interactive sessions. The
    // headless smoke test provisions its own pack into a temp dir instead, so
    // CI never writes into the runner's Documents folder.
    if (! headlessSmokeTest)
        riffLibrary.ensureStarterPack (RiffLibrary::getDefaultLibraryDirectory());

    // Bind the AI co-producer bridge only for real sessions: --smoke-test binds
    // its own loopback port so parallel/CI runs never collide on 9450.
    if (! headlessSmokeTest)
        ipcServer.startListening (9450);

    ipcServer.onEditMutatedByRpc = [this] (const juce::String& summary)
    {
        if (edit != nullptr)
            bpmSlider.setValue (edit->tempoSequence.getTempo (0)->getBpm(), juce::dontSendNotification);

        arrangementView.repaint();
        pianoRoll.repaint();
        mixerComponent.rebuildStrips();
        modularRackComponent.syncFromEdit();
        waveformEditorComponent.syncFromEdit();
        midiKeyboardDock.syncFromEdit();
        syncTransportControlsFromEdit();
        statusLabel.setText (summary, juce::dontSendNotification);
    };

    // Views
    addAndMakeVisible (arrangementView);
    addAndMakeVisible (pianoRoll);
    addAndMakeVisible (mixerComponent);
    addAndMakeVisible (modularRackComponent);
    addAndMakeVisible (bgmGeneratorComponent);
    addAndMakeVisible (waveformEditorComponent);
    waveformEditorComponent.setVisible (false);
    addAndMakeVisible (midiKeyboardDock);
    midiKeyboardDock.setVisible (false);

    mixerComponent.setRecordingManager (&recordingManager);
    arrangementView.setRecordingManager (&recordingManager);
    arrangementView.onTrackArmChanged = [this] { mixerComponent.rebuildStrips(); };

    midiKeyboardDock.getStepBeatsCallback = [this]
    {
        return pianoRoll.getSnapDivision();
    };

    midiKeyboardDock.onLiveMidiMessage = [this] (const juce::MidiMessage& msg)
    {
        recordingManager.captureLiveMidiMessage (msg);
    };

    recordingManager.onRecordingStateChanged = [this] (const juce::String& status)
    {
        recordButton.setToggleState (recordingManager.isRecording(), juce::dontSendNotification);
        arrangementView.repaint();
        pianoRoll.repaint();
        mixerComponent.rebuildStrips();
        syncTransportControlsFromEdit();
        statusLabel.setText (status, juce::dontSendNotification);
    };

    midiKeyboardDock.onNotesRecorded = [this]
    {
        arrangementView.repaint();
        pianoRoll.repaint();
        updateTransportPositionReadout();
    };

    arrangementView.onClipSelected = [this] (tracktion::EditItemID clipID)
    {
        pianoRoll.setTarget (edit.get(), clipID);
        waveformEditorComponent.setTarget (edit.get(), clipID);
    };

    arrangementView.onLoopChanged = [this]
    {
        syncTransportControlsFromEdit();
    };

    waveformEditorComponent.getTakesDirectoryCallback = [this]
    {
        return WaveformEditorComponent::getDefaultTakesDirectory (currentEditFile);
    };

    // RPC bounces land in the same session Takes directory as UI bounces.
    ipcServer.getTakesDirectoryCallback = [this]
    {
        return WaveformEditorComponent::getDefaultTakesDirectory (currentEditFile);
    };

    waveformEditorComponent.onWaveformBounced = [this] (const juce::String& msg)
    {
        arrangementView.repaint();
        statusLabel.setText (msg, juce::dontSendNotification);
    };

    waveformEditorComponent.onTranscribedToMidi = [this] (int /*trackIdx*/, tracktion::EditItemID clipID, int noteCount)
    {
        waveEditorToggleButton.setToggleState (false, juce::dontSendNotification);
        waveformEditorComponent.setVisible (false);
        pianoRoll.setVisible (true);

        arrangementView.setSelectedClipID (clipID);
        pianoRoll.setTarget (edit.get(), clipID);
        mixerComponent.rebuildStrips();
        midiKeyboardDock.syncFromEdit();
        markSessionDirty();
        resized();
        statusLabel.setText ("Transcribed " + juce::String (noteCount) + " MIDI notes",
                             juce::dontSendNotification);
    };

    pianoRoll.onSequenceChanged = [this]
    {
        arrangementView.repaint();
        markSessionDirty();
    };

    // Toolbar
    addAndMakeVisible (newButton);
    addAndMakeVisible (openButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (saveAsButton);
    addAndMakeVisible (exportWavButton);
    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (loopToggleButton);
    addAndMakeVisible (clickToggleButton);
    addAndMakeVisible (countInButton);
    addAndMakeVisible (autoToggleButton);
    addAndMakeVisible (scanPluginsButton);
    addAndMakeVisible (mixerToggleButton);
    addAndMakeVisible (rackToggleButton);
    addAndMakeVisible (keysToggleButton);
    addAndMakeVisible (bgmToggleButton);
    addAndMakeVisible (waveEditorToggleButton);
    addAndMakeVisible (audioSettingsButton);
    addAndMakeVisible (bpmSlider);
    addAndMakeVisible (transportPosLabel);
    addAndMakeVisible (statusLabel);

    transportPosLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    transportPosLabel.setColour (juce::Label::textColourId, juce::Colour (0xff38bdf8));
    transportPosLabel.setJustificationType (juce::Justification::centred);

    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 18);
    bpmSlider.setRange (40.0, 240.0, 1.0);
    bpmSlider.setValue (124.0, juce::dontSendNotification);

    // Session (after the views exist so attachEdit can bind them)
    const auto defaultFile = getDefaultProjectFile();

    if (defaultFile.existsAsFile())
    {
        juce::String error;

        if (loadSessionFromFileSafely (defaultFile, &error))
            statusLabel.setText ("Loaded: " + currentEditFile.getFileName(), juce::dontSendNotification);
    }

    if (edit == nullptr)
    {
        // The startup file exists but could not be read: preserve it on disk and
        // fall back to a fresh Untitled session rather than overwriting it.
        const bool startupFileUnreadable = defaultFile.existsAsFile();
        const auto target = startupFileUnreadable
                                ? getProjectsDir().getNonexistentChildFile ("Untitled", ".tracktionedit", false)
                                : defaultFile;

        createNewSession (target, true, false);
        saveSession();

        if (startupFileUnreadable)
            statusLabel.setText ("Startup file unreadable; created a new session (original preserved).",
                                 juce::dontSendNotification);
    }

    newButton.onClick = [this]
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);

        guardUnsavedChanges ([safeThis] (bool proceed)
        {
            if (safeThis == nullptr || ! proceed)
                return;

            const auto target = safeThis->getProjectsDir()
                                    .getNonexistentChildFile ("Untitled", ".tracktionedit", false);

            safeThis->createNewSession (target, true, true);
            safeThis->statusLabel.setText ("New session: " + safeThis->currentEditFile.getFileName(),
                                           juce::dontSendNotification);
        });
    };

    openButton.onClick = [this]
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);

        guardUnsavedChanges ([safeThis] (bool proceed)
        {
            if (safeThis == nullptr || ! proceed)
                return;

            safeThis->openSessionDialog();
        });
    };

    saveButton.onClick   = [this] { saveSession(); };
    saveAsButton.onClick = [this] { saveSessionAsDialog(); };
    exportWavButton.onClick = [this] { exportMixdownDialog(); };

    addTrackButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        const auto previouslySelectedClip = arrangementView.getSelectedClipID();

        edit->ensureNumberOfAudioTracks (te::getAudioTracks (*edit).size() + 1);
        auto tracks = te::getAudioTracks (*edit);
        tracks.getLast()->setName ("Track " + juce::String (tracks.size()));

        arrangementView.setEdit (edit.get());

        // setEdit() re-selects the first MIDI clip; restore the user's clip.
        if (previouslySelectedClip.isValid())
        {
            arrangementView.setSelectedClipID (previouslySelectedClip);

            if (arrangementView.resolveSelectedClip() == nullptr)
                arrangementView.setEdit (edit.get());
        }

        markSessionDirty();
        mixerComponent.rebuildStrips();
        midiKeyboardDock.syncFromEdit();
    };

    undoButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        // Guarded: a bare undo() on an empty stack is a silent no-op at best.
        auto& undoManager = edit->getUndoManager();

        if (! undoManager.canUndo())
            return;

        undoManager.undo();
        bpmSlider.setValue (edit->tempoSequence.getTempo (0)->getBpm(), juce::dontSendNotification);
        arrangementView.repaint();
        pianoRoll.repaint();
        mixerComponent.rebuildStrips();
        modularRackComponent.syncFromEdit();
        waveformEditorComponent.syncFromEdit();
        midiKeyboardDock.syncFromEdit();
        syncTransportControlsFromEdit();
        refreshDirtyFromSavedState();
    };

    redoButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        // Guarded so clicking Redo with an empty redo stack is a guaranteed
        // no-op (it must never fall through to an undo).
        auto& undoManager = edit->getUndoManager();

        if (! undoManager.canRedo())
            return;

        undoManager.redo();
        bpmSlider.setValue (edit->tempoSequence.getTempo (0)->getBpm(), juce::dontSendNotification);
        arrangementView.repaint();
        pianoRoll.repaint();
        mixerComponent.rebuildStrips();
        modularRackComponent.syncFromEdit();
        waveformEditorComponent.syncFromEdit();
        midiKeyboardDock.syncFromEdit();
        syncTransportControlsFromEdit();
        refreshDirtyFromSavedState();
    };

    playButton.onClick = [this]
    {
        if (edit != nullptr)
            edit->getTransport().play (false);
    };

    stopButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        if (recordingManager.isRecording())
            recordingManager.stopRecording();
        else
            edit->getTransport().stop (false, false);
    };

    recordButton.setClickingTogglesState (false);
    recordButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffdc2626));
    recordButton.setTooltip ("Start or stop live audio/MIDI recording");
    recordButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        if (recordingManager.isRecording())
            recordingManager.stopRecording();
        else
            recordingManager.startRecording();
    };

    loopToggleButton.setClickingTogglesState (true);
    loopToggleButton.setToggleState (false, juce::dontSendNotification);
    loopToggleButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        const bool enabled = loopToggleButton.getToggleState();
        auto& transport = edit->getTransport();

        if (enabled && transport.getLoopRange().isEmpty())
        {
            transport.setLoopRange ({
                tracktion::TimePosition::fromSeconds (0.0),
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (16.0))
            });
        }

        transport.looping = enabled;
        arrangementView.repaint();
        statusLabel.setText (enabled ? "Loop enabled" : "Loop disabled", juce::dontSendNotification);
    };

    clickToggleButton.setClickingTogglesState (true);
    clickToggleButton.setToggleState (false, juce::dontSendNotification);
    clickToggleButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        const bool enabled = clickToggleButton.getToggleState();
        edit->clickTrackGain = 0.7f;
        edit->clickTrackEmphasiseBars = true;
        edit->clickTrackEnabled = enabled;
        statusLabel.setText (enabled ? "Metronome Click: ON" : "Metronome Click: OFF", juce::dontSendNotification);
    };

    countInButton.setClickingTogglesState (true);
    countInButton.setToggleState (false, juce::dontSendNotification);
    countInButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff0284c7));
    countInButton.setTooltip ("Toggle 1-bar pre-roll count-in before recording");
    countInButton.onClick = [this]
    {
        if (edit == nullptr)
            return;

        const int bars = countInButton.getToggleState() ? 1 : 0;
        RecordingManager::setCountInBars (*edit, bars);
        statusLabel.setText (bars > 0 ? "Count-In: 1 Bar" : "Count-In: OFF", juce::dontSendNotification);
    };

    autoToggleButton.setClickingTogglesState (true);
    autoToggleButton.setToggleState (false, juce::dontSendNotification);
    autoToggleButton.setTooltip ("Show drawable track automation curves on timeline");
    autoToggleButton.onClick = [this]
    {
        const bool on = autoToggleButton.getToggleState();
        arrangementView.setAutomationOverlayEnabled (on);
        statusLabel.setText (on ? "Automation Overlay: ON" : "Automation Overlay: OFF", juce::dontSendNotification);
    };

    scanPluginsButton.onClick = [this] { startVst3Scan(); };

    mixerToggleButton.setClickingTogglesState (true);
    mixerToggleButton.setToggleState (true, juce::dontSendNotification);
    mixerToggleButton.onClick = [this]
    {
        mixerComponent.setVisible (mixerToggleButton.getToggleState());
        resized();
    };

    mixerComponent.onMixerChanged = [this] { arrangementView.repaint(); };

    rackToggleButton.setClickingTogglesState (true);
    rackToggleButton.setToggleState (true, juce::dontSendNotification);
    rackToggleButton.onClick = [this]
    {
        modularRackComponent.setVisible (rackToggleButton.getToggleState());
        resized();
    };

    bgmToggleButton.setClickingTogglesState (true);
    bgmToggleButton.setToggleState (true, juce::dontSendNotification);
    bgmToggleButton.onClick = [this]
    {
        bgmGeneratorComponent.setVisible (bgmToggleButton.getToggleState());
        resized();
    };

    bgmGeneratorComponent.onBgmGenerated = [this] (int clipsCreated)
    {
        arrangementView.repaint();
        mixerComponent.rebuildStrips();
        waveformEditorComponent.syncFromEdit();
        midiKeyboardDock.syncFromEdit();
        statusLabel.setText ("BGM Generated (" + juce::String (clipsCreated) + " riff clips)",
                             juce::dontSendNotification);
    };

    keysToggleButton.setClickingTogglesState (true);
    keysToggleButton.setToggleState (false, juce::dontSendNotification);
    keysToggleButton.onClick = [this]
    {
        const bool on = keysToggleButton.getToggleState();
        midiKeyboardDock.setVisible (on);
        resized();

        if (on && midiKeyboardDock.isShowing())
            midiKeyboardDock.focusKeyboard();
    };

    waveEditorToggleButton.setClickingTogglesState (true);
    waveEditorToggleButton.setToggleState (false, juce::dontSendNotification);
    waveEditorToggleButton.onClick = [this]
    {
        const bool on = waveEditorToggleButton.getToggleState();

        waveformEditorComponent.setVisible (on);
        pianoRoll.setVisible (! on);

        if (on && edit != nullptr)
            waveformEditorComponent.setTarget (edit.get(), arrangementView.getSelectedClipID());

        resized();
    };

    audioSettingsButton.onClick = [this]
    {
        auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
            engine.getDeviceManager().deviceManager,
            0, 2, 0, 2, true, false, true, false);
        selector->setSize (520, 420);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (selector.release());
        opts.dialogTitle = "Audio & MIDI Device Settings";
        opts.dialogBackgroundColour = juce::Colour (0xff1e2128);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = true;
        opts.launchAsync();
    };

    bpmSlider.onValueChange = [this]
    {
        if (edit != nullptr)
            edit->tempoSequence.getTempo (0)->setBpm (bpmSlider.getValue());
    };

    setSize (1280, 820);
    startTimerHz (30);
    updateWindowTitle();
}

MainComponent::~MainComponent()
{
    setLookAndFeel (nullptr);
    stopTimer();
    stopVst3Scan (3000);
    reapRetiredScanThreads();

    // Stop accepting RPC connections before the Edit (and its listener) goes away.
    ipcServer.stopListening();

    // Release any still-hung vendor DLL thread instead of triggering ~Thread().
    for (auto& retired : retiredScanThreads)
    {
        retired->signalThreadShouldExit();

        if (! retired->stopThread (500))
            retired.release();
    }
    retiredScanThreads.clear();

    activeFileChooser.reset();
    alertBox.close();

    if (edit != nullptr)
    {
        // Detach every child view (and their ValueTree listeners) BEFORE the
        // Edit is destroyed: ~Edit() fires tree teardown callbacks synchronously.
        recordingManager.onRecordingStateChanged = nullptr;
        recordingManager.setEdit (nullptr);
        arrangementView.setEdit (nullptr);
        pianoRoll.setTarget (nullptr, {});
        mixerComponent.setEdit (nullptr);
        modularRackComponent.setEdit (nullptr);
        bgmGeneratorComponent.setEdit (nullptr);
        ipcServer.setEdit (nullptr);
        waveformEditorComponent.setTarget (nullptr, {});
        midiKeyboardDock.setEdit (nullptr);

        edit->getTransport().stop (false, false);
        detachStateListener (*edit);
        edit->filePathResolver = nullptr;
        edit.reset();
    }
}

//==============================================================================
// Dirty-state tracking
//==============================================================================
bool MainComponent::isTransientEditSubtree (juce::ValueTree tree)
{
    while (tree.isValid())
    {
        const auto type = tree.getType();

        if (type == juce::Identifier ("TRANSPORT") || type == juce::Identifier ("VIEWSTATE"))
            return true;

        tree = tree.getParent();
    }

    return false;
}

void MainComponent::valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged,
                                             const juce::Identifier&)
{
    // Transport/view-state churn (playhead, zoom, selection) is not a content edit.
    if (! suppressDirtyTracking && ! isTransientEditSubtree (treeWhosePropertyHasChanged))
        markSessionDirty();
}

void MainComponent::valueTreeChildAdded (juce::ValueTree& parentTree, juce::ValueTree&)
{
    if (! suppressDirtyTracking && ! isTransientEditSubtree (parentTree))
        markSessionDirty();
}

void MainComponent::valueTreeChildRemoved (juce::ValueTree& parentTree, juce::ValueTree&, int)
{
    if (! suppressDirtyTracking && ! isTransientEditSubtree (parentTree))
        markSessionDirty();
}

void MainComponent::valueTreeChildOrderChanged (juce::ValueTree& parentTree, int, int)
{
    if (! suppressDirtyTracking && ! isTransientEditSubtree (parentTree))
        markSessionDirty();
}

void MainComponent::valueTreeParentChanged (juce::ValueTree& treeWhoseParentHasChanged)
{
    if (! suppressDirtyTracking && ! isTransientEditSubtree (treeWhoseParentHasChanged))
        markSessionDirty();
}

void MainComponent::valueTreeRedirected (juce::ValueTree& treeWhichHasBeenChanged)
{
    if (! suppressDirtyTracking && ! isTransientEditSubtree (treeWhichHasBeenChanged))
        markSessionDirty();
}

void MainComponent::markSessionDirty()
{
    if (suppressDirtyTracking || sessionDirty)
        return;

    sessionDirty = true;
    updateWindowTitle();
}

void MainComponent::snapshotSavedState()
{
    savedEditState = (edit != nullptr) ? getContentState (edit->state) : juce::ValueTree();
}

void MainComponent::refreshDirtyFromSavedState()
{
    if (suppressDirtyTracking || edit == nullptr)
        return;

    // Undo/redo can return the Edit to exactly its saved content; compare
    // against the snapshot instead of unconditionally flagging dirty.
    // Note: this JUCE's isEquivalentTo is order-sensitive, which is safe here
    // — the note list re-sorts by start beat in memory only, the ValueTree
    // children stay in insertion order, and undo restores a removed child at
    // its recorded index.
    const bool clean = savedEditState.isValid()
                       && getContentState (edit->state).isEquivalentTo (savedEditState);

    if (sessionDirty == ! clean)
        return;

    sessionDirty = ! clean;
    updateWindowTitle();
}

// The saved-state comparison covers content only: TRANSPORT (playhead) and
// VIEWSTATE (selection/zoom) churn constantly at runtime and must never make
// an otherwise-untouched session look dirty.
juce::ValueTree MainComponent::getContentState (const juce::ValueTree& state)
{
    auto copy = state.createCopy();

    for (auto type : { juce::Identifier ("TRANSPORT"), juce::Identifier ("VIEWSTATE") })
        if (auto child = copy.getChildWithName (type); child.isValid())
            copy.removeChild (child, nullptr);

    return copy;
}

void MainComponent::attachStateListener (te::Edit& editToWatch)
{
    editToWatch.state.addListener (this);
}

void MainComponent::detachStateListener (te::Edit& editToUnwatch)
{
    editToUnwatch.state.removeListener (this);
}

//==============================================================================
// Paths & plugin cache
//==============================================================================
juce::File MainComponent::getStudioRootDir() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Toxic Creationz Studio");

    dir.createDirectory();
    return dir;
}

juce::File MainComponent::getProjectsDir() const
{
    auto dir = getStudioRootDir().getChildFile ("Projects");
    dir.createDirectory();
    return dir;
}

juce::File MainComponent::getDefaultProjectFile() const
{
    return getProjectsDir().getChildFile ("autosave_session.tracktionedit");
}

juce::File MainComponent::getPluginCacheFile() const
{
    return getStudioRootDir().getChildFile ("known_vst3_plugins.xml");
}

void MainComponent::loadCachedPlugins()
{
    const auto cacheFile = getPluginCacheFile();

    if (! cacheFile.existsAsFile())
        return;

    if (auto xml = juce::XmlDocument::parse (cacheFile))
    {
        auto& knownList = engine.getPluginManager().knownPluginList;
        knownList.clear();
        knownList.recreateFromXml (*xml);

        statusLabel.setText ("Loaded " + juce::String (knownList.getNumTypes()) + " cached VST3 plugins.",
                             juce::dontSendNotification);
    }
}

void MainComponent::saveCachedPlugins()
{
    if (auto xml = engine.getPluginManager().knownPluginList.createXml())
        xml->writeTo (getPluginCacheFile());
}

//==============================================================================
// VST3 scanner lifecycle
//==============================================================================
void MainComponent::startVst3Scan()
{
    stopVst3Scan();

    scanAbortRequested.store (false);
    const int generation = ++scanGeneration;
    scanPluginsButton.setEnabled (false);
    statusLabel.setText ("Scanning VST3 directories...", juce::dontSendNotification);

    scanThread = std::make_unique<Vst3ScanThread> (*this, generation);
    scanThread->startThread();
}

void MainComponent::stopVst3Scan (int timeoutMs)
{
    if (scanThread == nullptr)
        return;

    scanAbortRequested.store (true);
    scanThread->signalThreadShouldExit();
    const bool joined = scanThread->stopThread (timeoutMs);

    if (joined)
    {
        scanThread.reset();
    }
    else
    {
        // Never destroy a thread that may still be running: quarantine and reap later.
        retiredScanThreads.push_back (std::move (scanThread));
    }
}

void MainComponent::reapRetiredScanThreads()
{
    for (auto it = retiredScanThreads.begin(); it != retiredScanThreads.end(); )
    {
        auto& retired = *it;

        if (retired->isFinished() && retired->stopThread (0))
            it = retiredScanThreads.erase (it);
        else
            ++it;
    }
}

bool MainComponent::isVst3ScanRunning() const
{
    return scanThread != nullptr || ! retiredScanThreads.empty();
}

//==============================================================================
// Session management
//==============================================================================
void MainComponent::installEditFilePathResolver (te::Edit& targetEdit)
{
    juce::Component::SafePointer<MainComponent> safeThis (this);

    targetEdit.filePathResolver = [safeThis] (const juce::String& path) -> juce::File
    {
        if (path.isEmpty())
            return {};

        if (juce::File::isAbsolutePath (path))
            return juce::File (path);

        if (safeThis != nullptr)
        {
            const auto editDir = safeThis->currentEditFile.getParentDirectory();

            if (editDir.isDirectory())
            {
                const auto relativeToEdit = editDir.getChildFile (path);

                if (relativeToEdit.existsAsFile())
                    return relativeToEdit;
            }

            const auto takesDir = WaveformEditorComponent::getDefaultTakesDirectory (safeThis->currentEditFile);
            const auto relativeToTakes = takesDir.getChildFile (path);

            if (relativeToTakes.existsAsFile())
                return relativeToTakes;

            const auto relativeToRiffs = RiffLibrary::getDefaultLibraryDirectory().getChildFile (path);

            if (relativeToRiffs.existsAsFile())
                return relativeToRiffs;

            if (editDir.isDirectory())
                return editDir.getChildFile (path);
        }

        return juce::File::getCurrentWorkingDirectory().getChildFile (path);
    };
}

void MainComponent::attachEdit (te::Edit& editToAttach)
{
    installEditFilePathResolver (editToAttach);
    recordingManager.setEdit (&editToAttach);
    arrangementView.setEdit (&editToAttach);
    pianoRoll.setTarget (&editToAttach, arrangementView.getSelectedClipID());
    mixerComponent.setEdit (&editToAttach);
    modularRackComponent.setEdit (&editToAttach);
    bgmGeneratorComponent.setEdit (&editToAttach);
    ipcServer.setEdit (&editToAttach);
    waveformEditorComponent.setTarget (&editToAttach, arrangementView.getSelectedClipID());
    midiKeyboardDock.setEdit (&editToAttach);
    bpmSlider.setValue (editToAttach.tempoSequence.getTempo (0)->getBpm(), juce::dontSendNotification);
    syncTransportControlsFromEdit();
}

void MainComponent::syncTransportControlsFromEdit()
{
    if (edit == nullptr)
        return;

    clickToggleButton.setToggleState (edit->clickTrackEnabled.get(), juce::dontSendNotification);
    loopToggleButton.setToggleState (edit->getTransport().looping.get(), juce::dontSendNotification);
    countInButton.setToggleState (RecordingManager::getCountInBars (*edit) > 0, juce::dontSendNotification);
    recordButton.setToggleState (recordingManager.isRecording(), juce::dontSendNotification);
    updateTransportPositionReadout();
}

void MainComponent::updateTransportPositionReadout()
{
    if (edit == nullptr)
        return;

    const auto bb = edit->tempoSequence.toBarsAndBeats (edit->getTransport().getPosition());
    const juce::String posText = "Bar " + juce::String (bb.bars + 1) + "." + juce::String (bb.getWholeBeats() + 1);

    if (transportPosLabel.getText() != posText)
        transportPosLabel.setText (posText, juce::dontSendNotification);
}

void MainComponent::guardUnsavedChanges (std::function<void (bool)> onProceed)
{
    if (onProceed == nullptr)
        return;

    if (! hasUnsavedChanges())
    {
        onProceed (true);
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);

    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle ("Unsaved Changes")
                       .withMessage ("The current session has unsaved changes. Save before continuing?")
                       .withButton (juce::MessageBoxButtonOptions ("Save", 1).withPreferredAction())
                       .withButton (juce::MessageBoxButtonOptions ("Discard", 2)
                                        .withRole (juce::MessageBoxButtonRole::destroy))
                       .withButton (juce::MessageBoxButtonOptions ("Cancel", 0)
                                        .withRole (juce::MessageBoxButtonRole::cancel))
                       .withAssociatedComponent (this);

    alertBox = juce::AlertWindow::showScopedAsync (options,
        [safeThis, onProceed] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1)  // Save
            {
                safeThis->saveSession ([safeThis, onProceed] (bool saved)
                {
                    if (safeThis != nullptr && saved)
                        onProceed (true);
                });
            }
            else if (result == 2)  // Discard
            {
                onProceed (true);
            }
            // 0 == Cancel: abort the pending action.
        });
}

void MainComponent::createNewSession (const juce::File& targetFile, bool seedDefaultContent, bool untitled)
{
    if (edit != nullptr)
    {
        // Detach views/listeners before ~Edit() runs its synchronous teardown.
        recordingManager.setEdit (nullptr);
        arrangementView.setEdit (nullptr);
        pianoRoll.setTarget (nullptr, {});
        mixerComponent.setEdit (nullptr);
        modularRackComponent.setEdit (nullptr);
        bgmGeneratorComponent.setEdit (nullptr);
        ipcServer.setEdit (nullptr);
        waveformEditorComponent.setTarget (nullptr, {});
        midiKeyboardDock.setEdit (nullptr);

        edit->getTransport().stop (false, false);
        detachStateListener (*edit);
        edit->filePathResolver = nullptr;
        edit.reset();
    }

    currentEditFile = targetFile;
    sessionUntitled = untitled;

    // Non-destructive: never delete targetFile here.
    edit = te::createEmptyEdit (engine, currentEditFile);
    edit->ensureNumberOfAudioTracks (4);
    edit->tempoSequence.getTempo (0)->setBpm (124.0);

    if (seedDefaultContent)
    {
        auto tracks = te::getAudioTracks (*edit);

        if (! tracks.isEmpty())
        {
            auto* track1 = tracks[0];
            track1->setName ("Lead Synth");

            if (auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
                track1->pluginList.insertPlugin (synth, 0, nullptr);

            const tracktion::TimeRange barOne {
                tracktion::TimePosition::fromSeconds (0.0),
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (4.0))
            };

            if (auto* clip = dynamic_cast<te::MidiClip*> (
                    track1->insertNewClip (te::TrackItem::Type::midi, "Intro Riff", barOne, nullptr)))
            {
                auto& seq = clip->getSequence();
                auto& um = edit->getUndoManager();

                seq.addNote (60, tracktion::BeatPosition::fromBeats (0.0), tracktion::BeatDuration::fromBeats (1.0), 110, 0, &um);
                seq.addNote (63, tracktion::BeatPosition::fromBeats (1.0), tracktion::BeatDuration::fromBeats (1.0), 105, 0, &um);
                seq.addNote (67, tracktion::BeatPosition::fromBeats (2.0), tracktion::BeatDuration::fromBeats (1.0), 115, 0, &um);
                seq.addNote (70, tracktion::BeatPosition::fromBeats (3.0), tracktion::BeatDuration::fromBeats (1.0), 120, 0, &um);
            }
        }
    }

    suppressDirtyTracking = true;
    edit->resetChangedStatus();
    sessionDirty = false;
    snapshotSavedState();
    suppressDirtyTracking = false;

    attachStateListener (*edit);
    attachEdit (*edit);
    updateWindowTitle();
}

bool MainComponent::loadSessionFromFileSafely (const juce::File& fileToLoad, juce::String* errorOut)
{
    if (errorOut != nullptr)
        errorOut->clear();

    auto fail = [errorOut] (const juce::String& message)
    {
        if (errorOut != nullptr)
            *errorOut = message;

        return false;
    };

    if (fileToLoad == juce::File() || ! fileToLoad.existsAsFile())
        return fail ("File not found: " + fileToLoad.getFullPathName());

    if (fileToLoad.getSize() == 0)
        return fail ("File is empty: " + fileToLoad.getFileName());

    auto xml = juce::XmlDocument::parse (fileToLoad);

    if (xml == nullptr || ! xml->hasTagName ("EDIT"))
        return fail ("Not a valid Tracktion edit: " + fileToLoad.getFileName());

    auto candidateEdit = te::loadEditFromFile (engine, fileToLoad);

    if (candidateEdit == nullptr)
        return fail ("Failed to load: " + fileToLoad.getFileName());

    if (edit != nullptr)
    {
        // Detach views/listeners before ~Edit() runs its synchronous teardown.
        recordingManager.setEdit (nullptr);
        arrangementView.setEdit (nullptr);
        pianoRoll.setTarget (nullptr, {});
        mixerComponent.setEdit (nullptr);
        modularRackComponent.setEdit (nullptr);
        bgmGeneratorComponent.setEdit (nullptr);
        ipcServer.setEdit (nullptr);
        waveformEditorComponent.setTarget (nullptr, {});
        midiKeyboardDock.setEdit (nullptr);

        edit->getTransport().stop (false, false);
        detachStateListener (*edit);
        edit->filePathResolver = nullptr;
        edit.reset();
    }

    edit = std::move (candidateEdit);
    currentEditFile = fileToLoad;
    sessionUntitled = false;

    suppressDirtyTracking = true;
    edit->resetChangedStatus();
    sessionDirty = false;
    snapshotSavedState();
    suppressDirtyTracking = false;

    attachStateListener (*edit);
    attachEdit (*edit);
    updateWindowTitle();
    return true;
}

void MainComponent::saveSession (std::function<void (bool)> completion)
{
    if (edit == nullptr)
    {
        if (completion != nullptr)
            completion (false);
        return;
    }

    if (sessionUntitled)
    {
        saveSessionAsDialog (completion);
        return;
    }

    if (currentEditFile == juce::File())
        currentEditFile = getDefaultProjectFile();

    currentEditFile.getParentDirectory().createDirectory();

    // Suppress first: flushState() stamps appVersion/modifiedBy properties,
    // which would otherwise trip the listener and flip dirty momentarily.
    suppressDirtyTracking = true;
    edit->flushState();

    bool saved = false;
    if (auto xml = edit->state.createXml())
        saved = xml->writeTo (currentEditFile);

    suppressDirtyTracking = false;

    if (saved)
    {
        edit->resetChangedStatus();
        sessionDirty = false;
        snapshotSavedState();

        statusLabel.setText ("Saved: " + currentEditFile.getFileName(), juce::dontSendNotification);
        updateWindowTitle();
    }
    else
    {
        statusLabel.setText ("Save failed: " + currentEditFile.getFullPathName(), juce::dontSendNotification);
    }

    if (completion != nullptr)
        completion (saved);
}

void MainComponent::saveSessionAsDialog (std::function<void (bool)> completion)
{
    if (edit == nullptr)
    {
        if (completion != nullptr)
            completion (false);
        return;
    }

    const auto suggested = currentEditFile.existsAsFile()
                               ? currentEditFile
                               : getProjectsDir().getChildFile ("untitled.tracktionedit");

    activeFileChooser = std::make_unique<juce::FileChooser> (
        "Save Tracktion Edit As", suggested, "*.tracktionedit");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    activeFileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis, completion] (const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
            {
                if (completion != nullptr)
                    completion (false);
                return;
            }

            auto target = chooser.getResult();

            if (target == juce::File())
            {
                if (completion != nullptr)
                    completion (false);
                return;
            }

            if (! target.hasFileExtension (".tracktionedit"))
                target = target.withFileExtension (".tracktionedit");

            safeThis->currentEditFile = target;
            safeThis->sessionUntitled = false;
            if (safeThis->edit != nullptr)
                safeThis->installEditFilePathResolver (*safeThis->edit);
            safeThis->saveSession (completion);
        });
}

void MainComponent::openSessionDialog()
{
    activeFileChooser = std::make_unique<juce::FileChooser> (
        "Open Tracktion Edit", getProjectsDir(), "*.tracktionedit");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    activeFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis] (const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;

            const auto chosen = chooser.getResult();

            if (chosen == juce::File())
                return;

            juce::String error;

            if (! safeThis->loadSessionFromFileSafely (chosen, &error))
            {
                safeThis->statusLabel.setText (error, juce::dontSendNotification);
                return;
            }

            safeThis->statusLabel.setText ("Opened: " + chosen.getFileName(), juce::dontSendNotification);
        });
}

void MainComponent::exportMixdownDialog()
{
    if (edit == nullptr)
        return;

    const auto startDir = currentEditFile.getParentDirectory().isDirectory()
                              ? currentEditFile.getParentDirectory()
                              : getProjectsDir();

    activeFileChooser = std::make_unique<juce::FileChooser> (
        "Export Master Mixdown WAV...",
        startDir.getChildFile ("MasterMixdown.wav"),
        "*.wav");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    activeFileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis] (const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || safeThis->edit == nullptr)
                return;

            auto target = chooser.getResult();

            if (target == juce::File())
                return;

            if (! target.hasFileExtension (".wav"))
                target = target.withFileExtension (".wav");

            const auto range = MixdownExporter::getDefaultExportRange (*safeThis->edit);
            const bool ok = MixdownExporter::exportEditToWav (*safeThis->edit, target, range);

            safeThis->statusLabel.setText (
                ok ? ("Exported WAV: " + target.getFileName())
                   : ("Export WAV failed: " + target.getFileName()),
                juce::dontSendNotification);
        });
}

void MainComponent::updateWindowTitle()
{
    if (auto* topWindow = findParentComponentOfClass<juce::DocumentWindow>())
    {
        const auto suffix = currentEditFile.exists() ? (" — " + currentEditFile.getFileName())
                                                     : juce::String();

        topWindow->setName (JUCE_APPLICATION_NAME_STRING + suffix + (sessionDirty ? " •" : ""));
    }
}

void MainComponent::timerCallback()
{
    reapRetiredScanThreads();
    updateTransportPositionReadout();
    mixerComponent.refreshMeters();

    if (edit != nullptr && (edit->getTransport().isPlaying() || recordingManager.isRecording()))
    {
        const double posSec = edit->getTransport().getPosition().inSeconds();
        statusLabel.setText (
            (recordingManager.isRecording() ? "Recording: " : "Playing: ") + juce::String (posSec, 2) + "s",
            juce::dontSendNotification);
        arrangementView.repaint();
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff12151c));
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        if (edit != nullptr)
        {
            if (recordingManager.isRecording())
                recordingManager.stopRecording();
            else if (edit->getTransport().isPlaying())
                edit->getTransport().stop (false, false);
            else
                edit->getTransport().play (false);
        }

        return true;
    }

    if (key.getModifiers().isCommandDown())
    {
        const int code = juce::CharacterFunctions::toLowerCase (static_cast<juce::juce_wchar> (key.getKeyCode()));

        if (code == 'z' && ! key.getModifiers().isShiftDown())
        {
            undoButton.triggerClick();
            return true;
        }

        if (code == 'y' || (code == 'z' && key.getModifiers().isShiftDown()))
        {
            redoButton.triggerClick();
            return true;
        }

        if (code == 's')
        {
            saveSession();
            return true;
        }

        const auto ch = key.getTextCharacter();
        if (ch == '=' || ch == '+' || code == '=')
        {
            arrangementView.zoomIn();
            pianoRoll.zoomIn();
            return true;
        }

        if (ch == '-' || code == '-')
        {
            arrangementView.zoomOut();
            pianoRoll.zoomOut();
            return true;
        }
    }

    return false;
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto row1 = area.removeFromTop (30);
    area.removeFromTop (4);
    auto row2 = area.removeFromTop (30);
    area.removeFromTop (6);

    auto addRowItem = [] (juce::Rectangle<int>& row, juce::Component& comp, int width, int gap = 4)
    {
        comp.setBounds (row.removeFromLeft (width));
        row.removeFromLeft (gap);
    };

    // Row 1 Left: Session, Export, Scanner & Device Settings
    addRowItem (row1, newButton, 52);
    addRowItem (row1, openButton, 64);
    addRowItem (row1, saveButton, 54);
    addRowItem (row1, saveAsButton, 74);
    addRowItem (row1, exportWavButton, 84);
    row1.removeFromLeft (8);
    addRowItem (row1, scanPluginsButton, 84);
    addRowItem (row1, audioSettingsButton, 106);

    // Row 1 Right (left-to-right visual order: BGM Gen, Wave Edit, Keys, Rack, Mixer)
    mixerToggleButton.setBounds (row1.removeFromRight (56));
    row1.removeFromRight (4);
    rackToggleButton.setBounds (row1.removeFromRight (52));
    row1.removeFromRight (4);
    keysToggleButton.setBounds (row1.removeFromRight (52));
    row1.removeFromRight (4);
    waveEditorToggleButton.setBounds (row1.removeFromRight (78));
    row1.removeFromRight (4);
    bgmToggleButton.setBounds (row1.removeFromRight (74));

    // Row 2: Transport, Record, Loop/Click/Count-In, Edit actions, BPM, Bar:Beat & Status
    addRowItem (row2, playButton, 46);
    addRowItem (row2, stopButton, 46);
    addRowItem (row2, recordButton, 44);
    addRowItem (row2, loopToggleButton, 46);
    addRowItem (row2, clickToggleButton, 46);
    addRowItem (row2, countInButton, 64);
    addRowItem (row2, autoToggleButton, 44);
    row2.removeFromLeft (4);
    addRowItem (row2, addTrackButton, 58);
    addRowItem (row2, undoButton, 46);
    addRowItem (row2, redoButton, 46);
    row2.removeFromLeft (4);
    bpmSlider.setBounds (row2.removeFromLeft (120));
    row2.removeFromLeft (4);
    transportPosLabel.setBounds (row2.removeFromLeft (78));
    row2.removeFromLeft (6);
    statusLabel.setBounds (row2);

    if (bgmGeneratorComponent.isVisible())
    {
        bgmGeneratorComponent.setBounds (area.removeFromTop (34));
        area.removeFromTop (4);
    }

    if (midiKeyboardDock.isVisible())
    {
        midiKeyboardDock.setBounds (area.removeFromBottom (74));
        area.removeFromBottom (4);
    }

    const bool showMixer = mixerComponent.isVisible();
    const bool showRack   = modularRackComponent.isVisible();

    // Middle pane: the waveform editor replaces the piano roll while Wave Edit
    // is enabled.
    const bool showWaveEditor = waveEditorToggleButton.getToggleState();
    waveformEditorComponent.setVisible (showWaveEditor);
    pianoRoll.setVisible (! showWaveEditor);

    juce::Component* const middlePane = showWaveEditor
                                          ? static_cast<juce::Component*> (&waveformEditorComponent)
                                          : static_cast<juce::Component*> (&pianoRoll);

    if (showMixer || showRack)
    {
        const int available = area.getHeight();
        const int arrangementHeight = juce::roundToInt (available * 0.30f);
        const int pianoHeight       = juce::roundToInt (available * 0.32f);

        arrangementView.setBounds (area.removeFromTop (arrangementHeight));
        area.removeFromTop (4);
        middlePane->setBounds (area.removeFromTop (pianoHeight));
        area.removeFromTop (4);

        const int panelCount = (showMixer ? 1 : 0) + (showRack ? 1 : 0);
        const int gap = 4;
        const int panelHeight = juce::jmax (20, (area.getHeight() - gap * (panelCount - 1)) / panelCount);

        if (showMixer)
        {
            mixerComponent.setBounds (area.removeFromTop (panelHeight));
            if (showRack) area.removeFromTop (gap);
        }
        if (showRack)
            modularRackComponent.setBounds (area);
    }
    else
    {
        const int arrangementHeight = juce::roundToInt (area.getHeight() * 0.44f);
        arrangementView.setBounds (area.removeFromTop (arrangementHeight));
        area.removeFromTop (4);
        middlePane->setBounds (area);
    }
}
