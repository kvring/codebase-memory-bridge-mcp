# 跨仓库包依赖桥接 — 设计规格

**日期：** 2026-07-14
**状态：** 设计阶段已确认 → 待写实现计划
**范围：** codebase-memory-mcp 源码 — 将跨仓库智能扩展到 npm 包依赖

## 问题

codebase-memory-mcp 每个仓库建一张独立知识图，两张图之间**默认零边**。唯一的跨仓库机制是 `cross-repo-intelligence` 模式（`pass_cross_repo.c`），它只匹配微服务 **Route / Channel / async topic**，不识别 npm 包导入。

于是前端主项目（TrainCRN）通过 `import { X } from '@ctrip/train_rn_common'` 消费组件库时，两图**断开**：

- 组件库图里有 `X` 的真实定义。
- App 图里有 import 语句和对 `X` 的调用，但都解析不过仓库边界。
- `cross-repo-intelligence` 报 0 条 CROSS_* 边（上一会话在真实 TrainCRN / train_rn_common 上验证过）。

更糟的是，消费方图**当前会把外部包 import 及其调用整个丢弃**（见 §3 使能改动 C），导致连桥接的锚点都没有。

## 目标

为 npm 包依赖做**符号级**跨仓库链接，使得：

1. `trace_path` 能从 App 的业务函数跨过包边界，一路追到组件库里方法的真实实现。
2. 反向影响分析：改了组件库某符号，能查出哪些 App 调用点受影响。

## 约束（brainstorming 阶段拍板）

| 决策 | 选择 |
|---|---|
| 链接精度 | **符号级**（连到真实导出符号） |
| 连哪些边 | **同时** import 语句（`CROSS_IMPORTS`）和调用点（`CROSS_CALLS`）— trace 连续所必需 |
| 生态范围 | v1 **仅 npm**，留 `PkgBridge` 策略接口缝，后续接 Go/Rust/Python |
| 触发 | **扩展现有 `cross-repo-intelligence` 模式**（一条命令同时跑 Route + Package） |
| 方向 | **双向**（两边 DB 都写边；支持反向影响分析） |

## 非目标（v1）

- Go modules / Cargo crates / pyproject / composer / pom 跨仓库链接（留缝，不实现）。
- Cypher `query_graph` 跨库遍历（改动太大；CROSS_* 边在 Cypher 里只作为"本库内边 + props 元数据"可读不可跳）。
- `search_graph` 跨库（只返回本库命中）。
- 重索引后自动失效跨仓库边（v1：任一方重索引后手动重跑桥接）。

## 架构背景（已在源码核实）

- `pass_cross_repo.c` — 现有 CROSS_* pass；`build_cross_props` / `insert_cross_edge` / `emit_cross_route_bidirectional` 定义了存储模式。每条 CROSS_* 边连**同一 DB 内两个真实节点**，跨项目对端只存在 `properties_json` 里（`target_project`/`target_function`/`target_file`）。
- `pass_pkgmap.c` — 把清单**自己的** `name` 字段 → 入口模块 QN（仓内）。`dependencies` 里列的外部依赖**不**注册，所以裸外部 import 不经 pkgmap 解析。
- `pass_calls.c` — `resolve_single_call` 在 callee 解析不到且非 HTTP/async 服务模式时返回 0（丢调用、不建边）。外部库调用被丢。
- `pass_parallel.c:create_imports_edges` — 仅当 `cbm_pipeline_resolve_import_node` 返回非 NULL target 才建 IMPORTS 边。外部包解析到一个 phantom QN，**从未物化为节点**，于是 import 被丢（或经 strategy-3 符号回退误连到仓内同名节点 — 假阳性）。
- `cbm_store_bfs`（store.c）— 单库递归 SQL CTE，按 `e.type IN (...)` 过滤，CTE 内跳不了库。`trace_path` 在一个 store 上跑。
- `extract_imports.c:511` — re-export（`export {x} from './m'`、`export *`）被捕获为 IMPORTS。
- `is_exported` 在 dump 期已序列化进每个节点的 `properties` JSON（`pass_definitions.c` / `pass_parallel.c` 把 `"is_exported":true/false` 写进 `properties_json`，经 `cbm_store_upsert_node` 持久化）。索引后可用 `json_extract(properties,'$.is_exported')` 查。注意：legacy DB 里有非法 JSON 行，裸 `json_extract` 会 abort（store.c:302-307 因此回滚过一个 `json_extract` 部分索引），导出索引查询必须用 `json_valid(properties)` 守卫。

