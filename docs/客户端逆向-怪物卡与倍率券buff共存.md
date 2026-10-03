# 客户端逆向：怪物卡与倍率券 buff 共存

> 姊妹文档：IDA 书签（前缀 `BUG031:`）与本文件成对；改址或改述时两侧同步。
> 基线：`Angel.exe`（v83，IDA `D:\Downloads\Angel\Angel.i64`）；发布客户端为 `BeiDou.exe`（本文件涉及的地址两侧一致）。
> 关联实现：`ezorsia/CardDefenseAttr.h` / `CardDefenseAttr.cpp`、`ezorsia/CardDefenseTable.h`（生成物）、
> `config.ini` 的 `cardBuffCoexist`。
> 服务端侧权威文档：Ursa-Server `docs/client/004-怪物卡属性耐性与buff共存.md`、`docs/bugs/029`、`docs/bugs/031`。

## 0. 一句话

怪物卡的防御系状态（`DEFENSE_ATT` / `DEFENSE_STATE` / `RESPECT_*` / `ITEM_UP_BY_ITEM`）与倍率券
（`COUPON_DRP1-3` / `COUPON_EXP1-4`）**共用同一个 buff 掩码位**，而客户端 `CTemporaryStatView` 对每个位只保留
一个表项（后到的包把先到的表项按位清掉、清空即移除）⇒ 卡与券会互相顶掉图标。本插件在
`OnTemporaryStatSet` / `OnTemporaryStatReset` 处理期间，给**不属于本次要发的那一条**表项注入一个
**没有 stat 使用的掩码位（bit 0）**，让位清空判定不成立；处理完写回原掩码。全程**不写客户端的列表**。

## 1. 机制

- **位共用**：`DEFENSE_ATT` = `0x800000` = `COUPON_DRP1`；`DEFENSE_STATE` = `0x1000000` = `COUPON_DRP2/3`；
  `RESPECT_PIMMUNE` = `0x200000` = `COUPON_EXP2`；`RESPECT_MIMMUNE` = `0x400000` = `COUPON_EXP3/4`；
  `ITEM_UP_BY_ITEM` = `0x100000` = `COUPON_EXP1`（对应服务端 `BuffStat` 常量）。
- **客户端自己的规则**：`CTemporaryStatView::SetTemporary` 对每个非 nType 3/4 表项做
  `entry->mask &= ~newMask`（`UINT128::operator&=`），随后"掩码非零？"判定为假就移除该表项。取消路径
  `ResetTemporary` 用同一套规则（`& ~resetMask` + 取空移除）。
- **惰性位**：两个掩码字里最小的在用值是 `MORPH = 0x2`，**bit 0 没有任何 stat 使用** ⇒ 把 bit 0 OR 进要保住的
  表项，`& ~newMask` 之后仍有一位，移除判定不成立；处理完把原掩码写回，不改变任何数值语义。
- **判据来源**：`OnTemporaryStatSet` 的包按客户端自己的形状解（矩阵 16 字节 + 每置位 `value/id/duration`），
  **只读**取 buff id ⇒ "本次要发的 id"与表项 id 相同的表项**不保护**（交回客户端原生替换，避免同 buff 两份图标），
  只保护 id 不同的表项（卡与券互保）。

## 2. 关键函数与地址

