# uefi/：Mu-Silicium 的覆盖文件

K90（annibale）的 UEFI 源码在 [hackios-mu-silicium](https://github.com/iola1999/hackios-mu-silicium) 的 `k90-port` 分支，克隆到 `Mu-Silicium/` 后直接构建，不经过本目录的 `apply.sh`。构建步骤见顶层工作区的 [annibale UEFI 与 loader 镜像构建](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/docs/analysis/annibale-uefi-build.md)。

本目录与 K90 有关的部分：

- `files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.[ch]`：从载荷启动 `loader.efi`，没有可启动的载荷或 loader 返回时直接发 PSCI SYSTEM_RESET 复位（`loader-v17` 起）。两个文件与 Mu 仓库 `k90-port` 的同名文件逐字节相同，`tools/test-xnu-payload-terminal.py` 检查一致性和控制流。
- `annibale/`：annibale 专属文件的维护副本，见 [annibale/README.md](annibale/README.md)。
- `mu-silicium.patch`：BootShim（把 UEFI 镜像后面的 XNUP 容器复制到 `0xF0000000`）与 BootManagerLib（不进启动菜单，改为启动载荷里的 loader）的改动，Mu 仓库的 `k90-port` 已包含。

其余内容来自上游的 POCO X5 Pro（redwood）移植，本分支不维护：

- `BASE` 是 redwood 移植所基于的 Mu-Silicium 提交。`apply.sh` 克隆 Mu-Silicium、应用 `mu-silicium.patch` 并复制 `files/`；`export.sh` 把 `Mu-Silicium/` 里的修改导回本目录。不要对 annibale 工作树运行这两个脚本。
- `files/Platforms/Xiaomi/redwoodPkg`、`files/Resources/` 与 `files/Binaries/redwood` 是 redwood 的平台描述、设备树和 UEFI 驱动，驱动来源见 [files/Binaries/redwood/README.md](files/Binaries/redwood/README.md)。
- 新机型的移植步骤见 [docs/PORTING.md](../docs/PORTING.md)。
