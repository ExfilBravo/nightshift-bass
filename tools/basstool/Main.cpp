#include <JuceHeader.h>
#include "Measure.h"

/** basstool: loads a built VST3 the way a DAW does, then renders, validates and
    measures it from the command line, so the plugins can be judged without ears.

      basstool --signals <folder>                              write the test signals
      basstool --params <plugin.vst3>                          list parameters
      basstool --render <plugin.vst3> <in.wav> <out.wav> [--set Name=value]... [--block 512] [--tail 2]
      basstool --validate <plugin.vst3>                        sanity suite, exit 1 on any FAIL
      basstool --measure <wav> [--stats] [--pitch] [--thd 1000] [--decay] [--bands] [--response [--ref dry.wav]]
*/
namespace
{

struct Args
{
    explicit Args (const juce::String& commandLine) : tokens (juce::StringArray::fromTokens (commandLine, true)) {}

    bool has (const juce::String& flag) const           { return tokens.contains (flag); }

    juce::String after (const juce::String& flag, int offset = 1) const
    {
        const int i = tokens.indexOf (flag);
        return i >= 0 && i + offset < tokens.size() ? tokens[i + offset].unquoted() : juce::String();
    }

    juce::StringArray allAfter (const juce::String& flag) const
    {
        juce::StringArray out;

        for (int i = 0; i < tokens.size(); ++i)
            if (tokens[i] == flag && i + 1 < tokens.size())
                out.add (tokens[i + 1].unquoted());

        return out;
    }

    juce::StringArray tokens;
};

void say (const juce::String& s)     { std::cout << s << std::endl; }

//==============================================================================
bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sr)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                 .withSampleRate (sr)
                                                 .withNumChannels (buffer.getNumChannels())
                                                 .withBitsPerSample (24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

bool readWav (const juce::File& file, juce::AudioBuffer<float>& buffer, double& sr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));

    if (reader == nullptr)
        return false;

    sr = reader->sampleRate;
    buffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    return reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
}

