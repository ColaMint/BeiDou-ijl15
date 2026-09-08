# NameSpace streaming 启动分析

分析对象：

- `v83.i64`：32 位 MapleStory v83 客户端
- `NameSpace.dll`：SHA-256 与 `LAA_WZ_RESOURCE_ANALYSIS.md` 记录版本一致
- 当前 WZ 目录：18 个 WZ，合计约 5.0 GiB；最大文件为约 1.99 GB 的
  `Character.wz`

## 结论

`nameSpaceStreaming=true` 本身没有让客户端少解析资源。它在
`NameSpace.dll+0xD10A` 的整文件 `MapViewOfFile` 返回失败后，触发 DLL 原有的同步文件
读取路径。该路径中的普通读取位于 `NameSpace.dll+0xCB29` 和 `+0xCA58`：映射可用时
只是 `memcpy`，映射不可用时则为每个逻辑读取调用 `ReadFile`；其中 `+0xCB29` 还会先
调用 `SetFilePointer(FILE_CURRENT)`，位置不符时再 seek。

因此启动慢的主要风险不是顺序读取 5 GiB 文件内容，而是初始化 WZ 目录和解析登录资源
时产生的大量、小尺寸、同步、带 seek 检查的读取。机械硬盘、实时杀毒、Wine 文件系统
转换或较慢存储会把这些系统调用的固定开销显著放大。静态分析可以确认该机制；具体用户
机器上的调用数和数据量应以新增的 `InitializeResMan` 聚合日志为准。

## 启动到登录界面

客户端 `CWvsApp::InitializeResMan` 位于 `0x009F7159`，执行以下工作：

1. 创建 `IWzResMan`，调用 `SetResManParam(0x11, -1, -1)`。
2. 创建根 `IWzNameSpace` 和指向游戏目录的 `IWzFileSystem`。
3. 初始化并挂载 Base/package namespace。
4. 顺序初始化并挂载 15 个主包：`Character`、`Mob`、`Skill`、`Reactor`、`Npc`、
   `UI`、`Quest`、`Item`、`Effect`、`String`、`Etc`、`Morph`、`TamingMob`、
   `Sound`、`Map`。
5. 每个包都以客户端版本 `83` 初始化，然后分别挂到包 namespace 和根 namespace。

`CLogin::Init` 位于 `0x005F42CD`。进入登录阶段后，明确请求：

- `UI/MapLogin.img` 及其 `info`
- `UI/Login.img/Common/frame`
- `CMapLoadable::Init` 所需的登录地图属性、图层等资源
- `CMapLoadable::PlayBGMFromMapInfo` 所需的 BGM

这说明首屏之前既有所有主 WZ 的 package 初始化，也有 UI、Map、Sound 三条资源解析链；
并不是只加载一张登录背景。

## NameSpace.dll 的两条路径

`NameSpace.dll+0xE4DB` 创建 package stream。只读模式下调用 `+0xCFCB`，后者：

1. `CreateFileMappingA`
2. `MapViewOfFile(offset=0, size=0)`，即映射完整文件
3. 成功后记录映射起止范围，后续读取直接从映射地址 `memcpy`
4. 失败后设置 stream fallback 标志并关闭 mapping handle

fallback 的 `ReadFile` 并非异步 I/O，也没有 DLL 内部 read-ahead。当前项目此前只完成了
第 4 步的强制触发，所以降低了 32 位虚拟地址占用，却把读取粒度完全暴露给文件系统。

## 本项目优化

`NameSpaceStreamingHook.cpp` 现在同时 hook `ReadFile`、`SetFilePointer` 和
`CloseHandle`：

- 只处理返回地址属于 `NameSpace.dll` 的同步小读，不影响其他模块
- 每个 WZ 文件句柄保留一个默认 32 KiB 的 read-ahead 窗口
- 命中时从窗口复制；未命中时合并为一次后端读取
- 在用户态维护 DLL 期望的逻辑文件位置，只在未命中时同步底层句柄
- 直接回答 DLL 在每次 fallback 读取前执行的零偏移 `FILE_CURRENT` 查询
- `FILE_BEGIN`、`FILE_CURRENT` 和 `FILE_END` seek 只更新逻辑游标
- `NameSpace.dll` 的模块地址范围只解析一次，热路径只做地址上下界比较
- 使用稳定条目和最近句柄快速路径，连续小读不再反复扫描全部 WZ 句柄
- 小于配置阈值的 WZ 在确认窗口读取量已经超过文件大小两倍后，自适应晋升为整文件缓存
- `CloseHandle` 时删除对应窗口，防止句柄复用返回旧数据
- 保留整文件映射失败策略，不重新占用数 GiB 虚拟地址

配置：

```ini
nameSpaceStreaming=true
nameSpaceStreamingReadAheadKiB=32
nameSpaceStreamingSmallFileCacheMiB=16
```

预读值为 `0` 时关闭预读，最大值限制为 `4096` KiB；小文件缓存值为 `0` 时关闭晋升，
最大值限制为 `128` MiB。实测 32 KiB 比 256 KiB 明显降低读取放大，也比 16 KiB 更快：
16 KiB 虽把后端读取量从约 907 MB 降到 612 MB，却把后端调用从 27541 次增加到
36908 次，登录阶段耗时为 8.23 秒；32 KiB 的对应耗时为 8.12 秒。因此当前默认值保留
32 KiB。

启用 16 MiB 小文件缓存后，`String.wz`（实际约 8.67 MB）按预期晋升：该文件的后端
读取从约 308.2 MB / 9408 次降到 26.0 MB / 531 次。全局后端读取从约 907.3 MB /
27541 次降到 625.1 MB / 18664 次，分别减少约 31.1% 和 32.2%；登录阶段墙钟时间从
8.12 秒降到 7.91 秒。热文件缓存下墙钟收益不大，但后端随机 I/O 的削减会主要惠及
机械硬盘、实时扫描和较慢的兼容层文件系统。

其余主要读取来自 `Item.wz`（约 278 MB）、`Skill.wz`（约 143 MB）和 `Mob.wz`
（约 136 MB）。它们的文件大小分别约为 219 MB、98 MB 和 1.37 GB，当前访问量不足以
证明整文件缓存能减少总读取；提高小文件阈值会增加 32 位进程的地址空间压力，且可能
为了缓存读入更多数据，因此 16 MiB 应继续作为保守默认值。

启动完成后关注：

```text
[NameSpaceStreaming] stats phase=InitializeResMan elapsedMs=... fallback=... logicalCalls=... logicalBytes=... cacheHits=... backendCalls=... backendBytes=...
[NameSpaceStreaming] stats phase=CLogin::Init elapsedMs=... fallback=... logicalCalls=... logicalBytes=... cacheHits=... backendCalls=... backendBytes=...
```

最关键的指标是 `backendCalls / logicalCalls`。该比例越低，越多细碎读取被合并。还应同时
比较两个 phase 的累计值即可分离 ResMan 初始化与后续 `CLogin::Init` 的 UI/Map/Sound
解析；`elapsedMs` 从 hook 安装开始计时，接近进程启动到对应 phase 返回的墙钟时间。
每个 phase 后的 `file` 行用于确认具体是哪些 WZ 产生了读取量。

## 后续可选方案

如果单窗口在实际日志中频繁抖动，可改为每句柄 2～4 个 LRU 窗口，但内存会按窗口数
线性增加。更激进的方案是 hook `NameSpace.dll` 内部 stream vtable 并实现分页映射；它
依赖该 DLL 的固定 RVA 和对象布局，版本兼容性、异常清理和并发风险都明显高于当前的
Win32 API 层 read-ahead，不建议作为第一步。
