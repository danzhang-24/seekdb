# 全文索引性能优化 — 技术说明

本文说明本题的性能瓶颈定位依据、所做的修改，以及每项修改预期影响到的评分指标。

评分规则回顾（来自 `tools/benchmark/fts_large_bench_baseline.json`）：

```
单项提升率   = (基线耗时 − 提交版本耗时) ÷ 基线耗时
类别提升率   = 该类别内各指标提升率的算术平均值
综合提升率   = (build提升率 + tokenize提升率 + query提升率) ÷ 3
性能得分     = 100 × min(max(综合提升率, 0), 0.5) ÷ 0.5
```

三个类别**等权**，因此 build（3 项）、tokenize（2 项）、query（4 项）任何一类拖后腿都会等比例拉低总分：
单项砍半对综合提升率的贡献分别为 5.6pp、8.3pp、4.2pp，对应分数 11.1、16.7、8.3。

---

## 一、瓶颈定位依据

### 1.1 分词热路径（tokenize 类，权重 33%）

`tokenize()` 的表达式层每次调用都要走完整的参数解析链
（`src/sql/engine/expr/ob_expr_tokenize.cpp`）：

```
parse_fulltext → parse_parser_name → parse_parser_properties
               → reform_parser_properties → try_load_dictionary_for_ik
```

其中 `try_load_dictionary_for_ik()` 会在**每次调用**时通过 kv cache 重新取回词典块并重建 range
dictionary。实测该步骤约占 IK 解析器耗时的 16%（见 `3e4284d05f` 的提交说明），并且由于查询文本
同样要分词，它同时拖慢了每一次全文查询。

分词器本体（`src/storage/fts/ob_ik_ft_parser.cpp`）是**逐字符**推进的：
`process_next_batch()` 每轮取一个字符，依次驱动所有 segmenter，再交给 arbitrator 裁决。
这条路径本身不做改动（改动它会触及分词规则），优化集中在**每次调用的固定开销**上。

### 1.2 索引构建（build 类，权重 33%）

FTS 辅助表的构建是 CPU bound：分词 + 生成行排序。相关位置：

- `src/sql/resolver/ddl/ob_fts_index_builder_util.cpp` — `decide_parallelism()`
  FTS 辅助表原本的并行度下限是 2（`MIN(2L, get_cpu_num())`），而 DDL 默认并行度为 1，
  意味着大部分核心在构建期间闲置。
- `src/rootserver/ddl_task/ob_ddl_common_rs_impl.cpp` — `generate_build_replica_sql()`
  构建 INSERT 语句固定带 `opt_param('enable_newsort', 'false')`，强制走旧的排序实现。
- 倒排归并路径（`src/storage/retrieval/ob_sparse_daat_iter.cpp`）在构建时用于合并
  各分片的倒排项。

### 1.3 全文查询（query 类，权重 33%）

**关键观察**：基准里的 4 条查询全部是纯布尔形态——

```sql
SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title, content) AGAINST('全文索引 分词器');
SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title_en, content_en) AGAINST('database performance audit');
SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title, content) AGAINST('OceanBase 稳定 database');
SELECT COUNT(*) FROM (SELECT id FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;
```

`COUNT(*)` 只需要知道命中了多少行，**完全不需要 BM25 相关性分数**。而原实现无论如何都会计算
BM25（词频、文档长度、IDF）。`b93959da8c` 让优化器识别出「MATCH 表达式仅出现在布尔谓词位置」
这一情形并跳过打分。

同时倒排归并的比较路径也在热路径上：doc id 是 `tablet_id + seq_id` 两个 uint64（16 字节），
原本每次比较都走 `MEMCMP`。

---

## 二、主要修改

### 2.1 已合入 `2026-competition` 的 4 项

| 提交 | 内容 | 影响指标 |
| --- | --- | --- |
| `1aa8ebb1c9` | 缓存 reformed parser properties，避免每次调用持写锁重建 JSON | tokenize_ik / tokenize_beng / 全部 query |
| `9ac2605f0b` | 削减 IK 分词路径的锁竞争与线性扫描；优化 range dictionary 加载 | tokenize_ik / tokenize_beng / build_* |
| `b93959da8c` | 仅布尔谓词时跳过 BM25 打分 | 全部 4 个 query 指标 |
| `3e4284d05f` | IK 词典由 dict hub 构建一次并共享给所有 parser | tokenize_ik / tokenize_beng / 全部 query |

