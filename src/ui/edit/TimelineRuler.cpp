#include "TimelineRuler.h"
#include <midi/Timecode.h>

#include "model/MidiModel.h"
#include "model/Session.h"
#include "ui/Theme.h"

namespace meridian
{

TimelineRuler::TimelineRuler (EditState& s) : state (s)
{
    state.addChangeListener (this);
}

TimelineRuler::~TimelineRuler()
{
    state.removeChangeListener (this);
    session.removeListener (this);
}

void TimelineRuler::setSessionState (juce::ValueTree s)
{
    session.removeListener (this);
    session = std::move (s);
    session.addListener (this); // the whole session: markers and loop settings live in it
    repaint();
}

juce::ValueTree TimelineRuler::markerAt (int x) const
{
    for (const auto& m : session.getChildWithName (ids::MARKERS))
    {
        const auto mx = trackHeaderWidth + (int) state.sampleToX ((juce::int64) m[ids::start]);
        const auto labelWidth = 14 + juce::GlyphArrangement::getStringWidthInt (theme::font (11.0f), m[ids::name].toString());
        if (x >= mx - 4 && x <= mx + labelWidth)
            return m;
    }
    return {};
}

void TimelineRuler::paintMarkers (juce::Graphics& g, juce::Rectangle<int> row)
{
    g.setFont (theme::font (11.0f));
    for (const auto& m : session.getChildWithName (ids::MARKERS))
    {
        const auto x = (float) (row.getX() + state.sampleToX ((juce::int64) m[ids::start]));
        juce::Path flag;
        flag.addTriangle (x - 4.0f, (float) row.getY() + 4.0f, x + 4.0f, (float) row.getY() + 4.0f, x, (float) row.getBottom() - 4.0f);
        g.setColour (theme::marker);
        g.fillPath (flag);
        g.drawVerticalLine ((int) x, (float) row.getBottom() - 4.0f, (float) row.getBottom());
        g.setColour (theme::text);
        g.drawText (m[ids::name].toString(), juce::Rectangle<float> (x + 7.0f, (float) row.getY(), 200.0f, (float) row.getHeight()),
                    juce::Justification::centredLeft);
    }
}

void TimelineRuler::showMarkerMenu (juce::ValueTree marker)
{
    juce::PopupMenu menu;
    menu.addItem ("Rename Marker...", [this, marker]
    {
        auto* window = new juce::AlertWindow ("Rename Marker", {}, juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("name", marker[ids::name].toString());
        window->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([this, marker, window] (int result) mutable
        {
            const auto name = window->getTextEditorContents ("name").trim();
            if (result == 1 && name.isNotEmpty())
            {
                auto* um = undoManager ? undoManager() : nullptr;
                if (um != nullptr)
                    um->beginNewTransaction ("Rename Marker");
                marker.setProperty (ids::name, name, um);
            }
        }), true);
    });
    menu.addItem ("Delete Marker", [this, marker]
    {
        auto* um = undoManager ? undoManager() : nullptr;
        if (um != nullptr)
            um->beginNewTransaction ("Delete Marker");
        auto parent = marker.getParent();
        parent.removeChild (marker, um);
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

void TimelineRuler::setPlayheadPosition (juce::int64 sample)
{
    if (sample == playhead)
        return;
    const auto oldX = trackHeaderWidth + (int) juce::jlimit (-1.0e6, 1.0e6, state.sampleToX (playhead));
    playhead = sample;
    const auto newX = trackHeaderWidth + (int) juce::jlimit (-1.0e6, 1.0e6, state.sampleToX (playhead));
    repaint (oldX - 6, 0, 12, getHeight());
    repaint (newX - 6, 0, 12, getHeight());
}

void TimelineRuler::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    auto area = getLocalBounds();
    auto labels = area.removeFromLeft (trackHeaderWidth);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("Markers", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);
    g.drawText ("Tempo", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);
    g.drawText ("Bars|Beats", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);
    g.drawText ("Min:Secs", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);
    g.drawText ("Timecode", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);

    g.reduceClipRegion (area);

    // Time selection, mirrored on the ruler as in the tracks.
    if (state.hasTimeRange())
    {
        const auto x1 = (float) (trackHeaderWidth + state.sampleToX (state.selectionStart));
        const auto x2 = (float) (trackHeaderWidth + state.sampleToX (state.selectionEnd));
        g.setColour (theme::selection);
        g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, (float) getHeight()));
    }

    // Loop range: a strip under the marker row, bright when loop playback is on.
    if (const juce::int64 loopEnd = session.getProperty (ids::loopEnd, 0), loopStart = session.getProperty (ids::loopStart, 0);
        loopEnd > loopStart)
    {
        const auto x1 = (float) (trackHeaderWidth + state.sampleToX (loopStart));
        const auto x2 = (float) (trackHeaderWidth + state.sampleToX (loopEnd));
        const bool on = session.getProperty (ids::loopEnabled, false);
        g.setColour (theme::loop.withAlpha (on ? 0.9f : 0.3f));
        g.fillRect (juce::Rectangle<float> (x1, (float) rowHeight - 4.0f, x2 - x1, 4.0f));
    }

    // Punch range: a red strip just above the loop strip.
    if (const juce::int64 punchOut = session.getProperty (ids::punchOut, 0), punchIn = session.getProperty (ids::punchIn, 0);
        punchOut > punchIn)
    {
        const auto x1 = (float) (trackHeaderWidth + state.sampleToX (punchIn));
        const auto x2 = (float) (trackHeaderWidth + state.sampleToX (punchOut));
        const bool on = session.getProperty (ids::punchEnabled, false);
        g.setColour (theme::record.withAlpha (on ? 0.9f : 0.3f));
        g.fillRect (juce::Rectangle<float> (x1, (float) rowHeight - 8.0f, x2 - x1, 3.0f));
    }

    paintMarkers (g, area.removeFromTop (rowHeight));
    paintTempo (g, area.removeFromTop (rowHeight));
    paintBarsBeats (g, area.removeFromTop (rowHeight));
    paintMinSecs (g, area.removeFromTop (rowHeight));
    paintTimecode (g, area.removeFromTop (rowHeight));

    g.setColour (theme::divider);
    g.drawHorizontalLine (rowHeight, (float) trackHeaderWidth, (float) getWidth());
    g.drawHorizontalLine (rowHeight * 2, (float) trackHeaderWidth, (float) getWidth());
    g.drawHorizontalLine (rowHeight * 3, (float) trackHeaderWidth, (float) getWidth());
    g.drawHorizontalLine (rowHeight * 4, (float) trackHeaderWidth, (float) getWidth());
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine (trackHeaderWidth - 1, 0.0f, (float) getHeight());

    // Playhead marker
    const auto px = (float) (trackHeaderWidth + state.sampleToX (playhead));
    juce::Path marker;
    marker.addTriangle (px - 5.0f, (float) getHeight() - 9.0f, px + 5.0f, (float) getHeight() - 9.0f, px, (float) getHeight() - 1.0f);
    g.setColour (theme::playhead);
    g.fillPath (marker);
}

void TimelineRuler::paintBarsBeats (juce::Graphics& g, juce::Rectangle<int> row)
{
    // Bars and beats follow the tempo and meter maps: bar lengths change with both.
    const auto& meter = state.meterMap;
    const auto firstBar = meter.positionOf (state.sampleToTicks (state.viewStart)).bar;
    const auto lastBar = meter.positionOf (state.sampleToTicks (state.xToSample (row.getWidth()))).bar + 1;

    // Label every 2^n bars so labels stay ~60 px apart (judged on the first visible bar).
    const double firstBarPixels = (double) (state.ticksToSample (meter.barToTicks (firstBar + 1))
                                            - state.ticksToSample (meter.barToTicks (firstBar))) / state.samplesPerPixel;
    int barStep = 1;
    while (firstBarPixels * barStep < 60.0 && barStep < 1024)
        barStep *= 2;

    g.setFont (theme::font (11.0f));
    for (auto bar = std::max<juce::int64> (0, firstBar - (firstBar % barStep)); bar <= lastBar; ++bar)
    {
        const auto& m = meter.meterAt (bar);
        const auto barStart = meter.barToTicks (bar);
        const auto x = (float) (row.getX() + state.sampleToX (state.ticksToSample (barStart)));
        const bool labelled = bar % barStep == 0;
        const auto beatPixels = (double) (state.ticksToSample (barStart + midi::MeterMap::ticksPerBeat (m)) - state.ticksToSample (barStart))
                                / state.samplesPerPixel;

        if (labelled || beatPixels >= 12.0)
        {
            g.setColour (labelled ? theme::text : theme::textDim);
            g.drawVerticalLine ((int) x, (float) row.getY() + (labelled ? 4.0f : 12.0f), (float) row.getBottom());
        }
        if (labelled)
        {
            // A new time signature is shown on the bar where it starts.
            const bool meterChanges = bar > 0 && std::any_of (meter.getChanges().begin(), meter.getChanges().end(),
                                                              [bar] (const midi::MeterMap::Change& c) { return c.bar == bar; });
            g.drawText (juce::String (bar + 1) + (meterChanges ? "  " + juce::String (m.numerator) + "/" + juce::String (m.denominator) : juce::String()),
                        juce::Rectangle<float> (x + 3.0f, (float) row.getY(), 90.0f, (float) row.getHeight()),
                        juce::Justification::centredLeft);
        }

        if (beatPixels >= 12.0)
        {
            g.setColour (theme::textDim);
            for (int beat = 1; beat < m.numerator; ++beat)
            {
                const auto bx = (float) (row.getX() + state.sampleToX (state.ticksToSample (barStart + beat * midi::MeterMap::ticksPerBeat (m))));
                g.drawVerticalLine ((int) bx, (float) row.getBottom() - 5.0f, (float) row.getBottom());
            }
        }
    }
}

void TimelineRuler::paintMinSecs (juce::Graphics& g, juce::Rectangle<int> row)
{
    static constexpr double steps[] = { 0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
    const double pixelsPerSecond = state.sampleRate / state.samplesPerPixel;

    double step = steps[std::size (steps) - 1];
    for (const double s : steps)
    {
        if (s * pixelsPerSecond >= 90.0)
        {
            step = s;
            break;
        }
    }

    const double startSeconds = (double) state.viewStart / state.sampleRate;
    const double endSeconds = (double) state.xToSample (row.getWidth()) / state.sampleRate;

    g.setFont (theme::font (11.0f));
    for (double t = std::floor (startSeconds / step) * step; t <= endSeconds; t += step)
    {
        const auto x = (float) (row.getX() + state.sampleToX ((juce::int64) std::llround (t * state.sampleRate)));
        g.setColour (theme::text);
        g.drawVerticalLine ((int) x, (float) row.getY() + 4.0f, (float) row.getBottom());

        juce::String label = timeformat::minSecs ((juce::int64) std::llround (t * state.sampleRate), state.sampleRate);
        if (step >= 1.0)
            label = label.upToLastOccurrenceOf (".", false, false);
        g.drawText (label, juce::Rectangle<float> (x + 3.0f, (float) row.getY(), 90.0f, (float) row.getHeight()),
                    juce::Justification::centredLeft);
    }
}

namespace
{
midi::FrameRate rateOf (const juce::ValueTree& session)
{
    return midi::frameRateFromName (session.getProperty (ids::timecodeRate, "25").toString().toStdString()).value_or (midi::FrameRate::fps25);
}
} // namespace

juce::String TimelineRuler::timecodeAt (juce::int64 sample) const
{
    return midi::format (sample, state.sampleRate, rateOf (session), (juce::int64) session.getProperty (ids::timecodeStart, 0));
}

void TimelineRuler::paintTimecode (juce::Graphics& g, juce::Rectangle<int> row)
{
    // Labels on whole frames, spaced at least 90 px apart: a number of frames, then of seconds.
    const auto rate = rateOf (session);
    const double fps = midi::framesPerSecond (rate);
    const double pixelsPerFrame = state.sampleRate / fps / state.samplesPerPixel;
    const int nominal = midi::nominalFramesPerSecond (rate);
    juce::int64 step = (juce::int64) nominal * 600;
    for (const juce::int64 frames : { (juce::int64) 1, (juce::int64) 2, (juce::int64) 5, (juce::int64) 10, (juce::int64) nominal,
                                      (juce::int64) nominal * 2, (juce::int64) nominal * 5, (juce::int64) nominal * 10,
                                      (juce::int64) nominal * 30, (juce::int64) nominal * 60, (juce::int64) nominal * 300 })
        if ((double) frames * pixelsPerFrame >= 90.0)
        {
            step = frames;
            break;
        }

    const auto first = midi::frameAtSample (std::max<juce::int64> (0, state.viewStart), state.sampleRate, rate);
    const auto last = midi::frameAtSample (state.xToSample (row.getWidth()), state.sampleRate, rate);
    g.setFont (theme::font (11.0f));
    for (auto f = (first / step) * step; f <= last; f += step)
    {
        const auto sample = midi::sampleAtFrame (f, state.sampleRate, rate);
        const auto x = (float) (row.getX() + state.sampleToX (sample));
        g.setColour (theme::text);
        g.drawVerticalLine ((int) x, (float) row.getY() + 4.0f, (float) row.getBottom());
        g.drawText (timecodeAt (sample), juce::Rectangle<float> (x + 3.0f, (float) row.getY(), 90.0f, (float) row.getHeight()),
                    juce::Justification::centredLeft);
    }
}

void TimelineRuler::showTimecodeMenu()
{
    const auto current = rateOf (session);
    const juce::int64 start = session.getProperty (ids::timecodeStart, 0);
    auto set = [this] (const juce::Identifier& property, const juce::var& value, const juce::String& name)
    {
        auto* um = undoManager ? undoManager() : nullptr;
        if (um != nullptr)
            um->beginNewTransaction (name);
        session.setProperty (property, value, um);
        repaint();
    };

    juce::PopupMenu menu;
    menu.addSectionHeader ("Timecode Rate");
    for (const auto r : { midi::FrameRate::fps23_976, midi::FrameRate::fps24, midi::FrameRate::fps25, midi::FrameRate::fps29_97,
                          midi::FrameRate::fps29_97drop, midi::FrameRate::fps30 })
    {
        // The start stays the same moment (its frame count at the new rate follows).
        menu.addItem (midi::frameRateLabel (r), true, r == current, [this, r, current, start, set]
        {
            const double startSeconds = (double) start / midi::framesPerSecond (current);
            set (ids::timecodeRate, juce::String (midi::frameRateName (r)), "Timecode Rate");
            session.setProperty (ids::timecodeStart, (juce::int64) std::llround (startSeconds * midi::framesPerSecond (r)),
                                 undoManager ? undoManager() : nullptr);
        });
    }
    menu.addSeparator();
    menu.addSectionHeader ("Session Start");
    const auto hour = (juce::int64) midi::toFrameCount ({ 1, 0, 0, 0 }, current); // int64_t is long on Linux
    menu.addItem ("00:00:00:00", true, start == 0, [set] { set (ids::timecodeStart, 0, "Timecode Start"); });
    menu.addItem ("01:00:00:00 (film and TV)", true, start == hour, [set, hour] { set (ids::timecodeStart, hour, "Timecode Start"); });
    menu.addItem ("Other...", [this, current, set]
    {
        auto* window = new juce::AlertWindow ("Session Start", "The timecode of the session's first sample (hh:mm:ss:ff).",
                                              juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("start", midi::toString (midi::fromFrameCount ((juce::int64) session.getProperty (ids::timecodeStart, 0), current), current));
        window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([window, current, set] (int result)
        {
            if (result != 1)
                return;
            if (const auto tc = midi::parse (window->getTextEditorContents ("start").toStdString(), current))
                set (ids::timecodeStart, (juce::int64) midi::toFrameCount (*tc, current), "Timecode Start");
        }), true);
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

void TimelineRuler::locateFromMouse (const juce::MouseEvent& e)
{
    if (e.x < trackHeaderWidth || ! onLocate)
        return;
    onLocate (state.snap (std::max<juce::int64> (0, state.xToSample (e.x - trackHeaderWidth))));
}

void TimelineRuler::mouseDown (const juce::MouseEvent& e)
{
    if (e.y >= rowHeight * 4 && e.mods.isPopupMenu())
    {
        showTimecodeMenu();
        return;
    }
    if (e.y >= rowHeight && e.y < rowHeight * 2 && e.mods.isPopupMenu() && e.x >= trackHeaderWidth)
    {
        if (const auto mark = tempoMarkAt (e.x))
            showTempoMenu (*mark);
        else
            showTempoDialog (state.meterMap.positionOf (state.sampleToTicks (std::max<juce::int64> (0, state.xToSample (e.x - trackHeaderWidth)))).bar);
        return;
    }
    if (e.y < rowHeight)
    {
        if (const auto m = markerAt (e.x); m.isValid())
        {
            if (e.mods.isPopupMenu())
                showMarkerMenu (m);
            else if (onLocate)
                onLocate ((juce::int64) m[ids::start]);
            return;
        }
    }
    locateFromMouse (e);
}
void TimelineRuler::mouseDrag (const juce::MouseEvent& e) { locateFromMouse (e); }

void TimelineRuler::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < rowHeight || e.y >= rowHeight * 2 || e.x < trackHeaderWidth)
        return;
    if (const auto mark = tempoMarkAt (e.x))
        showTempoDialog (mark->bar);
    else
        showTempoDialog (state.meterMap.positionOf (state.sampleToTicks (std::max<juce::int64> (0, state.xToSample (e.x - trackHeaderWidth)))).bar);
}

//==============================================================================
// Tempo ruler

namespace
{
juce::String formatBpm (double bpm)
{
    auto text = juce::String (bpm, 2);
    while (text.containsChar ('.') && (text.endsWithChar ('0') || text.endsWithChar ('.')))
        text = text.dropLastCharacters (1);
    return text;
}
} // namespace

std::vector<TimelineRuler::TempoMark> TimelineRuler::tempoMarks() const
{
    // From the session itself when there is one, so an edit shows at once (the Edit
    // window refreshes its own copies of the maps on a timer).
    const auto* s = getSession ? getSession() : nullptr;
    const auto tempo = s != nullptr ? midimodel::tempoMap (*s) : state.tempoMap;
    const auto meter = s != nullptr ? midimodel::meterMap (*s) : state.meterMap;
    std::vector<TempoMark> marks;
    auto add = [&marks] (juce::int64 tick, juce::int64 bar, bool isTempo)
    {
        for (auto& m : marks)
            if (m.tick == tick)
            {
                (isTempo ? m.hasTempo : m.hasMeter) = true;
                return;
            }
        TempoMark m;
        m.tick = tick;
        m.bar = bar;
        (isTempo ? m.hasTempo : m.hasMeter) = true;
        marks.push_back (m);
    };

    add (0, 0, true); // the starting tempo and time signature are always shown
    add (0, 0, false);
    for (const auto& c : tempo.getChanges())
        add ((juce::int64) c.tick, (juce::int64) meter.positionOf (c.tick).bar, true);
    for (const auto& c : meter.getChanges())
        add ((juce::int64) meter.barToTicks (c.bar), (juce::int64) c.bar, false);

    std::sort (marks.begin(), marks.end(), [] (const TempoMark& a, const TempoMark& b) { return a.tick < b.tick; });
    for (auto& m : marks)
    {
        juce::StringArray parts;
        if (m.hasTempo)
        {
            for (const auto& c : tempo.getChanges())
                if (c.tick == m.tick)
                    m.ramp = c.ramp && &c != &tempo.getChanges().back();
            parts.add (formatBpm (tempo.bpmAt (m.tick)) + " bpm" + (m.ramp ? " (ramp)" : ""));
        }
        if (m.hasMeter)
        {
            const auto& sig = meter.meterAt (m.bar);
            parts.add (juce::String (sig.numerator) + "/" + juce::String (sig.denominator));
        }
        m.label = parts.joinIntoString (" ");
    }
    return marks;
}

juce::StringArray TimelineRuler::getTempoLabels() const
{
    juce::StringArray labels;
    for (const auto& m : tempoMarks())
        labels.add (m.label);
    return labels;
}

std::optional<TimelineRuler::TempoMark> TimelineRuler::tempoMarkAt (int x) const
{
    for (const auto& m : tempoMarks())
    {
        const auto mx = trackHeaderWidth + (int) state.sampleToX (state.ticksToSample (m.tick));
        const auto width = 10 + juce::GlyphArrangement::getStringWidthInt (theme::font (11.0f), m.label);
        if (x >= mx - 4 && x <= mx + width)
            return m;
    }
    return std::nullopt;
}

void TimelineRuler::paintTempo (juce::Graphics& g, juce::Rectangle<int> row)
{
    g.setFont (theme::font (11.0f));
    for (const auto& m : tempoMarks())
    {
        const auto x = (float) (row.getX() + state.sampleToX (state.ticksToSample (m.tick)));
        if (x > (float) row.getRight())
            break;
        g.setColour (theme::automation);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) row.getY() + 3.0f, 3.0f, (float) row.getHeight() - 6.0f));
        g.setColour (theme::text);
        g.drawText (m.label, juce::Rectangle<float> (x + 5.0f, (float) row.getY(), 160.0f, (float) row.getHeight()),
                    juce::Justification::centredLeft);
    }
}

