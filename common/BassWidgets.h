#pragma once

#include <JuceHeader.h>
#include "BassTheme.h"
#include "BassTone.h"

namespace bass
{

class BassProcessor;

/** A choice parameter as a row of latching keys with an LED over each, as on hardware:
    click or drag across the keys, double-click for the default. An optional caption sits
    above in the same style as a Knob's. Labels default to the parameter's choices. */
class KeySwitch : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    KeySwitch (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& caption,
               juce::Colour led, juce::StringArray labels = {});

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent& e) override     { mouseDown (e); }
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** Width that fits every key's label without squeezing. */
    int getIdealWidth() const;
    static constexpr int idealHeight = 58;     // caption 16 + LED row 10 + key 24 + margins
    int getSelected() const                   { return current; }

private:
    juce::Rectangle<float> keyBounds (int index) const;
    int keyAt (juce::Point<float>) const;
    float keyWidth() const;

    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    juce::String caption;
    juce::StringArray labels;
    juce::Colour led;
    int current = 0;
};

//==============================================================================
/** A three-way choice parameter as a bat toggle on a pedal face. The lever points up, straight
    out or down; the choices are printed beside it, the last at the top, the chosen one lit.
    Click a choice (or level with it), drag up and down, double-click for the default. The
    caption is printed above. */
class MiniToggle : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    MiniToggle (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& caption,
                juce::Colour silkscreen, juce::Colour lit);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Width that fits the caption and the longest choice beside the lever. */
    int getIdealWidth() const;
    static constexpr int captionHeight = 16, rowHeight = 17, idealHeight = captionHeight + 2 + 3 * rowHeight + 1;
    int getSelected() const                   { return current; }

    /** The widest of this toggle's choices, and the room to keep for them: give a column of
        toggles the widest of theirs and their levers line up. */
    float getLabelsWidth() const;
    void setLabelsWidth (float width)         { labelsRoom = width; repaint(); }

private:
    float rowCentre (int row) const;          // row 0 is the top one: the last choice
    int choiceAt (float y) const;
    float blockX() const;                     // left of the lever and labels, centred
    void select (int choice);

    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    juce::String caption;
    juce::StringArray labels;
    juce::Colour silkscreen, lit;
    float labelsRoom = 0.0f, wheelTravel = 0.0f;
    int current = 0;
};

//==============================================================================
/** One band of the graphic EQ: a vertical fader with its dB value above and the band's
    frequency below. */
class EqFader : public juce::Component
{
public:
    EqFader (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label, juce::Colour accent);
    void resized() override;
    void paint (juce::Graphics&) override;
    juce::Slider& getSlider()                 { return slider; }

    /** The fader's travel, in this component's coordinates. */
    juce::Rectangle<int> getTrack() const     { return slider.getBounds().reduced (0, capHalf); }
    static constexpr int capHalf = 7;

private:
    juce::Slider slider;
    juce::String label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** The six faders on a recessed strip, with the EQ's actual response drawn through them. */
class GraphicEqPanel : public juce::Component
{
public:
    GraphicEqPanel (juce::AudioProcessorValueTreeState&, juce::Colour accent);
    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    float responseDb (float hz) const;

    juce::AudioProcessorValueTreeState& state;
    juce::Colour accent;
    juce::OwnedArray<EqFader> faders;
    juce::Rectangle<int> strip;
};

//==============================================================================
/** Cabinet picker: the six choices as keys, a LOAD IR button, and the loaded file's name. */
class CabSelector : public juce::Component
{
public:
    CabSelector (BassProcessor&, juce::Colour accent);
    void resized() override;
    void paint (juce::Graphics&) override;

    /** Call from the editor's timer: picks up an IR restored with a saved session. */
    void refresh();

    static constexpr int keysHeight = 44, idealHeight = keysHeight + 6 + 24;

private:
    void chooseFile();

    BassProcessor& processor;
    KeySwitch keys;
    juce::TextButton loadButton { "LOAD IR" };
    juce::String irName, status;
    bool irMissing = false;
    juce::Colour accent;
    std::unique_ptr<juce::FileChooser> chooser;
};

/** The selected cabinet's frequency response (or what the IR / Off choice means), on a
    recessed screen. Call refresh() from the editor's timer. */
class CabCurve : public juce::Component
{
public:
    CabCurve (BassProcessor&, juce::Colour accent);
    void paint (juce::Graphics&) override;
    void refresh();

private:
    BassProcessor& processor;
    juce::Colour accent;
    int shownCab = -1;
    juce::String shownIr;
};

//==============================================================================
/** A chrome stomp switch bound to the bypass parameter: press to toggle the effect. */
class Footswitch : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    Footswitch (juce::AudioProcessorValueTreeState&, const juce::String& bypassId);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool isEngaged() const                    { return ! bypassed; }

private:
    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    bool bypassed = false, pressed = false;
};

} // namespace bass
