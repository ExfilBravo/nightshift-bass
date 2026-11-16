#include "BassWidgets.h"
#include "BassProcessor.h"

namespace bass
{

using namespace theme;

namespace
{
    constexpr float keyH = 22.0f, keyGap = 4.0f, ledRow = 10.0f;

    float labelWidth (const juce::String& s)
    {
        return juce::GlyphArrangement::getStringWidth (font (12.0f, true), s);
    }
}

//==============================================================================
KeySwitch::KeySwitch (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& c,
                      juce::Colour l, juce::StringArray keyLabels)
    : parameter (*state.getParameter (paramId)),
      attachment (parameter, [this] (float v) { current = juce::roundToInt (v); repaint(); }, state.undoManager),
      caption (c), labels (std::move (keyLabels)), led (l)
{
    if (labels.isEmpty())
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&parameter))
            labels = choice->choices;

    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

float KeySwitch::keyWidth() const
{
    float w = 24.0f;

    for (auto& s : labels)
        w = std::max (w, labelWidth (s.toUpperCase()) + 12.0f);

    const int n = std::max (1, labels.size());
    const float available = (float) getWidth() - 10.0f - keyGap * (float) (n - 1);
    return std::min (w, available / (float) n);
}

int KeySwitch::getIdealWidth() const
{
    float w = 24.0f;

    for (auto& s : labels)
        w = std::max (w, labelWidth (s.toUpperCase()) + 12.0f);

    const int n = std::max (1, labels.size());
    return (int) std::ceil (w * (float) n + keyGap * (float) (n - 1) + 10.0f);
}

juce::Rectangle<float> KeySwitch::keyBounds (int index) const
{
    auto area = getLocalBounds().toFloat();

    if (caption.isNotEmpty())
        area.removeFromTop (16.0f);

    const int n = std::max (1, labels.size());
    const float w = keyWidth();
    const float x0 = area.getCentreX() - (w * (float) n + keyGap * (float) (n - 1)) * 0.5f;
    const float y0 = area.getCentreY() - (ledRow + keyH) * 0.5f + ledRow;
    return { x0 + (float) index * (w + keyGap), y0, w, keyH };
}

int KeySwitch::keyAt (juce::Point<float> p) const
{
    int best = 0;

    for (int i = 1; i < labels.size(); ++i)
        if (std::abs (p.x - keyBounds (i).getCentreX()) < std::abs (p.x - keyBounds (best).getCentreX()))
            best = i;

    return best;
}

void KeySwitch::mouseDown (const juce::MouseEvent& e)
{
    const int k = keyAt (e.position);

    if (k != current)
        attachment.setValueAsCompleteGesture ((float) k);
}

void KeySwitch::mouseDoubleClick (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (parameter.convertFrom0to1 (parameter.getDefaultValue()));
}

void KeySwitch::paint (juce::Graphics& g)
{
    if (caption.isNotEmpty())
    {
        g.setColour (textDim.brighter (0.15f));
        g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
        g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop (16), juce::Justification::centred, true);
    }

    if (labels.isEmpty())
        return;

    // A recessed plate holding the keys and their LEDs
    auto plate = keyBounds (0).getUnion (keyBounds (labels.size() - 1)).withTop (keyBounds (0).getY() - ledRow - 2.0f).expanded (4.0f, 4.0f);
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (plate, 5.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawRoundedRectangle (plate.translated (0.0f, 1.0f), 5.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (plate, 5.0f, 1.0f);

    for (int i = 0; i < labels.size(); ++i)
    {
        const bool on = i == current;
        auto key = keyBounds (i);
        auto lamp = juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ key.getCentreX(), key.getY() - 6.0f });

        if (on)
        {
            g.setColour (led.withAlpha (0.3f));
            g.fillEllipse (lamp.expanded (2.5f));
        }

        g.setColour (on ? led : led.withAlpha (0.14f));
        g.fillEllipse (lamp);

        // Raised when off, pressed in (darker, one pixel down) when latched
        if (on)
            key.translate (0.0f, 1.0f);

        g.setGradientFill (juce::ColourGradient (on ? juce::Colour (0xff2c323e) : juce::Colour (0xff434b5b), key.getCentreX(), key.getY(),
                                                 on ? juce::Colour (0xff1a1e26) : juce::Colour (0xff252a34), key.getCentreX(), key.getBottom(), false));
        g.fillRoundedRectangle (key, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawRoundedRectangle (key, 3.0f, 1.0f);

        if (! on)
        {
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.drawHorizontalLine ((int) key.getY() + 1, key.getX() + 3.0f, key.getRight() - 3.0f);
        }

        g.setColour (on ? led.brighter (0.3f) : textDim.brighter (0.35f));
        g.setFont (font (12.0f, true));
        g.drawText (labels[i].toUpperCase(), key, juce::Justification::centred, true);
    }
}

