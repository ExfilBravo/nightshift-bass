#pragma once

#include <JuceHeader.h>
#include "common/BassCircuits.h"
#include "common/BassDsp.h"
#include "common/BassEditor.h"
#include "common/BassParams.h"
#include "common/BassProcessor.h"

/** Nightshift Bass Drive: a bass overdrive with a clean blend. See SPEC.md.

    Chain: gate -> compressor -> split into
      clean:  delayed by the oversampler's latency, untouched
      drive:  drive gain -> grunt (how much low end reaches the clipper: highpass, plus a
              low shelf on Fat) -> attack (high shelf) -> 4x oversampled drive circuit
              (smooth: two valve stages / tight: op-amp and diode clippers / fuzz: starved
              transistor into germanium; level-calibrated) -> DC block -> tone (lowpass) -> level
    -> blend (clean to drive) -> graphic EQ -> cabinet.
    The clean and drive paths share one latency, so blending them never combs.
*/
class DriveProcessor : public bass::BassProcessor
{
public:
    DriveProcessor();
    juce::AudioProcessorEditor* createEditor() override;

    /** For the editor: the drive path's level after the clipper, the compressor's gain
        reduction in dB, and the gate's gain (1 open, 0 closed). */
    float getDriveActivity() const          { return driveActivity.load(); }
    float getCompReduction() const          { return compReduction.load(); }
    float getGateGain() const               { return gateGain.load(); }

protected:
    void prepareDsp (const juce::dsp::ProcessSpec&) override;
    void processDsp (juce::AudioBuffer<float>&) override;
    void resetDsp() override;
    int getDspLatency() const override      { return latency; }

private:
    static Layout createLayout();
    void updateTargets();
    void processChunk (float* const* channels, int numChannels, int numSamples);

    std::atomic<float>* pGate = nullptr;
    std::atomic<float>* pComp = nullptr;
    std::atomic<float>* pDrive = nullptr;
    std::atomic<float>* pGrunt = nullptr;
    std::atomic<float>* pAttack = nullptr;
    std::atomic<float>* pCharacter = nullptr;
    std::atomic<float>* pTone = nullptr;
    std::atomic<float>* pBlend = nullptr;
    std::atomic<float>* pLevel = nullptr;

    double sampleRate = 48000.0;
    int maxBlock = 512, numChannels = 2, latency = 0;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::AudioBuffer<float> cleanBuffer, driveBuffer;
    bass::dsp::Gate gate;
    bass::dsp::Compressor compressor;
    bass::dsp::IntDelay cleanDelay[2];
    bass::circuit::CircuitDrive circuit;
    bool channelsLinked = true;     // both channels' circuits in one state (see processChunk)
    bass::dsp::DcBlocker dcBlocker[2];

    // Drive-path filters, coefficients rebuilt only while their controls glide
    bass::dsp::Coeffs gruntHighpass, gruntShelf, attackShelf, toneLowpass, attackPostShelf;
    bass::dsp::Biquad gruntHighpassState[2], gruntShelfState[2], attackShelfState[2], toneLowpassState[2], attackPostState[2];
    bass::dsp::Smoothed gruntLog2Hz, gruntShelfDb, attackDb, attackPost, body, toneLog2Hz, blend, levelGain;
    float builtGrunt = -1.0f, builtShelf = -1000.0f, builtAttack = -1000.0f, builtAttackPost = -1000.0f, builtTone = -1.0f;

    float activityEnv = 0.0f;
    std::atomic<float> driveActivity { 0.0f }, compReduction { 0.0f }, gateGain { 1.0f };
};

//==============================================================================
/** On the left, a bass preamp pedal: Level and Blend over Drive and Tone, the Grunt, Attack
    and Character toggles between the two rows, the name, and the footswitch with its LED.
    On the right, the back panel: Dynamics (gate, comp, GR meter) and Out (mix, output), the
    graphic EQ, and the cabinet with its IR loader and response. */
class DriveEditor : public bass::BassEditor
{
public:
    explicit DriveEditor (DriveProcessor&);

protected:
    void layoutBody (juce::Rectangle<int>) override;
    void paintBody (juce::Graphics&, juce::Rectangle<int>) override;
    void animate (float seconds) override;

private:
    void paintPedal (juce::Graphics&, juce::Rectangle<float>);
    void paintBackPanel (juce::Graphics&, juce::Rectangle<float>);
    void paintSection (juce::Graphics&, juce::Rectangle<int>, const juce::String& title, juce::Colour);
    void paintReduction (juce::Graphics&, juce::Rectangle<float>);

    DriveProcessor& drive;
    bass::GraphicEqPanel eqPanel;
    bass::CabSelector cabSelector;
    bass::Footswitch footswitch;
    bass::CabCurve cabView;
    bass::MiniToggle grunt, attack, character;

    juce::Rectangle<int> pedal, backPanel, dynamicsPanel, outPanel, eqArea, cabArea, cabCurve, reductionMeter, logoArea;
    std::vector<bass::Knob*> pedalKnobs;
    bass::Knob* gateKnob = nullptr;
    bass::Knob* compKnob = nullptr;
    bass::Knob* mixKnob = nullptr;
    bass::Knob* outputKnob = nullptr;
    juce::Rectangle<float> ledArea;

    float ledGlow = 0.0f, reductionShown = 0.0f, gateShown = 1.0f;
};
