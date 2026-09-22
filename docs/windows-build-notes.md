# Windows 构建踩坑记录

> 背景：2026-09，在只有 VS2026（18.x） 的机器上从零编小狼毫。
> 结论：装 **VS2022 生成工具** 与 2026 并存，用 2022 的 prompt 编全程。

## 1. 装对版本：要 2022，不要 2026

- 仓库工程是 v143 工具集，CI 用 `windows-2022`。VS2026（18.x，cl 19.5x）下
  Boost 1.84 的 bootstrap 直接报 `Unknown toolset: vcunk`。
- 2026 的安装器“可用”页签里没有 2022，用 winget 装：
  `winget install -e --id Microsoft.VisualStudio.2022.BuildTools`
- 组件 5 件套：MSVC v143、ATL、MFC、Windows SDK、CMake 工具。
  其余（Build Insights、测试适配器、ASan、vcpkg、Copilot）可不装。
- 之后所有命令都在 **x64 Native Tools Command Prompt for VS 2022**
  里跑（`cl` 应显示 19.3x/19.4x），别跟 2026 的 prompt 混用。

## 2. env.bat 用 vs2022 模板

- `copy env.vs2022.bat env.bat`（别拿 vs2019 的，工具集对不上）。
- `BOOST_ROOT` 指向解压后的 Boost 源码，如 `deps\boost_1_84_0`。

## 3. Boost 源码手动下载

- `install_boost.bat` 依赖 `aria2c` + `7z`，缺谁就手动下：
  `https://archives.boost.io/release/1.84.0/source/boost_1_84_0.7z`
  解到 `deps\`，确认 `deps\boost_1_84_0\boost` 存在。
  7-Zip 可 `winget install -e --id 7zip.7zip`。

## 4. b2 认不出工具链（msvc-setup 起不来）

现象：`build.bat boost` 全是
`...skipped ... for lack of ... msvc-setup.nup...`，`stage\lib` 只有 cmake 文件。

修法（两件套，缺一不可）：

1. `deps\user-config.jam` 写死编译器和 setup 脚本（注意正斜杠，
   版本号按实际安装改）：
   ```
   using msvc : 14.3 : "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe" : <setup>"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvarsall.bat" ;
   ```
2. `set BOOST_BUILD_PATH=c:\src\weasel\deps`
  （换 prompt 就失效，建议 `setx BOOST_BUILD_PATH c:\src\weasel\deps` 持久化）。
3. 删脏缓存再重编：`rmdir /s /q deps\boost_1_84_0\bin.v2`。

验证：`stage\lib` 出现 `libboost_regex-vc143-mt-s-x64-1_84.lib` 即成功。

## 5. b2 莫名静默退出（exit 0，什么都不编）

- 症状：停在 `...found N targets...` 直接回提示符，无 `updating`、无报错。
- 我们遇到的根因是 `b2.exe` 自身：它最早是用 VS2026 的编译器编出来的，
  行为不稳定（同命令不同轮次目标数 1936/2078 来回变）。
- 修法：在 VS2022 prompt 里重编 b2（`cd deps\boost_1_84_0`，
  `bootstrap.bat msvc` 覆盖 `b2.exe`），删 `bin.v2` 后重跑。
- b2 自身是否健康可用 `b2 --version` 快速验证。

## 6. x86 库缺失导致 Win32 链接失败

现象：`weasel.sln` 链接报
`LINK : fatal error LNK1104: 无法打开文件“libboost_wserialization-vc143-mt-s-x32-1_84.lib”`。

- `stage\lib` 里 x64 全、x32 缺 `filesystem/json/locale/wserialization`
  即 x86 那轮没编完。`build.bat boost` 的 x86、x64 是两轮独立 b2，
  b2 的 skipped 不算失败也照样 exit 0往下走，所以要手动查
  `stage\lib\*x32*.lib` 是否齐（每个库 `-mt-s` / `-mt-sgd` 各一）。
- 补编 x86 单轮（VS2022 prompt，约 10-15 分钟）：
  ```
  cd deps\boost_1_84_0
  b2 -j8 --with-filesystem --with-json --with-locale --with-regex --with-serialization --with-system --with-thread define=BOOST_USE_WINAPI_VERSION=0x0603 toolset=msvc-14.3 link=static runtime-link=static --build-type=complete architecture=x86 address-model=32 stage
  ```
- 注意 `architecture=x86` 不能省，它决定文件名里的 `-x32-` 标签；
  省掉编出来的是无 tag 名，链接器照样找不到。

## 7. 增量构建顺序

- Boost + rime 编过一次后，只改小狼毫代码用 `build.bat weasel` 即可
  （约 5-10 分钟），不用重跑 `data opencc rime`。
- `librime\` 为空时先 `git submodule update --init --recursive`。
- 跳过 `installer`（需 NSIS）不影响本地测试；`output\install.bat`
  报 `WeaselSetupx64.exe 不是内部或外部命令` 可忽略，只要最后显示
  操作成功完成、输入法可切换即安装有效。

## 8. librime 的 C4251 warning 可忽略

`segmentation.h` 等报 `warning C4251`（STL 成员在 DLL 导出类里）是
上游祖传 warning，CI 也一样，只要不是 error 就让它编完。