//==============================================================================
namespace
{
    constexpr float leverColumn = 24.0f, labelGap = 12.0f;     // the lever's column, then a printed tick to the labels

    juce::Font toggleCaptionFont()      { return font (11.5f, true).withExtraKerningFactor (0.16f); }
    juce::Font toggleLabelFont()        { return font (12.0f, true); }
}

MiniToggle::MiniToggle (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& c,
                        juce::Colour print, juce::Colour litColour)
    : parameter (*state.getParameter (paramId)),
      attachment (parameter, [this] (float v) { current = juce::roundToInt (v); repaint(); }, state.undoManager),
      caption (c), silkscreen (print), lit (litColour)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&parameter))
        labels = choice->choices;

    jassert (labels.size() == 3);   // a three-way switch
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

float MiniToggle::getLabelsWidth() const
{
    float widest = 0.0f;

    for (auto& s : labels)
        widest = std::max (widest, juce::GlyphArrangement::getStringWidth (toggleLabelFont(), s.toUpperCase()));

    return widest;
}

int MiniToggle::getIdealWidth() const
{
    const float captionWidth = juce::GlyphArrangement::getStringWidth (toggleCaptionFont(), caption.toUpperCase());
    return (int) std::ceil (std::max (captionWidth, leverColumn + labelGap + std::max (getLabelsWidth(), labelsRoom)) + 8.0f);
}

float MiniToggle::blockX() const
{
    return std::floor (((float) getWidth() - (leverColumn + labelGap + std::max (getLabelsWidth(), labelsRoom))) * 0.5f);
}

float MiniToggle::rowCentre (int row) const
{
    return (float) (captionHeight + 2) + ((float) row + 0.5f) * (float) rowHeight;
}

int MiniToggle::choiceAt (float y) const
{
    const int n = std::max (1, labels.size());
    const int row = juce::jlimit (0, n - 1, (int) std::floor ((y - (float) (captionHeight + 2)) / (float) rowHeight));
    return n - 1 - row;
}

void MiniToggle::select (int choice)
{
    choice = juce::jlimit (0, std::max (0, labels.size() - 1), choice);

    if (choice != current)
        attachment.setValueAsCompleteGesture ((float) choice);
}

void MiniToggle::mouseDown (const juce::MouseEvent& e)
{
    // The caption is only a name: a click on it changes nothing (a drag from it still moves the lever)
    if (e.position.y >= (float) captionHeight)
        select (choiceAt (e.position.y));
}

void MiniToggle::mouseDrag (const juce::MouseEvent& e)
{
    select (choiceAt (e.position.y));
}

void MiniToggle::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    // Up throws the lever up, towards the last choice: a step a wheel notch, however finely a
    // trackpad slices its scrolling
    wheelTravel += wheel.isReversed ? -wheel.deltaY : wheel.deltaY;

    if (std::abs (wheelTravel) >= 0.1f)
    {
        select (current + (wheelTravel > 0.0f ? 1 : -1));
        wheelTravel = 0.0f;
    }
}

void MiniToggle::mouseDoubleClick (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (parameter.convertFrom0to1 (parameter.getDefaultValue()));
}

