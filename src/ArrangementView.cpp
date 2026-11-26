#include "ArrangementView.h"

#include <algorithm>
#include <cmath>

ArrangementView::ArrangementView()
{
    setOpaque (true);
}

void ArrangementView::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    selectedClipID = {};
    horizontalScrollBeats = 0.0;
    verticalScrollOffsetPx = 0;

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

void ArrangementView::setRecordingManager (RecordingManager* manager)
{
    recordingManager = manager;
    repaint();
}

void ArrangementView::setPixelsPerBeat (float newPixelsPerBeat)
{
    pixelsPerBeat = juce::jlimit (16.0f, 96.0f, newPixelsPerBeat);
    repaint();
}

void ArrangementView::zoomIn()
{
    setPixelsPerBeat (pixelsPerBeat * 1.25f);
}

void ArrangementView::zoomOut()
{
    setPixelsPerBeat (pixelsPerBeat / 1.25f);
}

void ArrangementView::setAutomationOverlayEnabled (bool enabled)
{
    automationOverlayEnabled = enabled;
    draggingAutoTrackIdx = -1;
    draggingAutoPointIndex = -1;
    repaint();
}

void ArrangementView::setAutomationParamName (const juce::String& paramName)
{
    const auto trimmed = paramName.trim().toLowerCase();
    automationParamName = (trimmed == "pan") ? "pan" : "volume";
    repaint();
}

void ArrangementView::setSelectedClipID (tracktion::EditItemID clipID)
{
    const bool changed = (selectedClipID != clipID);
    selectedClipID = clipID;

    if (edit != nullptr && clipID.isValid())
    {
        auto tracks = te::getAudioTracks (*edit);

        for (int i = 0; i < tracks.size(); ++i)
        {
            for (auto* clip : tracks[i]->getClips())
            {
                if (clip->itemID == clipID)
                {
                    ensureTrackVisible (i);
                    break;
                }
            }
        }
    }

    repaint();

    if (changed && onClipSelected != nullptr)
        onClipSelected (selectedClipID);
}

void ArrangementView::scrollToTrack (int trackIndex)
{
    if (edit == nullptr)
        return;

    ensureTrackVisible (trackIndex);
    repaint();
}

int ArrangementView::getMaxVerticalScrollPx() const
{
    if (edit == nullptr)
        return 0;

    const int trackCount = static_cast<int> (te::getAudioTracks (*edit).size());
    const int contentHeight = trackCount * trackHeight;
    const int viewportHeight = std::max (1, getHeight() - rulerHeight);
    return std::max (0, contentHeight - viewportHeight);
}

void ArrangementView::clampVerticalScroll()
{
    verticalScrollOffsetPx = juce::jlimit (0, getMaxVerticalScrollPx(), verticalScrollOffsetPx);
}

