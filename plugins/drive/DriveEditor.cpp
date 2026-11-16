#include "Drive.h"

using namespace bass;
namespace bdsp = bass::dsp;
using namespace bass::theme;

namespace
{
    // The pedal: midnight graphite in a matte powder coat, printed in warm cream, amber where lit.
    // (Literals, not copies of the theme's colours: those are another file's globals.)
    const juce::Colour faceTop     { 0xff2d323c };
    const juce::Colour faceBottom  { 0xff1f232b };
    const juce::Colour rimLight    { 0xff4a515e };
    const juce::Colour rimDark     { 0xff101217 };
    const juce::Colour silkscreen  { 0xffefe6d1 };
    const juce::Colour readout     { 0xffcfc6b1 };
    const juce::Colour lampAmber   { 0xffffa53d };

    // The back panel: the same metal, a shade darker
    const juce::Colour plateTop    { 0xff20242c };
    const juce::Colour plateBottom { 0xff181b22 };

    constexpr int rim = 7;                   // the die-cast edge round the pedal's face
    constexpr float pedalCorner = 18.0f;

    juce::Rectangle<int> knobCell (juce::Rectangle<int> r)
    {
        return r.withSizeKeepingCentre (Knob::defaultSize, std::min (r.getHeight(), Knob::defaultSize + 32));
    }

    /** A powder-coated face: matte, with a fine grain (seeded, so repaints don't shimmer) and a
        soft sheen across the top. */
    void paintMatte (juce::Graphics& g, juce::Rectangle<float> face, float corner, juce::Colour top, juce::Colour bottom,
                     int seed, int grains)
    {
        g.setGradientFill (juce::ColourGradient (top, face.getCentreX(), face.getY(), bottom, face.getCentreX(), face.getBottom(), false));
        g.fillRoundedRectangle (face, corner);

        // Only the grains inside what's being repainted are drawn (the LED repaints 30 times a second),
        // but every one is drawn from the sequence, so each lands in the same place every time
        juce::Random rng (seed);
        g.saveState();
        g.reduceClipRegion (face.reduced (2.0f).toNearestInt());
        const auto dirty = g.getClipBounds().toFloat().expanded (1.0f);

        for (int i = 0; i < grains; ++i)
        {
            const float x = face.getX() + rng.nextFloat() * face.getWidth();
            const float y = face.getY() + rng.nextFloat() * face.getHeight();
            const bool light = rng.nextBool();

            if (dirty.contains (x, y))
            {
                g.setColour ((light ? juce::Colours::white : juce::Colours::black).withAlpha (0.05f));
                g.fillRect (x, y, 1.0f, 1.0f);
            }
        }

        g.restoreState();

        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.05f), face.getCentreX(), face.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), face.getCentreX(), face.getY() + face.getHeight() * 0.4f, false));
        g.fillRoundedRectangle (face, corner);
    }
}

