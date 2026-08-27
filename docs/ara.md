# deepsvc ARA 插件

## 1. 功能

- ARA 2 Event FX 插件，目标宿主为 macOS 上的 Studio One（VST3 必需，AU 顺带构建）。
- 对事件音频做歌声转换：
  - 音高检测（RMVPE / FCPE）覆盖焦点音频块所落分段的区间（分段的定义见第 4.1 节）；每个 A/B 槽位用各自的估计器分别检测；钢琴卷只显示该分段内、激活槽位的音高，其他音频块不显示；
  - 从音色库选择参考音频进行合成，合成结果替换事件声音参与回放；激活槽位还没有音高数据时，点击合成会先自动执行音高检测再合成；
  - 参数：F0 估计器、扩散步数、音高偏移、音高微调、CFG 强度、输入增益、输出声码器。
- 片段独立：每个音频块持有自己的一整套状态，切分、复制、修剪得到的音频块在那一刻按继承规则各得一份副本，此后互不相干（见第 4.1 节）。
- A/B 对比：每个分段有两个完全独立的槽位，参数、音色、音高检测结果、合成结果、旁通设置互不共享；操作栏的 A|B 切换器切换，分别检测、分别合成、分别旁通（见第 4 节）。
- 音色库：拖入文件导入，双击重命名，每行右侧删除按钮，右上角打开文件夹图标，与目录双向同步。
- 推理在插件进程内执行（`native/` 编译为 Rust 静态库直接链入插件，使用 yingmusic crate），模型进程级加载一次、全部插件实例复用；模型权重打入插件 bundle，libsoxr 等依赖全部静态链接，插件自包含。
- 插件的一切状态随工程持久化（见第 4.2 节），归档版本 1，不兼容任何其他格式。

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
- A/B 双槽位是歌声转换的独有功能（两套参数的对比试听），OpenTune 无对应物；槽位切换的交叉淡化沿用渲染器的交叉淡入（`Source/ARA/OpenTunePlaybackRenderer.cpp`）。
- 状态按分段挂载（第 4.1 节）：OpenTune 的一个音频修改只有一套内容，本插件把内容按修改内部时间划分为分段，每个分段一套状态。分段布局变化时的继承沿用 OpenTune 的 `copyContentRange`（`Source/PluginProcessor.cpp`）：音频按样本区间切片、音高曲线平移到本地区间、其余设置整体拷贝。
- 归档记录为 persistentID + JSON 字符串，推理结果（音高、合成音频）直接入库；OpenTune 的记录为 persistentID + XML，渲染结果不入库、恢复后由渲染服务重建。见第 4.2 节。
- 编辑焦点解析在 OpenTune 的 focused → lastActive → earliest 链上多一级：lastActive 为空（新建实例）时落在本实例渲染器被宿主分配的音频块上，即 Event FX 所在的音频事件，不会落到其他音频块。
- 宿主选区存在共享文档控制器里，可能残留上一个编辑器会话选中的音频块；编辑器只把自己创建之后到达的选区通知当作焦点（文档控制器维护选区更新计数，编辑器记录创建时的计数）。
- 界面配色与组件样式使用第 6 节的设计语言，不使用 OpenTune 的 Aurora 主题。
- 音频块被宿主拉伸时，合成结果按拉伸后的时间映射直接读取，音高随拉伸变化；OpenTune 的对应物是时间拉伸重建（Stage2，`Source/Render/`），本插件未实现。

## 4. 状态管理与持久化

### 4.1 内容分段与 A/B 槽位

一个音频修改的内容被划分为若干**分段**。分段是修改内部时间上的一个区间，边界由该修改下所有音频块的内容窗口端点决定：把所有窗口的起点与终点取并集作为边界，相邻边界之间被至少一个窗口覆盖的区间成为一个分段。宿主每次编辑事务结束、以及音频块增删与属性变化时重新划分（`Source/Content/Segmentation.h`）。

状态挂在分段上。每个分段持有两个完全独立的槽位（A、B）与当前激活槽位，槽位之间零共享，各自持有：

- params：7 个合成参数的当前编辑值；
- synthParams：上次合成时的参数快照；
- timbreFile：音色引用；
- f0Times / f0Values：用该槽位的估计器对本分段区间的检测结果，时间轴为分段本地时间；
- renderedAudio：用该槽位的参数与音色对本分段区间合成的结果，覆盖分段区间；
- bypass：该槽位是否直通原声。

源 PCM 缓存挂在修改上，由音频文件唯一决定，所有分段共享同一份不可变缓存。

