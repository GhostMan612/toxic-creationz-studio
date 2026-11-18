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
    // A live WASAPI real-time callback thread races the rapid headless
    // Edit/RackType teardown that --smoke-test performs, so don't open audio
    // devices on that path.
    const bool headlessSmokeTest =
        juce::JUCEApplicationBase::getCommandLineParameters().contains ("--smoke-test");

    if (! headlessSmokeTest)
        engine.getDeviceManager().initialise (0, 2);

    engine.getPluginManager().createBuiltInType<CvLfoPlugin>();
    engine.getPluginManager().createBuiltInType<CvWobbleFilterPlugin>();
    loadCachedPlugins();

    // Views
    addAndMakeVisible (arrangementView);
    addAndMakeVisible (pianoRoll);
    addAndMakeVisible (mixerComponent);
    addAndMakeVisible (modularRackComponent);

    arrangementView.onClipSelected = [this] (tracktion::EditItemID clipID)
    {
        pianoRoll.setTarget (edit.get(), clipID);
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
    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (scanPluginsButton);
    addAndMakeVisible (mixerToggleButton);
    addAndMakeVisible (rackToggleButton);
    addAndMakeVisible (audioSettingsButton);
    addAndMakeVisible (bpmSlider);
    addAndMakeVisible (statusLabel);

    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
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
        refreshDirtyFromSavedState();
    };

    playButton.onClick = [this]
    {
        if (edit != nullptr)
            edit->getTransport().play (false);
    };

    stopButton.onClick = [this]
    {
        if (edit != nullptr)
            edit->getTransport().stop (false, false);
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

    modularRackComponent.onMixerChanged = [this] { arrangementView.repaint(); };

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

    setSize (1240, 820);
    startTimerHz (30);
    updateWindowTitle();
}

MainComponent::~MainComponent()
{
    stopTimer();
    stopVst3Scan (3000);
    reapRetiredScanThreads();

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
        arrangementView.setEdit (nullptr);
        pianoRoll.setTarget (nullptr, {});
        mixerComponent.setEdit (nullptr);
        modularRackComponent.setEdit (nullptr);

        edit->getTransport().stop (false, false);
        detachStateListener (*edit);
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
void MainComponent::attachEdit (te::Edit& editToAttach)
{
    arrangementView.setEdit (&editToAttach);
    pianoRoll.setTarget (&editToAttach, arrangementView.getSelectedClipID());
    mixerComponent.setEdit (&editToAttach);
    modularRackComponent.setEdit (&editToAttach);
    bpmSlider.setValue (editToAttach.tempoSequence.getTempo (0)->getBpm(), juce::dontSendNotification);
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
        arrangementView.setEdit (nullptr);
        pianoRoll.setTarget (nullptr, {});
        mixerComponent.setEdit (nullptr);
        modularRackComponent.setEdit (nullptr);

        edit->getTransport().stop (false, false);
        detachStateListener (*edit);
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
        arrangementView.setEdit (nullptr);
        pianoRoll.setTarget (nullptr, {});
        mixerComponent.setEdit (nullptr);
        modularRackComponent.setEdit (nullptr);

        edit->getTransport().stop (false, false);
        detachStateListener (*edit);
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

    if (edit != nullptr && edit->getTransport().isPlaying())
    {
        const double posSec = edit->getTransport().getPosition().inSeconds();
        statusLabel.setText ("Playing: " + juce::String (posSec, 2) + "s", juce::dontSendNotification);
        arrangementView.repaint();
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181a1f));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto topBar = area.removeFromTop (36);

    auto addToolbarItem = [&topBar] (juce::Component& comp, int width)
    {
        comp.setBounds (topBar.removeFromLeft (width));
        topBar.removeFromLeft (6);
    };

    addToolbarItem (newButton, 64);
    addToolbarItem (openButton, 74);
    addToolbarItem (saveButton, 64);
    addToolbarItem (saveAsButton, 82);
    addToolbarItem (addTrackButton, 72);
    addToolbarItem (undoButton, 56);
    addToolbarItem (redoButton, 56);

    topBar.removeFromLeft (8);

    addToolbarItem (playButton, 68);
    addToolbarItem (stopButton, 68);

    topBar.removeFromLeft (8);

    addToolbarItem (scanPluginsButton, 100);

    topBar.removeFromLeft (8);

    addToolbarItem (mixerToggleButton, 60);
    addToolbarItem (rackToggleButton, 50);
    addToolbarItem (audioSettingsButton, 110);

    topBar.removeFromLeft (8);

    bpmSlider.setBounds (topBar.removeFromLeft (150));
    topBar.removeFromLeft (10);

    statusLabel.setBounds (topBar);

    const bool showMixer = mixerComponent.isVisible();
    const bool showRack   = modularRackComponent.isVisible();

    if (showMixer || showRack)
    {
        const int available = area.getHeight();
        const int arrangementHeight = juce::roundToInt (available * 0.30f);
        const int pianoHeight       = juce::roundToInt (available * 0.32f);

        arrangementView.setBounds (area.removeFromTop (arrangementHeight));
        area.removeFromTop (4);
        pianoRoll.setBounds (area.removeFromTop (pianoHeight));
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
        pianoRoll.setBounds (area);
    }
}
