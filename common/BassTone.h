#pragma once

#include <JuceHeader.h>
#include "BassDsp.h"

namespace bass
{

/** Six-band graphic EQ, the same bands on every Nightshift Bass plugin. Peaking bands,
    Q 1.1 (about 1.3 octaves, so neighbours overlap like a graphic's), +-12 dB, gains
    smoothed over 30 ms. A band at 0 dB with settled state is skipped, which is exact. */
class GraphicEq
{
public:
    static constexpr int numBands = 6;
    static constexpr float frequencies[numBands] { 60.0f, 160.0f, 400.0f, 1000.0f, 2500.0f, 6000.0f };
    static constexpr double q = 1.1;

    static juce::String bandId (int band)     { return "eq" + juce::String (band + 1); }
    static juce::String bandLabel (int band);  // "60", "160", "1k", "2.5k"...

    void prepare (double sampleRate);
    void reset();
    void setGainDb (int band, float db)       { gains[band].setTargetValue (juce::jlimit (-12.0f, 12.0f, db)); }
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

private:
    double sr = 48000.0;
    dsp::Smoothed gains[numBands];
    dsp::Coeffs coeffs[numBands];
    dsp::Biquad state[2][numBands];
    float coeffGain[numBands] {};
};

//==============================================================================
/** The cabinet: four built-in voicings (filter models of common bass cab shapes, zero
    latency) or an impulse response file the player loads. All choices crossfade over
    50 ms; a voicing that is silent and settled is not run, and starts from a clean state
    when it fades back in. */
class Cabinet
{
public:
    static juce::StringArray choiceNames()    { return { "Off", "4x10", "8x10", "1x15", "2x12", "IR" }; }
    static constexpr int offChoice = 0, irChoice = 5, numVoicings = 4, numChoices = 6;

    void prepare (const juce::dsp::ProcessSpec&);
    void reset();
    void setChoice (int choice);
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Message thread. Reads the file, takes its first channel, trims silence and
        level-matches it to the voicings (0 dB average from 40 to 250 Hz), then hands it
        to the convolution, which swaps it in without blocking the audio thread. */
    bool loadImpulseResponse (const juce::File&, juce::String& error);
    void clearImpulseResponse();
    bool hasImpulseResponse() const           { return irReady.load(); }

    /** Magnitude in dB of voicing `v` at `hz`, for the editor's curve. */
    static float voicingResponseDb (int voicing, float hz, double sampleRate = 48000.0);

private:
    struct Peak { float hz, q, db; };

    struct Voicing
    {
        float highpassHz, highpassQ;
        Peak peaks[3];
        float lowpassHz;
        float trimDb;
    };

    static const Voicing voicings[numVoicings];
    static std::array<dsp::Coeffs, 6> design (const Voicing&, double sampleRate);

    double sr = 48000.0;
    int maxBlock = 512, requested = 1;
    std::array<dsp::Coeffs, 6> coeffs[numVoicings];
    dsp::Biquad state[numVoicings][2][6];
    float trim[numVoicings] {};
    dsp::Smoothed weights[numChoices];
    bool running[numChoices] {};

    juce::dsp::Convolution convolution;
    juce::AudioBuffer<float> irOut;
    std::atomic<bool> irReady { false };
};

} // namespace bass
