#pragma once
#include <juce_audio_formats/juce_audio_formats.h>

// Sample analysis and instrument-editing helpers for the native commands (message thread):
// finding files, waveform peaks, root-pitch detection, zero crossings, and the "auto-map by
// name", "fill gaps" and folder-import operations on the instrument JSON.
namespace SampleTools
{
    // Finds a sample named in the instrument (full path, or a bare name looked up in
    // `sampleFolder`, then Documents/mtyaSampler/Recordings and /Samples). Empty if not found.
    juce::File locate (const juce::String& name, const juce::String& sampleFolder);

    struct Audio
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0;
        int bitsPerSample = 0;
        juce::String formatName;
        bool ok() const { return sampleRate > 0 && buffer.getNumSamples() > 0; }
    };
    Audio read (const juce::File&);

    // [min, max] pairs across the whole file (both channels), for the waveform display.
    juce::var peaks (const juce::AudioBuffer<float>&, int pairs = 1000);

    // Fundamental frequency (YIN) of the steady part of the sound; 0 when none is found.
    double detectPitchHz (const juce::AudioBuffer<float>&, double sampleRate);

    // The nearest sample index to `position` where the signal crosses zero.
    int nearestZeroCrossing (const juce::AudioBuffer<float>&, int position);

    // What a file name says about a sample, e.g. "Piano_C#3_mf_rr2.wav" or "My_Guitar_Ds2_f_rr1.wav"
    struct NameInfo
    {
        int note = -1;          // MIDI note (C3 = 60), -1 = none found
        int dynamic = -1;       // ordering key for the velocity layer (ppp = 0 … fff = 7, or an explicit vNN), -1 = none
        int rr = 0;             // round-robin number, 0 = none
    };
    NameInfo parseName (const juce::String& fileName);

    // Edits on the instrument JSON (a DynamicObject with "layers"). They return a short message.
    juce::String autoMap (juce::var& instrument);                  // re-derive root, keys and velocities from sample names
    juce::String fillGaps (juce::var& instrument);                 // close key gaps between neighbouring zones
    juce::String importFiles (juce::var& instrument, const juce::Array<juce::File>& files, const juce::String& group);

    juce::Array<juce::File> audioFilesIn (const juce::File& folder);
    juce::String noteName (int note);
}
