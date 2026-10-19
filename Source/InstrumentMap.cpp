#include "InstrumentMap.h"

namespace
{
    float dbToGain (double db) { return juce::Decibels::decibelsToGain ((float) db, -100.0f); }

    // Fade gain at a low edge: `x` is the note or velocity, `edge` the first value inside the
    // range, `width` the fade width. The fade is centred on the boundary between edge-1 and edge,
    // so a neighbouring zone with the same width fades in exactly as this one fades out.
    float edgeGain (float x, float edge, float width)
    {
        if (width <= 0.0f)
            return x >= edge ? 1.0f : 0.0f;
        const auto t = juce::jlimit (0.0f, 1.0f, (x - (edge - 0.5f)) / width + 0.5f);
        return std::sin (t * juce::MathConstants<float>::halfPi);
    }

    template <typename Enum>
    Enum pick (const juce::var& v, std::initializer_list<std::pair<const char*, Enum>> options, Enum fallback)
    {
        const auto s = v.toString();
        for (const auto& [name, value] : options)
            if (s.equalsIgnoreCase (name))
                return value;
        return fallback;
    }

    int intOr (const juce::var& v, int fallback)       { return v.isVoid() ? fallback : (int) v; }
    double numOr (const juce::var& v, double fallback) { return v.isVoid() ? fallback : (double) v; }

    bool isAudioFile (const juce::File& f)
    {
        return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
    }
}

// ---------------- Layer ----------------
bool Layer::covers (int note, int velocity) const noexcept
{
    return zoneGain (note, velocity) > 0.0f;
}

float Layer::zoneGain (int note, int velocity) const noexcept
{
    const auto n = (float) note, v = (float) velocity;
    // No fade at the outer edges of the keyboard / velocity range: nothing lies beyond them.
    const auto kLo = lo <= 0 ? (n >= 0 ? 1.0f : 0.0f) : edgeGain (n, (float) lo, keyFade);
    const auto kHi = hi >= 127 ? 1.0f : edgeGain (-n, (float) -hi, keyFade);
    const auto vLo = vlo <= 1 ? 1.0f : edgeGain (v, (float) vlo, velFade);
    const auto vHi = vhi >= 127 ? 1.0f : edgeGain (-v, (float) -vhi, velFade);
    return kLo * kHi * vLo * vHi;
}

// ---------------- InstrumentMap ----------------
int InstrumentMap::applyVelocityCurve (int velocity) const noexcept
{
    const auto x = juce::jlimit (0.0f, 1.0f, (float) velocity / 127.0f);
    float y = x;
    switch (velocityCurve)
    {
        case Curve::soft:   y = std::pow (x, 0.6f); break;   // same curves as ui/js/layers.js
        case Curve::hard:   y = std::pow (x, 1.8f); break;
        case Curve::fixed:  y = 0.7f; break;
        case Curve::linear: break;
    }
    return juce::jlimit (1, 127, juce::roundToInt (y * 127.0f));
}

// ---------------- SampleCache ----------------
SampleCache::SampleCache()
{
    formats.registerBasicFormats();
}

juce::Array<juce::File> SampleCache::defaultSearchFolders()
{
    const auto base = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("mtyaSampler");
    return { base.getChildFile ("Recordings"), base.getChildFile ("Samples") };
}

juce::File SampleCache::resolve (const juce::String& name, const juce::File& extraFolder)
{
    if (juce::File::isAbsolutePath (name))
        return juce::File (name).existsAsFile() ? juce::File (name) : juce::File();

    if (extraFolder.isDirectory())
    {
        const auto f = extraFolder.getChildFile (name);
        if (f.existsAsFile())
            return f;
    }

    const auto key = juce::File::createFileWithoutCheckingPath (name).getFileName().toLowerCase();
    auto found = nameIndex.find (key);
    if (found != nameIndex.end() && found->second.existsAsFile())
        return found->second;

    // Not in the index: rescan the search folders, at most every few seconds (it walks the disk).
    const auto now = juce::Time::getMillisecondCounter();
    if (nameIndexTime == 0 || now - nameIndexTime > 3000)
    {
        nameIndexTime = now;
        nameIndex.clear();
        for (const auto& folder : defaultSearchFolders())
            if (folder.isDirectory())
                for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
                    if (isAudioFile (entry.getFile()))
                        nameIndex.emplace (entry.getFile().getFileName().toLowerCase(), entry.getFile());

        found = nameIndex.find (key);
        if (found != nameIndex.end())
            return found->second;
    }
    return {};
}

