# proto2mysql-cpp

将 Protobuf 消息映射到 MySQL 表的 C++ 库。保留原有 `pbmysql::Database` 接口，并参照 Go 版 `proto2mysql` 补齐参数化 SQL、完整主键保存、版本栅栏写入、字段号迁移和缓存信封。

## 构建与测试

依赖：C++17 编译器、CMake 3.20+、Protobuf（`protoc` 与运行库版本匹配）、MySQL/MariaDB 客户端开发库。运行测试还需要 GoogleTest。较新的 Protobuf/Abseil 如果要求 C++20，请配置 `-DCMAKE_CXX_STANDARD=20`。

Ubuntu/Debian 示例：

```sh
sudo apt-get install cmake g++ libprotobuf-dev protobuf-compiler default-libmysqlclient-dev libgtest-dev
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Windows 可使用 Visual Studio 的 CMake 支持，配置 `CMAKE_PREFIX_PATH`、`MYSQL_INCLUDE_DIR`、`MYSQL_LIBRARY` 指向已安装的依赖。消费者使用 `add_subdirectory` 后链接 `pbmysql::proto2mysql`。关闭测试可设置 `-DBUILD_TESTING=OFF`。旧 `test/mysql_client.sln` 依赖原业务工程；本仓库的独立构建入口是 CMake，不需要 muduo、Boost 或业务工程的 `common_proto`、`src/util`。

默认测试不连接数据库。集成测试必须显式提供 `PBMYSQL_TEST_HOST`，会在指定数据库中创建并清理独立命名的测试表：

```sh
PBMYSQL_TEST_HOST=127.0.0.1 PBMYSQL_TEST_PORT=3306 \
PBMYSQL_TEST_USER=root PBMYSQL_TEST_PASSWORD=your-password \
PBMYSQL_TEST_DATABASE=proto2mysql_test \
ctest --test-dir build --output-on-failure
```

## Proto 配置

新项目可以直接复用 Go 版的 `proto/proto2mysql_option.proto`：

```proto
syntax = "proto3";
import "proto2mysql_option.proto";
option (proto2mysql.db) = true;

message Player {
  option (proto2mysql.table_name) = "player";
  option (proto2mysql.primary_key) = "id";
  option (proto2mysql.unique_key) = "account";
  uint64 id = 1;
  string account = 2 [(proto2mysql.max_length) = 191];
  uint64 gold = 3;
  uint64 owner_epoch = 4;
  uint64 save_seq = 5;
  bytes data = 6;
}
message Players { repeated Player items = 1; }
```

```sh
protoc -I proto -I your-protos --cpp_out=generated your-protos/player.proto
```

旧 `db_base.proto` 中 `OptionTableName`、`OptionPrimaryKey`、`OptionIndex`、`OptionUniqueKey` 和 `OptionAutoIncrementKey` 继续可用。现代选项优先，代码里的 `TableOptions` 再覆盖它们；`std::optional` 区分“未覆盖”和“显式清空”。现代消息必须显式声明主键才能使用按主键的操作；旧消息保留首字段作为默认主键的行为。

## 使用

```cpp
#include "src/mysql_wrapper/database.h"
#include "player.pb.h"

pbmysql::Database db;
db.Connect("127.0.0.1", "user", "password", "game", 3306);
db.SetDbName("game");
db.RegisterTable(Player::default_instance());
// 也可：db.RegisterAllTables(Player::descriptor()->file());
db.SyncAllTables();

Player row;
row.set_id(1);
row.set_account("account-1");
row.set_gold(100);
db.Save(row);

Player loaded;
loaded.set_id(1);
if (db.FindOneByPrimaryKey(loaded)) {
    db.IncrByPrimaryKey(loaded, "gold", 10);
}

