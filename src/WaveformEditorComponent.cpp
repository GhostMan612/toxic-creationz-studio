#include "WaveformEditorComponent.h"
#include "AudioToMidiTranscriber.h"

#include <algorithm>
#include <cmath>

WaveformEditorComponent::WaveformEditorComponent()
{
    setOpaque (true);

    addAndMakeVisible (titleLabel);
    titleLabel.setText ("WAVEFORM EDITOR", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff34d399));

    for (auto* btn : { &normalizeButton, &reverseButton, &fadeInButton,
                       &fadeOutButton, &silenceButton, &selectAllButton, &toMidiButton })
    {
        addAndMakeVisible (btn);
    }

    normalizeButton.onClick = [this] { triggerOperation (DspOperation::normalize); };
    reverseButton.onClick   = [this] { triggerOperation (DspOperation::reverse); };
    fadeInButton.onClick    = [this] { triggerOperation (DspOperation::fadeIn); };
    fadeOutButton.onClick   = [this] { triggerOperation (DspOperation::fadeOut); };
    silenceButton.onClick   = [this] { triggerOperation (DspOperation::silence); };

    selectAllButton.onClick = [this]
    {
        selStartNorm = 0.0;
        selEndNorm   = 1.0;
        repaint();
    };

    toMidiButton.onClick = [this]
    {
        if (edit == nullptr || ! targetClipID.isValid())
            return;

        const auto res = AudioToMidiTranscriber::transcribeClipToMidi (*edit, targetClipID, -1, {});

        if (res.success && onTranscribedToMidi)
            onTranscribedToMidi (res.targetTrackIndex, res.createdClipID, static_cast<int> (res.notes.size()));
        else if (! res.success && onWaveformBounced && res.errorMessage.isNotEmpty())
            onWaveformBounced ("Transcribe failed: " + res.errorMessage);
    };
}

WaveformEditorComponent::~WaveformEditorComponent()
{
    setTarget (nullptr, {});
}

juce::File WaveformEditorComponent::getDefaultTakesDirectory (const juce::File& currentEditFile)
{
    if (currentEditFile != juce::File() && currentEditFile.getParentDirectory().isDirectory())
        return currentEditFile.getParentDirectory().getChildFile ("Takes");

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
             .getChildFile ("Toxic Creationz Studio")
             .getChildFile ("Takes");
}

void WaveformEditorComponent::setTarget (te::Edit* targetEdit, tracktion::EditItemID clipID)
{
    edit = targetEdit;
    targetClipID = clipID;

    // No valid WaveAudioClip ID was supplied: auto-bind to the first one.
    if (edit != nullptr && ! clipID.isValid() && resolveWaveClip() == nullptr)
    {
        for (auto* track : te::getAudioTracks (*edit))
        {
            for (auto* clip : track->getClips())
            {
                if (auto* wc = dynamic_cast<te::WaveAudioClip*> (clip))
                {
                    targetClipID = wc->itemID;
                    break;
                }
            }

            if (resolveWaveClip() != nullptr)
                break;
        }
    }

    cachedFile = juce::File();
    reloadWaveformSummary();
    repaint();
}

void WaveformEditorComponent::syncFromEdit()
{
    // Strictly read-only sync: never touch UndoManager.
    cachedFile = juce::File();
    reloadWaveformSummary();
    repaint();
}

te::WaveAudioClip* WaveformEditorComponent::resolveWaveClip() const
{
    if (edit == nullptr || ! targetClipID.isValid())
        return nullptr;

    for (auto* track : te::getAudioTracks (*edit))
        for (auto* clip : track->getClips())
            if (clip->itemID == targetClipID)
                return dynamic_cast<te::WaveAudioClip*> (clip);

    return nullptr;
}