bool TimelineRuler::setTempoAtBar (juce::int64 bar, double bpm, int numerator, int denominator)
{
    auto* s = getSession ? getSession() : nullptr;
    auto* um = undoManager ? undoManager() : nullptr;
    if (s == nullptr || bar < 0)
        return false;
    if (um != nullptr)
        um->beginNewTransaction ("Tempo Change");
    midimodel::setTempoChange (*s, (juce::int64) midimodel::meterMap (*s).barToTicks (bar), bpm, um);
    const auto current = midimodel::meterMap (*s).meterAt (bar);
    if (current.numerator != numerator || current.denominator != denominator)
        midimodel::setMeterChange (*s, bar, numerator, denominator, um);
    return true;
}

bool TimelineRuler::setRampAt (juce::int64 tick, bool ramp)
{
    auto* s = getSession ? getSession() : nullptr;
    auto* um = undoManager ? undoManager() : nullptr;
    if (s == nullptr)
        return false;
    if (um != nullptr)
        um->beginNewTransaction ("Tempo Ramp");
    return midimodel::setTempoRamp (*s, tick, ramp, um);
}

bool TimelineRuler::deleteTempoChange (juce::int64 tick, juce::int64 bar)
{
    auto* s = getSession ? getSession() : nullptr;
    auto* um = undoManager ? undoManager() : nullptr;
    if (s == nullptr)
        return false;
    if (um != nullptr)
        um->beginNewTransaction ("Delete Tempo Change");
    const bool tempoRemoved = midimodel::removeTempoChange (*s, tick, um);
    const bool meterRemoved = midimodel::removeMeterChange (*s, bar, um);
    return tempoRemoved || meterRemoved;
}

