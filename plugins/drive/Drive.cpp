#include "Drive.h"
#include "common/ScaledEditor.h"

using namespace bass;
namespace bdsp = bass::dsp;
using namespace bass::params;

namespace
{
    // Grunt: how much low end goes into the clipper. Thin keeps the lows out (the clean
    // path carries them), Raw lets the body in, Fat adds a low shelf on top. First-order
    // highpasses, as in the pedal circuits: they turn the drive path's fundamentals at most
    // 90 degrees against the clean path, where a second-order one turned them ~120 degrees
    // and the blend cancelled its own low end (basstool: blend 60 was quieter than either path)
    constexpr float gruntHighpassHz[3] = { 320.0f, 120.0f, 30.0f };
    constexpr float gruntShelfDb[3]    = { 0.0f, 0.0f, 6.0f };
    constexpr float gruntShelfHz       = 110.0f;

    // Attack: a high shelf into the circuit for more or less pick and string bite, and a
    // gentler one after it. A saturating circuit flattens whatever was boosted before it,
    // so on its own the pre shelf all but vanishes at high drive; the post shelf keeps the
    // switch audible at every setting (basstool: Boost vs Cut at 4 kHz)
    constexpr float attackShelfDb[3]   = { -6.0f, 0.0f, 6.0f };
    constexpr float attackShelfHz      = 1800.0f;
    constexpr float attackPostDb[3]    = { -4.0f, 0.0f, 4.0f };
    constexpr float attackPostHz       = 2500.0f;

    // Body: up to +4 dB at low drive, where the circuit is nearly linear and the grunt
    // filter has taken the fundamentals out with nothing put back yet; gone by full drive,
    // where the harmonics carry the energy (basstool: bassdi level within 4 dB, drive 0..100)
    float bodyGain (float drive01)          { return 1.0f + 0.6f * (1.0f - drive01) * (1.0f - drive01); }

    // Tone 0..100 -> 1.2..14 kHz lowpass after the circuit
    constexpr float toneMinHz = 1200.0f, toneMaxHz = 14000.0f;

    // The level the drive path's input typically peaks at (a DI near -8 dBFS, less what the
    // grunt filter takes): the circuits are level-calibrated against it
    constexpr float driveReference = 0.25f;

    juce::String gateText (float v, int)
    {
        return v <= -89.9f ? juce::String ("Off") : juce::String (juce::roundToInt (v)) + " dB";
    }
}

//==============================================================================
DriveProcessor::DriveProcessor()
    : BassProcessor ({ "Nightshift Bass Drive", "bass overdrive", theme::amber }, createLayout())
{
    pGate      = apvts.getRawParameterValue ("gate");
    pComp      = apvts.getRawParameterValue ("comp");
    pDrive     = apvts.getRawParameterValue ("drive");
    pGrunt     = apvts.getRawParameterValue ("grunt");
    pAttack    = apvts.getRawParameterValue ("attack");
    pCharacter = apvts.getRawParameterValue ("character");
    pTone      = apvts.getRawParameterValue ("tone");
    pBlend     = apvts.getRawParameterValue ("blend");
    pLevel     = apvts.getRawParameterValue ("level");
}

BassProcessor::Layout DriveProcessor::createLayout()
{
    Layout layout;
    layout.add (number   ("gate",      "Gate",      -90.0f, -30.0f, 0.1f, -90.0f, "dB", gateText));
    layout.add (percent  ("comp",      "Comp",      25.0f));
    layout.add (percent  ("drive",     "Drive",     45.0f));
    layout.add (choice   ("grunt",     "Grunt",     { "Thin", "Raw", "Fat" }, 1));
    layout.add (choice   ("attack",    "Attack",    { "Cut", "Flat", "Boost" }, 1));
    layout.add (choice   ("character", "Character", { "Smooth", "Tight", "Fuzz" }, 1));
    layout.add (percent  ("tone",      "Tone",      55.0f));
    layout.add (percent  ("blend",     "Blend",     60.0f));
    layout.add (decibels ("level",     "Level",     -12.0f, 12.0f, 0.0f));
    addToneParameters (layout);
    addCommonParameters (layout);
    return layout;
}

juce::AudioProcessorEditor* DriveProcessor::createEditor()
{
    return new bass::ScaledEditor (*this, new DriveEditor (*this));
}