void WaveformEditorComponent::reloadWaveformSummary()
{
    peakMinL.clear();
    peakMaxL.clear();
    peakMinR.clear();
    peakMaxR.clear();

    cachedTotalSamples = 0;
    cachedChannels = 0;

    auto* clip = resolveWaveClip();

    if (clip == nullptr)
        return;

    const juce::File sourceFile = clip->getSourceFileReference().getFile();

    if (! sourceFile.existsAsFile())
        return;

    cachedFile = sourceFile;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (sourceFile));

    if (reader == nullptr || reader->lengthInSamples <= 0)
        return;

    cachedSampleRate   = reader->sampleRate;
    cachedTotalSamples = reader->lengthInSamples;
    cachedChannels     = static_cast<int> (reader->numChannels);

    constexpr int numBuckets = 512;

    peakMinL.assign (numBuckets, 0.0f);
    peakMaxL.assign (numBuckets, 0.0f);
    peakMinR.assign (numBuckets, 0.0f);
    peakMaxR.assign (numBuckets, 0.0f);

    // Stride across the WHOLE file in bounded chunks. The old code read only
    // the first 60 s (44100 * 60 samples), so for longer clips the displayed
    // waveform was a stretched view of the first minute while the DSP
    // selection mapped onto the full file — the region shown was not the
    // region processed. Bucket b covers [b * total / 512, (b + 1) * total / 512).
    constexpr int chunkSize = 65536;
    juce::AudioBuffer<float> chunk (std::max (1, cachedChannels), chunkSize);

    juce::int64 samplesRead = 0;

    while (samplesRead < reader->lengthInSamples)
    {
        const int toRead = static_cast<int> (std::min<juce::int64> (chunkSize, reader->lengthInSamples - samplesRead));

        if (! reader->read (&chunk, 0, toRead, samplesRead, true, true))
            break;

        const float* left  = chunk.getReadPointer (0);
        const float* right = chunk.getNumChannels() > 1 ? chunk.getReadPointer (1) : left;

        for (int i = 0; i < toRead; ++i)
        {
            const int b = static_cast<int> (((samplesRead + i) * numBuckets) / reader->lengthInSamples);
            const auto bucket = static_cast<size_t> (juce::jlimit (0, numBuckets - 1, b));

            peakMinL[bucket] = std::min (peakMinL[bucket], left[i]);
            peakMaxL[bucket] = std::max (peakMaxL[bucket], left[i]);
            peakMinR[bucket] = std::min (peakMinR[bucket], right[i]);
            peakMaxR[bucket] = std::max (peakMaxR[bucket], right[i]);
        }

        samplesRead += toRead;
    }
}

//==============================================================================
// Bounce-to-take DSP
//==============================================================================
bool WaveformEditorComponent::applyBounceToTakeDsp (te::Edit& edit,
                                                    tracktion::EditItemID clipID,
                                                    DspOperation op,
                                                    double normStart,
                                                    double normEnd,
                                                    const juce::File& takesDirectory,
                                                    juce::File* outBouncedFile)
{
    te::WaveAudioClip* targetClip = nullptr;

    for (auto* track : te::getAudioTracks (edit))
    {
        for (auto* clip : track->getClips())
        {
            if (clip->itemID == clipID)
            {
                targetClip = dynamic_cast<te::WaveAudioClip*> (clip);
                break;
            }
        }

        if (targetClip != nullptr)
            break;
    }

    if (targetClip == nullptr)
        return false;

    const juce::File srcFile = targetClip->getSourceFileReference().getFile();

    if (! srcFile.existsAsFile())
        return false;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (srcFile));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return false;

    const int totalSamples = static_cast<int> (reader->lengthInSamples);
    const int numChannels  = static_cast<int> (reader->numChannels);
    const double sampleRate = reader->sampleRate;

    juce::AudioBuffer<float> buffer (numChannels, totalSamples);

    if (! reader->read (&buffer, 0, totalSamples, 0, true, true))
        return false;

    reader.reset(); // Close the input stream before writing the take.

    const double clampedStart = juce::jlimit (0.0, 1.0, std::min (normStart, normEnd));
    const double clampedEnd   = juce::jlimit (0.0, 1.0, std::max (normStart, normEnd));

    int s0 = static_cast<int> (std::floor (clampedStart * static_cast<double> (totalSamples)));
    int s1 = static_cast<int> (std::ceil  (clampedEnd   * static_cast<double> (totalSamples)));

    if (s1 - s0 < 2)
    {
        s0 = 0;
        s1 = totalSamples;
    }

    s0 = juce::jlimit (0, totalSamples - 1, s0);
    s1 = juce::jlimit (s0 + 1, totalSamples, s1);

    const int regionLen = s1 - s0;

    if (op == DspOperation::normalize)
    {
        float maxPeak = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* d = buffer.getReadPointer (ch);

            for (int i = s0; i < s1; ++i)
                maxPeak = std::max (maxPeak, std::abs (d[i]));
        }

        if (maxPeak > 1e-6f)
        {
            const float targetLinear = juce::Decibels::decibelsToGain (-0.5f);
            const float scale = targetLinear / maxPeak;

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.applyGain (ch, s0, regionLen, scale);
        }
    }
    else if (op == DspOperation::reverse)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* d = buffer.getWritePointer (ch);
            std::reverse (d + s0, d + s1);
        }
    }
    else if (op == DspOperation::fadeIn)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.applyGainRamp (ch, s0, regionLen, 0.0f, 1.0f);
    }
    else if (op == DspOperation::fadeOut)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.applyGainRamp (ch, s0, regionLen, 1.0f, 0.0f);
    }
    else if (op == DspOperation::silence)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.clear (ch, s0, regionLen);
    }

    // Write the processed buffer to a brand-new, non-destructive take file.
    takesDirectory.createDirectory();

    const juce::String baseStem = srcFile.getFileNameWithoutExtension()
                                    .upToFirstOccurrenceOf ("_take", false, false);

    const juce::File bouncedFile = takesDirectory.getNonexistentChildFile (baseStem + "_take", ".wav", false);

    {
        auto fileStream = std::make_unique<juce::FileOutputStream> (bouncedFile);

        if (fileStream == nullptr || ! fileStream->openedOk())
            return false;

        fileStream->setPosition (0);
        fileStream->truncate();

        std::unique_ptr<juce::OutputStream> outStream (fileStream.release());

        juce::WavAudioFormat wavFormat;

        const auto options = juce::AudioFormatWriterOptions{}
                                .withSampleRate (sampleRate)
                                .withNumChannels (numChannels)
                                .withBitsPerSample (16);

        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (outStream, options));

        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (buffer, 0, totalSamples))
            return false;
    }

    // Swap the clip's source inside a transaction. SourceFileReference::source
    // is a CachedValue bound to edit.getUndoManager(), so the assignment below
    // is recorded and Undo/Redo switch between the original and the take.
    auto& um = edit.getUndoManager();

    um.beginNewTransaction ("Bounce Audio Take");

    targetClip->getSourceFileReference().setToFile (
        bouncedFile, te::SourceFileReference::PathStyle::alwaysAbsolute, false);

    if (outBouncedFile != nullptr)
        *outBouncedFile = bouncedFile;

    return true;
}