SamplePtr SampleCache::load (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0)
        return nullptr;

    // RAM only for now (disk streaming later). Cap at 10 minutes so a stray long file can't eat memory.
    const auto maxLength = (juce::int64) (reader->sampleRate * 600.0);
    const auto length = (int) juce::jmin (reader->lengthInSamples, maxLength);
    const auto channels = (int) juce::jlimit (1u, 2u, reader->numChannels);

    auto s = std::make_shared<SampleData>();
    s->audio.setSize (channels, length);
    reader->read (&s->audio, 0, length, 0, true, channels > 1);
    s->sampleRate = reader->sampleRate;
    s->peak = juce::jmax (1.0e-6f, s->audio.getMagnitude (0, length));
    s->path = file.getFullPathName();
    return s;
}

SamplePtr SampleCache::missingTone (int rootNote)
{
    const auto key = "~tone:" + juce::String (rootNote);
    if (auto it = cache.find (key); it != cache.end())
        return it->second;

    // 2 s of a quiet sine at the root pitch, with short fades so it can loop or end cleanly.
    constexpr double sr = 44100.0;
    const int length = (int) (sr * 2.0), fade = (int) (sr * 0.01);
    auto s = std::make_shared<SampleData>();
    s->audio.setSize (1, length);
    s->sampleRate = sr;
    s->missing = true;
    s->peak = 0.25f;
    const auto delta = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (rootNote) / sr;
    auto* d = s->audio.getWritePointer (0);
    for (int i = 0; i < length; ++i)
    {
        const auto ramp = juce::jmin (1.0f, (float) juce::jmin (i, length - 1 - i) / (float) fade);
        d[i] = 0.25f * ramp * (float) std::sin (delta * i);
    }
    cache[key] = s;
    return s;
}

SamplePtr SampleCache::get (const juce::String& name, const juce::File& extraFolder, int rootNote)
{
    const auto trimmed = name.trim();
    // New layers carry a placeholder such as "(drop a sample)".
    if (trimmed.isEmpty() || trimmed.startsWith ("("))
        return missingTone (rootNote);

    const auto file = resolve (trimmed, extraFolder);
    if (file == juce::File())
        return missingTone (rootNote);

    // Reuse a loaded file unless it changed on disk (e.g. rewritten by session normalize)
    const auto key = file.getFullPathName();
    const auto modified = file.getLastModificationTime();
    if (auto it = cache.find (key); it != cache.end() && loadedAt[key] == modified)
        return it->second;

    auto s = load (file);
    if (s == nullptr)
        return missingTone (rootNote);
    cache[key] = s;
    loadedAt[key] = modified;
    return s;
}

void SampleCache::prune()
{
    for (auto it = cache.begin(); it != cache.end();)
    {
        if (it->second.use_count() == 1)
        {
            loadedAt.erase (it->first);
            it = cache.erase (it);
        }
        else
            ++it;
    }
}

// ---------------- building a map ----------------
juce::var defaultModRoutes()
{
    // The fixed destinations the UI used to show, plus a starting point for the macros.
    return juce::JSON::parse (R"([
        { "source": "env3",   "target": "grain_size",     "amount": 50 },
        { "source": "lfo1",   "target": "grain_pos",      "amount": 50 },
        { "source": "lfo2",   "target": "pan",            "amount": 100 },
        { "source": "lfo3",   "target": "cutoff",         "amount": 50 },
        { "source": "lfo4",   "target": "grain_spray",    "amount": 100 },
        { "source": "macro1", "target": "grain_density",  "amount": 40 },
        { "source": "macro1", "target": "grain_spray",    "amount": 30 },
        { "source": "macro2", "target": "param:lfo1_rate",  "amount": 30 },
        { "source": "macro2", "target": "param:lfo1_depth", "amount": 40 },
        { "source": "macro3", "target": "param:fx_reverb_mix", "amount": 30 },
        { "source": "macro3", "target": "param:fx_delay_mix",  "amount": 20 },
        { "source": "macro4", "target": "drive",          "amount": 40 },
        { "source": "macro4", "target": "param:fx_sat_drive", "amount": 40 }
    ])");
}