//==============================================================================
void DriveProcessor::prepareDsp (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    maxBlock = std::max (1, (int) spec.maximumBlockSize);
    numChannels = (int) juce::jlimit (1u, 2u, spec.numChannels);

    // 4x, linear-phase FIR halfbands with whole-sample latency: the clean path is delayed
    // by exactly that, so the blend lines up at every frequency
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) numChannels, 2,
                                                                      juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                                                      false, true);
    oversampling->initProcessing ((size_t) maxBlock);
    latency = (int) std::round (oversampling->getLatencyInSamples());

    cleanBuffer.setSize (2, maxBlock);
    driveBuffer.setSize (2, maxBlock);
    gate.prepare (sampleRate);
    compressor.prepare (sampleRate);
    circuit.prepare (sampleRate * 4.0, driveReference);

    for (int ch = 0; ch < 2; ++ch)
    {
        cleanDelay[ch].prepare (latency + 1);
        cleanDelay[ch].setDelay (latency);
        dcBlocker[ch].prepare (sampleRate, 5.0);
    }

    bdsp::prepareSmoothed (gruntLog2Hz,  sampleRate, std::log2 (gruntHighpassHz[1]));
    bdsp::prepareSmoothed (gruntShelfDb, sampleRate, 0.0f);
    bdsp::prepareSmoothed (attackDb,     sampleRate, 0.0f);
    bdsp::prepareSmoothed (attackPost,   sampleRate, 0.0f);
    bdsp::prepareSmoothed (body,         sampleRate, bodyGain (0.45f));
    bdsp::prepareSmoothed (toneLog2Hz,   sampleRate, std::log2 (4000.0f));
    bdsp::prepareSmoothed (blend,        sampleRate, 0.6f);
    bdsp::prepareSmoothed (levelGain,    sampleRate, 1.0f);

    prepareTone (spec);
    setTailSeconds (0.15 + latency / sampleRate);
    resetDsp();
}

void DriveProcessor::updateTargets()
{
    gate.setThreshold (pGate->load());
    compressor.setAmount (pComp->load() * 0.01f);

    const int g = juce::jlimit (0, 2, juce::roundToInt (pGrunt->load()));
    gruntLog2Hz.setTargetValue (std::log2 (gruntHighpassHz[g]));
    gruntShelfDb.setTargetValue (::gruntShelfDb[g]);
    const int a = juce::jlimit (0, 2, juce::roundToInt (pAttack->load()));
    attackDb.setTargetValue (attackShelfDb[a]);
    attackPost.setTargetValue (attackPostDb[a]);
    body.setTargetValue (bodyGain (pDrive->load() * 0.01f));
    circuit.setCharacter ((bdsp::Character) juce::jlimit (0, 2, juce::roundToInt (pCharacter->load())));
    circuit.setDrive (pDrive->load() * 0.01f);

    toneLog2Hz.setTargetValue (std::log2 (toneMinHz) + pTone->load() * 0.01f * std::log2 (toneMaxHz / toneMinHz));
    blend.setTargetValue (pBlend->load() * 0.01f);
    levelGain.setTargetValue (bdsp::dbToGain (pLevel->load()));
}

void DriveProcessor::resetDsp()
{
    // Called on the audio thread when bypass ends: no allocation below
    updateTargets();

    for (auto* s : { &gruntLog2Hz, &gruntShelfDb, &attackDb, &attackPost, &body, &toneLog2Hz, &blend, &levelGain })
        bdsp::snap (*s);

    builtGrunt = builtTone = -1.0f;
    builtShelf = builtAttack = builtAttackPost = -1000.0f;

    if (oversampling != nullptr)
        oversampling->reset();

    gate.reset();
    compressor.reset();
    circuit.reset();
    channelsLinked = true;

    for (int ch = 0; ch < 2; ++ch)
    {
        cleanDelay[ch].reset();
        dcBlocker[ch].reset();
        gruntHighpassState[ch].reset();
        gruntShelfState[ch].reset();
        attackShelfState[ch].reset();
        toneLowpassState[ch].reset();
        attackPostState[ch].reset();
    }

    activityEnv = 0.0f;
    resetTone();
}

void DriveProcessor::processDsp (juce::AudioBuffer<float>& buffer)
{
    const int channels = std::min (buffer.getNumChannels(), numChannels);
    const int total = buffer.getNumSamples();

    if (channels <= 0 || total <= 0 || oversampling == nullptr)
        return;

    // Targets once per call; everything glides sample by sample, so block size does not matter
    updateTargets();

    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = std::min (maxBlock, total - start);
        float* ptrs[2] = { buffer.getWritePointer (0, start), buffer.getWritePointer (std::min (1, channels - 1), start) };
        processChunk (ptrs, channels, n);
    }

    gateGain.store (gate.getGain());
    compReduction.store (compressor.getReductionDb());
    driveActivity.store (activityEnv);
}

