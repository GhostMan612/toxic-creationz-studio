#include "MixdownExporter.h"

#include "LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <memory>
tracktion::TimeRange MixdownExporter::getDefaultExportRange (te::Edit& edit)
{
    auto maxEnd = edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (16.0)); // 4-bar minimum

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            if (clip->getPosition().getEnd() > maxEnd)
                maxEnd = clip->getPosition().getEnd();

    return { tracktion::TimePosition::fromSeconds (0.0), maxEnd };
}

bool MixdownExporter::exportEditToWav (te::Edit& edit,
                                       const juce::File& outputWavFile,
                                       tracktion::TimeRange range,
                                       double sampleRate,
                                       int bitDepth)
{
    if (range.isEmpty() || range.getLength().inSeconds() <= 0.0)
        return false;

    if (outputWavFile == juce::File() || outputWavFile.isDirectory())
        return false;

    if (sampleRate <= 7000.0 || bitDepth <= 0)
        return false;

    outputWavFile.getParentDirectory().createDirectory();

    if (outputWavFile.existsAsFile() && ! outputWavFile.deleteFile())
        return false;

    juce::BigInteger tracksToDo;
    int firstAudioTrackBit = -1;
    const auto allTracks = te::getAllTracks (edit);

    for (int i = 0; i < allTracks.size(); ++i)
    {
        if (auto* at = dynamic_cast<te::AudioTrack*> (allTracks[i]))
        {
            if (firstAudioTrackBit < 0)
                firstAudioTrackBit = i;

            if (! at->isMuted (false))
                tracksToDo.setBit (i);
        }
    }

    if (tracksToDo.isZero())
    {
        if (firstAudioTrackBit < 0)
            return false;

        tracksToDo.setBit (firstAudioTrackBit);
    }

    auto* wavFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
    if (wavFormat == nullptr)
        return false;

    bool hasMissingWaveClip = false;
    juce::Array<te::Clip*> validClips;

    for (int i = 0; i < allTracks.size(); ++i)
    {
        if (! tracksToDo[i])
            continue;

        if (auto* at = dynamic_cast<te::AudioTrack*> (allTracks[i]))
        {
            for (auto* clip : at->getClips())
            {
                if (auto* wc = dynamic_cast<te::WaveAudioClip*> (clip))
                {
                    const auto resolvedFile = wc->getSourceFileReference().getFile();

                    if (! resolvedFile.existsAsFile())
                    {
                        hasMissingWaveClip = true;

                        if (resolvedFile != juce::File())
                            edit.engine.getAudioFileManager().forceFileUpdate (te::AudioFile (edit.engine, resolvedFile));

                        const auto playFile = wc->getPlaybackFile();

                        if (! playFile.isNull() && ! playFile.getFile().existsAsFile())
                            edit.engine.getAudioFileManager().forceFileUpdate (playFile);
                    }
                    else
                    {
                        validClips.add (wc);
                    }
                }
                else
                {
                    validClips.add (clip);
                }
            }
        }
    }

    te::Renderer::Parameters params (edit);
    params.destFile           = outputWavFile;
    params.audioFormat        = wavFormat;
    params.bitDepth           = bitDepth;
    params.sampleRateForAudio = sampleRate;
    params.blockSizeForAudio  = 512;
    params.time               = range;
    params.tracksToDo         = tracksToDo;
    if (hasMissingWaveClip && ! validClips.isEmpty())
        params.allowedClips   = validClips;
    params.canRenderInMono    = false; // Guarantee 2-channel stereo WAV
    params.usePlugins         = true;
    params.useMasterPlugins   = true;
    params.realTimeRender     = false;

    const te::Edit::ScopedRenderStatus scopedRenderStatus (edit, true);
    te::TransportControl::stopAllTransports (edit.engine, false, true);

    juce::String renderError;
    {
        te::Renderer::RenderTask task ("Master Mixdown", params, nullptr, nullptr);

        while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
        {
        }

        renderError = task.errorMessage;
    } // Destroy RenderTask (and its NodeRenderContext) BEFORE ~ScopedRenderStatus()

    if (renderError.isNotEmpty())
    {
        outputWavFile.deleteFile();
        return false;
    }

    return outputWavFile.existsAsFile() && outputWavFile.getSize() > 44;
}

