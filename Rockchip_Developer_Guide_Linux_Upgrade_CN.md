# Rockchip Linux Recovery 升级说明（当前代码草稿）

版本：V2.0.0-current

日期：2026-06-22

适用代码：`third_party/rockchip_recovery`

本文档基于当前 `rockchip_recovery` 代码整理，用于替代旧文档中已经不再准确的 `--partition` 位图描述。当前代码的升级选择改为 `--upgrade=<all|name,name,...>` 名称列表，升级范围由 `update.img` 中实际包含的 item 与 `--upgrade` 指定的分区共同决定。

## 1. 概述

当前工程包含两个主要可执行程序：

| 程序 | 作用 |
| --- | --- |
| `recovery` | Recovery 系统入口。负责读取 BCB/命令行、检测 SD/USB 升级源、挂载升级包所在分区、拉起 `updateEngine`，并在结束后清理 BCB、保存日志、重启。 |
| `updateEngine` | 实际执行升级。负责下载/拷贝升级包、解析 RK 固件、生成升级计划、执行 precheck、写入存储、必要时切换 A/B slot。 |

当前支持的主要升级模式：

| 模式 | 入口 | 说明 |
| --- | --- | --- |
| Recovery 模式 OTA | Normal 系统执行 `updateEngine --misc=update ...` | 非 A/B 产品推荐路径。Normal 阶段最多只升级 `recovery`，其余分区通过 BCB 交给 Recovery 系统升级。 |
| Recovery 系统内本地升级 | Recovery 读取 BCB 的 `--update_package` | `recovery` 启动后拉起 `updateEngine --update`，按 BCB 中的 `--upgrade` 列表升级。 |
| Linux A/B OTA | Normal 系统执行 `updateEngine --update ...` | 直接升级非当前 slot，升级完成后设置另一个 slot 为下次启动目标。 |
| SD/USB 启动盘升级 | `recovery` 自动检测 SD/USB 启动源 | 按 full-flash 模式升级整包，默认传入 `--upgrade=all,parameter,bootloader`。 |
| 恢复出厂设置 | `updateEngine --misc=wipe_userdata` | 写入 misc wipe 命令，重启后格式化 `/userdata`。 |

## 2. 目录结构

| 目录 | 内容 |
| --- | --- |
| `app/recovery/` | `recovery` 主流程、BCB 参数解析、日志保存、UI 菜单、启动 `updateEngine`。 |
| `app/engine/` | `updateEngine` 主流程、命令分发、升级计划、目标解析、precheck、版本比较。 |
| `app/common/` | SD/USB/本地升级源检测和路径解析。 |
| `core/` | RK 固件解析、MD5/CRC/RC4、BCB/A/B slot、普通刷写、差分刷写、GPT、IDBlock。 |
| `storage/` | 存储后端探测、by-name 路由、MTD/Block I/O、mount/format/fstab。 |
| `include/recovery/` | 公共头文件。 |
| `ui/` | UI 和 `RecoveryNoUi=1` 空 UI 实现。 |

## 3. 编译配置

### 3.1 GN/OpenHarmony 构建

```shell
hb build rockchip_recovery
```

`BUILD.gn` 当前主要裁剪开关：

| GN 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `rk_storage_emmc` | `true` | eMMC/block 后端。 |
| `rk_storage_mtd` | `true` | NAND/NOR MTD 后端。 |
| `rk_storage_ufs` | `true` | UFS/SATA SCSI 后端。 |
| `rk_source_sd` | `true` | SD 卡升级源。 |
| `rk_source_usb` | `true` | USB 升级源。 |
| `rk_source_url` | `false` | HTTP/HTTPS 下载，需要 curl。 |
| `rk_diff_update` | `true` | 差分升级，需要 bzip2。 |
| `rk_gpt_update` | `true` | `parameter`/GPT 升级。 |
| `rk_idblock_update` | `true` | `bootloader`/IDBlock 升级。 |
| `rk_ab_support` | `true` | A/B misc slot 支持。 |
| `rk_ab_retry_boot` | `false` | `false` 为 successful boot，`true` 为 retry boot。 |
| `rk_ui_enable` | `false` | 是否启用 UI。 |

