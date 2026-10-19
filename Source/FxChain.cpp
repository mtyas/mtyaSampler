#include "FxChain.h"

namespace
{
    // Delay divisions in quarter notes, in the order of fx_delay_div's choices
    constexpr float divisionBeats[] = { 0.125f, 0.25f * 2 / 3, 0.25f, 0.375f, 0.5f * 2 / 3, 0.5f, 0.75f, 2.0f / 3, 1.0f, 1.5f, 2.0f, 4.0f };

    float shape (int type, float x) noexcept
    {
        switch (type)
        {
            case 1:  return juce::jlimit (-1.0f, 1.0f, x);                               // hard
            case 2:  return std::tanh (x + 0.35f) - std::tanh (0.35f);                    // tube: asymmetric, adds even harmonics
            case 3:  return std::sin (juce::MathConstants<float>::halfPi * x);            // fold: wraps back past full scale
            default: return std::tanh (x);                                               // soft
        }
    }
}

FxChain::FxChain (const SamplerEngine& e) : engine (e)
{
    auto i = [&e] (const char* id) { return e.indexOf (id); };
    ix.on[sat] = i ("fx_sat_on");         ix.on[crush] = i ("fx_crush_on");     ix.on[chorusFx] = i ("fx_chorus_on");
    ix.on[delayFx] = i ("fx_delay_on");   ix.on[reverbFx] = i ("fx_reverb_on");
    ix.satDrive = i ("fx_sat_drive");     ix.satType = i ("fx_sat_type");       ix.satTone = i ("fx_sat_tone");   ix.satMix = i ("fx_sat_mix");
    ix.crushBits = i ("fx_crush_bits");   ix.crushRate = i ("fx_crush_rate");   ix.crushMix = i ("fx_crush_mix");
    ix.chorusMix = i ("fx_chorus_mix");   ix.chorusRate = i ("fx_chorus_rate"); ix.chorusDepth = i ("fx_chorus_depth"); ix.chorusFb = i ("fx_chorus_fb");
    ix.delayFb = i ("fx_delay_fb");       ix.delayTime = i ("fx_delay_time");   ix.delaySync = i ("fx_delay_sync");
    ix.delayDiv = i ("fx_delay_div");     ix.delayMix = i ("fx_delay_mix");     ix.delayPingPong = i ("fx_delay_pingpong");
    ix.delayTone = i ("fx_delay_tone");
    ix.reverbMix = i ("fx_reverb_mix");   ix.reverbSize = i ("fx_reverb_size"); ix.reverbDamp = i ("fx_reverb_damp");
    ix.reverbWidth = i ("fx_reverb_width"); ix.reverbPredelay = i ("fx_reverb_predelay");
}

void FxChain::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    const auto block = juce::jmax (1, maxBlockSize);
    dry.setSize (2, block);
    wet.setSize (2, block);
    delayLine.setSize (2, (int) (sr * 2.5) + 4);
    predelayLine.setSize (2, (int) (sr * 0.26) + 4);

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) block, 2 };
    chorus.prepare (spec);
    reverb.prepare (spec);

    for (auto& g : onGain)
        g.reset (sr, 0.02);
    delaySamples.reset (sr, 0.15);   // time changes glide like tape instead of clicking
    reset();
}

void FxChain::reset()
{
    for (auto& o : satUp) o.reset();
    for (auto& o : satDown) o.reset();
    satTone[0] = satTone[1] = 0;
    crushHold[0] = crushHold[1] = 0;
    crushPhase = 1.0;
    chorus.reset();
    delayLine.clear();
    delayWrite = 0;
    delayLp[0] = delayLp[1] = 0;
    delayFresh = true;
    reverb.reset();
    predelayLine.clear();
    predelayWrite = 0;
    for (int i = 0; i < numFx; ++i)
        onGain[i].setCurrentAndTargetValue (p (ix.on[i], 0) >= 0.5f ? 1.0f : 0.0f);
}

