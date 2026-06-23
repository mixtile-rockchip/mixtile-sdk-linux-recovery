# rockchip_recovery

Rockchip recovery + update engine for OpenHarmony small systems (rk3506 等).

## 构成

两个可执行文件：
- `recovery` — BCB/cmdline 解析、挂载、菜单，按需调起 `updateEngine`
- `updateEngine` — 升级流程执行（解析 `sdupdate.img` / OTA 包，写入存储）

## 目录结构

```
core/         业务层：RK 镜像、BCB + A/B slot、校验（CRC/MD5/RC4）、bsdiff、flash 调度
storage/      存储抽象 + 实现：eMMC / MTD+UBI / UFS 后端，GPT、fstab、mount、format
app/recovery/ recovery 入口：recovery_main + recovery_cmd（BCB / 菜单 / 拉起 updateEngine）
app/engine/   updateEngine 入口：engine_main + cmd_upgrade / cmd_misc / cmd_raw
app/common/   两者共享：source（SD/USB 检测）+ source_ota（OTA 源）
ui/           UI + minui（DRM → fbdev 回退链）+ 资源
utils/        基础库：log / device / process (run_cmd) / mtd_info / mtdutils (AOSP)
include/recovery/  公共头文件（扁平结构）
```

## 编译裁剪

BUILD.gn `declare_args()` 暴露下述开关（默认全开，除 `rk_source_url`、`rk_ui_enable`、`rk_ab_retry_boot`）：
- 存储后端：`rk_storage_emmc` / `rk_storage_mtd` / `rk_storage_ufs`
- 升级源：`rk_source_sd` / `rk_source_usb` / `rk_source_url`
- 升级特性：`rk_diff_update` / `rk_gpt_update` / `rk_idblock_update`
- A/B：`rk_ab_support`（来自 board config）/ `rk_ab_retry_boot`
- UI：`rk_ui_enable`（false 时 `ui_null.c` 空实现，true 时启用 minui + DRM/fbdev）

## 构建

```
hb build rockchip_recovery
```

独立 Makefile 目标用于本地 cross-compile 调试：
```
make CROSS_COMPILE=aarch64-linux-gnu- [RK_STORAGE_MTD=0 ...]
```