//==============================================================================
DriveEditor::DriveEditor (DriveProcessor& p)
    : BassEditor (p), drive (p),
      eqPanel (p.getState(), amber),
      cabSelector (p, amber),
      footswitch (p.getState(), "bypass"),
      cabView (p, amber),
      grunt (p.getState(), "grunt", "Grunt", silkscreen, lampAmber),
      attack (p.getState(), "attack", "Attack", silkscreen, lampAmber),
      character (p.getState(), "character", "Character", silkscreen, lampAmber)
{
    addAndMakeVisible (eqPanel);
    addAndMakeVisible (cabSelector);
    addAndMakeVisible (footswitch);
    addAndMakeVisible (cabView);
    addAndMakeVisible (grunt);
    addAndMakeVisible (attack);
    addAndMakeVisible (character);

    auto& levelKnob = addKnob ("level", "Level", amber, true);
    auto& blendKnob = addKnob ("blend", "Blend", amber);
    auto& driveKnob = addKnob ("drive", "Drive", amber);
    auto& toneKnob  = addKnob ("tone", "Tone", amber);
    levelKnob.getSlider().setTooltip ("Level of the drive path, to balance it against the clean");
    blendKnob.getSlider().setTooltip ("Clean to drive. The clean path keeps your low end solid under the dirt");
    driveKnob.getSlider().setTooltip ("How hard the drive path hits the clipper");
    toneKnob.getSlider().setTooltip ("Top end of the drive path, after the clipper: dark to open");
    pedalKnobs = { &levelKnob, &blendKnob, &driveKnob, &toneKnob };

    grunt.setTooltip ("How much low end the clipper gets: Thin keeps it out, Raw lets the body in, Fat pushes it");
    attack.setTooltip ("Pick and string bite into the clipper: cut, flat or boosted");
    character.setTooltip ("Smooth: warm valve-like. Tight: firm op-amp and diodes. Fuzz: asymmetric and torn");

    // One column: the three levers in a line, their choices in another
    const float labelsWidth = std::max ({ grunt.getLabelsWidth(), attack.getLabelsWidth(), character.getLabelsWidth() });

    for (auto* t : { &grunt, &attack, &character })
        t->setLabelsWidth (labelsWidth);

    gateKnob = &addKnob ("gate", "Gate", teal);
    compKnob = &addKnob ("comp", "Comp", teal);
    gateKnob->getSlider().setTooltip ("Noise gate threshold; all the way down is off");
    compKnob->getSlider().setTooltip ("Compressor before the drive: evens out the notes and adds sustain");

    mixKnob = &addKnob ("mix", "Mix", violet);
    outputKnob = &addKnob ("output", "Output", violet, true);

    for (auto* k : { &levelKnob, &blendKnob, &driveKnob, &toneKnob, gateKnob, compKnob, mixKnob, outputKnob })
        k->setPedalStyle (silkscreen, readout);

    setSize (960, 540);
}

void DriveEditor::layoutBody (juce::Rectangle<int> body)
{
    pedal = body.removeFromLeft (388);
    body.removeFromLeft (14);
    backPanel = body;

    // The pedal: two rows of knobs, Level and Blend over Drive and Tone, with the three toggles
    // stacked between the columns; then the name, then the LED over the footswitch
    {
        const auto face = pedal.reduced (rim);
        auto p = face.reduced (14, 14);
        const int inset = juce::roundToInt ((float) face.getWidth() * 0.21f);
        const int left = face.getX() + inset, right = face.getRight() - inset;
        constexpr int rowHeight = Knob::defaultSize + 32, rowGap = 6;

        auto cellAt = [] (int centreX, juce::Rectangle<int> row)
        {
            return juce::Rectangle<int> (Knob::defaultSize + 12, row.getHeight()).withCentre ({ centreX, row.getCentreY() });
        };

        auto rows = p.removeFromTop (2 * rowHeight + rowGap);
        const auto row1 = rows.removeFromTop (rowHeight);
        const auto row2 = rows.removeFromBottom (rowHeight);

        pedalKnobs[0]->setBounds (cellAt (left, row1));
        pedalKnobs[1]->setBounds (cellAt (right, row1));
        pedalKnobs[2]->setBounds (cellAt (left, row2));
        pedalKnobs[3]->setBounds (cellAt (right, row2));

        // The toggles, top caption level with the top knobs' names, last choice level with the bottom values
        const int toggleWidth = std::max ({ grunt.getIdealWidth(), attack.getIdealWidth(), character.getIdealWidth() });
        auto column = juce::Rectangle<int> (face.getCentreX() - toggleWidth / 2, row1.getY(), toggleWidth, row2.getBottom() - row1.getY());
        const int toggleGap = (column.getHeight() - 3 * MiniToggle::idealHeight) / 2;

        for (auto* t : { &grunt, &attack, &character })
        {
            t->setBounds (column.removeFromTop (MiniToggle::idealHeight));
            column.removeFromTop (toggleGap);
        }

        p.removeFromTop (8);
        logoArea = p.removeFromTop (52);

        constexpr int footSize = 64;
        footswitch.setBounds (juce::Rectangle<int> (footSize, footSize).withCentre ({ face.getCentreX(), p.getBottom() - footSize / 2 - 4 }));
        ledArea = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ (float) face.getCentreX(), (float) footswitch.getY() - 18.0f });
    }

    // The back panel: Dynamics and Out on one row, the graphic EQ, the cabinet
    {
        auto r = backPanel.reduced (12, 10);
        auto top = r.removeFromTop (124);
        dynamicsPanel = top.removeFromLeft (top.getWidth() * 56 / 100);
        top.removeFromLeft (8);
        outPanel = top;
        r.removeFromTop (8);
        eqArea = r.removeFromTop (172);
        r.removeFromTop (8);
        cabArea = r;
    }

    // Dynamics: gate, comp, and the gain-reduction meter with the gate lamp
    {
        auto r = dynamicsPanel.withTrimmedTop (20).reduced (8, 2);
        reductionMeter = r.removeFromRight (70).reduced (8, 10);
        const int w = r.getWidth() / 2;
        gateKnob->setBounds (knobCell (r.removeFromLeft (w)));
        compKnob->setBounds (knobCell (r));
    }

    {
        auto r = outPanel.withTrimmedTop (20).reduced (8, 2);
        const int w = r.getWidth() / 2;
        mixKnob->setBounds (knobCell (r.removeFromLeft (w)));
        outputKnob->setBounds (knobCell (r));
    }

    eqPanel.setBounds (eqArea.withTrimmedTop (24).reduced (14, 0).withTrimmedBottom (8));

    {
        auto r = cabArea.withTrimmedTop (24).reduced (14, 0).withTrimmedBottom (8);
        cabSelector.setBounds (r.removeFromLeft (290).withSizeKeepingCentre (290, CabSelector::idealHeight));
        r.removeFromLeft (14);
        cabCurve = r;
        cabView.setBounds (cabCurve);
    }
}

