#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Timbre/TimbreLibrary.h"

// 音色库面板：标题 + 右上角打开文件夹图标；列表可滚动，
// 拖入文件导入，双击重命名，每行右侧删除按钮；与目录双向同步
namespace deepsvc
{

// FileDragAndDropTarget 必须 public 继承：ComponentPeer 通过 dynamic_cast 查找拖拽目标，
// 私有继承会让转换失败，外部文件拖不进来
class TimbrePanel : public juce::Component
                  , private juce::ListBoxModel
                  , private juce::ChangeListener
                  , public juce::FileDragAndDropTarget
{
public:
    explicit TimbrePanel (TimbreLibrary& library);
    ~TimbrePanel() override;

    // 当前选中的音色文件名（相对库目录）；无选中返回空
    juce::String selectedTimbre() const;
    void selectTimbre (const juce::String& fileName);

    // 选中变化时触发
    std::function<void (const juce::String&)> onSelectionChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class RowComponent;

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    juce::Component* refreshComponentForRow (int rowNumber, bool isRowSelected,
                                             juce::Component* existingComponentToUpdate) override;
    void selectedRowsChanged (int lastRowSelected) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void rowClicked (int rowNumber);
    void confirmDeleteRow (int rowNumber);

    TimbreLibrary& library;
    juce::ListBox listBox;
    juce::Label hintLabel;

    class IconButton;
    std::unique_ptr<IconButton> openFolderButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbrePanel)
};

} // namespace deepsvc
