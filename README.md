# hackios-xnu-kernel（REDMI K90 移植分支）

本仓库是 [HackiOS K90 移植](https://github.com/iola1999/hackios-redmi-k90-annibale)的内核部分：XNU 12377 的补丁与驱动、驻留 loader、loader 镜像的装配脚本、ath12k 的 LinuxKPI 层，以及 iOS 26 的 Metal 驱动。默认分支 `k90-port`。使用方法、当前进展和排查记录都在顶层工作区。

本仓库 fork 自 OOSEX GANG 的 [hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel)，保留其 Git 历史。

## 1. 构建

内核（`xnu-12377.121.6`）在 macOS 上用 Xcode 构建，依赖放在 Git 忽略的 `xnu-deps/26/`：

```sh
xnu26/apply.sh                                # 取上游源码，应用 xnu26/patches/series，复制 xnu/files/ 与 xnu26/files/
xnu26/deps-macos.sh --stage1                  # 依赖源码与 SDK
xnu26/build-macos.sh exporthdrs installhdrs   # 导出内核头文件
xnu26/deps-macos.sh --stage2                  # 用内核头文件编 libfirehose_kernel.a、libpthread_kext.a
tools/mk-ath12k.sh                            # Wi-Fi 驱动 ath12k，build-macos.sh 发现 out/ath12k.o 时链接进内核
xnu26/build-macos.sh                          # 构建内核
```

- 产物是 `xnu-src/xnu-12377.121.6/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`。在顶层工作区执行 `python3 tools/k90auto/register-kernel.py 12377-local` 登记后，`tools/k90auto/cycle.py --kernel 12377-local` 用它启动。
- `xnu26/apply.sh` 在构建树已存在时直接退出；构建树里的修改用 `xnu26/export.sh` 导回仓库。
- 真机轮次实际用的构建命令记在每轮 `kernel.json` 的 `build.command`，增量构建的做法见顶层 [k90auto 说明](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/tools/k90auto/README.md)。
- loader：`EXTRA_CFLAGS=-DDIRECT_PHONE_PLAT xnuloader/build.sh`，产物是 `xnuloader/loader-phone.efi`。
- loader 镜像：`xnuloader/mkpayload.py` 封装载荷，`mkxnuimg.py` 把 BootShim、UEFI、载荷和设备树装成 Android boot 镜像。UEFI 源码在 [hackios-mu-silicium](https://github.com/iola1999/hackios-mu-silicium) 的 `k90-port`，克隆到 `Mu-Silicium/`。完整步骤见顶层 [annibale UEFI 与 loader 镜像构建](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/docs/analysis/annibale-uefi-build.md)。
- 系统盘、Mesa 与打包上机：见顶层 [README](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/README.md)。

## 2. 目录

| 路径 | 内容 |
|---|---|
| `xnu26/` | 12377 内核：`patches/`（对上游文件的改动，`series` 定顺序）、`files/`（12377 专用的新文件）、构建脚本 |
| `xnu/files/` | 内核驱动与子系统新文件。K90 的驱动在 `bsd/dev/`：Adreno 830（`adreno8*.c`）、按键、电量、调频、触摸、WCN7850 的 MHI 上电等 |
| `xnuloader/` | 驻留 loader（UEFI 应用）：USB 传输、看门狗、接收内核日志；`mkpayload.py` 封装载荷 |
| `mkxnuimg.py` | 装配 loader 镜像 |
| `uefi/` | Mu-Silicium 的覆盖文件，见 [uefi/README.md](uefi/README.md) |
| `wifi/lx/` | ath12k 在本内核上编译所需的 LinuxKPI 层 |
| `tools/userland-driver/` | iOS 26 的 Metal 驱动，由顶层 `tools/k90auto/package.py` 与用户态 `userland/build.sh` 编译 |
| `tools/` | 主机工具与测试：`mk-fw-wifi.py`（从 modem 分区取 Wi-Fi 固件）、`mk-ath12k.sh`、`reseal-loader-img.py`、`test-*.py` |
| `common/` | 设备配置 `devices/annibale.conf` 与构建辅助脚本 |
| `docs/` | 上游的移植指南 [PORTING.md](docs/PORTING.md) 与 [checks.md](docs/checks.md)（英文） |

## 3. 许可证

- 仓库不含 iOS 文件。系统盘在使用者自己的机器上由用户态仓库的 `ios26/fetch.sh` 从 IPSW 取出。
- 本仓库按 [Apple Public Source License 2.0](LICENSE) 发布。`xnu/`、`xnu26/` 是 XNU 的衍生作品，按 APSL 2.0 发布；loader、脚本等其余代码也按同一许可证发布。第三方代码保留各自的许可证，见下文。
- 内核链接的 ath12k（BSD-3-Clause-Clear）与 FreeBSD LinuxKPI（BSD-2-Clause）的版权与许可证全文见 [NOTICES.md](NOTICES.md)。
- `xnu/files/bsd/miscfs/apfs/libzbitmap/` 与 `xnu-deps/build/lzbitmap/` 是 libzbitmap（Corellium LLC，此处为 MIT）；`xnu/files/bsd/miscfs/apfs/libdeflate/` 是 libdeflate 1.24（Eric Biggers，MIT，见该目录的 `COPYING`）。
- `xnu/files/bsd/dev/mhi/mhi_boot.c` 与 `mhi_boot.h` 是 UniXNU 的 MhiBoot，未修改，BSD-2-Clause-Patent，取自 hakios-arm64e-dada（`bsd/dev/hakios_mhi_boot.c`）；`xnu/files/bsd/dev/mhi/mhi_pcie_fw.c` 参照该项目的 `bsd/dev/hakios_dada_wifi.c`（HakiOS，APSL 2.0）。
- ath12k、Linux 与 FreeBSD 的源码不在仓库里，由 `tools/mk-ath12k.sh` 下载固定版本。`wifi/lx/patches/*.patch` 是对 FreeBSD 头文件的修改，`wifi/lx/` 其余部分是本项目代码。`xnu-deps/build/ath12k/out/ath12k.o` 存在时构建的 12377 内核会链接 ath12k（BSD-3-Clause-Clear）和 FreeBSD LinuxKPI（BSD-2-Clause）代码。
- `prebuilt/uefi/mkbootimg.py` 来自 AOSP，Apache-2.0。
- HackiOS 是独立研究项目，与 Apple Inc. 没有关联，也没有得到其授权或认可。iOS、iPhone 和 Apple 是 Apple Inc. 的商标。

## 4. 致谢

上游 HackiOS 由 OOSEX GANG 开发：[@oodze](https://t.me/oodze)（发起人）、[@netusd](https://t.me/netusd)、[@wannqn](https://t.me/wannqn)、[@hxfuxyyXD](https://t.me/hxfuxyyXD)，动态见 [t.me/oosexgangdevs](https://t.me/oosexgangdevs)。感谢原作者开放 XNU 移植、loader、驱动和构建工具。
