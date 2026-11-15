# 11 Ultra（star / 11u）低电压内核 · GitHub 编译工程

以 [`ikun-1145141/android_kernel_xiaomi_sm8350_star`](https://github.com/ikun-1145141/android_kernel_xiaomi_sm8350_star) 的 `venus-5.10bpf` 分支为基础，在 **GitHub Actions** 上编译小米 11 Ultra 可用的内核，实现：

| 需求 | 实现 |
| --- | --- |
| 关机电压改成 **2.98 V** | 强制覆盖 vendor dtb 里的 `qcom,shutdown-voltage`（原厂 3400 mV），紧急关机判断改为 2980 mV；`qpnp-qg.c` 的截止/空电默认值同步改掉 |
| 原 3.4 V 及以下不再自动关机 | 3.0 V～3.4 V 区间内上报的电量按新表给出 **> 0%**，Android `BatteryService` 不会因 `capacity == 0` 关机；kernel 的 `kernel_power_off()` 也只在 ≤ 2980 mV 才触发 |
| 可重构「电压 → 手机电量」对应表 | 新增 `star_volt_soc` 驱动：编译期可换表、运行期可通过 sysfs 直接重写整张表（线性插值） |

> ⚠️ **务必先读「安全须知」**：2.98 V 低于常规锂离子电芯的放电下限。过放会造成不可逆容量损失、鼓包、内短路，回充时存在热失控风险；同时 bootloader / PMIC / ADSP 里的硬件保护阈值**不是内核能改的**，仍可能提前断电。本项目仅供有备份、能承担风险的折腾使用。

---

## 一、为什么这么改

分析上游源码后确认的链路（不是猜的）：

1. `star_dsl_defconfig` 里是 `CONFIG_QTI_BATTERY_CHARGER=y`（`drivers/power/supply/qti_battery_charger.c`），电量计是跑在 ADSP 上的 BQ gauge（`CONFIG_BQ_FUEL_GAUGE` / `BQ_FG_1S`），**OCV 表在固件和 vendor dtb 里，内核树改不到**。
2. `battery_chg_parse_dt()` 从 `qcom,shutdown-voltage` 读关机电压；本仓库的 `arch/arm64/boot/dts/` 只有上游通用 DTS，vendor dtb 在**原厂 boot.img** 里，AnyKernel3 刷入时保留原 dtb，所以只能在 C 代码里覆盖。
3. `battery_check_work()` 的逻辑是「SOC == 0 且 `voltage_now <= shutdown_volt_mv` → `kernel_power_off()`」，原厂 3.4 V 就关机正是这里。
4. 只改这一个阈值还不够：Android 的 `BatteryService` 见到 `capacity == 0` 也会关机。所以还要让 3.0～3.4 V 区间上报非零电量 —— 这就是新加的「电压→电量表」。

补丁落点：

```
drivers/power/supply/Kconfig                    新增 CONFIG_STAR_VOLT_SOC
drivers/power/supply/Makefile                   编译 star_volt_soc.o
drivers/power/supply/star_volt_soc.[ch]         新增：可重写查表 + 统一关机阈值
drivers/power/supply/qti_battery_charger.c      star 实际使用的驱动：CAPACITY 重映射、覆盖 DT 关机电压、关机判断
drivers/power/supply/qti_battery_charger_venus.c 同步（切换到 VENUS 配置时也生效）
drivers/power/supply/qcom/qpnp-qg.c             QG 默认截止/空电/低电阈值（其他 defconfig 用）
build_lowvolt.sh                                自带构建脚本（基于上游 build_star.sh）
```

---

## 二、快速开始（推到 GitHub → 一键编译）

本目录是一个**独立的构建仓库**，不包含内核源码；Actions 运行时才去拉上游内核并打补丁。

### 方式 A：脚本一键发布并触发编译（推荐）

`tools/gh_build.py` 全程只走 `api.github.com`，**不需要 `git push`，也不需要访问 github.com 主站**，适合本地代理只放行 API 的网络。先建一个 [classic PAT](https://github.com/settings/tokens/new?scopes=repo,workflow&description=star-11u-lowvolt-build)，勾选 `repo` 和 `workflow` 两个 scope：

```bash
export GH_TOKEN=ghp_xxxxxxxxxxxxxxxxxxxx    # 不要把 token 写进任何文件
python tools/gh_build.py                    # 建公开仓库 → 推送全部文件 → 触发构建 → 打印 run 链接
python tools/gh_build.py --follow           # 额外等待编译结束并报告结论/失败步骤
python tools/gh_build.py --status           # 随时查询最新一次运行状态

# 可选：私有仓库、自定义仓库名、改参数
python tools/gh_build.py --private --repo my-11u-kernel --shutdown-mv 2980 --llvm-ias 0
```

脚本用 Git Data API 建 blob/tree/commit/ref，把 `main` 设为默认分支，再调用 `workflow_dispatch`；token 仅作为内存中的 Authorization 头发送，不会落盘，也不会写进 `.git/config`。

### 方式 B：手工推送

```bash
# 在本目录执行
git init -b main
git add .
git commit -m "star 11u low-voltage kernel build"
git remote add origin https://github.com/<你的用户名>/<你的仓库名>.git
git push -u origin main
```

然后打开仓库 → **Actions** → `Build 11u low-voltage kernel` → **Run workflow**，可以直接用默认值（2980 mV + 默认电压表），也可以填参数：

| 输入 | 默认 | 说明 |
| --- | --- | --- |
| `upstream_ref` | `venus-5.10bpf` | 建议固定成 commit SHA（例如 `16494b7b74264898d3af6845c997cfac7957d19a`），避免上游更新导致补丁失配 |
| `shutdown_mv` | `2980` | 关机电压，范围 2500–3400 |
| `volt_soc_table` | 空 | 留空用 `config/volt_soc_table.default.txt` |
| `clang_url` | 空 | 默认用 Ubuntu 22.04 的 clang-14 + lld-14；若内核报错，可填 AOSP clang-r416183b 等预编译工具链 tarball |
| `llvm_ias` | `1` | 老工具链汇编报错时改 `0`（自动加 `CROSS_COMPILE=aarch64-linux-gnu-`） |
| `jobs` | `4` | 并行编译数 |

产物在 Artifacts（也可能是 tag 触发的 Release）：

- `Image.gz` —— 裸内核镜像
- `*.zip` —— AnyKernel3 可刷包（`do.devicecheck=1`，校验 `star` / `mars`）
- `LOWVOLT_BUILD_INFO.txt` —— 本次编译写入的关机电压和电压表
- `UPSTREAM_COMMIT.txt` —— 实际使用的上游 commit

**刷入**：先 `fastboot boot` 一个用原厂 boot.img 换掉 kernel 重打包的镜像验证；确认能开机后再在 TWRP/OrangeFox 里 `adb sideload` 那个 zip。务必保留原版 `boot.img` 以便回退。

---

## 三、电压 → 电量表

格式：`mV:百分比` 用逗号分隔，**电压必须严格递增、百分比必须非递减**。

默认表（`config/volt_soc_table.default.txt`）：

```
2980:0,3050:15,3150:20,3250:23,3350:26,3400:29,3500:33,3600:40,3700:50,3800:62,3900:74,4000:85,4100:94,4200:100,4350:100
```

即：**2.98 V = 0%（关机点），3.40 V = 29%，4.20 V = 100%**，点之间做线性插值。低压段被刻意抬高（3.4 V 从旧的 10% 抬到 29%），避开 HyperOS / MIUI 在 10% 左右触发的「电量低，N 秒后关机」倒计时。低于 2980 mV 直接报 0% 并交给内核关机；高于 2980 mV 时至少报 `min_soc`（默认 5%），避免 Android 在 0% 时自己关机。

### 三种「重构」方式

1. **改默认表 → 重新编译**：编辑 `config/volt_soc_table.default.txt`，或在 Actions 里填 `volt_soc_table`。
2. **运行期直接重写（不用重新编译）**：

   ```bash
   adb shell su -c 'cat /sys/module/star_volt_soc/parameters/table'
   adb shell su -c 'echo "2980:0,3200:4,3400:15,3600:30,3800:55,4000:82,4200:100" > /sys/module/star_volt_soc/parameters/table'
   ```

3. **临时关掉重映射**：`echo 0 > /sys/module/star_volt_soc/parameters/enable`（只回到原始电量计曲线，关机电压仍是 2.98 V）。

### sysfs 参数一览

| 路径（`/sys/module/star_volt_soc/parameters/`） | 默认 | 作用 |
| --- | --- | --- |
| `table` | 见上 | 整张电压-电量表，写入立即生效 |
| `shutdown_mv` | `2980` | 紧急关机电压，**立即生效**（关机检查每次调用都读它） |
| `volt_ema_alpha` | `5` | 电压 EMA 系数（%）—— 越小越平滑、越抗负载跌落；值越小，3.3 V 带载时越不容易被"误判"到 2.98 V |
| `soc_ema_alpha` | `50` | 电量百分比的二次 EMA 系数（%）—— 越小越平滑但跟随越慢。默认 `50`（较快跟随）：电压 EMA 已负责主要平滑，二次 EMA 若设太小（如 `4`）会导致电量"爬不到位就卡住"（实测停在 44%，真实应为 39%），因为 Android 在电量看起来稳定后就不再读取 |
| `debounce_ms` | `20000` | 滤波后电压需**持续低于**关机阈值这么久才真正关机（毫秒）。这是防止"负载瞬间跌落→瞬时电压 < 2.98 V→误关机"的关键 |
| `hard_cutoff_mv` | `2900` | 原始电压 ≤ 此值视为真没电，**立即**关机，不做防抖（安全底线，低于 `shutdown_mv`） |
| `enable` | `1` | 是否启用电压→电量重映射 |
| `apply_below_mv` | `0` | 只在此电压以下重映射（如填 `3600` 只改低压区，高压区沿用原电量计），`0` = 全区间 |
| `min_soc` | `5` | 电压高于关机点时允许显示的最小百分比 |
| `report_floor_mv` | `0`（默认关闭） | 放电时上报给 Android 的电压下限（mV，`0`=关闭）。**默认关闭**：把电压钳成常数会让 Android 认为电压"不再变化"，从而停止发送电量广播，状态栏电量会冻结不动（实测卡住 2 小时）。低电量关机倒计时实际由**电量百分比**触发，抬高低压段电量表（3.4V→29%）已足够规避，无需钳位电压。仅在你确认安全中心确实按电压触发时才手动开启（如 `3600`） |
| `debug` | `0` | 打开限速调试日志 |
| `status` | 只读 | 查看 `raw_soc / volt_uv / v_filt_mv / mapped_soc / shutdown_mv / debounce_ms / …` |

充电中（`CHARGING` / `FULL`）不套用电压表，直接用原电量计数值，因为充电时电压被拉高、纯电压曲线会跳变。

> **为什么之前 3.3 V 还会关机 / 放电会跳电？**
> 关机判定原来直接读**瞬时电压**：电池在 3.3 V 静置时，一旦带负载，端电压 = 开路电压 − 电流×内阻，会瞬间跌到 2.98 V 以下，于是立刻 `kernel_power_off()`。也就是说它"以为"到了 2.98 V，其实 3.4 V 没关、3.3 V 关，正好对应负载跌落幅度。放电"跳电/跳百分比"也是同一原因：瞬时电压随负载上下跳，百分比跟着跳。
>
> 本版的做法：关机判定改用 **EMA 滤波后的电压**，并且要求它**持续低于阈值 `debounce_ms` 才关机**；电量百分比同样基于滤波电压并再做一次 EMA 平滑。这样 3.3 V 带载的瞬时跌落不会再触发关机，百分比也不会乱跳。

### 现场调参（刷一次后不用重编即可试）

```bash
# 1) 看当前状态（滤波电压、映射百分比、各参数）
adb shell su -c 'cat /sys/module/star_volt_soc/parameters/status'

# 2) 如果 3.3 V 仍被关机：把电压滤波调更"迟钝"、防抖更长
adb shell su -c 'echo 3  > /sys/module/star_volt_soc/parameters/volt_ema_alpha'   # 更平滑
adb shell su -c 'echo 30000 > /sys/module/star_volt_soc/parameters/debounce_ms'   # 持续 30s 才关

# 3) 如果百分比仍跳：把 SOC 滤波调更平滑
adb shell su -c 'echo 2 > /sys/module/star_volt_soc/parameters/soc_ema_alpha'

# 4) 想真把关机点再压低一点（如 2.95 V）：
adb shell su -c 'echo 2950 > /sys/module/star_volt_soc/parameters/shutdown_mv'

# 5) 想临时恢复原厂曲线（关机电压仍是 2.98 V）：
adb shell su -c 'echo 0 > /sys/module/star_volt_soc/parameters/enable'
```

> 调参只是运行时生效，重启后回到编译期默认值。想固化，改 `config/volt_soc_table.default.txt` 或直接重跑 Actions（Workflow 有对应输入项）。

---

## 四、安全须知（请认真看）

- **2.98 V 低于绝大多数锂电芯的 3.0 V 放电下限**。深度放电会溶解铜集流体、造成永久容量损失与内短路，再次充电存在发热/鼓包/热失控风险。老化或低温电池内阻更大，电压会突然塌到截止点，可能直接黑屏断电、丢失数据。
- **硬件保护改不了**：bootloader、PMIC、电量计固件/BMS 里可能还有独立的欠压保护，仍可能在 3.0 V 以上切断供电；内核补丁只覆盖本仓库能控制的这条软件关机路径。
- 不要用这个内核跑重要数据、无人值守设备或长期插电/长期亏电；电量到低百分比后尽快充电。
- 首次务必 `fastboot boot` 验证、保留原厂 `boot.img`，并确认已解锁 BL、有可用 recovery。
- 本项目是第三方实验性内核，无质保；刷机变砖、电池损坏、数据丢失由使用者自行承担。

---

## 五、本地/排错说明

- 补丁器 `patches/apply_lowvolt.py` 使用**精确字符串匹配**，任何一处上游改动导致匹配数不为 1 都会直接报错并打印查找内容，不会静默产出错误内核。
- 想本地编译（Debian/Ubuntu）：先装依赖与 clang，然后

  ```bash
  git clone --depth 1 -b venus-5.10bpf https://github.com/ikun-1145141/android_kernel_xiaomi_sm8350_star.git kernel
  git -C kernel submodule update --init --recursive --force
  python3 patches/apply_lowvolt.py kernel
  cd kernel && JOBS=$(nproc) ./build_lowvolt.sh && bash anykernel3/build.sh
  ```

- 若 clang-14 + `LLVM_IAS=1` 在 arm64 汇编上报错：先把 `llvm_ias` 改成 `0`（脚本自动带 `CROSS_COMPILE=aarch64-linux-gnu-`）；还不行就换上游 `build.config.common` 指定的 **AOSP clang-r416183b**，把 tarball 地址填进 `clang_url`（tarball 里必须含 `*/bin/clang`：

  ```
  https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-r416183b.tar.gz
  # 若 404，把 main 换成 master；也可用 LineageOS 镜像：
  # https://github.com/LineageOS/android_prebuilts_clang_kernel_linux-x86_clang-r416183b/archive/refs/heads/lineage-20.0.tar.gz
  ```

  工作流会自动解压、找到 `bin/clang` 并加入 `PATH`。
- 编译日志里出现 `CONFIG_STAR_VOLT_SOC was dropped by olddefconfig`：说明 Kconfig 依赖 `QTI_PMIC_GLINK` 没打开，或补丁没命中（补丁器会先报错）。
- 若 `CONFIG_STAR_VOLT_SOC` 被 `olddefconfig` 丢掉，脚本会直接报错；检查 `CONFIG_QTI_PMIC_GLINK=y` 是否在 `.config` 里。
- `qti_battery_charger_odin.c` 未改（那是别的机型驱动）；如果你切到 `CONFIG_QTI_BATTERY_CHARGER_ODIN`，需要照同样思路补一次。
