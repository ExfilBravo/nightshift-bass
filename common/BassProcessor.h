#pragma once

#include <JuceHeader.h>
#include "BassTone.h"

namespace bass
{

/** The shape every Nightshift Bass plugin shares: a parameter tree, a bypass switch,
    dry/wet mix with latency compensation, an output trim, peak meters for the editor,
    and versioned state. Subclasses add their parameters and implement the wet path,
    ending it with processTone() (the shared graphic EQ and cabinet).
*/
class BassProcessor : public juce::AudioProcessor
{
public:
    struct Info
    {
        juce::String name;        // "Nightshift Cassette"
        juce::String tagline;     // "tape"
        juce::Colour accent;
    };

    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    BassProcessor (Info, Layout&& parameters);
    ~BassProcessor() override = default;

    //==============================================================================
    const Info& getInfo() const                        { return info; }
    juce::AudioProcessorValueTreeState& getState()     { return apvts; }

    /** Current value of a parameter, in its own units (dB, %, Hz...). Lock-free. */
    float param (const juce::String& id) const;

    /** Peak since the last call, as gain, for the editor's meters. */
    float getInputPeak (int channel)                   { return inputPeak[channel & 1].exchange (0.0f); }
    float getOutputPeak (int channel)                  { return outputPeak[channel & 1].exchange (0.0f); }

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override                   {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorParameter* getBypassParameter() const override    { return bypassParam; }
    bool hasEditor() const override                    { return true; }
    const juce::String getName() const override        { return info.name; }
    bool acceptsMidi() const override                  { return false; }
    bool producesMidi() const override                 { return false; }
    bool isMidiEffect() const override                 { return false; }
    double getTailLengthSeconds() const override       { return tailSeconds; }

    int getNumPrograms() override                      { return 1; }
    int getCurrentProgram() override                   { return 0; }
    void setCurrentProgram (int) override              {}
    const juce::String getProgramName (int) override   { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    /** The parameters every plugin gets: mix, output, bypass. Call last in your layout. */
    static void addCommonParameters (Layout&);

    /** The graphic EQ bands (eq1..eq6) and the cabinet choice (cab). */
    static void addToneParameters (Layout&);

    //==============================================================================
    /** Message thread. Loads a cab impulse response, remembers its path in the state and
        switches the cabinet to IR. */
    bool loadImpulseResponse (const juce::File&, juce::String& error);
    void clearImpulseResponse();

    /** The loaded IR's file name, "" for none; `missing` when the saved file is gone. */
    juce::String getImpulseResponseName (bool* missing = nullptr) const;

    static juce::String percentText (float v, int);
    static juce::String dbText (float v, int);
    static juce::String hzText (float v, int);
    static juce::String msText (float v, int);

protected:
    /** Allocate and reset everything here. The spec is the host's layout, 1 or 2 channels. */
    virtual void prepareDsp (const juce::dsp::ProcessSpec&) = 0;

    /** Process the wet path in place. The buffer has the channel count from prepareDsp. */
    virtual void processDsp (juce::AudioBuffer<float>&) = 0;

    /** Samples of delay the wet path adds; called after prepareDsp. */
    virtual int getDspLatency() const                  { return 0; }

    /** Return the wet path to silence: clear delay lines, oversamplers and filter states,
        and snap smoothers to their targets. Called on the audio thread when the plugin
        comes out of bypass, so it must not allocate; without it the first moments after
        un-bypass replay whatever was in the delay lines when bypass was engaged. */
    virtual void resetDsp()                            {}

    void setTailSeconds (double s)                     { tailSeconds = s; }

    /** The shared end of the chain: graphic EQ, then cabinet. Prepare and reset them from
        prepareDsp / resetDsp; run processTone on each chunk (at most the prepared block
        size) after the plugin's own processing. */
    void prepareTone (const juce::dsp::ProcessSpec&);
    void resetTone();
    void processTone (float* const* channels, int numChannels, int numSamples);

    /** Gain for the sound a plugin makes up itself (hiss, surface static, crackle) at a
        sample of the block being processed: 1 while the host's transport plays, gliding to
        0 over a tenth of a second when it stops and back when it starts, so the noise
        starts and stops with the song. 1 throughout where the host reports no transport. */
    float transportGateAt (int sample) const noexcept
    {
        return gateFrom + (gateTo - gateFrom) * (float) (sample + 1) / (float) std::max (1, gateBlockLength);
    }

    juce::AudioProcessorValueTreeState apvts;

private:
    /** Bypass fades the wet path out over the mixer's 50 ms ramp before it stops
        processing, and back in from a reset on return. While bypassed the input is
        passed through delayed by the reported latency, so host compensation holds. */
    enum class BypassState { active, fadingOut, bypassed };

    static constexpr int maxLatencySamples = 16384;

    Info info;
    juce::dsp::DryWetMixer<float> mixer { maxLatencySamples };
    juce::SmoothedValue<float> outputGain, inputGain;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* inputParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;
    std::atomic<float> inputPeak[2] { 0.0f, 0.0f }, outputPeak[2] { 0.0f, 0.0f };
    double tailSeconds = 0.0, currentSampleRate = 48000.0;
    BypassState bypassState = BypassState::active;
    int fadeRemaining = 0;
    bool primed = false;

    juce::SmoothedValue<float> transportGate;
    float gateFrom = 1.0f, gateTo = 1.0f;
    int gateBlockLength = 1;
    bool gatePrimed = false;

    GraphicEq eq;
    Cabinet cabinet;
    std::atomic<float>* eqParams[GraphicEq::numBands] {};
    std::atomic<float>* cabParam = nullptr;
    std::atomic<bool> irMissing { false };
    void restoreImpulseResponse();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassProcessor)
};

} // namespace bass
