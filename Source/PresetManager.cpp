#include "PresetManager.h"
#include "PluginProcessor.h"
#include "UiResources.h"

PresetManager::PresetManager (MtyaSamplerProcessor& p) : proc (p) {}

juce::File PresetManager::folder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("mtyaSampler").getChildFile ("Presets");
}

juce::var PresetManager::result (bool ok, const juce::String& message)
{
    auto* r = new juce::DynamicObject();
    r->setProperty ("ok", ok);
    r->setProperty ("message", message);
    return juce::var (r);
}

void PresetManager::setCurrent (const juce::String& newName, const juce::String& newCategory, const juce::String& newPath)
{
    name = newName;
    category = newCategory;
    path = newPath;
    if (onChange)
        onChange();
}

juce::var PresetManager::list() const
{
    juce::Array<juce::File> files;
    const auto root = folder();
    if (root.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator (root, true, juce::String ("*") + extension, juce::File::findFiles))
            files.add (entry.getFile());

    struct Item { juce::String category, name, path; };
    std::vector<Item> items;
    for (const auto& f : files)
    {
        const auto parent = f.getParentDirectory();
        items.push_back ({ parent == root ? juce::String() : parent.getRelativePathFrom (root), f.getFileNameWithoutExtension(), f.getFullPathName() });
    }
    std::sort (items.begin(), items.end(), [] (const Item& a, const Item& b)
    {
        const auto c = a.category.compareNatural (b.category);
        return c != 0 ? c < 0 : a.name.compareNatural (b.name) < 0;
    });

    juce::Array<juce::var> out;
    for (const auto& i : items)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", i.name);
        o->setProperty ("category", i.category);
        o->setProperty ("path", i.path);
        out.add (juce::var (o));
    }
    return out;
}

juce::var PresetManager::save (const juce::String& newName, const juce::String& newCategory)
{
    const auto cleanName = juce::File::createLegalFileName (newName.trim());
    if (cleanName.isEmpty())
        return result (false, "Give the preset a name");
    const auto cleanCategory = juce::File::createLegalFileName (newCategory.trim());

    auto* params = new juce::DynamicObject();
    for (auto* p : proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            params->setProperty (ranged->getParameterID(), ranged->convertFrom0to1 (ranged->getValue()));

    auto* preset = new juce::DynamicObject();
    preset->setProperty ("format", "mtyaSampler preset");
    preset->setProperty ("version", 1);
    preset->setProperty ("name", newName.trim());
    preset->setProperty ("category", newCategory.trim());
    preset->setProperty ("params", juce::var (params));
    preset->setProperty ("instrument", juce::JSON::parse (proc.getInstrumentJson()));

    auto dir = cleanCategory.isEmpty() ? folder() : folder().getChildFile (cleanCategory);
    if (! dir.createDirectory())
        return result (false, "Couldn't create " + dir.getFullPathName());
    const auto file = dir.getChildFile (cleanName + extension);
    if (! file.replaceWithText (juce::JSON::toString (juce::var (preset))))
        return result (false, "Couldn't write " + file.getFullPathName());

    setCurrent (newName.trim(), newCategory.trim(), file.getFullPathName());
    return result (true, "Saved “" + newName.trim() + "”");
}

juce::var PresetManager::load (const juce::File& file)
{
    const auto json = juce::JSON::parse (file.loadFileAsString());
    if (! json.isObject() || ! json["instrument"].isObject())
        return result (false, "“" + file.getFileName() + "” isn't an mtyaSampler preset");

    // Parameters the preset doesn't mention go back to their defaults, so every preset sounds the same each time.
    const auto& values = json["params"];
    for (auto* p : proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto& v = values[juce::Identifier (ranged->getParameterID())];
            const auto norm = v.isVoid() ? ranged->getDefaultValue() : ranged->convertTo0to1 ((float) (double) v);
            ranged->beginChangeGesture();
            ranged->setValueNotifyingHost (norm);
            ranged->endChangeGesture();
        }

    proc.setInstrumentJson (juce::JSON::toString (json["instrument"]), true);
    const auto rel = file.getParentDirectory() == folder() ? juce::String() : file.getParentDirectory().getRelativePathFrom (folder());
    setCurrent (json["name"].toString().isNotEmpty() ? json["name"].toString() : file.getFileNameWithoutExtension(),
                json["category"].isVoid() ? rel : json["category"].toString(), file.getFullPathName());
    return result (true, "Loaded “" + name + "”");
}

juce::var PresetManager::step (int delta)
{
    const auto all = list();
    const auto* items = all.getArray();
    if (items == nullptr || items->isEmpty())
        return result (false, "No presets yet: press Save to make one");
    int index = -1;
    for (int i = 0; i < items->size(); ++i)
        if ((*items)[i]["path"].toString() == path)
            index = i;
    const int n = items->size();
    const int next = index < 0 ? (delta > 0 ? 0 : n - 1) : ((index + delta) % n + n) % n;
    return load (juce::File ((*items)[next]["path"].toString()));
}

juce::var PresetManager::init (const juce::String& newName, const juce::String& instrumentJson)
{
    const auto clean = juce::JSON::parse (UiResources::getText ("clean-init.json"));
    const auto& values = clean["params"];
    for (auto* p : proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto& v = values[juce::Identifier (ranged->getParameterID())];
            ranged->beginChangeGesture();
            ranged->setValueNotifyingHost (v.isVoid() ? ranged->getDefaultValue() : ranged->convertTo0to1 ((float) (double) v));
            ranged->endChangeGesture();
        }
    const auto given = juce::JSON::parse (instrumentJson);
    const auto instrument = juce::JSON::toString (given.isObject() ? given : clean["instrument"]);
    proc.setInstrumentJson (instrument, true);
    setCurrent (newName.isNotEmpty() ? newName : juce::String ("Init"), {}, {});

    auto r = result (true, newName == "Init" ? juce::String ("Clean Init sound: no effects, no layers") : "New instrument “" + newName + "”");
    r.getDynamicObject()->setProperty ("instrument", instrument);
    return r;
}
