#include "BassTone.h"

namespace bass
{

namespace
{
    /** |H| of a biquad at angular frequency w (radians per sample). */
    double magnitude (const dsp::Coeffs& c, double w)
    {
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        const auto num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const auto den = 1.0 + c.a1 * z1 + c.a2 * z2;
        return std::abs (num / den);
    }
}

//==============================================================================
juce::String GraphicEq::bandLabel (int band)
{
    const float hz = frequencies[band];
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz < 2000.0f || std::fmod (hz, 1000.0f) == 0.0f ? 0 : 1) + "k"
                         : juce::String ((int) hz);
}

void GraphicEq::prepare (double sampleRate)
{
    sr = sampleRate;

    for (int b = 0; b < numBands; ++b)
    {
        dsp::prepareSmoothed (gains[b], sr, 0.0f);
        coeffGain[b] = 0.0f;
        coeffs[b] = dsp::Coeffs::peak (frequencies[b], q, 0.0, sr);
    }

    reset();
}

void GraphicEq::reset()
{
    for (int b = 0; b < numBands; ++b)
    {
        dsp::snap (gains[b]);
        coeffGain[b] = gains[b].getCurrentValue();
        coeffs[b] = dsp::Coeffs::peak (frequencies[b], q, coeffGain[b], sr);

        for (auto& ch : state)
            ch[b].reset();
    }
}

void GraphicEq::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::min (numChannels, 2);

    for (int i = 0; i < numSamples; ++i)
    {
        for (int b = 0; b < numBands; ++b)
        {
            const float g = gains[b].getNextValue();

            if (g != coeffGain[b])
            {
                coeffGain[b] = g;
                coeffs[b] = dsp::Coeffs::peak (frequencies[b], q, g, sr);
            }

            // A flat band whose state has died away passes the signal exactly
            if (g == 0.0f && state[0][b].idle() && (numChannels < 2 || state[1][b].idle()))
                continue;

            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][i] = (float) state[ch][b].process (channels[ch][i], coeffs[b]);
        }
    }
}

//==============================================================================
// Bass cab shapes, by ear and by the published curves of typical drivers: a port/box
// resonance at the bottom, a scoop or push in the mids, cone break-up, and a steep
// 24 dB/octave top where the drivers give out. Trims level-match them to Off on a bass
// line (set from basstool measurements).
const Cabinet::Voicing Cabinet::voicings[numVoicings] =
{
    // 4x10: punchy and even, a little presence where the cones break up
    { 46.0f, 1.0f, { { 110.0f, 1.0f, 2.0f }, { 450.0f, 1.2f, -3.0f }, { 2200.0f, 1.6f, 2.5f } }, 4300.0f, -1.0f },
    // 8x10: big low thump, scooped low-mids, darker top
    { 40.0f, 1.2f, { { 80.0f, 0.9f, 3.0f }, { 550.0f, 1.0f, -2.5f }, { 1500.0f, 1.4f, 2.0f } }, 3400.0f, -3.2f },
    // 1x15: round and deep, soft mids, the darkest
    { 36.0f, 0.8f, { { 70.0f, 0.8f, 2.5f }, { 800.0f, 0.9f, -4.0f }, { 1700.0f, 2.0f, -2.0f } }, 2500.0f, -1.6f },
    // 2x12: tighter bottom, mids forward, the brightest
    { 58.0f, 0.8f, { { 250.0f, 1.0f, -2.0f }, { 1200.0f, 1.0f, 3.0f }, { 3200.0f, 2.0f, 1.5f } }, 4500.0f, 1.2f },
};

std::array<dsp::Coeffs, 6> Cabinet::design (const Voicing& v, double sampleRate)
{
    return { dsp::Coeffs::highpass (v.highpassHz, v.highpassQ, sampleRate),
             dsp::Coeffs::peak (v.peaks[0].hz, v.peaks[0].q, v.peaks[0].db, sampleRate),
             dsp::Coeffs::peak (v.peaks[1].hz, v.peaks[1].q, v.peaks[1].db, sampleRate),
             dsp::Coeffs::peak (v.peaks[2].hz, v.peaks[2].q, v.peaks[2].db, sampleRate),
             dsp::Coeffs::lowpass (v.lowpassHz, 0.54119610, sampleRate),     // 4th-order Butterworth
             dsp::Coeffs::lowpass (v.lowpassHz, 1.30656296, sampleRate) };
}

float Cabinet::voicingResponseDb (int voicing, float hz, double sampleRate)
{
    const auto& v = voicings[juce::jlimit (0, numVoicings - 1, voicing)];
    const double w = juce::MathConstants<double>::twoPi * hz / sampleRate;
    double m = juce::Decibels::decibelsToGain ((double) v.trimDb);

    for (auto& c : design (v, sampleRate))
        m *= magnitude (c, w);

    return (float) juce::Decibels::gainToDecibels (m, -120.0);
}

