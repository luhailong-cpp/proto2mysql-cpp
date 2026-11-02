# Go ↔ C++ 数据库行为对照

本轮以 `D:/luyuan/proto2mysql` 当前工作区源码为基准，排除用户明确不需要的 Go 专用 GORM、context 取消与请求超时框架。C++ 以异常、标准容器、受互斥保护的单连接和显式对象生命周期表达相同的数据库操作；不要求函数拼写或 SQL 空格逐字节一致。SQLBuilder、codec 和工具逐项对照见 [api-parity.md](api-parity.md)。

## 运行时接口

| Go 接口/功能 | C++ 对应入口与行为 |
|---|---|
| NewDB / OpenDB / Close | `Database` 构造、`Connect`、`SetDbName`、`Close`；读取服务端 DATABASE()/lower_case_table_names 确认目标库，不偷偷 USE 切库；离线 SetDbName 可用 |
| RegisterTable / TableOption | `RegisterTable(message, TableOptions)`；代码选项覆盖现代 proto 选项，再覆盖兼容旧 C++ 选项；非法定义在 SQL 前拒绝 |
| RegisterAllTables | `RegisterAllTables()` 扫描已登记文件及依赖；`pbmysql_generate_cpp` 为 executable/static 目标自动登记，或 `PBMYSQL_REGISTER_PROTO_FILE(Type)` 一次登记文件；空表名跳过，嵌套继续递归 |
| descriptor-only 接入 | `RegisterDescriptorSet(path)` / `RegisterAllTables(shared_ptr<DescriptorSet>)`；独立 pool，显式输入的新版本不会被已链接旧版本吞掉；DB/mapper/builder 保留来源生命周期 |
| CreateOrUpdateTable / UpdateTableField / SyncAllTables | 同名公开入口；完整元数据预检，按计划逐条执行普通 ALTER 与主键/自增 ALTER；ExpandOnly 从 Database 传到全部映射 |
| IsTableExists / GenerateCreateTableSQLChecked / ValidateTableMessage | 存在性查询与离线检查入口；带原因的异常；历史 GenerateCreateTableSQL 校验失败返回空串 |
| Write/DumpCreateTableSQL | 先全批校验、按物理表名排序，再写 writer；大小写与 Unicode simple-fold 冲突拒绝；文件导出同目录临时文件关闭后替换 |
| Generate/Write/DumpMigrationSQL | 读取线上列、索引、表属性、自增水位、外键、触发器；整批验证后输出；失败保留已有输出 |
| Insert / InsertReturningID / InsertIgnore | 参数化 INSERT；返回完整 unsigned ID；仅 1062 视为未插入，其他错误保留 |
| BatchInsert | 整批同 descriptor/键值/序列化预检后分 1000 行；每个成功批次立即失效对应缓存 |
| Save / InsertOnDupUpdate | 完整 PK UPDATE → INSERT → 1062 时再次 UPDATE → current-read 逐列比较；备用唯一键冲突保留 1062；只写当前描述符认识的列 |
| SaveConditional | 原子字典序栅栏条件、AllowEqual、PreserveFields、四种 outcome；两轮 bounded retry、current read、Stored；Rejected 保留缓存，执行后不确定错误失效 |
| BatchSave | 全批预检后逐行 Save；事务外非原子；逐个成功行失效缓存 |
| Update / UpdateByWhereWithArgs | presence 更新与参数化条件更新；optional 可显式写零；已赋值 PK 与 Go 相同可参与 SET |
| UpdateFieldsByPK / UpdateKVByPK | 指定字段清零或 typed 单字段赋值；C++ 另保留旧长名入口 |
| UpdateIfVersion / UpdateFieldsIfVersion | 数值版本（含 float/double），自动 +1 与版本条件同句；自动字段为空/显式列表为空拒绝；显式列表中的版本名由统一 +1 处理 |
| IncrByPK / DecrByPKIfEnough | 数值列原子运算；扣减必须 delta>0 且 col>=delta，按 changed-row count 返回结果 |
| Delete / DeleteByKV / DeleteByWhereWithArgs | 完整 PK、typed 字段等值或可信参数化条件；空 mutation WHERE 拒绝 |
| BatchDelete | 整批 descriptor/完整 PK 预检，1000 行一批；复合键 tuple IN，前批成功后后批失败仍失效前批缓存 |
| FindOneByPK / FindOneByPKForUpdate | 缓存与锁定读；按完整 PK；锁定便利入口要求处于事务 |
| FindOrCreate | 先读，缺失后 INSERT；并发 1062 保留给调用者，不悄悄覆盖已有行 |
| FindOneByKV / FindOneByWhereWithArgs | 普通单行查询不暗加 LIMIT；多行结果抛 MultipleRowsError，未命中返回 false |
| FindOneWithOptions | 传递 typed WHERE args，强制 LIMIT 1 和 OFFSET 0，支持 ORDER BY / FOR UPDATE |
| FindAll / FindAllByWhereWithArgs / FindMultiByWhereWithArgs | 反射推导 list 元素，无需模板；允许额外 scalar metadata；恰一个 repeated message，拒绝歧义；旧显式模板继续可用 |
| FindMultiByKV / FindAllByKVIn / FindAllByPKIn | 数值文本正规化为 typed int64/uint64/double，不通过 DOUBLE 解释大整数；PK IN 明确拒绝联合键；空列表清空结果 |
| FindAllWithOptions / FindPage / FindPageByCursor | 全部支持 typed WHERE args；参数有效性/offset 溢出检查；cursor 值按字段类型正规化；首页 optional=nullopt |
| Count / CountByWhereWithArgs / Exists / ExistsByPK | 行消息或列表消息均可定位表，条件值参数化 |
| FindMultiByWhereClauses | 全批预检，保留各查询的 WhereArgs；C++ 按顺序执行 prepared statements，避免依赖字符串多语句插值 |
| RunInTransaction / Transaction | 同一个连接执行全部 CRUD，事务回调独占该实例，其他线程等待，嵌套拒绝；false/异常回滚；commit 未知结果保守失效缓存；开始/回滚状态不明关闭连接 |
| EnableCache / CacheKey / InvalidateCache | 注入 Cache，事务绕过，提交后去重失效、回滚不删；支持批量预检后失效 |