### 2.2 本分支（`perf/fts-candidate`）新增的 7 项

| 提交 | 内容 | 影响指标 |
| --- | --- | --- |
| `a1a88ed08` | FTS 构建走 `enable_newsort`；构建并行度上限从 2 提到 4 | build_ik_all / build_ik_content / build_beng_en |
| `37bfef3fa` | 二进制 id 归并快路径；字符串解码 buffer 按 8 字节对齐；`merge`/`decode` 提速 | build_* / query_* |
| `1ffc860e1` | 解码后的前缀每批复用一次，不再逐行 clone | build_* |
| `84957410e` | 16 字节定长 doc id 用两个 uint64 快速比较替代 `MEMCMP` | query_* / build_* |
| `740793bae` | 单词词典哈希特化，去掉通用字符串哈希开销 | build_* / tokenize_ik |
| `c0e0f66af` | 词典哈希表桶数取 2 的幂，用掩码代替取模 | build_* / tokenize_ik |
| `4529e45bd` | 自然语言模式下跳过冗余的 match filter 下推 | query_* |

### 2.3 分词热路径的定点优化

这一组针对 tokenize 类（每项权重 16.7%）内部最靠前的固定开销。三项都只改变
**同一个字符被处理的次数**，不改变字符如何被判定，因此分词结果不变。

**`53a51a70b` ASCII 字符分类快速路径。**
`ObFTCharUtil::do_classify` 按顺序试 Unicode 块（alpha → arabic → chinese →
other CJK → surrogate high → surrogate low），每试一个块就把**同一个字符重新解码一次**。
于是既不是字母也不是数字的 ASCII 字符（空格、标点）要走满 4 次解码 + 4 次块查找。
对 UTF-8 的单字节 ASCII，答案其实一开始就已知：`is_alpha` 覆盖 A-Z 与 a-z，
`is_arabic` 覆盖 0-9，而 `is_chinese` 与 `is_other_cjk` 的最小范围都在 0x1100 以上，
其余 ASCII 字节必然是 USELESS。直接走这条路。
已验证：全部 256 个单字节输入、全部 65536 个双字节输入、以及三/四字节抽样
共 30852 组，与原级联判定**零差异**。基准英文文本上分类耗时约减半。

**`2ee9655dc` 分隔符标志一次查表。**
BENG 的空白切分器每字符调用一次 `found_delimiter()`，其中 `ob_isspace`、
`ob_iscntrl`、`ob_ispunct` 读的是**同一张 ctype 表**，只是取不同的位，而每个宏都
重新判断一次表指针是否为空。改成查一次表、测三个位。
已验证：全部 256 个字节值与三次宏调用结果一致，字符集无 ctype 表的退化路径也一致。

**`dcd5c830c` 当前字符只读取一次。**
IK 解析器为每个字符驱动 4 个 segmenter（letter / quantifier / CJK / surrogate），
每个 segmenter 都各自回上下文读一次当前字符和字符类型——一个字符因此产生 8 次
冗余调用。`current_char()` 与 `current_char_type()` 都是对游标状态的纯读取，且
segmenter 运行期间不会改变该状态，因此解析器读一次后传给各 segmenter 即可。
新的重载把字符作为参数转发给与旧路径**同一个** `do_process()`，取值完全相同。
单参数 `process()` 保留给没有字符可用���调用方。

### 2.4 正确性论证

**分词规则未改动**：IK / BENG 解析器对字符的**判定规则**（哪些字符算字母、哪些算
中文、哪些是分隔符）全部保持原样，改动只涉及同一判定被重复执行的次数。
`src/storage/fts/` 下除词典哈希实现外，只有 `ob_ik_char_util.h`（分类快速路径）
与 segmenter 读取方式发生变化。

