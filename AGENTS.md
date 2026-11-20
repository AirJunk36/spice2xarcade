# Spice_Arcade 项目约定

## 项目目标

本项目基于 Spice2x，为真实街机机台提供 BI2 系列 I/O 适配。

用户已明确的需求：

- 支持 BI2 系列不同版本之间的接口／协议转译，例如 BI2X → BI2A。
- 同时支持 BI2A → BI2X，以及 IO2／EZUSB FX2 与 BIO2 的双向转译。
- 支持可配置的按键重定向，例如将某个物理按键映射为另一个游戏按键。
- 支持可配置的灯光重定向，例如将游戏的一条灯带映射到实机的另一条灯带。
- 转译与按键、灯光重定向需要能够组合使用。
- 首个目标是 Beatmania IIDX，实机为 Lightning Model，首阶段游戏范围为 18–26 代。
- 在 Lightning Model 的真实副屏上显示可触摸的双人小键盘、游戏 LED Ticker
  和 VEFX 效果滑条；老游戏本身没有 TDJ 副屏也要提供这些功能。

上述为目标需求，实现情况和验证边界如下；不能把上游已有的 I/O 模拟等同于实机转译支持。

已实现原生 BI2X 后端 `src/spice2x/bio2/`，以及 IIDX 的 BIO2 Buttons／BIO2 Lights
配置页，接入现有按键、转盘与灯光 API。包含输入重定向、RGB 通道与完整灯带重定向、
连接配置、灯光测试和重连处理。原生通信代码来自公共领域的
`dumcatt/iidxio-bi2x`，来源与修改见 `external/bi2x/UPSTREAM`。
使用方法与验证边界见 `src/spice2x/bio2/README.md`。

模拟板协议测试和 Windows 32／64 位驱动编译已通过；尚无实机验证。
关闭期间会保留初始化完成的连接，发送最后的熄灯指令后再释放设备。
游戏端沿用 Spice 原有 EZUSB／BIO2 适配，避免重复 USB 模拟改变设备识别顺序。
用户已将范围收敛至 Spice2x 原生覆盖的 18 代及以后，当前目标仍为 18–26 代；
9–17 代已排除，不再安排其启动适配。

新增 BI2A 原生 ACIO 后端和 IO2 原生 CyUSB3 后端，通过 `-bio2board` 选择实机。
旧 `-bi2x` 与通道编号保持兼容。传统板卡只提供其实际支持的按键、转盘、
五路滑条、27 路灯和 Ticker；不伪造 RGB 灯带能力。IO2 必须预先加载正确固件，
BI2A 必须已安装正确 IIDX 固件，代码不包含固件。
老游戏副屏通过 `games/iidx/legacy_subscreen.cpp` 接入原项目的 `backend.dll`，
原界面与触摸路径由其提供。功能默认关闭，启用后 IIDX IO Panel 仍可手动打开。
源码与协议来源见 `bio2/PROTOCOLS.md`。macOS 模拟板与布局测试已通过 4/4，
官方 Docker 内 UBSan 测试通过 4/4；关闭熄灯路径连续四轮验证通过。
最终 Docker 构建及 Windows 7／XP API 检查均通过，没有新增编译警告。
两个本地压缩包的完整性、程序架构及全部 59 个修改源码文件的一致性已验证。
尚无实机、真实触摸屏或逐游戏运行验证。
Docker、Buildx 与 Colima 已安装，专用 Docker 上下文为 `colima-spice-arcade`，
支持通过 Rosetta 运行 `linux/amd64` 构建容器。2026-11-15 已通过官方 Docker 整包构建（32 位、64 位及默认的 WinXP 32 位附加版本），
Windows 7／XP 系统 API 兼容性检查；本轮三类板卡的测试已扩展为 4/4。
本地构建包位于 `src/spice2x/dist/`，压缩包完整性、程序 PE 架构及全部修改源码文件的一致性
均已验证，报告为 `dist/build-verification.json`。实机验收仍未进行。

## 上游与源码

- 官方仓库：https://github.com/spice2x/spice2x.github.io
- 上游分支：`upstream/main`
- 2026-11-15 初始化时的上游基线：`3e3db7a411c80ff21e8cbee4f7609a8f9b8285b4`
- 源码目录：`src/spice2x/`
- 遵守根目录 `CONTRIBUTING.md` 的兼容性、代码风格及构建要求。

## 已确认的相关入口

