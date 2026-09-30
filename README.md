# KeyboardEyes

Linux 键盘按键计数程序，使用 libevdev 读取输入，并通过 SQLite 保存累计次数。
采用 C++20、Clang、libc++、CMake 和 Ninja 构建，需要 libevdev 和 SQLite
开发库（SQLite ≥ 3.24）。

## 编译与运行

在项目根目录执行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/hello_world /dev/input/eventX
```

将 `/dev/input/eventX` 替换为目标键盘设备，运行账户需要具备读取该设备的权限。
首次运行后按键，输出示例：

```text
Start listening: Example Keyboard
KEY_A 1
KEY_A 2
```

只统计 press，松开、长按自动重复以及丢失事件后的同步恢复均不计数。
默认数据库为**启动工作目录**下的 `./stats.db`，可通过 `--db` 指定：

```bash
./build/hello_world /dev/input/eventX --db /path/to/stats.db
./build/hello_world --db /path/to/stats.db /dev/input/eventX
```

数据库文件不存在时自动创建，父目录需要预先存在且可写；打开失败会报错退出，
不会自动更换路径。所有设备共用按键累计次数，不区分用户，当前只支持单个采集进程写入。

启动时加载已有累计次数，每次有效 press 保存成功后才打印最新次数。
重启并使用同一数据库即可继续累计。保存失败或计数达到 `INT64_MAX` 时报告错误并退出。
按 Ctrl+C 或发送 SIGTERM 会结束监听并释放设备、数据库等资源；空闲时也能退出。
每次更新已独立提交，退出时无需额外写回。正常信号退出返回 `0`，参数或运行错误返回 `1`。

运行已有三项数据库测试（建表、查询与修改、加载）：

```bash
(cd build && ctest --output-on-failure)
```

源码位于 `src/main.cpp`。默认工具链优先使用 `clang++-23`，其次使用
`clang++`；当前机器已安装 Clang 23、CMake 3.16 和 Ninja。
编译和链接均显式指定 `-stdlib=libc++`，配置阶段检查 C++20 和
`_LIBCPP_VERSION`，并验证 libc++ 可以成功链接。当前机器已安装
`libc++-23-dev` 和 `libc++abi-23-dev`；迁移环境时需要安装与 Clang
配套的 libc++ 和 libc++abi 开发包。

Linux 下可检查可执行文件的标准库依赖：

```bash
ldd build/hello_world
```

输出应包含 `libc++.so.1` 和 `libc++abi.so.1`，不应包含 `libstdc++.so.6`。
如果从此前的 libstdc++ 配置升级，首次配置时额外传入
`-U CMAKE_CXX_FLAGS`，清除旧的编译参数缓存。

## 编辑器与格式化

VS Code 可安装工作区推荐的 clangd 和 CMake Tools 扩展。
工作区已配置本机的 `/usr/bin/clangd-23`，迁移环境时按需修改此路径。
首次构建配置后生成 `build/compile_commands.json`，clangd 会通过
`.clangd` 配置读取它，提供补全和诊断。

格式化源码：

```bash
clang-format-23 -i src/main.cpp
```

添加新的源文件时，将其加入 `CMakeLists.txt` 的 `add_executable` 列表。
