# ARA AudioModification 与宿主克隆

本文给实现插件的模型读。结论来自 ARA SDK 接口、Celemony Melodyne 的 Studio One 文档、Vovious ARA 说明、以及本仓库在 Studio One 下的回调日志。

## 1. 对象职责

ARA 里一份可编辑内容对应一个 AudioModification。它总是属于一个 AudioSource，并拥有任意数量的 PlaybackRegion。AudioModification 在存档时持久。接口注释见 ARAInterface.h 约 900-903 行。

插件的音高数据、合成音频、dataRevision、合成快照写在 AudioModification 上。本仓库对应类是 DeepSvcAudioModification。工作参数写在进程内一份 WorkingParamsStore。

PlaybackRegion 描述这一块在文件里的窗口、在时间线上的放置、以及所属 RegionSequence。插件实时读取这些字段，不另存副本。

AudioSource 是样本来源。插件不在 AudioSource 上存放音高或合成结果。

同一 AudioModification 上的全部 PlaybackRegion 读同一份插件成员。改其中一块，其余块一起变。

## 2. 谁创建 AudioModification

createAudioModification 与 cloneAudioModification 由宿主调用，插件实现。声明在 ARADocumentControllerInterface，ARAInterface.h 约 2651-2661 行。调用必须包在 beginEditing 与 endEditing 之间。

插件入口是 doCreateAudioModification(source, hostRef, optionalModificationToClone)。hostRef 类型是 ARAAudioModificationHostRef，对插件不透明，ARAInterface.h 约 908-911 行。

optionalModificationToClone 非空时，插件把源 AudioModification 的成员复制进新对象，得到一份独立数据。optionalModificationToClone 为空时，新建一份空的 AudioModification，挂在给定 AudioSource 上。

插件可以 C++ new 一个 DeepSvcAudioModification。没有宿主传入的 hostRef 与 persistentID 时，宿主对象图不认识它，不会往上挂 PlaybackRegion，也不会按它做 ARA 归档。构造应只发生在 doCreateAudioModification 里，由宿主调用触发。

插件侧 DocumentController 提供的宿主实例含音频访问、归档、模型更新通知、回放控制，没有 createAudioModification 或 cloneAudioModification。

## 3. 谁决定 PlaybackRegion 属于哪一份 AudioModification

createPlaybackRegion 由宿主调用，参数里带上目标 ARAAudioModificationRef。ARAInterface.h 约 2694-2696 行。

库实现先 doCreatePlaybackRegion(audioModification, hostRef)，再在该 AudioModification 上 didAddPlaybackRegionToAudioModification。PlaybackRegion::_audioModification 是 AudioModification* const，ARAPlug.h，没有 setter。

ARAPlaybackRegionProperties 含窗口、放置、regionSequenceRef、名称、颜色，没有 audioModificationRef。插件不能靠改属性把区域改挂到另一份 AudioModification。

改写插件对象里的指针只动插件侧图。Studio One 宿主侧仍按自己的 ARAAudioModificationHostRef 认图。下一次 createPlaybackRegion 仍传入宿主选定的那一份。归档按宿主认识的 AudioModification persistentID 进行。

## 4. 共享与克隆

cloneAudioModification 的接口注释，ARAInterface.h 约 2653-2656 行：用来创建独立变体；在同一份 AudioModification 上继续添加 PlaybackRegion 则是别名。

ARA 2.0 宿主也可以把一份 AudioModification 归档再解档进新的 AudioModification，效果与克隆相同。ARAInterface.h 约 2657-2659 行。

共享：多个 PlaybackRegion 属于同一 AudioModification，插件成员一份。
克隆：宿主调用 cloneAudioModification，插件 doCreateAudioModification 收到非空 optionalModificationToClone，成员复制到新对象。之后新区域挂在新的 AudioModification 上。

## 5. Studio One 发出克隆的命令

下列两条是 DAW 命令，在时间线音频事件的右键菜单里，不在插件界面里。

分离共享副本：事件子菜单。macOS 快捷键 Option+C。英语名 Separate Shared Copies。MIDI 共用副本也可以用这条。

新建剪辑版本：音频子菜单。英语名 New Clip Version。Studio One 5.2 起提供。音频事件专用。增益曲线与 ARA 插件编辑属于该剪辑版本。

用户点这两条时，宿主调用 cloneAudioModification。插件看到 optionalModificationToClone 非空。

单选一个事件时，两条命令都把该事件从原来的共享组里拆出，赋予下一个空闲小组编号。

多选时（Melodyne Studio One 文档）：
新建剪辑版本给选中的几块同一个新编号，它们彼此仍共享，只与原来的组断开。
分离共享副本给选中的每一块各一个编号，彼此也独立。

顶层右键菜单看不到这两条，要打开事件或音频子菜单。事件还不是共享副本时，分离共享副本可能灰色或不出。

## 6. Studio One 不发出克隆的操作

