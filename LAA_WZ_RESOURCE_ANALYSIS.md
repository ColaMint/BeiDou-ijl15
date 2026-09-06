# MapleStory v83 WZ 资源链与 LAA 高地址兼容性分析

## 1. 文档目的

本文记录 BeiDou v83 客户端在 Windows 真机开启
`IMAGE_FILE_LARGE_ADDRESS_AWARE`（LAA）后，浏览怪物手册偶发闪退的分析结果，供后续继续修复
`BeiDou.exe` 及其资源 DLL 时使用。

分析对象：

- 客户端：`/Users/colamint/Maplestory/BeiDou-Client-ASM-WZ/BeiDou.exe`
- IDA 数据库：`/Users/colamint/Maplestory/idb/v83.i64`
- 诊断日志：`out/Release/ijl15.log`
- 原始 LAA EXE 备份：`BeiDou.exe.laa-enabled.bak`
- 当前临时兼容方案：`BeiDou.exe` 的 PE Characteristics 已从 `0x012F` 改为
  `0x010F`，即关闭 LAA。

用于确认二进制版本的 SHA-256：

```text
1198fa57ca5a7c489bae43ec13c69681d9cabe0f96762f3dc0357facf2e7d4df  BeiDou.exe.laa-enabled.bak
9a01f8a81fc6fe6aac2f564f4e053ea8882d751819751164cbcc8765b5b7b8ed  BeiDou.exe (关闭 LAA)
5e31263f9dbab4b6f161aa85485162d01038bddc859821cafe88ab805530189e  ResMan.dll
7eed6c13a60a99bd3f7c2e22ae4b9f0cdb492361260a81172703f260ab661251  NameSpace.dll
917181e24f3b152302f9f93adf4ac3e13baf086a68224d258f47df5bb3862cf1  PCOM.dll
ce65505fc65b3c03c22db309c66443e9b81fc1aa8e246c3bb4757bdd66a98ce9  ZLZ.dll
```

## 2. 结论摘要

1. WZ 资源读取不是只由一个 DLL 完成，而是由 `ResMan.dll`、`NameSpace.dll`、
   `PCOM.dll` 和 `ZLZ.dll` 分层协作。
2. 本次闪退并不是 WZ 节点缺失。传给 `IWzProperty::GetItem` 的 BSTR 参数在进入
   `GetItem` 前就已损坏。
3. 当前最明确的 LAA 不兼容代码位于 `BeiDou.exe` 的 ANSI 到 BSTR 转换函数：
   它向 `MultiByteToWideChar` 传入硬编码目标容量 `0x3FFFFFFF`，而不是实际分配容量。
4. 当目标 BSTR 位于高 2 GB，例如 `0xF8855D48` 时，Windows 对“目标地址 + 声明容量”
   的范围检查会溢出或判定容量无效，转换失败；日志中同时出现的 Win32 错误 122
   (`ERROR_INSUFFICIENT_BUFFER`) 与该行为吻合。
5. Wine 的实现和地址布局没有稳定触发这个边界条件，所以 Wine 稳定并不代表程序真正支持
   LAA。
6. `ResMan.dll`、`NameSpace.dll` 和 `PCOM.dll` 内也发现了相同的
   `push 0x3FFFFFFF` 转码模式。若以后要恢复 LAA，不能只修 `ResMan.dll`，至少要同时审计
   EXE 和整个 WZ 调用链。

证据等级：

- **已证实**：关闭 LAA 后实际闪退消失；失败 BSTR 位于高 2 GB；BSTR 在进入
  `IWzProperty::GetItem` 前已经损坏；EXE 和多个 DLL 中存在超大假容量转码代码。
- **强推断**：高地址加 `0x3FFFFFFF` WCHAR 的声明范围触发原生 Windows 转码实现的
  溢出/区间校验，从而留下未写入的 BSTR。地址分界、坏字符串和错误 122 均支持该推断。
- **尚未证实**：`ResMan.dll` 内存在某个具体的“有符号指针比较”并直接造成了本次崩溃。
  当前证据反而首先指向 EXE 的 BSTR 构造路径。

## 3. 哪些 DLL 在读取 WZ