API 命名统一使用 `PK`、`KV`；旧 `PrimaryKey` 长名、`Kv` 拼写、`tables()` 和显式模板作为兼容入口保留。未命中使用 bool 而不是 Go 的 `ErrNoRowsFound`，执行错误保留 `MysqlError::value()` / `sql_state()`，多行误匹配、schema 与条件保存有独立错误类型。

## Schema 与手工迁移

- 按列名优先、再按 `pb:N` 字段号认领；一次规划先匹配所有列，拒绝字段号重复占用和意外重新绑定。
- 列名/索引名使用 Unicode simple folding，避免大小写或 Kelvin sign 等等价名漏检；预检保宽后的完整索引预算。
- 不删列、不收窄、不改 signedness；NULL、DEFAULT、AUTO_INCREMENT、键排序规则和索引定义漂移分别分类。
- 现有列补主键，以及新增自增主键列和依赖索引，放到同一条 ALTER；普通列先单独对齐，调用者可以看见分阶段失败。
- Go 与 C++ 共享 `proto2mysql:sync:<FNV-1a32(database)>` 命名的 DB 级 advisory lock。只有明确“不支持 GET_LOCK”的服务器错误允许有提示的无锁降级；超时、NULL、网络和权限错误不能降级。释放状态不明关闭 session。
- 旧 TEXT 前缀键、可空键或旧排序规则返回 `SchemaError`，`Is(LegacyKeyColumn)` 与 `Is(SchemaDrift)` 均可分类；`Report()` 包含 reasons/checks/statements/warnings。
- 唯一键可原地迁移；旧主键生成影子表方案，保留线上宽列、额外可重建索引、自增高水位。外键、触发器、孤儿列和不可恢复索引逐项点名。危险方案只返回供审查，普通 Sync 不执行。

```cpp
try {
    db.CreateOrUpdateTable(row);
} catch (const pbmysql::SchemaError& error) {
    if (error.Is(pbmysql::SchemaErrorCode::LegacyKeyColumn)) {
        const auto& report = error.Report();
        // 先审阅 checks / warnings，再决定在维护窗口执行 statements。
        std::cout << report.ToString();
    }
    throw;
}
```

## 缓存与滚动升级

P2MC v1 信封、字段集合超集判断、二进制/布尔主键文本和 `%`/`:` 转义与 Go 对齐。旧 writer Save 保留数据库新版本专有列；新 reader 不接受少字段旧缓存。按任意 WHERE 的写入由业务手动失效缓存。C++ 在主键本身被数值操作或 KV 更新改变时同时失效旧/新身份，避免遗留新身份缓存。

C++ 保留两项更强的错误原子性：单行/列表解析失败不修改原消息，SQL 查询失败不清空旧列表；注册、导出预检失败不留下半批注册或部分输出。SQLBuilder 拥有映射快照，注册其他表或销毁 Database 后仍可使用；动态 builder 同时持有 descriptor 来源。直接从调用者自己的 dynamic Message 构造 builder 时，调用者仍应持有其 protobuf descriptor/factory。

## 验证入口

`DatabaseSave.*`、`DatabaseQueries.*`、`DatabaseBatch.*`、`DatabaseVersion.*`、`DatabaseCache.*`、`DatabaseTransactions.*`、`DescriptorRegistry.*`、`IdentifierMapping.*`、`SchemaExport.*` 验证行为和错误路径。`MysqlIntegration.*` 覆盖真实 DML/并发/升级/分页，`SchemaIntegration.*` 真正执行沙箱迁移脚本，`MysqlClient*` 覆盖传输层。跨节点用例必须配置 `PBMYSQL_TEST_HOST2` / `PBMYSQL_TEST_PORT2`；TiDB 不支持 stored procedures 时对应能力测试跳过，其余适用测试必须执行。

完整构建矩阵与实际结果记录在 [verification.md](verification.md)；清单本身不替代测试日志。

## 同实例线程安全

正常 Database API（包括 Execute/Query）串行保护连接、注册配置、事务状态、结果计数和缓存队列。事务回调持有连接租约，其他线程不会意外加入未提交事务；多实例仍可同时访问同表，靠数据库栅栏/行锁保证数据正确。ExecuteStatement 返回本次 SQL 的 affected_rows/last_insert_id，避免分开读取全局“上一次结果”时被其他线程插入操作。可变 Tables()/tables() 引用属于配置兼容接口，调用方需初始化时使用或自行同步。不要在事务回调内把同一实例交给其他线程并等待其完成；独立 MysqlClient 仍是一连接由调用方同步。
