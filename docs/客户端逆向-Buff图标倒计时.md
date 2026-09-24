# 客户端逆向：buff 图标倒计时

> 姊妹文档：IDA 书签（前缀 `idaMCP: BUFFTIMER:`）与本文件成对；改址或改述时两侧同步。
> 基线：`Angel.exe`（v83，IDA `D:\Downloads\Angel\Angel.i64`）；发布客户端为 `BeiDou.exe`。
> 关联实现：`ezorsia/BuffTimer.cpp`、`ezorsia/BuffTimer.h`；配置键见 `ezorsia/config.ini` 的 `buffTimer*`。
>
> **地址族群提醒**：本次逆向在 `Angel.exe` 上完成，与工程既有地址（BossHP/QuestBulb 等）同源。
> 换客户端前必须在目标 exe 的 i64 上复核下表。

## 1. 关键函数与地址

### 1.1 客户端侧（读取与排版）

| 地址 | 符号 | 原型 | 用途 |
|---|---|---|---|
| `0x00A202BE` | `CWvsContext::OnTemporaryStatSet` | `void __thiscall(CInPacket&)` | GIVE_BUFF 入口；本特性在此抓时长 |
| `0x00A2071F` | `CWvsContext::OnTemporaryStatReset` | `void __thiscall(CInPacket&)` | CANCEL_BUFF；**只带统计掩码，不带 id**，故未 hook |
| `0x00A03350` | `CWvsContext::Update` | `void __thiscall(void)` | 每帧 tick（内部调 `sub_7B2829` 驱动 buff 视图） |
| `0x00781D0E` | 临时状态解码器（未命名） | — | 逐位读 `{value, id, len}`，`timeGetTime()+len` 存为到期时刻 |
| `0x007B24D5` | `CTemporaryStatView::SetTemporary` | `void __thiscall(int,int,int,UINT128,ZXString,int,int)` | 新增一个图标条目 |
| `0x007B2679` | `CTemporaryStatView::ResetTemporary` | `void __thiscall(int,int)` | 删条目：`sub_7B4BD1` 摘链 + 释放 entry |
| `0x007B4BD1` | 摘链 + 释放条目（未命名） | — | ZList 摘除；**图层的生死完全靠引用计数** |
| `0x007B3176` | 条目构造函数 | — | 建 `entry+0x28`（图标）与 `entry+0x2C`（覆盖层）两个 Gr2DLayer |
| `0x007B2BB0` | `CTemporaryStatView::AdjustPosition` | `int __thiscall(void)` | 用 `raw_RelMove` 把两层叠在同一像素（增删 buff 后整行重排） |
| `0x007B2E58` | `CTemporaryStatView::ShowToolTip` | `int __thiscall(CUIToolTip&, tagPOINT&, long&, long)` | 读条目字段（本文件的字段布局来源之一） |
| `0x007B44F4` | 冷却数字重画（未命名） | — | 客户端自带的技能冷却数字（`entry+0x38 / +0x3C` 分档 → `UIWindow.img/Skill/Cooldown` 数字图） |
| `0x007B4819` | 单图标数值写入 | `void __thiscall(int value)` | 写 `entry+0x38`，并调 `sub_7B44F4` |
| `0x007B4D1D` | `ZList<ZRef<TEMPORARY_STAT>>::FindIndex` | `__POSITION* __thiscall(unsigned int) const` | 取第 n 个图标节点 |
| `0x004374CB` | `IWzGr2D::GetCenter` | `_com_ptr_t<IWzVector2D> __thiscall(void)` | Gr2D 中心向量：**图标层的 origin 就是它**（条目构造调用） |

### 1.2 Gr2D / 画布 / 字体（本特性的绘制口）

