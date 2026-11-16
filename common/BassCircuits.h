#pragma once

#include <JuceHeader.h>
#include "BassDsp.h"
#include <map>
#include <mutex>

/** Circuit-level models of the three drive characters, solved sample by sample at the
    oversampled rate. Unlike a fixed clipping curve these have memory: the clipping depends
    on frequency (capacitors in and around the clippers), loud playing shifts the operating
    point (coupling capacitors charge through grid conduction or a transistor's base) and
    recovers over tens of milliseconds, and the supply sags under load. That is what makes
    a real drive respond to how hard it is played.

    Units are volts: digital 1.0 is 1 V at the input, about what a bass pickup gives when
    dug into. Stiff parts (diodes conducting into tiny capacitors) are discretised with
    backward Euler, which is L-stable and cannot ring at Nyquist the way the trapezoidal
    rule does; the 4x oversampling keeps its extra damping far above the audio band.

    Each character's level is calibrated at prepare(): a reference-level bass tone is run
    through it at eleven drive settings and the makeup that keeps its RMS level with the
    input is tabulated, so turning drive up changes grit, not loudness. The tables are
    cached per sample rate and reference, so loading many instances costs nothing extra.
*/
namespace bass::circuit
{

/** e^x for the diode solvers: 2^(x log2 e), range-reduced, degree-7 polynomial (relative
    error ~2e-7, far below what moves a solved node voltage). Clamped to +-80. */
inline double fastExp (double x)
{
    x = juce::jlimit (-80.0, 80.0, x);
    const double t = x * 1.4426950408889634;
    double whole = (double) (long long) t;

    if (whole > t)
        whole -= 1.0;

    const double f = t - whole;
    const double p = 1.0 + f * (0.69314718055994531 + f * (0.24022650695910071 + f * (0.055504108664821580
                   + f * (0.0096181291076284772 + f * (0.0013333558146428443 + f * (1.5403530393381609e-4
                   + f * 1.5252733804059840e-5))))));

    // 2^whole straight into the exponent bits (whole is within +-116, always a normal double)
    const auto bits = (std::uint64_t) ((long long) whole + 1023) << 52;
    double scale;
    std::memcpy (&scale, &bits, sizeof (scale));
    return p * scale;
}

inline double fastTanh (double x)
{
    if (x > 9.0)  return 1.0;
    if (x < -9.0) return -1.0;
    const double e = fastExp (2.0 * x);
    return (e - 1.0) / (e + 1.0);
}

/** log(1 + e^x), a smooth max(0, x). */
inline double softplus (double x)
{
    return x > 30.0 ? x : std::log1p (fastExp (x));
}

//==============================================================================
/** Two diodes (or strings of them) back to back: Shockley's law each way. */
struct DiodePair
{
    double is = 2.52e-9;                          // saturation current, A
    double nvtPos = 1.752 * 0.02585;              // emission coefficient x thermal voltage x diodes in series
    double nvtNeg = 1.752 * 0.02585;

    /** 1N4148-class silicon: knee near 0.6 V per diode. */
    static DiodePair silicon (int seriesPos = 1, int seriesNeg = 1)
    {
        return { 2.52e-9, 1.752 * 0.02585 * seriesPos, 1.752 * 0.02585 * seriesNeg };
    }

    /** Germanium: a softer, lower knee near 0.25 V. */
    static DiodePair germanium (int seriesPos = 1, int seriesNeg = 1)
    {
        return { 2.0e-7, 1.3 * 0.02585 * seriesPos, 1.3 * 0.02585 * seriesNeg };
    }

