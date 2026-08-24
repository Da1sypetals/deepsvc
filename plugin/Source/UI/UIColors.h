#pragma once

#include <juce_graphics/juce_graphics.h>

// 设计令牌：梅粉单色阶浅色界面（docs/ara.md 设计语言章节）
namespace deepsvc
{

struct UIColors
{
    static constexpr int scrollBarThickness = 12;
    static constexpr float panelCornerRadius = 8.0f;
    static constexpr float controlCornerRadius = 6.0f;

    // 粉色阶
    static inline juce::Colour pink050 { 0xFFFDF6F8 };  // 窗口背景、下拉面板底色
    static inline juce::Colour pink100 { 0xFFFBEAF0 };  // 面板背景、列表行悬停
    static inline juce::Colour pink200 { 0xFFF3D3DF };  // 边框、分隔线、滑杆轨道、进度条轨道
    static inline juce::Colour pink300 { 0xFFE9B7C9 };  // 钢琴卷小节线、波形包络
    static inline juce::Colour pink500 { 0xFFC96382 };  // 主色悬停
    static inline juce::Colour pink600 { 0xFFB5446E };  // 主色：主按钮、选中态、滑杆填充、F0 曲线
    static inline juce::Colour pink700 { 0xFF96365A };  // 主色按下
    static inline juce::Colour pink900 { 0xFF5C2438 };  // 播放头、强调元素

    // 墨色阶
    static inline juce::Colour ink900 { 0xFF3A2931 };   // 主要文字
    static inline juce::Colour ink600 { 0xFF7A5C68 };   // 次要文字
    static inline juce::Colour ink300 { 0xFFB99AA6 };   // 禁用文字、失效状态的 F0 曲线

    // 功能色：只出现在状态栏与徽标
    static inline juce::Colour success { 0xFF3E9B6D };
    static inline juce::Colour warning { 0xFFC98A2D };
    static inline juce::Colour warningBg { 0xFFF9EEDD };
    static inline juce::Colour failure { 0xFFD0454C };
    static inline juce::Colour failureBg { 0xFFFBE7E8 };

    // 钢琴键
    static inline juce::Colour keyWhite { 0xFFFFFFFF };
    static inline juce::Colour keyBlack { 0xFF4A3540 };

    // 面板：pink100 平铺 + 1px pink200 边框，无投影
    static void fillPanelBackground (juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        g.setColour (pink100);
        g.fillRoundedRectangle (bounds, radius);
    }

    static void drawPanelFrame (juce::Graphics& g, const juce::Rectangle<float>& bounds, float radius)
    {
        g.setColour (pink200);
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
    }
};

} // namespace deepsvc
