[English](README.md) | 简体中文

# 让安卓手机用 AAC-ELD 连接 AirPods

AirPods 对任何手机都提供三种编码：SBC、AAC，以及苹果自己的 AAC-ELD
变体，也就是它和苹果设备之间使用的低延迟编码。安卓不认识第三种，所以最后
总是用 AAC。

这是一个 root 模块，让 **小米 17 Pro Max** 的蓝牙协议栈学会这种编码。装上
之后，手机会和 AirPods 协商使用 AAC-ELD，并由手机自己完成编码，码率最高
320 kbps。系统分区不做任何改动：模块只在内存里 hook 蓝牙进程，卸载后一切
恢复原样。

**其他机型：** 目前只在这一款手机上运行过。模块在设计上会自己适应不同的
系统版本，[docs/PORTING.md](docs/PORTING.md)（英文）是一份分步骤的移植指南，
写给想在其他安卓手机上尝试的开发者或 AI 助手，不需要事先了解蓝牙协议栈。

## 当前状态

| | |
|---|---|
| 手机 | 小米 17 Pro Max（`popsicle`，骁龙，arm64） |
| 系统 | HyperOS 3（Android 16）和 HyperOS 4.0.6.0（Android 17） |
| 耳机 | AirPods 5、AirPods Pro |
| Root | KernelSU Next + NeoZygisk。Magisk 和 APatch 的安装流程已经写好，但没有实际跑过 |
| 版本 | v0.15.1（协议栈模块本身自 v0.15 起没有变化） |

每个系统上哪些测过、哪些没测，见
[docs/notes/development-log.md](docs/notes/development-log.md)（英文）中的
"What has been verified on which system"。简单说：各档码率都能正常播放；
重新连接、切换编码、调节音量、走出信号范围再回来都测试过；HyperOS 4 上测过的
项目比 HyperOS 3 少。

没有测量过的：端到端延迟、耗电，以及 288 和 320 kbps 是否比 256 kbps
更好听。

## 需要什么

- 一台小米 17 Pro Max，系统为上表之一。其他情况请先看
  [docs/PORTING.md](docs/PORTING.md)。
- 带 Zygisk 的 root：KernelSU 或 APatch 加一个 Zygisk 模块（NeoZygisk、
  ReZygisk、Zygisk Next），或者打开了 Zygisk 的 Magisk。
- 提供这种编码的 AirPods。AirPods 5 和 AirPods Pro 都可以。
- 可选：Xposed 框架（LSPosed、Vector），用来在蓝牙设置里显示开关。没有的话
  这几项设置就是系统属性，用命令修改。

## 安装

1. 从 Releases 页面下载 `AAC-ELD-AirPods-<版本>.zip`，或者自己编译（见下文）。
2. 在 root 管理器里刷入，并看一下安装程序的输出：能自动完成的它会做好，
   剩下需要你手动做的会列出来。系统语言是中文时，这些提示是中文的。
3. 重启手机。
4. 连接 AirPods。蓝牙设置里它旁边的编码标识会显示 "AAC-ELD"。

用电脑确认：

```bash
adb shell dumpsys bluetooth_manager | grep -m1 'mCodecConfig:'
```

AAC-ELD 会显示为 `codecName:Opus`，并带有 `mCodecSpecific1:5013505`。
它借用了协议栈里 Opus 的位置，这个数字（0x4C8001）是它的标记。

```bash
adb logcat -s AacEldA2dp
```

可以看到模块找到了什么、协商了什么、发出了什么。

## 设置

装了设置扩展之后，AirPods 在蓝牙设置里的页面上会多出两个开关和一个滑块：
**AAC-ELD**、**可变码率**、**最高码率**。没有装扩展时，对应的是下面三个属性：

| 属性 | 默认值 | 含义 |
|---|---|---|
| `persist.bluetooth.aaceld.enabled` | true | 设为 false：AirPods 重新使用 AAC |
| `persist.bluetooth.aaceld.vbr` | true | 可变码率，和 macOS 发送的一样。false：固定码率 |
| `persist.bluetooth.aaceld.bitrate` | 256000 | 最高码率，64000 到 320000 |

```bash
adb shell su -c 'setprop persist.bluetooth.aaceld.bitrate 320000'
```

全部设置项、默认值背后的测量数据以及已知限制，见
[module/README.md](module/README.md)（英文）。

## 关闭与卸载

- 让 AirPods 回到 AAC：关掉耳机页面上的 AAC-ELD 开关；或者把
  `persist.bluetooth.aaceld.enabled` 设为 `false`，然后重新连接耳机。
- 不卸载、但让模块完全不介入协议栈：把 `persist.aaceld.enabled` 设为
  `false`，然后关闭再打开蓝牙。
- 彻底移除：在 root 管理器里卸载模块并重启。"AAC-ELD 蓝牙设置扩展"是一个
  普通应用，像其他应用一样卸载即可。

