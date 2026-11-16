#include "Split.h"
#include "common/ScaledEditor.h"

using namespace bass;
namespace bdsp = bass::dsp;
using namespace bass::params;

namespace
{
    constexpr double butterworthQ = 0.70710678118654752;

    // The level each band typically peaks at in a bass DI: each band's circuit is
    // level-calibrated against it, so its drive changes grit rather than loudness
    constexpr float midReference = 0.15f, highReference = 0.06f;

    constexpr float offDb = -40.0f;

    juce::String gateText (float v, int)
    {
        return v <= -89.9f ? juce::String ("Off") : juce::String (juce::roundToInt (v)) + " dB";
    }

    juce::String levelText (float v, int)
    {
        return v <= offDb + 0.05f ? juce::String ("Off") : (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB";
    }

    juce::String crossText (float v, int)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
    }

    float levelGain (float db)      { return db <= offDb + 0.05f ? 0.0f : bdsp::dbToGain (db); }
}

//==============================================================================
SplitProcessor::SplitProcessor()
    : BassProcessor ({ "Nightshift Bass Split", "multiband bass drive", theme::teal }, createLayout())
{
    pGate      = apvts.getRawParameterValue ("gate");
    pLowCross  = apvts.getRawParameterValue ("lowCross");
    pHighCross = apvts.getRawParameterValue ("highCross");
    pLowComp   = apvts.getRawParameterValue ("lowComp");
    pLowLevel  = apvts.getRawParameterValue ("lowLevel");
    pMidDrive  = apvts.getRawParameterValue ("midDrive");
    pMidLevel  = apvts.getRawParameterValue ("midLevel");
    pHighDrive = apvts.getRawParameterValue ("highDrive");
    pHighLevel = apvts.getRawParameterValue ("highLevel");
    pCharacter = apvts.getRawParameterValue ("character");
}

BassProcessor::Layout SplitProcessor::createLayout()
{
    Layout layout;
    layout.add (number  ("gate",      "Gate",       -90.0f, -30.0f, 0.1f, -90.0f, "dB", gateText));
    layout.add (number  ("lowCross",  "Low Split",  60.0f, 400.0f, 0.1f, 160.0f, "Hz", crossText, 160.0f));
    layout.add (number  ("highCross", "High Split", 600.0f, 5000.0f, 0.1f, 1600.0f, "Hz", crossText, 1600.0f));
    layout.add (percent ("lowComp",   "Low Comp",   40.0f));
    layout.add (number  ("lowLevel",  "Low Level",  offDb, 12.0f, 0.1f, 0.0f, "dB", levelText, -6.0f));
    layout.add (percent ("midDrive",  "Mid Drive",  50.0f));
    layout.add (number  ("midLevel",  "Mid Level",  offDb, 12.0f, 0.1f, 0.0f, "dB", levelText, -6.0f));
    layout.add (percent ("highDrive", "High Drive", 40.0f));
    layout.add (number  ("highLevel", "High Level", offDb, 12.0f, 0.1f, -3.0f, "dB", levelText, -6.0f));
    layout.add (choice  ("character", "Character",  { "Smooth", "Tight", "Fuzz" }, 1));
    addToneParameters (layout);
    addCommonParameters (layout);
    return layout;
}

juce::AudioProcessorEditor* SplitProcessor::createEditor()
{
    return new bass::ScaledEditor (*this, new SplitEditor (*this));
}

//==============================================================================
void SplitProcessor::prepareDsp (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    overRate = sampleRate * 4.0;
    maxBlock = std::max (1, (int) spec.maximumBlockSize);
    numChannels = (int) juce::jlimit (1u, 2u, spec.numChannels);

    oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) numChannels, 2,
                                                                      juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                                                      false, true);
    oversampling->initProcessing ((size_t) maxBlock);
    latency = (int) std::round (oversampling->getLatencyInSamples());

    gate.prepare (sampleRate);
    lowCompressor.prepare (overRate);
    midCircuit.prepare (overRate, midReference);
    highCircuit.prepare (overRate, highReference);

    for (int ch = 0; ch < 2; ++ch)
    {
        midDc[ch].prepare (overRate, 5.0);
        highDc[ch].prepare (overRate, 5.0);
    }

    bdsp::prepareSmoothed (lowCrossLog2,  overRate, std::log2 (160.0f), 0.05);
    bdsp::prepareSmoothed (highCrossLog2, overRate, std::log2 (1600.0f), 0.05);
    bdsp::prepareSmoothed (lowGain,       overRate, 1.0f);
    bdsp::prepareSmoothed (midGain,       overRate, 1.0f);
    bdsp::prepareSmoothed (highGain,      overRate, 1.0f);

    prepareTone (spec);
    setTailSeconds (0.15 + latency / sampleRate);
    resetDsp();
}

void SplitProcessor::updateTargets()
{
    gate.setThreshold (pGate->load());
    lowCompressor.setAmount (pLowComp->load() * 0.01f);

    const auto character = (bdsp::Character) juce::jlimit (0, 2, juce::roundToInt (pCharacter->load()));
    midCircuit.setCharacter (character);
    highCircuit.setCharacter (character);

    lowCrossLog2.setTargetValue (std::log2 (juce::jlimit (60.0f, 400.0f, pLowCross->load())));
    highCrossLog2.setTargetValue (std::log2 (juce::jlimit (600.0f, 5000.0f, pHighCross->load())));
    midCircuit.setDrive (pMidDrive->load() * 0.01f);
    highCircuit.setDrive (pHighDrive->load() * 0.01f);
    lowGain.setTargetValue (levelGain (pLowLevel->load()));
    midGain.setTargetValue (levelGain (pMidLevel->load()));
    highGain.setTargetValue (levelGain (pHighLevel->load()));
}

