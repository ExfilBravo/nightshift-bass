#include "Split.h"

using namespace bass;
namespace bdsp = bass::dsp;
using namespace bass::theme;

namespace
{
    // Looked up when drawn, not copied into an array of its own at startup: the theme's colours are
    // globals of another file, and in what order files' globals start up is the linker's choice (a
    // Mac's took the copy first, while they were still black)
    juce::Colour bandColour (int b)     { return b == 0 ? violet : b == 1 ? amber : teal; }

    const char* const bandNames[3] = { "LOW", "MID", "HIGH" };

    constexpr float loHz = 30.0f, hiHz = 16000.0f, floorDb = -30.0f;

    juce::Rectangle<int> knobCell (juce::Rectangle<int> r)
    {
        return r.withSizeKeepingCentre (Knob::defaultSize, std::min (r.getHeight(), Knob::defaultSize + 32));
    }

    /** Place knobs in equal cells across a row. */
    void placeRow (const std::vector<Knob*>& knobs, juce::Rectangle<int> row, int cells)
    {
        const int w = row.getWidth() / cells;
        auto r = row.withSizeKeepingCentre (w * (int) knobs.size(), row.getHeight());

        for (auto* k : knobs)
            k->setBounds (knobCell (r.removeFromLeft (w)));
    }

    /** "160", "1.6k", "1.65k", "10k": no trailing zeros. */
    juce::String hzText (float hz)
    {
        if (hz < 1000.0f)
            return juce::String (juce::roundToInt (hz));

        auto s = juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 0);

        if (s.containsChar ('.'))
            s = s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return s + "k";
    }
}

//==============================================================================
SplitEditor::BandDisplay::BandDisplay (SplitProcessor& p)
    : processor (p),
      lowAttachment (*p.getState().getParameter ("lowCross"), [this] (float v) { lowHz = v; repaint(); }, nullptr),
      highAttachment (*p.getState().getParameter ("highCross"), [this] (float v) { highHz = v; repaint(); }, nullptr)
{
    lowAttachment.sendInitialUpdate();
    highAttachment.sendInitialUpdate();
    setTooltip ("Drag a split line to move the crossover; double-click it to reset");
}

juce::Rectangle<float> SplitEditor::BandDisplay::plotArea() const
{
    return getLocalBounds().toFloat().reduced (10.0f, 8.0f).withTrimmedTop (16.0f).withTrimmedBottom (18.0f);
}

float SplitEditor::BandDisplay::xForHz (float hz) const
{
    const auto p = plotArea();
    return p.getX() + p.getWidth() * std::log (hz / loHz) / std::log (hiHz / loHz);
}

float SplitEditor::BandDisplay::hzForX (float x) const
{
    const auto p = plotArea();
    return loHz * std::pow (hiHz / loHz, juce::jlimit (0.0f, 1.0f, (x - p.getX()) / p.getWidth()));
}

int SplitEditor::BandDisplay::handleNear (float x) const
{
    const float dl = std::abs (x - xForHz (lowHz)), dh = std::abs (x - xForHz (highHz));

    if (std::min (dl, dh) > 10.0f)
        return -1;

    return dl <= dh ? 0 : 1;
}

void SplitEditor::BandDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int h = handleNear (e.position.x);

    if (h != hover)
    {
        hover = h;
        setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void SplitEditor::BandDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragging = handleNear (e.position.x);

    if (dragging == 0)  lowAttachment.beginGesture();
    if (dragging == 1)  highAttachment.beginGesture();
}

void SplitEditor::BandDisplay::mouseDrag (const juce::MouseEvent& e)
{
    const float hz = hzForX (e.position.x);

    if (dragging == 0)  lowAttachment.setValueAsPartOfGesture (juce::jlimit (60.0f, 400.0f, hz));
    if (dragging == 1)  highAttachment.setValueAsPartOfGesture (juce::jlimit (600.0f, 5000.0f, hz));
}

void SplitEditor::BandDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragging == 0)  lowAttachment.endGesture();
    if (dragging == 1)  highAttachment.endGesture();
    dragging = -1;
}

void SplitEditor::BandDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int h = handleNear (e.position.x);

    if (h == 0)  lowAttachment.setValueAsCompleteGesture (160.0f);
    if (h == 1)  highAttachment.setValueAsCompleteGesture (1600.0f);
}

void SplitEditor::BandDisplay::push (const float* newLevels, float reductionDb)
{
    bool changed = std::abs (reductionDb - reduction) > 0.05f;
    const float bandDb[3] = { processor.param ("lowLevel"), processor.param ("midLevel"), processor.param ("highLevel") };

    for (int b = 0; b < 3; ++b)
    {
        changed = changed || bandDb[b] != drawnDb[b];
        drawnDb[b] = bandDb[b];
        const float target = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (newLevels[b], -60.0f) + 60.0f) / 60.0f);
        const float next = levels[b] + (target - levels[b]) * (target > levels[b] ? 0.6f : 0.15f);
        changed = changed || std::abs (next - levels[b]) > 0.002f;
        levels[b] = next;
    }

    reduction = reductionDb;

    if (changed)
        repaint();
}