//==============================================================================
void DriveEditor::paintBody (juce::Graphics& g, juce::Rectangle<int>)
{
    paintBackPanel (g, backPanel.toFloat());
    paintSection (g, dynamicsPanel, "Dynamics", teal);
    paintSection (g, outPanel, "Out", violet);
    paintSection (g, eqArea, "Graphic EQ", amber);
    paintSection (g, cabArea, "Cabinet", amber);
    paintReduction (g, reductionMeter.toFloat());
    paintPedal (g, pedal.toFloat());
}

void DriveEditor::paintBackPanel (juce::Graphics& g, juce::Rectangle<float> r)
{
    juce::DropShadow (juce::Colours::black.withAlpha (0.45f), 12, { 0, 4 }).drawForRectangle (g, r.reduced (6.0f).toNearestInt());
    g.setGradientFill (juce::ColourGradient (rimLight.darker (0.3f), 0.0f, r.getY(), rimDark, 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 12.0f);
    paintMatte (g, r.reduced (4.0f), 9.0f, plateTop, plateBottom, 11, 900);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r.reduced (4.0f), 9.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
}

void DriveEditor::paintSection (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, juce::Colour accent)
{
    // A shallow recess in the panel: shadowed inside its top edge, catching the light under its bottom one
    auto box = r.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.18f));
    g.fillRoundedRectangle (box, 7.0f);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.drawRoundedRectangle (box.reduced (0.5f), 7.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawLine (box.getX() + 7.0f, box.getBottom() + 0.5f, box.getRight() - 7.0f, box.getBottom() + 0.5f, 1.0f);
    drawSectionTitle (g, r.withHeight (18).translated (0, 4).reduced (14, 0), title, accent);
}

