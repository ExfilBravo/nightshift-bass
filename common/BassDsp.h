#pragma once

#include <JuceHeader.h>

/** Header-only DSP building blocks shared by the Nightshift Bass plugins. Everything is
    per-sample and deterministic, so 64-sample blocks render exactly like 512-sample ones,
    and nothing allocates on the audio thread after prepare(). Filter maths runs in double:
    the crossovers and drive filters sit at 40-400 Hz on a 4x-oversampled clock, where
    float coefficients would drift.
*/
namespace bass::dsp
{

inline float dbToGain (float db)    { return juce::Decibels::decibelsToGain (db, -120.0f); }

using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

inline void prepareSmoothed (Smoothed& s, double sampleRate, float initial, double rampSeconds = 0.03)
{
    s.reset (sampleRate, rampSeconds);
    s.setCurrentAndTargetValue (initial);
}

inline void snap (Smoothed& s)      { s.setCurrentAndTargetValue (s.getTargetValue()); }

//==============================================================================
/** Normalised biquad coefficients (a0 = 1), RBJ cookbook designs. */
struct Coeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static Coeffs raw (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    static double omega (double hz, double sr)
    {
        return juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sr * 0.49, hz) / sr;
    }

    static Coeffs lowpass (double hz, double q, double sr)
    {
        const double w = omega (hz, sr), c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        return raw ((1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    static Coeffs highpass (double hz, double q, double sr)
    {
        const double w = omega (hz, sr), c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        return raw ((1.0 + c) * 0.5, -(1.0 + c), (1.0 + c) * 0.5, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    /** Second-order allpass; at Q 1/sqrt2 it equals an LR4 lowpass + highpass pair summed. */
    static Coeffs allpass (double hz, double q, double sr)
    {
        const double w = omega (hz, sr), c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        return raw (1.0 - alpha, -2.0 * c, 1.0 + alpha, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
    }

    static Coeffs peak (double hz, double q, double db, double sr)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = omega (hz, sr), c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        return raw (1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
    }

    /** Shelves with slope S = 1 (no overshoot). */
    static Coeffs lowShelf (double hz, double db, double sr)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = omega (hz, sr), c = std::cos (w), alpha = std::sin (w) * 0.5 * std::sqrt (2.0);
        const double k = 2.0 * std::sqrt (A) * alpha;
        return raw (A * ((A + 1.0) - (A - 1.0) * c + k), 2.0 * A * ((A - 1.0) - (A + 1.0) * c), A * ((A + 1.0) - (A - 1.0) * c - k),
                    (A + 1.0) + (A - 1.0) * c + k, -2.0 * ((A - 1.0) + (A + 1.0) * c), (A + 1.0) + (A - 1.0) * c - k);
    }

    static Coeffs highShelf (double hz, double db, double sr)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = omega (hz, sr), c = std::cos (w), alpha = std::sin (w) * 0.5 * std::sqrt (2.0);
        const double k = 2.0 * std::sqrt (A) * alpha;
        return raw (A * ((A + 1.0) + (A - 1.0) * c + k), -2.0 * A * ((A - 1.0) + (A + 1.0) * c), A * ((A + 1.0) + (A - 1.0) * c - k),
                    (A + 1.0) - (A - 1.0) * c + k, 2.0 * ((A - 1.0) - (A + 1.0) * c), (A + 1.0) - (A - 1.0) * c - k);
    }

    /** First-order sections (bilinear, prewarped). */
    static Coeffs lowpass1 (double hz, double sr)
    {
        const double K = std::tan (omega (hz, sr) * 0.5);
        return { K / (1.0 + K), K / (1.0 + K), 0.0, (K - 1.0) / (K + 1.0), 0.0 };
    }

    static Coeffs highpass1 (double hz, double sr)
    {
        const double K = std::tan (omega (hz, sr) * 0.5);
        return { 1.0 / (1.0 + K), -1.0 / (1.0 + K), 0.0, (K - 1.0) / (K + 1.0), 0.0 };
    }
};

/** Transposed direct form II, double state. */
struct Biquad
{
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept                   { z1 = z2 = 0.0; }
    bool idle() const noexcept              { return std::abs (z1) < 1e-12 && std::abs (z2) < 1e-12; }

    double process (double x, const Coeffs& c) noexcept
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

/** One-pole highpass that removes DC after asymmetric clipping. */
class DcBlocker
{
public:
    void prepare (double sampleRate, double hz = 5.0)
    {
        r = 1.0 - juce::MathConstants<double>::twoPi * hz / sampleRate;
        reset();
    }

    void reset() noexcept                   { x1 = y1 = 0.0; }

    float process (float x) noexcept
    {
        const double y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return (float) y;
    }

private:
    double r = 0.9995, x1 = 0.0, y1 = 0.0;
};

/** A delay of a whole number of samples, for latency-matching a parallel path. */
class IntDelay
{
public:
    void prepare (int maxDelay)
    {
        buffer.assign ((size_t) juce::nextPowerOfTwo (std::max (2, maxDelay + 1)), 0.0f);
        mask = (int) buffer.size() - 1;
        reset();
    }

    void setDelay (int d)                   { delay = juce::jlimit (0, mask, d); }
    void reset() noexcept                   { std::fill (buffer.begin(), buffer.end(), 0.0f); pos = 0; }

    float process (float x) noexcept
    {
        buffer[(size_t) pos] = x;
        const float y = buffer[(size_t) ((pos - delay) & mask)];
        pos = (pos + 1) & mask;
        return y;
    }

private:
    std::vector<float> buffer;
    int mask = 0, pos = 0, delay = 0;
};

//==============================================================================
/** The three clipping characters, as memoryless curves with closed-form antiderivatives,
    run with first-order antiderivative anti-aliasing (ADAA) on top of 4x oversampling.
    Every curve has unity gain for small signals (so drive 0 is nearly transparent, and
    switching character never jumps in level) and saturates around an average of 1.

      smooth  tanh with a bias: soft, asymmetric (+0.76 / -1.24), even harmonics
              (a warm valve stage)
      tight   cubic soft clipper, flat at +-1 from 1.5 in: a firm knee (op-amp into diodes)
      fuzz    asymmetric: a hard ceiling at +0.75, a soft one at -1.25 (a transistor
              fuzz pushed hard: bright, torn, strong even harmonics)
*/
enum class Character { smooth = 0, tight = 1, fuzz = 2 };

namespace shaper
{
    constexpr double smoothBias = 0.25;
    inline const double tanhOfBias = std::tanh (smoothBias);
    inline const double smoothScale = 1.0 - tanhOfBias * tanhOfBias;   // the curve's slope at 0
    constexpr double tightKnee = 1.5;
    constexpr double fuzzTop = 0.75, fuzzBottom = 1.25;

    inline double logCosh (double x)
    {
        const double a = std::abs (x);
        return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531;
    }

    /** 1.5x - 0.5x^3, flat at +-1 beyond +-1 (slope 1.5 at 0). */
    inline double cubic (double x)
    {
        return x >= 1.0 ? 1.0 : (x <= -1.0 ? -1.0 : 1.5 * x - 0.5 * x * x * x);
    }

    inline double cubicIntegral (double x)
    {
        const double a = std::abs (x);
        return a >= 1.0 ? a - 0.375 : 0.75 * x * x - 0.125 * x * x * x * x;
    }

    inline double curve (double x, Character c)
    {
        switch (c)
        {
            case Character::smooth:  return std::tanh (x / smoothScale + smoothBias) - tanhOfBias;
            case Character::tight:   return cubic (x / tightKnee);
            case Character::fuzz:    return x >= 0.0 ? std::min (x, fuzzTop) : fuzzBottom * std::tanh (x / fuzzBottom);
        }

        return x;
    }

    inline double integral (double x, Character c)
    {
        switch (c)
        {
            case Character::smooth:  return smoothScale * logCosh (x / smoothScale + smoothBias) - x * tanhOfBias;
            case Character::tight:   return tightKnee * cubicIntegral (x / tightKnee);
            case Character::fuzz:    return x >= 0.0 ? (x <= fuzzTop ? 0.5 * x * x : fuzzTop * x - 0.5 * fuzzTop * fuzzTop)
                                                     : fuzzBottom * fuzzBottom * logCosh (x / fuzzBottom);
        }

        return 0.5 * x * x;
    }

    /** First-order ADAA: the curve's average over the segment from the previous input. */
    inline double adaa (double x, double xPrev, Character c)
    {
        const double dx = x - xPrev;

        if (std::abs (dx) < 1.0e-6)
            return curve (0.5 * (x + xPrev), c);

        return (integral (x, c) - integral (xPrev, c)) / dx;
    }
}

/** A clipper whose character can change while it plays: the three curves crossfade over
    20 ms. Runs at the oversampled rate; keeps one sample of history per channel. */
class Clipper
{
public:
    void prepare (double oversampledRate)
    {
        for (auto& w : weights)
            prepareSmoothed (w, oversampledRate, 0.0f, 0.02);

        weights[1].setCurrentAndTargetValue (1.0f);
        reset();
    }

    void setCharacter (Character c)
    {
        for (int i = 0; i < 3; ++i)
            weights[i].setTargetValue (i == (int) c ? 1.0f : 0.0f);
    }

    void reset() noexcept
    {
        for (auto& w : weights)
            snap (w);

        xPrev[0] = xPrev[1] = 0.0;
    }

    /** Advance the crossfade by one sample; call once per sample before process(). */
    void tick() noexcept
    {
        for (int i = 0; i < 3; ++i)
            current[i] = weights[i].getNextValue();
    }

    float process (int channel, float in) noexcept
    {
        double y = 0.0;

        for (int i = 0; i < 3; ++i)
        {
            if (current[i] <= 0.0f)
                continue;

            y += current[i] * shaper::adaa (in, xPrev[channel], (Character) i);
        }

        xPrev[channel] = in;
        return (float) y;
    }

private:
    Smoothed weights[3];
    float current[3] { 0.0f, 1.0f, 0.0f };
    double xPrev[2] {};
};

/** Level after the clipper, so the drive knob changes the grit more than the loudness:
    a signal at `reference` (the expected band level) driven by `gain` saturates near 1;
    this brings it back to about `reference`. Unity at 0 dB of drive. */
inline float driveMakeup (float gain, float reference)
{
    const float a = gain * reference;
    return std::sqrt (1.0f + a * a) / std::max (1.0e-6f, gain);
}

//==============================================================================
/** Single-knob compressor: 0 is off, 100 is -36 dB threshold at 8:1 with a soft knee.
    Peak detector that holds each peak for 30 ms (longer than one cycle of a low B, so a
    held note reads steady instead of rippling at its own frequency) and then falls over
    100 ms; gain smoothing 2 ms attack / 150 ms release in dB, so pick attacks are caught;
    auto makeup level-matched at a -12 dBFS peak. Per sample; feed it the linked level. */
class Compressor
{
public:
    void prepare (double sampleRate)
    {
        holdSamples = (int) (0.030 * sampleRate);
        fall = std::exp (-1.0 / (0.100 * sampleRate));
        attack = coeff (0.002, sampleRate);
        release = coeff (0.150, sampleRate);
        prepareSmoothed (amount, sampleRate, 0.0f, 0.05);
        reset();
    }

    void reset() noexcept
    {
        snap (amount);
        peak = 0.0;
        holdLeft = 0;
        gainDb = 0.0;
    }

    void setAmount (float zeroToOne)        { amount.setTargetValue (juce::jlimit (0.0f, 1.0f, zeroToOne)); }
    float getReductionDb() const noexcept   { return (float) -gainDb; }

    /** The gain to apply to this sample, from the level of the detector signal. */
    float next (float detector) noexcept
    {
        const double a = amount.getNextValue();
        const double x = std::abs ((double) detector);

        if (x >= peak)
        {
            peak = x;
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            peak = std::max (x, peak * fall);
        }

        const double levelDb = 20.0 * std::log10 (peak + 1.0e-10);
        const double threshold = -36.0 * a, ratio = 1.0 + 7.0 * a, knee = 8.0;
        const double over = levelDb - threshold;
        const double slope = 1.0 / ratio - 1.0;
        double target = 0.0;

        if (2.0 * over > knee)
            target = slope * over;
        else if (2.0 * over > -knee)
            target = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);

        gainDb += (target - gainDb) * (target < gainDb ? attack : release);

        // Makeup: what the reduction would be for a -12 dBFS peak, so a bass at a normal
        // DI level comes out about as loud at any setting
        const double nominalOver = -12.0 - threshold;
        const double makeupDb = nominalOver > 0.0 ? nominalOver * (1.0 - 1.0 / ratio) : 0.0;
        return (float) std::pow (10.0, (gainDb + makeupDb) / 20.0);
    }

private:
    static double coeff (double seconds, double sr)    { return 1.0 - std::exp (-1.0 / (seconds * sr)); }

    Smoothed amount;
    double fall = 0.999, attack = 0.01, release = 0.001, peak = 0.0, gainDb = 0.0;
    int holdSamples = 1440, holdLeft = 0;
};

//==============================================================================
/** Noise gate with hysteresis (closes 6 dB under the threshold) and a 40 ms hold; opens
    in 1 ms, closes over ~60 ms. A threshold at or below -90 dB keeps it open. */
class Gate
{
public:
    void prepare (double sampleRate)
    {
        envRelease = std::exp (-1.0 / (0.02 * sampleRate));
        openStep = (float) (1.0 / (0.001 * sampleRate));
        closeCoeff = (float) (1.0 - std::exp (-1.0 / (0.06 * sampleRate)));
        holdSamples = (int) (0.04 * sampleRate);
        reset();
    }

    void reset() noexcept
    {
        env = 0.0;
        gain = isOff() ? 1.0f : 0.0f;
        open = isOff();
        holdLeft = 0;
    }

    void setThreshold (float db)
    {
        if (db != thresholdDb)
        {
            thresholdDb = db;
            threshold = dbToGain (db);
        }
    }

    bool isOff() const noexcept             { return thresholdDb <= -89.9f; }
    float getGain() const noexcept          { return gain; }

    float next (float detector) noexcept
    {
        env = std::max ((double) std::abs (detector), env * envRelease);

        if (isOff())
        {
            open = true;
        }
        else if (env >= threshold)
        {
            open = true;
            holdLeft = holdSamples;
        }
        else if (env < threshold * 0.5)
        {
            if (holdLeft > 0)
                --holdLeft;
            else
                open = false;
        }

        if (open)
            gain = std::min (1.0f, gain + openStep);
        else
            gain -= gain * closeCoeff;

        return gain;
    }

private:
    double env = 0.0, envRelease = 0.999, threshold = 0.0;
    float thresholdDb = -90.0f, gain = 1.0f, openStep = 0.02f, closeCoeff = 0.001f;
    int holdSamples = 1920, holdLeft = 0;
    bool open = true;
};

} // namespace bass::dsp