//==============================================================================
int writeSignals (const juce::File& folder)
{
    folder.createDirectory();
    const double sr = 48000.0;
    const int seconds = 5;
    const int n = (int) sr * seconds;
    bool ok = true;

    auto make = [&] (const juce::String& name, int length, std::function<float (int)> fn)
    {
        juce::AudioBuffer<float> b (2, length);

        for (int i = 0; i < length; ++i)
        {
            const float v = fn (i);
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }

        ok = writeWav (folder.getChildFile (name), b, sr) && ok;
        say ("wrote " + name);
    };

    // The phase stays double until after sin(): cast before it and float resolution at
    // 30 000 rad adds phase noise around -70 dB, which would be blamed on the plugins
    const float minus12 = juce::Decibels::decibelsToGain (-12.0f);
    auto tone = [&] (double hz) { return [=] (int i) { return minus12 * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr); }; };
    make ("sine1k.wav", n, tone (1000.0));
    make ("sine440.wav", n, tone (440.0));
    make ("sine100.wav", n, tone (100.0));
    make ("sine5k.wav", n, tone (5000.0));

    // Exponential sweep 20 Hz .. 20 kHz at -12 dBFS, with fades
    make ("sweep.wav", n, [&] (int i)
    {
        const double t = i / sr, T = seconds;
        const double f0 = 20.0, f1 = 20000.0;
        const double phase = juce::MathConstants<double>::twoPi * f0 * T / std::log (f1 / f0) * (std::exp (t / T * std::log (f1 / f0)) - 1.0);
        const float fade = (float) std::min ({ 1.0, i / (sr * 0.05), (n - i) / (sr * 0.05) });
        return minus12 * fade * (float) std::sin (phase);
    });

    juce::Random rng (42);
    const float minus20 = juce::Decibels::decibelsToGain (-20.0f);
    make ("noise.wav", n, [&] (int) { return minus20 * (rng.nextFloat() * 2.0f - 1.0f); });
    make ("silence.wav", n, [] (int) { return 0.0f; });
    make ("impulse.wav", (int) sr * 3, [&] (int i) { return i == (int) (sr * 0.1) ? 0.9f : 0.0f; });
    make ("square100.wav", n, [&] (int i) { return std::fmod (i / sr * 100.0, 1.0) < 0.5 ? 0.99f : -0.99f; });

    // Bass registers: open E (41.2 Hz), E an octave up, and a tone high enough to show
    // aliasing. 1490 Hz divides neither 48 kHz nor the 192 kHz oversampled rate, so every
    // alias lands between the harmonics, where --alias can see it
    make ("sine41.wav", n, tone (41.2034));
    make ("sine82.wav", n, tone (82.4069));
    make ("sine1490.wav", n, tone (1490.0));

    // For the gate: a second of a bass note, then two seconds of hiss at -70 dBFS
    {
        juce::Random hiss (44);
        const float minus70 = juce::Decibels::decibelsToGain (-70.0f);
        make ("gatetest.wav", (int) sr * 3, [&] (int i)
        {
            const float noise = minus70 * 1.7320508f * (hiss.nextFloat() * 2.0f - 1.0f);   // uniform, rms -70
            return i < (int) sr ? minus12 * (float) std::sin (juce::MathConstants<double>::twoPi * 82.4069 * i / sr) + noise : noise;
        });
    }

    // For the circuits' memory: a second of a hard-played E (-6 dBFS), then two seconds of a
    // soft one (-26 dBFS). A real drive compresses the soft note at first and recovers as
    // its coupling capacitors and supply settle; a fixed curve treats it the same throughout
    {
        const float minus6 = juce::Decibels::decibelsToGain (-6.0f), minus26 = juce::Decibels::decibelsToGain (-26.0f);
        make ("burst.wav", (int) sr * 3, [&] (int i)
        {
            const float level = i < (int) sr ? minus6 : minus26;
            return level * (float) std::sin (juce::MathConstants<double>::twoPi * 82.4069 * i / sr);
        });

        // The same soft note with nothing before it: the reference the burst is compared to
        make ("softalone.wav", (int) sr * 3, [&] (int i)
        {
            return i < (int) sr ? 0.0f : minus26 * (float) std::sin (juce::MathConstants<double>::twoPi * 82.4069 * i / sr);
        });
    }

    // Quiet white noise, for responses through paths that must stay linear
    const float minus30 = juce::Decibels::decibelsToGain (-30.0f);
    juce::Random rng30 (43);
    make ("noise30.wav", n, [&] (int) { return minus30 * (rng30.nextFloat() * 2.0f - 1.0f); });

    // A bass line like a DI'd fingerstyle bass: ideal plucked strings (pluck point 0.18 of the
    // length, harmonics falling as 1/n^2 with faster decay up the series) plus a 2 ms finger
    // click, eight notes from open E up, peaks at -8 dBFS
    {
        const double notes[] = { 41.2034, 41.2034, 55.0, 61.7354, 73.4162, 55.0, 82.4069, 97.9989 };
        const int noteLength = (int) (sr * 0.6);
        const int total = noteLength * (int) std::size (notes) + (int) (sr * 0.2);
        std::vector<double> line ((size_t) total, 0.0);
        juce::Random clickNoise (11);

        for (size_t k = 0; k < std::size (notes); ++k)
        {
            const double f = notes[k];
            const int start = (int) k * noteLength;

            for (int i = 0; i < noteLength + (int) (sr * 0.1) && start + i < total; ++i)
            {
                const double t = i / sr;
                const double release = i < noteLength ? 1.0 : std::exp (-(i - noteLength) / (sr * 0.015));
                double v = 0.0;

                for (int h = 1; h <= 60 && f * h < sr * 0.45; ++h)
                {
                    const double amp = std::sin (h * juce::MathConstants<double>::pi * 0.18) / (h * h);
                    v += amp * std::exp (-t * (0.8 + 0.45 * h)) * std::sin (juce::MathConstants<double>::twoPi * f * h * t);
                }

                if (i < (int) (sr * 0.002))
                    v += 0.08 * (clickNoise.nextDouble() * 2.0 - 1.0) * (1.0 - i / (sr * 0.002));

                line[(size_t) (start + i)] += v * release;
            }
        }

        double peak = 0.0;

        for (auto v : line)
            peak = std::max (peak, std::abs (v));

        const double scale = juce::Decibels::decibelsToGain (-8.0) / peak;
        make ("bassdi.wav", total, [&] (int i) { return (float) (line[(size_t) i] * scale); });
    }

    return ok ? 0 : 1;
}

//==============================================================================
struct Host
{
    Host()
    {
        juce::addDefaultFormatsToManager (formats);
    }