bool MixdownExporter::exportSeamlessLoopWav (te::Edit& edit, double startBeat, double endBeat,
                                             const juce::File& outputWavFile,
                                             double targetLufs, double tailSeconds)
{
    if (! std::isfinite (startBeat) || ! std::isfinite (endBeat) || endBeat <= startBeat
        || outputWavFile == juce::File() || outputWavFile.isDirectory())
        return false;

    const double sr = 44100.0;
    const double tail = juce::jlimit (0.1, 5.0, tailSeconds);
    const auto start = edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat));
    const auto end = edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endBeat));
    const auto overhangEnd = tracktion::TimePosition::fromSeconds (end.inSeconds() + tail);

    outputWavFile.getParentDirectory().createDirectory();
    const auto rawFile = outputWavFile.getSiblingFile (outputWavFile.getFileNameWithoutExtension() + ".loop_raw.wav");
    if (! exportEditToWav (edit, rawFile, { start, overhangEnd }, sr, 16))
        return false;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (rawFile));
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        rawFile.deleteFile();
        return false;
    }

    const int numCh = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
    const int totalFrames = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> raw (numCh, totalFrames);
    raw.clear();
    reader->read (&raw, 0, totalFrames, 0, true, numCh > 1);
    rawFile.deleteFile();

    const int loopFrames = juce::jlimit (64, totalFrames,
        static_cast<int> (std::lround ((end.inSeconds() - start.inSeconds()) * sr)));
    const int tailFrames = juce::jmin (totalFrames - loopFrames,
                                       static_cast<int> (std::lround (tail * sr)));
    const int fadeFrames = juce::jmax (16, static_cast<int> (std::lround (0.005 * sr)));

    juce::AudioBuffer<float> loop (numCh, loopFrames);
    loop.clear();
    for (int ch = 0; ch < numCh; ++ch)
    {
        for (int i = 0; i < loopFrames; ++i)
            loop.setSample (ch, i, raw.getSample (ch, i));
        for (int i = 0; i < tailFrames; ++i)
            loop.addSample (ch, i, raw.getSample (ch, loopFrames + i));

        // Fold the tail into the head with a 5 ms boundary micro-fade.
        const float last = loop.getSample (ch, loopFrames - 1);
        const int fade = juce::jmin (fadeFrames, loopFrames / 2);
        for (int i = 0; i < fade; ++i)
        {
            const float t = static_cast<float> (i) / static_cast<float> (fade);
            loop.setSample (ch, i, loop.getSample (ch, i) * t + last * (1.0f - t));
        }
    }

    tcs::loudness::KWeightedLoudnessMeter meter (sr, numCh);
    for (int i = 0; i < loopFrames; ++i)
        meter.processFrame (loop.getSample (0, i), numCh > 1 ? loop.getSample (1, i) : 0.0f);

    const double lufs = meter.getLoudnessLufs();
    const float peak = loop.getMagnitude (0, loopFrames);
    const double target = juce::jlimit (-40.0, -3.0, targetLufs);
    const double gainDb = target - lufs;
    double gain = std::pow (10.0, gainDb / 20.0);
    const double ceiling = std::pow (10.0, -1.0 / 20.0);
    if (peak > 1.0e-6f && peak * gain > ceiling)
        gain = ceiling / static_cast<double> (peak);

    for (int ch = 0; ch < numCh; ++ch)
        for (int i = 0; i < loopFrames; ++i)
            loop.setSample (ch, i, juce::jlimit (-1.0f, 1.0f,
                static_cast<float> (static_cast<double> (loop.getSample (ch, i)) * gain)));

    juce::WavAudioFormat wavFormat;
    auto stream = std::make_unique<juce::FileOutputStream> (outputWavFile);
    if (! stream->openedOk())
        return false;
    stream->setPosition (0);
    stream->truncate();
    std::unique_ptr<juce::OutputStream> os (stream.release());
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sr).withNumChannels (numCh).withBitsPerSample (16);
    auto writer = std::unique_ptr<juce::AudioFormatWriter> (wavFormat.createWriterFor (os, options));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (loop, 0, loopFrames);
}