void DriveEditor::paintPedal (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto face = r.reduced ((float) rim);
    const float jackY = (float) footswitch.getBounds().getCentreY();

    // Its shadow on the desk, the die-cast rim lit along the top, then the matte face
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 16, { 0, 7 }).drawForRectangle (g, r.reduced (8.0f).toNearestInt());
    g.setGradientFill (juce::ColourGradient (rimLight, 0.0f, r.getY(), rimDark, 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, pedalCorner);
    paintMatte (g, face, pedalCorner - 5.0f, faceTop, faceBottom, 7, 1400);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (face.expanded (0.5f), pedalCorner - 4.5f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.drawRoundedRectangle (r.reduced (1.0f), pedalCorner - 1.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r.reduced (0.5f), pedalCorner, 1.0f);

    // The name: a crescent and NIGHTSHIFT spaced out, over BASS DRIVE in heavy capitals between two amber rules
    {
        auto logo = logoArea.toFloat();
        const auto brandFont = font (11.5f, true).withExtraKerningFactor (0.5f);
        const auto brandRow = logo.removeFromTop (16.0f);
        g.setColour (silkscreen.withAlpha (0.85f));
        g.setFont (brandFont);
        g.drawText ("NIGHTSHIFT", brandRow, juce::Justification::centred, false);

        // The crescent just ahead of the word: a disc with an offset disc taken out of it
        const float brandWidth = juce::GlyphArrangement::getStringWidth (brandFont, "NIGHTSHIFT");
        const auto moon = juce::Rectangle<float> (11.0f, 11.0f).withCentre ({ brandRow.getCentreX() - brandWidth * 0.5f - 12.0f, brandRow.getCentreY() });
        juce::Path crescent, disc;
        disc.addEllipse (moon);
        crescent.addEllipse (moon);
        crescent.addEllipse (moon.reduced (0.6f).translated (3.4f, -1.6f));
        crescent.setUsingNonZeroWinding (false);
        g.saveState();
        g.reduceClipRegion (disc);
        g.setColour (lampAmber);
        g.fillPath (crescent);
        g.restoreState();

        logo.removeFromTop (4.0f);
        const auto nameFont = displayFont (28.0f).withExtraKerningFactor (0.05f);
        const float nameWidth = juce::GlyphArrangement::getStringWidth (nameFont, "BASS DRIVE");
        g.setColour (silkscreen);
        g.setFont (nameFont);
        g.drawText ("BASS DRIVE", logo, juce::Justification::centred, false);

        const float y = logo.getCentreY() - 0.75f, inset = 30.0f, gap = 14.0f;
        g.setColour (lampAmber.withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (face.getX() + inset, y, face.getCentreX() - nameWidth * 0.5f - gap, y + 1.5f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (face.getCentreX() + nameWidth * 0.5f + gap, y, face.getRight() - inset, y + 1.5f));
    }

    // Jack legends at the sides, arrows the way the signal runs: in at the right, out at the left
    {
        const auto legendFont = font (11.5f, true).withExtraKerningFactor (0.16f);
        g.setFont (legendFont);
        g.setColour (silkscreen.withAlpha (0.85f));

        auto arrow = [&g] (float tipX, float y)
        {
            juce::Path p;
            p.addTriangle (tipX, y, tipX + 6.0f, y - 4.0f, tipX + 6.0f, y + 4.0f);
            g.fillPath (p);
        };

        const float leftX = face.getX() + 14.0f, rightX = face.getRight() - 14.0f;
        arrow (leftX, jackY);
        g.drawText ("OUT", juce::Rectangle<float> (leftX + 11.0f, jackY - 8.0f, 60.0f, 16.0f), juce::Justification::centredLeft, false);
        g.drawText ("IN", juce::Rectangle<float> (rightX - 60.0f, jackY - 8.0f, 60.0f, 16.0f), juce::Justification::centredRight, false);
        arrow (rightX - juce::GlyphArrangement::getStringWidth (legendFont, "IN") - 11.0f, jackY);
    }

    // The LED in its bezel: lit while engaged, brighter with how hard the drive is working
    const bool engaged = footswitch.isEngaged();
    const float glow = engaged ? 0.55f + 0.45f * ledGlow : 0.0f;

    if (glow > 0.0f)
    {
        g.setGradientFill (juce::ColourGradient (lampAmber.withAlpha (0.5f * glow), ledArea.getCentreX(), ledArea.getCentreY(),
                                                 lampAmber.withAlpha (0.0f), ledArea.getCentreX() + 17.0f, ledArea.getCentreY(), true));
        g.fillEllipse (ledArea.expanded (13.0f));
    }

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc4c9d1), 0.0f, ledArea.getY() - 3.0f, juce::Colour (0xff3a3f48), 0.0f, ledArea.getBottom() + 3.0f, false));
    g.fillEllipse (ledArea.expanded (3.5f));
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillEllipse (ledArea.expanded (1.2f));
    g.setColour (engaged ? lampAmber.interpolatedWith (juce::Colour (0xffffe3bd), 0.45f * ledGlow) : lampAmber.withAlpha (0.16f));
    g.fillEllipse (ledArea);
    g.setColour (juce::Colours::white.withAlpha (engaged ? 0.6f : 0.15f));
    g.fillEllipse (ledArea.reduced (3.2f).translated (-1.4f, -1.4f));
}

