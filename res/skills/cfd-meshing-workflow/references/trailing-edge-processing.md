# 飞行器后缘面处理

用户提及"后缘面处理"、"后缘网格"、"trailing edge"时进入此流程。

**该流程已并入一体化 Skill `wing-anisotropy-processing`。当前技能包中不存在 `trailing-edge-processing` 这个 Skill，调用 `read_skill_resource(skill_id="trailing-edge-processing", ...)` 会直接报 `unknown skill`。**

后缘面处理已在 `WingAnisoProcessWingAnisotropy` 一体化入口内部完成（step2b 分类、step4b 分布设置、step8b 删除原面并装配），正常情况下不需要单独执行。

仅当该入口返回的**顶层 `status` 为 `"failed"` 且 `message` 指向后缘面处理失败时**，才读取 `wing-anisotropy-processing` 的类型步骤资料做回退（`steps` 内子步骤字段（如 `aniso_domains`）的 `failed` 不代表整体失败，不作判据）：

```
read_skill_resource(skill_id="wing-anisotropy-processing", relative_path="references/type1.md")
read_skill_resource(skill_id="wing-anisotropy-processing", relative_path="references/type2.md")
```

类型一（翼梢相邻）与类型二（吊舱-机身相邻）步骤完全不同，先处理全部类型一，再处理全部类型二，严禁混淆。不要在本 Skill 目录下找这两个文件。