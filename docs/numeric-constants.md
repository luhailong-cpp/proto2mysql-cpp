# 数字常量的来源

本轮清理手写库代码、CLI、测试中的隐藏策略与协议数字。已存在的 SDK/protoc 常量直接复用，项目自己的策略集中命名，不再为相同含义重复维护编号。

| 类别 | 处理方式 |
|---|---|
| Protobuf options | 使用生成的 `k*FieldNumber`，保持 `.proto` 为 wire 编号唯一声明 |
| MySQL 错误码、默认端口 | 使用 `ER_*`、`CR_*`、`MYSQL_PORT` |
| 批量行数、条件保存轮数 | `database_limits.h` 的 `kBatchSize`、`kConditionalSaveMaxRounds` |
| 键长度、索引预算、标识符长度、TiDB shard 上限 | 同一共享限额头；最大字符键长度由字节预算与编码宽度推导 |
| DATETIME 精度、时间范围和单位换算 | 共享精度、Protobuf `TimeUtil` 和 `std::chrono`，格式位置由日期形状字符串推导 |
| P2MC 缓存信封和 varint | `cache_wire` 命名版本、header size、payload bits/mask、分组边界；最大字段号取 Protobuf 定义 |
| UTF-8、hex、FNV、名称截断 | `text_encoding.h` 单一实现，明确各协议常量，删除重复编码算法 |
| 类型容量和 schema 转换结果 | 命名枚举、固定宽度类型 `sizeof`，保留有名的 MySQL MEDIUMINT 特殊存储宽度 |
| information_schema 查询行布局 | 命名列枚举替代裸位置下标，投影长度使用枚举终值或自动推导 |
| 锁等待、锁返回状态、版本递增 | 有名策略常量或状态枚举；锁等待通过参数绑定 |
| CLI 路径容量、重试、退出码、Windows 转义 | 文件内语义常量；字符串切片使用实际前缀长度 |
| 客户端结果缓冲、next-result 哨兵 | 有名常量；保留 SDK 已公开的结果枚举 |
| CI 的 readiness 参数和测试连接端口 | 具名参数及服务环境变量；版本、服务端口声明保持配置来源 |

保留的数字属于声明或测试数据：`.proto` 字段编号、protoc 生成文件、生成的 Unicode 映射、SQL 中的 `SELECT 1` / `DEFAULT 0` 等语法值、普通循环索引与 0/1 判断，以及测试明确给出的输入/预期结果和独立 wire golden。它们不替换成没有含义的 `kZero` / `kOne`；独立 golden 继续验证格式兼容，避免测试与实现共用常量后同时漂移。
