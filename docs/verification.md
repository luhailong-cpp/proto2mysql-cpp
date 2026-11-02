# 验证记录

对照 Go 源码提交 `e81a1e79105864610232139a57a28df273b866eb`，排除 Go 专用 GORM/context；原 Go 工作区未修改。C++ 基础移植为 `0e21503`，数据库行为补齐和跨平台验证为 `5219080`；本记录保留各阶段的实际验证范围。

## 数字常量清理回归（2026-11-02）

清理范围和保留数字的依据见 [数字常量的来源](numeric-constants.md)。最终源码重新构建后，每套完整 CTest 共 132 项，比前一阶段新增 8 项：UTF-8/hex/FNV/标识符边界、浮点极值、Timestamp 边界，以及 legacy 批量 SQL 的表名关键字回归。

| 环境 | 后端 | PASS | SKIP | FAIL |
|---|---|---:|---:|---:|
| Linux GCC 13.4 / Protobuf 35.1 / C++20 | MySQL 8.4.13 | 131 | 1 | 0 |
| Linux GCC 13.4 / Protobuf 3.21.12 / C++17 | MySQL 8.4.13 | 131 | 1 | 0 |
| Windows MSVC 19.51 / Protobuf 35.1 / C++20 | MySQL 8.4.13 | 131 | 1 | 0 |

每组跳过的 1 项均为双节点专用用例。Windows 和 Linux 安装后的消费者各为 3 PASS / 0 SKIP / 0 FAIL，覆盖 direct、static 注册保留和 shared/module 配置期拒绝。

Linux Release / `BUILD_TESTING=OFF` 的库、CLI 构建及安装通过；Linux Release 和 Windows Debug 安装目录均确认包含新增的两个共享头文件。

首次验证期间源码修正与编译任务发生重叠，旧对象被增量构建复用，新增 SQL 回归用例检出了该问题。强制重编相关源文件后，以上三套均重新完整执行；首次日志另存为 `pre-final-snapshot`，最终结果不与旧运行拼接。

本轮日志位于本地 `verification/constants/`。以下 TiDB 和 Go 参考库结果属于数据库行为补齐阶段。

## 数据库行为补齐的完整测试矩阵

每组完整 CTest 套件共 124 项。跳过项只涉及该后端不适用的能力，不能把跳过计作执行成功。

| 环境 | 后端 | PASS | SKIP | FAIL |
|---|---|---:|---:|---:|
| Linux GCC 13.4 / Protobuf 35.1 / C++20 | MySQL 8.4.13 | 123 | 1 | 0 |
| Linux GCC 13.4 / Protobuf 3.21.12 / C++17 | MySQL 8.4.13 | 123 | 1 | 0 |
| Windows MSVC 19.51 / Protobuf 35.1 / C++20 | MySQL 8.4.13 | 123 | 1 | 0 |
| Linux GCC 13.4 / Protobuf 35.1 / C++20 | TiDB 8.5.2 单节点 | 122 | 2 | 0 |
| Linux GCC 13.4 / Protobuf 35.1 / C++20 | PD + TiKV + 两个 TiDB 8.5.2 节点 | 123 | 1 | 0 |

MySQL 的 1 项跳过为双节点专用用例；单节点 TiDB 跳过双节点用例和不支持的 stored procedure 用例；真实 TiDB 集群执行跨节点用例，仅跳过 stored procedure 能力检查。其余适用测试均执行。

Windows 完整测试首次暴露 relocation smoke 复制 EXE 时缺少原运行 DLL 搜索路径。修复仅对子进程保留 DLL 搜索目录后，`proto2sql_smoke` 1/1 独立复验通过；数据库、codec 和并发实现没有因该测试环境修复变更。上表 Windows 结果由完整运行与该项针对性复验合并，保留两份原始日志供核对。

## 覆盖范围

- 全部 DML 家族、typed uint64/FLOAT 谓词、字段子集、guard、空条件拒绝、批量预检和完整复合主键 tuple IN。
- Save 精确主键/备用唯一键分类、条件保存四种 outcome、严格/相等/保留列、跨版本新增列保留。
- 多实例服务端竞争、同实例共享连接线程安全、事务跨线程隔离、回滚/提交/异常清理、结果计数和缓存失效。
- 自动 generated descriptor 注册、静态链接保留、动态 descriptor source 生命周期、Unicode 等价标识符、空表名跳过。
- map/repeated/嵌套 wire、nullable/default/optional presence、Timestamp 多格式、失败原子性、NaN/Inf/非法类型拒绝。
- 字段号改名保数据、signed/NULL/default/索引漂移、ExpandOnly、两阶段补主键/自增、legacy unique 与影子表迁移。
- 迁移方案在沙箱表真实执行：保宽、额外索引、自增水位、外键/触发器/孤儿列提示。
- Windows UTF-8 来源、descriptor、输出路径；源文件/descriptor CLI、DROP、排序、目录导出、失败回滚和安装资源查找。

## 构建和安装交付

- Linux Release / BUILD_TESTING=OFF：库和 CLI 构建、安装成功。
- Linux、Windows 安装后的消费者均为 3 PASS：可执行文件自动注册、static 目标未引用生成类型时仍保留注册、shared/module 目标配置期明确拒绝。
- Windows 安装包含找到的 libmysql.dll，已安装 CLI 在不添加 MySQL SDK PATH 时可以启动；中文路径和安装目录内 proto 资源查找验证通过。
- 显式导出 Threads 依赖，公开 CMake package 可通过 find_package 使用；原始 SDK 头文件/库依赖仍按正常安装配置。

## Go 参考库

使用同一轮隔离后端运行 `go test -count=1 ./...`：MySQL 8.4 与 TiDB 双节点均通过；独立 `tools/proto2sql` module 的全部测试通过。Go 端后端选择用 `PROTO2MYSQL_INTEGRATION=1`、`PROTO2MYSQL_TEST_DSN` 和 TiDB 的 `PROTO2MYSQL_TEST_DSN2`，未改动源码或测试配置文件。

## 复现

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
PBMYSQL_TEST_HOST=127.0.0.1 PBMYSQL_TEST_PORT=33384 \
PBMYSQL_TEST_USER=root PBMYSQL_TEST_PASSWORD=proto2mysql_test \
PBMYSQL_TEST_DATABASE=proto2mysql_test \
ctest --test-dir build --output-on-failure
```

`test/docker-compose.yml` 提供隔离 MySQL、TiDB 单节点及 PD/TiKV/双 TiDB 拓扑。集群测试额外设置 `PBMYSQL_TEST_HOST2`、`PBMYSQL_TEST_PORT2`；不同编译矩阵使用同版本配套 protoc/libprotobuf。最新 Protobuf/Abseil 需要时配置 `-DCMAKE_CXX_STANDARD=20`。

构建和真库测试的详细日志放在本地 `verification/`（已忽略提交）；CI 的 `.github/workflows/cpp.yml` 复现 MySQL、TiDB 单节点/双节点及安装消费者测试。