void DriveProcessor::processChunk (float* const* ch, int channels, int n)
{
    // 1. Gate and compressor on the linked level, then the split: the clean path is delayed
    //    to meet the oversampled drive path; the drive path is voiced into the circuit
    for (int i = 0; i < n; ++i)
    {
        float level = 0.0f;

        for (int c = 0; c < channels; ++c)
            level = std::max (level, std::abs (ch[c][i]));

        const float g = gate.next (level);
        const float gain = g * compressor.next (level * g);

        const float gHz = gruntLog2Hz.getNextValue();
        const float shelf = gruntShelfDb.getNextValue();
        const float att = attackDb.getNextValue();

        if (gHz != builtGrunt)       { builtGrunt = gHz;    gruntHighpass = bdsp::Coeffs::highpass1 (std::exp2 ((double) gHz), sampleRate); }
        if (shelf != builtShelf)     { builtShelf = shelf;  gruntShelf = bdsp::Coeffs::lowShelf (gruntShelfHz, shelf, sampleRate); }
        if (att != builtAttack)      { builtAttack = att;   attackShelf = bdsp::Coeffs::highShelf (attackShelfHz, att, sampleRate); }

        for (int c = 0; c < channels; ++c)
        {
            const float x = ch[c][i] * gain;
            cleanBuffer.setSample (c, i, cleanDelay[c].process (x));

            double d = x;
            d = gruntHighpassState[c].process (d, gruntHighpass);
            d = gruntShelfState[c].process (d, gruntShelf);
            d = attackShelfState[c].process (d, attackShelf);
            driveBuffer.setSample (c, i, (float) d);
        }
    }

    // 2. The drive circuit, at 4x
    {
        // A mono source on both channels runs one channel's circuit and copies it; the
        // moment the channels part, channel 1 takes over channel 0's exact state
        const bool identical = channels == 2 && std::memcmp (driveBuffer.getReadPointer (0), driveBuffer.getReadPointer (1),
                                                             sizeof (float) * (size_t) n) == 0;

        if (channelsLinked && channels == 2 && ! identical)
        {
            circuit.copyChannel0To1();
            channelsLinked = false;
        }

        const bool runOne = channelsLinked && identical;
        juce::dsp::AudioBlock<float> block (driveBuffer.getArrayOfWritePointers(), (size_t) channels, (size_t) n);
        auto up = oversampling->processSamplesUp (block);
        const int upSamples = (int) up.getNumSamples();

        for (int j = 0; j < upSamples; ++j)
        {
            circuit.tick();

            if (runOne)
            {
                float* p0 = up.getChannelPointer (0);
                p0[j] = circuit.process (0, p0[j]);
                up.getChannelPointer (1)[j] = p0[j];
                continue;
            }

            for (int c = 0; c < channels; ++c)
            {
                float* p = up.getChannelPointer ((size_t) c);
                p[j] = circuit.process (c, p[j]);
            }
        }

        oversampling->processSamplesDown (block);
    }

    // 3. After the circuit (level-calibrated already): DC block, tone and level, then the blend
    for (int i = 0; i < n; ++i)
    {
        const float tone = toneLog2Hz.getNextValue();
        // Equal-power blend: the clean path (the only one carrying the deep lows) holds on
        // longer than a linear crossfade would let it
        const float b = blend.getNextValue();
        const float cleanGain = std::cos (b * juce::MathConstants<float>::halfPi);
        const float wetGain = std::sin (b * juce::MathConstants<float>::halfPi);
        const float lvl = levelGain.getNextValue() * body.getNextValue();
        const float post = attackPost.getNextValue();

        if (post != builtAttackPost) { builtAttackPost = post; attackPostShelf = bdsp::Coeffs::highShelf (attackPostHz, post, sampleRate); }

        if (tone != builtTone)       { builtTone = tone;    toneLowpass = bdsp::Coeffs::lowpass (std::exp2 ((double) tone), 0.70710678, sampleRate); }

        for (int c = 0; c < channels; ++c)
        {
            float d = dcBlocker[c].process (driveBuffer.getSample (c, i));
            d = (float) attackPostState[c].process (toneLowpassState[c].process (d, toneLowpass), attackPostShelf) * lvl;
            activityEnv = std::max (std::abs (d) * wetGain, activityEnv * 0.9995f);
            ch[c][i] = cleanBuffer.getSample (c, i) * cleanGain + d * wetGain;
        }
    }

    processTone (ch, channels, n);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DriveProcessor();
}