### 3.2 本地 Makefile 构建

```shell
make RecoveryNoUi=1
```

常用裁剪验证命令：

```shell
make RecoveryNoUi=1 RK_DIFF_UPDATE=0 RK_SOURCE_URL=0
make RecoveryNoUi=1 RK_DIFF_UPDATE=1 RK_SOURCE_URL=1
make RecoveryNoUi=1 RK_GPT_UPDATE=0 RK_IDBLOCK_UPDATE=0
```

Makefile 开关名与 GN 基本对应，例如 `RK_STORAGE_MTD=0`、`RK_SOURCE_URL=1`、`RK_DIFF_UPDATE=0`。

## 4. updateEngine 参数

当前 `updateEngine` 主要参数：

| 参数 | 说明 |
| --- | --- |
| `--update` | 执行升级。可带可选值 `sdboot` 或 `usbboot`，用于 SD/USB full-flash 路径。 |
| `--misc=update` | Recovery 模式 OTA 编排入口。Normal 系统先处理 `recovery`，再写 BCB 交给 Recovery。 |
| `--misc=now` | A/B 模式下标记当前 slot 启动成功。通常由开机脚本调用。 |
| `--misc=other` | 设置另一个 A/B slot 为下次启动目标。 |
| `--misc=display` | 打印 A/B 和 misc 信息，用于调试。 |
| `--misc=wipe_userdata` | 写入恢复出厂设置命令。 |
| `--misc_custom=read|write|clean` | 读写/清理自定义 misc cmdline。 |
| `--image_url=<path|url>` | 升级包路径。本地路径或 HTTP/HTTPS URL。HTTP/HTTPS 需要编译 `RK_SOURCE_URL=1`。 |
| `--savepath=<path>` | 将升级包保存或拷贝到指定路径。为空或与 `--image_url` 相同则原地使用。 |
| `--upgrade=<list>` | 指定升级范围。除 SD/USB 自动流程外，升级时必须传入。 |
| `--version_url=<path|url>` / `--check=<path|url>` | 版本比较输入。远程版本文件需要 `RK_SOURCE_URL=1`。 |
| `--reboot` | 命令成功后重启。 |
| `--version` | 打印 `updateEngine` 版本。 |
| `--help` | 打印帮助。 |
| `--pipefd=<fd>` | `recovery` 内部使用，用于接收 `updateEngine` 输出，不建议用户直接使用。 |

### 4.1 `--upgrade` 语义

当前代码不再使用旧文档的 `--partition=0x...` 位图。升级范围由下面三者相交得到：

```text
update.img 中实际包含的 item
  ∩ --upgrade 指定的名称
  ∩ A/B slot 过滤规则
```

支持格式：

| 写法 | 含义 |
| --- | --- |
| `--upgrade=boot,rootfs` | 只升级 `boot` 和 `rootfs`。 |
| `--upgrade=all` | 升级包中除 `package-file`、`parameter`、`bootloader` 外的所有 item。 |
| `--upgrade=all,parameter` | 在 `all` 基础上额外升级 GPT/parameter。 |
| `--upgrade=all,bootloader` | 在 `all` 基础上额外升级 IDBlock/bootloader。 |
| `--upgrade=all,parameter,bootloader` | SD/USB full-flash 当前使用的完整升级范围。 |
| `--upgrade=all,-userdata` | `all` 但排除 `userdata`。推荐用 `-name`，避免 shell 对 `!` 做历史展开。 |
| `--upgrade='all,!userdata'` | 与 `all,-userdata` 等价，但需要引号。 |

注意事项：

1. `parameter` 和 `bootloader` 是显式升级项，`all` 不会自动包含它们。
2. `boot` 这类基础名可以匹配 `boot_a`/`boot_b`。A/B 模式下会保留非当前 slot 的分区，跳过当前 slot。
3. `-name` 或 `!name` 排除规则优先级高于显式包含和 `all`。
4. 当前代码没有对拼错的 token 做独立报错。例如 `--upgrade=boot.recovery` 不会匹配 `boot` 和 `recovery`，最终通常表现为 `no partitions to upgrade`。