    std::unique_ptr<juce::AudioPluginInstance> load (const juce::File& vst3, double sr, int block, juce::String& error)
    {
        juce::OwnedArray<juce::PluginDescription> found;

        for (auto* f : formats.getFormats())
            f->findAllTypesForFile (found, vst3.getFullPathName());

        if (found.isEmpty())
        {
            error = "no plugin found in " + vst3.getFullPathName();
            return nullptr;
        }

        auto instance = formats.createPluginInstance (*found[0], sr, block, error);

        if (instance != nullptr)
        {
            instance->enableAllBuses();
            instance->setPlayConfigDetails (2, 2, sr, block);
            instance->setNonRealtime (true);
            instance->setPlayHead (&playing);
            instance->prepareToPlay (sr, block);
        }

        return instance;
    }

    /** A transport that is always playing, as a DAW's is while it renders. The plugins make
        their own noise (hiss, surface, crackle) only while the host plays; with no transport
        at all, VST3 tells them it's stopped. */
    struct Playing : public juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (true);
            return info;
        }
    } playing;

    static juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, const juce::String& nameOrId)
    {
        for (auto* param : p.getParameters())
        {
            if (param->getName (64).equalsIgnoreCase (nameOrId))
                return param;

            if (auto* hosted = dynamic_cast<juce::AudioPluginInstance::HostedParameter*> (param))
                if (hosted->getParameterID().equalsIgnoreCase (nameOrId))
                    return param;
        }

        return nullptr;
    }

    /** "Name=value" in the parameter's own units, or a choice's text. */
    static bool set (juce::AudioPluginInstance& p, const juce::String& assignment, juce::String& error)
    {
        const auto name = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto text = assignment.fromFirstOccurrenceOf ("=", false, false).trim();
        auto* param = findParam (p, name);

        if (param == nullptr)
        {
            error = "no parameter called '" + name + "'";
            return false;
        }

        param->setValueNotifyingHost (param->getValueForText (text));
        return true;
    }

    /** Runs the whole buffer through the plugin in blocks, with a per-block hook. */
    static void process (juce::AudioPluginInstance& p, juce::AudioBuffer<float>& audio, int block,
                         const std::function<void (int blockStart)>& beforeBlock = nullptr)
    {
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> chunk (2, block);
        const int n = audio.getNumSamples();

        for (int start = 0; start < n; start += block)
        {
            const int len = std::min (block, n - start);

            if (beforeBlock != nullptr)
                beforeBlock (start);

            for (int ch = 0; ch < 2; ++ch)
                chunk.copyFrom (ch, 0, audio, std::min (ch, audio.getNumChannels() - 1), start, len);

            juce::AudioBuffer<float> view (chunk.getArrayOfWritePointers(), 2, len);
            p.processBlock (view, midi);

            for (int ch = 0; ch < 2; ++ch)
                audio.copyFrom (ch, start, chunk, ch, 0, len);
        }
    }

    juce::AudioPluginFormatManager formats;
};

juce::AudioBuffer<float> stereo (const juce::AudioBuffer<float>& in)
{
    juce::AudioBuffer<float> out (2, in.getNumSamples());

    for (int ch = 0; ch < 2; ++ch)
        out.copyFrom (ch, 0, in, std::min (ch, in.getNumChannels() - 1), 0, in.getNumSamples());

    return out;
}

juce::AudioBuffer<float> sine (double sr, double hz, double seconds, float gain = 0.25f)
{
    juce::AudioBuffer<float> b (2, (int) (sr * seconds));

    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float v = gain * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }

    return b;
}

float maxDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    float d = 0.0f;

    for (int ch = 0; ch < std::min (a.getNumChannels(), b.getNumChannels()); ++ch)
        for (int i = 0; i < std::min (a.getNumSamples(), b.getNumSamples()); ++i)
            d = std::max (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

    return d;
}

//==============================================================================
int listParams (Host& host, const juce::File& vst3)
{
    juce::String error;
    auto p = host.load (vst3, 48000.0, 512, error);

    if (p == nullptr)
    {
        say ("ERROR " + error);
        return 1;
    }

    say (p->getName() + "  (latency " + juce::String (p->getLatencySamples()) + " samples, tail " + juce::String (p->getTailLengthSeconds(), 2) + " s)");

    for (auto* param : p->getParameters())
    {
        juce::String id;

        if (auto* hosted = dynamic_cast<juce::AudioPluginInstance::HostedParameter*> (param))
            id = hosted->getParameterID();

        say ("  " + param->getName (64).paddedRight (' ', 14) + " id=" + id.paddedRight (' ', 12)
             + " default=" + param->getText (param->getDefaultValue(), 32).paddedRight (' ', 16)
             + " min=" + param->getText (0.0f, 32).paddedRight (' ', 14) + " max=" + param->getText (1.0f, 32)
             + (param->isDiscrete() ? "  (" + juce::String (param->getNumSteps()) + " steps)" : juce::String()));
    }

    return 0;
}