## 设计

### §1 数据模型与边类型

两种新边，镜像现有 CROSS_* 模式（连本库真实节点；跨项目对端放 props；不插 stub）：

| 边 | 正向边（消费方 DB） | 含义 |
|---|---|---|
| `CROSS_IMPORTS` | 消费方 File/Module 节点 → 本库外部包 phantom 节点 | `import {X} from '@pkg'` 连到提供方符号 X |
| `CROSS_CALLS` | 消费方调用点函数 → 本库未解析调用目标节点 | `pkg.X()` 连到提供方方法 X |

**反向边**（提供方 DB）：`CROSS_IMPORTED_BY` / `CROSS_CALLED_BY`，从提供方符号节点到本库包入口/符号节点，props 带消费方调用点 — 在提供方库内即可查"改了某符号→谁受影响"。

**props schema**（扩展 `build_cross_props`）：

```json
{
  "target_project": "Users-chen-train_rn_common",
  "target_symbol": "TrainUBTLogUtil",
  "target_file": "src/utils/ubt.ts",
  "target_qn": "Users-chen-train_rn_common.src.utils.ubt.TrainUBTLogUtil",
  "imported_name": "TrainUBTLogUtil",
  "pkg": "@ctrip/train_rn_common",
  "strategy": "npm_export",
  "confidence": 0.95
}
```

**无 stub 节点。** 消费方图除 `is_external=1` 的 phantom import 节点（使能 C，代表"本文件 import 了外部包"这一真实信息）外，不加新节点。`search_graph` 若觉得 phantom 噪声大，可按 `is_external` 过滤。

**生命周期/幂等：** `delete_cross_edges` 扩展，清 `CROSS_IMPORTS`/`CROSS_CALLS`/`CROSS_IMPORTED_BY`/`CROSS_CALLED_BY` 后重算。重跑不堆叠。

### §2 提供方导出索引

桥接 pass 期间，对每个提供方项目**现场从其持久化 SQLite DB 构建**（内存、跑完即弃，不持久化，保持每 DB 自包含）：

```
1. 读 <provider_root>/package.json → name(@ctrip/train_rn_common)、
   entry(exports["."] | main | module)。
2. entry → 提供方 Module/File 节点 QN（cbm_pipeline_fqn_module，与 pass_pkgmap 同算法）。
3. 从 entry 节点沿 IMPORTS 边 BFS（re-export 链：`export {x} from './y'` 已是 IMPORTS），
   深度上限（如 8）断 re-export 环。
4. 沿途收集 is_exported=true 节点（Function/Class/Variable/Interface/Type）。
5. 索引：(package_name, symbol_name) → {provider_qn, node_id, file, label}。
```

**无需为 `is_exported` 改 schema。** 它已持久化在每个节点的 `properties` JSON 里（见架构背景）。导出索引构建用 `WHERE json_valid(properties) AND json_extract(properties,'$.is_exported')='true'` 查询。`json_valid` 守卫是必须的：legacy DB 里有非法 JSON 行，裸 `json_extract` 会 abort（store.c:302-307 因此回滚过部分索引）。

**re-export 过近似：** re-export 的 IMPORTS 边只带目标模块路径（不带转出哪些名），所以 BFS 会收齐 re-export 目标模块的**所有** is_exported 节点。对 linkage 可接受（宁多连不漏；`export *` 本就暴露全部）。

**同名歧义：** 两个可达模块导出同名符号时，取从 entry 可达的那个（公开 API）而非内部定义；仍冲突取 QN 字典序最小并降 `confidence`。罕见；记日志。

**入口缺失：** `exports`/`main` 缺失或入口节点 QN 找不到 → 退化为包入口级链接（包名→任意模块），降 `confidence`。绝不中断 pass。

### §3 消费方锚点识别

#### 使能改动 B：IMPORTS 边记录具名导入符号
增强 TS/JS 提取器，IMPORTS 边 props 从 `{"local_name":"..."}` 扩成
`{"local_name":"...", "imported_names":["TrainUBTLogUtil","Colors"]}`。
否则消费方不知道一条 import 语句绑定了哪些符号，符号级 `CROSS_IMPORTS` 无从谈起。

