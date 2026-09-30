# KeyboardEyes

Linux 键盘按键计数程序，使用 libevdev 读取输入，并通过 SQLite 保存累计次数。
采用 C++20、Clang、libc++、CMake 和 Ninja 构建，需要 libevdev 和 SQLite
开发库（SQLite ≥ 3.24）。项目同时提供独立的只读程序 `KeyboardEyesViewer`，
定时查询数据库并在终端完整打印按键计数。默认启用测试，配置时还需要 Python 3
（含标准库 sqlite3）；仅构建程序时可以指定 `-DBUILD_TESTING=OFF`。

## 编译与运行

在项目根目录执行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/KeyboardEyes /dev/input/eventX
```

将 `/dev/input/eventX` 替换为目标键盘设备，运行账户需要具备读取该设备的权限。
默认仅向 stderr 输出启动、连接变化、停止和错误日志，不打印逐次按键计数。
需要查看每次按键时添加 `--verbose`，参数可放在设备路径前后，也可重复指定：

```bash
./build/KeyboardEyes --verbose /dev/input/eventX --db ./stats.db
```

首次运行时的状态日志示例（stderr）：

```text
Started; database: ./stats.db
Start listening: Example Keyboard
```

开启 `--verbose` 后，按键计数输出示例（stdout）：

```text
KEY_A 1
KEY_A 2
```

只统计 press，松开、长按自动重复以及丢失事件后的同步恢复均不计数。
默认数据库为**启动工作目录**下的 `./stats.db`，可通过 `--db` 指定：

```bash
./build/KeyboardEyes /dev/input/eventX --db /path/to/stats.db
./build/KeyboardEyes --db /path/to/stats.db /dev/input/eventX
```

数据库文件不存在时自动创建，父目录需要预先存在且可写；打开失败会报错退出，
不会自动更换数据库路径。所有设备共用按键累计次数，不区分用户。

启动时加载已有累计次数，每次有效 press 都保存到数据库；仅在开启 `--verbose` 时，
保存成功后才打印最新次数。日志开关不影响计数和持久化。
重启并使用同一数据库即可继续累计。保存失败或计数达到 `INT64_MAX` 时报告错误并退出。
按 Ctrl+C 或发送 SIGTERM 会结束监听并释放设备、数据库等资源；空闲时也能退出。
每次更新已独立提交，退出时无需额外写回。正常信号退出返回 `0`，参数或运行错误返回 `1`。

## 数据库单实例限制

程序在初始化 SQLite 前对数据库文件获取非阻塞独占 `flock` 锁。同一数据库已有采集进程时，
第二个进程立即报告路径和占用错误，以状态码 `1` 退出，不等待、不打开键盘。
不同数据库可以分别启动。相对路径、绝对路径、符号链接和硬链接指向同一文件时也会互斥。

锁在设备断开、等待重连期间持续保留，数据库关闭后才释放；正常退出、异常退出或进程被终止后，
锁随描述符关闭自动释放，不需要删除锁文件，也不会删除数据库。

该机制面向 Linux 本地文件系统上的普通数据库文件，不支持 `:memory:` 或 SQLite URI。
运行期间不要替换数据库文件或更改路径指向。锁是采集进程之间的协作约定，不阻止普通 SQLite
工具查询，但查询仍受 SQLite 自身事务锁约束；当前未配置 WAL 或锁等待超时。

## 定时读取与终端展示

构建后运行独立程序，不需要访问键盘设备：

```bash
./build/KeyboardEyesViewer --db ./stats.db --interval 5
./build/KeyboardEyesViewer --help
```

`--db` 默认为**启动工作目录**下的 `./stats.db`，`--interval` 默认为 5 秒，
支持 1 到 2147483 的正整数秒；参数顺序任意，但不允许重复指定。
数据库必须是已经存在的本地普通文件，不支持 `:memory:` 或 SQLite URI。
程序以只读方式打开文件，不创建数据库、不初始化表、不修改日志模式，也不申请采集端的
`flock` 锁。运行期间不要替换数据库文件或更改路径指向。

启动后立即读取一次，每轮结束后等待指定间隔，再读取最新已提交的数据。
每次成功都向 stdout 追加 `key_counts` 的全部三列和全部记录，按 `id` 升序排列，
即使数据没有变化也会完整打印并刷新输出。时间戳使用 UTC，例如：

```text
Table: key_counts | Read at: 2026-09-30T09:00:00Z
| id | key_name  | press_count |
| 1  | KEY_A     | 12          |
| 2  | KEY_ENTER | 7           |
Rows: 2
```

空表仍输出表头和 `Rows: 0`；不打印 `sqlite_sequence` 等内部表。
计数保留完整的 64 位整数精度。异常按键名中的控制字符、竖线转义为 `\xHH`，
反斜杠转义为 `\\`，防止打乱表格，不截断字段。

查询先将完整数据读入内存并释放查询及读事务，再格式化、打印；查询失败不会打印半张表。
读取端设置 200 毫秒的锁等待；遇到 SQLite busy/locked 错误时向 stderr 报告，
等待下个周期重试。文件不存在、权限不足、缺失所需列、字段类型错误、负数计数或数据库损坏
等错误会报告原因并以状态码 `1` 退出。标准输出写入失败也会报错退出。
按 Ctrl+C 或发送 SIGTERM 正常退出返回 `0`，无需等到轮询间隔结束。
启动、停止和错误日志均输出至 stderr。

**并发限制：现有采集程序没有配置 SQLite 锁等待。** 读取程序已尽量缩短读锁时间，
但在当前回滚日志模式下，仍可能与采集端写入竞争锁，导致采集端报错退出。
读取端的锁等待只能处理自身读取失败，无法改变采集端行为；本次未修改采集程序。

读取 systemd 服务数据库时需要明确指定路径，并使用有权读取文件及遍历父目录的账户：

```bash
./build/KeyboardEyesViewer --db /var/lib/keyboardeyes/stats.db --interval 5
```

默认服务数据目录属于 `keyboardeyes:keyboardeyes`，权限为 0750，普通账户可能无法访问。
如果有 sudo 授权，并且服务账户能够访问构建产物，可使用：

```bash
sudo -u keyboardeyes ./build/KeyboardEyesViewer --db /var/lib/keyboardeyes/stats.db
```

程序不会自动提升权限或更改文件权限。现有部署脚本仍只安装和启动采集程序，
不会为读取程序创建 systemd 服务。

读取程序代码位于 `src/viewer/`：`DatabaseReader::read_snapshot()` 返回包含读取时间及
全部记录的独立快照，`print_snapshot(snapshot, ostream)` 负责终端展示，`main.cpp`
负责参数、轮询和信号处理。未来可视化可接收同一快照，替换展示层。

## 设备断开与重连

启动时设备尚未连接，或运行期间设备断开，程序会保持运行，每隔 2 秒重新打开指定路径，
直到连接成功。等待期间仍可通过 Ctrl+C 或 SIGTERM 正常退出。
权限不足、无效输入设备及数据库错误仍会报错退出，不会无限重试。

建议指定 `/dev/input/by-id/` 或 `/dev/input/by-path/` 下存在的稳定设备链接，例如：

```bash
./build/KeyboardEyes /dev/input/by-id/你的键盘-event-kbd --db ./stats.db
```

每次重连都会重新解析指定路径，不自动搜索其他键盘。直接使用 `/dev/input/eventX` 时，
设备编号可能变化，无法保证该路径重新出现后仍对应原键盘。

断开期间保留数据库连接和内存计数，重连后继续累计；无法补记断开期间的输入，
也不会将设备初始化状态或同步恢复事件算作新按键。同一等待阶段中，相同失败原因只记录一次，
原因变化或连接恢复时再输出日志。

## 测试

运行三项原有数据库测试（建表、查询与修改、加载）及读取程序集成测试：

```bash
(cd build && ctest --output-on-failure)
```

读取程序测试通过 Python 3 启动真实进程，使用临时 SQLite 数据库，覆盖完整输出、
定时更新、空表、只读行为、无效参数和数据、权限错误、锁竞争恢复、采集端 `flock`、
信号退出和输出失败；不会访问服务数据库或真实键盘。root 运行时跳过权限拒绝用例。
也可以单独执行：

```bash
python3 tests/viewer_test.py --viewer ./build/KeyboardEyesViewer -v
```

部署脚本的隔离测试需要 Python 3，通过临时目录及模拟系统命令验证安装和回滚，
不会在当前主机创建账户、安装或启动服务：

```bash
python3 tests/deploy_test.py
```

## systemd 一键部署

在项目中执行以下命令即可构建 Release、运行 CTest、安装并启动采集系统服务，
同时启用开机自启动。替换为自己的键盘稳定路径：

```bash
./scripts/deploy.sh --device /dev/input/by-id/你的键盘-event-kbd
```

脚本可从任意工作目录执行，支持 `--help`。普通用户执行时先以当前用户身份构建，
系统安装阶段由 sudo 提权；也支持 root 直接执行。脚本不自动安装依赖软件包。
需要正在运行的 systemd、`input` 组，以及 Bash、CMake、Ninja、CTest、pkg-config、
Clang/libc++、libevdev/SQLite 开发库和 sudo（非 root）、useradd、runuser、flock、
Python 3（含 sqlite3）、ldd、systemd-analyze 等系统工具。服务配置以本机 systemd 245 为验证基线。

Release 产物独立保存在 `build/deploy`，不改变现有调试构建。部署位置固定为：

| 内容 | 路径 |
| --- | --- |
| 可执行程序 | `/usr/local/bin/KeyboardEyes` |
| 服务配置 | `/etc/systemd/system/keyboardeyes.service` |
| 设备路径配置 | `/etc/default/keyboardeyes` |
| 服务数据库 | `/var/lib/keyboardeyes/stats.db` |

脚本创建禁止登录的 `keyboardeyes` 系统账户和同名组，服务通过 `input` 附加组读取设备。
若设备当前存在，会验证该账户是否能读取；设备尚未连接时允许部署，程序会等待重连。
脚本不修改设备权限、不自动挑选键盘，也不启用逐次按键日志。
systemd 管理数据目录（0750），服务 umask 为 0027；异常退出后间隔 5 秒重启，
60 秒内最多允许 5 次启动。

再次执行同一命令即可更新程序及设备配置：先构建和检查，再停止旧服务并更新。
服务数据库始终保留。安装或启动检查失败时，脚本尝试恢复旧程序、配置和原有启用/运行状态，
输出服务状态及近期日志；恢复失败会报告保留的备份目录。已创建的账户和数据不会自动删除。
并行部署会被拒绝；不属于脚本管理的同名安装文件、账户冲突或现有服务 drop-in 会报错，
不会直接覆盖。

服务启动后会观察至少 3 秒，确认进程持续 active、未重启且已启用开机自启动。
这只验证启动稳定性；实际键盘计数和真实插拔仍需验收。

```bash
systemctl status keyboardeyes
sudo systemctl restart keyboardeyes
sudo systemctl stop keyboardeyes
journalctl -u keyboardeyes -f
sudo systemctl disable --now keyboardeyes   # 停止并取消开机自启，保留数据
```

首次部署**不会导入项目中的 `stats.db`**。若要保留旧计数，先停止服务和使用旧数据库的
所有采集进程，备份服务库，再手动复制旧库到 `/var/lib/keyboardeyes/stats.db`，
将所有者设为 `keyboardeyes:keyboardeyes`、权限设为 0640，然后重新启动服务。
服务运行期间不要覆盖数据库。启动次数达到限流后，先查看日志并修复原因，再执行：

```bash
sudo systemctl reset-failed keyboardeyes
sudo systemctl start keyboardeyes
```

完成部署后确认关闭终端不影响服务、计数能够保存、键盘断开重连能够恢复；
在方便重启机器时再验证开机自启。部署脚本不会自动重启机器。

## 工具链说明

采集入口位于 `src/main.cpp`，独立读取程序位于 `src/viewer/`。
默认工具链优先使用 `clang++-23`，其次使用
`clang++`；当前机器已安装 Clang 23、CMake 3.16 和 Ninja。
编译和链接均显式指定 `-stdlib=libc++`，配置阶段检查 C++20 和
`_LIBCPP_VERSION`，并验证 libc++ 可以成功链接。当前机器已安装
`libc++-23-dev` 和 `libc++abi-23-dev`；迁移环境时需要安装与 Clang
配套的 libc++ 和 libc++abi 开发包。

Linux 下可检查可执行文件的标准库依赖：

```bash
ldd build/KeyboardEyes
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

添加新的源文件时，将其加入 `CMakeLists.txt` 中对应目标的 `add_executable` 或
`add_library` 列表。`KeyboardEyesViewer` 链接独立的 `keyboardeyes_reader` 静态库和
SQLite，不链接 libevdev 或采集端写入库；整体项目配置仍需要 libevdev 开发包。