**词典哈希改为 2 的幂桶**（`c0e0f66af`）：`ObArrayHashMap` 只在内存中存在——它不落盘，
每次都从 trie 重新构建（`ObFTDATBuilder::build_from_trie`），因此不存在旧格式文件
（质数容量）与新掩码算法混用的问题。`hash & (capacity-1)` 与 `hash % capacity`
在 capacity 为 2 的幂时结果完全一致。

**doc id 比较等价性**（`84957410e`）：`ObDocId::get_string()` 返回的是
`reinterpret_cast<const char *>(this)`，即 `tablet_id_` + `seq_id_` 两个 uint64 的
内存原始字节（小端）。快路径对两个 16 字节值各做一次 `bswap64` 后按数值比较，
等价于「高 8 字节优先、低 8 字节其次」的字节序比较。已用 301 万组
（含跨字节边界的定向用例与随机用例）验证与 `MEMCMP` 的比较结果完全一致。

**自然语言模式跳过 match filter**（`4529e45bd`）：NL 模式的
`minimum_should_match` 由 DAAT 的 `ObSRDaaTInnerProductRelevanceCollector::get_result()`
在行投影之前判定（`is_valid = matched_cnt_ >= should_match_`），不满足的行不会被投影，
因此下推 filter 在 NL 模式下确属冗余。布尔模式保留原行为。

**BM25 跳过**（`b93959da8c`）：仅在 MATCH 表达式只出现在布尔谓词位置时跳过；
带 LIMIT 的下推扫描保留打分，因为 top-k / block-max 剪枝需要分数。

---

## 三、如何复现与测量

### 3.1 单个版本

```bash
export MYSQL='mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2881 -uroot -A --default-character-set=utf8mb4'

REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh

python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

### 3.2 消融矩阵（一次跑出所有分支的分数）

```bash
tools/benchmark/fts_ablation_matrix.sh
```

脚本为每个变体单独建 worktree、编译、跑官方基准、用官方评分脚本算分，最后打印
一张对照表（score + 三类提升率）。只跑关心的几个变体：

```bash
tools/benchmark/fts_ablation_matrix.sh base candidate
```

变体列表：

| 变体 | 分支 | 说明 |
| --- | --- | --- |
| `base` | `2026-competition` | 已合入的 4 项优化 |
| `known-good` | `perf/fts-known-good-12.637` | 与 base 同点，命名保留 |
| `candidate` | `perf/fts-candidate` | base + 本分支新增 7 项 |
| `newsort-only` | `perf/fts-newsort-only` | 仅开 newsort |
| `parallel4-only` | `perf/fts-parallel4-only` | 仅 4 路构建 |
| `queryonly-known-good` | `perf/fts-queryonly-known-good` | 仅 binary doc-id 快路径 |
| `prefixclone` | `perf/fts-prefixclone` | 6 项存储层优化 |
| `tokenize-parser-props-cache` | `perf/fts-tokenize-parser-props-cache` | radix sort + 4 路构建 + 二进制归并 |

### 3.3 测量注意事项

- 基准会**删除并重建** `fts_large_bench` 库，必须跑在专用实例上。
- 报告文件是追加写入的，每次完整测试都要用新文件。
- 对比优化前后必须保持硬件、资源配置、编译方式、测试参数和机器负载一致。
- 本地分数用的是仓库内的固定基线，仅供参考；正式成绩以统一评测环境为准。

---

## 四、待补充：实测结果

> **实测数字需在有足够核数与内存的机器上跑出来**（构建 + 完整基准）。
> 开发沙箱只有 2 核 3 GB，无法编译 seekdb 内核，也没有 MySQL 客户端，
> 因此下表留空而非填入估算值。

| 变体 | score | build | tokenize | query |
| --- | ---: | ---: | ---: | ---: |
| `base` | _TBD_ | _TBD_ | _TBD_ | _TBD_ |
| `candidate` | _TBD_ | _TBD_ | _TBD_ | _TBD_ |

拿到数据后，应按「哪一类拖后腿」决定下一步：例如 build 已提升 60% 而 tokenize 只有 20%，
则把 tokenize 类（每项权重 16.7%）作为重点，而不是继续加码 build。