继承规则：分段布局变化时，每个新区间从旧布局中重叠最大的分段深拷贝继承状态；音高曲线平移到新区间的本地时间并裁剪，合成音频截取新区间覆盖的样本区段，参数、音色、旁通、激活槽位整体拷贝。对应 OpenTune 的 `copyContentRange`（`Source/PluginProcessor.cpp`：音频按样本区间切片、音高曲线平移到本地区间、其余设置整体拷贝）。

由此得到的行为：

- 切分音频块，切点成为分段边界，两半各得切分那一刻完整状态的一份副本，此后各自检测、各自合成、各自旁通，互不相干。
- 修剪音频块窗口，端点变化产生新的分段划分，状态跟随并按新区间裁剪。
- 原样复制且两份窗口完全相同时，两份落在同一个分段上，表现为镜像；其中任意一份被修剪后立即分开。
- 分段布局重建时，落在已消失分段上的进行中任务被取消，结果不会写到不存在的分段。

其余行为规则：

- 操作栏的 A|B 切换器切换激活槽位，每个分段各自记住激活槽位；切换时参数面板、钢琴卷音高、旁通按钮、回放音频全部切到新槽位，渲染器在块边界做 5ms 等增益交叉淡化（沿用 `Source/ARA/OpenTunePlaybackRenderer.cpp` 的交叉淡入）。
- 编辑器焦点音频块所落的分段是界面的读写目标：APVTS 是该分段激活槽位的编辑面，切换焦点时把新分段的 params 推进 APVTS，编辑时写回，两边始终同步。
- 检测与合成任务携带分段与槽位索引，完成时写回发起时的分段与槽位；切换焦点或槽位不影响进行中的任务。
- 激活槽位旁通开启或无合成结果时，该音频块回放原声。
- 回放取样：合成音频的时间原点是分段起点，源音频的时间原点是源窗口起点。

### 4.2 持久化

插件的一切状态随工程保存：

| 状态 | 机制 |
| --- | --- |
| 每个音频修改的全部分段（区间端点、两个槽位的完整数据、激活槽位） | ARA 归档 |
| 当前参数值（7 个 APVTS 参数） | 宿主 VST3 state chunk |
| 音色库文件 | 磁盘目录 |

ARA 归档对应 OpenTune 的 `serializeAudioModificationContent` / `restoreAudioModificationContent` / `doStoreObjectsToStream` / `doRestoreObjectsFromStream`（`Source/ARA/OpenTuneDocumentController.cpp`）：

- 每个音频修改一条记录：persistentID + 自描述 JSON 字符串（OpenTune 用 XML）。编解码在 `Source/Content/ContentArchive.h`。
- JSON 结构：`sourceWindow`（start、end）、`segments[]`，每个分段含 `start`、`duration`、`activeSlot`、`slots[2]`（params、synthParams、timbreFile、synthTimbreFile、f0Times、f0Values、renderedAudio、bypass）。分段的区间端点即分段身份，随归档持久，恢复时无需匹配。
- 推理结果（音高、合成音频）直接入库。OpenTune 的渲染结果是确定性 DSP，恢复后由渲染服务重建；本插件的扩散采样无种子（`yingmusic-svc-mlx/src/core.rs:590`），重跑得到的是不同的随机采样，推理结果必须入库。
- 恢复：filter 映射 persistentID，解析 JSON，校验 sourceWindow 与当前源一致，装载分段数组并按起点排序；记录无法映射或目标不存在则跳过该条，仅数据损坏才整体失败。
- 脏标记：任何入库状态变化（检测完成、合成完成、槽位切换、旁通切换、参数编辑、分段划分变化）后调 `audioModification->notifyContentChanged`（OpenTune 在每次入库状态变化后调用同一接口），宿主才知道工程已修改并保存。
- 归档版本 1，不兼容任何其他格式。

### 4.3 宿主旁通

重写 `processBlockBypassed`：以直通模式调 playback renderer 渲染源音频。先实测 Studio One 旁通 ARA 插件时是否自行播放原始音频，是则插件无需处理。

## 5. 构建与测试

- 构建并安装：仓库根目录运行 `python build.py`。
- 修改 `plugin/Source` 后运行 `tools/check-utf8-literals.py`。
- 测试在 `.cache/tests/`，随实现同步编写，全部通过才算完成。

## 6. 界面设计

### 6.1 设计语言

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

### 6.2 布局

左栏音色库（固定 200px），中部钢琴卷（弹性），右侧参数面板（固定 240px），底部操作与状态栏（48px）。窗口最小 860×480，默认 1100×700。