| 地址 | 符号 | 原型 | 用途 |
|---|---|---|---|
| `0x00426C7E` | `IWzGr2D::CreateLayer` | `_com_ptr_t<IWzGr2DLayer> __thiscall(long x,long y,ulong cx,ulong cy,long z,variant&,variant&)` | 建标签层 |
| `0x00425D2E` | `IWzGr2DLayer::GetCanvas` | `_com_ptr_t<IWzCanvas> __thiscall(variant&)` | 取（必要时建）图层画布；raw 槽 **+256** |
| `0x004143FB` | `IWzGr2DLayer::RemoveCanvas` | `_com_ptr_t<IWzCanvas> __thiscall(variant&)` | 摘掉画布；raw 槽 **+264**。**本特性不用**（见 §6.2） |
| `0x0048ECA7` | `IWzCanvas::Create` | `long __thiscall(int cx,int cy,variant&,variant&)` | **原地重分配画布 = 擦除**；raw 槽 **+44** |
| `0x004277AD` | `IWzCanvas::DrawTextA` | `unsigned long __thiscall(long,long,Ztl_bstr_t,IWzFont*,variant&,variant&)` | 画字 |
| `0x0042782E` | `IWzFont::CalcTextWidth` | `int __thiscall(Ztl_bstr_t,variant&)` | 量宽（水平钳制用） |
| `0x0045144A` | `IWzGr2DLayer::Putcolor` | `void __thiscall(unsigned long argb)` | raw 槽 **+224**；`0xFFFFFFFF` 可见 / `0` 隐藏 |
| `0x00440C00` / `0x00440C2A` | `IWzGr2DLayer::GetWidth/GetHeight` | `long __thiscall(void)` | 图层尺寸（= `CreateLayer` 传入的 cx/cy） |
| `0x0044337D` | `IWzGr2DLayer::GetZ` | `long __thiscall(void)` | 取图标层深度，标签层照抄 |
| `0x0098A707` | `get_basic_font` | `_com_ptr_t<IWzFont> __cdecl(FONT_TYPE)` | 56 个字体槽（缓存于 `0x00BF13B8`），含颜色与字号 |
| `0x00463670` | `PcCreateObject::IWzFont` | `int __cdecl(const wchar_t* 类名, void** ppOut, int)` | 建字体对象（`get_basic_font` 同款工厂） |
| `0x0046341A` | `IWzFont::Init` | `long __thiscall(_bstr_t* 字面, int size, unsigned long argb, variant&)` | 设字号/颜色；raw 槽 **+12**；**会释放传入的 bstr** |
| `0x0079E805` | `StringPool::GetInstance` | `StringPool& __cdecl(void)` | 取串池单例 |
| `0x00406276` / `0x00406292` | `StringPool::GetStringW` / `GetBSTR` | `ZXString<G>/Ztl_bstr_t __thiscall(unsigned int id)` | 字体类名（id **1410**）与字面名（id **5527**） |
| `0x004033A7` | `ZXString<unsigned short>::_Release` | `void __cdecl(_ZXStringData*)` | 释放 `GetStringW` 的返回值（传 `数据指针-12`） |

vtable 槽位汇总（本轮实测）：`IWzVector2D` `get_x +32` / `get_y +40` / `put_origin +100` / `raw_RelMove +144`；
`IWzGr2DLayer` `PutZ +180` / `Putcolor +224`；`IWzCanvas` `Create +44` / `DrawCanvas +128` / `DrawRectangle +140`。

**调用约定（坑）**：`GetCanvas`/`RemoveCanvas`/`CreateLayer`/`GetCenter` 都是「按值返回 `_com_ptr_t`」的成员函数。
MSVC 把隐藏返回槽当**第一个栈参数**压入，因此实际形态是
`__fastcall(pThis /*ecx*/, edx, void** ppOut /*栈*/, …其余实参…)`。
参数个数写错会静默毁栈（参见 `QuestBulb.cpp` 的同类记录）。同理：

- `Ztl_bstr_t` / `Ztl_variant_t` 都是**4 字节单指针**，按值传递时栈上就是那一个指针；本工程用
  `_bstr_t::_bstr_t(char const*)`（`0x00406301`）在本地槽位里就地造一个，**调用方不得再释放**（被调方会释放）。
- `raw_RelMove(x, y, v1, v2)` 必须把两个 VARIANT 显式写出来（漏传 = 栈上 32 字节垃圾）。

## 2. 数据结构

### `CWvsContext`

| 偏移 | 内容 |
|---|---|
| `+0x2EA8` | `CTemporaryStatView`（buff 图标行） |

### `CTemporaryStatView`

| 偏移 | 内容 |
|---|---|
| `+0x04` | 内嵌 `ZList<ZRef<TEMPORARY_STAT>>` 起始 |
| `+0x0C` | 排版基准项数（`AdjustPosition` 的 `32 * *(this+3)`） |
| `+0x10` | 链表头 |
| `+0x14` | 链表尾 |

`FindIndex` 的 `this` = **`view+0x04`**；返回节点指针，`*(节点+4)` = 条目指针。

### `CTemporaryStatView::TEMPORARY_STAT`（0x40 字节）