#### 使能改动 C：外部包 import 物化为 phantom 节点 + 边
在 `create_imports_edges`（及共享的 `cbm_pipeline_resolve_import_node` 回退路径）里，当 import 解析到外部包（非相对、非别名、不在本地 pkgmap）时，**upsert 一个 phantom Module 节点**（QN=`<project>.<module_path>`，`is_external=1`）并建 IMPORTS 边到它 — 取代当前的丢弃（或 strategy-3 假阳性）。这与 `pass_cross_repo` 给外部库合成 route 节点（`create_svc_route_node`）同思路，给桥接一个确定性消费方锚点。当前的丢弃是上一会话 0 边结果的**根因**。

#### CROSS_IMPORTS 锚点
有了 B + C + §2 导出索引：扫消费方 DB 的 IMPORTS 边，挑目标是 external phantom 的，读 props 里 `imported_names`，逐个匹配提供方导出索引 → 发 `CROSS_IMPORTS`（消费方 File 节点 → 本库 phantom，props 带提供方符号）。

#### CROSS_CALLS 锚点
调用边被 `pass_calls.c` 丢了（没锚点）。桥接 pass **自己重提**：对每个有 bridged IMPORTS 边的消费方文件，跑 `cbm_extract_file` 取它的 calls；对 callee 匹配提供方导出符号的调用（含 `obj.method`，其中 obj 是已桥接 import 的具名库符号），从消费方调用点函数合成 `CROSS_CALLS` 到本库未解析调用目标（若无调用目标节点则到 phantom），props 带提供方方法。

### §4 查询侧跨库跳

只建边不够 — `trace_path` / `get_code_snippet` 得学会顺跨项目指针走。这是 v1 风险最高的部分。

#### 4.1 trace 要认 CROSS_* 边类型
`resolve_trace_edge_types` 当前不含 CROSS_*。经新 `mode="cross_repo"` 加入 `CROSS_IMPORTS`/`CROSS_CALLS`（及反向类型）（倾向 opt-in，避免污染普通 trace 结果）。

#### 4.2 多 store 缝合 BFS
单条 SQL CTE 跳不了库。两段式缝合：

```
1. 在起始 store 跑 cbm_store_bfs（含 CROSS_* 边类型）到本库可达边界。
2. 扫边界上带 target_project 的 CROSS_* 边：
   - resolve_store(target_project) → 打开目标 store（缓存）。
   - cbm_store_find_node_by_qn(target_qn) 在目标 store 查节点。
   - 从该节点再跑 cbm_store_bfs（同边类型集，深度续算），
     结果打 cross_project=true + target_project/target_symbol。
3. 跨库递归，但有总跨库跳预算（如 3）+ 跨库 visited(qn) 集合防环/防指数爆炸。
   超预算则截断并标 cross_depth_capped（绝不静默）。
```

server 级**已开 store 缓存**（按 project 名）避免反复 open。

#### 4.3 get_code_snippet 跨库
`get_code_snippet(qualified_name)`：解析 QN 的项目前缀（`<project>.<path>.<name>`）；若 != 当前 project，按前缀 `resolve_store` 切库再取 snippet。让 agent 在 trace 到对端符号后能拉到真实源码。

#### 4.4 scope 卡边
v1 只改 `trace_path` + `get_code_snippet`。`query_graph`（Cypher）和 `search_graph` 不跨库；CROSS_* 边表现为"本库内边 + props 元数据"（可读、Cypher 不可跳）— 文档注明。

### §5 触发、集成、双向、生命周期

**触发：** 扩展 mcp.c 的 `handle_cross_repo_mode` — 现有 Route/Channel 匹配跑完后，调新函数 `cbm_cross_repo_package_bridge`。结果 JSON 加 `cross_import_edges` / `cross_call_edges` 字段。

**桥接函数：**
```c
cbm_cross_repo_result_t cbm_cross_repo_package_bridge(
    const char *project, const char **target_projects, int target_count);
```
对每个 target：读 package.json `name`；若消费方 DB 有指向它的 external phantom，建导出索引，匹配具名 import + 重提 calls，双向写边。

**双向：** 正向（消费方 DB）`CROSS_IMPORTS`/`CROSS_CALLS`；反向（提供方 DB）`CROSS_IMPORTED_BY`/`CROSS_CALLED_BY` — 镜像 `emit_cross_route_bidirectional`。

**幂等：** `delete_cross_edges` 扩展清四种新类型；每次重算。

**不变式保持：** 每 DB 仍自包含；跨项目信息在 props。唯一新节点是 `is_external=1` 的 phantom（代表真实外部 import 事实）。