    void eval (double v, double& current, double& conductance) const noexcept
    {
        // Past ~25 thermal voltages one side's diode is out by e^-25 and need not be computed
        const double ep = v < -25.0 * nvtPos ? 0.0 : fastExp (v / nvtPos);
        const double en = v > 25.0 * nvtNeg ? 0.0 : fastExp (-v / nvtNeg);
        current = is * (ep - en);
        conductance = is * (ep / nvtPos + en / nvtNeg);
    }
};

/** Solves c v + i_d(v) = q for v (c > 0): every backward-Euler diode node in this file
    reduces to that. F(v) = c v + i_d(v) - q is increasing, convex above 0 and concave
    below, so Newton from the far side of the root (where F has q's sign) slides down to
    it monotonically. It starts from the previous sample's answer, which at the
    oversampled rate is nearly always within a hair of the new root (one or two steps).
    A step from the near side can overshoot deep into the exponential, where Newton then
    crawls; any big outward step is capped at a closed-form point known to lie just past
    the root (the nearer of the linear-only and diode-only solutions), from where the
    descent is monotone and quick. */
inline double solveDiode (double c, double q, const DiodePair& d, double previous) noexcept
{
    if (q == 0.0)
        return 0.0;

    const bool positive = q > 0.0;
    double v = (previous > 0.0) == positive ? previous : 0.0;
    double bound = 0.0, i = 0.0, g = 0.0;
    bool haveBound = false;

    for (int iteration = 0; iteration < 40; ++iteration)
    {
        d.eval (v, i, g);
        double next = v - (c * v + i - q) / (c + g);

        if (positive ? next > v + 0.05 : next < v - 0.05)
        {
            if (! haveBound)
            {
                // The diode-only solution is always past the root. The linear one is too for
                // matched diodes, but with unequal strings (germanium 1 + 2) the pair is convex
                // on both sides of 0 near it, and the linear point can land short of the root,
                // where capping at it would bounce forever: use it only when F says it's past
                const double nvt = positive ? d.nvtPos : d.nvtNeg;
                const double linear = q / (c + d.is * (1.0 / d.nvtPos + 1.0 / d.nvtNeg));
                const double diodeOnly = (positive ? nvt : -nvt) * std::log1p (std::abs (q) / d.is);
                bound = diodeOnly;

                if (std::abs (linear) < std::abs (diodeOnly))
                {
                    double il = 0.0, gl = 0.0;
                    d.eval (linear, il, gl);

                    if ((c * linear + il - q > 0.0) == positive)
                        bound = linear;
                }

                haveBound = true;
            }

            if (positive ? next > bound : next < bound)
                next = bound;
        }

        const double step = next - v;
        v = next;

        if (std::abs (step) < 1.0e-7)    // 0.1 uV: -140 dB below a volt
            break;
    }

    return v;
}

//==============================================================================
/** A first-order envelope: fast up, slow down. */
class Envelope
{
public:
    void prepare (double sampleRate, double attackSeconds, double releaseSeconds)
    {
        up = 1.0 - std::exp (-1.0 / (attackSeconds * sampleRate));
        down = 1.0 - std::exp (-1.0 / (releaseSeconds * sampleRate));
        reset();
    }

    void reset() noexcept                   { level = 0.0; }
    double get() const noexcept             { return level; }

    void push (double x) noexcept
    {
        const double a = std::abs (x);
        level += (a - level) * (a > level ? up : down);
    }

private:
    double up = 0.01, down = 0.001, level = 0.0;
};

/** Non-inverting op-amp gain stage with antiparallel diodes and a capacitor across its
    feedback resistor: the soft, "clean underneath" clipping of an overdrive. The ground
    leg (Rg in series with Cg) sets which lows are amplified; Cf rolls the gain off up top,
    more as Rf (the drive) rises, so highs clip less than lows. Output = in + feedback
    voltage, through the op-amp's +-4.5 V rails (9 V supply). */
class OpAmpClipper
{
public:
    static constexpr double rg = 4700.0, cg = 2.2e-6, cf = 100.0e-12, rails = 4.5;

    void prepare (double sampleRate)
    {
        dt = 1.0 / sampleRate;
        groundLeg = dsp::Coeffs::highpass1 (1.0 / (juce::MathConstants<double>::twoPi * rg * cg), sampleRate);
        diodes = DiodePair::silicon();
        reset();
    }

    void reset() noexcept                   { vf = 0.0; groundState.reset(); }
    void setFeedbackResistance (double r)   { rf = r; }

    double process (double vin) noexcept
    {
        const double ig = groundState.process (vin, groundLeg) / rg;
        const double cdt = cf / dt;
        vf = solveDiode (cdt + 1.0 / rf, cdt * vf + ig, diodes, vf);
        return rails * fastTanh ((vin + vf) / rails);
    }

private:
    double dt = 1.0 / 192000.0, rf = 4700.0, vf = 0.0;
    dsp::Coeffs groundLeg;
    dsp::Biquad groundState;
    DiodePair diodes;
};

/** Series resistor into diodes and a capacitor to ground: the hard clipper of a
    distortion pedal. The capacitor makes the clipping frequency-dependent and gives the
    node a memory. */
class DiodeClipper
{
public:
    void prepare (double sampleRate, double resistance, double capacitance, DiodePair d)
    {
        dt = 1.0 / sampleRate;
        r = resistance;
        c = capacitance;
        diodes = d;
        reset();
    }