void ArrangementView::ensureTrackVisible (int trackIdx)
{
    if (edit == nullptr)
        return;

    auto tracks = te::getAudioTracks (*edit);

    if (tracks.isEmpty())
        return;

    const int safeIdx = juce::jlimit (0, tracks.size() - 1, trackIdx);
    const int viewportHeight = std::max (trackHeight, getHeight() - rulerHeight);
    const int trackTop = safeIdx * trackHeight;
    const int trackBottom = trackTop + trackHeight;

    if (trackTop < verticalScrollOffsetPx)
        verticalScrollOffsetPx = trackTop;
    else if (trackBottom > verticalScrollOffsetPx + viewportHeight)
        verticalScrollOffsetPx = trackBottom - viewportHeight;

    clampVerticalScroll();
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

int ArrangementView::getTotalBars() const
{
    int maxBar = 16;

    if (edit != nullptr)
    {
        for (auto* track : te::getAudioTracks (*edit))
        {
            for (auto* clip : track->getClips())
            {
                const double endBeat = edit->tempoSequence.toBeats (clip->getPosition().getEnd()).inBeats();
                const int endBar = static_cast<int> (std::ceil (endBeat / 4.0));
                maxBar = std::max (maxBar, endBar + 4);
            }
        }
    }

    return juce::jlimit (16, 128, maxBar);
}

int ArrangementView::getTrackIndexAtY (int y) const
{
    if (y < rulerHeight)
        return -1;

    return (y - rulerHeight + verticalScrollOffsetPx) / trackHeight;
}

double ArrangementView::getBeatAtX (int x) const
{
    return std::max (0.0, horizontalScrollBeats + static_cast<double> (x - headerWidth) / pixelsPerBeat);
}

int ArrangementView::getXForBeat (double beat) const
{
    return headerWidth + static_cast<int> (std::round ((beat - horizontalScrollBeats) * pixelsPerBeat));
}

int ArrangementView::getYForAutomationValue (float domainValue, int trackIdx) const
{
    const int top = getTrackTopY (trackIdx);
    const float norm = (automationParamName == "pan")
                           ? juce::jlimit (0.0f, 1.0f, (domainValue + 1.0f) * 0.5f)
                           : juce::jlimit (0.0f, 1.0f, (domainValue + 60.0f) / 66.0f);
    return (top + trackHeight - 5) - juce::roundToInt (norm * static_cast<float> (trackHeight - 10));
}

float ArrangementView::getAutomationValueAtY (int y, int trackIdx) const
{
    const int top = getTrackTopY (trackIdx);
    const float norm = juce::jlimit (0.0f, 1.0f,
                                     static_cast<float> ((top + trackHeight - 5) - y)
                                         / static_cast<float> (trackHeight - 10));

    if (automationParamName == "pan")
        return norm * 2.0f - 1.0f;

    return -60.0f + norm * 66.0f;
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
    g.drawText ("ARRANGEMENT",
                10, 0, automationOverlayEnabled ? headerWidth - 78 : headerWidth - 20, rulerHeight,
                juce::Justification::centredLeft);

    if (automationOverlayEnabled)
    {
        const juce::Rectangle<int> autoParamRect (headerWidth - 64, 5, 56, 18);
        g.setColour (automationParamName == "pan" ? juce::Colour (0xff0e7490) : juce::Colour (0xffb45309));
        g.fillRoundedRectangle (autoParamRect.toFloat(), 3.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (automationParamName == "pan" ? "Auto:Pan" : "Auto:Vol",
                    autoParamRect.toFloat(), juce::Justification::centred);
    }

    const int totalBeats = getTotalBars() * 4;

    {
        juce::Graphics::ScopedSaveState timelineClip (g);
        g.reduceClipRegion (headerWidth, 0, bounds.getWidth() - headerWidth, bounds.getHeight());

        for (int beat = 0; beat <= totalBeats; ++beat)
        {
            const int x = getXForBeat (static_cast<double> (beat));
            if (x > bounds.getWidth())
                break;
            if (x + 60 < headerWidth)
                continue;

            const bool isBar = (beat % 4 == 0);
            g.setColour (isBar ? juce::Colour (0xff3a404e) : juce::Colour (0xff232731));
            g.drawVerticalLine (x, static_cast<float> (rulerHeight),
                                static_cast<float> (bounds.getHeight()));

            if (isBar)
            {
                g.setColour (juce::Colour (0xff9aa3b2));
                g.drawText ("Bar " + juce::String (beat / 4 + 1),
                            x + 4, 2, 54, rulerHeight - 4,
                            juce::Justification::centredLeft);
            }
        }
    }

    if (edit == nullptr)
        return;

    clampVerticalScroll();
    auto tracks = te::getAudioTracks (*edit);

    {
        juce::Graphics::ScopedSaveState tracksClip (g);
        g.reduceClipRegion (0, rulerHeight, bounds.getWidth(), std::max (0, bounds.getHeight() - rulerHeight));

        for (int i = 0; i < tracks.size(); ++i)
        {
            auto* track = tracks[i];
            const int top = getTrackTopY (i);

            if (top + trackHeight <= rulerHeight)
                continue;

            if (top > bounds.getHeight())
                break;

        // Track lane + header
        const bool armed = RecordingManager::isTrackArmed (*track);
        const bool isActivelyRecording = (recordingManager != nullptr && recordingManager->isRecording() && armed);

        g.setColour (i % 2 == 0 ? juce::Colour (0xff181b22) : juce::Colour (0xff1b1e26));
        g.fillRect (headerWidth, top, bounds.getWidth() - headerWidth, trackHeight);

        g.setColour (juce::Colour (0xff222630));
        g.fillRect (0, top, headerWidth, trackHeight);

        if (isActivelyRecording)
        {
            g.setColour (juce::Colour (0x22dc2626));
            g.fillRect (0, top, headerWidth, trackHeight);
            g.setColour (juce::Colour (0xffef4444));
            g.fillRect (0, top, 4, trackHeight);
        }

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
        juce::Rectangle<int> armRect  (66, top + 30, 24, 18);

        g.setColour (muted ? juce::Colour (0xffd9534f) : juce::Colour (0xff313644));
        g.fillRoundedRectangle (muteRect.toFloat(), 3.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("M", muteRect.toFloat(), juce::Justification::centred);

        g.setColour (solo ? juce::Colour (0xfff0ad4e) : juce::Colour (0xff313644));
        g.fillRoundedRectangle (soloRect.toFloat(), 3.0f);
        g.setColour (solo ? juce::Colours::black : juce::Colours::white);
        g.drawText ("S", soloRect.toFloat(), juce::Justification::centred);

        g.setColour (armed ? juce::Colour (0xffdc2626) : juce::Colour (0xff313644));
        g.fillRoundedRectangle (armRect.toFloat(), 3.0f);
        g.setColour (juce::Colours::white);
        g.drawText ("R", armRect.toFloat(), juce::Justification::centred);

        const auto takes = RecordingManager::getTrackTakes (*track);
        int activeTake = -1;
        for (int t = 0; t < takes.size(); ++t)
            if (takes[t].active)
                activeTake = t;

        juce::Rectangle<int> takeRect (94, top + 30, 58, 18);
        if (takes.size() >= 2)
        {
            g.setColour (juce::Colour (0xff2b6f6f));
            g.fillRoundedRectangle (takeRect.toFloat(), 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (9.0f)));
            g.drawText ("Take " + juce::String (activeTake + 1) + "/" + juce::String (takes.size()),
                        takeRect.toFloat(), juce::Justification::centred);
        }

        g.setColour (juce::Colour (0xff9aa3b2));
        g.drawText (juce::String (db, 1) + " dB", 154, top + 30, 30, 18, juce::Justification::centredRight);

        g.setColour (juce::Colour (0xff2b303c));
        g.drawHorizontalLine (top + trackHeight - 1, 0.0f, static_cast<float> (bounds.getWidth()));

        // Clips on this track (clipped to timeline pane so scrolled clips never bleed onto headers)
        {
            juce::Graphics::ScopedSaveState clipGuard (g);
            g.reduceClipRegion (headerWidth, top, bounds.getWidth() - headerWidth, trackHeight);

            for (auto* clip : track->getClips())
            {
                const auto pos = clip->getPosition();
                const double startBeat = edit->tempoSequence.toBeats (pos.getStart()).inBeats();
                const double endBeat   = edit->tempoSequence.toBeats (pos.getEnd()).inBeats();

                const int clipX = getXForBeat (startBeat);
                const int clipW = std::max (12, getXForBeat (endBeat) - clipX);
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

            if (automationOverlayEnabled)
            {
                const auto pts = AutomationManager::getAutomationPoints (*edit, *track, automationParamName);
                const juce::Colour curveCol = (automationParamName == "pan")
                                                  ? juce::Colour (0xff22d3ee)
                                                  : juce::Colour (0xfffbbf24);

                if (! pts.empty())
                {
                    juce::Path path;

                    for (size_t idx = 0; idx < pts.size(); ++idx)
                    {
                        const float px = static_cast<float> (getXForBeat (pts[idx].beat));
                        const float py = static_cast<float> (getYForAutomationValue (pts[idx].value, i));

                        if (idx == 0)
                            path.startNewSubPath (px, py);
                        else
                            path.lineTo (px, py);
                    }

                    g.setColour (curveCol.withAlpha (0.9f));
                    g.strokePath (path, juce::PathStrokeType (2.0f));

                    for (const auto& pt : pts)
                    {
                        const float px = static_cast<float> (getXForBeat (pt.beat));
                        const float py = static_cast<float> (getYForAutomationValue (pt.value, i));

                        g.setColour (juce::Colour (0xff0f172a));
                        g.fillEllipse (px - 5.0f, py - 5.0f, 10.0f, 10.0f);
                        g.setColour (curveCol);
                        g.fillEllipse (px - 3.5f, py - 3.5f, 7.0f, 7.0f);
                    }
                }
            }
        }
        }
    }

    const int maxVScroll = getMaxVerticalScrollPx();
    if (maxVScroll > 0 && bounds.getHeight() > rulerHeight + 16)
    {
        const int sbTrackX = bounds.getWidth() - 6;
        const int sbTrackY = rulerHeight + 2;
        const int sbTrackH = bounds.getHeight() - rulerHeight - 4;
        const int totalContentH = static_cast<int> (tracks.size()) * trackHeight;
        const int thumbH = std::max (20, juce::roundToInt (
            static_cast<float> (sbTrackH) * static_cast<float> (sbTrackH)
            / static_cast<float> (std::max (1, totalContentH))));
        const int thumbY = sbTrackY + juce::roundToInt (
            (static_cast<float> (verticalScrollOffsetPx) / static_cast<float> (maxVScroll))
            * static_cast<float> (std::max (0, sbTrackH - thumbH)));

        g.setColour (juce::Colour (0xff1c212b));
        g.fillRoundedRectangle (static_cast<float> (sbTrackX), static_cast<float> (sbTrackY),
                                4.0f, static_cast<float> (sbTrackH), 2.0f);
        g.setColour (juce::Colour (0xff64748b));
        g.fillRoundedRectangle (static_cast<float> (sbTrackX), static_cast<float> (thumbY),
                                4.0f, static_cast<float> (thumbH), 2.0f);
    }

    // Header / timeline divider
    g.setColour (juce::Colour (0xff363c4a));
    g.drawVerticalLine (headerWidth, 0.0f, static_cast<float> (bounds.getHeight()));

    // Loop range highlight & playhead clipped to timeline area
    juce::Graphics::ScopedSaveState timelineOverlayClip (g);
    g.reduceClipRegion (headerWidth, 0, bounds.getWidth() - headerWidth, bounds.getHeight());

    if (edit->getTransport().looping.get())
    {
        const auto loopRange = edit->getTransport().getLoopRange();

        if (! loopRange.isEmpty())
        {
            const double startBeat = edit->tempoSequence.toBeats (loopRange.getStart()).inBeats();
            const double endBeat   = edit->tempoSequence.toBeats (loopRange.getEnd()).inBeats();

            const int x1 = juce::jlimit (headerWidth, bounds.getWidth(), getXForBeat (startBeat));
            const int x2 = juce::jlimit (headerWidth, bounds.getWidth(), getXForBeat (endBeat));

            if (x2 > x1)
            {
                const juce::Colour loopCyan (0xff22d3ee);
                g.setColour (loopCyan.withAlpha (0.28f));
                g.fillRect (x1, 0, x2 - x1, rulerHeight - 1);
                g.setColour (loopCyan);
                g.fillRect (x1, 0, x2 - x1, 2);
                g.fillRect (x1, rulerHeight - 3, x2 - x1, 2);
            }
        }
    }

    // Transport playhead
    const double playheadBeat = edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats();
    const int playheadX = getXForBeat (playheadBeat);

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

    if (automationOverlayEnabled && e.y < rulerHeight && e.x < headerWidth)
    {
        const juce::Rectangle<int> autoParamRect (headerWidth - 64, 5, 56, 18);
        if (autoParamRect.contains (e.getPosition()))
        {
            setAutomationParamName (automationParamName == "volume" ? "pan" : "volume");
            return;
        }
    }

    // Ruler click -> Shift+Click sets loop end (bar-snapped), plain click seeks playhead
    if (e.y < rulerHeight && e.x >= headerWidth)
    {
        const double rawBeat = getBeatAtX (e.x);

        if (e.mods.isShiftDown())
        {
            const auto curLoop = edit->getTransport().getLoopRange();
            const double loopStartBeat = curLoop.isEmpty()
                                             ? 0.0
                                             : std::max (0.0, edit->tempoSequence.toBeats (curLoop.getStart()).inBeats());
            const double snappedEndBeat = std::max (loopStartBeat + 4.0,
                                                    std::ceil (std::max (0.25, rawBeat) / 4.0) * 4.0);

            edit->getTransport().setLoopRange ({
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (loopStartBeat)),
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (snappedEndBeat))
            });
            edit->getTransport().looping = true;

            if (onLoopChanged != nullptr)
                onLoopChanged();

            repaint();
            return;
        }

        const double clickedBeat = std::floor (rawBeat * 4.0) / 4.0;
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
    const int top = getTrackTopY (trackIdx);

    // Header click -> Mute / Solo / Record Arm toggles
    if (e.x < headerWidth)
    {
        juce::Rectangle<int> muteRect (10, top + 30, 24, 18);
        juce::Rectangle<int> soloRect (38, top + 30, 24, 18);
        juce::Rectangle<int> armRect  (66, top + 30, 24, 18);

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
        else if (armRect.contains (e.getPosition()))
        {
            const bool nextArmed = ! RecordingManager::isTrackArmed (*track);
            RecordingManager::setTrackArmed (*edit, *track, nextArmed);
            repaint();

            if (onTrackArmChanged != nullptr)
                onTrackArmChanged();
        }

        juce::Rectangle<int> takeRect (94, top + 30, 58, 18);
        if (takeRect.contains (e.getPosition()))
        {
            const auto takes = RecordingManager::getTrackTakes (*track);
            if (takes.size() >= 2)
            {
                if (e.mods.isShiftDown())
                {
                    RecordingManager::flattenCompToClip (
                        *edit, *track, takes[0].startBeat, takes[0].endBeat, 10.0);
                }
                else
                {
                    int active = 0;
                    for (int t = 0; t < takes.size(); ++t)
                        if (takes[t].active)
                            active = t;
                    RecordingManager::selectActiveTake (*edit, *track, 0, (active + 1) % takes.size());
                }
                repaint();
            }
            return;
        }

        return;
    }

    // Automation overlay editing inside track lane
    if (automationOverlayEnabled && e.y >= rulerHeight && e.x >= headerWidth)
    {
        const auto pts = AutomationManager::getAutomationPoints (*edit, *track, automationParamName);
        int hitIdx = -1;

        for (int idx = 0; idx < static_cast<int> (pts.size()); ++idx)
        {
            const float px = static_cast<float> (getXForBeat (pts[static_cast<size_t> (idx)].beat));
            const float py = static_cast<float> (getYForAutomationValue (pts[static_cast<size_t> (idx)].value, trackIdx));

            if (e.position.getDistanceFrom ({ px, py }) <= 9.0f)
            {
                hitIdx = idx;
                break;
            }
        }

        if (e.mods.isRightButtonDown())
        {
            if (hitIdx >= 0)
            {
                AutomationManager::removeAutomationPointAtIndex (*edit, *track, automationParamName, hitIdx);
                repaint();
            }
            return;
        }

        if (e.mods.isLeftButtonDown())
        {
            if (hitIdx >= 0)
            {
                edit->getUndoManager().beginNewTransaction ("Move Automation Point");
                draggingAutoTrackIdx = trackIdx;
                draggingAutoPointIndex = hitIdx;
                return;
            }

            const double snappedBeat = std::max (0.0, std::floor (getBeatAtX (e.x) * 4.0 + 0.5) / 4.0);
            const float domainVal = getAutomationValueAtY (e.y, trackIdx);
            AutomationManager::setAutomationPoints (*edit, *track, automationParamName,
                                                    { { snappedBeat, domainVal, 0.0f } }, false);
            repaint();
            return;
        }
    }

    // Timeline click -> select the hit clip
    const double beat = getBeatAtX (e.x);
    const auto timeAtClick = edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (beat));

    for (auto* clip : track->getClips())
    {
        if (clip->getPosition().time.contains (timeAtClick))
        {
            // Any clip type is selectable: MidiClip for the piano roll,
            // WaveAudioClip for the waveform editor.
            setSelectedClipID (clip->itemID);
            return;
        }
    }
}

void ArrangementView::mouseDrag (const juce::MouseEvent& e)
{
    if (edit == nullptr || ! automationOverlayEnabled || draggingAutoTrackIdx < 0 || draggingAutoPointIndex < 0)
        return;

    auto tracks = te::getAudioTracks (*edit);
    if (! juce::isPositiveAndBelow (draggingAutoTrackIdx, tracks.size()))
        return;

    const double newBeat = std::max (0.0, std::floor (getBeatAtX (e.x) * 4.0 + 0.5) / 4.0);
    const float newVal = getAutomationValueAtY (e.y, draggingAutoTrackIdx);
    AutomationManager::moveAutomationPointAtIndex (*edit, *tracks[draggingAutoTrackIdx], automationParamName,
                                                  draggingAutoPointIndex, newBeat, newVal, &draggingAutoPointIndex);
    repaint();
}

void ArrangementView::mouseUp (const juce::MouseEvent&)
{
    draggingAutoTrackIdx = -1;
    draggingAutoPointIndex = -1;
}

void ArrangementView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown())
    {
        if (wheel.deltaY > 0.0f) zoomIn();
        else if (wheel.deltaY < 0.0f) zoomOut();
        return;
    }

    if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > 0.0f)
    {
        const double delta = (std::abs (wheel.deltaX) > 0.0f) ? -wheel.deltaX : -wheel.deltaY;
        horizontalScrollBeats = juce::jlimit (0.0, static_cast<double> (getTotalBars() * 4),
                                              horizontalScrollBeats + delta * 2.0);
        repaint();
        return;
    }

    if (std::abs (wheel.deltaY) > 0.0f)
    {
        const int maxScroll = getMaxVerticalScrollPx();
        if (maxScroll > 0)
        {
            const int stepPx = juce::roundToInt (-wheel.deltaY * 120.0f);
            verticalScrollOffsetPx = juce::jlimit (0, maxScroll, verticalScrollOffsetPx + stepPx);
            repaint();
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