void SplitEditor::BandDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.0f);

    const auto p = plotArea();
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (floorDb, 6.0f, db), 6.0f, floorDb, p.getY(), p.getBottom()); };

    // Grid
    g.setColour (juce::Colours::white.withAlpha (0.05f));

    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        g.drawVerticalLine (juce::roundToInt (xForHz (hz)), p.getY(), p.getBottom());

    g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), p.getX(), p.getRight());

    g.setColour (textDim);
    g.setFont (monoFont (12.0f));

    for (float hz : { 100.0f, 1000.0f, 10000.0f })
        g.drawText (hzText (hz), juce::Rectangle<float> (xForHz (hz) - 20.0f, p.getBottom() + 3.0f, 40.0f, 12.0f), juce::Justification::centred, true);

    // Each band's Linkwitz-Riley response, scaled by its level and lit by its signal
    const float bandDb[3] = { processor.param ("lowLevel"), processor.param ("midLevel"), processor.param ("highLevel") };

    for (int b = 0; b < 3; ++b)
    {
        if (bandDb[b] <= -39.9f)
            continue;

        juce::Path curve, fill;
        bool first = true;

        for (float x = p.getX(); x <= p.getRight() + 0.5f; x += 2.0f)
        {
            const float hz = hzForX (x);
            const float a = std::pow (hz / lowHz, 4.0f), c = std::pow (hz / highHz, 4.0f);
            const float lp1 = 1.0f / (1.0f + a), hp1 = a / (1.0f + a), lp2 = 1.0f / (1.0f + c), hp2 = c / (1.0f + c);
            const float mag = b == 0 ? lp1 : (b == 1 ? hp1 * lp2 : hp1 * hp2);
            const float y = yFor (juce::Decibels::gainToDecibels (mag, -100.0f) + bandDb[b]);

            if (first)
            {
                curve.startNewSubPath (x, y);
                fill.startNewSubPath (x, p.getBottom());
                first = false;
            }
            else
            {
                curve.lineTo (x, y);
            }

            fill.lineTo (x, y);
        }

        fill.lineTo (p.getRight(), p.getBottom());
        fill.closeSubPath();

        const auto colour = bandColour (b);
        g.setColour (colour.withAlpha (0.07f + 0.28f * levels[b]));
        g.fillPath (fill);
        g.setColour (colour.withAlpha (0.55f + 0.45f * levels[b]));
        g.strokePath (curve, juce::PathStrokeType (1.6f));
    }

    // Band names, centred in their regions, with the low band's compression
    const float edges[4] = { p.getX(), xForHz (lowHz), xForHz (highHz), p.getRight() };

    for (int b = 0; b < 3; ++b)
    {
        auto label = juce::Rectangle<float> (edges[b], r.getY() + 4.0f, edges[b + 1] - edges[b], 15.0f);
        g.setColour (bandColour (b).withAlpha (bandDb[b] <= -39.9f ? 0.6f : 1.0f));
        g.setFont (font (11.5f, true).withExtraKerningFactor (0.1f));
        juce::String name = bandNames[b];

        if (b == 0 && reduction > 0.05f)
            name << "  -" << juce::String (reduction, 1) << " dB";

        if (bandDb[b] <= -39.9f)
            name << " (off)";

        g.drawText (name, label, juce::Justification::centred, true);
    }

    // Crossover handles
    for (int h = 0; h < 2; ++h)
    {
        const float hz = h == 0 ? lowHz : highHz;
        const float x = xForHz (hz);
        const bool hot = hover == h || dragging == h;

        g.setColour (juce::Colours::white.withAlpha (hot ? 0.75f : 0.35f));
        g.drawLine (x, p.getY(), x, p.getBottom(), hot ? 1.6f : 1.0f);

        auto tag = juce::Rectangle<float> (46.0f, 15.0f).withCentre ({ x, p.getBottom() - 10.0f });
        tag.setX (juce::jlimit (p.getX(), p.getRight() - tag.getWidth(), tag.getX()));
        g.setColour (juce::Colour (0xff1d2230));
        g.fillRoundedRectangle (tag, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (hot ? 0.6f : 0.25f));
        g.drawRoundedRectangle (tag, 3.0f, 1.0f);
        g.setColour (text);
        g.setFont (monoFont (12.0f));
        g.drawText (hzText (hz), tag, juce::Justification::centred, true);

        auto grip = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ x, p.getY() + 3.0f });
        g.setColour (hot ? juce::Colours::white : juce::Colour (0xffb9c0cf));
        g.fillEllipse (grip);
        g.setColour (outline);
        g.drawEllipse (grip, 1.0f);
    }
}