void MiniToggle::paint (juce::Graphics& g)
{
    const int n = labels.size();

    if (caption.isNotEmpty())
    {
        g.setColour (silkscreen);
        g.setFont (toggleCaptionFont());
        g.drawText (caption.toUpperCase(), getLocalBounds().removeFromTop (captionHeight), juce::Justification::centred, false);
    }

    if (n == 0)
        return;

    const float x0 = blockX();
    const float cx = x0 + leverColumn * 0.5f;
    const float labelX = x0 + leverColumn + labelGap;
    const int row = n - 1 - current;                     // the lit row
    const float cy = rowCentre (1);                      // the bushing sits level with the middle choice

    // Printed ticks from the lever to each choice, and the choices
    g.setFont (toggleLabelFont());

    for (int r = 0; r < n; ++r)
    {
        const bool on = r == row;
        const float y = rowCentre (r);
        g.setColour (on ? lit : silkscreen.withAlpha (0.45f));
        g.fillRect (juce::Rectangle<float> (cx + 13.0f, y - 0.6f, labelX - cx - 17.0f, 1.2f));

        g.setColour (on ? lit : silkscreen.withAlpha (0.72f));
        g.drawText (labels[n - 1 - r].toUpperCase(), juce::Rectangle<float> (labelX, y - (float) rowHeight * 0.5f, (float) getWidth() - labelX, (float) rowHeight),
                    juce::Justification::centredLeft, false);
    }

    // The hex nut and the threaded bushing it holds
    juce::Path nut;

    for (int i = 0; i < 6; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * (float) i / 6.0f;
        const juce::Point<float> p (cx + 10.5f * std::cos (a), cy + 10.5f * std::sin (a));
        i == 0 ? nut.startNewSubPath (p) : nut.lineTo (p);
    }

    nut.closeSubPath();
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillPath (nut, juce::AffineTransform::translation (0.0f, 2.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc9ced6), cx, cy - 10.0f, juce::Colour (0xff4f545e), cx, cy + 10.0f, false));
    g.fillPath (nut);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.strokePath (nut, juce::PathStrokeType (1.0f));

    const auto bushing = juce::Rectangle<float> (13.0f, 13.0f).withCentre ({ cx, cy });
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff23262c), cx, bushing.getY(), juce::Colour (0xff6c727d), cx, bushing.getBottom(), false));
    g.fillEllipse (bushing);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawEllipse (bushing, 1.0f);

    // The bat: thrown up or down it lies along the face, a slim neck out to a rounded tip;
    // in the middle it points straight out, so only its tip shows
    auto chrome = [&] (juce::Rectangle<float> r)
    {
        return juce::ColourGradient (juce::Colour (0xfff5f7fa), r.getX(), r.getY(), juce::Colour (0xff7b818c), r.getRight(), r.getBottom(), false);
    };

    if (current == 1 || n != 3)
    {
        const auto tip = juce::Rectangle<float> (11.0f, 11.0f).withCentre ({ cx, cy });
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (tip.translated (1.5f, 2.5f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffbfcfe), tip.getX() + 3.0f, tip.getY() + 2.5f,
                                                 juce::Colour (0xff6d737e), tip.getRight(), tip.getBottom(), true));
        g.fillEllipse (tip);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawEllipse (tip, 0.8f);
    }
    else
    {
        const float dir = row == 0 ? -1.0f : 1.0f;
        const float tipY = rowCentre (row) + dir * -1.0f;

        juce::Path bat;
        bat.startNewSubPath (cx - 2.4f, cy);
        bat.lineTo (cx - 3.4f, tipY - dir * 4.0f);
        bat.lineTo (cx + 3.4f, tipY - dir * 4.0f);
        bat.lineTo (cx + 2.4f, cy);
        bat.closeSubPath();
        bat.addEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ cx, tipY }));

        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillPath (bat, juce::AffineTransform::translation (2.0f, 2.5f));
        g.setGradientFill (chrome (bat.getBounds()));
        g.fillPath (bat);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.strokePath (bat, juce::PathStrokeType (0.8f));
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ cx - 1.5f, tipY - 1.5f }));
    }
}

//==============================================================================
EqFader::EqFader (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& l, juce::Colour accent)
    : slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox), label (l)
{
    slider.setColour (juce::Slider::trackColourId, accent);
    slider.setWantsKeyboardFocus (false);
    slider.setMouseDragSensitivity (160);
    slider.setVelocityBasedMode (false);
    addAndMakeVisible (slider);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
    slider.setDoubleClickReturnValue (true, 0.0);
    slider.setTooltip (l + " Hz band: drag, or double-click for 0 dB");
}

void EqFader::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (16);
    r.removeFromBottom (16);
    slider.setBounds (r.withSizeKeepingCentre (std::min (r.getWidth(), 30), r.getHeight()));
}