## 5. Recovery 模式 OTA

Recovery 模式 OTA 推荐在 Normal 系统中使用 `--misc=update`：

```shell
updateEngine --image_url=/userdata/update.img \
    --misc=update \
    --savepath=/userdata/update.img \
    --upgrade=boot,recovery,rootfs \
    --reboot &
```

网络下载示例：

```shell
updateEngine --image_url=http://server/update.img \
    --misc=update \
    --savepath=/userdata/update.img \
    --upgrade=boot,recovery,rootfs \
    --reboot &
```

### 5.1 非 A/B Normal 系统的两步流程

当前代码通过 `get_current_slot()` 判断是否为 A/B。非 A/B 系统中 `current_slot == -1`。

当在 Normal 系统执行 `--misc=update` 时：

1. `updateEngine` 先把升级包保存到 `--savepath`。如果 `--savepath` 为空，则使用 `--image_url` 原路径。
2. 如果 `--upgrade` 中包含 `recovery`，Normal 阶段只升级 `recovery` 分区。
3. 其他分区通过 BCB 写给 Recovery 系统，BCB 内容包括：
   - `--update_package=<savepath 或 image_url>`
   - `--upgrade=<剩余分区列表>`
4. 如果原始参数是 `--upgrade=all` 且包含 `recovery`，写入 BCB 的列表会变成 `all,!recovery`，避免新启动的 recovery 再次刷写自己的分区。
5. `--reboot` 生效后进入 Recovery 系统继续升级。
6. Recovery 升级成功后删除升级包、清理 BCB、保存日志并重启回 Normal 系统。

示例：

| Normal 命令中的 `--upgrade` | Normal 阶段 | 写入 BCB 的 `--upgrade` |
| --- | --- | --- |
| `boot,recovery` | 先刷 `recovery` | `boot` |
| `all` | 先刷 `recovery` | `all,!recovery` |
| `boot,rootfs` | 不刷分区，只写 BCB | `boot,rootfs` |
| `recovery` | 只刷 `recovery` | 不写剩余分区 |

### 5.2 Recovery 系统执行 OTA

Recovery 从 BCB 读取参数后，最终会拉起：

```shell
/usr/bin/updateEngine --pipefd=<fd> \
    --image_url=<update_package> \
    --update \
    --upgrade=<BCB 中的列表>
```

Recovery 会挂载升级包所在分区，并尝试卸载其他分区。`updateEngine` 还会在写入前执行 precheck，如果目标分区仍然处于挂载状态，则会中止升级。

如果升级包放在 `/userdata/update.img`，且同时选择升级 `userdata`，当前 precheck 会认为目标仍被挂载，升级会失败。SDK 使用时应避免把升级包放在将要刷写的同一个分区，或不要在该次升级中选择 `userdata`。

## 6. Linux A/B 模式

A/B 模式下推荐在 Normal 系统直接执行 `--update`：

```shell
updateEngine --image_url=/userdata/update_ota.img \
    --update \
    --upgrade=boot,rootfs \
    --reboot &
```

当前 A/B 规则：

1. `get_current_slot()` 返回 `0` 表示当前 A slot，返回 `1` 表示当前 B slot。
2. 升级计划保留非当前 slot 的分区。例如当前为 A，则 `boot_b`、`system_b` 会进入计划，`boot_a`、`system_a` 会被跳过。
3. 未带 `_a`/`_b` 后缀的共享分区不做 slot 过滤。
4. 升级成功后调用 `set_slot_activity()`，将另一个 slot 设置为更高优先级并设置 retry 次数。
5. 新 slot 正常启动后，应由开机脚本调用：

```shell
updateEngine --misc=now
```

手动切换到另一个 slot：

```shell
updateEngine --misc=other --reboot
```

查看 A/B 和 misc 信息：