static Layer::Region parseRegion (const juce::var& entry)
{
    Layer::Region r;
    if (! entry.isObject())
        return r;
    auto f = [&entry] (const char* key, float fallback)
    {
        const auto& v = entry[key];
        return v.isVoid() ? fallback : juce::jlimit (0.0f, 1.0f, (float) (double) v);
    };
    r.start = f ("start", r.start);
    r.end = f ("end", r.end);
    if (r.end < r.start)
        std::swap (r.start, r.end);
    r.loopStart = juce::jlimit (r.start, r.end, f ("loopStart", r.loopStart));
    r.loopEnd = juce::jlimit (r.start, r.end, f ("loopEnd", r.loopEnd));
    if (r.loopEnd < r.loopStart)
        std::swap (r.loopStart, r.loopEnd);
    return r;
}

static void parseRoutes (const juce::var& list, const juce::StringArray& paramIds, std::vector<ModRoute>& out)
{
    const auto* routes = list.getArray();
    if (routes == nullptr)
        return;
    for (const auto& r : *routes)
    {
        ModRoute route;
        const auto src = r["source"].toString();
        route.source = -1;
        for (int i = 0; i < Mod::numSources; ++i)
            if (src == Mod::sourceIds[i])
                route.source = i;
        const auto target = r["target"].toString();
        if (target.startsWith ("param:"))
        {
            route.param = paramIds.indexOf (target.substring (6));
        }
        else
            for (int i = 0; i < Mod::numTargets; ++i)
                if (target == Mod::targetIds[i])
                    route.target = i;
        route.amount = (float) juce::jlimit (-1.0, 1.0, numOr (r["amount"], 0) / 100.0);
        if (route.source >= 0 && (route.target >= 0 || route.param >= 0) && route.amount != 0.0f)
            out.push_back (route);
    }
}

