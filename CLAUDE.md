# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目简介

Dstore 是一个可独立编译、独立测试的存储引擎组件（最初从 openGauss / GaussDB 中剥离出来）。最终产物是 `libdstore.so` / `libdstore.a` 以及若干命令行调试工具（`pagedump`、`waldump`、`htablookup`、`whatiserrcode` 等）。

README 仅有中文版；`interface/` 目录下是对外暴露的 C++ 公共头文件，供调用方链接使用。

## 环境准备（任何编译动作之前必须完成）

构建系统不会从 `PATH` 查找编译器或第三方库 —— 全部通过 `LOCAL_LIB_PATH` 与一份固定版本的 GCC 7.3 工具链解析。

每次新开 shell 都要执行 `source buildenv` —— 它会自动把 `BUILD_ROOT` 解析为脚本所在目录（基于 `${BASH_SOURCE[0]}`，必须用 `source` 引入而非 `bash` 执行），并导出 `LOCAL_LIB_PATH`、`CC`/`CXX`（固定指向 `$LOCAL_LIB_PATH/buildtools/gcc7.3/gcc/bin/{gcc,g++}`）以及 lz4 / cjson / gtest / 华为 securec 的库搜索路径。

`LOCAL_LIB_PATH` 必须指向同级的 `local_libs/` 目录，结构如下：

```
local_libs/
├── buildtools/gcc7.3/{gcc,gmp,isl,mpc,mpfr}/
├── secure/{include,lib}/    # 华为 securec，v3.0.9
├── lz4/{include,lib}/       # v1.10.0
├── cjson/{include,lib}/     # v1.7.17
└── gtest/{include,lib}/     # v1.10.0
```

版本必须严格匹配 —— `scripts/common.sh` 仅接受 gcc 7.3 或 10.3。

## 编译

必须先编译 `utils/` —— `src/CMakeLists.txt` 链接依赖 `utils/output/lib/libgsutils.so` 中的 `gsutils`：

```bash
cd utils && bash build.sh -m release   # 生成 utils/output/lib/libgsutils.so
cd ..    && bash build.sh -m release   # 生成 output/lib/libdstore.{so,a}
```

`build.sh` 参数说明：
- `-m {release|debug|memcheck|coverage}` — 编译类型（设置 `DEBUG_TYPE` / `CMAKE_BUILD_TYPE`）。
- `-tm {ut|fuzz|perf|tpcc|lcov}` — 切换 `ENABLE_UT`/`ENABLE_FUZZ`/`ENABLE_TPCC`/`ENABLE_LCOV` 等 cmake 开关。
- `-co "<额外的 cmake 选项>"` — 原样透传给 cmake。
- `clean` — 清空 `tmp_build/` 与 `output/`。

构建产物分别落到 `tmp_build/`（cmake 构建目录）和 `output/`（安装前缀）。每次执行 `build.sh` 都会重建这两个目录 —— 顶层不支持增量构建；增量迭代请直接进入 `tmp_build/` 跑 `make`。

## 测试

测试代码位于 `tests/` 下（`unittest/`、`tpcctest/`），需要打开 `ENABLE_UT=ON` / `ENABLE_TPCC=ON`。两种运行方式：

**直接调用脚本**（`tests/build_and_run_ut.sh`）—— 重新构建到 `tmp_build/` 并运行 `unittest` 二进制；`-r false` 跳过重建，`-g <gtest-filter>` 跑指定子集，`-a ON` 打开 ASAN：

```bash
bash tests/build_and_run_ut.sh -t $LOCAL_LIB_PATH -u $(pwd)/utils/output -r true
bash tests/build_and_run_ut.sh -t $LOCAL_LIB_PATH -u $(pwd)/utils/output -r false -g 'UTBtree*.*'
```

**预定义的 cmake 目标**（在 `tests/CMakeLists.txt` 中定义）—— `tmp_build/` 用 `-DENABLE_UT=ON` 配置之后，可以直接调用分组运行器：

```bash
cd tmp_build && cmake .. -DENABLE_UT=ON -DUTILS_PATH=$(pwd)/../utils/output
make run_dstore_ut_all                       # 全部 UT
make run_dstore_buffer_unittest              # 仅缓冲池相关
make run_dstore_index_unittest               # btree level0
make run_dstore_xact_and_lock_unittest       # csn / 事务 / 锁
make run_dstore_ha_unittest                  # wal / pdb 副本 / 逻辑复制 / 恢复
make run_dstore_datamanager_unittest         # heap / 表空间 / 控制文件 / pdb / vfs / catalog
make run_dstore_framework_unittest           # memctx / 线程 / common
make run_dstore_tpcctest                     # 单节点 TPC-C；需要 -DENABLE_TPCC=ON
# ASAN 变体：run_dstore_ut_asan、run_dstore_tpcctest_asan
```

完整的分组过滤器列表（如 `ut_buffer_filter1`）见 `tests/CMakeLists.txt` —— 进一步用 `--gtest_filter` 收窄范围时可参考。

## 代码架构

### 目录结构