//==============================================================================
SplitEditor::SplitEditor (SplitProcessor& p)
    : BassEditor (p), split (p), display (p),
      eqPanel (p.getState(), teal),
      cabSelector (p, teal),
      cabView (p, teal)
{
    addAndMakeVisible (display);
    addAndMakeVisible (eqPanel);
    addAndMakeVisible (cabSelector);
    addAndMakeVisible (cabView);

    gateKnob = &addKnob ("gate", "Gate", textDim.brighter (0.4f));
    gateKnob->getSlider().setTooltip ("Noise gate threshold; all the way down is off");
    character = &addKeys ("character", "Character", amber);
    character->setTooltip ("Clipping in the mid and high bands. Smooth: warm. Tight: firm. Fuzz: asymmetric and torn");

    auto& lowSplit = addKnob ("lowCross", "Split", violet);
    auto& lowComp = addKnob ("lowComp", "Comp", violet);
    auto& lowLevel = addKnob ("lowLevel", "Level", violet);
    lowSplit.getSlider().setTooltip ("Where the clean low band ends and the driven mids begin");
    lowComp.getSlider().setTooltip ("Compression on the clean low band: a steady floor under the dirt");
    lowKnobs = { &lowSplit, &lowComp, &lowLevel };

    auto& midDrive = addKnob ("midDrive", "Drive", amber);
    midDrive.getSlider().setTooltip ("Drive into the mid band's clipper: the growl");
    midKnobs = { &midDrive, &addKnob ("midLevel", "Level", amber) };

    auto& highSplit = addKnob ("highCross", "Split", teal);
    auto& highDrive = addKnob ("highDrive", "Drive", teal);
    highSplit.getSlider().setTooltip ("Where the mid band ends and the high band begins");
    highDrive.getSlider().setTooltip ("Drive into the high band's clipper: the fizz and bite");
    highKnobs = { &highSplit, &highDrive, &addKnob ("highLevel", "Level", teal) };

    outKnobs = { &addKnob ("mix", "Mix", violet), &addKnob ("output", "Output", violet, true) };

    setSize (1000, 620);
}

void SplitEditor::layoutBody (juce::Rectangle<int> body)
{
    auto row1 = body.removeFromTop (170);
    body.removeFromTop (10);
    auto row2 = body.removeFromTop (150);
    body.removeFromTop (10);
    auto row3 = body;

    displayArea = row1.removeFromLeft (586);
    row1.removeFromLeft (10);
    inputPanel = row1;

    const int third = (row2.getWidth() - 20) / 3;
    lowPanel = row2.removeFromLeft (third);
    row2.removeFromLeft (10);
    midPanel = row2.removeFromLeft (third);
    row2.removeFromLeft (10);
    highPanel = row2;

    eqArea = row3.removeFromLeft (460);
    row3.removeFromLeft (10);
    cabArea = row3.removeFromLeft (300);
    row3.removeFromLeft (10);
    outPanel = row3;

    display.setBounds (displayArea.withTrimmedTop (24).reduced (12, 0).withTrimmedBottom (10));

    {
        auto r = inputPanel.withTrimmedTop (22).reduced (10, 2);
        gateKnob->setBounds (knobCell (r.removeFromLeft (110)));
        character->setBounds (r.withSizeKeepingCentre (character->getIdealWidth(), KeySwitch::idealHeight));
    }

    auto content = [] (juce::Rectangle<int> area) { return area.withTrimmedTop (20).reduced (8, 0); };
    placeRow (lowKnobs, content (lowPanel), 3);
    placeRow (midKnobs, content (midPanel), 3);
    placeRow (highKnobs, content (highPanel), 3);
    placeRow (outKnobs, content (outPanel).withTrimmedTop (10), 2);

    eqPanel.setBounds (eqArea.withTrimmedTop (24).reduced (12, 0).withTrimmedBottom (8));
    {
        auto r = cabArea.withTrimmedTop (24).reduced (5, 0).withTrimmedBottom (8);
        cabSelector.setBounds (r.removeFromTop (CabSelector::idealHeight).withSizeKeepingCentre (290, CabSelector::idealHeight));
        r.removeFromTop (6);
        cabView.setBounds (r.reduced (5, 0));
    }
}

void SplitEditor::paintPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, juce::Colour accent)
{
    auto box = r.toFloat();
    g.setColour (theme::panel.withAlpha (0.55f));
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);
    drawSectionTitle (g, r.withHeight (18).translated (0, 4).reduced (14, 0), title, accent);
}

void SplitEditor::paintBody (juce::Graphics& g, juce::Rectangle<int>)
{
    paintPanel (g, displayArea, "Bands", text);
    paintPanel (g, inputPanel, "Input", textDim.brighter (0.4f));
    paintPanel (g, lowPanel, "Low  -  clean", violet);
    paintPanel (g, midPanel, "Mid  -  drive", amber);
    paintPanel (g, highPanel, "High  -  drive", teal);
    paintPanel (g, eqArea, "Graphic EQ", teal);
    paintPanel (g, cabArea, "Cabinet", teal);
    paintPanel (g, outPanel, "Out", violet);
}

void SplitEditor::animate (float)
{
    cabSelector.refresh();
    cabView.refresh();
    const float levels[3] = { split.getBandLevel (0), split.getBandLevel (1), split.getBandLevel (2) };
    display.push (levels, split.getCompReduction());
}