如果蓝牙在模块启动过程中崩溃，下次启动时模块会自动不再介入。所以遇到不适配的
系统版本，代价只是蓝牙重启一次，不会卡在开机循环。

## 编译

需要 Android NDK、`cmake` 和 `zip`；编译设置扩展还需要 Android SDK 的
build-tools 和 JDK。

```bash
./build.sh
```

会生成 `AAC-ELD-AirPods-<版本>.zip`。编译用到的代码都在本仓库里，不会联网
下载任何东西。

## 工作原理（六句话）

1. 一个 Zygisk 模块加载进 `com.android.bluetooth`，利用安卓留在库文件里的
   调试符号，按名称找到协议栈的函数。
2. 它 hook 了一批回答"这是什么厂商编码"的小函数，让协议栈认出苹果的编码。
3. 协商时借用协议栈里 Opus 编码的位置，因为没有哪款 AirPods 使用 Opus。
4. 它提供一个编码器，驱动协议栈里本来就有的 FDK-AAC 库：48 kHz，每帧
   10 ms，三帧一个数据包，每帧前面加上苹果的四字节头。
5. 它修正了 AirPods 比较挑剔的几个细节，最重要的是 RTP 时间戳。
6. 硬件编码（A2DP offload）保持开启：其他编码仍然像原来一样由手机的音频
   芯片完成。

详细版本见 [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)（英文），包括空中
传输的每一个字节。

## 文档

除本页外，其余文档目前只有英文。

| | |
|---|---|
| [docs/PORTING.md](docs/PORTING.md) | 在其他机型或系统版本上尝试，分步骤说明 |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | 故障排查：现象、检查方法、原因、解决办法 |
| [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) | 设计思路，以及这种编码在空中的格式 |
| [module/README.md](module/README.md) | 全部设置项、测量数据、限制 |
| [label/README.md](label/README.md) | 小米"设置"应用的扩展 |
| [docs/notes/](docs/notes/) | 开发过程中的原始笔记：每个结论是怎么得出的 |

## 仓库结构

| | |
|---|---|
| `src/` | 模块本体：启动与符号查找（`target.cpp`）、hook（`hooks.cpp`）、编码器与数据包（`eld_encoder.cpp`） |
| `module/` | 刷机包里除库文件以外的内容：安装脚本、开机脚本、卸载脚本 |
| `label/` | 针对小米"设置"的 Xposed 扩展：编码标识和耳机页面上的选项 |
| `tools/` | 移植和测试用的工具：从手机取出并检查协议栈库、列出符号、反汇编、按日志文字查找代码、在测试进程里跑启动流程、过滤日志、播放测试文件 |
| `third_party/` | hook 引擎（ShadowHook）和 Zygisk API 头文件，各自保留原许可证 |
| `docs/` | 见上表 |

## 需要知道的事

- 请保持 AirPods 的 **"与本机音量同步"** 开启（即绝对音量）。关掉之后音质
  会明显变差；设置扩展会强制保持它开启。
- 小米的 **"空间音频"** 不能和 AAC-ELD 同时使用。手机的空间音效只接在硬件
  编码那条通路上。
- 开发者选项里这种编码显示为 **Opus**。
- 设置是全局的，不区分耳机。
- 在 HyperOS 4 上，一副真正支持 Opus 的耳机现在会用上 Opus，而原厂系统不会。
  `persist.aaceld.priority=0` 可以恢复原厂的优先级。没有用这类耳机实测过。
- 其他耳机的行为应该和没装模块时完全一样；实际检查过的只有 AirPods 使用
  AAC 的情况。
- 界面语言：安装提示和设置扩展的界面跟随系统语言，中文系统显示简体中文，
  其他语言显示英文。

## 参与

最有用的是来自其他机型的反馈：见 [docs/PORTING.md](docs/PORTING.md) 末尾的
"Reporting a port"。请不要上传蓝牙抓包文件（`btsnoop_hci.log`），里面包含
你所有已配对设备的配对密钥（link key）。

## 许可证与致谢

Copyright (C) the developer.

本项目是自由软件，采用 **GNU 通用公共许可证第 2 版**（GPL-2.0-only），见
[LICENSE](LICENSE)。不提供任何担保。它会改变已 root 手机上一个系统进程的
行为，使用风险自负。

第三方代码保留各自的许可证，列在
[third_party/README.md](third_party/README.md)：ShadowHook 和 xDL（MIT）、
linux-syscall-support（BSD）、Zygisk API 头文件。

本项目所依据的苹果 AAC-ELD 码流说明，以及用来对照的参考发送端，来自
[USBPods-Pico2W](https://github.com/wasdwasd0105/USBPods-Pico2W)。

FDK-AAC 编码器不在本仓库中，也不在模块里。模块调用的是手机蓝牙协议栈中
已经包含的那一份。

本项目与 Apple、小米均无关联。AirPods 是 Apple Inc. 的商标。
