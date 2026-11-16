#include "ArrangementView.h"

ArrangementView::ArrangementView()
{
    setOpaque (true);
}

void ArrangementView::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    selectedClipID = {};

    // Auto-select the first available MIDI clip in the Edit.
    if (edit != nullptr)
    {
        for (auto* track : te::getAudioTracks (*edit))
        {
            for (auto* clip : track->getClips())
            {
                if (auto* mc = dynamic_cast<te::MidiClip*> (clip))
                {
                    selectedClipID = mc->itemID;
                    break;
                }
            }

            if (selectedClipID.isValid())
                break;
        }
    }

    repaint();

    if (onClipSelected != nullptr)
        onClipSelected (selectedClipID);
}

void ArrangementView::setSelectedClipID (tracktion::EditItemID clipID)
{
    if (selectedClipID != clipID)
    {
        selectedClipID = clipID;
        repaint();

        if (onClipSelected != nullptr)
            onClipSelected (selectedClipID);
    }
}

te::MidiClip* ArrangementView::resolveSelectedClip() const
{
    if (edit == nullptr || ! selectedClipID.isValid())
        return nullptr;

    for (auto* track : te::getAudioTracks (*edit))
        for (auto* clip : track->getClips())
            if (clip->itemID == selectedClipID)
                return dynamic_cast<te::MidiClip*> (clip);

    return nullptr;
}

int ArrangementView::getTrackIndexAtY (int y) const
{
    if (y < rulerHeight)
        return -1;

    return (y - rulerHeight) / trackHeight;
}

double ArrangementView::getBeatAtX (int x) const
{
    return std::max (0.0, static_cast<double> (x - headerWidth) / pixelsPerBeat);
}

void ArrangementView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14161b));

    const auto bounds = getLocalBounds();

    // Top ruler bar
    g.setColour (juce::Colour (0xff1e2128));
    g.fillRect (0, 0, bounds.getWidth(), rulerHeight);
    g.setColour (juce::Colour (0xff2d323d));
    g.drawHorizontalLine (rulerHeight - 1, 0.0f, static_cast<float> (bounds.getWidth()));

    g.setColour (juce::Colours::lightgrey);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("ARRANGEMENT", 10, 0, headerWidth - 20, rulerHeight, juce::Justification::centredLeft);

    const int totalBeats = totalBars * 4;

    for (int beat = 0; beat <= totalBeats; ++beat)
    {
        const float x = headerWidth + beat * pixelsPerBeat;
        if (x > bounds.getWidth())
            break;

        const bool isBar = (beat % 4 == 0);
        g.setColour (isBar ? juce::Colour (0xff3a404e) : juce::Colour (0xff232731));
        g.drawVerticalLine (static_cast<int> (x), static_cast<float> (rulerHeight),
                            static_cast<float> (bounds.getHeight()));

        if (isBar)
        {
            g.setColour (juce::Colour (0xff9aa3b2));
            g.drawText ("Bar " + juce::String (beat / 4 + 1),
                        static_cast<int> (x) + 4, 2, 54, rulerHeight - 4,
                        juce::Justification::centredLeft);
        }
    }

    if (edit == nullptr)
        return;

    auto tracks = te::getAudioTracks (*edit);

    for (int i = 0; i < tracks.size(); ++i)
    {
        auto* track = tracks[i];
        const int top = rulerHeight + i * trackHeight;
        if (top > bounds.getHeight())
            break;

        // Track lane + header
        g.setColour (i % 2 == 0 ? juce::Colour (0xff181b22) : juce::Colour (0xff1b1e26));
        g.fillRect (headerWidth, top, bounds.getWidth() - headerWidth, trackHeight);

        g.setColour (juce::Colour (0xff222630));
        g.fillRect (0, top, headerWidth, trackHeight);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (juce::String (i + 1) + ". " + track->getName(),
                    10, top + 6, headerWidth - 20, 20,
                    juce::Justification::centredLeft, true);

        const bool muted = track->isMuted (false);
        const bool solo  = track->isSolo (false);
        const float db   = track->getVolumePlugin() != nullptr ? track->getVolumePlugin()->getVolumeDb() : 0.0f;

        juce::Rectangle<int> muteRect (10, top + 30, 24, 18);
        juce::Rectangle<int> soloRect (38, top + 30, 24, 18);

        g.setColour (muted ? juce::Colour (0xffd9534f) : juce::Colour (0xff313644));
        g.fillRoundedRectangle (muteRect.toFloat(), 3.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("M", muteRect.toFloat(), juce::Justification::centred);

        g.setColour (solo ? juce::Colour (0xfff0ad4e) : juce::Colour (0xff313644));
        g.fillRoundedRectangle (soloRect.toFloat(), 3.0f);
        g.setColour (solo ? juce::Colours::black : juce::Colours::white);
        g.drawText ("S", soloRect.toFloat(), juce::Justification::centred);

        g.setColour (juce::Colour (0xff9aa3b2));
        g.drawText (juce::String (db, 1) + " dB", 70, top + 30, 108, 18, juce::Justification::centredRight);

        g.setColour (juce::Colour (0xff2b303c));
        g.drawHorizontalLine (top + trackHeight - 1, 0.0f, static_cast<float> (bounds.getWidth()));

        // Clips on this track
        for (auto* clip : track->getClips())
        {
            const auto pos = clip->getPosition();
            const double startBeat = edit->tempoSequence.toBeats (pos.getStart()).inBeats();
            const double endBeat   = edit->tempoSequence.toBeats (pos.getEnd()).inBeats();

            const int clipX = headerWidth + static_cast<int> (startBeat * pixelsPerBeat);
            const int clipW = std::max (12, static_cast<int> ((endBeat - startBeat) * pixelsPerBeat));
            juce::Rectangle<int> clipRect (clipX + 1, top + 5, clipW - 2, trackHeight - 10);

            const bool isSelected = (clip->itemID == selectedClipID);
            const auto* midiClip  = dynamic_cast<te::MidiClip*> (clip);

            g.setColour (midiClip != nullptr
                             ? (isSelected ? juce::Colour (0xff3b82f6) : juce::Colour (0xff2563eb))
                             : juce::Colour (0xff10b981));
            g.fillRoundedRectangle (clipRect.toFloat(), 5.0f);

            g.setColour (isSelected ? juce::Colours::white : juce::Colour (0xff93c5fd));
            g.drawRoundedRectangle (clipRect.toFloat(), 5.0f, isSelected ? 2.0f : 1.0f);

            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText (clip->getName(),
                        clipRect.reduced (6, 3).removeFromTop (16).toFloat(),
                        juce::Justification::centredLeft, true);

            // Mini MIDI note preview inside the clip block
            if (midiClip != nullptr && clipW > 20)
            {
                const double clipLenBeats = std::max (0.25, endBeat - startBeat);
                g.setColour (juce::Colours::white.withAlpha (0.75f));

                for (auto* note : midiClip->getSequence().getNotes())
                {
                    const double relStart = note->getStartBeat().inBeats() / clipLenBeats;
                    const double relLen   = note->getLengthBeats().inBeats() / clipLenBeats;
                    const float normPitch = juce::jlimit (0.0f, 1.0f, (note->getNoteNumber() - 36) / 48.0f);

                    const int nx = clipRect.getX() + 4 + static_cast<int> (relStart * (clipRect.getWidth() - 8));
                    const int nw = std::max (2, static_cast<int> (relLen * (clipRect.getWidth() - 8)));
                    const int ny = clipRect.getBottom() - 6 - static_cast<int> (normPitch * (clipRect.getHeight() - 24));
                    g.fillRect (nx, ny, nw, 2);
                }
            }
        }
    }

    // Header / timeline divider
    g.setColour (juce::Colour (0xff363c4a));
    g.drawVerticalLine (headerWidth, 0.0f, static_cast<float> (bounds.getHeight()));

    // Transport playhead
    const double playheadBeat = edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats();
    const int playheadX = headerWidth + static_cast<int> (playheadBeat * pixelsPerBeat);

    if (playheadX >= headerWidth && playheadX <= bounds.getWidth())
    {
        g.setColour (juce::Colour (0xfff43f5e));
        g.drawVerticalLine (playheadX, 0.0f, static_cast<float> (bounds.getHeight()));
    }
}