- `src/spice2x/acio/bi2a/bi2a.cpp`：BI2A 接口 Hook 与状态、灯光处理。
- `src/spice2x/games/*/bi2x_hook.cpp`：不同游戏的 BI2X 接口 Hook。
- `src/spice2x/games/iidx/bi2a.cpp`、`bi2x.cpp`：IIDX 串口设备模拟。
- `src/spice2x/games/iidx/ezusb.cpp`：老 IIDX 的 EZUSB 设备模拟。
- `src/spice2x/overlay/windows/iidx_sub.cpp`：显示游戏已有的 TDJ 副屏，
  不会为老游戏生成独立副屏。
- `src/spice2x/overlay/windows/iidx_seg.cpp`、`keypad.cpp`、`iopanel_iidx.cpp`：
  现有 Ticker、小键盘、五路效果滑条控件。
- `src/spice2x/acio2emu/`：ACIO2 协议与 BI2X 节点模拟。
- `src/spice2x/cfg/api.h`、`api.cpp`：游戏按键、模拟量和灯光访问。
- `src/spice2x/util/tapeled.h`、`tapeled.cpp`：灯带数据处理。

按键和灯带协议及编号可能随游戏、板卡和机台型号变化；应按实际配置核对。
实机灯带重定向需区分完整逐像素 RGB 数据与上游用于部分输出的单色提取。

## IIDX 18–26 适配边界

Lightning Model 是物理硬件，不能等同于让所有老游戏启用 `-iidxtdj`。
游戏端应保持各自的启动方式与 I/O 接口，实机端连接 Lightning Model I/O。

- 9–17：用户已明确排除，不实现额外的早期 EXE 启动适配。
- 18–24：需逐版验证 DLL 启动及 EZUSB 接口；现有 EZUSB 模拟不能证明实机已可用。
- 25–26：现有代码通过 COM2 模拟 BIO2／BI2A，需接入实机后端并逐版验证。
- 输入方向为实机 BI2X → 按键／转盘重定向 → 老游戏所需的 I/O 接口。
- 输出方向为老游戏灯光 → 灯光／灯带重定向 → 实机 BI2X。
- 小键盘输入需送入对应玩家的读卡器／小键盘接口，不能作为普通七键输入处理。
- Ticker 需要读取老游戏输出的九位字符；五路效果滑条为 VEFX、Low EQ、High EQ、
  Filter、Play Volume，需回写对应游戏 I/O 模拟量。
- 独立副屏需要适配实际显示器与触摸坐标，并避免触摸抢占游戏主屏焦点。

上述为目标范围，尚无 18–26 任一版本的实机兼容性验证结果。

## 参考项目与已核实事实

- https://github.com/cloud11665/iidx_btools_subscreen
  - README 标注支持 IIDX 12–24；提供 Ticker、小键盘、效果滑条。
  - 使用 BemaniTools 的 `vefxio`／`eamio` 接口，不能直接视为 Spice2x 插件。
  - 另有 BIO2 IIDXIO 包装器，不能据此认定已支持 Lightning Model 的 BI2X。
  - 主项目为 MIT 许可；若复用代码须保留版权与许可，素材需分别核对来源与许可。
- https://github.com/djhackersdev/bemanitools/tree/master/doc/iidxhook
  - 文档明确区分 9–17 的注入启动与 18 起的 DLL 启动，可用于核对兼容性边界。

当前仓库未复制 `iidx_btools_subscreen` 的代码、图片或字体。
BI2A／IO2 根据 Bemanitools 的公开协议布局与初始化行为实现，保留其 Unlicense
出处；固定提交为 `5ae0b175170d9ae657b7634ca514b7848923401e`。
原生 BI2X 驱动与模拟板来自 `https://github.com/dumcatt/iidxio-bi2x`，
固定提交为 `de0938f91646ecf211b619071a06edfc639ff3ae`；仓库保留其 Unlicense，
且不包含游戏 DLL 或固件模块。测试仅使用人工生成的合成模块数据。

## 提交要求

用户要求完成后精简代码、优化性能并提交至 GitHub，暂不创建或发布 Release。
本地构建包可用于验证，不代表授权发布 GitHub Release。用户指定的 Fork 为
`https://github.com/AirJunk36/spice2xarcade`，已确认其 parent 是官方仓库。
使用具有该仓库写权限的身份推送 `codex/` 分支，并保留上游兼容性与验证记录。