void EqFader::paint (juce::Graphics& g)
{
    const float v = (float) slider.getValue();
    g.setColour (std::abs (v) < 0.05f ? textDim : text);
    g.setFont (monoFont (12.0f));
    g.drawText ((v > 0.05f ? "+" : "") + juce::String (v, std::abs (v) < 9.95f ? 1 : 0), getLocalBounds().removeFromTop (16),
                juce::Justification::centred, true);

    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (12.0f, true));
    g.drawText (label, getLocalBounds().removeFromBottom (16), juce::Justification::centred, true);
}

//==============================================================================
GraphicEqPanel::GraphicEqPanel (juce::AudioProcessorValueTreeState& s, juce::Colour a)
    : state (s), accent (a)
{
    for (int b = 0; b < GraphicEq::numBands; ++b)
    {
        auto* f = faders.add (new EqFader (state, GraphicEq::bandId (b), GraphicEq::bandLabel (b), accent));
        f->getSlider().onValueChange = [this, f] { f->repaint(); repaint(); };
        addAndMakeVisible (f);
    }
}

void GraphicEqPanel::resized()
{
    strip = getLocalBounds();
    auto r = strip.reduced (26, 0).withTrimmedRight (-10);
    const int w = r.getWidth() / GraphicEq::numBands;

    for (int b = 0; b < GraphicEq::numBands; ++b)
        faders[b]->setBounds (r.getX() + b * w, r.getY(), w, r.getHeight());
}

float GraphicEqPanel::responseDb (float hz) const
{
    constexpr double sr = 48000.0;
    const double w = juce::MathConstants<double>::twoPi * hz / sr;
    double db = 0.0;

    for (int b = 0; b < GraphicEq::numBands; ++b)
    {
        const float gain = state.getRawParameterValue (GraphicEq::bandId (b))->load();

        if (gain == 0.0f)
            continue;

        const auto c = dsp::Coeffs::peak (GraphicEq::frequencies[b], GraphicEq::q, gain, sr);
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        db += juce::Decibels::gainToDecibels (std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2)), -120.0);
    }

    return (float) db;
}

void GraphicEqPanel::paint (juce::Graphics& g)
{
    // The scale: +12, 0, -12 lines across the faders' travel
    const auto track = faders[0]->getTrack() + faders[0]->getPosition();
    const float top = (float) track.getY(), bottom = (float) track.getBottom();
    auto yFor = [&] (float db) { return juce::jmap (db, 12.0f, -12.0f, top, bottom); };

    const float left = (float) faders.getFirst()->getX(), right = (float) faders.getLast()->getRight();

    for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f })
    {
        g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.10f : 0.045f));
        g.drawHorizontalLine (juce::roundToInt (yFor (db)), left + 4.0f, right - 4.0f);
    }

    g.setColour (textDim);
    g.setFont (monoFont (12.0f));

    for (float db : { 12.0f, 0.0f, -12.0f })
        g.drawText (db > 0.0f ? "+12" : (db < 0.0f ? "-12" : "0"),
                    juce::Rectangle<float> (left - 26.0f, yFor (db) - 6.0f, 22.0f, 12.0f), juce::Justification::centredRight, true);

    // The response through the bands: log frequency, one fader per band centre
    std::array<float, GraphicEq::numBands> centres {};

    for (int b = 0; b < GraphicEq::numBands; ++b)
        centres[(size_t) b] = (float) (faders[b]->getBounds().getCentreX());

    auto hzAt = [&] (float x)
    {
        const float spacing = centres[1] - centres[0];
        const float pos = (x - centres[0]) / spacing;                       // in band steps
        const int k = juce::jlimit (0, GraphicEq::numBands - 2, (int) std::floor (pos));
        const float t = pos - (float) k;
        const float l0 = std::log2 (GraphicEq::frequencies[k]), l1 = std::log2 (GraphicEq::frequencies[k + 1]);
        return std::exp2 (l0 + (l1 - l0) * t);
    };

    juce::Path curve;
    juce::Path fill;
    const float zeroY = yFor (0.0f);

    for (float x = left; x <= right; x += 2.0f)
    {
        const float y = juce::jlimit (top - 4.0f, bottom + 4.0f, yFor (responseDb (hzAt (x))));

        if (x == left)
        {
            curve.startNewSubPath (x, y);
            fill.startNewSubPath (x, zeroY);
        }
        else
        {
            curve.lineTo (x, y);
        }

        fill.lineTo (x, y);
    }

    fill.lineTo (right, zeroY);
    fill.closeSubPath();
    g.setColour (accent.withAlpha (0.10f));
    g.fillPath (fill);
    g.setColour (accent.withAlpha (0.55f));
    g.strokePath (curve, juce::PathStrokeType (1.5f));
}

