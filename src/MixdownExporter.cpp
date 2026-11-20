#include "MixdownExporter.h"

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