void TimelineRuler::showTempoDialog (juce::int64 bar)
{
    const auto tick = state.meterMap.barToTicks (bar);
    const auto& sig = state.meterMap.meterAt (bar);
    auto* window = new juce::AlertWindow ("Tempo at bar " + juce::String (bar + 1),
                                          "Tempo (20-999 bpm) and time signature from the start of this bar.",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("bpm", formatBpm (state.tempoMap.bpmAt (tick)), "Tempo");
    window->addTextEditor ("meter", juce::String (sig.numerator) + "/" + juce::String (sig.denominator), "Time signature");
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<TimelineRuler> (this), bar, window] (int result)
        {
            if (safe == nullptr || result != 1)
                return;
            const auto bpmText = window->getTextEditorContents ("bpm").trim();
            const auto meterText = window->getTextEditorContents ("meter").trim();
            const auto numerator = meterText.upToFirstOccurrenceOf ("/", false, false).trim();
            const auto denominator = meterText.fromFirstOccurrenceOf ("/", false, false).trim();
            const double bpm = bpmText.getDoubleValue();
            const bool valid = bpmText.containsOnly ("0123456789.") && bpm >= midi::TempoMap::minBpm && bpm <= midi::TempoMap::maxBpm
                               && meterText.containsChar ('/') && numerator.isNotEmpty() && denominator.isNotEmpty()
                               && numerator.containsOnly ("0123456789") && denominator.containsOnly ("0123456789");
            if (! valid)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Tempo",
                                                        "Give a tempo from 20 to 999 bpm and a time signature such as 3/4 or 6/8.");
                return;
            }
            safe->setTempoAtBar (bar, bpm, numerator.getIntValue(), denominator.getIntValue());
        }), true);
}

void TimelineRuler::showTempoMenu (const TempoMark& mark)
{
    juce::PopupMenu menu;
    menu.addItem ("Edit Tempo / Time Signature...", [this, bar = mark.bar] { showTempoDialog (bar); });
    menu.addItem ("Ramp to Next Change", mark.hasTempo, mark.ramp, [this, mark] { setRampAt (mark.tick, ! mark.ramp); });
    menu.addItem ("Delete This Change", mark.tick > 0, false,
                  [this, mark] { deleteTempoChange (mark.hasTempo ? mark.tick : 0, mark.hasMeter ? mark.bar : 0); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

} // namespace meridian