int render (Host& host, const Args& args)
{
    const juce::File vst3 (args.after ("--render", 1)), in (args.after ("--render", 2)), out (args.after ("--render", 3));
    const int block = std::max (16, args.has ("--block") ? args.after ("--block").getIntValue() : 512);
    const double tail = args.has ("--tail") ? args.after ("--tail").getDoubleValue() : 0.0;

    juce::AudioBuffer<float> input;
    double sr = 48000.0;

    if (! readWav (in, input, sr))
    {
        say ("ERROR can't read " + in.getFullPathName());
        return 1;
    }

    juce::String error;
    auto p = host.load (vst3, sr, block, error);

    if (p == nullptr)
    {
        say ("ERROR " + error);
        return 1;
    }

    for (auto& assignment : args.allAfter ("--set"))
        if (! Host::set (*p, assignment, error))
        {
            say ("ERROR " + error);
            return 1;
        }

    auto audio = stereo (input);
    audio.setSize (2, audio.getNumSamples() + (int) (tail * sr), true, true);
    Host::process (*p, audio, block);

    if (! writeWav (out, audio, sr))
    {
        say ("ERROR can't write " + out.getFullPathName());
        return 1;
    }

    say ("rendered " + out.getFullPathName() + "  (" + measure::describe (measure::stats (audio)) + ")");
    return 0;
}

