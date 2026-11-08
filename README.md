# enigma2-hi3798cv200

将 [openatv/enigma2](https://github.com/openatv/enigma2)（OpenATV 8.0）移植到
海思 **HiSilicon Hi3798CV200** 平台（覆盖歌华 DVB-IP 1001 / 1002 / 1004 / 1006 系列机顶盒）+ **Debian 12 / 13 arm64**。

本工程是应用层适配与打包入口，在官方上游源码基础上应用 HiSTB 平台补丁与资源组装，
产出与配套内核及 HiVXE FFmpeg 协同运行的 Debian 原生运行时包。

> **适用机型**：本工程适用于海思 Hi3798CV200 方案的歌华 DVB-IP 全系列（1001 / 1002 / 1004 / 1006），
> 共享相同的 VDP 显控、VDH 硬件解码与 AIAO 音频架构，板型差异由底层设备树（DTB）与前端调谐器驱动承载。

## 构建与工作流

所有编译均在 GitHub Actions 的 `ubuntu-24.04-arm` 原生 runner 上完成：

| 工作流 | 说明 | 产物 |
|---|---|---|
| `enigma2-debian-arm64` | OpenATV 8.0 master + HiSTB 维护补丁，Debian 12 / 13 构建矩阵 | `enigma2-hi3798cv200_*.deb`、`libdvbsi++_*.deb`、`libtuxtxt_*.deb`、`sources.lock.json` |
| `kernel-dvbip1002` | `HiSilicon-Development/linux-6.12.y` 内核与模块 | `linux-image-*.deb`、`linux-headers-*.deb`、`linux-libc-dev-*.deb` |

- **源码与依赖管理**：
  - Enigma2 默认跟踪官方 `openatv/enigma2` master，OpenWebif 与 skindefault 跟踪官方 HEAD；每次构建自动生成 `sources.lock.json` 记录准确快照。
  - `libdvbsi++`（OpenVisionE2）与 `tuxtxt`（oe-alliance 提供 32bpp 渲染）随 workflow 一并补编为独立运行库 `.deb`。
  - Debian 12 镜像自带私有 Python 3.13 运行时（满足 PEP 701 语法）；Debian 13 原生使用系统 Python 3.13。
  - 内核配置片段 `dvbip_1001/1002/1004/1006_defconfig` 需通过 `scripts/kconfig/merge_config.sh -m` 叠在 `hi3798cv200_defconfig` 基座上。

## 音视频与显控管线

`packaging/decoder_histb_impl.h` 直接编译进 Enigma2 核心（`HAVE_HISTB`），实现无独立 video service 的内嵌硬件管线：

```
DVB 射频前端 → histb-dmx (TSDEMUX_TAP)
  → 用户态 TS / PES 解包 (HistbPesDemux)
  → HiVXE FFmpeg 9.0.1 (V4L2 Request hwaccel: MPEG1/2, H.264, HEVC, AVS)
  → DRM PRIME 导出 DMA-BUF → AddFB2 → drmModeSetPlane(V0)
     - 4K Main10: 原生 VDH Tiled 4:2:0 modifier (0x7e)，旁路 VPSS / CPU 拷贝直通显控
     - 8-bit / 兼容: 线性 NV12 / NV15 回退
  → VDP V0 平面硬件缩放 (ZME 8/6/4/4) + VDM 14-bit YCbCr→RGB 硬件转换 → HDMI 输出
```

- **视频解码与流同步**：
  - Coded 几何直接取自 `AVHWFramesContext`（由底层 V4L2 `G_FMT` 确立），按 256 字节 stride 对齐与 64 字节行对齐计算缓冲区，确保原生 tiled 内存完整。
  - `histbVideoEntryPoint()`（由 `packaging/tests/test-video-entry.cpp` 覆盖测试）扫描 AUD / SEI / 扩展头后的真实 SPS / PPS / VPS 关键帧入口，防止切台卡顿。
  - H.264 开放 GOP 严格拒绝缺少重建像素的参考场，防止坏帧引发内核 DPB 污染。
- **DRM / KMS 多平面显示**：
  - 图形层 G0（主 OSD，线性 ARGB8888）、G1、G3 送入 `MIXG0/GP0`；视频层 V0、V1 送入 `MIXV0/VP0`。两组图层各自保持连续，支持全局 / 像素透明度混合。
  - 使用真实 fbdev / DRM 模式与 stride 回读，避免 720p 皮肤在 1080p 画布上出现错位或限制。
  - 无 HDMI 接入或冷启动无 `/dev/fb0` 时，动态分配线性 ARGB8888 DRM dumb / GEM 离屏画布。
  - HDMI 高速链路（TMDS > 340 MHz，如 4K@50Hz / 60Hz）在真实 HPD 插拔后自动恢复 SCDC 扰码与时钟比。
- **HDMI PCM 音频与流控**：
  - AIAO TX3 DAI 1 馈入 HDMI 音频桥，固定协商 48 kHz stereo LPCM（S16_LE）。
  - MP2 音频解复用经流解析器剥离 TS stuffing 并跨 PES 重组完整音频帧。
  - 软件音量基于 Q16 数字平滑增益调节（50% 为 `0x8000` = 0.5，0% = 静音）；停流时在 `ACR_CTRL` 门控 CTS / N 时钟请求，防止杂音。

## 维护中的补丁与集成

在官方源码上按序应用的维护补丁（`.ci/patches/`）：

| 补丁 | 作用 |
|---|---|
| `0001-swig43-output-typemaps.patch` | 兼容 SWIG 4.3 的 Python 指针返回绑定。 |
| `0002-histb-use-actual-framebuffer-geometry.patch` | 回读 fbdev 实际分辨率与 stride，防止 OSD 绘制受限或错位。 |
| `0003-persist-video-wizard-exit.patch` | 退出视频设置向导后持久化完成标记，避免重启重复弹出。 |
| `0004-decode-raw-device-labels.patch` | 对 sysfs 非 UTF-8 设备标签安全解码，防止 UI 启动崩溃。 |
| `0005-histb-display-mode-geometry.patch` | 基于真实 KMS timing 保持桌面比例，支持无 fbdev 离屏画布。 |
| `0006-openatv-ui-capabilities.patch` | 设置项保存 / 取消事务机制与真实硬件能力门控。 |
| `0007-histb-pcm-volume.patch` | 将数字音量与静音直接应用到 HDMI PCM 样本。 |
| `0008-real-platform-and-all-settings.patch` | 呈现真实平台信息，禁止格式化或擦除根分区。 |
| `0009-histb-hdmi-hotplug-recovery.patch` | HDMI 热插拔恢复与严格 EDID 筛选。 |

- **OpenWebif**：集成桌面与手机响应式控制台，支持真实配置保存 / 取消、截图及虚拟遥控器。
- **数据库保护**：兼容上游 `loadBouquet(bool)` 签名并自动清理失效空条目；保存 `lamedb` 时自动过滤无效的零频率转发器。

## 盒子端安装与使用

选择与系统主版本对应的安装包：

```sh
# 安装运行库与应用包（依赖缺失时可用 apt install -y -f 自动补齐）
dpkg -i ./libdvbsi++_*.deb ./libtuxtxt_*.deb ./enigma2-hi3798cv200_*.deb
systemctl daemon-reload
systemctl enable --now e2.service
```

- **配置持久化**：用户配置与频道列表保存在 `/etc/enigma2`；包升级仅补充缺失默认项，不覆盖用户配置，首次开机自动继承系统 `/etc/localtime` 时区。
- **固件管理**：解码固件（`histb-avsp.bin`、`histb-h264-cabac.bin`、`histb-hevc-cabac.bin`）由底层系统镜像 / 内核管理，应用包不打包亦不覆盖固件。
- **远程管理**：浏览器访问 `http://<机顶盒IP>/` 即可使用 OpenWebif 控制台与虚拟遥控器（默认未开启身份认证，仅限可信局域网使用）。
- **Softcam**：原生支持对接外部 `oscam.service`，经 CSA2 四字硬件事务解扰未加密流。

## 已知边界

- **AVS2**：Hi3798CV200 硬件 VDH 无 AVS2 解码引擎，不支持 AVS2 硬解。
- **4K HEVC**：需配套修复了完成判断逻辑的内核 `histb-vdec` 驱动模块。