| 模块 | 已确认的职责 | 证据 |
| --- | --- | --- |
| `ResMan.dll` | 资源管理、缓存和按路径取得对象，提供 `IWzResMan` 一侧的实现 | 运行时 `g_rm` 对象虚表位于 `ResMan.dll`：`0x51006140` |
| `NameSpace.dll` | 打开和映射 WZ/package 文件，维护命名空间、archive、package 和 stream | 导入 `CreateFileMappingA`、`MapViewOfFile`、`SetFilePointer`；RTTI 包含 `CWzPackage`、`CWzArchive`、`CWzRawArchive`、`CWzFileSystem`；运行时 `g_root` 虚表为 `0x50817150` |
| `PCOM.dll` | WZ 属性对象和 COM 基础设施，包括 property、list、UOL、序列化 | RTTI 包含 `CWzProperty`、`CWzList`、`CWzUOL`；本次父 property 虚表为 `0x50C131C4` |
| `ZLZ.dll` | WZ 数据流的压缩/解压 | 包含 `ZLZCreateInflator`、`ZLZCreateDeflator`，内部标识为 inflate 1.1.3 |

可把主要调用关系理解为：

```text
BeiDou.exe
  -> ResMan.dll       按资源路径请求和缓存对象
  -> NameSpace.dll    打开、映射并遍历 WZ/package
  -> PCOM.dll         表示 property/list/UOL 等节点并提供 COM 接口
  -> ZLZ.dll          解压需要展开的数据流
```

`Canvas.dll`、`Shape2D.dll`、`Gr2D_DX8.dll` 和 `Sound_DX8.dll` 会消费 WZ 中的图形、
形状和声音数据，但不是打开 WZ 容器的唯一入口。

注意：DLL 自身的 PE Characteristics 没有 LAA 位并不能保护它。地址空间上限由主 EXE
决定；只要 DLL 被加载进 LAA 进程，它就可能收到 `>= 0x80000000` 的指针。

## 4. 本次崩溃的直接证据

正常查询示例：

```text
BSTR=0x7F38D368
UOL UTF-16 units: 0069 006E 0066 006F
UOL escaped: info
Result vt: 13 (VT_UNKNOWN)
```

第一次发现的坏查询来自装备 `1482003`：

```text
BSTR=0xF8856498
UOL UTF-16 units: 0000 004F 004C 0000
UOL escaped: \0OL\0
LastError: 122
Result vt: 0 (VT_EMPTY)
HRESULT: 0x80004003 (E_POINTER)
```

最终触发错误弹窗的怪物手册查询：

```text
Monster Book card item ID: 2381099
Monster ID: 9410039
BSTR=0xF8855D48
UOL escaped: \0OL\0
LastError: 122
Result vt: 0 (VT_EMPTY)
HRESULT: 0x80004003 (E_POINTER)
FINAL ERROR DIALOG
```

关键点：

- 前 286 次成功查询的 BSTR 都低于 `0x80000000`。
- 两次失败的 BSTR 都位于 `0xF8xxxxxx`。
- property 父对象、虚表、`g_rm`、`g_root` 和当前目录在失败时仍然有效。
- 参数在进入原始 `IWzProperty::GetItem` 前已经不是 `"info"`。
- 所以 `Mob/9410039.img/info` 的“空结果”是坏查询参数造成的后果，不是 WZ 缺数据。

## 5. 已定位的危险实现

### 5.1 EXE 中的调用链

相关地址以当前 v83 IDB 为准：

```text
StringPool::GetBSTR                 0x00406292
  -> _bstr_t/Data_t 构造           0x00406359
  -> ANSI 到 BSTR 转换             0x0040638C
  -> 自定义 BSTR 分配              0x00402B4F

IWzProperty::GetItem hook 入口      0x00403935
装备 root -> "info" 调用点         0x005CADC3
怪物 root -> "info" 调用点         0x00866E28
怪物 info -> "default" 调用点      0x00866EA6
```

`0x0040638C` 的逻辑可还原为：

```cpp
int required = MultiByteToWideChar(0, 0, input, -1, nullptr, 0);
BSTR output = AllocCustomBSTR(required - 1);

// 错误：output 实际只有 required 个 WCHAR（含结尾 NUL）的容量。
MultiByteToWideChar(0, 0, input, -1, output, 0x3FFFFFFF);
return output;
```

对应关键汇编：

```asm
004063A4  call dword ptr [00BF03D0] ; 查询 required
004063AE  dec eax
004063B1  call 00402B4F            ; 分配 required - 1 字符的 BSTR
004063B8  push 3FFFFFFFh            ; 错误的 cchWideChar
004063C5  call dword ptr [00BF03D0] ; MultiByteToWideChar
```