| 偏移 | 内容 |
|---|---|
| `+0x04` | 引用计数 |
| `+0x1C` | `nType`：1=道具 buff，2=技能 buff，3/4=家族/公会等 |
| `+0x20` | `nID`（**道具 buff 为负数**：`OnTemporaryStatSet` 在调 `SetTemporary` 前取反） |
| `+0x24` | 提示字符串 |
| `+0x28` | 图标 `IWzGr2DLayer`（**标签层挂靠它**） |
| `+0x2C` | 覆盖层 `IWzGr2DLayer`（与图标逐像素同位；客户端的冷却数字画在这层） |
| `+0x30` | 上次显示的冷却档位 |
| `+0x34` | 冷却开关（条目构造里由 `Item.wz` prop 4037 写入） |
| `+0x38` | 当前冷却值 |
| `+0x3C` | 冷却除数 = 满值/16（`sub_7B44F4` 算 `+0x38 / +0x3C` 并钳到 15） |

### `CInPacket`

| 偏移 | 内容 |
|---|---|
| `+0x08` | 数据基址 |
| `+0x14` | **读游标**（`Decode1`/`Decode2` 都推进它） |

## 3. 报文布局（GIVE_BUFF）

服务端 `PacketCreator.giveBuff` 写入顺序与客户端解码顺序逐字段一致：

```
writeLongMask              16 字节（两个 long = 128 位统计掩码）
每个置位： writeShort(value) | writeInt(buffid) | writeInt(bufflength)
writeInt(0) | writeByte(0) | writeInt(首个 statup 的值) | [special 时 skip(3)]
```

- 客户端 `sub_781D0E` 用 `CInPacket::DecodeBuffer(mask, 16)` 取掩码，`timeGetTime()` 取一次当前时间，
  然后逐位置读 `Decode2 / Decode4 / Decode4`，存 `now + bufflength` 为到期时刻。
- **`bufflength` 单位是毫秒**（技能 `time` 秒 ×1000、道具时长本就按 ms 处理，见 `StatEffect.java:298-303`）。
- **本插件的数字完全来自这个字段**，不读 wz、不做任何时长推导 ⇒ 显示的时长永远等于服务端下发的时长。
  服务端自身可能对时长做修正（例：药剂精通 `Y%` 对物品/技能的效果，登记在 Ursa-Server 的
  `docs/milestones/M3.23-飞侠三转技能对拍与回补.md` G-1），因此**数字与技能说明文字不一致时以服务端为准**。

本特性的 `CaptureDurations` 在 hook 里**保存并还原 `CInPacket+0x14`**，客户端随后按原样解析。
由于每个三元组是「id 与时长一起读」，**掩码位的遍历顺序不影响配对正确性**，只需要三元组个数 = 置位数。

## 4. 字体：`spFontBasic` 56 槽与本插件自建字体

`get_basic_font`（`0x0098A707`）是一个 56 分支的 switch（跳表 `0x98CB81`），每支都走同一套构造：
`StringPool::GetStringW(1410)` 取类名 → `PcCreateObject::IWzFont` 建对象 → `StringPool::GetBSTR(5527)`
取字面名 → `IWzFont::Init(字面, 字号, ARGB, empty)`。结果缓存进 `spFontBasic`（`0x00BF13B8`）。
**颜色与字号由槽位决定，不由绘制调用决定**（绘制只传字体指针）。

逐槽实测（字号 / ARGB）：

| 槽位 | 字号 | 颜色 |
|---|---|---|
| 0、19、22、24、28、29 | 12 | `0xFFFFFFFF` 白 |
| 1、18、21、25、30、37、44、49 | 12 | `0xFF000000` 黑 |
| 2、14、40 | 12 | `0xFF674B36` 棕 |
| 3 | 12 | `0xFF404040` 深灰 |
| 4、27 | 12 | `0xFFFFFF20` 黄 |
| 5、30、32、38、45 | 12 | `0xFF2000FF` 蓝 |
| 6 | 12 | `0xFF64B4F0` 天蓝 |
| 7、31、33、39、46 | 12 | `0xFFFF2020` 红 |
| 8 | 12 | **`0xFF28C99B` 薄荷绿** |
| 9 | 12 | `0xFFFF99CC` 粉 |
| 10 | 12 | `0xFFFF9900` 橙 |
| 11 | 12 | `0xFFFF3399` 品红 |
| 12 | 12 | `0xFF006699` 深蓝 |
| 13 | 12 | `0xFFCC0066` 紫红 |
| 15 | 12 | `0xFF336600` 深绿 |
| 16、17 | 12 | `0xFFE9C4FF` / `0xFFA4F0FF` 淡紫/淡青 |
| 20、26 | 12 | `0xFFBBBBBB` / `0xFF505050` 灰 |
| 34 / 36 / 35 | 11 / 11 / **15** | 白 / 灰 / 白 |
| 41–48 | **9** | 白/黑/蓝/红/米/白/黑（唯一 9px 组） |
| 50–53 | 12 | `0xFFFF7E00` / `0xFFBE3C03` / `0xFF007AF4` / `0xFF00BDC4` |
| 54、55 | 12 | `0xFF629A00` 橄榄 / `0xFFFF5400` 橙红 |

