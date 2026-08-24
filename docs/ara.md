# deepsvc ARA 插件

## 1. 功能

- ARA 2 Event FX 插件，目标宿主为 macOS 上的 Studio One（VST3 必需，AU 顺带构建）。
- 对事件音频做歌声转换：
  - 音高检测（RMVPE / FCPE）只覆盖当前选中音频块的内容窗口，钢琴卷只显示选中音频块的音高，其他音频块不显示；
  - 从音色库选择参考音频进行合成，合成结果替换事件声音参与回放；
  - 参数：F0 估计器、扩散步数、音高偏移、CFG 强度、输入增益、保留第一级声码器输出。
- 对比原声：按下后回放原声，弹起回放合成结果。
- 音色库：拖入文件导入，双击重命名，每行右侧删除按钮，右上角打开文件夹图标，与目录双向同步。
- 推理在插件进程内执行（`native/` 编译为 Rust 静态库直接链入插件，使用 yingmusic crate），模型进程级加载一次、全部插件实例复用；模型权重打入插件 bundle，libsoxr 等依赖全部静态链接，插件自包含。
- 插件状态随 ARA 文档归档保存，归档版本 0，不兼容任何其他格式。

## 2. 插件侧架构：一一对应 OpenTune 源码

插件（JUCE 侧）的每个组件 1:1 复刻 OpenTune（github.com/YuFeng926/OpenTune，本仓库 `.cache/reference/opentune/` 为完整克隆）的对应物。实现时直接阅读对应文件照其结构编写，不自行设计。

| 插件组件 | OpenTune 源码 |
| --- | --- |
| DocumentController | `Source/ARA/OpenTuneDocumentController.{h,cpp}` |
| PlaybackRenderer | `Source/ARA/OpenTunePlaybackRenderer.{h,cpp}` |
| EditorView | `Source/ARA/OpenTuneEditorView.{h,cpp}` |
| 编辑器 | `Source/Plugin/PluginEditor.{h,cpp}` |
| 钢琴卷 | `Source/Standalone/UI/PianoRollComponent.{h,cpp}` 与 `Source/Standalone/UI/PianoRoll/` |
| 时间线视口 | `Source/Standalone/UI/TimelineViewportCamera.h`、`Source/Standalone/UI/TimelineViewportPolicy.{h,cpp}` |
| 内容投影 | `Source/Utils/ContentTimelineProjection.h` |
| 内容身份 | `Source/Content/ContentKey.h` |
| 放置身份与视口会话记忆 | `Source/PluginProcessor.h`（`PianoRollPlacementIdentity`、`PianoRollViewportPrimitive`、`PluginPianoRollSessionState`） |
| 波形显示 | `Source/Standalone/UI/WaveformMipmap.{h,cpp}` |
| 参数面板 | `Source/Standalone/UI/ParameterPanel.{h,cpp}` |
| 引擎桥接与任务管理 | `Source/Inference/F0InferenceService.{h,cpp}`（进程级共享 session、串行推理） |

## 3. 与 OpenTune 的差异

仅以下各项存在差异，差异部分也沿用 OpenTune 同类模式实现：

- 歌声转换的检测与合成由按钮触发；OpenTune 的对应物是自动触发的 F0 提取与渲染服务（`Source/Services/`、`Source/Render/`、`Source/Runtime/`）。推理代码在 `native/`（Rust，yingmusic crate），编译为静态库经 C ABI 在插件进程内直接调用，与 OpenTune 的进程内推理一致；C ABI 定义在 `native/src/ffi.rs`。
- 音色库是歌声转换的独有功能（参考音频选择），占据界面左栏。
- 对比原声沿用渲染器的直通/渲染双态与交叉淡入（`Source/ARA/OpenTunePlaybackRenderer.cpp`）。
- 编辑焦点解析在 OpenTune 的 focused → lastActive → earliest 链上多一级：lastActive 为空（新建实例）时落在本实例渲染器被宿主分配的音频块上，即 Event FX 所在的音频事件，不会落到其他音频块。
- 宿主选区存在共享文档控制器里，可能残留上一个编辑器会话选中的音频块；编辑器只把自己创建之后到达的选区通知当作焦点（文档控制器维护选区更新计数，编辑器记录创建时的计数）。
- 界面配色与组件样式使用第 5 节的设计语言，不使用 OpenTune 的 Aurora 主题。

## 4. 构建与测试

- 构建并安装：仓库根目录运行 `python build.py`。
- 修改 `plugin/Source` 后运行 `tools/check-utf8-literals.py`。
- 测试在 `.cache/tests/`，随实现同步编写，全部通过才算完成。

## 5. 界面设计

### 5.1 设计语言

梅粉色单色阶，浅色界面。全部颜色取自下表，禁止引入表外颜色。

