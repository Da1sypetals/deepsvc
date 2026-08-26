#include "TimbrePanel.h"

#include "UIColors.h"

// OpenTune 无对应物：音色库是本插件独有功能（docs/ara.md）
// OpenTune 无对应物：音色库是本插件独有功能（docs/ara.md）
// OpenTune 无对应物：音色库是本插件独有功能（docs/ara.md）
namespace deepsvc
{

// 矢量图标按钮：文件夹 / 删除叉
class TimbrePanel::IconButton : public juce::Button
{
public:
    enum class Icon { folder, close };

    explicit IconButton (Icon icon)
        : juce::Button ({})
        , iconKind (icon)
    {
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto colour = UIColors::ink600;
        if (down)
            colour = UIColors::pink700;
        else if (highlighted)
            colour = iconKind == Icon::close ? UIColors::failure : UIColors::pink600;

        const auto area = getLocalBounds().toFloat().reduced (getWidth() * 0.28f);
        g.setColour (colour);

        if (iconKind == Icon::close)
        {
            g.drawLine ({ area.getX(), area.getY(), area.getRight(), area.getBottom() }, 1.5f);
            g.drawLine ({ area.getX(), area.getBottom(), area.getRight(), area.getY() }, 1.5f);
            return;
        }

        // 文件夹：主体圆角矩形 + 左上小页签
        const float x = area.getX();
        const float y = area.getY();
        const float w = area.getWidth();
        const float h = area.getHeight();
        g.fillRoundedRectangle (x, y + h * 0.25f, w, h * 0.75f, 2.0f);
        g.fillRoundedRectangle (x, y, w * 0.45f, h * 0.5f, 2.0f);
    }

private:
    Icon iconKind;
};

// 列表行：文件名 + 右侧删除按钮；单击选中，双击进入行内重命名
class TimbrePanel::RowComponent : public juce::Component
{
public:
    RowComponent (TimbrePanel& panel, int row)
        : owner (panel)
    {
        deleteButton = std::make_unique<IconButton> (IconButton::Icon::close);
        deleteButton->setTooltip (juce::String (u8"删除"));
        deleteButton->onClick = [this]
        {
            owner.confirmDeleteRow (rowNumber);
        };
        addAndMakeVisible (*deleteButton);

        nameEditor = std::make_unique<juce::TextEditor>();
        nameEditor->setFont (juce::Font (juce::FontOptions (13.0f)));
        nameEditor->setJustification (juce::Justification::centredLeft);
        nameEditor->setIndents (10, 0);
        nameEditor->setColour (juce::TextEditor::backgroundColourId, UIColors::pink050);
        nameEditor->setColour (juce::TextEditor::textColourId, UIColors::ink900);
        nameEditor->setColour (juce::TextEditor::outlineColourId, UIColors::pink600);
        nameEditor->setColour (juce::TextEditor::focusedOutlineColourId, UIColors::pink600);
        nameEditor->onReturnKey = [this] { commitRename(); };
        nameEditor->onFocusLost = [this] { commitRename(); };
        nameEditor->onEscapeKey = [this] { nameEditor->setVisible (false); };
        addChildComponent (*nameEditor);

        update (row);
    }

    void update (int row)
    {
        nameEditor->setVisible (false);
        rowNumber = row;
        const auto& entries = owner.library.entries();
        fileName = row >= 0 && row < static_cast<int> (entries.size())
            ? entries[static_cast<size_t> (row)] : juce::String();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const bool selected = owner.listBox.isRowSelected (rowNumber);
        if (selected)
        {
            g.setColour (UIColors::pink200);
            g.fillRect (getLocalBounds());
            g.setColour (UIColors::pink600);
            g.fillRect (0, 0, 3, getHeight());
        }
        else if (isMouseOver())
        {
            g.setColour (UIColors::pink100);
            g.fillRect (getLocalBounds());
        }

        g.setColour (selected ? UIColors::ink900 : UIColors::ink600);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (juce::File (fileName).getFileNameWithoutExtension(),
                    10, 0, getWidth() - 36, getHeight(),
                    juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        deleteButton->setBounds (getWidth() - 26, 0, 26, getHeight());
        nameEditor->setBounds (0, 0, getWidth() - 30, getHeight());
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        owner.rowClicked (rowNumber);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (fileName.isEmpty())
            return;
        nameEditor->setText (juce::File (fileName).getFileNameWithoutExtension(), false);
        nameEditor->setVisible (true);
        nameEditor->grabKeyboardFocus();
        nameEditor->selectAll();
    }

private:
    void commitRename()
    {
        if (! nameEditor->isVisible())
            return;

        auto newName = nameEditor->getText().trim();
        nameEditor->setVisible (false);

        const auto oldName = fileName;
        const auto oldBase = juce::File (oldName).getFileNameWithoutExtension();
        if (newName.isEmpty() || newName == oldBase)
            return;

        newName = juce::File::createLegalFileName (newName);
        newName << juce::File (oldName).getFileExtension();
        owner.library.renameEntry (oldName, newName);
    }

    TimbrePanel& owner;
    int rowNumber = -1;
    juce::String fileName;
    std::unique_ptr<IconButton> deleteButton;
    std::unique_ptr<juce::TextEditor> nameEditor;
};

TimbrePanel::TimbrePanel (TimbreLibrary& libraryRef)
    : library (libraryRef)
{
    listBox.setModel (this);
    listBox.setRowHeight (28);
    listBox.setColour (juce::ListBox::backgroundColourId, UIColors::pink050);
    addAndMakeVisible (listBox);

    openFolderButton = std::make_unique<IconButton> (IconButton::Icon::folder);
    openFolderButton->setTooltip (juce::String (u8"打开文件夹"));
    openFolderButton->onClick = [this]
    {
        library.root().startAsProcess();
    };
    addAndMakeVisible (*openFolderButton);

    hintLabel.setText (juce::String (u8"拖入音频文件即可导入"), juce::dontSendNotification);
    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setColour (juce::Label::textColourId, UIColors::ink300);
    addChildComponent (hintLabel);

    library.addChangeListener (this);
}

TimbrePanel::~TimbrePanel()
{
    library.removeChangeListener (this);
}

juce::String TimbrePanel::selectedTimbre() const
{
    const int row = listBox.getSelectedRow();
    const auto& entries = library.entries();
    if (row < 0 || row >= static_cast<int> (entries.size()))
        return {};
    return entries[static_cast<size_t> (row)];
}

void TimbrePanel::selectTimbre (const juce::String& fileName)
{
    const auto& entries = library.entries();
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i] == fileName)
        {
            listBox.selectRow (static_cast<int> (i));
            return;
        }
}