**表里没有的东西**：最亮的绿也只有 `0xFF28C99B`，且"有色 + 比 12px 大"一档都没有（15px 那档是白的）。
所以本插件**自建字体**：照抄上面那条构造链，但字号/颜色换成 `config.ini` 的
`buffTimerSize` / `buffTimerMinuteColor` / `buffTimerSecondColor` / `buffTimerOutlineColor`
（`0xRRGGBB`，插件补 `0xFF` alpha）。`buffTimerSize=-1` 时跳过自建、退回表槽位（`buffTimerMinuteFont` 等）。
自建失败（任一步抛 `_com_error` 或返回空）也退表槽位，只记 `buff_timer.log`，不影响功能。

## 5. 绘制方案

**每个图标一个自有标签层**（不借用 `entry+0x2C`，原因见 §6.1），尺寸 = 图标槽位的 32×32：

```
IWzGr2D::CreateLayer(gr2d=*(IWzGr2D**)0x00BF14EC, 0, 0, 32, 32, z=0xC006156C, empty, empty)
IWzVector2D::put_origin  (+100)  <- VARIANT{VT_UNKNOWN, 图标层 entry+0x28}
IWzVector2D::raw_RelMove (+144)  <- (0, 0, empty, empty)          ← 与图标层逐像素同位
IWzGr2DLayer::PutZ       (+180)  <- 图标层的 GetZ
IWzGr2DLayer::Putcolor   (+224)  <- 0xFFFFFFFF                     ← 新层不调它不渲染
```

- **origin = 图标层**（不是 Gr2D 中心）：这样客户端 `AdjustPosition` 每次重排整行时，标签自动跟着走，
  插件不需要自己算坐标。代价是 origin 会持有图标层的 COM 引用，**必须在 buff 消失时交还**（§6.3）。
- **擦除 = `IWzCanvas::Create(cx, cy, empty, empty)`**（raw `+44`）：图层自己的画布原地重分配成空白，
  对象不变、图层引用不变。这是客户端自己的复用方式（`CField_LimitedView::Init` `0x0055BC6C`、
  `sub_537FDA` `0x005384AA` 都对成员画布调它）。数字变化时先擦后画，`10 → 9` 不会残留 `0`。
- **画字**：`DrawTextA(canvas, x, y, bstr, font, empty, empty)`；描边 = 黑字字体在 8 个邻位各画一遍，
  再画主体色一遍（BossHP 已验证的同一手法）。位置 `(buffTimerX, buffTimerY)`，并按图层宽高与字号钳制。
- **分档与封顶**：`bufflength ≥ buffTimerMaxMinutes`（默认 10 分钟）→ 不显示；`≥ 1 分钟` → 绿色向上取整分钟；
  否则黄色向上取整秒。**边界向上取整**：剩 9:59 显示 `10`，剩 59.9 秒显示 `60`。

## 6. 踩坑

### 6.1 零尺寸图层 = 共享画布

条目构造函数建层时传的是**宽度 = 高度 = 0**（`0x7B407A` 图标层、`0x7B431D` 覆盖层，均为
`CreateLayer(0,0,0,0,0xC006156C,(VT_I4)0,empty)`）。**0×0 的图层没有自己的画布**：`GetCanvas` 对每一个
这样的图层都返回**同一块共享画布**（实测三个不同图层的 `GetCanvas` 返回同一个指针 `24E9CD1C`）——
画上去的数字会同时出现在**所有** buff 图标上（实机：2 倍 drop 本来正确不显示，一用轻功就跟着变成 5）。
`CUIToolTip::MakeLayer`（`0x008F3141`）传的是真实宽高，故不踩此坑。⇒ 自建层必须给真实尺寸。

### 6.2 `RemoveCanvas` 是重画数字的死路

`RemoveCanvas`（raw `+264`）把图层的画布**摘走**，下一次 `GetCanvas` 另建一块。实测：这样换过画布之后
**图层整体不再渲染**（数字全部消失），且它还需要一个 variant 实参（客户端自己传 `(VT_I4)-2`，
`0x007B44F4` 就是这么写的；传空 variant 会被判 `E_INVALIDARG`）。⇒ 擦除改用 `IWzCanvas::Create`（§5）。