    void reset() noexcept                   { v = 0.0; }

    double process (double vin) noexcept
    {
        const double cdt = c / dt;
        v = solveDiode (cdt + 1.0 / r, cdt * v + vin / r, diodes, v);
        return v;
    }

private:
    double dt = 1.0 / 192000.0, r = 1000.0, c = 4.7e-9, v = 0.0;
    DiodePair diodes;
};

/** A triode gain stage, behaviourally: a 3/2-power plate law with a smooth cutoff below the
    bias point, grid conduction that soft-clamps positive swings, and the coupling
    capacitor that grid current charges, pushing the bias colder on loud passages
    (compression and the slight "blocking" sag of a driven valve), recovering over
    ~60 ms. Inverting, like the real stage; output about -0.3..+1. */
class TriodeStage
{
public:
    static constexpr double bias = 1.5, knee = 8.0, gridClamp = 0.9, charge = 12.0, recovery = 0.06;

    void prepare (double sampleRate)        { dt = 1.0 / sampleRate; reset(); }
    void reset() noexcept                   { vc = 0.0; }

    double process (double x) noexcept
    {
        const double vg = x - vc;
        const double conduct = softplus (knee * vg) / knee;            // grid current, smoothly from 0 V
        const double grid = vg - gridClamp * conduct;                    // the clamp
        vc = (vc + dt * charge * conduct) / (1.0 + dt / recovery);      // bias shift, backward Euler
        const double e = softplus (knee * (grid + bias)) / knee;        // smooth cutoff
        return -(e * std::sqrt (e) - quiescent) / quiescent;
    }

private:
    static inline const double quiescent = bias * std::sqrt (bias);
    double dt = 1.0 / 192000.0, vc = 0.0;
};

/** A transistor fuzz stage: collector current follows the base-emitter exponential from a
    bias near the knee, saturating softly on top and cutting off hard below, which is
    strongly asymmetric. Positive swings charge the coupling network and starve the bias,
    so as a note decays the stage drops toward cutoff and sputters, the way a fuzz gates. */
class FuzzStage
{
public:
    static constexpr double ceiling = 5.0, starveRate = 40.0, starveRecovery = 0.04;

    void prepare (double sampleRate)        { dt = 1.0 / sampleRate; reset(); }
    void reset() noexcept                   { starve = 0.0; }

    double process (double u) noexcept
    {
        const double ic = fastExp (juce::jlimit (-30.0, 12.0, u - starve));
        const double y = ceiling * fastTanh (ic / ceiling) - quiescent;
        starve = (starve + dt * starveRate * std::max (0.0, y)) / (1.0 + dt / starveRecovery);
        return y;
    }

private:
    static inline const double quiescent = ceiling * std::tanh (1.0 / ceiling);
    double dt = 1.0 / 192000.0, starve = 0.0;
};

//==============================================================================
/** The three characters as full circuits, one per channel. drive is 0..1. */
struct TightCircuit
{
    void prepare (double sr)
    {
        opamp.prepare (sr);
        clipper.prepare (sr, 1000.0, 2.2e-9, DiodePair::silicon());
        sag.prepare (sr, 0.005, 0.12);     // a regulated 9 V pedal: hardly any sag
    }

    void reset()                            { opamp.reset(); clipper.reset(); sag.reset(); }

    void setDrive (double d)
    {
        opamp.setFeedbackResistance (4700.0 * std::pow (100.0, d));    // gain 2 .. 101
        gain2 = 1.0 + 9.0 * d;                                           // second stage 1 .. 10
    }

    double process (double x) noexcept
    {
        const double in = 0.5 * x / (1.0 + 0.15 * sag.get());
        double v = opamp.process (in);
        v = OpAmpClipper::rails * fastTanh (v * gain2 / OpAmpClipper::rails);
        v = clipper.process (v);
        sag.push (v);
        return v;
    }

    OpAmpClipper opamp;
    DiodeClipper clipper;
    Envelope sag;
    double gain2 = 1.0;
};

struct SmoothCircuit
{
    void prepare (double sr)
    {
        first.prepare (sr);
        second.prepare (sr);
        coupling = dsp::Coeffs::highpass1 (20.0, sr);
        sag.prepare (sr, 0.010, 0.25);     // a valve supply: deep, slow sag (the spongy bloom after a hard hit)
    }

    void reset()                            { first.reset(); second.reset(); couplingState.reset(); sag.reset(); }
    void setDrive (double d)                { gain = std::pow (40.0, d); }   // 1 .. 40 into the first grid

