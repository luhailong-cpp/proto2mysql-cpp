# Go ↔ C++ SQL、codec 与离线工具对照

对照基准是 `D:/luyuan/proto2mysql` 当前源码，而非历史发布说明。运行时数据库接口与结构同步的对照见 `runtime-parity.md`。C++ 返回 `Statement{sql,args}`，验证失败抛 `std::invalid_argument`；执行失败保留 MySQL 错误码。上下文和 GORM 属于 Go 专用集成。

## SQLBuilder

下表方法在 C++ `pbmysql::SQLBuilder` 中使用同名接口。Go 的可变参数在 C++ 中用 `StringVector` / `std::vector<Assign>`；构造器既接受 `MessageTable`，也接受消息和可选 `TableOptions`。

| Go 方法 | C++ 行为与验收 |
|---|---|
| `TableName`, `Table`, `CreateTable`, `PrimaryKeyWhere` | 对应表名、映射、DDL、完整主键 WHERE 片段和带类型参数；主键零值合法；无主键拒绝 |
| `Insert`, `InsertSetFields` | 全列 / presence 列子集；空 presence 拒绝；optional 零值保留 |
| `InsertIgnore`, `InsertIgnoreSetFields` | 使用 no-op ODKU，不能用会吞截断错误的 `INSERT IGNORE` |
| `Replace` | 显式 `REPLACE INTO`，其删除再插入语义不等同运行时 `Save` |
| `BatchInsert`, `BatchInsertIgnore`, `BatchReplace` | 多行 VALUES；空批次、NULL 元素、混合 descriptor、超过 1000 行预检拒绝 |
| `Upsert`, `BatchUpsert` | 默认覆盖全部非主键列；可以显式指定列；没有可赋值列拒绝 |
| `UpsertAdd` | 必须显式点名数值标量列，repeated/map、bool、enum、文本和 BLOB 拒绝 |
| `UpsertKeepOld` | 完整插入 + `pk=pk`；必须有主键 |
| `UpsertWith`, `BatchUpsertWith` | 参数按 INSERT → 赋值表达式顺序排列 |
| `SelectByPK`, `SelectByPKForUpdate` | 全主键查询，不额外添加 LIMIT；后者 `FOR UPDATE` |
| `SelectWhere`, `SelectColumns` | 空条件 `1=1`；空列集合回退全列；支持 ORDER BY、LIMIT、OFFSET、FOR UPDATE |
| `SelectByKVIn`, `SelectByPKIn` | 按值数量展开占位符；空集合拒绝；PK 版本拒绝联合主键 |
| `Count`, `Exists`, `ExistsByPKForUpdate` | 对应 COUNT / LIMIT 1 / 完整主键锁定存在性查询 |
| `UpdateByPK`, `UpdateByPKIf` | 按 presence 写入，包括已赋值主键；后者增加 guard |
| `UpdateFieldsByPK` | 指定列即使为 0、false、空字符串也写；不被不参与 SQL 的其他唯一键值阻挡 |
| `UpdateWhere`, `UpdateAssignsByPK`, `UpdateAssignsWhere` | 仅 mutation 拒绝空条件；参数先 SET 后 WHERE；重复 Assign 保留顺序 |
| `IncrByPK`, `DecrByPKIfEnough` | 数值标量运算；扣减要求 delta > 0，守卫 `col >= delta` |
| `DeleteByPK`, `DeleteByPKIf`, `DeleteWhere` | 主键 / guard / 条件删除，拒绝空 mutation WHERE |
| `DeleteWhereLimit` | 正数 LIMIT，保留原始 ORDER BY，适合分批清理 |
| `DeleteByKVIn`, `DeleteByPKIn` | 与 SELECT IN 相同的 typed 数值正规化与空集合规则 |

全部 DML 不带结尾分号。标识符验证字段存在并用反引号转义。OFFSET 只在 LIMIT > 0 时输出；非正 LIMIT 不限制结果。

`SetCol`, `AddCol`, `SubCol`, `SetColExpr`, `SetNew`, `AddNew`, `MinNew`, `MaxNew`, `SetNewIfZero`, `KeepOld` 均已实现。数值赋值既验证列类型，也拒绝非数值文本、NULL、NaN、Inf 操作数。重复赋值是 MySQL 合法的顺序表达式，保留顺序而不去重。

`NormalizeComparisonValue(field,value)` 及 builder 的 `ComparisonValue(column,value)` / `NormalizeComparisonValue(column,value)` 将数值文本正规化为带类型参数，避免 BIGINT/UNSIGNED 经 DOUBLE 比较损失 2^53 以上精度；FLOAT 谓词先恢复 float32，再绑定 double。BLOB、集合、嵌套消息及 DATETIME 谓词保持调用者原始值，不做 parse/reserialize。

与 Go 相比保留的安全改进：guard 两侧加括号，避免裸 `OR` 脱离主键限制；空白或可选 `WHERE` 前缀也正规化；`VALUES()` Assign 用于普通 UPDATE 时拒绝。WHERE、ORDER BY 和自定义表达式仍是可信 SQL 片段，业务值放进 args。

## 低层兼容 SQL 与运行时 Save 的区别

Go `MessageTable` 的以下方法映射到 C++ SQLBuilder 的同名 `Statement` 方法；legacy literal 方法复用同一实现：