void GraphicEqPanel::paintOverChildren (juce::Graphics&) {}

//==============================================================================
CabSelector::CabSelector (BassProcessor& p, juce::Colour a)
    : processor (p),
      keys (p.getState(), "cab", {}, a, { "Off", "4x10", "8x10", "1x15", "2x12", "IR" }),
      accent (a)
{
    keys.setTooltip ("Cabinet: four built-in voicings, or IR to use the impulse response file you load");
    addAndMakeVisible (keys);

    loadButton.setTooltip ("Load a cab impulse response (WAV/AIFF), level-matched to the built-in cabs");
    loadButton.onClick = [this] { chooseFile(); };
    addAndMakeVisible (loadButton);
    refresh();
}

void CabSelector::resized()
{
    auto r = getLocalBounds();
    keys.setBounds (r.removeFromTop (keysHeight).withWidth (std::min (r.getWidth(), keys.getIdealWidth())));
    r.removeFromTop (6);
    auto row = r.removeFromTop (24);
    loadButton.setBounds (row.removeFromLeft (78).withX (keys.getX() + 4));
}

void CabSelector::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().withTrimmedTop (keysHeight + 6).removeFromTop (24).withTrimmedLeft (keys.getX() + 90);
    g.setFont (monoFont (12.0f));

    if (status.isNotEmpty())
    {
        g.setColour (danger);
        g.drawText (status, r, juce::Justification::centredLeft, true);
    }
    else if (irName.isNotEmpty())
    {
        g.setColour (irMissing ? danger : text);
        g.drawText (irMissing ? irName + " (missing)" : irName, r, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (textDim);
        g.drawText ("no IR loaded", r, juce::Justification::centredLeft, true);
    }
}

void CabSelector::refresh()
{
    bool missing = false;
    const auto name = processor.getImpulseResponseName (&missing);

    if (name != irName || missing != irMissing)
    {
        irName = name;
        irMissing = missing;
        repaint();
    }
}

void CabSelector::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a cab impulse response", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (! file.existsAsFile())
                                  return;

                              juce::String error;
                              status = processor.loadImpulseResponse (file, error) ? juce::String() : error;
                              refresh();
                              repaint();
                          });
}

//==============================================================================
CabCurve::CabCurve (BassProcessor& p, juce::Colour a) : processor (p), accent (a)
{
    setInterceptsMouseClicks (false, false);
}

void CabCurve::refresh()
{
    const int cab = juce::roundToInt (processor.param ("cab"));
    const auto ir = processor.getImpulseResponseName();

    if (cab != shownCab || ir != shownIr)
    {
        shownCab = cab;
        shownIr = ir;
        repaint();
    }
}

void CabCurve::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

    auto plot = r.reduced (10.0f, 6.0f);
    auto captionRow = plot.removeFromTop (14.0f);
    plot.removeFromBottom (12.0f);

    constexpr float loHz = 30.0f, hiHz = 12000.0f, topDb = 8.0f, bottomDb = -32.0f;
    auto xFor = [&] (float hz) { return plot.getX() + plot.getWidth() * std::log (hz / loHz) / std::log (hiHz / loHz); };
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (bottomDb, topDb, db), topDb, bottomDb, plot.getY(), plot.getBottom()); };

    g.setColour (juce::Colours::white.withAlpha (0.05f));

    for (float hz : { 100.0f, 1000.0f, 10000.0f })
        g.drawVerticalLine (juce::roundToInt (xFor (hz)), plot.getY(), plot.getBottom());

    g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), plot.getX(), plot.getRight());

    g.setColour (textDim);
    g.setFont (monoFont (12.0f));

    for (float hz : { 100.0f, 1000.0f, 10000.0f })
        g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz),
                    juce::Rectangle<float> (xFor (hz) - 15.0f, plot.getBottom() + 1.0f, 30.0f, 11.0f), juce::Justification::centred, true);

    const int cab = juce::roundToInt (processor.param ("cab"));
    juce::String caption;

    if (cab == Cabinet::offChoice)
    {
        caption = "OFF: straight DI";
        g.setColour (accent.withAlpha (0.6f));
        g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), plot.getX(), plot.getRight());
    }
    else if (cab == Cabinet::irChoice)
    {
        bool missing = false;
        const auto name = processor.getImpulseResponseName (&missing);
        caption = name.isEmpty() ? "IR: load a file (plays as Off until then)" : (missing ? "IR missing: plays as Off" : "IR: " + name);
    }
    else
    {
        juce::Path curve;

        for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
        {
            const float hz = loHz * std::pow (hiHz / loHz, (x - plot.getX()) / plot.getWidth());
            const float y = yFor (Cabinet::voicingResponseDb (cab - 1, hz));
            x == plot.getX() ? curve.startNewSubPath (x, y) : curve.lineTo (x, y);
        }

        g.setColour (accent.withAlpha (0.85f));
        g.strokePath (curve, juce::PathStrokeType (1.6f));
        caption = Cabinet::choiceNames()[cab].toUpperCase() + " CABINET";
    }

    g.setColour (textDim.brighter (0.2f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText (caption, captionRow, juce::Justification::centredLeft, true);
}

//==============================================================================
Footswitch::Footswitch (juce::AudioProcessorValueTreeState& state, const juce::String& bypassId)
    : parameter (*state.getParameter (bypassId)),
      attachment (parameter, [this] (float v) { bypassed = v >= 0.5f; repaint(); }, state.undoManager)
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Footswitch: bypass on and off");
}