void FxChain::process (juce::AudioBuffer<float>& buffer, double bpm) noexcept
{
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || n > dry.getNumSamples())
        return;   // the host sent more than it announced: skip the effects rather than allocate

    for (const auto fx : engine.fxOrder())
    {
        auto& gain = onGain[fx];
        gain.setTargetValue (p (ix.on[fx], 0) >= 0.5f ? 1.0f : 0.0f);
        if (! gain.isSmoothing() && gain.getTargetValue() == 0.0f)
            continue;

        const bool blending = gain.isSmoothing() || gain.getCurrentValue() < 1.0f;
        if (blending)
            for (int ch = 0; ch < 2; ++ch)
                dry.copyFrom (ch, 0, buffer, ch, 0, n);

        switch (fx)
        {
            case sat:      processSaturate (buffer, n); break;
            case crush:    processCrush (buffer, n); break;
            case chorusFx: processChorus (buffer, n); break;
            case delayFx:  processDelay (buffer, n, bpm); break;
            case reverbFx: processReverb (buffer, n); break;
            default: break;
        }

        if (blending)
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto* dl = dry.getReadPointer (0);
            const auto* dr = dry.getReadPointer (1);
            for (int i = 0; i < n; ++i)
            {
                const auto g = gain.getNextValue();
                l[i] = dl[i] + g * (l[i] - dl[i]);
                r[i] = dr[i] + g * (r[i] - dr[i]);
            }
            // Fully off: clear the effect's memory so it starts clean next time
            if (gain.getCurrentValue() == 0.0f)
            {
                if (fx == delayFx) { delayLine.clear(); delayLp[0] = delayLp[1] = 0; delayFresh = true; }
                if (fx == reverbFx) { reverb.reset(); predelayLine.clear(); }
                if (fx == chorusFx) chorus.reset();
            }
        }
    }
}

void FxChain::processSaturate (juce::AudioBuffer<float>& buffer, int n) noexcept
{
    const auto drive = juce::Decibels::decibelsToGain (p (ix.satDrive, 0));
    const auto type = juce::roundToInt (p (ix.satType, 0));
    const auto mix = p (ix.satMix, 100) * 0.01f;
    // Tone: a gentle low-pass after the shaper, 800 Hz … open
    const auto tone = p (ix.satTone, 100) * 0.01f;
    const auto toneHz = 800.0f * std::pow (25.0f, tone);
    const auto toneCoef = tone >= 0.999f ? 1.0f : 1.0f - std::exp (-juce::MathConstants<float>::twoPi * toneHz / (float) sr);
    const auto makeup = 1.0f / std::sqrt (drive);

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            float a, b;
            satUp[ch].upsample (d[i] * drive, a, b);
            const auto y = satDown[ch].downsample (shape (type, a), shape (type, b)) * makeup;
            satTone[ch] += toneCoef * (y - satTone[ch]);
            d[i] += mix * (satTone[ch] - d[i]);
        }
    }
}

void FxChain::processCrush (juce::AudioBuffer<float>& buffer, int n) noexcept
{
    const auto steps = std::exp2 (juce::jlimit (1.0f, 16.0f, p (ix.crushBits, 16)) - 1.0f);
    const auto rate = juce::jlimit (100.0, sr, (double) p (ix.crushRate, (float) sr));
    const auto mix = p (ix.crushMix, 100) * 0.01f;
    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        // Sample-and-hold at the reduced rate, then quantise
        crushPhase += rate / sr;
        if (crushPhase >= 1.0)
        {
            crushPhase -= std::floor (crushPhase);
            crushHold[0] = std::round (l[i] * steps) / steps;
            crushHold[1] = std::round (r[i] * steps) / steps;
        }
        l[i] += mix * (crushHold[0] - l[i]);
        r[i] += mix * (crushHold[1] - r[i]);
    }
}

void FxChain::processChorus (juce::AudioBuffer<float>& buffer, int n) noexcept
{
    chorus.setRate (p (ix.chorusRate, 0.6f));
    chorus.setDepth (p (ix.chorusDepth, 40) * 0.01f);
    chorus.setCentreDelay (7.0f);
    chorus.setFeedback (p (ix.chorusFb, 0) * 0.01f);
    chorus.setMix (p (ix.chorusMix, 30) * 0.01f);
    juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, (size_t) n);
    chorus.process (juce::dsp::ProcessContextReplacing<float> (block));
}

