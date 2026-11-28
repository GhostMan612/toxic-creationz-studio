#pragma once

// Platform-neutral game asset packaging: phase-locked adaptive BGM stems,
// VOX bark trimming/normalization and Godot 4 audio manifest export.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "LoudnessMeter.h"
#include "MixdownExporter.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tcs
{
namespace game
{
struct StemLayer
{
    juce::String name;
    juce::Array<int> trackIndices; // empty => group by track-name prefix
};

struct ManifestAsset
{
    juce::String path;
    juce::String category;   // bgm / sfx / ambient / vox
    bool loop = false;
    double durationSec = 0.0;
    double lufs = -70.0;
    double peakDb = -70.0;
    juce::String variationGroup;
};

class GameAssetExporter
{
public:
    static int exportAdaptiveBgmStems (te::Edit& edit, double startBeat, double endBeat,
                                       const juce::File& outputRoot,
                                       const juce::Array<StemLayer>& layers,
                                       double targetLufs, juce::Array<juce::File>* outFiles = nullptr)
    {
        if (! std::isfinite (startBeat) || ! std::isfinite (endBeat) || endBeat <= startBeat
            || layers.isEmpty())
            return 0;

        const auto bgmDir = outputRoot.getChildFile ("bgm");
        bgmDir.createDirectory();
        auto tracks = te::getAudioTracks (edit);
        juce::Array<bool> savedMutes;
        for (auto* track : tracks)
            savedMutes.add (track->isMuted (false));

        int written = 0;
        for (const auto& layer : layers)
        {
            juce::Array<int> selected = layer.trackIndices;
            if (selected.isEmpty())
            {
                for (int i = 0; i < tracks.size(); ++i)
                    if (tracks[i]->getName().trim().startsWithIgnoreCase (layer.name))
                        selected.add (i);
            }
            if (selected.isEmpty())
                continue;

            for (int i = 0; i < tracks.size(); ++i)
                tracks[i]->setMute (! selected.contains (static_cast<int> (i)));

            const auto wav = bgmDir.getChildFile (layer.name + "_"
                + juce::String (startBeat, 0) + "-" + juce::String (endBeat, 0) + ".wav");
            const bool rendered = MixdownExporter::exportSeamlessLoopWav (
                edit, startBeat, endBeat, wav, targetLufs, 2.0);

            if (! rendered)
            {
                // An empty intensity layer still needs a phase-locked silent stem
                // so Godot's AudioStreamSynchronized crossfades stay frame-aligned.
                const auto start = edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (startBeat));
                const auto end = edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (endBeat));
                const int frames = juce::jmax (64, static_cast<int> (std::lround (
                    (end.inSeconds() - start.inSeconds()) * 44100.0)));
                juce::AudioBuffer<float> silent (2, frames);
                silent.clear();
                writeWav (silent, wav, 44100.0, 2);
            }

            if (wav.existsAsFile() && wav.getSize() > 44)
            {
                if (outFiles != nullptr)
                    outFiles->add (wav);
                ++written;
            }
        }

        for (int i = 0; i < tracks.size(); ++i)
            tracks[i]->setMute (savedMutes.getUnchecked (i));
        return written;
    }

    static int processVoxBarkBatch (const juce::Array<juce::File>& inputFiles,
                                    const juce::File& outputRoot, const juce::String& actor,
                                    const juce::String& action, double targetLufs,
                                    juce::Array<juce::File>* outFiles = nullptr)
    {
        const auto voxDir = outputRoot.getChildFile ("vox");
        voxDir.createDirectory();
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        const double threshold = std::pow (10.0, -50.0 / 20.0);
        int written = 0;

        for (const auto& input : inputFiles)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (input));
            if (reader == nullptr || reader->lengthInSamples <= 0)
                continue;

            const int channels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
            const int frames = static_cast<int> (reader->lengthInSamples);
            juce::AudioBuffer<float> source (channels, frames);
            source.clear();
            reader->read (&source, 0, frames, 0, true, channels > 1);

            int first = frames;
            int last = -1;
            for (int i = 0; i < frames; ++i)
            {
                float mag = 0.0f;
                for (int ch = 0; ch < channels; ++ch)
                    mag = juce::jmax (mag, std::abs (source.getSample (ch, i)));
                if (mag > static_cast<float> (threshold))
                {
                    if (first == frames) first = i;
                    last = i;
                }
            }
            if (last <= first)
                continue;

            const double sr = reader->sampleRate;
            const int headFrames = static_cast<int> (std::lround (0.015 * sr));
            const int tailFrames = static_cast<int> (std::lround (0.040 * sr));
            const int start = juce::jmax (0, first - headFrames);
            const int end = juce::jmin (frames - 1, last + tailFrames);
            const int outFrames = end - start + 1;
            juce::AudioBuffer<float> bark (channels, outFrames);
            bark.clear();

            for (int ch = 0; ch < channels; ++ch)
            {
                for (int i = 0; i < outFrames; ++i)
                    bark.setSample (ch, i, source.getSample (ch, start + i));

                const int fadeIn = juce::jmin (headFrames, outFrames / 4);
                const int fadeOut = juce::jmin (tailFrames, outFrames / 4);
                for (int i = 0; i < fadeIn; ++i)
                    bark.setSample (ch, i, bark.getSample (ch, i)
                        * (static_cast<float> (i) / static_cast<float> (fadeIn)));
                for (int i = 0; i < fadeOut; ++i)
                    bark.setSample (ch, outFrames - 1 - i, bark.getSample (ch, outFrames - 1 - i)
                        * (static_cast<float> (i) / static_cast<float> (fadeOut)));
            }

            tcs::loudness::KWeightedLoudnessMeter meter (sr, channels);
            for (int i = 0; i < outFrames; ++i)
                meter.processFrame (bark.getSample (0, i), channels > 1 ? bark.getSample (1, i) : 0.0f);

            const double lufs = meter.getLoudnessLufs();
            const float peak = bark.getMagnitude (0, outFrames);
            double gain = std::pow (10.0, (juce::jlimit (-60.0, 0.0, targetLufs) - lufs) / 20.0);
            const double ceiling = std::pow (10.0, -1.0 / 20.0);
            if (peak > 1.0e-6f && static_cast<double> (peak) * gain > ceiling)
                gain = ceiling / static_cast<double> (peak);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < outFrames; ++i)
                    bark.setSample (ch, i, juce::jlimit (-1.0f, 1.0f,
                        static_cast<float> (static_cast<double> (bark.getSample (ch, i)) * gain)));

            const auto prefix = (actor.isNotEmpty() ? actor : "actor") + "_"
                              + (action.isNotEmpty() ? action : "bark");
            const auto wav = voxDir.getChildFile (prefix + "_"
                + juce::String (written + 1).paddedLeft ('0', 2) + ".wav");
            if (writeWav (bark, wav, sr, channels))
            {
                if (outFiles != nullptr)
                    outFiles->add (wav);
                ++written;
            }
        }
        return written;
    }

    static bool writeWav (const juce::AudioBuffer<float>& buffer, const juce::File& file,
                          double sampleRate = 44100.0, int channels = 2)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat wavFormat;
        auto stream = std::make_unique<juce::FileOutputStream> (file);
        if (! stream->openedOk())
            return false;
        stream->setPosition (0);
        stream->truncate();
        std::unique_ptr<juce::OutputStream> os (stream.release());
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate (sampleRate).withNumChannels (channels).withBitsPerSample (16);
        auto writer = std::unique_ptr<juce::AudioFormatWriter> (wavFormat.createWriterFor (os, options));
        return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    static ManifestAsset analyseAsset (const juce::File& file, const juce::String& relativePath,
                                       const juce::String& category, bool loop,
                                       const juce::String& variationGroup = {})
    {
        ManifestAsset asset;
        asset.path = relativePath;
        asset.category = category;
        asset.loop = loop;
        asset.variationGroup = variationGroup.isEmpty() ? category : variationGroup;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0.0)
            return asset;

        const int frames = static_cast<int> (reader->lengthInSamples);
        const int channels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
        asset.durationSec = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
        juce::AudioBuffer<float> buffer (channels, frames);
        buffer.clear();
        reader->read (&buffer, 0, frames, 0, true, channels > 1);
        asset.peakDb = 20.0 * std::log10 (std::max (1.0e-6,
            static_cast<double> (buffer.getMagnitude (0, frames))));

        tcs::loudness::KWeightedLoudnessMeter meter (reader->sampleRate, channels);
        for (int i = 0; i < frames; ++i)
            meter.processFrame (buffer.getSample (0, i), channels > 1 ? buffer.getSample (1, i) : 0.0f);
        asset.lufs = meter.getLoudnessLufs();
        return asset;
    }

    static bool writeAudioManifestJson (const juce::File& outputRoot, double bpm,
                                        const juce::Array<ManifestAsset>& assets)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("schema_version", 1);
        object->setProperty ("bpm", bpm);

        juce::Array<juce::var> entries;
        for (const auto& asset : assets)
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty ("path", asset.path);
            entry->setProperty ("category", asset.category);
            entry->setProperty ("loop", asset.loop);
            entry->setProperty ("duration_sec", asset.durationSec);
            entry->setProperty ("lufs", asset.lufs);
            entry->setProperty ("peak_db", asset.peakDb);
            entry->setProperty ("variation_group", asset.variationGroup);
            entries.add (juce::var (entry));
        }
        object->setProperty ("assets", juce::var (entries));

        outputRoot.createDirectory();
        const auto file = outputRoot.getChildFile ("audio_manifest.json");
        return file.replaceWithText (juce::JSON::toString (juce::var (object), true));
    }
};
} // namespace game
} // namespace tcs