void WaveformEditorComponent::triggerOperation (DspOperation op)
{
    if (edit == nullptr || ! targetClipID.isValid())
        return;

    const juce::File takesDir = getTakesDirectoryCallback
                                  ? getTakesDirectoryCallback()
                                  : getDefaultTakesDirectory (juce::File());

    juce::File bouncedFile;

    if (applyBounceToTakeDsp (*edit, targetClipID, op, selStartNorm, selEndNorm, takesDir, &bouncedFile))
    {
        reloadWaveformSummary();
        repaint();

        if (onWaveformBounced)
            onWaveformBounced ("Bounced take: " + bouncedFile.getFileName());
    }
}

juce::Rectangle<int> WaveformEditorComponent::getWaveformBounds() const
{
    return getLocalBounds().withTrimmedTop (topBarHeight).reduced (12, 8);
}

double WaveformEditorComponent::xToNorm (int x) const
{
    const auto wb = getWaveformBounds();

    if (wb.getWidth() <= 1)
        return 0.0;

    return juce::jlimit (0.0, 1.0, static_cast<double> (x - wb.getX()) / static_cast<double> (wb.getWidth()));
}

void WaveformEditorComponent::mouseDown (const juce::MouseEvent& e)
{
    if (! getWaveformBounds().contains (e.getPosition()))
        return;

    dragAnchorNorm = xToNorm (e.x);
    selStartNorm   = dragAnchorNorm;
    selEndNorm     = dragAnchorNorm;
    repaint();
}

void WaveformEditorComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (e.y < topBarHeight)
        return;

    const double curr = xToNorm (e.x);

    selStartNorm = std::min (dragAnchorNorm, curr);
    selEndNorm   = std::max (dragAnchorNorm, curr);
    repaint();
}

void WaveformEditorComponent::resized()
{
    auto bar = getLocalBounds().removeFromTop (topBarHeight).reduced (8, 4);

    toMidiButton.setBounds (bar.removeFromRight (60));
    bar.removeFromRight (4);
    selectAllButton.setBounds (bar.removeFromRight (38));
    bar.removeFromRight (4);
    silenceButton.setBounds (bar.removeFromRight (54));
    bar.removeFromRight (4);
    fadeOutButton.setBounds (bar.removeFromRight (60));
    bar.removeFromRight (4);
    fadeInButton.setBounds (bar.removeFromRight (56));
    bar.removeFromRight (4);
    reverseButton.setBounds (bar.removeFromRight (46));
    bar.removeFromRight (4);
    normalizeButton.setBounds (bar.removeFromRight (54));
    bar.removeFromRight (6);

    titleLabel.setBounds (bar);
}

void WaveformEditorComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff11141b));

    const auto bounds = getLocalBounds();

    g.setColour (juce::Colour (0xff1b202b));
    g.fillRect (0, 0, bounds.getWidth(), topBarHeight);

    g.setColour (juce::Colour (0xff2d3444));
    g.drawHorizontalLine (topBarHeight - 1, 0.0f, static_cast<float> (bounds.getWidth()));

    auto* clip = resolveWaveClip();

    if (clip == nullptr || peakMaxL.empty())
    {
        g.setColour (juce::Colour (0xff94a3b8));
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawText ("Select an audio clip on the timeline (or generate BGM) to edit its waveform.",
                    bounds.withTrimmedTop (topBarHeight).toFloat(),
                    juce::Justification::centred);
        return;
    }

    const auto wb = getWaveformBounds();

    g.setColour (juce::Colour (0xff161b24));
    g.fillRoundedRectangle (wb.toFloat(), 5.0f);

    const int selX0 = wb.getX() + static_cast<int> (selStartNorm * wb.getWidth());
    const int selX1 = wb.getX() + static_cast<int> (selEndNorm   * wb.getWidth());

    if (selX1 > selX0)
    {
        g.setColour (juce::Colour (0xff0ea5e9).withAlpha (0.20f));
        g.fillRect (selX0, wb.getY(), selX1 - selX0, wb.getHeight());

        g.setColour (juce::Colour (0xff38bdf8));
        g.drawVerticalLine (selX0, static_cast<float> (wb.getY()), static_cast<float> (wb.getBottom()));
        g.drawVerticalLine (selX1, static_cast<float> (wb.getY()), static_cast<float> (wb.getBottom()));
    }

    const int halfH = wb.getHeight() / 2;
    const float midL = static_cast<float> (wb.getY() + halfH / 2);
    const float midR = static_cast<float> (wb.getY() + halfH + halfH / 2);
    const float ampScale = static_cast<float> (std::max (8, halfH / 2 - 4));

    g.setColour (juce::Colour (0xff2b3344));
    g.drawHorizontalLine (static_cast<int> (midL), static_cast<float> (wb.getX()), static_cast<float> (wb.getRight()));
    g.drawHorizontalLine (static_cast<int> (midR), static_cast<float> (wb.getX()), static_cast<float> (wb.getRight()));
    g.drawHorizontalLine (wb.getY() + halfH, static_cast<float> (wb.getX()), static_cast<float> (wb.getRight()));

    g.setColour (juce::Colour (0xff34d399));

    const int numBuckets = static_cast<int> (peakMaxL.size());

    for (int px = 0; px < wb.getWidth(); ++px)
    {
        const int b = juce::jlimit (0, numBuckets - 1, (px * numBuckets) / std::max (1, wb.getWidth()));
        const float x = static_cast<float> (wb.getX() + px);

        const float yMinL = midL - peakMaxL[static_cast<size_t> (b)] * ampScale;
        const float yMaxL = midL - peakMinL[static_cast<size_t> (b)] * ampScale;
        g.drawVerticalLine (static_cast<int> (x), std::min (yMinL, yMaxL), std::max (yMinL, yMaxL) + 1.0f);

        const float yMinR = midR - peakMaxR[static_cast<size_t> (b)] * ampScale;
        const float yMaxR = midR - peakMinR[static_cast<size_t> (b)] * ampScale;
        g.drawVerticalLine (static_cast<int> (x), std::min (yMinR, yMaxR), std::max (yMinR, yMaxR) + 1.0f);
    }

    g.setColour (juce::Colour (0xffe2e8f0));
    g.setFont (juce::Font (juce::FontOptions (11.0f)));

    const juce::String metaText = clip->getName() + "  |  File: " + cachedFile.getFileName()
                                  + "  |  " + juce::String (cachedSampleRate, 0) + " Hz, "
                                  + juce::String (cachedChannels) + " ch";

    g.drawText (metaText, wb.reduced (8, 4).removeFromTop (16).toFloat(), juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff334155));
    g.drawRoundedRectangle (wb.toFloat(), 5.0f, 1.0f);
}
