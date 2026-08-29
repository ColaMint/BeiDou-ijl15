# BeiDou-ijl15

BeiDou v83 客户端插件。项目基于主分支 v1 继续开发，面向 BeiDou/TurtleMS 客户端，包含中文环境适配、分辨率修正、战斗与交互扩展，以及由服务端同步的昼夜和天气系统。

主分支 v2 在部分环境中进入登录界面时会崩溃，因此本分支不再与主分支同步。

## 构建

构建环境：

- Windows 10 SDK
- MSVC 平台工具集 v145
- Visual Studio 的“使用 C++ 的桌面开发”组件
- PowerShell、MIDL 和 x86 C++ 编译工具

使用 Visual Studio 打开 `ezorsia.sln`，选择 **Release | Win32** 后生成解决方案。客户端是 32 位程序，不要使用 x64 配置。

天气模块使用的 WzLib COM 头文件会在预生成事件中通过 MIDL 和 PowerShell 自动生成，不再依赖 Python。构建结果位于：

```text
out/Release/ijl15.dll
out/Release/config.ini
```

`detours.pdb` 缺失产生的 `LNK4099` 只表示 Detours 没有调试符号，不影响 Release DLL 使用。

## 安装

1. 将客户端原有的 `ijl15.dll` 重命名为 `2ijl15.dll`。
2. 将生成的 `out/Release/ijl15.dll` 放入客户端目录。
3. 将 `out/Release/config.ini` 放入同一目录。
4. 天气系统还需要配套的 WZ 资源和服务端天气代码，两端自定义 opcode 必须一致。

DLL 使用固定的 v83 客户端地址，仅适用于本项目对应的 BeiDou 客户端版本。

## 功能

### 客户端与中文环境

- 支持中文输入和中文角色名，并提供两种 IME 修复方案。
- 中文文字资源加载和界面汉化。
- 修复 Eqp、Etc 汉化后崩溃及 Use 汉化后药品无声音的问题。
- 修复中文字符串在物品、技能说明中的换行异常。
- 调整装备提示、有效期文字和日期显示格式。
- 修复滚轮异常、好友申请、学院窗口和气泡提示位置。
- 长快捷键栏移到界面右侧。
- 世界地图扩展并居中，交易中心和部分窗口位置适配宽屏。
- 保存窗口位置，修正 Tooltip 超出游戏窗口的问题。
- 支持高刷新率显示器，修复刷新率高于 60 Hz 时客户端无法启动。
- Wine 下选择角色崩溃修复。开启后服务端无法取得客户端的正确 MAC 地址。

### 界面与数值

- 自定义分辨率、登录框布局、窗口模式和启动 Logo。
- Boss HP 百分比显示。
- 物理攻击、魔攻/魔防、命中、回避、移动、跳跃和实际输出上限调整。
- 爬绳速度固定倍率或随移动速度自适应。
- HP/MP 警报设置与服务端双向同步：客户端保存时上报，服务端也可下发恢复。
- 可配置拍卖行最低价、最高价和未税价格显示。

### 可选玩法调整

- Tubi 连续操作机制。
- 允许重复发言并调整发言间隔。
- 弓手、标飞近身攻击时不挥拳。
- 允许丢弃现金道具、忽略装备性别限制。
- 自由分配技能点、攻击不停和对话文字立即显示。
- 调试模式下可解除客户端密码限制，仍需服务端配合。

## 天气与昼夜

天气状态由服务端管理，通过 `0x373D` 封包同步；客户端只负责渲染和本地平滑过渡。进入地图会立即同步，在线期间服务端也会定时校准。

当前支持九种天气：

- 晴天 `CLEAR`
- 下雨 `RAIN`
- 下雪 `SNOW`
- 阴天 `OVERCAST`
- 暴雨 `STORM`
- 暴雪 `BLIZZARD`
- 落叶 `LEAVES`
- 花瓣 `BLOSSOM`
- 沙尘暴 `SANDSTORM`

已接入的表现包括：

- 服务端统一昼夜时钟、黎明和黄昏渐变。
- 按区域设置夜间色调和天气权重。
- 云层、雨雪、花瓣、落叶、沙尘、暴雪雾层和沙尘天空。
- 暴雨闪电和雨后彩虹，多客户端按服务端种子同步节奏。
- 风向和粒子横向漂移。
- 雨声、风声等循环环境音。
- 路灯、灯光渐变及场景局部照明。
- 雨滴飞溅、水洼及逐渐干燥。
- 积雪、落叶、花瓣堆积和脚印。
- 积雪达到一定程度后的湿滑移动效果。