| 方法 | 冲突合并行为 |
|---|---|
| `GetSaveSQLWithArgs`, `GetBatchSaveSQLWithArgs` | 每个非主键列 `IF(完整PK身份一致, VALUES(col), col)`，覆盖已知整行 |
| `GetInsertOnDupUpdateSQLWithArgs` | 相同身份守卫，仅更新 presence 非主键列；值参数在 INSERT 参数后 |
| `GetInsertOnDupKeyForPrimaryKeyWithArgs` | `pk=pk`，不把备用唯一键行的主键改成入参主键 |

这些低层 ODKU helper 碰到其他主键的备用 UNIQUE 行时只能 no-op，无法可靠报出重复。运行时 `Database::Save` / `SaveConditional` 采用 UPDATE → INSERT 与 current read 分类，可以保留另一行并返回 1062；不能用低层 ODKU 替代该流程。

## Value codec

| 类型 / 场景 | C++ 对齐 |
|---|---|
| int32/int64/uint32/uint64 | 完整带符号或无符号范围；SQL 参数保持 typed 64 位 |
| float/double | 写入拒绝 NaN/Inf；float32 写入最短十进制，比较绑定其精确 double 容器 |
| bool/enum | bool 写 1/0；enum 写整数；读取 bool 支持 Go ParseBool 的常见大小写别名 |
| string/bytes | 文本/原始二进制，无 Base64；长度安全，保留 NUL |
| nullable / 空标量读取 | SQL NULL、空串转字段默认值；optional 标量保留已设置 presence；复用消息不会残留旧值 |
| Timestamp | 未设置 / Go 零时间对应 NULL；写 UTC DATETIME(6)，截断到微秒；读取原始 MySQL、DATE、RFC3339 与时区偏移 |
| message | 子消息裸 protobuf 字节；NULL/空值清除 presence |
| repeated / map | 同消息类型 holder 中只保留该字段，以原字段号的 protobuf wire 编码；兼容 Go wire 形态 |
| map 与嵌套 map | deterministic protobuf serialization，防止重复编码导致 UPDATE 与匹配谓词字节不一致 |
| real oneof | DDL/codec 预检拒绝；synthetic optional 放行 |
| sint/fixed/sfixed 六种标量 | 与当前 Go 声明一致，明确拒绝，不静默建错列 |

`ParseRow` 在临时消息上完成整个转换，再替换调用者消息；错误不清空调用者主键。C++ 完整行解析严格校验列数，原始坏数字、越界与非有限浮点值拒绝。这些检查比 Go 的部分行逐字段变更更严格。

与 Go 一致，`nullable` 影响列约束，不把未设置的普通标量自动改写成 SQL NULL；普通标量仍写其 protobuf 默认值。需要显式 NULL 时使用 `SetCol(column, nullptr)`。Timestamp 的未设置值会自动绑定 NULL。

## proto2sql

兼容已有 `--descriptor-set file.pb [--out schema.sql]`，也支持 Go 源文件工作流：

```text
proto2sql -proto a.proto,b.proto -proto-file c.proto -I ./proto -out ./sql
proto2sql -proto a.proto -out ./sql -single schema.sql -drop
```

`-proto`、`-proto-file`、`-I` 可重复；支持 `--` 长形式、`-Ipath` 和 `flag=value`。源文件模式通过外部 protoc 编译，不使用 shell；可通过 `--protoc` 或 `PROTO2SQL_PROTOC` 选择可执行文件。输入目录自动加入搜索路径，拒绝同 basename / import shadowing；只导出显式输入根文件的表，包含嵌套消息。默认不要求文件 db 标记，`--require-db` 可启用过滤。

安装后的 CLI 根据实际可执行文件位置发现 `../share/proto2mysql/proto`，重定位后也能导入自带选项文件；显式 `-I` 仍优先。Windows 命令行及文件 API 保持 Unicode，不依赖系统 ANSI 代码页。

所有表先完整生成、验证及按物理表名排序；Unicode simple-fold 检测等价表名冲突。默认每表一个文件，`-single` 合并；`-drop` 的警告写进生成物。文件名采用跨平台校验，拒绝路径穿越、控制字符、Windows 非法字符、保留设备名及不稳定结尾。

导出先在目标目录内 stage 并关闭所有文件，再保留旧备份、逐个安装；commit point 前失败恢复旧文件，commit 后仅备份清理失败报告 warning。不能保证进程/机器崩溃时跨多个路径原子性。

## 验收清单

- `BuilderFixture.*`：全部 DML 家族、Assign、完整主键、typed IN、guard、offset、空条件、空列回退、批次预检、低层身份守卫、opaque wire、参数顺序和独立所有权。
- `CodecParity.*`：nullable/empty/default presence、复用消息、bool、签名数字、Timestamp 多格式、失败原子性。
- `CodecPreflight.*`：oneof 拒绝且保留调用者状态。
- `CodecDeterministic.*`：128 元素 map/嵌套 map 反向插入与 50 轮 FieldValue/UPDATE/ParseRow 稳定性。
- `proto2sql_smoke`：descriptor/source、CSV/重复/别名、带空格及引号路径、只导出根、db 过滤、DROP banner、排序、Unicode 冲突、保留名、批量写失败保留旧输出。
- 数据库执行、事务、缓存、schema 与跨语言实测由运行时和集成测试覆盖；测试通过状态见该轮实际验证记录，不由本清单推断。