- `src/` — 实现代码，按存储引擎子系统切分（一个目录一个职责：`buffer/`、`catalog/`、`heap/`、`index/`、`lock/`、`transaction/`、`undo/`、`wal/`、`page/`、`fsm/`、`tablespace/`、`logical_replication/`、PDB 副本（位于 `cdb/`）、`framework/`、`recovery/` 等）。
- `include/` — 内部头文件，与 `src/` 子系统目录一一对应，仅供 dstore 内部使用。
- `interface/` — **对外**公开的头文件，目录划分与子系统一致。`src/<subsys>/` 下的 `*_interface.cpp` 是对接公共头的实现层 —— 修改公共 API 时必须同步两侧。
- `tools/` — 独立诊断二进制（`pagedump`、`waldump`、`htablookup`、`whatiserrcode`、`buflookup`、`concurrency_test`），链接存储引擎主体。
- `tests/unittest/` — gtest 用例；`tests/tpcctest/` — 内嵌的 TPC-C 测试框架；`tests/utilities/` — 测试公用 fixture。
- `utils/` — 独立编译的支撑库（`libgsutils.so`），提供内存上下文、线程、VFS、日志等基础设施，被 dstore 复用。
- `cmake/` — 构建辅助；`cmake/build_options.cmake` 是编译开关、sanitizer 模式、特性开关的集中入口。
- `scripts/common.sh`、`scripts/compile.sh` — 由 `build.sh` source 使用，请勿直接调用。

### 子系统边界（"大图景"）

引擎由一组协作的存储子系统组成，每个子系统在 `interface/<name>/` 中提供公共门面，并在 `src/<name>/` 中给出 `*_interface.cpp` 适配层：

- **framework** — 进程生命周期（`dstore_instance`）、线程池与 CPU 自动绑定（`dstore_thread*`）、VFS 适配（`dstore_vfs_adapter`、`dstore_dr_vfs_adapter`）、并行执行框架、性能计数器、PDB（可插拔数据库）句柄。这是其他所有模块挂载的入口。
- **buffer / page / fsm** — 缓冲池、页面格式、空闲空间管理。
- **heap / index / tuple / catalog / systable** — 表存储、btree 索引、元组格式、系统目录。
- **transaction / lock / undo** — 基于 CSN 的 MVCC、锁管理器、undo 日志。
- **wal / recovery / flashback / logical_replication / pdbreplica** — 预写日志、崩溃恢复、闪回、逻辑解码、副本复制。PDB 副本机制的源码实际位于 `cdb/`，对外名称仍叫 `pdbreplica/`。
- **tablespace / control / port** — 物理存储布局、控制文件、操作系统适配层。

跟踪代码路径时，先从对应公共头匹配的 `*_interface.cpp` 入手 —— 它们是经过整理的入口；其余都是内部细节。

### 自动生成的文件（不要手改、不要提交）

`src/CMakeLists.txt` 在 configure 阶段会生成三类文件，它们在 `git status` 中显示为未追踪，且每次 cmake 都会重新生成：

- `src/common/instrument/wait_event/wait_state.{h,cpp}` ← `perl generate-wait_state_types.pl wait_state_names.txt`
  - 随后被复制到 `interface/framework/dstore_wait_state.h`。
- `src/common/instrument/wait_event/io_event.{h,cpp}` ← `perl generate-io_event_types.pl io_event_names.txt`
  - 随后被复制到 `interface/framework/dstore_io_event.h`。
- `include/config.h` ← `cmake/config.h.in` 经 `configure_file()` 生成。

要新增 wait state 或 IO event，请编辑相应的 `*_names.txt` 后重新跑 cmake。

### 编译模式矩阵（cmake/build_options.cmake）

- `CMAKE_BUILD_TYPE=debug` → `-O0 -ggdb`，开启断言（`DSTORE_USE_ASSERT_CHECKING`）。
- `CMAKE_BUILD_TYPE=release` → `-O2 -g3`，hidden visibility，启用 hotpatch、gc-sections。
- `MODE=ASAN` → `ENABLE_MEMORY_CHECK` → `-fsanitize=address,leak,undefined`，去掉 `-fstack-protector` 和 `-Werror`。
- `MODE=TSAN` → `ENABLE_TSAN_CHECK` → `-fsanitize=thread`，链接 `libtsan.a`。
- `ENABLE_UT=ON` → 切换为 `-fvisibility=default`、去掉 `-Werror`、二进制加 `-fPIC`（便于以测试库形式重链）、打开 `LOCK_DEBUG` 与 `ENABLE_FAULT_INJECTION`，并引入 gmock/gtest。
- `ENABLE_LCOV=ON` → 增加 `-fprofile-arcs -ftest-coverage`，链接 `gcov`。

还有几个编译期开关是 cmake 阶段从环境变量读取（不是命令行 flag）：`GS_USE_NEW_CONFIG_METHOD`、`GS_ON_DEMAND_PAGE_REPLAY`、`GS_RECOVERY_PERF_COLLECTION`、`GS_BUFFERPOOL_DEBUG`、`GS_BUFFERPOOL_SYNC_LOCK`、`GS_LOCK_DEBUG`。需要时在调用 `build.sh` 前 export 即可。

### 链接关系

`libdstore.so` 链接 `securec lz4 cjson z gsutils dl`。其中 `gsutils` 来自 `utils/output/` —— 若链接报缺 `gs*` 符号，多半是 utils 没构建或已过期。
