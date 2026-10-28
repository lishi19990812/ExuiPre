# ExuiPre —— 易语言EXUI静态编译单文件交付方案
> 让易语言静态编译的 exe **不再依赖 EXUI 的 ExuiKrnln_Win32.lib 文件**：编译时自动把支持库打包进 exe，
> 运行时自释放、退出自清理，实现真正的**单文件交付**。

[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE) ![Platform](https://img.shields.io/badge/platform-Windows-blue) ![Lang](https://img.shields.io/badge/易语言-静态编译-orange)

> 仓库地址：https://github.com/lishi19990812/ExuiPre
> 精易论坛发布帖：（预留）
---

## 这是什么？
易语言EXUI插件
易语言静态编译时，程序依赖 `lib\ExuiKrnln\ExuiKrnln_Win32.lib` 之类的支持库文件，
交付时必须连 lib 一起打包发给别人，既麻烦又容易漏。

**ExuiPre 一套解决**：

| 组件 | 作用 |
|---|---|
| `ExuiPre.fne` | 易语言支持库插件（IDE 安装用），提供安装/卸载 |
| `ExuiPre_static.lib` | 编译进用户 exe 的运行时机制（TLS 释放 + 看门清理） |
| `ExuiPack.exe` | 链接后自动打包工具（追加 payload + TLS 修补），全自动，无需人工干预 |

## 工作原理

```
编译期：
链接器生成 exe ──► ExuiPack.exe 自动执行：
1. 追加支持库到 exe 尾部（payload）
2. 修补 PE 的 TLS 回调指针，指向 .exuicb 节中的释放回调
3. 埋入元数据（payload 长度等）

运行期：

Windows 加载器在入口点之前触发 TLS 回调（抢跑）
回调从文件尾部倒扫 payload 标记 ──► 释放EXUI的lib 到临时目录
易语言运行时正常加载EXUI支持库
程序退出时"看门娃"自动删除释放的 lib（自清理）
```
**几个设计亮点：**

- 🔍 **倒扫定位**：运行时从文件尾倒扫 payload 标记，不依赖任何固定偏移 —— 
  即使 exe 尾部被追加其他数据（如数字签名证书）也能正确定位
- 🔁 **防重复打包**：多次编译/手动误操作不会重复追加
- ✍️ **签名友好**：打包后签名不影响运行；verify 模式可检查打包/签名状态
- 🚫 **兼容性自检**：检测到未使用 Exui 支持的项目自动跳过，不误伤普通工程

## 项目演进：从易语言模块版到 C++ 版

本仓库中的 `EXUI前置.e` 是本项目的**第一代方案**（纯易语言模块实现），其原理：

1. 将 ExuiKrnln_Win32.lib 以资源形式打包进程序
2. 程序启动时检查运行目录，若无 lib 则释放出来并重启自身
3. 程序结束前通过 bat 脚本删除释放的 lib，实现"单文件交付"

**为何被淘汰**：新版本 Exui 支持库将 lib 存在性检测提前到了更早的加载阶段，
模块代码在易语言初始化阶段才执行，释放 lib 已经来不及，方案失效。

因此第二代方案改用 C++ 实现（本仓库主体）：

| | 方案一：EXUI前置.e | 方案二：ExuiPre（C++） |
|---|---|---|
| 实现方式 | 易语言模块，启动后释放 | 静态库 + TLS 回调，**入口点之前抢跑** |
| 释放时机 | 程序初始化阶段（晚） | 加载器触发 TLS 回调（最早） |
| 是否重启进程 | 后台重启一次，用户无感 | 无需重启，用户无感 |
| 清理方式 | bat 脚本删除（需在程序结束前调用） | 看门娃自动清理，零接入 |
| 兼容UPX | **兼容**，因为是程序执行内调用 | **不兼容**，UPX后TLS回调会失败 |
| 兼容新版 Exui | ✗ 已失效 | ✓ |

`EXUI前置.e` 保留在仓库中仅作历史存档与学习参考，如果要用前请自行重新修改图片资源中的ExuiKrnln_Win32.lib官方文件，**请勿在新项目中使用**。

## 安装

1. 将 `压缩包` 解压的三个文件夹放入易语言目录：
```
易语言根目录\
├─ tools\ExuiPack.exe          ← 打包工具
├─ lib\ExuiPre.fne             ← 易语言支持库插件
└─ static_lib
   ├─ ExuiKrnln_Win32.lib      ← EXUI官方静态库（安装的EXUI官方静态支持库）
   └─ ExuiPre_static.lib       ← 释放机制库
```

2. 安装并编译一次 `ExuiPre.fne`（在易语言-工具-支持库配置 中勾选该支持库），
   它会自动完成 link.ini 的链接后动作注册（ExuiPack 也具备自注册能力，首次运行时自动写入）

3. 完成。之后**每次静态编译自动打包**，编译输出窗口会提示：

```
    [ExuiPack] 编译成功
【重要】单文件模式与 UPX 加壳互斥，二选一：
◆ 使用本插件（推荐）：不要对 exe 做 UPX 加壳，直接签名即可发布。
◆ 需要 UPX 加壳：请勿安装本插件，将 lib 与 exe 放在同一目录随程序发布。
```

## 使用

无需任何手动操作 —— 装好之后，正常编译即可。

**手动模式**（可选）：直接双击 `ExuiPack.exe`，选择 exe 即可手动打包。

**诊断模式**（打包异常时排查用）：
```
ExuiPack.exe --verify 目标.exe
```

输出 TLS 修补状态、埋点状态、payload 位置、签名状态等详细判定信息，只读不改文件。

**调试日志**：默认精简输出；排障时把 `ExuiPack.cpp` 顶部的 `EXUIPACK_LOG` 改为 `1`
重新编译即可开启完整日志（同款开关在 `ExuiPre_static.cpp` 中也有）。

## ⚠️ 与 UPX 不兼容（重要）

**本插件与 UPX 等加壳软件互斥，二选一：**

- **使用本插件**：不要对编译出的 exe 做 UPX 加壳，直接签名即可发布
- **需要 UPX 加壳**：请卸载本插件后重新编译，改为将
  `lib\ExuiKrnln\ExuiKrnln_Win32.lib` 与 exe 放在同一目录随程序发布

> 原因：释放机制（TLS 回调代码）位于 exe 内部，UPX 会把它和其余代码一起压缩。
> 加壳后 TLS 回调触发时 UPX 尚未解压，释放逻辑无法执行，程序无法启动。
> 此为原理性冲突，非 bug。

## 从源码构建
- **ExuiPack.exe**：Visual Studio 2022+ 打开 `ExuiPack/ExuiPack.vcxproj`，选 Release | Win32 直接生成
  （含图标与版本信息资源，rc 已集成在工程内）
- **ExuiPre_static.lib**：对应的静态库工程，Release | Win32 生成
- **ExuiPre.fne**：易语言支持库工程，按易语言 fne 规范编译
> 如果提示缺少SDK,通常在你下载安装好的易语言目录内的sdk文件夹内，编译时，有问题就修改sdk路径，没有问题就可以直接编译

## 目录结构

```
├── ExuiPack/ # 打包工具（VS 工程，C++）
├── ExuiPre_static/ # 运行时释放机制（静态库工程，C++）
├── ExuiPre/ # 易语言支持库插件（fne）
├── EXUI前置.e # 易语言模块源码
├── README.md
└── LICENSE
```
>EXUI前置.e 是易语言模块源码，原理都是一样的，支持EXUI旧版本，新版本EXUI的lib检测机制提前了，所以才有了C++版的插件。
## 常见问题

**Q：编译输出"跳过打包"？**
A：说明当前工程没有使用 Exui 支持库（正常现象），或 exe 已打包/已签名（防重复机制生效）。

**Q：运行时报缺支持库 / 直接闪退？**
A：确认 ExuiPre_static.lib 版本与 ExuiPack.exe 版本配套（两边是特征扫描对接的，
老 lib + 新 ExuiPack 或反之都会在 verify 模式中报"没有 .exuicb 节"或"找不到回调数组"）。

**Q：能加壳吗（UPX 之外的壳）？**
A：凡会压缩/重写代码节和 TLS 目录的壳均不兼容，原理同上。

**Q：用本插件编译的程序，我必须开源吗？**
A：不必。本项目采用 MIT 协议，允许将 ExuiPre_static.lib 静态链接进闭源程序，
你的程序可自由闭源商用。

## License
本项目采用 **MIT License** 开源 —— 详见 [LICENSE](LICENSE)

你可以自由使用、修改、分发本项目，包括将 ExuiPre_static.lib 静态链接进你自己的
程序（无论你的程序开源与否、商用与否），无需开放你的源码。
唯一要求：保留原许可声明与版权信息。

## 致谢

- 易语言静态编译机制研究
- 精易论坛的各位朋友

---

*041 专属 · 2026*