低地址时，`output + 0x3FFFFFFF * sizeof(wchar_t)` 尚未发生 32 位无符号地址回绕，
Windows 的实现碰巧允许调用继续。`output` 位于 `0xF8xxxxxx` 时，这个声明范围必然回绕，
原生 Windows 拒绝转换，实际 BSTR 数据没有被正确写入。Wine 对参数检查和内存区间验证的
实现不同，因此没有稳定复现。

`LastError=122` 是在 `GetItem` 返回后采集的，严格来说它可能是线程上遗留的错误码；但它与
上述 `MultiByteToWideChar` 失败、BSTR 未写入以及高地址分界完全一致，是强关联证据。

### 5.2 BSTR 最终从哪里分配

`0x00402B4F` 调用函数指针 `dword_BF0550` 分配 `2 * length + 6` 字节，写入 4 字节
BSTR 长度前缀并返回 `allocation + 4`。

初始化函数在 `0x00796666` 将该指针设置为 EXE 导出的分配函数。EXE 导出如下：

```text
ZtlTaskMemAllocImp     RVA 0x005F2503 / VA 0x009F2503
ZtlTaskMemFreeImp      RVA 0x005F2514 / VA 0x009F2514
ZtlTaskMemReallocImp   RVA 0x005F2525 / VA 0x009F2525
```

`ZtlTaskMemAllocImp` 再进入 `ZAllocEx<ZAllocAnonSelector>::Alloc` (`0x00403065`)。
因此，本次坏 BSTR 的分配和第一次转码发生在 EXE 侧；不能只因为最终 COM 调用进入
`PCOM.dll` 就认定根因一定在 `ResMan.dll`。

### 5.3 同类模式不只存在于 EXE

静态扫描发现以下模块包含 `push 0x3FFFFFFF` 形式的超大转码容量。计数是该立即数作为
`push` 的出现次数，是审计候选数，不代表每一处都已确认属于同一个 API：

| 模块 | 候选数 | 已确认示例地址 |
| --- | ---: | --- |
| `BeiDou.exe` | 46 | `0x004063B8` |
| `ResMan.dll` | 1 | `0x51005044` |
| `NameSpace.dll` | 5 | `0x50803A4B`、`0x50805031` |
| `PCOM.dll` | 4 | `0x50C02A1A`、`0x50C0857A` |
| `Canvas.dll` | 3 | `0x50002835` |
| `Shape2D.dll` | 2 | `0x51402DA0` |
| `Gr2D_DX8.dll` | 3 | `0x5040536B` |
| `Sound_DX8.dll` | 4 | `0x51802BE5` |

WZ 核心链中的 `ResMan.dll`、`NameSpace.dll` 和 `PCOM.dll` 已经能看到与 EXE 非常相似的
“先计算长度、分配、再以 `0x3FFFFFFF` 作为输出容量调用转码 API”代码形态。后续应将每个
DLL 单独载入 IDA，确认间接调用究竟是 `MultiByteToWideChar` 还是
`WideCharToMultiByte`，并分别改为真实容量。

## 6. 后续修复时应重点搜索的代码姿势

### 6.1 硬编码超大输出容量

最高优先级搜索：

```asm
push 3FFFFFFFh
push <destination pointer>
...
call MultiByteToWideChar/WideCharToMultiByte
```

正确源代码应保留第一次长度查询的返回值，并把真实容量传给第二次转换：

```cpp
int required = MultiByteToWideChar(cp, flags, src, -1, nullptr, 0);
if (required <= 0)
    return failure;

BSTR dst = AllocateBSTRCharacters(required - 1);
if (!dst)
    return out_of_memory;

int written = MultiByteToWideChar(cp, flags, src, -1, dst, required);
if (written != required) {
    FreeBSTR(dst);
    return failure;
}
```

宽转窄版本也必须使用实际字节容量，不能继续使用 `0x3FFFFFFF`。

### 6.2 把指针当有符号整数判断

需要人工确认下面的反编译结果是否真的在判断“指针值”：

```cpp
int p = (int)pointer;
if (p < 0) ...
if (p >= 0) ...
```

汇编中常表现为对指针寄存器执行 `test`/`cmp` 后使用 `js`、`jns`、`jl`、`jle`、
`jg` 或 `jge`。高地址 `0x80000000..0xFFFFFFFF` 按 `int32_t` 会被视为负数。

修复原则：

- 指针只与 `nullptr` 比较。
- 地址排序和边界检查使用 `uintptr_t` 及无符号跳转条件。
- 不要用 `-1` 或负数与正常指针共用一个字段表示失败。

