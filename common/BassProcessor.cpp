#include "BassProcessor.h"

namespace bass
{

BassProcessor::BassProcessor (Info i, Layout&& layout)
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "state", std::move (layout)),
      info (std::move (i))
{
    mixParam = apvts.getRawParameterValue ("mix");
    outputParam = apvts.getRawParameterValue ("output");
    inputParam = apvts.getRawParameterValue ("input");
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    jassert (mixParam != nullptr && outputParam != nullptr && bypassParam != nullptr);   // forgot addCommonParameters?

    for (int b = 0; b < GraphicEq::numBands; ++b)
        eqParams[b] = apvts.getRawParameterValue (GraphicEq::bandId (b));

    cabParam = apvts.getRawParameterValue ("cab");
}

//==============================================================================
void BassProcessor::addToneParameters (Layout& layout)
{
    for (int b = 0; b < GraphicEq::numBands; ++b)
    {
        const auto label = GraphicEq::bandLabel (b);
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID (GraphicEq::bandId (b), 1),
                                                                 "EQ " + label + (label.endsWith ("k") ? "Hz" : " Hz"),
                                                                 juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f,
                                                                 juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    }

    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID ("cab", 1), "Cab", Cabinet::choiceNames(), 1));
}

void BassProcessor::prepareTone (const juce::dsp::ProcessSpec& spec)
{
    eq.prepare (spec.sampleRate);
    cabinet.prepare (spec);
    resetTone();
}

void BassProcessor::resetTone()
{
    for (int b = 0; b < GraphicEq::numBands; ++b)
        if (eqParams[b] != nullptr)
            eq.setGainDb (b, eqParams[b]->load());

    if (cabParam != nullptr)
        cabinet.setChoice (juce::roundToInt (cabParam->load()));

    eq.reset();
    cabinet.reset();
}

void BassProcessor::processTone (float* const* channels, int numChannels, int numSamples)
{
    for (int b = 0; b < GraphicEq::numBands; ++b)
        if (eqParams[b] != nullptr)
            eq.setGainDb (b, eqParams[b]->load());

    if (cabParam != nullptr)
        cabinet.setChoice (juce::roundToInt (cabParam->load()));

    eq.process (channels, numChannels, numSamples);
    cabinet.process (channels, numChannels, numSamples);
}

//==============================================================================
bool BassProcessor::loadImpulseResponse (const juce::File& file, juce::String& error)
{
    if (! cabinet.loadImpulseResponse (file, error))
        return false;

    apvts.state.setProperty ("irFile", file.getFullPathName(), nullptr);
    irMissing = false;

    if (auto* cab = apvts.getParameter ("cab"))
    {
        cab->beginChangeGesture();
        cab->setValueNotifyingHost (cab->convertTo0to1 ((float) Cabinet::irChoice));
        cab->endChangeGesture();
    }

    return true;
}

void BassProcessor::clearImpulseResponse()
{
    cabinet.clearImpulseResponse();
    apvts.state.removeProperty ("irFile", nullptr);
    irMissing = false;
}

juce::String BassProcessor::getImpulseResponseName (bool* missing) const
{
    if (missing != nullptr)
        *missing = irMissing.load();

    return juce::File (apvts.state.getProperty ("irFile").toString()).getFileName();
}

void BassProcessor::restoreImpulseResponse()
{
    const juce::String path = apvts.state.getProperty ("irFile").toString();

    if (path.isEmpty())
    {
        cabinet.clearImpulseResponse();
        irMissing = false;
        return;
    }

    juce::String error;
    irMissing = ! cabinet.loadImpulseResponse (juce::File (path), error);
}

float BassProcessor::param (const juce::String& id) const
{
    if (auto* v = apvts.getRawParameterValue (id))
        return v->load();

    jassertfalse;
    return 0.0f;
}

//==============================================================================
void BassProcessor::addCommonParameters (Layout& layout)
{
    // Input: how hot the bass hits the circuits (they respond to level like the real ones);
    // the dry path and bypass are untouched by it
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID ("input", 1), "Input",
                                                             juce::NormalisableRange<float> (-12.0f, 24.0f, 0.1f), 0.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID ("mix", 1), "Mix",
                                                             juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (percentText)));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID ("output", 1), "Output",
                                                             juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID ("bypass", 1), "Bypass", false));
}