int TimbrePanel::getNumRows()
{
    return static_cast<int> (library.entries().size());
}

void TimbrePanel::paintListBoxItem (int, juce::Graphics&, int, int, bool)
{
    // 行由 RowComponent 绘制
}

juce::Component* TimbrePanel::refreshComponentForRow (int rowNumber, bool isRowSelected,
                                                      juce::Component* existingComponentToUpdate)
{
    juce::ignoreUnused (isRowSelected);

    auto* row = dynamic_cast<RowComponent*> (existingComponentToUpdate);
    if (row == nullptr)
        row = new RowComponent (*this, rowNumber);
    else
        row->update (rowNumber);
    return row;
}

void TimbrePanel::rowClicked (int rowNumber)
{
    listBox.selectRow (rowNumber);
}

void TimbrePanel::confirmDeleteRow (int rowNumber)
{
    const auto& entries = library.entries();
    if (rowNumber < 0 || rowNumber >= static_cast<int> (entries.size()))
        return;

    const auto target = entries[static_cast<size_t> (rowNumber)];

    auto* window = new juce::AlertWindow (juce::String (u8"删除音色"),
                                          juce::String (u8"确定删除「")
                                              + juce::File (target).getFileNameWithoutExtension()
                                              + juce::String (u8"」吗？"),
                                          juce::MessageBoxIconType::WarningIcon,
                                          getTopLevelComponent());
    window->setLookAndFeel (&getLookAndFeel());
    window->addButton (juce::String (u8"删除"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (juce::String (u8"取消"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
                             juce::ModalCallbackFunction::create (
                                 [this, target, window] (int result)
    {
        if (result == 1)
            library.removeEntry (target);
        delete window;
    }),
                             true);
}

void TimbrePanel::selectedRowsChanged (int)
{
    if (onSelectionChanged)
        onSelectionChanged (selectedTimbre());
}

void TimbrePanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // 目录内容变化后保留选中项
    const auto selected = selectedTimbre();
    listBox.updateContent();
    if (selected.isNotEmpty())
        selectTimbre (selected);
    hintLabel.setVisible (library.entries().empty());
    listBox.repaint();
}

bool TimbrePanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& file : files)
        if (TimbreLibrary::isSupportedAudioFile (juce::File (file)))
            return true;
    return false;
}

void TimbrePanel::fileDragEnter (const juce::StringArray&, int, int)
{
    setFileDragActive (true);
}

void TimbrePanel::fileDragExit (const juce::StringArray&)
{
    setFileDragActive (false);
}

void TimbrePanel::filesDropped (const juce::StringArray& files, int, int)
{
    setFileDragActive (false);
    juce::Array<juce::File> fileList;
    for (const auto& file : files)
        fileList.add (juce::File (file));
    library.importFiles (fileList);
}

void TimbrePanel::setFileDragActive (bool active)
{
    if (fileDragActive == active)
        return;

    fileDragActive = active;
    listBox.setColour (juce::ListBox::backgroundColourId,
                       active ? UIColors::pink100 : UIColors::pink050);
    hintLabel.setText (active ? juce::String (u8"松开鼠标导入音色")
                              : juce::String (u8"拖入音频文件即可导入"),
                       juce::dontSendNotification);
    hintLabel.setColour (juce::Label::textColourId,
                         active ? UIColors::pink700 : UIColors::ink300);
    hintLabel.setVisible (active || library.entries().empty());
    repaint();
}

void TimbrePanel::paint (juce::Graphics& g)
{
    UIColors::fillPanelBackground (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);
    if (fileDragActive)
    {
        g.setColour (UIColors::pink600);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.25f),
                                UIColors::panelCornerRadius, 2.5f);
    }
    else
    {
        UIColors::drawPanelFrame (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);
    }

    g.setColour (UIColors::ink900);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawText (juce::String (u8"音色库"), 12, 8, getWidth() - 48, 22,
                juce::Justification::centredLeft, false);
}

void TimbrePanel::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto header = area.removeFromTop (30);
    openFolderButton->setBounds (header.removeFromRight (26).withSizeKeepingCentre (22, 22));

    area.removeFromTop (4);
    listBox.setBounds (area);
    hintLabel.setBounds (area.withSizeKeepingCentre (area.getWidth(), 24));
    hintLabel.setVisible (fileDragActive || library.entries().empty());
}

} // namespace deepsvc