db.Transaction([&](pbmysql::Database& tx) {
    if (!tx.FindOneByPrimaryKeyForUpdate(loaded)) return false;
    loaded.set_gold(0);
    tx.UpdateFieldsByPrimaryKey(loaded, {"gold"});
    return true; // false 或抛异常回滚
});
```

`Database` 持有一个连接。每个并发工作线程使用独立实例，不要共享同一连接并发调用；不要在 `Transaction` 回调内手动执行事务控制 SQL。自动重连被关闭，避免断线后事务悄悄消失。数据库错误抛 `MysqlError`（`value()` 保留原 MySQL 错误码），参数错误抛 `std::invalid_argument`，状态错误抛 `std::logic_error`。

`Save` 按**完整主键**先整行 UPDATE、缺失时 INSERT；仅写本描述符认识的列，零值照写。备用唯一键冲突抛 1062，不能更新另一条记录。`InsertOnDupUpdate` 是 `Save` 的历史别名；需要显式的 MySQL ODKU 时使用下面的 `SQLBuilder::Upsert`。`InsertIgnore` 仅把 1062 转成 `false`，其他错误继续抛出。批量操作在显式事务外不保证原子性。

`Update` 只写已赋值的非主键字段。proto3 隐式标量零值不会出现在该集合中；清零应使用 `optional` 或 `UpdateFieldsByPrimaryKey`。列表查询要求输出消息恰好有一个对应类型的 repeated message 字段。

## 条件保存

```cpp
pbmysql::SaveOptions options;
options.fence_fields = {"owner_epoch", "save_seq"};
row.set_owner_epoch(5);
row.set_save_seq(12);
auto result = db.SaveConditional(row, options);
if (result.outcome == pbmysql::SaveOutcome::Rejected) {
    // result.stored 按 fence_fields 的顺序给出数据库内栅栏。
}
```

条件与写入在同一条 UPDATE 内完成，按栅栏元组的字典序判断库内值是否更旧。栅栏只接受非主键、非 nullable、非自增的 uint32/uint64 列，严格模式拒绝全零栅栏。`allow_equal=true` 允许相同版本再次写入。`preserve_fields` 只在更新时保留指定列，插入时照常写入。

结果为 `Updated`、`Inserted`、`Unchanged` 或 `Rejected`，`Applied()` 对前三种为真。判定查询使用 `FOR UPDATE` current read，最多尝试两轮。无法收敛时抛 `ConditionalSaveError`，通过 `kind` 区分 `Race` 与 `MatchMismatch`，`stored` 保留最后读到的栅栏。事务死锁等数据库错误仍按原错误码上抛，调用者应重试整个事务。

同一张栅栏表的写入方应统一使用条件保存。普通 Save/Update 可以任意重写版本列；物理删除后，旧消息会被视为新行插入。

## 只生成 SQL

```cpp
pbmysql::SQLBuilder b(Player::default_instance()); // 无需连接或注册
// 也可：auto b = db.Builder(row); // 复用注册时的配置
auto stmt = b.UpsertAdd(row, {"gold"});
// stmt.sql 含 ?；stmt.args 是 variant<nullptr_t,int64_t,uint64_t,double,string>
db.Execute(stmt); // prepared statement；string 保留二进制长度
```

| 分类 | 接口 |
|---|---|
| 插入 | Insert / InsertSetFields / InsertIgnore / InsertIgnoreSetFields / Replace |
| 批量 | BatchInsert / BatchInsertIgnore / BatchReplace / BatchUpsert / BatchUpsertWith |
| 冲突合并 | Upsert / UpsertAdd / UpsertKeepOld / UpsertWith |
| 查询 | SelectByPK / SelectByPKForUpdate / SelectWhere / SelectColumns / SelectByKVIn / SelectByPKIn |
| 聚合 | Count / Exists / ExistsByPKForUpdate |
| 更新 | UpdateByPK / UpdateByPKIf / UpdateFieldsByPK / UpdateWhere / UpdateAssignsByPK / UpdateAssignsWhere |
| 原子计数 | IncrByPK / DecrByPKIfEnough |
| 删除 | DeleteByPK / DeleteByPKIf / DeleteWhere / DeleteWhereLimit / DeleteByKVIn / DeleteByPKIn |
| 赋值 | SetCol / AddCol / SubCol / SetColExpr / SetNew / AddNew / MinNew / MaxNew / SetNewIfZero / KeepOld |

字段名必须存在于消息里，统一引用。UPDATE/DELETE 的空条件、空 IN、重复赋值和非数值列的数值运算在发 SQL 前被拒绝。`where`、`order_by`、guard 和表达式是调用方提供的可信 SQL 片段；业务取值必须放入 `args`。DML 不带末尾分号。`Database` 的旧 Where 方法继续接受常量条件，业务参数用新增 `*ByWhereWithArgs`。直接 `Execute` builder 语句属于底层接口，缓存失效由调用者负责。

`QueryOptions` 支持 `order_by`、`limit`、`offset`、`for_update`；应用需要排序稳定时应显式指定 ORDER BY。FOR UPDATE 必须放在事务内才能持续持锁。

## 类型和表结构演进

- int32/int64/uint32/uint64、bool、enum、float/double 对应 MySQL 数值类型；拒绝 NaN/Inf。与当前 Go 版一致，不支持 sint/fixed/sfixed 六种标量编码及真实 oneof。
- 普通 string → MEDIUMTEXT，bytes、嵌套消息、map/repeated → MEDIUMBLOB。嵌套消息存自身 PB；map/repeated 存只包含该字段的同类型消息 PB，兼容 Go 的容器编码。
- Timestamp → UTC DATETIME(6)，缺失值为 NULL；读取会清除旧字段，解析失败保留调用方原消息。
- 主键/唯一键里的 string → VARCHAR(N) + utf8mb4_0900_bin，bytes → VARBINARY(N)，默认 N=191，完整索引，验证 UTF-8、长度与 3072B 索引预算。字符串键需要支持该排序规则的 MySQL 8.0+ 或相应 TiDB；MariaDB 客户端库可连接 MySQL，但这里不承诺 MariaDB 服务端兼容。
- 建表写入 `COMMENT 'pb:N'`。迁移按同名列或字段号识别改名，不删列、不收窄已有更宽类型；signed/unsigned、NULL、DEFAULT、自增、键列排序规则和声明索引冲突会先拒绝，再由业务显式迁移。
- 缺失列/二级索引可以补齐；未声明的已有列/索引保留。`MessageTable::SetExpandOnly(true)` 限制为新增结构，禁止改名和修改已有列。
- 支持 TiDB NONCLUSTERED、SHARD_ROW_ID_BITS、PRE_SPLIT_REGIONS、AUTO_ID_CACHE=1 扩展注释。
- `GenerateMigrationSql` 可先检查 SQL；`CreateOrUpdateTable` 用 session advisory lock 串行化本库的 schema 写入，要求后端支持 GET_LOCK/RELEASE_LOCK。释放锁状态不明时会关闭连接，避免遗留持锁会话。DDL 会隐式提交，因此事务内拒绝运行 schema 同步。

旧 C++ string/bytes 列与当前映射可能不同；同步发现不安全差异会报错。部署前使用迁移 SQL 和备份核对数据。旧 TEXT 前缀唯一键或不区分大小写的键列须显式迁移，C++ 版不生成 Go 版完整的影子表重建指导。

## 缓存

实现 `pbmysql::Cache` 的 Get/Set/Del 后调用 `EnableCache`。按主键读取使用 cache-aside，写入后删除；事务绕过缓存，提交后批量失效，回滚不删除，缓存异常降级到数据库。按任意 WHERE 的修改无法确定受影响主键，需业务调用 `InvalidateCache`。不要长期使用无过期时间的缓存。

值使用 Go 兼容的 P2MC v1 信封，验证写入方字段集合包含读取方字段集合，避免滚动升级读到缺列数据；损坏/裸 protobuf 视为 miss。键的表名和各主键分量会转义 `%`、`:`，避免复合键碰撞。跨语言升级时应统一编码版本，并清理旧格式缓存。

## 离线 proto2sql

```sh
protoc -I proto -I your-protos --include_imports \
  --descriptor_set_out=schema.pb your-protos/player.proto