std::unique_ptr<InstrumentMap> buildInstrumentMap (const juce::var& json, SampleCache& cache, const juce::StringArray& paramIds)
{
    auto map = std::make_unique<InstrumentMap>();
    map->keyswitchArticulation.fill (-1);

    map->velocityCurve = pick (json["velocityCurve"], { { "linear", InstrumentMap::Curve::linear }, { "soft", InstrumentMap::Curve::soft },
                                                        { "hard", InstrumentMap::Curve::hard }, { "fixed", InstrumentMap::Curve::fixed } },
                               InstrumentMap::Curve::linear);

    parseRoutes (json.hasProperty ("modRoutes") ? json["modRoutes"] : defaultModRoutes(), paramIds, map->routes);

    if (const auto* order = json["fxOrder"].getArray())
    {
        // Keep any valid order; effects missing from the list keep their place at the end.
        std::vector<int> seen;
        for (const auto& id : *order)
            for (int i = 0; i < numFxIds; ++i)
                if (id.toString() == fxIds[i] && std::find (seen.begin(), seen.end(), i) == seen.end())
                    seen.push_back (i);
        for (int i = 0; i < numFxIds; ++i)
            if (std::find (seen.begin(), seen.end(), i) == seen.end())
                seen.push_back (i);
        std::copy (seen.begin(), seen.end(), map->fxOrder.begin());
    }

    const auto folderText = json["sampleFolder"].toString();
    const auto extraFolder = juce::File::isAbsolutePath (folderText) ? juce::File (folderText) : juce::File();

    const auto* layers = json["layers"].getArray();
    if (layers == nullptr)
        return map;

    bool anySolo = false;
    for (const auto& l : *layers)
        anySolo = anySolo || (bool) l["solo"];

    juce::StringArray rrGroups, articulations;
    int index = 0;

    for (const auto& l : *layers)
    {
        ++index;
        if ((bool) l["muted"] || (anySolo && ! (bool) l["solo"]))
            continue;

        Layer layer;
        layer.id = intOr (l["id"], 100000 + index);

        layer.lo   = juce::jlimit (0, 127, intOr (l["lo"], 0));
        layer.hi   = juce::jlimit (layer.lo, 127, intOr (l["hi"], 127));
        layer.root = juce::jlimit (0, 127, intOr (l["root"], 60));
        layer.vlo  = juce::jlimit (0, 127, intOr (l["vlo"], 0));
        layer.vhi  = juce::jlimit (layer.vlo, 127, intOr (l["vhi"], 127));
        layer.keyFade  = (float) juce::jlimit (0.0, 48.0, numOr (l["keyFade"], 0));
        layer.velFade  = (float) juce::jlimit (0.0, 127.0, numOr (l["velFade"], 0));
        layer.keyTrack = (float) (numOr (l["keyTrack"], 100) / 100.0);

        using L = Layer;
        layer.rrMode  = pick (l["rrMode"], { { "off", L::RRMode::off }, { "cycle", L::RRMode::cycle }, { "random", L::RRMode::random },
                                             { "norepeat", L::RRMode::noRepeat } }, L::RRMode::off);
        layer.rrReset = pick (l["rrReset"], { { "never", L::RRReset::never }, { "idle", L::RRReset::idle },
                                              { "transport", L::RRReset::transport } }, L::RRReset::idle);
        layer.trigger = pick (l["trigger"], { { "attack", L::Trigger::attack }, { "release", L::Trigger::release },
                                              { "legato", L::Trigger::legato }, { "first", L::Trigger::first } }, L::Trigger::attack);
        layer.steal   = pick (l["steal"], { { "oldest", L::Steal::oldest }, { "quietest", L::Steal::quietest },
                                            { "lowest", L::Steal::lowest }, { "highest", L::Steal::highest } }, L::Steal::oldest);
        layer.voiceMode = pick (l["voiceMode"], { { "poly", L::VoiceMode::poly }, { "mono", L::VoiceMode::mono },
                                                  { "legato", L::VoiceMode::legato } }, L::VoiceMode::poly);

        // Layers with no RR group name each get their own counter.
        auto groupName = l["rrGroup"].toString().trim();
        if (groupName.isEmpty())
            groupName = "#layer" + juce::String (index);
        layer.rrGroup = rrGroups.indexOf (groupName);
        if (layer.rrGroup < 0)
        {
            layer.rrGroup = rrGroups.size();
            rrGroups.add (groupName);
            map->rrGroupSizes.push_back (0);
        }

        // Each entry is a file name, or { "file", "start", "end", "loopStart", "loopEnd" } (fractions 0–1)
        if (const auto* entries = l["samples"].getArray())
            for (const auto& e : *entries)
            {
                layer.samples.push_back (cache.get (e.isObject() ? e["file"].toString() : e.toString(), extraFolder, layer.root));
                layer.regions.push_back (parseRegion (e));
            }
        if (layer.samples.empty())
        {
            layer.samples.push_back (cache.get ({}, extraFolder, layer.root));
            layer.regions.push_back ({});
        }

        for (const auto& s : layer.samples)
            ++(s->missing ? map->missingSamples : map->loadedSamples);

        if ((bool) l["normalize"])
        {
            float peak = 0;
            for (const auto& smp : layer.samples)
                peak = juce::jmax (peak, smp->peak);
            layer.normalizeGain = peak > 0 ? juce::jmin (1000.0f, 1.0f / peak) : 1.0f;
        }

        auto& groupSize = map->rrGroupSizes[(size_t) layer.rrGroup];
        groupSize = juce::jmax (groupSize, (int) layer.samples.size());

        layer.gain = dbToGain (numOr (l["volume"], 0));
        const auto pan = (float) juce::jlimit (-1.0, 1.0, numOr (l["pan"], 0) / 50.0);
        layer.panL = juce::jmin (1.0f, 1.0f - pan);
        layer.panR = juce::jmin (1.0f, 1.0f + pan);
        const auto out = l["output"].toString();
        layer.output = out.startsWith ("Bus ") ? juce::jlimit (0, 3, out.getTrailingIntValue() - 1) : 0;

        layer.releaseGain    = dbToGain (numOr (l["releaseVolume"], -6));
        layer.releaseDecayMs = (float) juce::jmax (0.0, numOr (l["releaseDecay"], 600));
        layer.velToVol       = (float) juce::jlimit (0.0, 1.0, numOr (l["velToVol"], 80) / 100.0);

        layer.polyphony = juce::jlimit (1, 128, intOr (l["polyphony"], 16));
        layer.choke     = juce::jlimit (0, 16, intOr (l["choke"], 0));
        layer.glideMs   = (float) juce::jlimit (0.0, 5000.0, numOr (l["glide"], 0));

        layer.keyswitch = intOr (l["keyswitch"], -1);
        if (layer.keyswitch < 0 || layer.keyswitch > 127)
            layer.keyswitch = -1;
        if (layer.keyswitch >= 0)
        {
            const auto name = l["articulation"].toString().trim();
            layer.articulation = articulations.indexOf (name);
            if (layer.articulation < 0)
            {
                layer.articulation = articulations.size();
                articulations.add (name);
            }
            auto& slot = map->keyswitchArticulation[(size_t) layer.keyswitch];
            if (slot < 0)
                slot = layer.articulation;
            if (map->defaultKeyswitch < 0 || layer.keyswitch < map->defaultKeyswitch)
                map->defaultKeyswitch = layer.keyswitch;
        }

        map->layers.push_back (std::move (layer));
    }

    return map;
}