底部操作与状态栏：左侧依次为 A|B 分段切换器、主按钮「音高检测」「开始合成」、次按钮「取消」「旁通」；右侧依次为回放指示、状态文字、失效徽标「参数已修改」、进度条。

### 6.3 组件样式

- 主按钮：pink-600 填充、白字；悬停 pink-500，按下 pink-700，禁用 pink-200。
- 次按钮：1px pink-200 描边、ink-900 文字；悬停 pink-100 底，按下 pink-200 底。
- 滑杆：轨道 pink-200，已填充段 pink-600，滑块白色圆形带 pink-600 描边；数值可直接输入。
- 下拉框与文本输入：pink-050 底、1px pink-200 边框、ink-900 文字。
- 开关：选中 pink-600。
- 列表：行高 28px；选中 pink-200 底 + 左侧 3px pink-600 竖条；悬停 pink-100 底。
- 滚动条：细，pink-200 滑块，悬停 pink-300。
- 模态对话框：pink-050 底，按钮沿用上述样式。

### 6.4 参数

| 参数 | 范围 | 步进 | 默认 |
| --- | --- | --- | --- |
| F0 估计器 | RMVPE / FCPE | — | RMVPE |
| 扩散步数 | 1–64 | 1 | 16 |
| 音高偏移（半音） | -24 – +24 | 1 | 12 |
| 音高微调（cents） | -100 – +100 | 1 | 0 |
| CFG 强度 | 0–2 | 0.05 | 0.9 |
| 输入增益 (dB) | -12 – +3 | 0.5 | -2 |
| 输出声码器 | pupu-vocoder (level 1) / pc-nsf-hifigan (level 2) | — | pc-nsf-hifigan (level 2) |

音高偏移附 -12 / +12 快捷按钮。全部控件绑定 APVTS。参数为每个分段的每个 A/B 槽位各自一份，APVTS 是焦点分段激活槽位的编辑面（见第 4.1 节）。

### 6.5 钢琴卷

- 背景 pink-050；黑键行铺 pink-100；半音分隔线 pink-100，C 行分隔线 pink-200。
- 琴键列：白键 #FFFFFF 整行、pink-200 分隔线，黑键 #4A3540 占左侧 62%；C 音名标注 ink-600，行高足够时显示。
- 波形 pink-300；F0 曲线 pink-600 2px；播放头 pink-900。
- 时间标尺：pink-100 底、pink-300 刻度、ink-600 文字。
- 光标坐标（秒 / 音名 / 音分 / Hz）显示在内容区右下角，pink-100 底、pink-200 边框、ink-900 文字。
- 交互（与 Studio One 一致）：触控板双指上下左右滚动两个轴；鼠标滚轮上下滚动，Shift+滚轮左右滚动；Cmd+滚轮或触控板 Cmd+上下滑动以光标为锚点纵向缩放（琴键高度），Cmd+Shift+滚轮以光标为锚点横向缩放（时间轴）；触控板捏合横向缩放；双击适配内容全长；拖拽平移（两个轴同时跟随）。
- 默认纵向视图：每半音 20px，C4 居中；检测完成后纵向适配到 F0 音高范围，键高限制在 5–40px。
- 显示的音高永远是原音频的音高：检测输入永远是源 PCM，合成结果不参与检测；切换槽位显示该槽位检测到的源音高。

### 6.6 音色库

- 标题「音色库」，右上角文件夹图标按钮（打开库目录）。
- 列表可滚动；行内显示文件名（不含扩展名）与右侧删除按钮（移入回收站）。
- 拖入音频文件导入；双击行重命名（保留扩展名）。
- 空库时列表中央显示「拖入音频文件即可导入」（ink-300）。
- 与目录双向同步：目录变化刷新列表并保留选中项；各槽位的选中项随文档持久化。

### 6.7 状态显示

任务状态机：idle → queued → loadingModels → running → succeeded / failed / cancelled。

- queued：状态文字「排队中 #n」。
- loadingModels：「模型加载中」。
- running：阶段名 + 百分比（如「合成 42%」）。
- succeeded：「完成 · 耗时 x.x 秒」（success 色）。
- failed：「失败：原因」（failure 色）。
- cancelled：「已取消」。
- 进度条只在 queued / loadingModels / running 可见。
- 激活槽位的当前参数或音色与上次合成的快照不一致时，显示失效徽标「参数已修改」（warning 色）。
- 回放指示常驻：激活槽位旁通或无合成结果时显示「播放：原声」，否则显示「播放：合成 · 音色名 · 移调 +12 · 16 步」（取自该槽位的合成参数快照与音色）。
- 「旁通」切换激活槽位的旁通设置，两个槽位各自独立。