./build/proto2sql --descriptor-set schema.pb --out schema.sql
```

扫描 db=true 文件中的现代 table_name 消息（含嵌套类型），以及旧 OptionTableName 消息；输出确定顺序的建表 SQL。输出前完成所有配置校验，错误返回非零退出码。运行时也提供 `WriteCreateTableSql`、`DumpCreateTableSqlFile`、`WriteMigrationSql` 与 `DumpMigrationSqlFile`。

## 与 Go 的接口边界

数据库行为与 SQLBuilder 按当前 Go 版对齐，C++ 的连接管理、异常和容器采用原生接口。Go 特有的 GORM、context 取消以及全局描述符自动枚举没有移植；C++ 自动注册显式接收 FileDescriptor，离线工具接收 descriptor set。字符串键的人工迁移方案、TiDB 集群行为需要在目标环境另行核对。

## 本次验证

Linux 容器内，GCC 13.4 / CMake 3.25 / MySQL 8.4.13：Protobuf 3.21.12 + C++17 与 Protobuf 35.1 + C++20 均为 61/61 测试通过，包含真实 MySQL 的并发栅栏、唯一键冲突、事务回滚、二进制读写、在线迁移以及离线 CLI。Release 的无测试构建和安装通过。原生 Windows 编译和 TiDB 集群未实测。
