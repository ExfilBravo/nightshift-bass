#pragma once

#include <JuceHeader.h>
#include "common/BassCircuits.h"
#include "common/BassDsp.h"
#include "common/BassEditor.h"
#include "common/BassParams.h"
#include "common/BassProcessor.h"

/** Nightshift Bass Split: multiband bass drive. See SPEC.md.

    Chain: gate -> 4x oversampling -> three-way Linkwitz-Riley split (24 dB/octave; the
    low band gets the high crossover's allpass, so the bands sum flat) ->
      low:   clean, compressed (lowComp), lowLevel
      mid:   midDrive into the drive circuit (character: valve / op-amp + diodes / fuzz), midLevel
      high:  highDrive into its own circuit, highLevel
    -> sum -> back to 1x -> graphic EQ -> cabinet.
    Everything between the oversampler's filters runs on one clock, so the bands stay
    phase-coherent whatever the drive.
*/
class SplitProcessor : public bass::BassProcessor
{
public:
    SplitProcessor();
    juce::AudioProcessorEditor* createEditor() override;

    /** For the editor: each band's output level (peak, decaying), and the low band's
        compressor reduction in dB. */
    float getBandLevel (int band) const     { return bandLevel[juce::jlimit (0, 2, band)].load(); }
    float getCompReduction() const          { return compReduction.load(); }

protected:
    void prepareDsp (const juce::dsp::ProcessSpec&) override;
    void processDsp (juce::AudioBuffer<float>&) override;
    void resetDsp() override;
    int getDspLatency() const override      { return latency; }

private:
    static Layout createLayout();
    void updateTargets();
    void processChunk (float* const* channels, int numChannels, int numSamples);
    void updateCrossovers (float lowLog2, float highLog2);

    std::atomic<float>* pGate = nullptr;
    std::atomic<float>* pLowCross = nullptr;
    std::atomic<float>* pHighCross = nullptr;
    std::atomic<float>* pLowComp = nullptr;
    std::atomic<float>* pLowLevel = nullptr;
    std::atomic<float>* pMidDrive = nullptr;
    std::atomic<float>* pMidLevel = nullptr;
    std::atomic<float>* pHighDrive = nullptr;
    std::atomic<float>* pHighLevel = nullptr;
    std::atomic<float>* pCharacter = nullptr;

    double sampleRate = 48000.0, overRate = 192000.0;
    int maxBlock = 512, numChannels = 2, latency = 0;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    bass::dsp::Gate gate;
    bass::dsp::Compressor lowCompressor;
    bass::circuit::CircuitDrive midCircuit, highCircuit;
    bool channelsLinked = true;     // both channels in one state while the input is mono (see processChunk)
    bass::dsp::DcBlocker midDc[2], highDc[2];

    // Crossover: LR4 = two Butterworth sections per side
    bass::dsp::Coeffs lowLp, lowHp, highLp, highHp, highAp;
    struct ChannelState { bass::dsp::Biquad lowLp[2], lowHp[2], highLp[2], highHp[2], highAp; } xover[2];

    // Glide at the oversampled rate
    bass::dsp::Smoothed lowCrossLog2, highCrossLog2, lowGain, midGain, highGain;
    float builtLow = -1.0f, builtHigh = -1.0f;

    float env[3] {};
    std::atomic<float> bandLevel[3] {}, compReduction { 0.0f };
};

//==============================================================================
/** The band display with draggable crossovers across the top, beside Input (gate and
    character); the three band panels; then the graphic EQ, cabinet and Out. */
class SplitEditor : public bass::BassEditor
{
public:
    explicit SplitEditor (SplitProcessor&);

protected:
    void layoutBody (juce::Rectangle<int>) override;
    void paintBody (juce::Graphics&, juce::Rectangle<int>) override;
    void animate (float seconds) override;

private:
    /** The spectrum strip: three coloured bands on a log axis, live band levels, and two
        crossover handles to drag (double-click a handle to reset it). */
    class BandDisplay : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        explicit BandDisplay (SplitProcessor&);
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void push (const float* levels, float reductionDb);

    private:
        juce::Rectangle<float> plotArea() const;
        float xForHz (float hz) const;
        float hzForX (float x) const;
        int handleNear (float x) const;

        SplitProcessor& processor;
        juce::ParameterAttachment lowAttachment, highAttachment;
        float lowHz = 160.0f, highHz = 1600.0f;
        float levels[3] {}, reduction = 0.0f, drawnDb[3] {};
        int dragging = -1, hover = -1;
    };

    void paintPanel (juce::Graphics&, juce::Rectangle<int>, const juce::String& title, juce::Colour);

    SplitProcessor& split;
    BandDisplay display;
    bass::GraphicEqPanel eqPanel;
    bass::CabSelector cabSelector;
    bass::CabCurve cabView;

    juce::Rectangle<int> displayArea, inputPanel, lowPanel, midPanel, highPanel, eqArea, cabArea, outPanel;
    bass::Knob* gateKnob = nullptr;
    bass::KeySwitch* character = nullptr;
    std::vector<bass::Knob*> lowKnobs, midKnobs, highKnobs, outKnobs;
};