另：`GetCanvas`/`RemoveCanvas` 的包装都是「HRESULT < 0 就 `_com_issue_errorex`」，而 `_com_raise_error`
（`0x00A605C3`）**抛 C++ `_com_error`**，客户端顶层会把它变成弹框（首版就是这么崩的：
`0x80070057 E_INVALIDARG`）。本工程所有 Gr2D 调用都套 `try/catch(...)`，失败退化为"不显示数字"。

### 6.3 `put_origin` 持有 COM 引用 ⇒ 过期图标不消失

`put_origin` 是属性赋值，**会自己 AddRef**（客户端拿到 `GetCenter()` 的向量后 `put_origin` 完立刻 Release
自己那份，即证据）。所以：

- **不要**在传 origin 前手动 `AddRef`（多出来的引用永远没人还）；
- buff 消失时（`ResetTemporary` 只释放 entry、**exe 里没有 `RemoveLayer` 这种 API**）必须把标签层的 origin
  交还给 `IWzGr2D::GetCenter()`，否则标签层一直攥着图标层的引用 ⇒ 图标层析构不掉，**过期图标继续留在屏幕上**，
  而排版已经把它从行里去掉 ⇒ 旧图标与左移过来的图标重叠；要等新 buff 复用该槽位时才消失。
  交还 origin 后再 `Putcolor(0)` 隐藏、擦净画布，把层留给下一个图标复用。

### 6.4 `get_x/get_y` 返回的是**解算后**位置

给标签层换 origin 时，如果把图标层的 `get_x/get_y` 直接抄进另一个 origin 坐标系，会整体错位（实测表现为
数字跑到屏幕外）。证据：标签层 origin = 图标层、偏移 (0,0) 时，两者的 `get_x/get_y` 打印值**完全相同**；
若 `get_x` 是"相对自身 origin 的值"，那一刻标签应当报 (0,0)。⇒ 跟随排版要么走 origin（§5 的做法），
要么在同一 origin 坐标系里用相对量换算，不要混用两种口径。

### 6.5 `GetAlpha` 在新建图层上抛 `E_POINTER`

`GetAlpha`（`0x004143C6`，无参、返回 `_com_ptr_t<IWzVector2D>`）对刚 `CreateLayer` 出来的图层会失败
（`0x80004003`）。客户端自己的淡入是另一条路：`OnEnterField` 对图标层 `GetAlpha` + `raw_RelMove(210,0)`，
条目构造里再用 `Animate(64, 210, 500)`。本插件**不用 alpha**：`Putcolor(0xFFFFFFFF)` 才是让层可见的调用，
alpha 只影响淡入。同理 `Putcolor` 的 alpha 位有效（条目构造用 `0xD30000FF`），所以 `Putcolor(0)` = 隐藏。

## 7. 已知风险与未验证点

1. **HUD 整体隐藏时不跟随**：标签层是独立根图层，只继承坐标不继承图标的 alpha（§6.5）⇒ 客户端隐藏整行 buff
   时（切图/演出）数字可能仍可见。需要的话按 §6.5 的客户端手法给标签层做 alpha 镜像。
2. **`CWvsContext::Update` 的调用频率**假定为每帧（`Tick` 内部按 100ms 自限流，不依赖帧率）。
3. **数字与技能说明文字可能不一致**：本插件忠实显示服务端下发的时长（§3）；服务端的时长修正属服务端登记面。
4. **地址族**：见页首提醒；换 exe 必须复核 §1 全表与 §2 偏移。

## 8. 配置键（`config.ini` 的 `[optional]`）

| 键 | 默认 | 含义 |
|---|---|---|
| `buffTimer` | `true` | 总开关 |
| `buffTimerMaxMinutes` | `10` | 剩余时间 ≥ 该值不显示；`0` = 退回 1 小时 |
| `buffTimerSize` | `14` | 自建字体字号；`-1` = 不用自建字体、退回 `spFontBasic` 槽位 |
| `buffTimerMinuteColor` | `0x5AFFAA` | ≥1 分钟档的 `0xRRGGBB` |
| `buffTimerSecondColor` | `0xFFFF20` | <1 分钟档的 `0xRRGGBB` |
| `buffTimerOutlineColor` | `0x000000` | 8 向描边色 |
| `buffTimerMinuteFont` / `SecondFont` / `OutlineFont` | `7` / `3` / `1` | 自建字体不可用时的表槽位 |
| `buffTimerX` / `buffTimerY` | `4` / `17` | 文字锚点（32×32 图标画布内，左下附近） |

`[debug] debug=true` 时 `buff_timer.log` 一直写（否则只写前若干条：报文 20 条、图标 300 行、绘制 60 行）。
