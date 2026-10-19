#include "SampleTools.h"
#include "InstrumentMap.h"
#include <map>

namespace SampleTools
{
    juce::File locate (const juce::String& nameIn, const juce::String& sampleFolder)
    {
        const auto name = nameIn.trim();
        if (name.isEmpty() || name.startsWith ("("))
            return {};
        if (juce::File::isAbsolutePath (name))
            return juce::File (name).existsAsFile() ? juce::File (name) : juce::File();

        if (juce::File::isAbsolutePath (sampleFolder))
        {
            const auto f = juce::File (sampleFolder).getChildFile (name);
            if (f.existsAsFile())
                return f;
        }
        const auto fileName = juce::File::createFileWithoutCheckingPath (name).getFileName();
        for (const auto& folder : SampleCache::defaultSearchFolders())
            if (folder.isDirectory())
            {
                juce::RangedDirectoryIterator it (folder, true, fileName, juce::File::findFiles);
                if (it != juce::RangedDirectoryIterator())
                    return it->getFile();
            }
        return {};
    }

    Audio read (const juce::File& file)
    {
        Audio a;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return a;
        const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0));
        const auto channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
        a.buffer.setSize (channels, length);
        reader->read (&a.buffer, 0, length, 0, true, channels > 1);
        a.sampleRate = reader->sampleRate;
        a.bitsPerSample = (int) reader->bitsPerSample;
        a.formatName = reader->getFormatName();
        return a;
    }

    juce::var peaks (const juce::AudioBuffer<float>& b, int pairs)
    {
        juce::Array<juce::var> out;
        const int n = b.getNumSamples();
        if (n == 0)
            return out;
        pairs = juce::jmin (pairs, n);
        for (int i = 0; i < pairs; ++i)
        {
            const int a = (int) ((juce::int64) i * n / pairs), e = juce::jmax (a + 1, (int) ((juce::int64) (i + 1) * n / pairs));
            float mn = 0, mx = 0;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                const auto r = b.findMinMax (ch, a, e - a);
                mn = juce::jmin (mn, r.getStart());
                mx = juce::jmax (mx, r.getEnd());
            }
            out.add (juce::Array<juce::var> { std::round (mn * 1000.0f) / 1000.0f, std::round (mx * 1000.0f) / 1000.0f });
        }
        return out;
    }

    double detectPitchHz (const juce::AudioBuffer<float>& b, double sr)
    {
        // YIN on a window from the steady part (after the loudest point, skipping the attack)
        const int n = b.getNumSamples();
        const int window = (int) juce::jmin (4096.0, sr * 0.09);
        if (n < window * 2)
            return 0;
        int peakAt = 0;
        float peak = 0;
        const auto* d = b.getReadPointer (0);
        for (int i = 0; i < juce::jmin (n, (int) sr); ++i)
            if (std::abs (d[i]) > peak) { peak = std::abs (d[i]); peakAt = i; }
        const int start = juce::jmin (n - window * 2, peakAt + (int) (sr * 0.05));

        const int minTau = (int) (sr / 2000.0), maxTau = juce::jmin (window - 1, (int) (sr / 27.0));
        std::vector<float> diff ((size_t) maxTau + 1, 0.0f);
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            float s = 0;
            for (int i = 0; i < window; ++i)
            {
                const auto x = d[start + i] - d[start + i + tau];
                s += x * x;
            }
            diff[(size_t) tau] = s;
        }
        // Cumulative mean normalised difference
        float running = 0;
        diff[0] = 1;
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            running += diff[(size_t) tau];
            diff[(size_t) tau] = running > 0 ? diff[(size_t) tau] * (float) tau / running : 1.0f;
        }
        int best = -1;
        for (int tau = minTau; tau < maxTau; ++tau)
            if (diff[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < maxTau && diff[(size_t) tau + 1] < diff[(size_t) tau])
                    ++tau;
                best = tau;
                break;
            }
        if (best < 0)
        {
            // No clear dip: take the global minimum if it's reasonably periodic
            best = minTau;
            for (int tau = minTau; tau < maxTau; ++tau)
                if (diff[(size_t) tau] < diff[(size_t) best]) best = tau;
            if (diff[(size_t) best] > 0.4f)
                return 0;
        }
        // Parabolic interpolation around the dip
        double t = best;
        if (best > 1 && best < maxTau)
        {
            const double a = diff[(size_t) best - 1], c = diff[(size_t) best], e = diff[(size_t) best + 1];
            const double denom = a - 2 * c + e;
            if (std::abs (denom) > 1e-12)
                t += 0.5 * (a - e) / denom;
        }
        return sr / t;
    }

    int nearestZeroCrossing (const juce::AudioBuffer<float>& b, int position)
    {
        const int n = b.getNumSamples();
        if (n < 2)
            return 0;
        position = juce::jlimit (0, n - 1, position);
        const auto* d = b.getReadPointer (0);
        auto crossesAt = [d] (int i) { return (d[i - 1] <= 0.0f && d[i] > 0.0f) || (d[i - 1] >= 0.0f && d[i] < 0.0f) || d[i] == 0.0f; };
        for (int off = 0; off < n; ++off)
        {
            if (position - off >= 1 && crossesAt (position - off)) return position - off;
            if (position + off < n && position + off >= 1 && crossesAt (position + off)) return position + off;
            if (position - off < 1 && position + off >= n) break;
        }
        return position;
    }

    juce::String noteName (int note)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((note % 12) + 12) % 12]) + juce::String (note / 12 - 2);
    }

    NameInfo parseName (const juce::String& fileName)
    {
        NameInfo info;
        const auto base = juce::File::createFileWithoutCheckingPath (fileName).getFileNameWithoutExtension();
        juce::StringArray tokens;
        tokens.addTokens (base, "_- .", "");
        tokens.removeEmptyStrings();

        static const juce::StringArray dynamics { "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff" };
        for (const auto& t : tokens)
        {
            const auto lower = t.toLowerCase();
            if (lower.startsWith ("rr") && lower.substring (2).containsOnly ("0123456789") && lower.length() > 2)
            {
                info.rr = lower.substring (2).getIntValue();
                continue;
            }
            if (dynamics.contains (lower))
            {
                info.dynamic = dynamics.indexOf (lower);
                continue;
            }
            for (const auto* prefix : { "vel", "v" })
                if (lower.startsWith (prefix) && lower.length() > (int) strlen (prefix) && lower.substring ((int) strlen (prefix)).containsOnly ("0123456789"))
                    info.dynamic = 100 + lower.substring ((int) strlen (prefix)).getIntValue();   // after the named dynamics

            // Note names: C3, F#2, Db4, Ds2 (s = sharp), C-1. C3 = 60.
            if (info.note < 0 && t.length() >= 2 && t.length() <= 4)
            {
                const auto letter = juce::CharacterFunctions::toUpperCase (t[0]);
                const int pcs[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
                if (letter < 'A' || letter > 'G')
                    continue;
                int pc = pcs[letter - 'A'];
                int i = 1;
                if (t[i] == '#' || t[i] == 's' || t[i] == 'S' || t[i] == 0x266F) { ++pc; ++i; }
                else if ((t[i] == 'b' || t[i] == 0x266D) && t.length() > 2 && (juce::CharacterFunctions::isDigit (t[2]) || t[2] == '-')) { --pc; ++i; }
                const auto octave = t.substring (i);
                if (octave.isEmpty() || ! (octave.containsOnly ("0123456789-")) || octave == "-")
                    continue;
                const int note = (octave.getIntValue() + 2) * 12 + pc;
                if (note >= 0 && note <= 127)
                    info.note = note;
            }
        }
        return info;
    }

    juce::Array<juce::File> audioFilesIn (const juce::File& folder)
    {
        juce::Array<juce::File> files;
        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
            if (entry.getFile().hasFileExtension ("wav;aif;aiff;flac;ogg;mp3"))
                files.add (entry.getFile());
        files.sort();
        return files;
    }

    // ---------------- instrument edits ----------------
    namespace
    {
        juce::Array<juce::var>* layersOf (juce::var& instrument)
        {
            if (! instrument.isObject())
                instrument = juce::var (new juce::DynamicObject());
            if (! instrument["layers"].isArray())
                instrument.getDynamicObject()->setProperty ("layers", juce::Array<juce::var>());
            return instrument["layers"].getArray();
        }

        void set (juce::var& o, const juce::Identifier& k, const juce::var& v) { o.getDynamicObject()->setProperty (k, v); }

        // Spreads velocity tiers over 0–127 and key zones between neighbouring roots, per group.
        void spreadZones (juce::Array<juce::var>& layers, const std::vector<int>& which, const std::map<int, int>& tierOf)
        {
            // tierOf: layer index → velocity ordering key (or -1)
            std::map<juce::String, std::vector<int>> groups;
            for (auto i : which)
                groups[layers.getReference (i)["group"].toString()].push_back (i);

            for (auto& [groupName, members] : groups)
            {
                juce::ignoreUnused (groupName);
                std::vector<int> tiers;
                for (auto i : members)
                {
                    const auto t = tierOf.at (i);
                    if (std::find (tiers.begin(), tiers.end(), t) == tiers.end())
                        tiers.push_back (t);
                }
                std::sort (tiers.begin(), tiers.end());
                const int numTiers = (int) tiers.size();

                for (int k = 0; k < numTiers; ++k)
                {
                    const int vlo = k == 0 ? 0 : juce::roundToInt (k * 128.0 / numTiers);
                    const int vhi = k == numTiers - 1 ? 127 : juce::roundToInt ((k + 1) * 128.0 / numTiers) - 1;
                    std::vector<int> tier;
                    for (auto i : members)
                        if (tierOf.at (i) == tiers[(size_t) k])
                            tier.push_back (i);
                    std::sort (tier.begin(), tier.end(), [&] (int a, int b) { return (int) layers.getReference (a)["root"] < (int) layers.getReference (b)["root"]; });

                    for (size_t j = 0; j < tier.size(); ++j)
                    {
                        auto& l = layers.getReference (tier[j]);
                        const int root = l["root"];
                        const int lo = j == 0 ? juce::jmax (0, root - 12) : ((int) layers.getReference (tier[j - 1])["root"] + root) / 2 + 1;
                        const int hi = j + 1 == tier.size() ? juce::jmin (127, root + 12) : (root + (int) layers.getReference (tier[j + 1])["root"]) / 2;
                        set (l, "lo", juce::jmin (lo, root));
                        set (l, "hi", juce::jmax (hi, root));
                        set (l, "vlo", vlo);
                        set (l, "vhi", vhi);
                    }
                }
            }
        }

        juce::var newLayer (int id, const juce::String& name, const juce::String& group, int root, const juce::Array<juce::var>& samples)
        {
            auto l = juce::JSON::parse (R"({ "keyFade": 0, "velFade": 0, "keyTrack": 100, "rrMode": "off", "rrGroup": "", "rrReset": "idle",
                "volume": 0, "pan": 0, "output": "Main", "muted": false, "solo": false, "trigger": "attack", "releaseVolume": -6,
                "releaseDecay": 600, "velToVol": 80, "polyphony": 16, "choke": 0, "steal": "oldest", "voiceMode": "poly", "glide": 0,
                "keyswitch": -1, "articulation": "All" })");
            set (l, "id", id);
            set (l, "name", name);
            set (l, "group", group);
            set (l, "root", root);
            set (l, "lo", root);
            set (l, "hi", root);
            set (l, "vlo", 0);
            set (l, "vhi", 127);
            set (l, "samples", samples);
            if (samples.size() > 1)
            {
                set (l, "rrMode", "cycle");
                set (l, "rrGroup", "RR-" + group);
            }
            return l;
        }
    }

    juce::String autoMap (juce::var& instrument)
    {
        auto& layers = *layersOf (instrument);
        std::vector<int> mapped;
        std::map<int, int> tierOf;
        for (int i = 0; i < layers.size(); ++i)
        {
            auto& l = layers.getReference (i);
            const auto* samples = l["samples"].getArray();
            if (samples == nullptr || samples->isEmpty())
                continue;
            const auto info = parseName ((*samples)[0].toString());
            if (info.note < 0)
                continue;
            set (l, "root", info.note);
            mapped.push_back (i);
            tierOf[i] = info.dynamic;
        }
        spreadZones (layers, mapped, tierOf);
        return mapped.empty() ? juce::String ("No note names found in the sample names (like C3, F#2 or Ds2)")
                              : "Mapped " + juce::String ((int) mapped.size()) + " layer" + (mapped.size() == 1 ? "" : "s") + " by name";
    }

    juce::String fillGaps (juce::var& instrument)
    {
        auto& layers = *layersOf (instrument);
        // Bands: same group and same velocity range
        std::map<juce::String, std::vector<int>> bands;
        for (int i = 0; i < layers.size(); ++i)
        {
            const auto& l = layers.getReference (i);
            bands[l["group"].toString() + "|" + l["vlo"].toString() + "|" + l["vhi"].toString()].push_back (i);
        }
        int closed = 0;
        for (auto& [key, members] : bands)
        {
            juce::ignoreUnused (key);
            std::sort (members.begin(), members.end(), [&] (int a, int b) { return (int) layers.getReference (a)["lo"] < (int) layers.getReference (b)["lo"]; });
            for (size_t j = 0; j + 1 < members.size(); ++j)
            {
                auto& a = layers.getReference (members[j]);
                auto& b = layers.getReference (members[j + 1]);
                const int aHi = a["hi"], bLo = b["lo"];
                if (bLo - aHi <= 1)
                    continue;
                // Split the gap between the two roots (or in the middle when the roots are outside it)
                const int ra = a["root"], rb = b["root"];
                int split = (juce::jlimit (aHi, bLo, ra) + juce::jlimit (aHi, bLo, rb)) / 2;
                split = juce::jlimit (aHi, bLo - 1, split);
                set (a, "hi", split);
                set (b, "lo", split + 1);
                ++closed;
            }
        }
        return closed == 0 ? juce::String ("No gaps to fill") : "Filled " + juce::String (closed) + " gap" + (closed == 1 ? "" : "s");
    }

    juce::String importFiles (juce::var& instrument, const juce::Array<juce::File>& files, const juce::String& group)
    {
        auto& layers = *layersOf (instrument);
        if (files.isEmpty())
            return "No audio files found";

        // Group files by (note, dynamic): each pair becomes one layer, its files the round robins
        struct Key { int note, dynamic; bool operator< (const Key& o) const { return std::tie (note, dynamic) < std::tie (o.note, o.dynamic); } };
        std::map<Key, std::vector<std::pair<int, juce::File>>> byZone;
        int unpitched = 0;
        for (const auto& f : files)
        {
            auto info = parseName (f.getFileName());
            if (info.note < 0)
                info.note = -1 - unpitched++;   // one layer each; placed on consecutive keys below
            byZone[{ info.note, info.dynamic }].push_back ({ info.rr, f });
        }

        int nextId = (int) (juce::Time::currentTimeMillis() % 1000000000);
        int drumKey = 36;   // unpitched files (drums, one-shots) go on consecutive keys from C1
        std::vector<int> pitched;
        std::map<int, int> tierOf;
        for (auto& [key, list] : byZone)
        {
            std::sort (list.begin(), list.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
            juce::Array<juce::var> samples;
            for (const auto& [rr, f] : list)
            {
                juce::ignoreUnused (rr);
                samples.add (f.getFullPathName());
            }
            const bool hasNote = key.note >= 0;
            const int root = hasNote ? key.note : juce::jmin (127, drumKey++);
            auto name = list.front().second.getFileNameWithoutExtension();
            if (list.size() > 1)
                name = name.upToLastOccurrenceOf ("_rr", false, true);
            layers.add (newLayer (nextId++, name, group, root, samples));
            if (hasNote)
            {
                pitched.push_back (layers.size() - 1);
                tierOf[layers.size() - 1] = key.dynamic;
            }
            else
                set (layers.getReference (layers.size() - 1), "keyTrack", 0);
        }
        spreadZones (layers, pitched, tierOf);
        return "Imported " + juce::String (files.size()) + " file" + (files.size() == 1 ? "" : "s") + " into "
               + juce::String ((int) byZone.size()) + " layer" + (byZone.size() == 1 ? "" : "s");
    }
}