```shell
updateEngine --misc=display
```

## 7. SD/USB 启动盘升级

Recovery 启动后会根据 kernel cmdline 判断是否从 SD/USB 升级介质启动，并挂载对应介质。

当前镜像文件名优先级：

1. `update_ab.img`
2. `update_ota.img`
3. `sdupdate.img`
4. `update.img`

如果 SD/USB 根目录存在 `sd_boot_config.config`，并包含 `image=<name>`，会优先尝试该文件名。

SD/USB 升级时，`recovery` 自动调用：

```shell
updateEngine --image_url=<resolved-image> \
    --update=sdboot 或 --update=usbboot \
    --upgrade=all,parameter,bootloader
```

该模式按 full-flash 解析：

1. 普通分区写到对应后端的 whole-device 节点，偏移来自固件 item 的 `flash_offset`。
2. `parameter` 走 GPT 生成与写入路径。
3. `bootloader` 走 IDBlock 写入路径。
4. SD/USB 模式当前跳过写后 MD5 readback verify，以减少整盘升级耗时。
5. 升级结束后要求用户拔出 SD/USB 介质，否则重启后可能再次进入 SD/USB recovery。

## 8. 参数分区和 Bootloader 升级

### 8.1 `parameter` / GPT

`parameter` 对应 GPT 升级，只在编译 `RK_GPT_UPDATE=1` 时可用。

规则：

1. `parameter` 必须通过 `--upgrade=parameter` 或 `--upgrade=all,parameter` 显式指定。
2. 如果本地 OTA 选择了 `parameter`，当前升级计划会进入 whole-disk 坐标系，即日志中显示 `upgrade mode: full-flash (GPT in set -> whole-disk)`。
3. GPT 写入前会解析新 `parameter.txt`，生成 primary GPT 和 backup GPT。
4. GPT 几何使用后端 `disk_lbas()` 获取，不能获取容量时 whole-device 目标会报错中止。

GPT precheck 当前策略：

1. 如果目标是 block whole-disk，允许存在挂载中的子分区，但要求挂载分区在新 parameter 布局中仍存在、起始 sector 不变、大小不缩小。
2. 如果某个普通分区因 GPT-in-set 也走 whole-disk 写入，则只要写入范围不与挂载分区重叠即可。
3. 如果新 GPT 中某个已有分区起始位置移动，但该分区未包含在本次 `--upgrade` 列表中，precheck 会中止，避免 GPT 已变但数据仍留在旧位置。
4. 起始位置不变但大小变化且未升级该分区，目前只告警；`-@` grow 分区按扩展到末尾处理。
5. 非 block GPT 目标，例如 MTD whole-device，仍采用保守挂载检查。

### 8.2 `bootloader` / IDBlock

`bootloader` 对应 IDBlock 升级，只在编译 `RK_IDBLOCK_UPDATE=1` 时可用。

规则：

1. `bootloader` 必须通过 `--upgrade=bootloader` 或 `--upgrade=all,bootloader` 显式指定。
2. IDBlock 目标使用后端 `bootloader_dev`，而不是普通 by-name 分区。
3. eMMC 和 MTD 的 `bootloader_dev` 通常与 whole-device 相同。
4. UFS 使用 boot LUN，例如 `/dev/sdb`。
5. SATA 不作为 bootloader 介质，`bootloader_dev == NULL`。如果固件 item 被路由到 SATA 且显式要求升级 bootloader，会报错中止。

如果编译时关闭 `RK_GPT_UPDATE` 或 `RK_IDBLOCK_UPDATE`，当前代码会跳过对应 item，且不会退回到普通 raw 分区写入，避免把 GPT 文本或 IDBlock 镜像错误写入普通分区。

## 9. precheck 写入前检查

`updateEngine` 在真正写入前会对完整计划执行 precheck。任一项失败都会在写入前中止。

当前检查项：

