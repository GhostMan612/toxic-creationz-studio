#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <vector>

namespace te = tracktion::engine;

//==============================================================================
// Audacity-style non-destructive waveform editor.
//
// Tracktion plays WaveAudioClips through memory-mapped, time-stretched reads,
// so DSP here never touches the source file: the processed buffer is written to
// a brand-new take .wav and the clip's SourceFileReference is swapped inside an
// UndoManager transaction. Only an EditItemID is retained, never a clip
// pointer, so a reload can never leave this component holding a dead clip.
//==============================================================================
class WaveformEditorComponent : public juce::Component
{
public:
    enum class DspOperation
    {
        normalize,
        reverse,
        fadeIn,
        fadeOut,
        silence
    };

    WaveformEditorComponent();
    ~WaveformEditorComponent() override;

    void setTarget (te::Edit* targetEdit, tracktion::EditItemID clipID);
    void syncFromEdit();

    tracktion::EditItemID getTargetClipID() const noexcept { return targetClipID; }

    // Non-destructive bounce-to-take processor, shared by the UI buttons,
    // DawIpcServer and --smoke-test.
    static bool applyBounceToTakeDsp (te::Edit& edit,
                                      tracktion::EditItemID clipID,
                                      DspOperation op,
                                      double normStart,
                                      double normEnd,
                                      const juce::File& takesDirectory,
                                      juce::File* outBouncedFile = nullptr);

    static juce::File getDefaultTakesDirectory (const juce::File& currentEditFile);

    std::function<void (const juce::String& /*statusMessage*/)> onWaveformBounced;
    std::function<void (int /*trackIndex*/, tracktion::EditItemID /*clipID*/, int /*noteCount*/)> onTranscribedToMidi;
    std::function<juce::File()> getTakesDirectoryCallback;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

private:
    te::WaveAudioClip* resolveWaveClip() const;
    void reloadWaveformSummary();
    void triggerOperation (DspOperation op);
    juce::Rectangle<int> getWaveformBounds() const;
    double xToNorm (int x) const;

    static constexpr int topBarHeight = 32;

    te::Edit* edit = nullptr;
    tracktion::EditItemID targetClipID;

    juce::File cachedFile;
    double cachedSampleRate = 44100.0;
    juce::int64 cachedTotalSamples = 0;
    int cachedChannels = 0;

    std::vector<float> peakMinL, peakMaxL;
    std::vector<float> peakMinR, peakMaxR;

    double selStartNorm = 0.0;
    double selEndNorm   = 1.0;
    double dragAnchorNorm = 0.0;

    juce::Label titleLabel;
    juce::TextButton normalizeButton { "Norm" };
    juce::TextButton reverseButton   { "Rev" };
    juce::TextButton fadeInButton    { "Fade In" };
    juce::TextButton fadeOutButton   { "Fade Out" };
    juce::TextButton silenceButton   { "Silence" };
    juce::TextButton selectAllButton { "All" };
    juce::TextButton toMidiButton    { "To MIDI" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformEditorComponent)
};
