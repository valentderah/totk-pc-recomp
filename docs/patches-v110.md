# Game 1.1.0 patch coverage

The built-in table is [recomp/patches-v110.txt](../recomp/patches-v110.txt). These sites were located in the local 1.1.0 main and checked against its original instruction words, rather than applying a version-wide address shift. The generator rejects a different word or an invalid spin back-edge.

## Added coverage

| Set | Sites added | Runtime control |
| --- | ---: | --- |
| realtime | 1 | Equipment-switch cooldown: step every positive remainder, including values below FLT_EPSILON. Follows the existing real-time clock switch. |
| cursor | 3 | Scale map panning, vertical ability aim and vertical gyro aim by the 30 Hz ratio; follows `TOTK_HIGHFPS_FIX`. |
| aspect | 4 | The two camera and two viewport projections read page +56. The resolution code enables it for non-16:9 output. |
| fov | 2 | The two camera projections multiply their vertical angle by page +60. The field-of-view setting is live; 100 leaves the original path. |
| pooloff | 2 | Widen texture-pool offsets unsigned, covering 2..4 GiB without changing offsets below 2 GiB. Enabled by default; `TOTK_POOL_OFFSET_PATCH=0` retains the original path. |
| shadow | 1 | The sun shadow map takes its fixed size from page +92 before rounding. The shadow-resolution setting enables it; `shadow_res=game` leaves the game's sizing. |
| cascade | 4 | Read the four sun shadow cascade defaults from page +64..76. Shadow range 100 leaves the original constructor. |

补丁表现有 14 组、53 个指令站点（本轮新增 climb 1 点、gears 2 点）。 Its existing 889 spin back-edges are unchanged. The new sets use the existing runtime page and settings; no new setting, allocator or renderer path is introduced.

The camera sites were cross-checked by normalized instruction-window matching from the local 1.0.0 main. The viewport routine has two matching copies: both are patched. Other sites were checked through their data flow: the equipment timer and its gate, the map clock, the ability clock, the NVN texture storage/package calls, the shadow size publication and the cascade parameter constructor. FOV's s8 is overwritten by the next original instruction; x10 is saved. The cursor sites use the same scratch registers as their 1.2.1 counterparts.

## Deliberately separate

- Direct mouse: 1.1.0 continues to use stick emulation. Porting the complete camera hook set also needs the runtime layout and load-site table; matching an individual store or one camera mode is not sufficient. No partial set is enabled here.
- Fade: the four 1.4.3 sites are not presumed to exist in 1.1.0. An equivalent zero-increment path has not been established.
- Native Zstandard: the existing host bridge calls back to hard-coded 1.4.3 decoder entry points on fallback. Adding only the BL patches to 1.1.0 would be incorrect.
- Live DLSS scene sizing and game-menu mouse input retain their existing version restrictions. They are not patch-table parity work.

## Validation

Run with Python's numpy, lz4, capstone and unicorn dependencies and a runtime-supported Clang:

```sh
# CC selects the Clang used by the differential harness.
python tests/patches_v110_check.py
python tests/patches_v110_check.py --cflags=-march=x86-64-v2
python tests/patches_v110_check.py --exefs <local ExeFS directory>
```

Without ExeFS, the test uses synthetic memory and the patch table only. It compares the complete integer/SIMD register state, SP and NZCV of the lifted C and Unicorn for all 17 additions, on and off, at 30/60/120 Hz, including pool offsets immediately below and above 2 GiB and at the end of the 32-bit range. Windows test mappings respect its 64 KiB allocation granularity. With ExeFS it additionally validates every original word and spin target. Extracted images and generated C are local build artifacts, never repository content.

These checks establish instruction and code-generation contracts. World-scene visual acceptance, texture packs allocating a pool above 2 GiB, physical gyro and comparisons of shadow quality still require gameplay checks.

## 0.4.0 后续：攀爬与齿轮

以固定上游 `c89b0f3ee3b131275c5c5c26426128f16469f2cd` 为基准，对比 `3d386fe` 后的最终补丁和运行时 gating；目标是 **1.1.0 / v131072**，不是 1.1.1。

| 上游项目 | 1.1.0 审计结果 | 本轮处理 |
| --- | --- | --- |
| climb | 本地 main+0x1927948 有同构速度补偿和相同的 r=s9、s8=0.5 钳位；常量计算顺序不同 | 移植至 +0x19279f0，沿用现有 TOTK_CLIMB_FIX 与 realtime 依赖 |
| gears | main+0x213eec4 用 RotateStartDeg/30 检测转动，忽略更新上下文增量 | 两点补齐，沿用 TOTK_HIGHFPS_FIX；不依赖 realtime |
| fixres / 大尺寸 scene | 原有 output/scene 站点仍在；固定动态分辨率出口 +0xce9280 有候选 | 不移植；尚未验收层级、render scale、HeapDef 和实际渲染结果 |
| grass | 构造器 +0x100e838、范围常量 +0x100e890 有候选 | 不移植；六点完整定位、纹理/堆和世界场景效果仍需专门验收 |
| optref / ability / report | 没有证明 v110 存在同一崩溃根因；report 本来就不是所有版本都有同构循环 | 不移植，不用守卫掩盖未定位的问题 |
| sync_ahead | [nvn_api_core.c](../runtime/nvn_api_core.c) 的 frame_wait_lr 只列 121/143，且要求 shadow storage | 保持关闭版本 gating；未验证 GPU/readback/fence 行为 |

### 原始代码与补偿语义