| 检查 | 行为 |
| --- | --- |
| 挂载检查 | 普通目标不能处于挂载状态；whole-device 目标默认也不能有挂载子分区。GPT block 目标有布局安全例外。 |
| 容量检查 | `flash_offset + img_size` 不能超过目标容量。whole-device 目标探测容量失败时中止。 |
| GPT layout closure | 升级 GPT 时，如果某个分区起始位置变化但没有被一起升级，中止。 |
| GPT mounted layout | 升级 GPT 时，挂载中的 block 子分区必须保持起始位置不变且不能缩小。 |

典型失败：

```text
precheck: userdata target /dev/block/by-name/userdata is mounted ... abort
```

这表示升级包所在分区或目标分区仍处于挂载状态。解决方式应由升级命令和打包策略决定，例如不要选择正在承载升级包的 `userdata`，或将升级包放到其他分区/介质。

## 10. 存储后端和写入路径

当前存储后端分为 block 和 MTD 两类。

| 后端 | 设备类型 | 说明 |
| --- | --- | --- |
| eMMC | block | `/dev/mmcblk*`，支持 whole-device 和 by-name。 |
| UFS | block/SCSI | user LUN 通常为 `/dev/sda`，bootloader LUN 为 `/dev/sdb`。 |
| SATA | block/SCSI | 通常为 `/dev/sda`，不可写 bootloader，常见于 NOR+SATA 组合。 |
| NAND | MTD | 使用 `nanddump`、`nandwrite`、`flash_erase`，处理坏块/OOB/ECC。 |
| NOR | MTD | 使用 `MEMERASE`、`read`、`write` 等 syscall，支持 RMW。 |

目标解析：

1. OTA 普通分区优先使用 `/dev/block/by-name/<name>`。
2. MTD 分区如果 by-name 指向 `mtdblock`，实际写入会转为 `/dev/mtdN` 字符设备。
3. 如果没有 by-name，MTD 会扫描 `/proc/mtd` 中的分区名。
4. SD/USB full-flash 或 GPT-in-set 使用 whole-device 节点和固件中的物理偏移。
5. 双存储固件通过 SSFW item 的 storage code 路由到对应后端，例如 NOR、NAND、eMMC、SATA。

写入行为：

| 介质 | 普通大镜像写入 | 小块数据写入 |
| --- | --- | --- |
| block | 从升级包分片读出，写入目标 offset。 | 直接写目标 offset。 |
| NAND | 继续使用外部 `flash_erase`、`nandwrite -p`、`nanddump`。大镜像按文件路径写入，避免整分区进内存。 | 对非擦除块对齐的小数据使用 read-modify-write，保留同一 eraseblock 内其他数据。 |
| NOR | 通过 syscall 擦除并写入。 | 非擦除块对齐时做 read-modify-write。 |

当前 `storage_ops->read/write` 接口同时支持内存 buffer 和文件路径两类来源/目标。该接口仍处于重构讨论阶段；当前版本先按现有代码记录，不把它作为稳定 SDK API。

## 11. 差分升级

编译 `RK_DIFF_UPDATE=1` 后启用差分升级路径。

当前代码行为：

1. `flash_normal()` 会检查 image item 尾部是否为差分格式。
2. block 分区差分会先生成 patched 临时文件，再写入 block 目标。
3. raw MTD 分区差分会先通过 MTD read 读出旧数据，应用 patch 后再通过存储后端写入。
4. UBI SquashFS 分区差分走 UBI 相关路径。
5. 如果 image item 不是差分格式，会回退到普通整镜像写入。

注意：

1. 差分包制作工具仍需保证旧固件与目标设备一致。
2. 差分过程需要额外空间保存 patch 结果；通常要求升级包所在分区还有足够空间存放新镜像临时文件。
3. 差分工具对 A/B、ext 文件系统等组合的支持情况应以打包工具实际能力为准。

## 12. 日志

`recovery` 和 `updateEngine` 当前都会同时输出到串口和临时 recovery log。Recovery 结束时会把日志复制到持久化路径。

常用查看方式：

```shell
cat /userdata/recovery/Log
cat /userdata/recovery/last_log
```