| 令牌 | 色值 | 用途 |
| --- | --- | --- |
| pink-050 | #FDF6F8 | 窗口背景、下拉面板底色 |
| pink-100 | #FBEAF0 | 面板背景、列表行悬停 |
| pink-200 | #F3D3DF | 边框、分隔线、滑杆轨道、进度条轨道 |
| pink-300 | #E9B7C9 | 钢琴卷小节线、波形包络 |
| pink-500 | #C96382 | 主色悬停 |
| pink-600 | #B5446E | 主色：主按钮、选中态、滑杆填充、进度条填充、F0 曲线 |
| pink-700 | #96365A | 主色按下 |
| pink-900 | #5C2438 | 播放头、强调元素 |
| ink-900 | #3A2931 | 主要文字 |
| ink-600 | #7A5C68 | 次要文字 |
| ink-300 | #B99AA6 | 禁用文字、失效状态的 F0 曲线 |
| success | #3E9B6D | 完成状态 |
| warning | #C98A2D / 底 #F9EEDD | 失效徽标、提示 |
| failure | #D0454C / 底 #FBE7E8 | 失败状态 |
| 琴键白 | #FFFFFF | 钢琴卷白键 |
| 琴键黑 | #4A3540 | 钢琴卷黑键 |

字体：系统默认（PingFang SC / SF Pro）。正文 13px，辅助说明 12px，面板标题 14px 半粗。

间距与形状：4px 网格，面板内边距 12px，控件间距 8px；面板圆角 8px，按钮与输入框圆角 6px；边框 1px pink-200；无投影。

### 5.2 布局

左栏音色库（固定 200px），中部钢琴卷（弹性），右侧参数面板（固定 240px），底部操作与状态栏（48px）。窗口最小 860×480，默认 1100×700。

底部操作与状态栏：左侧依次为主按钮「音高检测」「开始合成」、次按钮「取消」「对比原声」；右侧依次为状态文字、失效徽标「参数已修改」、进度条。

### 5.3 组件样式

- 主按钮：pink-600 填充、白字；悬停 pink-500，按下 pink-700，禁用 pink-200。
- 次按钮：1px pink-200 描边、ink-900 文字；悬停 pink-100 底，按下 pink-200 底。
- 滑杆：轨道 pink-200，已填充段 pink-600，滑块白色圆形带 pink-600 描边；数值可直接输入。
- 下拉框与文本输入：pink-050 底、1px pink-200 边框、ink-900 文字。
- 开关：选中 pink-600。
- 列表：行高 28px；选中 pink-200 底 + 左侧 3px pink-600 竖条；悬停 pink-100 底。
- 滚动条：细，pink-200 滑块，悬停 pink-300。
- 模态对话框：pink-050 底，按钮沿用上述样式。

### 5.4 参数

| 参数 | 范围 | 步进 | 默认 |
| --- | --- | --- | --- |
| F0 估计器 | RMVPE / FCPE | — | RMVPE |
| 扩散步数 | 1–64 | 1 | 16 |
| 音高偏移（半音） | -24 – +24 | 1 | 12 |
| CFG 强度 | 0–2 | 0.05 | 0.9 |
| 输入增益 (dB) | -12 – +3 | 0.5 | -2 |
| 输出声码器 | pupu-vocoder (level 1) / pc-nsf-hifigan (level 2) | — | pc-nsf-hifigan (level 2) |

音高偏移附 -12 / +12 快捷按钮。全部控件绑定 APVTS。

### 5.5 钢琴卷

- 背景 pink-050；黑键行铺 pink-100；半音分隔线 pink-100，C 行分隔线 pink-200。
- 琴键列：白键 #FFFFFF 整行、pink-200 分隔线，黑键 #4A3540 占左侧 62%；C 音名标注 ink-600，行高足够时显示。
- 波形 pink-300；F0 曲线 pink-600 2px；播放头 pink-900。
- 时间标尺：pink-100 底、pink-300 刻度、ink-600 文字。
- 光标坐标（秒 / 音名 / 音分 / Hz）显示在内容区右下角，pink-100 底、pink-200 边框、ink-900 文字。
- 交互：触控板双指上下左右滚动两个轴；鼠标滚轮横向滚动，Shift+滚轮纵向滚动；Cmd+滚轮以光标为锚点横向缩放；触控板捏合横向缩放；双击适配内容全长；拖拽平移（按主轴锁定方向）。
- 默认纵向视图：每半音 20px，C4 居中；检测完成后纵向适配到 F0 音高范围，键高限制在 5–40px。

### 5.6 音色库

- 标题「音色库」，右上角文件夹图标按钮（打开库目录）。
- 列表可滚动；行内显示文件名（不含扩展名）与右侧删除按钮（移入回收站）。
- 拖入音频文件导入；双击行重命名（保留扩展名）。
- 空库时列表中央显示「拖入音频文件即可导入」（ink-300）。
- 与目录双向同步：目录变化刷新列表并保留选中项；当前选中项随文档持久化。

### 5.7 状态显示

任务状态机：idle → queued → loadingModels → running → succeeded / failed / cancelled。

- queued：状态文字「排队中 #n」。
- loadingModels：「模型加载中」。
- running：阶段名 + 百分比（如「合成 42%」）。
- succeeded：「完成 · 耗时 x.x 秒」（success 色）。
- failed：「失败：原因」（failure 色）。
- cancelled：「已取消」。
- 进度条只在 queued / loadingModels / running 可见。
- 参数或音色在上次合成后被修改时，显示失效徽标「参数已修改」（warning 色）。
- 「对比原声」只在存在合成结果且无任务进行中时可用。