void FxChain::processDelay (juce::AudioBuffer<float>& buffer, int n, double bpm) noexcept
{
    float ms = p (ix.delayTime, 375);
    if (p (ix.delaySync, 0) >= 0.5f && bpm > 0)
    {
        const auto div = juce::jlimit (0, (int) std::size (divisionBeats) - 1, juce::roundToInt (p (ix.delayDiv, 5)));
        ms = (float) (divisionBeats[div] * 60000.0 / bpm);
    }
    const int length = delayLine.getNumSamples();
    const auto target = juce::jlimit (1.0f, (float) (length - 4), ms * 0.001f * (float) sr);
    if (delayFresh)
        delaySamples.setCurrentAndTargetValue (target);
    else
        delaySamples.setTargetValue (target);
    delayFresh = false;
    const auto fb = juce::jlimit (0.0f, 0.95f, p (ix.delayFb, 40) * 0.01f);
    const auto mix = p (ix.delayMix, 30) * 0.01f;
    const bool pingPong = p (ix.delayPingPong, 0) >= 0.5f;
    const auto toneCoef = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * p (ix.delayTone, 6000) / (float) sr);

    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getWritePointer (1);
    auto* dlL = delayLine.getWritePointer (0);
    auto* dlR = delayLine.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const auto d = delaySamples.getNextValue();
        auto readPos = (float) delayWrite - d;
        if (readPos < 0) readPos += (float) length;
        const int i0 = (int) readPos;
        const int i1 = (i0 + 1) % length;
        const auto frac = readPos - (float) i0;
        const auto wl = dlL[i0] + frac * (dlL[i1] - dlL[i0]);
        const auto wr = dlR[i0] + frac * (dlR[i1] - dlR[i0]);

        // Darker with every repeat
        delayLp[0] += toneCoef * (wl - delayLp[0]);
        delayLp[1] += toneCoef * (wr - delayLp[1]);

        if (pingPong)
        {
            // Mono input enters on the left; each repeat swaps sides
            dlL[delayWrite] = 0.5f * (l[i] + r[i]) + fb * delayLp[1];
            dlR[delayWrite] = fb * delayLp[0];
        }
        else
        {
            dlL[delayWrite] = l[i] + fb * delayLp[0];
            dlR[delayWrite] = r[i] + fb * delayLp[1];
        }
        delayWrite = (delayWrite + 1) % length;

        l[i] += mix * (wl - l[i]);
        r[i] += mix * (wr - r[i]);
    }
}

void FxChain::processReverb (juce::AudioBuffer<float>& buffer, int n) noexcept
{
    juce::dsp::Reverb::Parameters rp;
    rp.roomSize = juce::jlimit (0.0f, 1.0f, p (ix.reverbSize, 65) * 0.01f);
    rp.damping = juce::jlimit (0.0f, 1.0f, p (ix.reverbDamp, 40) * 0.01f);
    rp.width = juce::jlimit (0.0f, 1.0f, p (ix.reverbWidth, 100) * 0.01f);
    rp.wetLevel = 1.0f;
    rp.dryLevel = 0.0f;
    reverb.setParameters (rp);

    // Pre-delay the signal going into the reverb
    const int length = predelayLine.getNumSamples();
    const int pre = juce::jlimit (0, length - 2, (int) (p (ix.reverbPredelay, 0) * 0.001f * (float) sr));
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* line = predelayLine.getWritePointer (ch);
        const auto* in = buffer.getReadPointer (ch);
        auto* w = wet.getWritePointer (ch);
        int pos = predelayWrite;
        for (int i = 0; i < n; ++i)
        {
            line[pos] = in[i];
            auto readPos = pos - pre;
            if (readPos < 0) readPos += length;
            w[i] = line[readPos];
            pos = (pos + 1) % length;
        }
    }
    predelayWrite = (predelayWrite + n) % length;

    juce::dsp::AudioBlock<float> block (wet.getArrayOfWritePointers(), 2, (size_t) n);
    reverb.process (juce::dsp::ProcessContextReplacing<float> (block));

    const auto mix = p (ix.reverbMix, 30) * 0.01f;
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        const auto* w = wet.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
            d[i] += mix * (w[i] - d[i]);
    }
}