    /** How far the sagging supply lowers the plate's swing as well as the drive into the grid. Without it a soft
        note right after a hard one came out louder, not softer: the coupling capacitors' colder bias stops it
        reaching grid conduction, so it's clamped less, and less drive into a stage that's clipping anyway barely
        shows. With it the soft note starts compressed and recovers as the supply does (check.ps1's memory line). */
    static constexpr double plateSag = 0.5;

    double process (double x) noexcept
    {
        const double load = sag.get();
        const double in = gain * x / (1.0 + 0.8 * load);
        double v = first.process (in);
        v = couplingState.process (v, coupling) * 3.0;
        v = second.process (v);
        sag.push (v);
        return v / (1.0 + plateSag * load);
    }

    TriodeStage first, second;
    dsp::Coeffs coupling;
    dsp::Biquad couplingState;
    Envelope sag;
    double gain = 1.0;
};

struct FuzzCircuit
{
    void prepare (double sr)
    {
        stage.prepare (sr);
        coupling = dsp::Coeffs::highpass1 (10.0, sr);
        clipper.prepare (sr, 1000.0, 2.2e-9, DiodePair::germanium (1, 2));
        sag.prepare (sr, 0.005, 0.15);
    }

    void reset()                            { stage.reset(); couplingState.reset(); clipper.reset(); sag.reset(); }
    void setDrive (double d)                { gain = 2.0 * std::pow (40.0, d); }   // 2 .. 80

    double process (double x) noexcept
    {
        const double u = gain * x / (1.0 + 0.5 * sag.get());
        double v = stage.process (u);
        v = couplingState.process (v, coupling);
        v = clipper.process (0.35 * v);
        sag.push (v);
        return v;
    }

    FuzzStage stage;
    dsp::Coeffs coupling;
    dsp::Biquad couplingState;
    DiodeClipper clipper;
    Envelope sag;
    double gain = 2.0;
};

//==============================================================================
/** The drive section: all three circuits per channel, crossfading over 20 ms when the
    character changes (a circuit that fades in starts from rest), with its level
    calibrated so `reference`-level input comes out at the same RMS at every drive. */
class CircuitDrive
{
public:
    static constexpr int numCharacters = 3, calibrationPoints = 11;
    using Table = std::array<std::array<float, calibrationPoints>, numCharacters>;

    void prepare (double oversampledRate, float referenceLevel)
    {
        rate = oversampledRate;
        table = calibrate (rate, referenceLevel);

        for (auto& w : weights)
            dsp::prepareSmoothed (w, rate, 0.0f, 0.02);

        weights[1].setCurrentAndTargetValue (1.0f);
        dsp::prepareSmoothed (drive, rate, 0.5f, 0.03);

        for (int ch = 0; ch < 2; ++ch)
        {
            tight[ch].prepare (rate);
            smooth[ch].prepare (rate);
            fuzz[ch].prepare (rate);
        }

        reset();
    }

    void reset()
    {
        for (auto& w : weights)
            dsp::snap (w);

        dsp::snap (drive);

        for (int c = 0; c < numCharacters; ++c)
        {
            current[c] = weights[c].getCurrentValue();
            running[c] = current[c] > 0.0f;
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            tight[ch].reset();
            smooth[ch].reset();
            fuzz[ch].reset();
        }

        builtDrive = -1.0f;
        applyDrive (drive.getCurrentValue());
    }

    void setCharacter (dsp::Character c)
    {
        for (int i = 0; i < numCharacters; ++i)
            weights[i].setTargetValue (i == (int) c ? 1.0f : 0.0f);
    }

    void setDrive (float zeroToOne)         { drive.setTargetValue (juce::jlimit (0.0f, 1.0f, zeroToOne)); }

    /** Once per oversampled sample, before process() for each channel. */
    void tick() noexcept
    {
        for (int c = 0; c < numCharacters; ++c)
        {
            current[c] = weights[c].getNextValue();
            const bool wanted = current[c] > 0.0f || weights[c].getTargetValue() > 0.0f;

            if (wanted && ! running[c])
                for (int ch = 0; ch < 2; ++ch)
                    resetCircuit (c, ch);

            running[c] = wanted;
        }

        const float d = drive.getNextValue();

        if (d != builtDrive)
            applyDrive (d);
    }