//==============================================================================
int validate (Host& host, const juce::File& vst3)
{
    int fails = 0;
    auto result = [&] (bool pass, const juce::String& what, const juce::String& detail = {})
    {
        if (! pass)
            ++fails;

        say (juce::String (pass ? "PASS  " : "FAIL  ") + what + (detail.isNotEmpty() ? "  -  " + detail : juce::String()));
    };
    auto info = [&] (const juce::String& what) { say ("INFO  " + what); };
    auto warn = [&] (const juce::String& what) { say ("WARN  " + what); };

    juce::String error;

    // Loads and runs at the usual sample rates
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        auto p = host.load (vst3, sr, 512, error);

        if (p == nullptr)
        {
            result (false, "load at " + juce::String ((int) sr) + " Hz", error);
            continue;
        }

        auto audio = sine (sr, 1000.0, 1.0);
        Host::process (*p, audio, 512);
        auto s = measure::stats (audio);
        result (s.finite && s.peakDb < 12.0, "runs at " + juce::String ((int) sr) + " Hz", measure::describe (s));

        if (sr == 48000.0)
            info ("latency " + juce::String (p->getLatencySamples()) + " samples, tail " + juce::String (p->getTailLengthSeconds(), 2) + " s, "
                  + juce::String (p->getParameters().size()) + " parameters");
    }

    const double sr = 48000.0;

    // Determinism: two fresh instances give identical output
    {
        auto a = host.load (vst3, sr, 512, error), b = host.load (vst3, sr, 512, error);

        if (a != nullptr && b != nullptr)
        {
            auto x = sine (sr, 440.0, 2.0), y = sine (sr, 440.0, 2.0);
            Host::process (*a, x, 512);
            Host::process (*b, y, 512);
            const float d = maxDifference (x, y);

            if (d == 0.0f)
                result (true, "deterministic across instances");
            else
                warn ("two instances differ by up to " + juce::String (juce::Decibels::gainToDecibels (d), 1) + " dBFS (seed noise from prepareToPlay to fix)");
        }
    }

    // Block-size invariance: 512 vs 64 sample blocks agree
    {
        auto a = host.load (vst3, sr, 512, error), b = host.load (vst3, sr, 64, error);

        if (a != nullptr && b != nullptr)
        {
            auto x = sine (sr, 440.0, 2.0), y = sine (sr, 440.0, 2.0);
            Host::process (*a, x, 512);
            Host::process (*b, y, 64);
            const float d = maxDifference (x, y);
            result (d < 1e-3f, "block-size invariant (512 vs 64)", "max difference " + juce::String (juce::Decibels::gainToDecibels (d, -200.0f), 1) + " dBFS");
        }
    }

    // Bypass passes the input through untouched
    {
        auto p = host.load (vst3, sr, 512, error);

        if (p != nullptr)
        {
            if (auto* bypass = p->getBypassParameter())
            {
                // Bypassed, the input must come out untouched but late by the reported
                // latency, so a host's delay compensation stays right
                bypass->setValueNotifyingHost (1.0f);
                auto x = sine (sr, 440.0, 1.0);
                const auto y = x;
                Host::process (*p, x, 512);

                const int latency = p->getLatencySamples();
                float d = 0.0f;

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i + latency < x.getNumSamples(); ++i)
                        d = std::max (d, std::abs (x.getSample (ch, i + latency) - y.getSample (ch, i)));

                result (d < 1e-6f, "bypass is transparent (latency-aligned)",
                        "max difference " + juce::String (juce::Decibels::gainToDecibels (d, -200.0f), 1) + " dBFS after " + juce::String (latency) + " samples");
            }
            else
            {
                result (false, "has a bypass parameter");
            }
        }
    }

    // Toggling bypass on and off while playing must not click
    {
        auto p = host.load (vst3, sr, 512, error);

        if (p != nullptr && p->getBypassParameter() != nullptr)
        {
            auto* bypass = p->getBypassParameter();
            auto x = sine (sr, 440.0, 3.0);
            const int onAt = (int) sr, offAt = (int) sr * 2;

            Host::process (*p, x, 512, [&] (int start)
            {
                if (start <= onAt && onAt < start + 512)    bypass->setValueNotifyingHost (1.0f);
                if (start <= offAt && offAt < start + 512)  bypass->setValueNotifyingHost (0.0f);
            });

            auto* d = x.getReadPointer (0);
            auto maxStep = [&] (int from, int to)
            {
                float m = 0.0f;

                for (int i = std::max (1, from); i < std::min (x.getNumSamples(), to); ++i)
                    m = std::max (m, std::abs (d[i] - d[i - 1]));

                return m;
            };

            const int window = (int) (sr * 0.1);
            const float steadyOn = maxStep (onAt - 2 * window, onAt), steadyOff = maxStep (offAt - 2 * window, offAt);
            const float atOn = maxStep (onAt, onAt + window), atOff = maxStep (offAt, offAt + window);

            // A toggle crossfades between the processed and the bypassed signal, so its slope
            // may reach the steeper of the two (a distortion's output is steeper than its dry
            // input); a click is a step beyond both
            const float reference = std::max (steadyOn, steadyOff);
            const bool pass = atOn <= reference * 1.5f + 0.02f && atOff <= reference * 1.5f + 0.02f;

            result (pass, "bypass toggles without clicks",
                    "largest step entering bypass " + juce::String (atOn, 3) + " (steady " + juce::String (steadyOn, 3) + "), leaving "
                    + juce::String (atOff, 3) + " (steady " + juce::String (steadyOff, 3) + ")");
        }
    }

    // State survives a save/load round trip
    {
        auto a = host.load (vst3, sr, 512, error), b = host.load (vst3, sr, 512, error);

        if (a != nullptr && b != nullptr)
        {
            juce::Random rng (7);

            for (auto* param : a->getParameters())
            {
                float v = rng.nextFloat();

                // Stepped parameters snap, so ask for a value that is already on a step
                if (param->isDiscrete() && param->getNumSteps() > 1)
                    v = std::round (v * (float) (param->getNumSteps() - 1)) / (float) (param->getNumSteps() - 1);

                param->setValueNotifyingHost (v);
            }

            // The plugin and the host's parameter cache sync through the message loop
            juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
            juce::MemoryBlock state;
            a->getStateInformation (state);
            b->setStateInformation (state.getData(), (int) state.getSize());
            juce::MessageManager::getInstance()->runDispatchLoopUntil (60);

            float worst = 0.0f;
            const auto pa = a->getParameters();
            const auto pb = b->getParameters();

            for (int i = 0; i < std::min (pa.size(), pb.size()); ++i)
                worst = std::max (worst, std::abs (pa[i]->getValue() - pb[i]->getValue()));

            result (worst < 2e-3f && state.getSize() > 0, "state round-trips", juce::String (state.getSize()) + " bytes, worst parameter mismatch " + juce::String (worst, 5));
        }
    }

    // Every parameter can be swept while running without blowing up
    {
        auto p = host.load (vst3, sr, 512, error);

        if (p != nullptr)
        {
            auto maxStep = [] (const juce::AudioBuffer<float>& b, int from, int to)
            {
                float m = 0.0f;

                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                {
                    auto* d = b.getReadPointer (ch);

                    for (int i = std::max (1, from); i < std::min (b.getNumSamples(), to); ++i)
                        m = std::max (m, std::abs (d[i] - d[i - 1]));
                }

                return m;
            };

            // A click is a step well beyond what the plugin does at rest at the settings the
            // sweep passes through; the waveform's own slope (a boosted sine, a 4 kHz staircase,
            // hiss at full) is not one. Each eighth of the sweep is compared with the plugin
            // held still at both ends of that eighth.
            constexpr int segments = 8;
            float worstJump = 0.0f, worstExcess = -1.0f;
            juce::String worstParam, worstDetail, clicks;

            for (auto* param : p->getParameters())
            {
                if (param == p->getBypassParameter())
                    continue;

                auto fresh = host.load (vst3, sr, 512, error);
                auto still = host.load (vst3, sr, 512, error);

                if (fresh == nullptr || still == nullptr)
                    continue;

                const int latency = fresh->getLatencySamples();
                auto* target = fresh->getParameters()[param->getParameterIndex()];
                auto x = sine (sr, 440.0, 3.0);
                const int n = x.getNumSamples();
                const int sweepStart = (int) (n * 0.2f), sweepLength = (int) (n * 0.6f);

                Host::process (*fresh, x, 512, [&] (int start)
                {
                    const float t = juce::jlimit (0.0f, 1.0f, (float) (start - sweepStart) / (float) sweepLength);
                    target->setValueNotifyingHost (t);
                });

                // At rest: one second per point, the first 0.4 s left for smoothers to settle
                const int hold = (int) sr;
                auto y = sine (sr, 440.0, (double) (segments + 1));
                auto* held = still->getParameters()[param->getParameterIndex()];

                Host::process (*still, y, 512, [&] (int start)
                {
                    held->setValueNotifyingHost ((float) std::min (start / hold, segments) / (float) segments);
                });

                std::array<float, segments + 1> rest {};

                for (int k = 0; k <= segments; ++k)
                    rest[(size_t) k] = maxStep (y, k * hold + (int) (sr * 0.4) + latency, (k + 1) * hold);

                const int lag = (int) (sr * 0.05);   // smoothing lets each eighth spill into the next

                for (int k = 0; k < segments; ++k)
                {
                    const int from = sweepStart + k * sweepLength / segments + latency;
                    const float moving = maxStep (x, from, from + sweepLength / segments + lag);
                    const float atRest = std::max (rest[(size_t) k], rest[(size_t) k + 1]);
                    const float excess = moving - atRest;

                    if (moving > atRest * 1.5f + 0.02f)
                        clicks << (clicks.isEmpty() ? "" : ", ") << param->getName (64) << " near " << juce::String ((k + 0.5f) / segments, 2);

                    if (excess > worstExcess)
                    {
                        worstExcess = excess;
                        worstDetail = param->getName (64) + " " + juce::String (moving, 3) + " moving vs " + juce::String (atRest, 3) + " at rest";
                    }
                }

                const float jump = maxStep (x, 1, n);

                if (jump > worstJump)
                {
                    worstJump = jump;
                    worstParam = param->getName (64);
                }

                auto s = measure::stats (x);

                if (! (s.finite && s.peakDb < 12.0))
                    result (false, "sweeping " + param->getName (64), measure::describe (s));
            }

            result (true, "all parameters sweep without blowing up", "largest sample-to-sample step " + juce::String (worstJump, 3) + " on " + worstParam);
            result (clicks.isEmpty(), "no parameter clicks while swept",
                    clicks.isEmpty() ? "worst " + worstDetail : "clicks: " + clicks + "; worst " + worstDetail);
        }
    }

    // Noise floor on silence, which bass boxes are allowed to have, but reported
    {
        auto p = host.load (vst3, sr, 512, error);

        if (p != nullptr)
        {
            juce::AudioBuffer<float> x (2, (int) sr * 2);
            x.clear();
            Host::process (*p, x, 512);
            auto s = measure::stats (x);
            result (s.finite, "silence in", "output " + measure::describe (s));
        }
    }

    say (fails == 0 ? "ALL PASSED" : juce::String (fails) + " FAILED");
    return fails == 0 ? 0 : 1;
}

