# enigma2-hi3798cv200

将 [openatv/enigma2](https://github.com/openatv/enigma2)（OpenATV 8.0 GUI）移植到
HiSilicon **Hi3798CV200**（DVBIP-1002 板）+ **Debian 12 (bookworm) arm64** 的构建工程。

所有编译均在 GitHub Actions 公开仓库的 **ubuntu-24.04-arm 原生 arm64 runner** 上完成，
本地不需要交叉工具链。

## 工作流

| Workflow | 内容 | 产物 |
|---|---|---|
| `kernel-dvbip1002` | `HiSilicon-Development/linux-6.12.y`，`hi3798cv200_defconfig` 基座 + `dvbip_1002_defconfig` 片段（merge_config.sh 合并） | `linux-image-*.deb`、`linux-headers-*.deb`、`linux-libc-dev-*.deb` |
| `enigma2-debian12-arm64` | openatv/enigma2 master + Debian 12 原生构建 | `enigma2-hi3798cv200_*.deb` |

> 注意：`dvbip_1001/1002/1004/1006_defconfig` 均为 1-2 行的差异片段，不能单独使用，
> 必须叠在 `hi3798cv200_defconfig` 基座上（`scripts/kconfig/merge_config.sh -m`）。

上游参考固件（HiSilicon-Development/firmware releases）：
- `linux-headers-6.12.112-dvbip-1002_6.12.112-1_arm64.deb`（header 包命名与版本对齐参考）
- `update-1002.tar.gz`（刷机包）

触发方式：手动 `workflow_dispatch`、推送对应 workflow 文件、每周定时跟踪上游。

## Debian 12 源里没有的依赖（workflow 内源码补编）

| 依赖 | 来源 | 说明 |
|---|---|---|
| `sigc++-3.0` | libsigcplusplus 3.2.0 | bookworm 只有 sigc++-2.0 |
| `libdvbsi++` | OpenVisionE2/libdvbsi | autotools |
| `tuxbox-tuxtxt` | OpenPLi/tuxtxt (libtuxtxt) | enigma2 强依赖的 PKG_CHECK |

## 盒子端安装顺序

```sh
dpkg -i linux-image-*.deb            # 内核（重启进入 6.12-dvbip-1002）
apt install -y gstreamer1.0-plugins-{base,good,bad} gstreamer1.0-libav python3-twisted
dpkg -i enigma2-hi3798cv200_*.deb
```

> 注意：`sigc++-3.0`、`libdvbsi++`、`libtuxtxt` 由 workflow 单独产出运行库后随 enigma2
> 依赖安装（后续版本会一并提供 .deb）。

## 当前里程碑

- [x] 仓库与 CI 框架
- [x] 内核 `hi3798cv200_defconfig` + `dvbip_1002_defconfig` 编译通过并产出 .deb
- [x] enigma2 在 Debian 12 arm64 下 configure/build 通过并产出 .deb（40MB artifact）
- [ ] 三个缺失依赖库的 .deb 化（sigc++3 / libdvbsi++ / libtuxtxt32bpp）
- [ ] 盒子端运行适配（framebuffer / DVB 前端 / 遥控 / 音视频 sink，boxtype 私有化）

`BOXTYPE`/`MACHINEBUILD` 目前默认 `u5`（enigma2 的 HiSilicon 通用路径），运行适配阶段
会新增 `dvbip1002` boxtype 并落到本仓库维护的 patch。