若产品使用旧 SDK 的 `.rkdebug` 方式打开串口日志，请以对应 buildroot overlay 配置为准。当前代码本身默认将 `recovery` 和 `updateEngine` 日志输出到控制台。

## 13. 恢复出厂设置

执行：

```shell
updateEngine --misc=wipe_userdata --reboot
```

流程：

1. 写入 misc wipe 命令。
2. 重启进入对应处理流程。
3. 格式化 `/userdata`。

Recovery 菜单中选择 wipe data 时也会调用同一类格式化路径。

## 14. 常用命令示例

### 14.1 非 A/B OTA，升级 boot/recovery/rootfs

```shell
updateEngine --image_url=/udisk/update.img \
    --misc=update \
    --savepath=/userdata/update.img \
    --upgrade=boot,recovery,rootfs \
    --reboot &
```

### 14.2 非 A/B OTA，升级除 userdata 外的普通分区

```shell
updateEngine --image_url=/udisk/update.img \
    --misc=update \
    --savepath=/userdata/update.img \
    --upgrade=all,-userdata \
    --reboot &
```

### 14.3 非 A/B OTA，同时显式升级 parameter

```shell
updateEngine --image_url=/udisk/update.img \
    --misc=update \
    --savepath=/userdata/update.img \
    --upgrade=all,-userdata,parameter \
    --reboot &
```

该命令会进入 GPT-in-set whole-disk 模式。若 `/userdata` 承载升级包，precheck 要求新 parameter 中 `userdata` 起始位置不变且不缩小，并且其他写入范围不能覆盖 `/userdata`。

### 14.4 SD/USB full-flash 手动调用

```shell
updateEngine --image_url=/mnt/udisk/sdupdate.img \
    --update=usbboot \
    --upgrade=all,parameter,bootloader
```

正常 SD/USB 启动升级由 `recovery` 自动调用，不需要用户手动执行。

### 14.5 A/B OTA

```shell
updateEngine --image_url=/userdata/update_ota.img \
    --update \
    --upgrade=boot,rootfs \
    --reboot &
```

### 14.6 查询 misc/A/B 信息

```shell
updateEngine --misc=display
```

## 15. 自定义分区升级

当前代码不再维护旧版固定 `UPDATE_CMD update_cmd[]` 分区表。普通分区升级是数据驱动的：

1. 打包时把自定义 item 加入 `package-file`，例如 `factory Image/factory.img`。
2. 设备端需要能解析到目标分区：
   - block 设备存在 `/dev/block/by-name/factory`；或
   - MTD 设备的 `/proc/mtd` 中存在同名分区。
3. 调用时显式指定：

```shell
updateEngine --image_url=/userdata/update.img \
    --update \
    --upgrade=factory
```

普通自定义分区默认走 `flash_normal()`。只有 `parameter` 和 `bootloader` 这类特殊 item 会进入专用写入路径。

## 16. 当前限制和注意事项

1. `--upgrade` 拼写错误当前不会在解析阶段明确指出，只会导致匹配不到分区或计划为空。
2. `!name` 在交互式 shell 中可能触发 history expansion，建议 SDK 文档示例使用 `-name`。
3. GPT layout closure 当前以第一个 `ITEM_GPT` 为主。多存储、多 parameter 的复杂场景需要结合真实固件布局验证。
4. NAND 仍依赖外部 `flash_erase`、`nandwrite`、`nanddump`。这是为了继续利用 mtd-utils 对坏块、OOB、ECC 的处理能力。
5. NAND/NOR 进度回调当前不是完整的上层流式进度模型，后续如要做细粒度进度，建议先重新设计 storage write request/source 抽象。
6. SD/USB full-flash 当前跳过写后 MD5 readback verify，这是耗时策略，不代表写入错误不可检测。
7. full-flash、GPT、IDBlock 等 whole-device 目标要求后端能获取容量和 whole-device 节点；探测失败会中止，不静默猜值。
8. Buildroot SDK 树与当前 OpenHarmony `third_party/rockchip_recovery` 树是独立代码树，本文档只描述当前目录代码。