//==============================================================================
int measureFile (const Args& args)
{
    juce::AudioBuffer<float> audio;
    double sr = 48000.0;
    const juce::File file (args.after ("--measure"));

    if (! readWav (file, audio, sr))
    {
        say ("ERROR can't read " + file.getFullPathName());
        return 1;
    }

    // --from / --to (seconds) measure only that stretch of the file
    if (args.has ("--from") || args.has ("--to"))
    {
        const int from = juce::jlimit (0, audio.getNumSamples(), (int) (args.after ("--from").getDoubleValue() * sr));
        const int to = args.has ("--to") ? juce::jlimit (from, audio.getNumSamples(), (int) (args.after ("--to").getDoubleValue() * sr))
                                         : audio.getNumSamples();
        juce::AudioBuffer<float> part (audio.getNumChannels(), std::max (1, to - from));

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            part.copyFrom (ch, 0, audio, ch, from, part.getNumSamples());

        audio = std::move (part);
    }

    const bool all = ! (args.has ("--stats") || args.has ("--pitch") || args.has ("--thd") || args.has ("--decay") || args.has ("--bands")
                        || args.has ("--response") || args.has ("--alias") || args.has ("--peakpos") || args.has ("--crest")
                        || args.has ("--dynamics"));
    say (file.getFileName() + "  " + juce::String (audio.getNumSamples() / sr, 3) + " s, " + juce::String ((int) sr) + " Hz, " + juce::String (audio.getNumChannels()) + " ch");

    if (all || args.has ("--stats"))     say ("stats:    " + measure::describe (measure::stats (audio)));

    if (args.has ("--crest"))
    {
        const auto s = measure::stats (audio);
        say ("crest:    " + juce::String (s.peakDb - s.rmsDb, 2) + " dB (peak " + juce::String (s.peakDb, 1) + ", rms " + juce::String (s.rmsDb, 1) + ")");
    }

    if (args.has ("--peakpos"))
    {
        auto* d = audio.getReadPointer (0);
        int at = 0;

        for (int i = 1; i < audio.getNumSamples(); ++i)
            if (std::abs (d[i]) > std::abs (d[at]))
                at = i;

        say ("peakpos:  sample " + juce::String (at) + " (" + juce::String (d[at], 4) + ")");
    }

    if (args.has ("--alias"))
        say ("alias:    " + measure::describe (measure::alias (audio, sr, args.after ("--alias").getDoubleValue())));

    // How much the level moves over time: 50 ms RMS windows (those above -60 dBFS), the
    // spread between the loud 95th and quiet 10th percentile. A compressor narrows it.
    if (args.has ("--dynamics"))
    {
        const int w = (int) (sr * 0.05);
        auto* d = audio.getReadPointer (0);
        std::vector<double> levels;

        for (int start = 0; start + w <= audio.getNumSamples(); start += w)
        {
            double sum = 0.0;

            for (int i = start; i < start + w; ++i)
                sum += (double) d[i] * d[i];

            const double level = 10.0 * std::log10 (sum / w + 1.0e-20);

            if (level > -60.0)
                levels.push_back (level);
        }

        if (levels.size() < 4)
        {
            say ("dynamics: too few windows above -60 dBFS");
        }
        else
        {
            std::sort (levels.begin(), levels.end());
            const double p10 = levels[levels.size() / 10], p95 = levels[levels.size() * 95 / 100];
            say ("dynamics: 50 ms windows p95 " + juce::String (p95, 1) + " dBFS, p10 " + juce::String (p10, 1) + " dBFS, range "
                 + juce::String (p95 - p10, 2) + " dB over " + juce::String ((int) levels.size()) + " windows");
        }
    }
    if (all || args.has ("--bands"))     say ("bands:    " + measure::describe (measure::bands (audio, sr)));
    if (all || args.has ("--pitch"))     say ("pitch:    " + measure::describe (measure::pitchTrack (audio, sr)));

    if (args.has ("--thd"))
        say ("thd:      " + measure::describe (measure::thd (audio, sr, args.after ("--thd").getDoubleValue())));
    else if (all)
        say ("thd:      " + measure::describe (measure::thd (audio, sr, measure::pitchTrack (audio, sr).meanHz)));

    if (all || args.has ("--decay"))     say ("decay:    " + measure::describe (measure::decay (audio, sr)));

    if (all || args.has ("--response"))
    {
        // With --ref <dry.wav> this is the plugin's frequency response: processed minus dry,
        // which also cancels the 3 dB/octave tilt an exponential sweep carries by itself
        std::vector<double> freqs { 30, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000, 20000 };

        // --freqs 60,160,400 measures at those centres instead
        if (args.has ("--freqs"))
        {
            freqs.clear();

            for (auto& f : juce::StringArray::fromTokens (args.after ("--freqs"), ",", ""))
                if (f.getDoubleValue() > 0.0)
                    freqs.push_back (f.getDoubleValue());
        }
        auto response = measure::responseAt (audio, sr, freqs);
        juce::String line = "response:";
        bool relative = false;

        if (args.has ("--ref"))
        {
            juce::AudioBuffer<float> ref;
            double refRate = sr;

            if (readWav (juce::File (args.after ("--ref")), ref, refRate))
            {
                auto dry = measure::responseAt (ref, refRate, freqs);

                for (size_t i = 0; i < response.size(); ++i)
                    response[i].second -= dry[i].second;

                relative = true;
            }
            else
            {
                say ("WARN can't read --ref file; showing absolute levels");
            }
        }

        for (auto& [hz, level] : response)
            line += " " + (hz >= 1000 ? juce::String (hz / 1000.0, hz < 10000 ? 1 : 0) + "k" : juce::String ((int) hz)) + "=" + juce::String (level, 2);

        say (line + (relative ? "  (dB, relative to --ref)" : "  (dB, absolute per band; use --ref <dry.wav> for a response)"));
    }

    return 0;
}