    float process (int channel, float x) noexcept
    {
        double y = 0.0;

        if (running[0]) y += current[0] * makeup[0] * smooth[channel].process (x);
        if (running[1]) y += current[1] * makeup[1] * tight[channel].process (x);
        if (running[2]) y += current[2] * makeup[2] * fuzz[channel].process (x);

        return (float) y;
    }

    /** Mono in stereo (a DI'd bass on both channels): the caller runs only channel 0 while
        the two are bit-identical, and calls this the moment they part, so channel 1 picks
        up from exactly where channel 0 is. */
    void copyChannel0To1() noexcept
    {
        smooth[1] = smooth[0];
        tight[1] = tight[0];
        fuzz[1] = fuzz[0];
    }

    /** Runs a reference-level bass tone through fresh circuits at eleven drive settings and
        tabulates the makeup (dB) that brings each back to the input's RMS. Cached. */
    static Table calibrate (double oversampledRate, float referenceLevel)
    {
        static std::mutex lock;
        static std::map<std::pair<long long, int>, Table> cache;
        const std::pair<long long, int> key { (long long) std::llround (oversampledRate), (int) std::lround (referenceLevel * 1.0e5f) };

        {
            const std::lock_guard<std::mutex> guard (lock);

            if (auto found = cache.find (key); found != cache.end())
                return found->second;
        }

        Table result {};
        const int warm = (int) (0.06 * oversampledRate), measured = (int) (0.10 * oversampledRate);

        // 55 Hz with three harmonics and a touch of 440 Hz: a plucked A string's body
        std::vector<double> tone ((size_t) (warm + measured));
        double peak = 0.0;

        for (size_t i = 0; i < tone.size(); ++i)
        {
            const double t = (double) i / oversampledRate, w = juce::MathConstants<double>::twoPi * 55.0 * t;
            tone[i] = std::sin (w) + 0.5 * std::sin (2.0 * w + 0.3) + 0.3 * std::sin (3.0 * w + 0.7) + 0.12 * std::sin (8.0 * w + 1.1);
            peak = std::max (peak, std::abs (tone[i]));
        }

        for (auto& v : tone)
            v *= referenceLevel / peak;

        double inSq = 0.0;

        for (int i = warm; i < warm + measured; ++i)
            inSq += tone[(size_t) i] * tone[(size_t) i];

        auto run = [&] (auto& circuit, double d)
        {
            circuit.prepare (oversampledRate);
            circuit.setDrive (d);
            double outSq = 0.0;

            for (int i = 0; i < warm + measured; ++i)
            {
                const double y = circuit.process (tone[(size_t) i]);

                if (i >= warm)
                    outSq += y * y;
            }

            return (float) (10.0 * std::log10 (inSq / std::max (1.0e-30, outSq)));
        };

        for (int k = 0; k < calibrationPoints; ++k)
        {
            const double d = (double) k / (calibrationPoints - 1);
            SmoothCircuit s;
            TightCircuit t;
            FuzzCircuit f;
            result[0][(size_t) k] = run (s, d);
            result[1][(size_t) k] = run (t, d);
            result[2][(size_t) k] = run (f, d);
        }

        const std::lock_guard<std::mutex> guard (lock);
        cache[key] = result;
        return result;
    }

private:
    void resetCircuit (int c, int ch)
    {
        if (c == 0) smooth[ch].reset();
        if (c == 1) tight[ch].reset();
        if (c == 2) fuzz[ch].reset();
    }

    void applyDrive (float d)
    {
        builtDrive = d;

        for (int ch = 0; ch < 2; ++ch)
        {
            smooth[ch].setDrive (d);
            tight[ch].setDrive (d);
            fuzz[ch].setDrive (d);
        }

        const float pos = d * (calibrationPoints - 1);
        const int k = juce::jlimit (0, calibrationPoints - 2, (int) pos);
        const float t = pos - (float) k;

        for (int c = 0; c < numCharacters; ++c)
            makeup[c] = (float) juce::Decibels::decibelsToGain (table[(size_t) c][(size_t) k] * (1.0f - t)
                                                                + table[(size_t) c][(size_t) k + 1] * t);
    }

    double rate = 192000.0;
    Table table {};
    TightCircuit tight[2];
    SmoothCircuit smooth[2];
    FuzzCircuit fuzz[2];
    dsp::Smoothed weights[numCharacters], drive;
    float current[numCharacters] { 0.0f, 1.0f, 0.0f }, makeup[numCharacters] { 1.0f, 1.0f, 1.0f };
    bool running[numCharacters] { false, true, false };
    float builtDrive = -1.0f;
};

} // namespace bass::circuit