攀爬补偿来自本仓库固定基准的 [1.2.1 climb 定义](../recomp/patches-v121.txt)。本地 1.1.0 原因子先乘 -0.181、再 fadd 加倍，最后加 1.272；代数上是 1.272 - 0.362(r-1)，但不能用合并乘法替换其浮点舍入顺序。本补丁只扩展最后的钳位，保留前面的实际 v110 指令。

正常 30 Hz、以及 r>=1 的慢帧，原因子的低32位不变；60 Hz 是有意修正的范围，正常 r=0.5 时补偿约为 1.8665，而非原来的约 1.453。高于60 Hz 的曲线采用上游 g(u)，u=max(page+0,r)，慢动作不能单靠缩小时间比例触发更高帧率的 g 分支。它读取既有 page+0，不增加页字段。

climb 使用96字节、16字节对齐的临时栈，完整保存 x8 及 q5/q6/q7/q16/q17；仅 s0/s1 是声明输出，标量写入会清掉对应向量高位。这样不依赖 1.2.1 的死寄存器假设。关闭 TOTK_CLIMB_FIX 时只执行原始 fmov；现有 [frame_step_init](../runtime/frame_rate.c) 会在 realtime=0 时关闭整组。

齿轮函数由虚表0x347e8e8的+0x110进入。AI调度器+0x1205b90、+0x2ae29ac把上下文+8复制至传入结构的+8；AI根更新+0x1085c88调用 owner 的虚表+0x30，并把返回的帧增量发布到该结构。首站+0x213eee4位于原有 str d12 入栈之后，后续到阈值站点前没有本函数对 s12/d12/q12 的写入。跨调用只依赖 AAPCS64 保证的 d12 低64位：所需增量在低32位，不依赖上半128位跨调用保持。第二站重新产生完整标量 s12，原有函数尾恢复 d12；q12 的高半本来不属于 callee-saved 契约。

阈值变为 (RotateStartDeg/30)*ctx.delta，三处原始 fcmp/b.le（+0x213f10c、+0x213f174、+0x213f1b0）共同读取它。第二站额外保存完整 q4，SP恢复后才继续原函数。gears 不读取 patch page，realtime=0 时仍生效，TOTK_HIGHFPS_FIX=0 恢复两个原始站点。

### 匹配唯一性与原指令守卫

在本机1.1.0 main中，下列原始窗口各仅命中一次；这些是地址和哈希，不是游戏字节转储：

| 窗口 | 长度 | SHA-256 |
| --- | ---: | --- |
| +0x19279b0 | 108字节 | 8262a11c180c734f2692a74e9563140f26d743f0f1f95d31732f975b46c27331 |
| +0x213eee4 | 32字节 | 17487c0fe195655e9a1bcaf49835208bd1ebca86eeef9a2ca0059752f503e378 |
| +0x213f0d0 | 28字节 | d07279a2abea53cffbd8861e95034cd1a5c85edf90f6094c5261ba55da203577 |

生成器继续逐点检查原始指令；可选本地测试还核验原始常量、d12保存、虚表入口、三处比较及 b.le 目标。未使用全版本固定偏移。

### 新增验证

[高帧补丁测试](../tests/patches_v110_highfps_check.py)复用公共 CC 选择、平台后缀和映射实现，路径使用 pathlib；[补丁解析器](../recomp/patches.py)显式按 UTF-8 读取中文注释，不依赖 Windows 默认代码页。

```sh
env CC=clang python tests/patches_v110_highfps_check.py
python tests/patches_v110_highfps_check.py --cflags=-march=x86-64-v2
python tests/patches_v110_highfps_check.py --exefs <本地ExeFS> --work-dir <本地产物目录>
```

- Windows v3/v2 × 合成/本地四组各3240例 native/Unicorn通过，含开/关、0/极小增量、0.5两侧相邻ULP、30/60/120/240Hz、慢帧及16种NZCV。
- 全部31个GPR、32个128位SIMD、SP、NZCV及整块scratch memory CRC对照；所有非声明输出必须与输入逐位相同。q4/q8/q9高64位非零哨兵是显式前置条件。
- 精确检查保存槽内容、16字节栈对齐、SP恢复、槽间空隙、前后canary和所有其他scratch内存不变。没有排除整块内存或放宽SIMD allowlist。
- 直接编译真实 [patches.c](../runtime/patches.c)、frame_step_init、step_store 的9种默认/显式配置组合通过；旧 [#1002 回归](../tests/patches_v110_check.py) 保持原 SIMD allowlist 修复，四组各408例通过。
- 五种内存中负向变异全部被拒绝：破坏x8、破坏q17、保存区外写入、q4只恢复d4、q4只恢复s4。实际补丁文件未施加变异。组合测试还主动破坏q12的高64位，确认跨调用仅依赖d12低位。
- WSL Ubuntu22.04原生9种gating编译执行通过。完整SIMD native replay在现有Clang14编译公共[simd_fp.h](../runtime/simd_fp.h)时因缺少elementwise内建而失败；**没有Linux 3240例通过的结论**。已在本地保留待新工具链回放的合成输入/输出，不提交。Linux的Python差分依赖也未安装，不修改发行版环境。

以上证明指令、数值和配置契约，不等于真实攀爬、神庙齿轮、slow motion、极端卡顿或GPU场景验收。此分支没有启动或部署游戏；完整构建、实际安装更新和游戏验收由维护者另行完成。

### 重新生成要求

必须重新lift main+0x1927948和main+0x213eec4所在生成分块，并由生成器重写patch元数据；推荐正常完整generation，不手改生成C。RT_PATCH_COUNT从12变14，原12组索引保持不变，末尾加入climb/gears。patch page地址和布局不变，climb复用+0，gears无页字段。旧generated/对象的lift哈希不可直接复用；runtime库也需要读取新生成的patch宏重编。