植被、树木、绳索和吊挂物摇摆的实现仍保留在代码中，但当前三个启用调用点已注释，因此本版本不会显示摇摆效果。

`weatherSystem=false` 会关闭整个客户端天气系统：不安装天气、昼夜、地面效果、环境音和路灯相关 hook，收到 `WEATHER_SYNC` / `LAMP_PREVIEW` 也会直接丢弃，不更新客户端天气状态。

### 服务端依赖

天气功能需要配套的 TurtleMS 服务端实现：

- `WEATHER_SYNC`：`0x373D`
- `LAMP_PREVIEW`：`0x373F`
- HP/MP 警报同步：`0x1000`

服务端会每分钟广播状态，并约每 15 分钟按区域重新抽取天气。昼夜完整循环为现实时间 4 小时。GM 强制天气或时间期间会暂停相应的自动状态，超时后恢复。

## 配置

配置文件为 [ezorsia/config.ini](ezorsia/config.ini)，构建时会复制到输出目录。布尔值应写成 `true` 或 `false`，不要在值末尾添加分号。

### general

| 配置项 | 作用 |
| --- | --- |
| `imeType` | 中文输入修复方案，支持 `0` 和 `1` |
| `width` / `height` | 游戏分辨率 |
| `ServerIP_Address` / `serverIP_Port` | 服务端地址和端口 |
| `SwitchChinese` | 加载中文文字替换 |
| `MsgAmount` | 右下角消息显示数量 |
| `EzorsiaV2WzIncluded` | UI.wz 是否包含 Ezorsia v2 资源 |
| `CustomLoginFrame` | 是否使用分辨率对应的登录框资源 |
| `bigLoginFrame` | 是否按大登录框布局界面 |
| `WindowedMode` | 强制窗口模式 |
| `RemoveLogos` | 跳过启动 Logo |
| `UseVirtuProtect` | 写入客户端内存前是否调整页面保护 |
| `fixWine` | 修复 Wine 下选择角色崩溃 |

### optional

| 配置项 | 作用 |
| --- | --- |
| `setDamageCap` | 物理攻击面板上限 |
| `setMAtkCap` | 魔攻/魔防面板上限 |
| `setAccCap` / `setAvdCap` | 命中和回避上限 |
| `setAtkOutCap` | 实际输出上限 |
| `speedMovementCap` / `jumpCap` | 移动和跳跃上限 |
| `climbSpeedAuto` / `climbSpeed` | 爬绳速度模式和基础倍率 |
| `useTubi` | 连续操作机制 |
| `ownLoginFrame` / `ownCashShopFrame` | 使用自行修改的登录/商城背景 |
| `talkRepeat` / `talkTime` | 重复发言和发言间隔 |
| `closeRangeShooting` | 远程职业近身不挥拳 |
| `dropCashItem` | 允许丢弃现金道具 |
| `ignoreGender` | 忽略装备性别限制 |
| `freeSPAllocation` | 自由分配技能点 |
| `nonStopAttack` | 攻击不停 |
| `instantTextDisplay` | 对话文字立即显示 |
| `auctionMinPrice` / `auctionMaxPrice` | 拍卖行价格范围 |
| `auctionTaxFree` | 拍卖行显示卖家未税标价 |
| `weatherSystem` | 天气系统总开关；关闭时不注入天气/昼夜逻辑，也不处理天气同步封包 |

### debug

| 配置项 | 作用 |
| --- | --- |
| `debug` | 启用客户端调试类功能 |
| `noPassword` | 解除客户端密码限制，需要同时开启 `debug` |

## 推荐服务端

- 当前天气、灯光和 HP/MP 同步：TurtleMS Server `gms-server`
- 原始 BeiDou 服务端：https://github.com/SleepNap/BeiDou

## 近期更新

- 加入服务端同步的区域天气和昼夜系统。
- 加入路灯、地面积累、环境音、风力、闪电和彩虹效果。
- 统一自定义服务端封包分发，避免登录阶段错误路由。
- 加入 HP/MP 警报收发同步。
- 修复 Wine 选择角色崩溃。
- 修复高刷新率显示器启动失败。
- 增加拍卖行价格上下限和未税价格显示。
- 增加攻击不停、自由分配技能点和对话文字速显。