### 6.3 地址加长度发生 32 位回绕

危险形式：

```cpp
if (pointer + length < pointer) ...
end = (DWORD)pointer + byte_count;
```

必须先验证长度是否超过 `UINTPTR_MAX - address`，并确保 API 收到的容量与实际分配容量一致。
本次 `0x3FFFFFFF` 正是这种问题的具体实例。

### 6.4 使用指针最高位作为标志

搜索：

```cpp
value & 0x80000000
value | 0x80000000
value & 0x7FFFFFFF
```

如果 `value` 是地址或可能携带地址，高位标记与 LAA 不兼容。标志应放进独立字段。

### 6.5 指针截断、打包或序列化

搜索指针到 `LONG`、`int`、31 位位域及自定义句柄之间的转换。32 位程序中 `DWORD` 虽然不会
截断指针宽度，但如果后续采用有符号比较，仍会破坏高地址语义。

### 6.6 自定义分配器和空闲链表

重点检查：

- freelist 头或 next 指针是否使用有符号判断；
- chunk 地址的加减、对齐是否可能回绕；
- 跨 EXE/DLL 的 alloc/free 是否来自同一分配器；
- BSTR 的 `pointer - 4` 长度前缀是否始终有效；
- 引用计数释放后是否仍有缓存指针。

但不要机械修改所有最高位判断。当前 EXE 的
`ZAllocEx<ZAllocAnonSelector>::Free` (`0x004031ED`) 会检查 `[pointer - 4]` 的最高位；
这里判断的是“分配块长度头”，不是指针本身，可能是合法的元数据编码。

## 7. 不应误判或随意修改的模式

- `HRESULT < 0` 或汇编中的 `test eax,eax; jl ...`：COM 使用最高位表示失败，这是正确语义。
- 对 `VARIANT.vt`、字符串长度、分配块长度头的最高位判断：先确认字段类型，不能按指针修。
- `test pointer,pointer; jz ...`：这是普通空指针判断，与 LAA 无冲突。
- `0x80000000` 出现在哈希、颜色、压缩或加密算法中：通常与地址无关。
- 只把 DLL 的 PE 头也加上 LAA：DLL 的这个标志不能修正内部代码，反而容易制造错误安全感。

## 8. 推荐的修复顺序

1. 保持当前关闭 LAA 的 EXE，作为可工作的基线。
2. 在副本上恢复 EXE 的 LAA 位，先修 `BeiDou.exe:0x0040638C` 的真实容量问题。
3. 同时审计 EXE 中另外 45 个 `push 0x3FFFFFFF` 候选点。
4. 分别载入 `ResMan.dll`、`NameSpace.dll`、`PCOM.dll`，确认并修复其转码调用。
5. 再审计 `Canvas.dll`、`Shape2D.dll`、`Gr2D_DX8.dll`、`Sound_DX8.dll`，避免在加载图片或
   声音后出现另一类高地址失败。
6. 最后审计所有模块中的有符号指针判断、最高位标记和地址范围运算。

不建议把“修 DLL”限定为只改 `ResMan.dll`。本次已捕获的直接失败点在 EXE 的 BSTR 构造
路径，而且相同运行库模板被静态编译进多个 DLL。

## 9. 恢复 LAA 后的验证标准

修复版必须在 Windows 真机上验证，Wine 只能作为辅助测试环境：

1. 恢复 `IMAGE_FILE_LARGE_ADDRESS_AWARE`，确认 PE Characteristics 为 `0x012F`。
2. 让进程产生高 2 GB 分配，日志中应实际观察到 `BSTR >= 0x80000000`。
3. 即使 BSTR 位于高地址，`"info"` 的 UTF-16 单元仍应为
   `0069 006E 0066 006F`，且转换 API 返回成功。
4. `IWzProperty::GetItem` 应返回 `VT_UNKNOWN`/有效对象，不得出现由坏 key 导致的
   `VT_EMPTY` 和 `0x80004003`。
5. 重复快速浏览怪物手册、装备加载、地图切换和声音/画布加载。
6. 做长时间运行和内存压力测试，确认没有 alloc/free 跨模块错误及其他高地址问题。

如果只修一个调用点后怪物手册不再崩溃，仍不能宣布整个客户端支持 LAA；只有在多个模块都
真实接收过高地址指针并通过上述测试后，才可以重新默认开启 LAA。
