# annibale 维护副本

annibale 的 UEFI 源码在公开仓库 [iola1999/hackios-mu-silicium](https://github.com/iola1999/hackios-mu-silicium) 的 `k90-port` 分支（Project-Silicium/Mu-Silicium 的副本），构建步骤见顶层工作区的 `docs/analysis/annibale-uefi-build.md`。这里按 `files/<相对路径>` 存放 annibale 专属覆盖文件的副本，与 `uefi/files/`（redwood 移植的导出目录）分开维护：`uefi/export.sh` 重写 `uefi/files/` 时不读取本目录，也不要对 annibale 工作树运行 `export.sh` 或 redwood 的 `apply.sh`。

`BASE` 记录本副本对应的 Mu-Silicium 提交 `5e3fafb466449bb35c31345e4ec4dd83dfa9abf6`（`k90-port` 上加入副本之前的那一笔）。`k90-port` 的下一笔提交 `cb43663` 已经包含这份副本，从公开仓库克隆 `k90-port` 时不需要再手工复制。

| 文件 | 基线哈希（复制前目标文件应等于此值） | 目标路径（相对 Mu-Silicium） |
|---|---|---|
| MemoryMapLib.c | `7516ffe8c689a127ff8cfdb8f1ce31d3df17d327a218c394f9a2df34f245a8bb` | `Platforms/Xiaomi/annibalePkg/Library/MemoryMapLib/MemoryMapLib.c` |

副本在内存映射表中新增 `XNU_Payload` 保留区间 `[0xF0000000, 0xF3800000)`（`AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN`），在 PEI 阶段就把 BootShim 复制 payload 的窗口排除出 DXE 可分配范围；区间终点受 DTB `trust_ui_vm_region` 静态保留起点约束。当前副本内容哈希为 `1d94d9b2abec170cd5560475e7b57e59f049796c60183993c8b4e98e6f6bcee6`。真机在用的 loader v6～v16 镜像里的 FD 构建于这份副本之前（Mu 仓库标签 `loader-v16`），不含这个保留区间。

XnuLoader.c/h 继续走 `uefi/files/` 的既有 overlay，Mu 与 kernel 两份的字节一致性由 `tools/test-xnu-payload-terminal.py` 检查；本副本与 Mu 文件的字节一致性、保留区间与 DTB 静态范围的契约由 `tools/test-annibale-payload-contract.py` 检查。
