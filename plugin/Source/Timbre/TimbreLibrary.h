#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

// 音色库：一个目录中的音频文件集合，与文件系统双向同步。
// OpenTune 无对应物：音色库是本插件独有功能（docs/ara.md）
// OpenTune 无对应物：音色库是本插件独有功能（docs/ara.md）
// OpenTune 无对应物：音色库是本插件独有功能（docs/ara.md）
namespace deepsvc
{

class TimbreLibrary : public juce::ChangeBroadcaster
                    , private juce::Timer
{
public:
    explicit TimbreLibrary (juce::File rootDirectory);
    ~TimbreLibrary() override;

    const juce::File& root() const noexcept { return rootDirectory; }

    // 目录中的音频文件名（相对路径，按名称排序）
    const std::vector<juce::String>& entries() const noexcept { return currentEntries; }

    juce::File fileFor (const juce::String& fileName) const;

    // 复制外部文件进库；返回实际入库的文件名
    std::vector<juce::String> importFiles (const juce::Array<juce::File>& files);
    bool removeEntry (const juce::String& fileName);
    bool renameEntry (const juce::String& from, const juce::String& to);

    // 立即重扫目录
    void refresh();

    static bool isSupportedAudioFile (const juce::File& file);

private:
    void timerCallback() override;
    std::vector<juce::String> scanDirectory() const;

    juce::File rootDirectory;
    std::vector<juce::String> currentEntries;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbreLibrary)
};

} // namespace deepsvc