void ArrangementView::mouseDown (const juce::MouseEvent& e)
{
    if (edit == nullptr)
        return;

    // Ruler click -> seek playhead
    if (e.y < rulerHeight && e.x >= headerWidth)
    {
        const double clickedBeat = std::floor (getBeatAtX (e.x) * 4.0) / 4.0;
        const auto targetTime = edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (clickedBeat));
        edit->getTransport().setPosition (targetTime);
        repaint();
        return;
    }

    const int trackIdx = getTrackIndexAtY (e.y);
    auto tracks = te::getAudioTracks (*edit);

    if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
        return;

    auto* track = tracks[trackIdx];
    const int top = rulerHeight + trackIdx * trackHeight;

    // Header click -> Mute / Solo toggles
    if (e.x < headerWidth)
    {
        juce::Rectangle<int> muteRect (10, top + 30, 24, 18);
        juce::Rectangle<int> soloRect (38, top + 30, 24, 18);

        if (muteRect.contains (e.getPosition()))
        {
            track->setMute (! track->isMuted (false));
            repaint();
        }
        else if (soloRect.contains (e.getPosition()))
        {
            track->setSolo (! track->isSolo (false));
            repaint();
        }

        return;
    }

    // Timeline click -> select the hit clip
    const double beat = getBeatAtX (e.x);
    const auto timeAtClick = edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (beat));

    for (auto* clip : track->getClips())
    {
        if (clip->getPosition().time.contains (timeAtClick))
        {
            if (auto* mc = dynamic_cast<te::MidiClip*> (clip))
            {
                setSelectedClipID (mc->itemID);
                return;
            }
        }
    }
}

void ArrangementView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (edit == nullptr || e.x < headerWidth || e.y < rulerHeight)
        return;

    const int trackIdx = getTrackIndexAtY (e.y);
    auto tracks = te::getAudioTracks (*edit);

    if (! juce::isPositiveAndBelow (trackIdx, tracks.size()))
        return;

    auto* track = tracks[trackIdx];
    const double clickedBeat = getBeatAtX (e.x);
    const double barStartBeat = std::floor (clickedBeat / 4.0) * 4.0;

    const tracktion::TimeRange newRange {
        edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (barStartBeat)),
        edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (barStartBeat + 4.0))
    };

    // Only create if nothing already occupies this range
    for (auto* existing : track->getClips())
        if (existing->getPosition().time.overlaps (newRange))
            return;

    if (auto* newClip = dynamic_cast<te::MidiClip*> (
            track->insertNewClip (te::TrackItem::Type::midi,
                                  "Pattern " + juce::String (track->getClips().size() + 1),
                                  newRange,
                                  nullptr)))
    {
        setSelectedClipID (newClip->itemID);
    }
}
