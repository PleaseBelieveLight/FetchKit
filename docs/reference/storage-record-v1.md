# 存储记录 v1

[文档中心](../README.md)

2026-10-27 固定 M2 字节格式。参考实现为 `src/storage/record.cpp`，独立编码的黄金样本为
[record-v1.hex](../../tests/fixtures/record-v1.hex)。整数均无符号小端；不依赖 C/C++ 结构布局。

## 封装

| 偏移/长度 | 内容                                                                               |
|-----------|------------------------------------------------------------------------------------|
| 0 / 8     | `46 4b 52 45 43 30 31 00`（`FKREC01\0`）                                           |
| 8 / 4     | 格式版本 1                                                                         |
| 12 / 4    | payload 字节数，最大 65536                                                         |
| 16 / 4    | IEEE CRC32，由平台 zlib 对头部 0～15 字节及 payload 连续计算，初值 `crc32(0, ...)` |
| 20 / N    | TLV payload；实际文件长度必须恰为 20+N                                             |

解析先检查文件大小再分配；最大文件 65556 字节。CRC 检测偶然损坏，不验证攻击者篡改。没有
MAC/加密，目录不得供不可信写入者共享。

每 TLV 为 `tag:u16, flags:u16, length:u32, value:length`。flags=1 表示必须理解，flags=0 为可选；其他位拒绝。
已知字段不重复，未知重复 tag 也拒绝。未知可选字段跳过，未知必需字段及未来格式版本返回
UnsupportedMetadata 并保留隔离文件， 不尝试降级写入。支持的记录重写时会省略未知可选字段，因此它们不能承载旧实现必须保留的事务语义。

## 字段

| tag | 内容               | 约束                                                   |
|-----|--------------------|--------------------------------------------------------|
| 1   | 持久 task ID       | 64 字节小写 SHA-256 hex；必须匹配身份及记录文件名      |
| 2   | namespace          | 非空 UTF-8，最多 256 字节                              |
| 3   | resource_key       | 非空 UTF-8，最多 1024 字节                             |
| 4   | version            | 非空 UTF-8，最多 256 字节                              |
| 5   | representation_key | UTF-8，最多 1024 字节，可空                            |
| 6   | address_reference  | UTF-8，最多 2048 字节，可空，不保存 URL/headers        |
| 7   | expected_sha256    | 空或 64 字节小写 hex                                   |
| 8   | require_digest     | u64，0 或 1                                            |
| 9   | committed_offset   | u64，最多 INT64_MAX，不超过已知总长                    |
| 10  | generation         | u64，1～UINT64_MAX-1；溢出前拒绝再运行                 |
| 11  | strong_etag        | 空或最多 1024 字节的强 ETag，按不透明响应字节保留      |
| 12  | state              | u64，当前 C 头定义的状态值 0～9；活动状态扫描为 Paused |
| 13  | integrity          | u64：0 未完成，1 TransportComplete，2 DigestVerified   |
| 14  | error              | u64，当前定义的结构化错误值                            |
| 15  | expected_length    | 可选 u64；缺字段表示未知，值 0 表示已知空文件          |
| 16  | total_length       | 可选 u64；不能与 expected_length 冲突                  |

1～14 必须出现，即使字符串为空；数字字段恰好 8 字节。UTF-8 字段拒绝 NUL、过长编码、代理码点和非法序列。
Publishing/Completed 必须有 total=committed，integrity 必须与摘要要求一致；其他非法/溢出状态隔离。
CRC、ETag 和 ID 哈希均不作为可信内容摘要。身份为四个 UTF-8 字段以 NUL 分隔串联，不追加最终分隔符。

M3 的 pin、访问时间、过期与重试预算尚未分配字段；实现其恢复语义时再新增可兼容字段或升级版本，不写无效占位状态。
目录路径由 ID 派生，记录不编码远端文件名或任意文件路径。Cancelled 是先提交再清理的 tombstone，扫描不会复活下载。

修改此格式时必须保留旧黄金样本、增加新版本解析与故障恢复测试。未来版本隔离后不会被老实现自动覆盖；已有未知对象的同身份创建返回
Busy。