juce::String BassProcessor::percentText (float v, int)    { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String BassProcessor::dbText (float v, int)         { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String BassProcessor::msText (float v, int)         { return juce::String (v, v < 10.0f ? 1 : 0) + " ms"; }

juce::String BassProcessor::hzText (float v, int)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " Hz";
}

//==============================================================================
bool BassProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void BassProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) std::max (1, getTotalNumOutputChannels()) };
    currentSampleRate = sampleRate;

    prepareDsp (spec);

    const int latency = getDspLatency();
    jassert (latency >= 0 && latency < maxLatencySamples);   // raise maxLatencySamples if a plugin needs more

    mixer.prepare (spec);
    mixer.setMixingRule (juce::dsp::DryWetMixingRule::linear);
    mixer.setWetLatency ((float) juce::jlimit (0, maxLatencySamples - 1, latency));
    setLatencySamples (latency);

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
    inputGain.reset (sampleRate, 0.02);
    inputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputParam->load()));

    transportGate.reset (sampleRate, 0.1);
    transportGate.setCurrentAndTargetValue (1.0f);
    gateFrom = gateTo = 1.0f;
    gatePrimed = false;

    primed = false;   // the first block picks up the bypass state without a fade

    for (auto& p : inputPeak)  p.store (0.0f);
    for (auto& p : outputPeak) p.store (0.0f);
}

void BassProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numChannels = std::min (buffer.getNumChannels(), getTotalNumOutputChannels());
    const int numSamples = buffer.getNumSamples();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float peak = buffer.getMagnitude (ch, 0, numSamples);
        inputPeak[ch & 1].store (std::max (inputPeak[ch & 1].load(), peak));
    }

    const bool bypassRequested = bypassParam->get();
    const float mix = mixParam->load() * 0.01f;
    const float trim = juce::Decibels::decibelsToGain (outputParam->load());

    if (! primed)
    {
        primed = true;
        bypassState = bypassRequested ? BypassState::bypassed : BypassState::active;
        mixer.setWetMixProportion (bypassRequested ? 0.0f : mix);
        mixer.reset();
        outputGain.setCurrentAndTargetValue (bypassRequested ? 1.0f : trim);
    }
    else if (bypassRequested && bypassState == BypassState::active)
    {
        // Keep processing while the wet path fades out, then stop
        bypassState = BypassState::fadingOut;
        fadeRemaining = (int) std::ceil (currentSampleRate * 0.05) + 1;
    }
    else if (! bypassRequested && bypassState != BypassState::active)
    {
        // Back from bypass: start the wet path from silence and fade it in, while the
        // latency-matched dry keeps playing, so nothing from before the bypass is replayed
        if (bypassState == BypassState::bypassed)
            resetDsp();

        bypassState = BypassState::active;
    }

    const bool active = bypassState == BypassState::active;
    mixer.setWetMixProportion (active ? mix : 0.0f);
    outputGain.setTargetValue (active ? trim : 1.0f);

    // Made-up sound follows the host's transport, when it reports one: a stopped song is
    // silent, not still hissing (the first block lands on it, with no fade)
    float gateTarget = 1.0f;

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            gateTarget = position->getIsPlaying() ? 1.0f : 0.0f;

    if (gatePrimed)
    {
        transportGate.setTargetValue (gateTarget);
    }
    else
    {
        transportGate.setCurrentAndTargetValue (gateTarget);
        gatePrimed = true;
    }

    gateFrom = transportGate.getCurrentValue();
    gateTo = transportGate.skip (numSamples);
    gateBlockLength = numSamples;

    juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numChannels, (size_t) numSamples);
    mixer.pushDrySamples (block);

    if (bypassState != BypassState::bypassed)
    {
        // Input trim on the wet path only, so the dry path and bypass stay the raw input
        inputGain.setTargetValue (juce::Decibels::decibelsToGain (inputParam->load()));

        if (inputGain.isSmoothing())
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = inputGain.getNextValue();

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.getWritePointer (ch)[i] *= g;
            }
        }
        else if (inputGain.getCurrentValue() != 1.0f)
        {
            buffer.applyGain (0, numSamples, inputGain.getCurrentValue());
        }

        processDsp (buffer);
    }

    // Bypassed, the buffer still holds the input; at a wet proportion of 0 the mixer
    // returns the dry path alone, delayed by the reported latency
    mixer.mixWetSamples (block);

    if (outputGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outputGain.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else if (outputGain.getCurrentValue() != 1.0f)
    {
        buffer.applyGain (0, numSamples, outputGain.getCurrentValue());
    }

    if (bypassState == BypassState::fadingOut && (fadeRemaining -= numSamples) <= 0)
        bypassState = BypassState::bypassed;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float peak = buffer.getMagnitude (ch, 0, numSamples);
        outputPeak[ch & 1].store (std::max (outputPeak[ch & 1].load(), peak));
    }
}

//==============================================================================
void BassProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("pluginVersion", JucePlugin_VersionString, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void BassProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            restoreImpulseResponse();
        }
}

} // namespace bass