void SplitProcessor::resetDsp()
{
    updateTargets();

    for (auto* s : { &lowCrossLog2, &highCrossLog2, &lowGain, &midGain, &highGain })
        bdsp::snap (*s);

    builtLow = builtHigh = -1.0f;

    if (oversampling != nullptr)
        oversampling->reset();

    gate.reset();
    lowCompressor.reset();
    midCircuit.reset();
    highCircuit.reset();
    channelsLinked = true;

    for (int ch = 0; ch < 2; ++ch)
    {
        midDc[ch].reset();
        highDc[ch].reset();
        xover[ch] = {};
    }

    for (auto& e : env)
        e = 0.0f;

    resetTone();
}

void SplitProcessor::updateCrossovers (float lowLog2, float highLog2)
{
    if (lowLog2 != builtLow)
    {
        builtLow = lowLog2;
        const double hz = std::exp2 ((double) lowLog2);
        lowLp = bdsp::Coeffs::lowpass (hz, butterworthQ, overRate);
        lowHp = bdsp::Coeffs::highpass (hz, butterworthQ, overRate);
    }

    if (highLog2 != builtHigh)
    {
        builtHigh = highLog2;
        const double hz = std::exp2 ((double) highLog2);
        highLp = bdsp::Coeffs::lowpass (hz, butterworthQ, overRate);
        highHp = bdsp::Coeffs::highpass (hz, butterworthQ, overRate);
        highAp = bdsp::Coeffs::allpass (hz, butterworthQ, overRate);
    }
}

void SplitProcessor::processDsp (juce::AudioBuffer<float>& buffer)
{
    const int channels = std::min (buffer.getNumChannels(), numChannels);
    const int total = buffer.getNumSamples();

    if (channels <= 0 || total <= 0 || oversampling == nullptr)
        return;

    updateTargets();

    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = std::min (maxBlock, total - start);
        float* ptrs[2] = { buffer.getWritePointer (0, start), buffer.getWritePointer (std::min (1, channels - 1), start) };
        processChunk (ptrs, channels, n);
    }

    for (int b = 0; b < 3; ++b)
        bandLevel[b].store (env[b]);

    compReduction.store (lowCompressor.getReductionDb());
}

void SplitProcessor::processChunk (float* const* ch, int channels, int n)
{
    // 1. Gate at the input, on the linked level
    for (int i = 0; i < n; ++i)
    {
        float level = 0.0f;

        for (int c = 0; c < channels; ++c)
            level = std::max (level, std::abs (ch[c][i]));

        const float g = gate.next (level);

        for (int c = 0; c < channels; ++c)
            ch[c][i] *= g;
    }

    // 2. Split, process and sum the three bands at 4x. A mono source on both channels runs
    //    channel 0 only and copies it; when the channels part, channel 1 takes over channel
    //    0's exact state (crossover, circuits, DC blockers)
    const bool identical = channels == 2 && std::memcmp (ch[0], ch[1], sizeof (float) * (size_t) n) == 0;

    if (channelsLinked && channels == 2 && ! identical)
    {
        xover[1] = xover[0];
        midDc[1] = midDc[0];
        highDc[1] = highDc[0];
        midCircuit.copyChannel0To1();
        highCircuit.copyChannel0To1();
        channelsLinked = false;
    }

    const int runChannels = channelsLinked && identical ? 1 : channels;
    juce::dsp::AudioBlock<float> block (ch, (size_t) channels, (size_t) n);
    auto up = oversampling->processSamplesUp (block);
    const int upSamples = (int) up.getNumSamples();
    const float envFall = 0.99997f;

    for (int j = 0; j < upSamples; ++j)
    {
        updateCrossovers (lowCrossLog2.getNextValue(), highCrossLog2.getNextValue());

        const float gl = lowGain.getNextValue(), gm = midGain.getNextValue(), gh = highGain.getNextValue();
        midCircuit.tick();
        highCircuit.tick();

        float low[2] {}, mid[2] {}, high[2] {};
        float lowPeak = 0.0f;

        for (int c = 0; c < runChannels; ++c)
        {
            auto& s = xover[c];
            const double x = up.getChannelPointer ((size_t) c)[j];

            double l = s.lowLp[1].process (s.lowLp[0].process (x, lowLp), lowLp);
            l = s.highAp.process (l, highAp);
            const double rest = s.lowHp[1].process (s.lowHp[0].process (x, lowHp), lowHp);
            const double m = s.highLp[1].process (s.highLp[0].process (rest, highLp), highLp);
            const double h = s.highHp[1].process (s.highHp[0].process (rest, highHp), highHp);

            low[c] = (float) l;
            mid[c] = (float) m;
            high[c] = (float) h;
            lowPeak = std::max (lowPeak, std::abs (low[c]));
        }

        const float lowComp = lowCompressor.next (lowPeak);

        for (int c = 0; c < runChannels; ++c)
        {
            const float lo = low[c] * lowComp * gl;
            const float mi = midDc[c].process (midCircuit.process (c, mid[c])) * gm;
            const float hi = highDc[c].process (highCircuit.process (c, high[c])) * gh;

            env[0] = std::max (std::abs (lo), env[0] * envFall);
            env[1] = std::max (std::abs (mi), env[1] * envFall);
            env[2] = std::max (std::abs (hi), env[2] * envFall);

            up.getChannelPointer ((size_t) c)[j] = lo + mi + hi;
        }
    }

    if (runChannels < channels)
        juce::FloatVectorOperations::copy (up.getChannelPointer (1), up.getChannelPointer (0), upSamples);

    oversampling->processSamplesDown (block);

    // 3. Graphic EQ and cabinet
    processTone (ch, channels, n);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SplitProcessor();
}