| 地址 | 符号 | 说明 |
|---|---|---|
| `0x00A202BE` | `CWvsContext::OnTemporaryStatSet(CInPacket&)` | GIVE_BUFF 入口（本模块的读 id 点） |
| `0x00A2071F` | `CWvsContext::OnTemporaryStatReset(CInPacket&)` | CANCEL_BUFF 入口；**只带掩码、不带 id** |
| `0x007B24D5` | `CTemporaryStatView::SetTemporary` | 位清空 + 取空移除的判定点 |
| `0x007B2717` | `CTemporaryStatView::ResetTemporary` | 取消路径的同一套判定 |
| `0x007B4D1D` | `ZList::FindIndex` | 列表遍历：返回**节点**，表项 = `*(node+4)` |
| `0x00781D0E` | GIVE_BUFF 解码体（唯一调用方 = `OnTemporaryStatSet`） | `DecodeBuffer(mask,16)` → 逐置位 `Decode2`(value) + `Decode4`(buffid) + `Decode4`(duration) |
| `0x0042470C` | `CInPacket::Decode2` | 报文偏移基准：`+0x08` 缓冲基址、`+0x14` 读游标 |
| `0x007B4BD1` | 表项删除（客户端自用） | **本插件禁止调用**（见 §5） |
| `0x007B2BB0` | `AdjustPosition` | **本插件禁止调用**（见 §5） |

## 3. 表项布局（`sub_7B3176` 构造）

`+0x00` vtbl、`+0x0C` 掩码（UINT128）、`+0x1C` nType、`+0x20` nId（本构建里物品 buff 存**正数** item id）、
`+0x28`/`+0x2C` 两层、`+0x38` 剩余毫秒。视图在 `CWvsContext + 0x2EA8`，列表在视图 `+0x04`，表项数在 `+0x0C`。

## 4. 实现（本插件当前行为）

1. `OnTemporaryStatSet`：只读取本次包的 buff id（读完把 `CInPacket+0x14` 写回原值；不调用客户端解码器）→
   对每个"卡/券"表项：id 命中本次集合 ⇒ 跳过；否则 OR 进 bit 0 并记快照 → 调用原函数 → 写回原掩码。
2. `OnTemporaryStatReset`：取消包没有 id ⇒ 只保护**与券同位的卡表项**（券不保护：券表项携带约 24 h 倒计时，
   误保会留下永久幽灵图标）；保住的项在取消返回后写回原掩码。
3. 读包失败（id 数 0）⇒ 回落到"保护全部"，只会出现副本，不会碰列表。
4. **不写日志**：模块静默工作（诊断期曾写 `carddefense.log`，验收后已移除）。

## 5. 坑

- **禁止调用客户端的列表写操作**（`sub_7B4BD1` 删除 / `sub_7B2BB0` 重排）：客户端自用时是临界区内的复合动作
  （先对表项取引用再摘链），外部只复刻"摘链"会把链表写坏成 `-1` 形态，**当场不报错、约 2 秒后客户端 AV 崩溃**
  （实测 dump：`0xC0000005` 读 `0xFFFFFFFF`）。需要"清理"时改为**不产生**副本（本模块 §4 第 1 条）。
- **本地倒计时不摘表项**：把表项剩余毫秒置 1 不会让它消失（实测该表项长期滞留并转负），摘表项只发生在服务端
  CANCEL 或整表清空 ⇒ 不能指望倒计时自愈。
- **不要照抄别处的 `CInPacket` 偏移**：`+0x08` 基址 / `+0x14` 游标以客户端自身实现（`Decode2`）为准；
  另有模块把 `Data` 写在 `+0x04`（那里是 flag）导致其接收路径从未生效。
- **保护券要谨慎**：券的倒计时约 24 h，误保一条就是永久幽灵图标 ⇒ 只保卡、且只在"与券同位"时保。

## 6. 实机验证

| 步骤 | 判据 |
|---|---|
| 持券 → 用卡 → 看图标 | 两个图标同屏；`cardBuffCoexist=false` 时复现"一个顶掉另一个" |
| 同一张卡再施加一次 | 只留一个图标（同 id 旧表项由客户端原生替换） |
| 连续换线 2~3 次 / 进出商城 | 每个 buff 一份图标、**不崩溃** |

实测记录（2026-10-03，带诊断日志的验收轮）：21 分钟会话、多次换线 + 进出商城（含买券），每次 `GIVE_BUFF`
的 id 均解析成功（`incoming=1..2`）、保护数 `0..3`、**无重复表项报告**、无 crash dump ⇒ 上述三条成立。
