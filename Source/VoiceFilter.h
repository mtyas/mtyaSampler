#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <vector>

// Per-voice filter with analogue-style behaviour:
//  * LP24: a zero-delay-feedback transistor ladder with tanh stages (after Mystran's "cheap
//    non-linear zero-delay filters"). Resonance past ~95 % self-oscillates; the stages keep it
//    from blowing up, so it sings instead of clipping.
//  * LP12 / BP / HP: a TPT state-variable filter whose band integrator saturates, so very high
//    resonance (damping below zero) also self-oscillates smoothly.
//  * Comb: a feedback comb with a damped, saturating loop; at full resonance it rings on its own.
// The ladder and SVF run at 2× the sample rate (half-band FIR up/down) to keep the saturation
// clean, and the cutoff and resonance glide per sample between control updates (no zipper noise).
// No allocation after prepare().
namespace mtya
{
    // Fast tanh(x)/x (Padé), used to linearise the ladder's stages around their current state.
    inline float tanhXdX (float x) noexcept
    {
        const auto a = x * x;
        return ((a + 105.0f) * a + 945.0f) / ((15.0f * a + 420.0f) * a + 945.0f);
    }

    // Smooth saturation that tends to ±1.
    inline float softClip (float x) noexcept
    {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        const auto x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    // Half-band FIR kernel for 2× up/downsampling (Kaiser windowed sinc, flat to ~0.39 × fs).
    struct HalfBand
    {
        static constexpr int taps = 12;           // non-zero odd taps per side
        std::array<float, taps> h {};             // h[i] = coefficient at offset 2i+1 (centre is 0.5)

        HalfBand()
        {
            constexpr double beta = 8.0;
            const double halfLength = 2.0 * taps;  // window spans |j| <= 2*taps - 1
            auto bessel0 = [] (double x)
            {
                double sum = 1, term = 1;
                for (int k = 1; k < 30; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
                return sum;
            };
            for (int i = 0; i < taps; ++i)
            {
                const double j = 2 * i + 1;
                const double sinc = std::sin (juce::MathConstants<double>::pi * j / 2.0) / (juce::MathConstants<double>::pi * j / 2.0);
                const double r = j / halfLength;
                const double w = bessel0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / bessel0 (beta);
                h[(size_t) i] = (float) (0.5 * sinc * w);
            }
        }

        static const HalfBand& get() { static const HalfBand hb; return hb; }
    };

    // 2× oversampler for one channel: push a base-rate sample, get two samples at 2× rate;
    // push two processed samples back, get one base-rate sample.
    class Oversampler2x
    {
    public:
        void reset() { up.fill (0.0f); down.fill (0.0f); upPos = downPos = 0; }

        void upsample (float x, float& first, float& second) noexcept
        {
            constexpr int n = HalfBand::taps;
            upPos = (upPos + upLen - 1) % upLen;
            up[(size_t) upPos] = up[(size_t) (upPos + upLen)] = x;
            const auto* hist = up.data() + upPos;   // hist[k] = x[n-k]
            const auto& h = HalfBand::get().h;
            float mid = 0;
            for (int i = 0; i < n; ++i)
                mid += 2.0f * h[(size_t) i] * (hist[n - 1 - i] + hist[n + i]);
            first = hist[n];
            second = mid;
        }

        float downsample (float a, float b) noexcept
        {
            constexpr int n = HalfBand::taps;
            push (a);
            push (b);
            const auto* d = down.data() + downPos;  // d[0] = newest
            const auto& h = HalfBand::get().h;
            constexpr int c = 2 * n - 1;
            float y = 0.5f * d[c];
            for (int i = 0; i < n; ++i)
                y += h[(size_t) i] * (d[c - (2 * i + 1)] + d[c + (2 * i + 1)]);
            return y;
        }

    private:
        void push (float v) noexcept
        {
            downPos = (downPos + downLen - 1) % downLen;
            down[(size_t) downPos] = down[(size_t) (downPos + downLen)] = v;
        }

        static constexpr int upLen = 2 * HalfBand::taps + 1;
        static constexpr int downLen = 4 * HalfBand::taps;
        std::array<float, upLen * 2> up {};
        std::array<float, downLen * 2> down {};
        int upPos = 0, downPos = 0;
    };

    class VoiceFilter
    {
    public:
        enum Type { lp24 = 0, lp12, bp, hp, comb };

        // Called once (not on the audio thread): sizes the comb delay for the lowest cutoff.
        void prepare (double sampleRate)
        {
            sr = sampleRate;
            combLength = (int) std::ceil (sampleRate / 20.0) + 4;
            combLine.assign ((size_t) combLength * 2, 0.0f);
        }

        void reset() noexcept
        {
            for (auto& c : ch) c = {};
            for (auto& o : osIn) o.reset();
            for (auto& o : osOut) o.reset();
            std::fill (combLine.begin(), combLine.end(), 0.0f);
            combWrite = 0;
            first = true;
        }

        // Control-rate update. The new values are reached smoothly over `rampSamples` samples.
        void setTargets (int newType, float cutoffHz, float reso01, float driveDb, int rampSamples) noexcept
        {
            type = newType;
            const auto nyquistSafe = (float) (sr * 0.45);
            cutoffHz = juce::jlimit (20.0f, nyquistSafe, cutoffHz);
            reso01 = juce::jlimit (0.0f, 1.0f, reso01);
            const auto driveGain = std::pow (10.0f, juce::jlimit (0.0f, 36.0f, driveDb) / 20.0f);

            if (type == comb)
            {
                tDelay = juce::jlimit (1.0f, (float) (combLength - 2), (float) (sr / cutoffHz));
                // Past 90 % the feedback goes above 1: it rings on its own (the loop saturates)
                tFeedback = reso01 <= 0.9f ? 0.2f + 0.75f * reso01 : 0.875f + (reso01 - 0.9f) * 1.85f;
            }
            else
            {
                // At 2× rate
                tG = (float) std::tan (juce::MathConstants<double>::pi * cutoffHz / (2.0 * sr));
                // Ladder feedback: > 4 oscillates. SVF damping follows the UI's curve (Q = 0.707 + 9 × Reso)
                // up to 90 %, then dips below zero so the top of the knob self-oscillates.
                tRes = type == lp24 ? 4.3f * reso01
                                    : (reso01 <= 0.9f ? 1.0f / (0.707f + 9.0f * reso01)
                                                      : 0.1144f - (reso01 - 0.9f) * 1.8f);
            }
            // Internal level: the ladder runs quieter inside (its stages saturate early), the others at half scale
            const auto inside = type == lp24 ? 0.25f : 0.5f;
            tInGain = inside * driveGain;
            tOutGain = 1.0f / (inside * std::sqrt (driveGain));

            if (first)
            {
                g = tG; res = tRes; delay = tDelay; feedback = tFeedback; inGain = tInGain; outGain = tOutGain;
                first = false;
            }
            const auto steps = (float) juce::jmax (1, rampSamples);
            dG = (tG - g) / (2.0f * steps);
            dRes = (tRes - res) / (2.0f * steps);
            dDelay = (tDelay - delay) / steps;
            dFeedback = (tFeedback - feedback) / steps;
            dIn = (tInGain - inGain) / steps;
            dOut = (tOutGain - outGain) / steps;
            rampLeft = (int) steps;
        }

        // Processes one sample of up to two channels in place.
        void process (float& l, float& r, bool stereo) noexcept
        {
            if (type == comb)
            {
                processComb (l, 0);
                if (stereo) processComb (r, 1);
                combWrite = (combWrite + 1) % combLength;
                stepRamp (false);
                return;
            }

            for (int c = 0; c < (stereo ? 2 : 1); ++c)
            {
                auto& x = c == 0 ? l : r;
                float a, b;
                osIn[c].upsample (x * inGain, a, b);
                a = runCore (a, c, g, res);
                b = runCore (b, c, g + dG, res + dRes);
                x = osOut[c].downsample (a, b) * outGain;
            }
            stepRamp (true);
        }

    private:
        struct Channel
        {
            float s[4] {};       // ladder stages
            float zi = 0;        // previous ladder input
            float ic1 = 0, ic2 = 0;   // SVF integrators
            float combLp = 0;    // comb loop damping
            float combX1 = 0, combY1 = 0;   // comb loop DC blocker
        };

        void stepRamp (bool twice) noexcept
        {
            if (rampLeft <= 0) return;
            g += dG * (twice ? 2.0f : 1.0f);
            res += dRes * (twice ? 2.0f : 1.0f);
            delay += dDelay; feedback += dFeedback; inGain += dIn; outGain += dOut;
            if (--rampLeft == 0)
            {
                g = tG; res = tRes; delay = tDelay; feedback = tFeedback; inGain = tInGain; outGain = tOutGain;
            }
        }

        float runCore (float x, int c, float f, float k) noexcept
        {
            auto& st = ch[c];
            if (type == lp24)
            {
                // Bass compensation: without it the passband drops by 1 / (1 + k) as the resonance rises
                x *= 1.0f + k;
                const auto ih = 0.5f * (x + st.zi);
                st.zi = x;
                auto* s = st.s;
                const auto t0 = tanhXdX (ih - k * s[3]);
                const auto t1 = tanhXdX (s[0]), t2 = tanhXdX (s[1]), t3 = tanhXdX (s[2]), t4 = tanhXdX (s[3]);
                const auto g0 = 1.0f / (1.0f + f * t1), g1 = 1.0f / (1.0f + f * t2);
                const auto g2 = 1.0f / (1.0f + f * t3), g3 = 1.0f / (1.0f + f * t4);
                const auto f3 = f * t3 * g3, f2 = f * t2 * g2 * f3, f1 = f * t1 * g1 * f2, f0 = f * t0 * g0 * f1;
                const auto y3 = (g3 * s[3] + f3 * g2 * s[2] + f2 * g1 * s[1] + f1 * g0 * s[0] + f0 * x) / (1.0f + k * f0);
                const auto xx = t0 * (x - k * y3);
                const auto y0 = t1 * g0 * (s[0] + f * xx);
                const auto y1 = t2 * g1 * (s[1] + f * y0);
                const auto y2 = t3 * g2 * (s[2] + f * y1);
                s[0] += 2.0f * f * (xx - y0);
                s[1] += 2.0f * f * (y0 - y1);
                s[2] += 2.0f * f * (y1 - y2);
                s[3] += 2.0f * f * (t4 * y2 - y3);
                return y3;
            }

            // State-variable filter, saturating input and band state
            const auto v0 = softClip (x);
            const auto a1 = 1.0f / (1.0f + f * (f + k));
            const auto a2 = f * a1, a3 = f * a2;
            const auto v3 = v0 - st.ic2;
            const auto v1 = a1 * st.ic1 + a2 * v3;
            const auto v2 = st.ic2 + a2 * st.ic1 + a3 * v3;
            // The band state saturates: loud resonant peaks compress like an analogue filter, and when
            // the damping goes negative the oscillation settles at about full scale (limit × output gain).
            st.ic1 = svfLimit * softClip ((2.0f * v1 - st.ic1) / svfLimit);
            st.ic2 = 2.0f * v2 - st.ic2;
            switch (type)
            {
                // Unity at the centre; near self-oscillation the gain rises so the BP sings as loudly as the LP
                case bp:  return v1 * (k >= 0.12f ? k : 0.12f + (0.12f - k) * 10.0f);
                case hp:  return v0 - k * v1 - v2;
                default:  return v2;
            }
        }

        void processComb (float& x, int c) noexcept
        {
            auto* line = combLine.data() + c * combLength;
            auto readPos = (float) combWrite - delay;
            if (readPos < 0) readPos += (float) combLength;
            const int i0 = (int) readPos;
            const auto frac = readPos - (float) i0;
            const auto delayed = line[i0] + frac * (line[(i0 + 1) % combLength] - line[i0]);
            auto& st = ch[c];
            st.combLp += 0.6f * (delayed - st.combLp);          // gentle damping in the loop
            const auto blocked = st.combLp - st.combX1 + 0.995f * st.combY1;   // DC blocker, so feedback > 1 rings at pitch
            st.combX1 = st.combLp;
            st.combY1 = blocked;
            const auto y = x * inGain + feedback * blocked;
            line[combWrite] = softClip (y);                     // the loop saturates, so it can ring forever
            x = y * juce::jmax (0.12f, 1.0f - feedback) * outGain;
        }

        static constexpr float svfLimit = 2.0f;   // band-state level where the SVF starts to saturate

        double sr = 44100.0;
        int type = lp12;
        Channel ch[2];
        Oversampler2x osIn[2], osOut[2];
        std::vector<float> combLine;
        int combLength = 1, combWrite = 0;

        float g = 0.5f, res = 0, delay = 1, feedback = 0, inGain = 0.5f, outGain = 2.0f;
        float tG = 0.5f, tRes = 0, tDelay = 1, tFeedback = 0, tInGain = 0.5f, tOutGain = 2.0f;
        float dG = 0, dRes = 0, dDelay = 0, dFeedback = 0, dIn = 0, dOut = 0;
        int rampLeft = 0;
        bool first = true;
    };
}
