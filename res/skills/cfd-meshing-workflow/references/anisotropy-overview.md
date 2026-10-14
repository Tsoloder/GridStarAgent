# 各向异性处理总览（机翼 / 吊舱 / 尾翼 / 其余面）

> 本文件是"表面网格生成之后、体网格块创建之前"这一组各向异性子阶段的**唯一清单**。
> 路由表、流程 B、硬闸门、`cad-to-mesh.md` 只指向本文件，**不再各自重复列出四类**。
> 各向异性处理不是一件事，而是下面四件；不得只执行其中一部分就当作"各向异性处理"完成。

## 一、四类子阶段

| 子阶段 | 入口工具 | 前置分组（存在才做） | 详细资料 |
|---|---|---|---|
| 机翼各向异性（含后缘面分类 / 分布 / 装配） | `WingAnisoProcessWingAnisotropy` | `wingUpper` / `wingLower` / `wingTip` / `wingTrailingEdge` | Skill `wing-anisotropy-processing`；尺寸计算另见 `references/anisotropic-mesh.md` |
| 吊舱各向异性 | `NacelleAnisoProcessNacelleAnisotropy` | `engineInner` / `engineOuter` / `enginePylonInner` / `enginePylonOuter` / `enginePylon`（无 `enginePylon` 时用 `enginePylonDefault`） / `engineTrailingEdge` / `enginePylonTrailingEdge` | Skill `nacelle-anisotropy-processing` |
| 尾翼各向异性 | `TailAnisoProcessTailAnisotropy` | `verticalTailRight` / `verticalTailLeft` / `verticalTailTip` / `verticalTailTrailingEdge` / `horizontalTailUpper` / `horizontalTailLower` / `horizontalTailTip` / `horizontalTailTrailingEdge` | Skill `tail-anisotropy-processing` |
| 其余面各向异性 | `RemainingAnisoProcessRemainingAnisotropy` | 前三类完成后执行；覆盖机身 / 机尾等未被前三类覆盖的面 | Skill `remaining-anisotropy-processing` |

## 二、执行规则

1. **顺序固定**：机翼 → 吊舱 → 尾翼 → 其余面。
2. **每类"前置分组存在即必做"**：先查分组是否存在（如 `GetSpliteAssemlyDomainsBatch` / `GetAllSpitAssemblyGroupProperty`），存在就调用该类入口工具。**只有查询确认缺失（结果为空）的那一类**才标 `skipped` 并在阶段 `note` 中说明原因——不得因为"用户没提""没列进计划""之前没做"而跳过存在子组的类别；**未确认缺失的类别一律视为必做**。
3. **每类成败只看入口返回的顶层 `status`**：为 `"success"` 即该类完成；`steps` 子步骤字段（如 `aniso_domains`）出现 `failed` 不算失败（见基础提示词第 2.4 节第 8 条）。失败时读 `message`，并按该类独立 Skill 的说明处理。
4. **计划里把这四类列为独立阶段**（如 `aniso-wing` / `aniso-nacelle` / `aniso-tail` / `aniso-remaining`），**不得用一个笼统的"各向异性处理"阶段代替**——否则容易只做机翼一类就误判为完成。
5. 四类全部结束后，"各向异性处理"才算完成，之后才可进入体网格块创建。

## 三、与后缘面处理的关系

机翼各向异性一体化入口已包含后缘面处理（step2b 分类 / step4b 分布 / step8b 删除原面并装配）。仅当该入口**顶层 `status` 为 `"failed"` 且 `message` 指向后缘面失败时**才回退，回退步骤见 `wing-anisotropy-processing` 的 `references/type1.md` / `references/type2.md`。

## 四、前置条件（四类共通）

1. 数模已导入，自动部件分割已完成（各类前置分组见上表）。
2. 表面网格已生成。
3. 四类按上表顺序执行，前一类完成后才进入后一类。