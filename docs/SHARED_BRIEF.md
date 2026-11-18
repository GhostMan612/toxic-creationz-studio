## Pipeline State
- **Latest `main`:** `4105512` (`WO-001` through `WO-006` merged and audited clean)
- **Active Branch / PR:** `feat/wo-007-mcp-ipc-coproducer` (`PR #7`)
- **Builder Status (LongCat):** `WO-007` implemented, 5x Release `--smoke-test` verified green, and pushed — awaiting Muse PR Doctor audit & merge
- **Auditor Status (Muse):** Ready to review `PR #7` (`DawIpcServer`, `mcp/daw_mcp_server.py`, loopback TCP JSON-RPC smoke tests)
- **Next Queued Work Order:** `WO-008` — Phase 5 Audacity-Style Non-Destructive Waveform Editor & Bounce-to-Take DSP (gated on `main` post-`PR #7`)

---

### Work Order Index (LongCat <-> Muse)

| Work Order | Title | State |
| --- | --- | --- |
| WO-001 | Phase 1 Hardening, Session Persistence & Automated Verification | Merged to `main` (`7d67dba`, PR #1) |
| WO-002 | Project Data Safety & Scanner Lifecycle Hardening | Merged to `main` (`77aea26`, PR #2) |
| WO-003 | Multi-Track Arrangement Timeline & FL Studio-Style Piano Roll | Merged to `main` (`f0ea3cb`, PR #3) |
| WO-004 | Interactive Channel Mixer, VST3 Plugin Rack & Audio Device Settings | Merged to `main` (`757e8cc`, PR #4) |
| WO-005 | Reason-Style `RackType` Modular Audio, MIDI & CV Patchbay | Merged to `main` (`5e36347`, PR #5) |
| WO-006 | Phase 3 Riff-Layering BGM Solver, Starter WAV Provisioner & Time-Stretched Audio Clips | Merged to `main` (`4105512`, PR #6) |
| WO-007 | Phase 4 Native JSON-RPC 2.0 IPC Server (`DawIpcServer`) & Python FastMCP Co-Producer Bridge | In review (`feat/wo-007-mcp-ipc-coproducer`, PR #7) |

---

\### Multi-Agent Execution Contract (`Toxic Creationz Studio`)



With \*\*Gemini\*\* as Chief R\&D / Architect, \*\*LongCat 2.5 (Hermes)\*\* as Chief Local Executor on the Windows workstation, \*\*Muse Mobile\*\* as GitHub PR Doctor, \*\*ChatGPT\*\* as Supporting Auditor, and \*\*you\*\* as Project Director, every implementation cycle will follow a strict \*\*Work Order (`WO-XXX`)\*\* pipeline:



```text

\[Gemini: WO-XXX Spec + Verified Code]

&#x20;      │

&#x20;      ▼

\[LongCat 2.5: Branch feat/wo-xxx -> Edit -> MSVC Build -> --smoke-test -> gh pr create]

&#x20;      │

&#x20;      ▼

\[Muse Mobile: Diff \& Lifecycle Audit -> Complete Implementation PR Summary]

&#x20;      │

&#x20;      ▼

\[ChatGPT: Architectural Audit] ──▶ \[Glenn: Final Merge Approval]



```



\*\*Hard Engineering Rules for All Work Orders:\*\*



1\. \*\*Single Local Writer:\*\* Only LongCat 2.5 modifies `C:\\Toxic Creationz Studio` and pushes branches. Mobile agents operate strictly via GitHub PRs and Work Order artifacts.

2\. \*\*Reconciled Tracktion `develop` (`c629312`) / JUCE `9.0.5` API Rules:\*\*

\* `TimePosition`, `TimeDuration`, `TimeRange`, `BeatPosition`, and `BeatDuration` live in `tracktion::` (via `tracktion::inline core`), \*\*never\*\* `tracktion::engine` (`te::`).

\* `AudioTrack::insertNewClip(...)` returns a raw `te::Clip\*` (never call `.get()`).

\* JUCE 9 disables synchronous modal loops by default; all file dialogs must use `juce::FileChooser::launchAsync(...)`.





3\. \*\*License Boundary:\*\* Keep third-party AI workers, Python MCP servers, and any proprietary SDKs strictly out-of-process over localhost TCP (`127.0.0.1:9450`) so the C++ core remains cleanly AGPLv3/GPLv3-compliant during R\&D with a clean commercial upgrade path.



\---



\### Work Order `WO-001`: Phase 1 Hardening, Session Persistence \& Automated Verification



\*\*Target Branch:\*\* `feat/wo-001-phase1-hardening` (from `main` @ `bb79d32`)



\*\*Resolves Audit Findings:\*\*



1\. \*\*Dangling `this` \& Data Race in VST3 Scanner:\*\* Replaces detached `juce::Thread::launch` + raw `this` capture with an owned, interruptible `juce::Thread` (`Vst3ScanThread`), thread-local `juce::KnownPluginList` scanning, `juce::Component::SafePointer<MainComponent>` UI callbacks, and XML plugin cache persistence (`known\_vst3\_plugins.xml`).

2\. \*\*Session Persistence (`.tracktionedit`):\*\* Replaces destructive temp-file deletion on startup with real `New`, `Open...`, `Save`, and `Save As...` workflows backed by `te::EditFileOperations` and a persistent default project directory (`Documents/Toxic Creationz Studio/Projects/`).

3\. \*\*Headless Smoke Test \& GitHub Actions CI:\*\* Adds `--smoke-test` CLI mode to `AI DAW.exe` (exercises `te::Engine`, `te::Edit`, `4OSC`, `MidiClip` serialization/deserialization, and exit codes without requiring an interactive display or audio hardware) plus `.github/workflows/windows-build.yml`.

4\. \*\*Briefing Reconciliation:\*\* Updates `docs/BRIEFING.md` so Part 2's upcoming `DawIpcServer.h` snippet uses `tracktion::` time/beat types and raw `Clip\*` pointers before Phase 4 begins.



\---



\### Complete Implementation Files for LongCat 2.5



\#### 1. `src/MainComponent.h`



```cpp

\#pragma once



\#include <juce\_gui\_basics/juce\_gui\_basics.h>

\#include <juce\_audio\_processors/juce\_audio\_processors.h>

\#include <tracktion\_engine/tracktion\_engine.h>



namespace te = tracktion::engine;



class MainComponent : public juce::Component,

&#x20;                     private juce::Timer

{

public:

&#x20;   MainComponent();

&#x20;   \~MainComponent() override;



&#x20;   void paint (juce::Graphics\& g) override;

&#x20;   void resized() override;



private:

&#x20;   class Vst3ScanThread;



&#x20;   void timerCallback() override;



&#x20;   juce::File getStudioRootDir() const;

&#x20;   juce::File getDefaultProjectFile() const;

&#x20;   juce::File getPluginCacheFile() const;



&#x20;   void loadCachedPlugins();

&#x20;   void saveCachedPlugins();

&#x20;   void startVst3Scan();

&#x20;   void stopVst3Scan();



&#x20;   void createNewSession (const juce::File\& targetFile, bool seedDefaultContent);

&#x20;   void openSessionDialog();

&#x20;   void saveSession();

&#x20;   void saveSessionAsDialog();

&#x20;   void updateWindowTitle();



&#x20;   te::Engine engine { "AI DAW" };

&#x20;   std::unique\_ptr<te::Edit> edit;

&#x20;   juce::File currentEditFile;



&#x20;   std::unique\_ptr<Vst3ScanThread> scanThread;

&#x20;   std::unique\_ptr<juce::FileChooser> activeFileChooser;



&#x20;   juce::TextButton newButton         { "New" };

&#x20;   juce::TextButton openButton        { "Open..." };

&#x20;   juce::TextButton saveButton        { "Save" };

&#x20;   juce::TextButton saveAsButton      { "Save As..." };

&#x20;   juce::TextButton playButton        { "Play" };

&#x20;   juce::TextButton stopButton        { "Stop" };

&#x20;   juce::TextButton scanPluginsButton { "Scan VST3" };

&#x20;   juce::Label statusLabel            { {}, "Ready" };



&#x20;   JUCE\_DECLARE\_NON\_COPYABLE\_WITH\_LEAK\_DETECTOR (MainComponent)

};



```



\#### 2. `src/MainComponent.cpp`



```cpp

\#include "MainComponent.h"



// ============================================================================

// Owned, Interruptible Background VST3 Scanner (Zero Dangling `this`)

// ============================================================================

class MainComponent::Vst3ScanThread : public juce::Thread

{

public:

&#x20;   explicit Vst3ScanThread (MainComponent\& ownerComponent)

&#x20;       : juce::Thread ("Vst3ScanThread"),

&#x20;         safeOwner (\&ownerComponent)

&#x20;   {

&#x20;   }



&#x20;   void run() override

&#x20;   {

&#x20;       juce::VST3PluginFormat vst3Format;

&#x20;       const juce::FileSearchPath searchPaths = vst3Format.getDefaultLocationsToSearch();

&#x20;       juce::KnownPluginList threadLocalList;



&#x20;       juce::PluginDirectoryScanner scanner (

&#x20;           threadLocalList,

&#x20;           vst3Format,

&#x20;           searchPaths,

&#x20;           true,

&#x20;           juce::File());



&#x20;       juce::String currentPlugin;

&#x20;       while (! threadShouldExit() \&\& scanner.scanNextFile (true, currentPlugin))

&#x20;       {

&#x20;           auto safeTarget = safeOwner;

&#x20;           const juce::String pluginNameCopy = currentPlugin;



&#x20;           juce::MessageManager::callAsync (\[safeTarget, pluginNameCopy]

&#x20;           {

&#x20;               if (safeTarget != nullptr)

&#x20;                   safeTarget->statusLabel.setText ("Scanning: " + pluginNameCopy,

&#x20;                                                    juce::dontSendNotification);

&#x20;           });

&#x20;       }



&#x20;       if (threadShouldExit())

&#x20;           return;



&#x20;       auto xmlSnapshot = threadLocalList.createXml();

&#x20;       auto safeTarget = safeOwner;



&#x20;       juce::MessageManager::callAsync (\[safeTarget, xml = std::move (xmlSnapshot)]() mutable

&#x20;       {

&#x20;           if (safeTarget == nullptr)

&#x20;               return;



&#x20;           auto\& knownList = safeTarget->engine.getPluginManager().knownPluginList;

&#x20;           knownList.clear();

&#x20;           if (xml != nullptr)

&#x20;               knownList.recreateFromXml (\*xml);



&#x20;           safeTarget->saveCachedPlugins();

&#x20;           safeTarget->scanPluginsButton.setEnabled (true);

&#x20;           safeTarget->statusLabel.setText (

&#x20;               "VST3 Scan Complete. Found " + juce::String (knownList.getNumTypes()) + " plugins.",

&#x20;               juce::dontSendNotification);

&#x20;       });

&#x20;   }



private:

&#x20;   juce::Component::SafePointer<MainComponent> safeOwner;

};



// ============================================================================

// MainComponent Implementation

// ============================================================================

MainComponent::MainComponent()

{

&#x20;   loadCachedPlugins();



&#x20;   const auto defaultFile = getDefaultProjectFile();

&#x20;   if (defaultFile.existsAsFile())

&#x20;   {

&#x20;       edit = te::loadEditFromFile (engine, defaultFile);

&#x20;       if (edit != nullptr)

&#x20;       {

&#x20;           currentEditFile = defaultFile;

&#x20;           statusLabel.setText ("Loaded: " + currentEditFile.getFileName(), juce::dontSendNotification);

&#x20;       }

&#x20;   }



&#x20;   if (edit == nullptr)

&#x20;   {

&#x20;       createNewSession (defaultFile, true);

&#x20;       saveSession();

&#x20;   }



&#x20;   addAndMakeVisible (newButton);

&#x20;   addAndMakeVisible (openButton);

&#x20;   addAndMakeVisible (saveButton);

&#x20;   addAndMakeVisible (saveAsButton);

&#x20;   addAndMakeVisible (playButton);

&#x20;   addAndMakeVisible (stopButton);

&#x20;   addAndMakeVisible (scanPluginsButton);

&#x20;   addAndMakeVisible (statusLabel);



&#x20;   newButton.onClick = \[this]

&#x20;   {

&#x20;       createNewSession (getDefaultProjectFile(), true);

&#x20;       saveSession();

&#x20;       statusLabel.setText ("Created new session: " + currentEditFile.getFileName(),

&#x20;                            juce::dontSendNotification);

&#x20;   };



&#x20;   openButton.onClick   = \[this] { openSessionDialog(); };

&#x20;   saveButton.onClick   = \[this] { saveSession(); };

&#x20;   saveAsButton.onClick = \[this] { saveSessionAsDialog(); };



&#x20;   playButton.onClick = \[this]

&#x20;   {

&#x20;       if (edit != nullptr)

&#x20;           edit->getTransport().play (false);

&#x20;   };



&#x20;   stopButton.onClick = \[this]

&#x20;   {

&#x20;       if (edit != nullptr)

&#x20;           edit->getTransport().stop (false, false);

&#x20;   };



&#x20;   scanPluginsButton.onClick = \[this] { startVst3Scan(); };



&#x20;   setSize (1040, 620);

&#x20;   startTimerHz (30);

&#x20;   updateWindowTitle();

}



MainComponent::\~MainComponent()

{

&#x20;   stopTimer();

&#x20;   stopVst3Scan();

&#x20;   activeFileChooser.reset();



&#x20;   if (edit != nullptr)

&#x20;   {

&#x20;       edit->getTransport().stop (false, false);

&#x20;       edit.reset();

&#x20;   }

}



juce::File MainComponent::getStudioRootDir() const

{

&#x20;   auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)

&#x20;                  .getChildFile ("Toxic Creationz Studio");

&#x20;   dir.createDirectory();

&#x20;   return dir;

}



juce::File MainComponent::getDefaultProjectFile() const

{

&#x20;   auto projectsDir = getStudioRootDir().getChildFile ("Projects");

&#x20;   projectsDir.createDirectory();

&#x20;   return projectsDir.getChildFile ("autosave\_session.tracktionedit");

}



juce::File MainComponent::getPluginCacheFile() const

{

&#x20;   return getStudioRootDir().getChildFile ("known\_vst3\_plugins.xml");

}



void MainComponent::loadCachedPlugins()

{

&#x20;   const auto cacheFile = getPluginCacheFile();

&#x20;   if (! cacheFile.existsAsFile())

&#x20;       return;



&#x20;   if (auto xml = juce::XmlDocument::parse (cacheFile))

&#x20;   {

&#x20;       auto\& knownList = engine.getPluginManager().knownPluginList;

&#x20;       knownList.clear();

&#x20;       knownList.recreateFromXml (\*xml);

&#x20;       statusLabel.setText ("Loaded " + juce::String (knownList.getNumTypes()) + " cached VST3 plugins.",

&#x20;                            juce::dontSendNotification);

&#x20;   }

}



void MainComponent::saveCachedPlugins()

{

&#x20;   if (auto xml = engine.getPluginManager().knownPluginList.createXml())

&#x20;       xml->writeTo (getPluginCacheFile());

}



void MainComponent::startVst3Scan()

{

&#x20;   stopVst3Scan();

&#x20;   scanPluginsButton.setEnabled (false);

&#x20;   statusLabel.setText ("Scanning VST3 directories...", juce::dontSendNotification);



&#x20;   scanThread = std::make\_unique<Vst3ScanThread> (\*this);

&#x20;   scanThread->startThread();

}



void MainComponent::stopVst3Scan()

{

&#x20;   if (scanThread != nullptr)

&#x20;   {

&#x20;       scanThread->signalThreadShouldExit();

&#x20;       scanThread->stopThread (3000);

&#x20;       scanThread.reset();

&#x20;   }

}



void MainComponent::createNewSession (const juce::File\& targetFile, bool seedDefaultContent)

{

&#x20;   if (edit != nullptr)

&#x20;   {

&#x20;       edit->getTransport().stop (false, false);

&#x20;       edit.reset();

&#x20;   }



&#x20;   targetFile.getParentDirectory().createDirectory();

&#x20;   if (targetFile.existsAsFile())

&#x20;       targetFile.deleteFile();



&#x20;   currentEditFile = targetFile;

&#x20;   edit = te::createEmptyEdit (engine, currentEditFile);

&#x20;   edit->ensureNumberOfAudioTracks (4);

&#x20;   edit->tempoSequence.getTempo (0)->setBpm (124.0);



&#x20;   if (seedDefaultContent)

&#x20;   {

&#x20;       auto tracks = te::getAudioTracks (\*edit);

&#x20;       if (! tracks.isEmpty())

&#x20;       {

&#x20;           auto\* track1 = tracks\[0];

&#x20;           track1->setName ("Lead Synth");



&#x20;           if (auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))

&#x20;               track1->pluginList.insertPlugin (synth, 0, nullptr);



&#x20;           const tracktion::TimeRange barOne {

&#x20;               tracktion::TimePosition::fromSeconds (0.0),

&#x20;               edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (4.0))

&#x20;           };



&#x20;           if (auto\* clip = dynamic\_cast<te::MidiClip\*> (

&#x20;                   track1->insertNewClip (te::TrackItem::Type::midi, "Intro Riff", barOne, nullptr)))

&#x20;           {

&#x20;               auto\& seq = clip->getSequence();

&#x20;               auto\& um = edit->getUndoManager();

&#x20;               seq.addNote (60, tracktion::BeatPosition::fromBeats (0.0), tracktion::BeatDuration::fromBeats (1.0), 110, 0, \&um);

&#x20;               seq.addNote (63, tracktion::BeatPosition::fromBeats (1.0), tracktion::BeatDuration::fromBeats (1.0), 105, 0, \&um);

&#x20;               seq.addNote (67, tracktion::BeatPosition::fromBeats (2.0), tracktion::BeatDuration::fromBeats (1.0), 115, 0, \&um);

&#x20;               seq.addNote (70, tracktion::BeatPosition::fromBeats (3.0), tracktion::BeatDuration::fromBeats (1.0), 120, 0, \&um);

&#x20;           }

&#x20;       }

&#x20;   }



&#x20;   updateWindowTitle();

}



void MainComponent::saveSession()

{

&#x20;   if (edit == nullptr)

&#x20;       return;



&#x20;   if (currentEditFile == juce::File())

&#x20;       currentEditFile = getDefaultProjectFile();



&#x20;   edit->editFile = currentEditFile;

&#x20;   te::EditFileOperations fileOps (\*edit);

&#x20;   if (fileOps.save (true, true, false))

&#x20;   {

&#x20;       statusLabel.setText ("Saved: " + currentEditFile.getFileName(), juce::dontSendNotification);

&#x20;       updateWindowTitle();

&#x20;   }

&#x20;   else

&#x20;   {

&#x20;       statusLabel.setText ("Save failed: " + currentEditFile.getFullPathName(), juce::dontSendNotification);

&#x20;   }

}



void MainComponent::openSessionDialog()

{

&#x20;   auto projectsDir = getStudioRootDir().getChildFile ("Projects");

&#x20;   activeFileChooser = std::make\_unique<juce::FileChooser> (

&#x20;       "Open Tracktion Edit",

&#x20;       projectsDir,

&#x20;       "\*.tracktionedit");



&#x20;   juce::Component::SafePointer<MainComponent> safeThis (this);

&#x20;   activeFileChooser->launchAsync (

&#x20;       juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,

&#x20;       \[safeThis] (const juce::FileChooser\& chooser)

&#x20;       {

&#x20;           if (safeThis == nullptr)

&#x20;               return;



&#x20;           const auto chosen = chooser.getResult();

&#x20;           if (! chosen.existsAsFile())

&#x20;               return;



&#x20;           if (safeThis->edit != nullptr)

&#x20;           {

&#x20;               safeThis->edit->getTransport().stop (false, false);

&#x20;               safeThis->edit.reset();

&#x20;           }



&#x20;           safeThis->edit = te::loadEditFromFile (safeThis->engine, chosen);

&#x20;           if (safeThis->edit != nullptr)

&#x20;           {

&#x20;               safeThis->currentEditFile = chosen;

&#x20;               safeThis->statusLabel.setText ("Opened: " + chosen.getFileName(), juce::dontSendNotification);

&#x20;               safeThis->updateWindowTitle();

&#x20;           }

&#x20;           else

&#x20;           {

&#x20;               safeThis->statusLabel.setText ("Failed to open: " + chosen.getFileName(), juce::dontSendNotification);

&#x20;           }

&#x20;       });

}



void MainComponent::saveSessionAsDialog()

{

&#x20;   if (edit == nullptr)

&#x20;       return;



&#x20;   auto projectsDir = getStudioRootDir().getChildFile ("Projects");

&#x20;   activeFileChooser = std::make\_unique<juce::FileChooser> (

&#x20;       "Save Tracktion Edit As",

&#x20;       projectsDir.getChildFile ("untitled.tracktionedit"),

&#x20;       "\*.tracktionedit");



&#x20;   juce::Component::SafePointer<MainComponent> safeThis (this);

&#x20;   activeFileChooser->launchAsync (

&#x20;       juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles

&#x20;           | juce::FileBrowserComponent::warnAboutOverwriting,

&#x20;       \[safeThis] (const juce::FileChooser\& chooser)

&#x20;       {

&#x20;           if (safeThis == nullptr || safeThis->edit == nullptr)

&#x20;               return;



&#x20;           auto target = chooser.getResult();

&#x20;           if (target == juce::File())

&#x20;               return;



&#x20;           if (! target.hasFileExtension (".tracktionedit"))

&#x20;               target = target.withFileExtension (".tracktionedit");



&#x20;           safeThis->currentEditFile = target;

&#x20;           safeThis->saveSession();

&#x20;       });

}



void MainComponent::updateWindowTitle()

{

&#x20;   if (auto\* topWindow = findParentComponentOfClass<juce::DocumentWindow>())

&#x20;   {

&#x20;       const auto suffix = currentEditFile.exists() ? (" — " + currentEditFile.getFileName()) : juce::String();

&#x20;       topWindow->setName (JUCE\_APPLICATION\_NAME\_STRING + suffix);

&#x20;   }

}



void MainComponent::timerCallback()

{

&#x20;   if (edit != nullptr \&\& edit->getTransport().isPlaying())

&#x20;   {

&#x20;       const double posSec = edit->getTransport().getPosition().inSeconds();

&#x20;       statusLabel.setText ("Playing: " + juce::String (posSec, 2) + "s", juce::dontSendNotification);

&#x20;   }

}



void MainComponent::paint (juce::Graphics\& g)

{

&#x20;   g.fillAll (juce::Colour (0xff181a1f));

}



void MainComponent::resized()

{

&#x20;   auto area = getLocalBounds().reduced (12);

&#x20;   auto topBar = area.removeFromTop (36);



&#x20;   auto addToolbarBtn = \[\&topBar] (juce::TextButton\& btn, int width)

&#x20;   {

&#x20;       btn.setBounds (topBar.removeFromLeft (width));

&#x20;       topBar.removeFromLeft (6);

&#x20;   };



&#x20;   addToolbarBtn (newButton, 64);

&#x20;   addToolbarBtn (openButton, 74);

&#x20;   addToolbarBtn (saveButton, 64);

&#x20;   addToolbarBtn (saveAsButton, 82);

&#x20;   topBar.removeFromLeft (8);

&#x20;   addToolbarBtn (playButton, 68);

&#x20;   addToolbarBtn (stopButton, 68);

&#x20;   topBar.removeFromLeft (8);

&#x20;   addToolbarBtn (scanPluginsButton, 100);

&#x20;   topBar.removeFromLeft (10);



&#x20;   statusLabel.setBounds (topBar);

}



```



\#### 3. `src/Main.cpp` (With Headless `--smoke-test` Harness)



```cpp

\#include "MainComponent.h"

\#include <iostream>



namespace

{

int runHeadlessSmokeTest()

{

&#x20;   std::cout << "\[SMOKE\_TEST] Initializing Tracktion Engine..." << std::endl;

&#x20;   te::Engine engine { "AI DAW SmokeTest" };



&#x20;   const auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)

&#x20;                             .getChildFile ("aidaw\_ci\_smoke.tracktionedit");

&#x20;   tempFile.deleteFile();



&#x20;   {

&#x20;       auto edit = te::createEmptyEdit (engine, tempFile);

&#x20;       if (edit == nullptr)

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: createEmptyEdit returned nullptr" << std::endl;

&#x20;           return 1;

&#x20;       }



&#x20;       edit->ensureNumberOfAudioTracks (4);

&#x20;       edit->tempoSequence.getTempo (0)->setBpm (128.0);



&#x20;       auto tracks = te::getAudioTracks (\*edit);

&#x20;       if (tracks.size() != 4)

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: expected 4 tracks, got " << tracks.size() << std::endl;

&#x20;           return 1;

&#x20;       }



&#x20;       auto\* track1 = tracks\[0];

&#x20;       track1->setName ("Smoke Lead");



&#x20;       auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});

&#x20;       if (synth == nullptr)

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: failed to instantiate FourOscPlugin" << std::endl;

&#x20;           return 1;

&#x20;       }

&#x20;       track1->pluginList.insertPlugin (synth, 0, nullptr);



&#x20;       const tracktion::TimeRange barOne {

&#x20;           tracktion::TimePosition::fromSeconds (0.0),

&#x20;           edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (4.0))

&#x20;       };



&#x20;       auto\* clip = dynamic\_cast<te::MidiClip\*> (

&#x20;           track1->insertNewClip (te::TrackItem::Type::midi, "Smoke Riff", barOne, nullptr));

&#x20;       if (clip == nullptr)

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: insertNewClip did not return MidiClip\*" << std::endl;

&#x20;           return 1;

&#x20;       }



&#x20;       auto\& seq = clip->getSequence();

&#x20;       auto\& um = edit->getUndoManager();

&#x20;       seq.addNote (60, tracktion::BeatPosition::fromBeats (0.0), tracktion::BeatDuration::fromBeats (1.0), 100, 0, \&um);

&#x20;       seq.addNote (64, tracktion::BeatPosition::fromBeats (1.0), tracktion::BeatDuration::fromBeats (1.0), 105, 0, \&um);

&#x20;       seq.addNote (67, tracktion::BeatPosition::fromBeats (2.0), tracktion::BeatDuration::fromBeats (1.0), 110, 0, \&um);

&#x20;       seq.addNote (72, tracktion::BeatPosition::fromBeats (3.0), tracktion::BeatDuration::fromBeats (1.0), 115, 0, \&um);



&#x20;       edit->editFile = tempFile;

&#x20;       te::EditFileOperations fileOps (\*edit);

&#x20;       if (! fileOps.save (false, true, false))

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: EditFileOperations::save failed" << std::endl;

&#x20;           return 1;

&#x20;       }

&#x20;   }



&#x20;   // Reload from disk and verify round-trip persistence

&#x20;   {

&#x20;       auto reloaded = te::loadEditFromFile (engine, tempFile);

&#x20;       if (reloaded == nullptr)

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: loadEditFromFile returned nullptr" << std::endl;

&#x20;           tempFile.deleteFile();

&#x20;           return 1;

&#x20;       }



&#x20;       auto tracks = te::getAudioTracks (\*reloaded);

&#x20;       if (tracks.size() != 4)

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: reloaded track count mismatch" << std::endl;

&#x20;           tempFile.deleteFile();

&#x20;           return 1;

&#x20;       }



&#x20;       int totalNotes = 0;

&#x20;       for (auto\* c : tracks\[0]->getClips())

&#x20;           if (auto\* mc = dynamic\_cast<te::MidiClip\*> (c))

&#x20;               totalNotes += mc->getSequence().getNumNotes();



&#x20;       if (totalNotes != 4)

&#x20;       {

&#x20;           std::cerr << "\[SMOKE\_TEST] FAIL: expected 4 reloaded MIDI notes, got " << totalNotes << std::endl;

&#x20;           tempFile.deleteFile();

&#x20;           return 1;

&#x20;       }

&#x20;   }



&#x20;   tempFile.deleteFile();

&#x20;   std::cout << "\[SMOKE\_TEST] PASS: Engine, 4OSC, MIDI sequence, and .tracktionedit round-trip verified." << std::endl;

&#x20;   return 0;

}

} // namespace



class AIDAWApplication : public juce::JUCEApplication

{

public:

&#x20;   const juce::String getApplicationName() override    { return JUCE\_APPLICATION\_NAME\_STRING; }

&#x20;   const juce::String getApplicationVersion() override { return JUCE\_APPLICATION\_VERSION\_STRING; }

&#x20;   bool moreThanOneInstanceAllowed() override          { return false; }



&#x20;   void initialise (const juce::String\& commandLine) override

&#x20;   {

&#x20;       if (commandLine.contains ("--smoke-test"))

&#x20;       {

&#x20;           setApplicationReturnValue (runHeadlessSmokeTest());

&#x20;           quit();

&#x20;           return;

&#x20;       }



&#x20;       mainWindow = std::make\_unique<MainWindow> (getApplicationName());

&#x20;   }



&#x20;   void shutdown() override

&#x20;   {

&#x20;       mainWindow.reset();

&#x20;   }



&#x20;   void systemRequestedQuit() override

&#x20;   {

&#x20;       quit();

&#x20;   }



&#x20;   class MainWindow : public juce::DocumentWindow

&#x20;   {

&#x20;   public:

&#x20;       explicit MainWindow (juce::String name)

&#x20;           : DocumentWindow (name, juce::Colour (0xff181a1f), DocumentWindow::allButtons)

&#x20;       {

&#x20;           setUsingNativeTitleBar (true);

&#x20;           setContentOwned (new MainComponent(), true);

&#x20;           setResizable (true, true);

&#x20;           centreWithSize (getWidth(), getHeight());

&#x20;           setVisible (true);

&#x20;       }



&#x20;       void closeButtonPressed() override

&#x20;       {

&#x20;           JUCEApplication::getInstance()->systemRequestedQuit();

&#x20;       }

&#x20;   };



private:

&#x20;   std::unique\_ptr<MainWindow> mainWindow;

};



START\_JUCE\_APPLICATION (AIDAWApplication)



```



\#### 4. `.github/workflows/windows-build.yml`



```yaml

name: Windows Build \& Smoke Test



on:

&#x20; push:

&#x20;   branches: \[ main ]

&#x20; pull\_request:

&#x20;   branches: \[ main ]



jobs:

&#x20; build-and-smoke-test:

&#x20;   runs-on: windows-latest



&#x20;   steps:

&#x20;     - name: Checkout repository with submodules

&#x20;       uses: actions/checkout@v4

&#x20;       with:

&#x20;         submodules: recursive



&#x20;     - name: Configure CMake (VS 2022 x64)

&#x20;       run: cmake -B build -S . -G "Visual Studio 17 2022" -A x64



&#x20;     - name: Build Release

&#x20;       run: cmake --build build --config Release -j 4



&#x20;     - name: Run Headless Engine Smoke Test

&#x20;       run: |

&#x20;         \& ".\\build\\AIDAW\_artefacts\\Release\\AI DAW.exe" --smoke-test

&#x20;         if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }



```



\---



\### Execution Commands for LongCat 2.5 (Hermes)



Copy/paste this block directly to LongCat 2.5 in `C:\\Toxic Creationz Studio`:



```powershell

\# 1. Create feature branch for WO-001

git checkout -b feat/wo-001-phase1-hardening



\# 2. Write src/MainComponent.h, src/MainComponent.cpp, src/Main.cpp,

\#    and .github/workflows/windows-build.yml from WO-001 above.

\# 3. Also patch docs/BRIEFING.md: replace any remaining `te::TimeRange`,

\#    `te::TimePosition`, `te::BeatPosition`, `te::BeatDuration` with `tracktion::\*`

\#    and remove `.get()` on `insertNewClip(...)` inside the DawIpcServer reference snippet.



\# 4. Build \& run headless smoke test

$CM = "C:\\Users\\612co\\AppData\\Local\\Microsoft\\WinGet\\Links\\cmake.exe"

\& $CM -B build -S . -G "Visual Studio 17 2022" -A x64

\& $CM --build build --config Release -j 8

\& ".\\build\\AIDAW\_artefacts\\Release\\AI DAW.exe" --smoke-test

echo "SMOKE\_EXIT=$LASTEXITCODE"



\# 5. Commit, push branch, and open PR for Muse Mobile review

git add -A

git commit -m "WO-001: safe async VST3 scanner, session persistence, smoke test \& Windows CI"

git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin feat/wo-001-phase1-hardening

gh pr create --base main --head feat/wo-001-phase1-hardening `

&#x20; --title "WO-001: Phase 1 Hardening, Session Persistence \& Windows CI" `

&#x20; --body "Implements thread-safe Vst3ScanThread with SafePointer lifecycle protection, .tracktionedit New/Open/Save/Save As persistence, --smoke-test CLI verification, GitHub Actions Windows CI, and docs/BRIEFING.md API reconciliation."



```



\---



\### Acceptance Checklist for Muse Mobile (PR Doctor)



When LongCat opens the `WO-001` PR, Muse should verify these four gates before recommending merge:



1\. \*\*Zero Dangling `this`:\*\* `MainComponent.cpp` contains zero calls to `juce::Thread::launch`; `Vst3ScanThread` captures `juce::Component::SafePointer<MainComponent>` for every `MessageManager::callAsync` and is joined in `\~MainComponent()`.

2\. \*\*Thread-Safe Plugin List Mutation:\*\* `PluginDirectoryScanner` writes to a thread-local `juce::KnownPluginList` and merges into `engine.getPluginManager().knownPluginList` exclusively on the message thread.

3\. \*\*JUCE 9 Non-Blocking Dialogs:\*\* `openSessionDialog()` and `saveSessionAsDialog()` use `juce::FileChooser::launchAsync(...)` with `SafePointer<MainComponent>` (no synchronous `browseForFileToOpen()`).

4\. \*\*CI \& Smoke Test Evidence:\*\* GitHub Actions `build-and-smoke-test` passes green and prints `\[SMOKE\_TEST] PASS` with exit code `0`.