本仓库日志：一份活动 AudioSource 通常 mods=1，persistentID 形如 {GUID}.1。doCreateAudioModification 的 clone 始终为 null。未见 mods 大于等于 2，也未见 {GUID}.2。前提是用户没有点第 5 节那两条。

切分：doCreatePlaybackRegion 挂在同一份 AudioModification 上，再缩短原区域窗口。原 PlaybackRegion 不销毁。两块共享同一份插件成员。

挪轨：第一次编辑事务 willBeginEditing、willDestroyPlaybackRegion、didEndEditing。第二次编辑事务 willBeginEditing、doCreatePlaybackRegion 挂在同一份 AudioModification、换一条 RegionSequence、didEndEditing。数据仍在那份 AudioModification 上。

平移：同一 PlaybackRegion 指针，只改 startInPlaybackTime。

复制、Duplicate、Option 拖动复制：新 PlaybackRegion 仍挂同一份 AudioModification，Melodyne 称为 shared copies。事件上可出现小组编号，同号块共享编辑。

Bounce Selection（并轨所选，Command+B）生成新的更短音频文件，即新的 AudioSource。新文件会得到新的 AudioModification。这与 cloneAudioModification 是另一条宿主路径。Melodyne 建议切一小段再编辑前先并轨，以便分析范围对应这段文件。

## 7. Melodyne

Studio One 文档 Tracks and clips：时间线上对事件的静音、缩短、移动、复制、增益与淡化、Time Tool 拉伸、Follow Tempo，ARA 下 Melodyne 自动跟着。编辑数据在 AudioModification 上，区域只是窗口与放置。

复制出来的音频事件一开始互相依赖。Melodyne 里改其中一块，其它块一起变。文档把这比作 MIDI 的 Duplicate Shared。

要单独改某一块：选中后分离共享副本，或新建剪辑版本。文档写事件上会出现小组编号 1、2。同号共享，异号独立。

Track Mode 显示整条音轨上各 clip 的内容，clip 边界画成竖线，边界在 DAW 里改。Clip Mode 一次看一个 clip，边界外的音符仍可见（灰色底），便于 comping 时把越过边界的音符移进窗口。

Logic：Alt 复制得到可独立编辑的副本（宿主克隆）。Loop 或 Shift+Alt 克隆区域是别名，Melodyne 内容共享。

同一音频文件在 Track Mode 下音符可以跨 clip 边界复制。不同录音文件之间不行。

Studio One 里 Edit with Melodyne 作为 Event FX 时，分析对象是所引用的整份音频文件，即使用户在时间线上只露出一截。Celemony 建议需要按切片独立分析时先 Bounce Selection。

Comping：Logic 文档写每个 take 保留自己的 Melodyne 记忆，对应宿主如何为 take 建 AudioSource 或 AudioModification，插件仍按宿主给的图工作。

静音区域：blob 隐藏但编辑保留，取消静音后仍在。AudioModification 还在。

## 8. Vovious

产品站点 dawIntegration：ARA 2 下跟随 clip 的添加、移动、删除、修剪、切分、剪切、复制。Loop clips 可以像 MIDI 那样一次改完各段，对应共享同一份 AudioModification。

公开手册没有写插件自行把 PlaybackRegion 改挂到另一份 AudioModification。合同与 Melodyne 相同：成员在 AudioModification 上；独立编辑发生在宿主克隆之后。

## 9. 对本仓库的含义

挪轨后数据还在：因为成员在 AudioModification 上，宿主销毁的是 PlaybackRegion，同一份 AudioModification 还在，新区域仍 getAudioModification() 到它。

切分后两块一起变：Studio One 切分不调用 cloneAudioModification。两块共用 DeepSvcAudioModification。用户要独立编辑时，在 DAW 里对该块做分离共享副本或新建剪辑版本。

插件不能强迫切分结果挂到不同 AudioModification 上。Studio One 切分只 doCreatePlaybackRegion。

归档键使用 AudioModification::getPersistentID() 时，与宿主对象图一致。按 PlaybackRegion::getPersistentID() 归档时，Studio One 挪轨会销毁区域；本仓库日志从未打印过区域 persistentID，不得假定新区域沿用旧 ID。

合成与分析范围：工作区间是该 AudioModification 上全部 PlaybackRegion 窗口起点的最小值到终点的最大值，一段连续文件坐标。音高分析与合成都读取这一段。

## 10. 接口与文档出处

ARAInterface.h：ARAAudioModificationRef / ARAAudioModificationHostRef；createAudioModification；cloneAudioModification；createPlaybackRegion；AudioModification 拥有 PlaybackRegion 的注释。

Melodyne Studio One：https://helpcenter.celemony.com/M5/doc/melodyneEssential5/en/M5tour_StudioOneARA_SpurenClips?env=studioOne

Vovious DAW Integration：https://www.vovious.com/dawIntegration

本仓库插件：plugin/Source/ARA/DeepSvcAudioModification.h、DeepSvcAudioModification.cpp；DeepSvcDocumentController 的 doCreateAudioModification、doStore、doRestore。