void DriveEditor::paintReduction (juce::Graphics& g, juce::Rectangle<float> r)
{
    // GR: a vertical LED column 0..12 dB filling downwards, then the gate lamp under it
    auto lamp = r.removeFromBottom (24.0f);
    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText ("GR", r.removeFromTop (14.0f), juce::Justification::centred, true);

    auto column = r.withSizeKeepingCentre (14.0f, r.getHeight() - 4.0f);
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (column, 3.0f);

    constexpr int segments = 12;
    const float segH = (column.getHeight() - 4.0f) / segments;

    for (int s = 0; s < segments; ++s)
    {
        const bool on = reductionShown >= (float) s + 0.5f;
        auto seg = juce::Rectangle<float> (column.getX() + 2.0f, column.getY() + 2.0f + s * segH, column.getWidth() - 4.0f, segH - 1.0f);
        g.setColour (on ? teal : teal.withAlpha (0.10f));
        g.fillRect (seg);
    }

    g.setColour (textDim);
    g.setFont (monoFont (12.0f));
    g.drawText (juce::String (reductionShown, 1), column.withY (column.getBottom() + 1.0f).withHeight (15.0f).expanded (14.0f, 0.0f),
                juce::Justification::centred, true);

    const bool gateOn = processor.param ("gate") > -89.9f;
    auto led = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ lamp.getX() + 10.0f, lamp.getCentreY() + 4.0f });
    const auto colour = gateOn ? (gateShown > 0.5f ? ledOn : danger) : textDim;
    g.setColour (colour.withAlpha (gateOn ? 1.0f : 0.25f));
    g.fillEllipse (led);
    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (12.0f, true));
    g.drawText ("GATE", lamp.withTrimmedLeft (18.0f).withTrimmedTop (8.0f), juce::Justification::centredLeft, true);
}

//==============================================================================
void DriveEditor::animate (float seconds)
{
    juce::ignoreUnused (seconds);
    cabSelector.refresh();

    // LED: follows the drive path's level, fast up, slower down
    const float activity = juce::jlimit (0.0f, 1.0f, drive.getDriveActivity() * 3.0f);
    ledGlow += (activity - ledGlow) * (activity > ledGlow ? 0.6f : 0.15f);
    repaint (ledArea.expanded (20.0f).getSmallestIntegerContainer());

    const float gr = juce::jlimit (0.0f, 12.0f, drive.getCompReduction());
    const float gate = drive.getGateGain();

    if (std::abs (gr - reductionShown) > 0.05f || std::abs (gate - gateShown) > 0.05f)
    {
        reductionShown += (gr - reductionShown) * 0.5f;
        gateShown = gate;
        repaint (reductionMeter.expanded (14));
    }

    cabView.refresh();
}