//==============================================================================
/** How much of one core the plugin takes in real time: ten seconds of a bass-register
    signal at the given block size (default 32, a low-latency ASIO buffer). */
int cpu (Host& host, const Args& args)
{
    const juce::File vst3 (args.after ("--cpu"));
    const int block = std::max (8, args.has ("--block") ? args.after ("--block").getIntValue() : 32);
    const double sr = args.has ("--rate") ? args.after ("--rate").getDoubleValue() : 48000.0;
    juce::String error;
    auto p = host.load (vst3, sr, block, error);

    if (p == nullptr)
    {
        say ("ERROR " + error);
        return 1;
    }

    for (auto& assignment : args.allAfter ("--set"))
        if (! Host::set (*p, assignment, error))
        {
            say ("ERROR " + error);
            return 1;
        }

    p->setNonRealtime (false);
    constexpr double seconds = 10.0;
    juce::AudioBuffer<float> audio (2, (int) (sr * seconds));

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const double t = i / sr;
        const float v = 0.3f * (float) (std::sin (juce::MathConstants<double>::twoPi * 55.0 * t) + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 165.0 * t));
        audio.setSample (0, i, v);
        audio.setSample (1, i, v);
    }

    // One warm-up pass, then the timed one
    auto warm = audio;
    Host::process (*p, warm, block);
    const auto t0 = juce::Time::getHighResolutionTicks();
    Host::process (*p, audio, block);
    const double elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);

    say ("cpu:      " + juce::String (elapsed * 1000.0, 1) + " ms for " + juce::String (seconds, 0) + " s at " + juce::String ((int) sr)
         + " Hz in " + juce::String (block) + "-sample blocks = " + juce::String (100.0 * elapsed / seconds, 2) + " % of one core, latency "
         + juce::String (p->getLatencySamples()) + " samples (" + juce::String (1000.0 * p->getLatencySamples() / sr, 2) + " ms)");
    return 0;
}

//==============================================================================
int run (const juce::String& commandLine)
{
    const Args args (commandLine);
    Host host;

    if (args.has ("--signals"))   return writeSignals (juce::File (args.after ("--signals")));
    if (args.has ("--params"))    return listParams (host, juce::File (args.after ("--params")));
    if (args.has ("--render"))    return render (host, args);
    if (args.has ("--validate"))  return validate (host, juce::File (args.after ("--validate")));
    if (args.has ("--measure"))   return measureFile (args);
    if (args.has ("--cpu"))       return cpu (host, args);

    say ("usage: basstool --signals <folder> | --params <vst3> | --render <vst3> <in> <out> [--set Name=value] | --validate <vst3>"
         " | --measure <wav> [--alias f0] [--crest] [--peakpos] | --cpu <vst3> [--block 32]");
    return 2;
}

} // namespace

//==============================================================================
class BassToolApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "basstool"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Plugins expect a message thread; run once the loop is up
        juce::MessageManager::callAsync ([this, commandLine]
        {
            setApplicationReturnValue (run (commandLine));
            quit();
        });
    }

    void shutdown() override {}
};

START_JUCE_APPLICATION (BassToolApplication)