void Cabinet::prepare (const juce::dsp::ProcessSpec& spec)
{
    sr = spec.sampleRate;
    maxBlock = (int) spec.maximumBlockSize;

    for (int v = 0; v < numVoicings; ++v)
    {
        coeffs[v] = design (voicings[v], sr);
        trim[v] = dsp::dbToGain (voicings[v].trimDb);
    }

    for (int c = 0; c < numChoices; ++c)
        dsp::prepareSmoothed (weights[c], sr, c == requested ? 1.0f : 0.0f, 0.05);

    convolution.prepare ({ sr, spec.maximumBlockSize, 2 });
    irOut.setSize (2, maxBlock);
    reset();
}

void Cabinet::reset()
{
    for (int c = 0; c < numChoices; ++c)
    {
        dsp::snap (weights[c]);
        running[c] = weights[c].getCurrentValue() > 0.0f;
    }

    for (auto& voicing : state)
        for (auto& ch : voicing)
            for (auto& b : ch)
                b.reset();

    convolution.reset();
}

void Cabinet::setChoice (int choice)
{
    // IR picked with no file loaded behaves like Off rather than going silent
    requested = juce::jlimit (0, numChoices - 1, choice);

    if (requested == irChoice && ! irReady.load())
        requested = offChoice;

    for (int c = 0; c < numChoices; ++c)
        weights[c].setTargetValue (c == requested ? 1.0f : 0.0f);
}

void Cabinet::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::min (numChannels, 2);

    // The impulse response runs on whole blocks; it is started fresh when it fades in
    auto& irWeight = weights[irChoice];
    const bool irWanted = irWeight.getCurrentValue() > 0.0f || irWeight.getTargetValue() > 0.0f;

    if (irWanted)
    {
        if (! running[irChoice])
        {
            convolution.reset();
            running[irChoice] = true;
        }

        for (int ch = 0; ch < 2; ++ch)
            irOut.copyFrom (ch, 0, channels[std::min (ch, numChannels - 1)], numSamples);

        juce::dsp::AudioBlock<float> block (irOut.getArrayOfWritePointers(), 2, (size_t) numSamples);
        convolution.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

    for (int i = 0; i < numSamples; ++i)
    {
        float w[numChoices];

        for (int c = 0; c < numChoices; ++c)
            w[c] = weights[c].getNextValue();

        for (int v = 0; v < numVoicings; ++v)
        {
            const int c = v + 1;
            const bool wanted = w[c] > 0.0f || weights[c].getTargetValue() > 0.0f;

            if (wanted && ! running[c])
            {
                for (auto& ch : state[v])
                    for (auto& b : ch)
                        b.reset();

                running[c] = true;
            }
            else if (! wanted)
            {
                running[c] = false;
            }
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float x = channels[ch][i];
            double y = w[offChoice] * x;

            for (int v = 0; v < numVoicings; ++v)
            {
                if (! running[v + 1])
                    continue;

                double s = x;

                for (int k = 0; k < 6; ++k)
                    s = state[v][ch][k].process (s, coeffs[v][(size_t) k]);

                y += w[v + 1] * trim[v] * s;
            }

            if (irWanted)
                y += w[irChoice] * irOut.getSample (ch, i);

            channels[ch][i] = (float) y;
        }
    }

    if (irWanted && irWeight.getCurrentValue() <= 0.0f && irWeight.getTargetValue() <= 0.0f)
        running[irChoice] = false;
}

//==============================================================================
bool Cabinet::loadImpulseResponse (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        error = "can't read " + file.getFileName();
        return false;
    }

    // Cab IRs are a few thousand samples; anything past a second is not a cabinet
    const int length = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) reader->sampleRate);

    if (length < 16)
    {
        error = file.getFileName() + " is too short to be an impulse response";
        return false;
    }

    juce::AudioBuffer<float> ir (1, length);
    reader->read (&ir, 0, length, 0, true, false);

    // Level-match: average magnitude from 40 to 250 Hz at 0 dB. That is where a bass line's
    // energy sits, and where the voicings' trims put them level with Off on one
    const auto* d = ir.getReadPointer (0);
    double sumDb = 0.0;
    int count = 0;

    for (double hz = 40.0; hz <= 250.0; hz *= std::pow (2.0, 1.0 / 6.0))
    {
        const double w = juce::MathConstants<double>::twoPi * hz / reader->sampleRate;
        std::complex<double> h;

        for (int n = 0; n < length; ++n)
            h += (double) d[n] * std::polar (1.0, -w * n);

        sumDb += juce::Decibels::gainToDecibels (std::abs (h), -120.0);
        ++count;
    }

    const double averageDb = sumDb / std::max (1, count);

    if (averageDb < -100.0)
    {
        error = file.getFileName() + " is silent";
        return false;
    }

    ir.applyGain ((float) juce::Decibels::decibelsToGain (-averageDb));
    convolution.loadImpulseResponse (std::move (ir), reader->sampleRate, juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::yes, juce::dsp::Convolution::Normalise::no);
    irReady = true;
    return true;
}

void Cabinet::clearImpulseResponse()
{
    irReady = false;
}

} // namespace bass