### §6 测试与错误处理

#### 测试矩阵
| 层 | 测试 | harness |
|---|---|---|
| is_exported 查询 | `json_extract(properties,'$.is_exported')='true'` + `json_valid` 守卫返回导出定义；非法 JSON 行被跳过不 abort | `test_store_arch` 风格 |
| 使能 B | IMPORTS `imported_names` 落盘 + 解析 | `test_edge_imports` |
| 使能 C | 外部 import 物化 phantom + 边；不丢、不假阳性 | `test_edge_imports` |
| 导出索引 | re-export BFS、深度上限断环、同名取公开 | 新 `test_cross_pkg_export` |
| 桥接 | `import {X}` → CROSS_IMPORTS 命中 X；`obj.X()` → CROSS_CALLS 命中方法；双向都在；重跑幂等 | 新 `test_cross_pkg_bridge`（双 project fixture） |
| 查询侧 | trace_path 缝合到对端符号、深度上限截断、visited 去重；get_code_snippet 按 QN 前缀切库 | 新 `test_trace_cross_repo` |

双 project fixture：两个临时 repo（假 App + 假组件库），各索引、跑桥接、断言边 + trace 结果。`test_incremental` 有多 project 先例可参照。

#### 错误处理/降级
- 提供方无 package.json / name → 跳过该 target，记日志，不中断。
- 入口模块 QN 找不到 → 退化为包入口级，降 confidence。
- re-export 环 → 深度上限截断，标 `reexport_depth_capped`。
- 对端 store 打不开（未索引） → 跳过该边的跨库跳（本库边仍在）；trace 在该点截断，标 `target_not_indexed`。
- 跨库跳超预算 → 截断标 `cross_depth_capped`，绝不静默。

#### 使能改动的回归保护
B、C 改了现有提取行为（A 无需改代码 — `is_exported` 已在节点 `properties` JSON 里）。全量现有套件（5604 tests）须过。每个使能配一条"行为不变"回归断言：B — 新 `imported_names` prop 不影响现有 IMPORTS 解析；C — phantom 物化不给仓内 import 引入假阳性 IMPORTS 边，且原先经 strategy-3 误连的外部 import 现改连 phantom。

## 已知风险

- **CROSS_CALLS 重提成本：** 桥接要对每个 import 了 bridged 包的消费方文件重跑 `cbm_extract_file`。大型 App 仓库（TrainCRN train_main 单 2088 条 import 语句）开销不小。缓解：只重提 IMPORTS 边指向 bridged provider phantom 的文件（有界子集），复用现有 result cache。
- **re-export 过近似** 可能多连同名常见符号（如某无关 re-export 模块也导出 `Colors`）。confidence 评分 + 优先 entry 可达定义缓解；作为已知近似记录在案。
- **多 store BFS 性能/正确性：** 缝合循环是风险最高的新查询侧代码。跨库跳预算 + visited 集是主要护栏；专门 `test_trace_cross_repo` 覆盖环、深度上限、未索引对端。

## 文件改动清单（给实现计划用）

- `src/store/store.c`（+ `.h`）— `cbm_store_bfs` 不变（单库）；新跨库缝合辅助放 mcp 层。**不加 `is_exported` 列** — 它已在节点 `properties` JSON 里，用 `json_extract` + `json_valid` 守卫查。
- `src/pipeline/pass_pkgmap.c` — import 解析路径里的外部包 phantom 物化（使能 C）。
- `src/pipeline/pass_parallel.c:create_imports_edges` — `imported_names` prop（使能 B）、phantom upsert（使能 C）。
- `internal/cbm/extract_imports.c` — 每条 import 语句捕获具名导入符号（使能 B 源头）。（`extract_defs.c` / `pass_definitions.c` **无需改** — `is_exported` 已写进节点 `properties` JSON。）
- `src/pipeline/pass_cross_repo.c`（+ `.h`）— 新 `cbm_cross_repo_package_bridge`、导出索引构建、双向边发射、`delete_cross_edges` 扩展。
- `src/mcp/mcp.c` — `handle_cross_repo_mode` 扩展；`trace_path` 跨 store 缝合 + `mode="cross_repo"` 边类型集；`get_code_snippet` 按 QN 前缀切库；已开 store 缓存。
- `tests/` — `test_cross_pkg_export`、`test_cross_pkg_bridge`、`test_trace_cross_repo`，加现有套件里的使能回归断言。
