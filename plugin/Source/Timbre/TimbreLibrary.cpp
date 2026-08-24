#include "TimbreLibrary.h"

namespace deepsvc
{

namespace
{
const juce::StringArray& supportedExtensions()
{
    static const juce::StringArray extensions { ".wav", ".aif", ".aiff", ".mp3", ".flac", ".ogg", ".m4a" };
    return extensions;
}
} // namespace

TimbreLibrary::TimbreLibrary (juce::File root)
    : rootDirectory (std::move (root))
{
    rootDirectory.createDirectory();
    currentEntries = scanDirectory();
    startTimer (2000);
}

TimbreLibrary::~TimbreLibrary()
{
    stopTimer();
}

bool TimbreLibrary::isSupportedAudioFile (const juce::File& file)
{
    return supportedExtensions().contains (file.getFileExtension().toLowerCase());
}

juce::File TimbreLibrary::fileFor (const juce::String& fileName) const
{
    return rootDirectory.getChildFile (fileName);
}

std::vector<juce::String> TimbreLibrary::scanDirectory() const
{
    std::vector<juce::String> found;
    for (const auto& file : rootDirectory.findChildFiles (juce::File::findFiles, false))
        if (isSupportedAudioFile (file))
            found.push_back (file.getFileName());

    std::sort (found.begin(), found.end());
    return found;
}

void TimbreLibrary::refresh()
{
    auto found = scanDirectory();
    if (found != currentEntries)
    {
        currentEntries = std::move (found);
        sendChangeMessage();
    }
}

void TimbreLibrary::timerCallback()
{
    refresh();
}

std::vector<juce::String> TimbreLibrary::importFiles (const juce::Array<juce::File>& files)
{
    std::vector<juce::String> imported;
    for (const auto& file : files)
    {
        if (! file.existsAsFile() || ! isSupportedAudioFile (file))
            continue;

        auto target = rootDirectory.getChildFile (file.getFileName());
        if (target.existsAsFile())
        {
            // 同名已存在：追加序号
            const auto base = file.getFileNameWithoutExtension();
            const auto extension = file.getFileExtension();
            int suffix = 2;
            do
            {
                target = rootDirectory.getChildFile (base + " " + juce::String (suffix++) + extension);
            } while (target.existsAsFile());
        }

        if (file.copyFileTo (target))
            imported.push_back (target.getFileName());
    }

    if (! imported.empty())
        refresh();
    return imported;
}

bool TimbreLibrary::removeEntry (const juce::String& fileName)
{
    const auto file = fileFor (fileName);
    if (! file.existsAsFile())
        return false;

    if (! file.moveToTrash())
        return false;

    refresh();
    return true;
}

bool TimbreLibrary::renameEntry (const juce::String& from, const juce::String& to)
{
    const auto source = fileFor (from);
    auto target = fileFor (to);
    if (! source.existsAsFile() || to.isEmpty() || target.existsAsFile())
        return false;

    // 保留原扩展名
    if (target.getFileExtension().isEmpty())
        target = target.withFileExtension (source.getFileExtension());

    if (! source.moveFileTo (target))
        return false;

    refresh();
    return true;
}

} // namespace deepsvc