void Footswitch::mouseDown (const juce::MouseEvent&)
{
    pressed = true;
    attachment.setValueAsCompleteGesture (bypassed ? 0.0f : 1.0f);
    repaint();
}

void Footswitch::mouseUp (const juce::MouseEvent&)
{
    pressed = false;
    repaint();
}

void Footswitch::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const float size = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();

    auto disc = [] (float radius, juce::Point<float> at) { return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (at); };
    auto hex = [c] (float radius)
    {
        juce::Path p;

        for (int i = 0; i < 6; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / 6.0f + juce::MathConstants<float>::pi / 6.0f;
            const juce::Point<float> at (c.x + radius * std::cos (a), c.y + radius * std::sin (a));
            i == 0 ? p.startNewSubPath (at) : p.lineTo (at);
        }

        p.closeSubPath();
        return p;
    };

    // Its shadow, and the washer it's mounted with
    const float washerR = size * 0.49f;
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (disc (washerR, c.translated (0.0f, 2.0f)));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7d838e), c.x, c.y - washerR, juce::Colour (0xff272a31), c.x, c.y + washerR, false));
    g.fillEllipse (disc (washerR, c));
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (disc (washerR, c).reduced (0.5f), 1.0f);

    // The hex nut, its chamfer catching the light
    const float nutR = size * 0.43f;
    const auto nut = hex (nutR);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc6cbd3), c.x - nutR * 0.4f, c.y - nutR, juce::Colour (0xff3d424b), c.x + nutR * 0.4f, c.y + nutR, false));
    g.fillPath (nut);
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.strokePath (nut, juce::PathStrokeType (1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.2f));
    g.strokePath (hex (nutR * 0.86f), juce::PathStrokeType (1.0f));

    // The threaded collar
    const float collarR = size * 0.29f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1f2227), c.x, c.y - collarR, juce::Colour (0xff5d636e), c.x, c.y + collarR, false));
    g.fillEllipse (disc (collarR, c));
    g.setColour (juce::Colours::black.withAlpha (0.5f));

    for (float k : { 0.92f, 0.82f })
        g.drawEllipse (disc (collarR * k, c), 0.8f);

    // The plunger's chrome cap, a touch lower and darker while pressed
    const float capR = size * (pressed ? 0.215f : 0.23f);
    const auto cap = disc (capR, c.translated (0.0f, pressed ? 1.0f : 0.0f));
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillEllipse (cap.translated (0.0f, 1.5f).expanded (1.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (pressed ? 0xffd9dde4 : 0xfffafbfd), cap.getX() + capR * 0.65f, cap.getY() + capR * 0.55f,
                                             juce::Colour (0xff59606b), cap.getRight(), cap.getBottom(), true));
    g.fillEllipse (cap);
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.drawEllipse (cap, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.75f));
    g.fillEllipse (juce::Rectangle<float> (capR * 0.55f, capR * 0.35f).withCentre (cap.getCentre().translated (-capR * 0.3f, -capR * 0.42f)));
}

} // namespace bass
