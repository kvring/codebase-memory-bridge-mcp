# Cross-Repo Package-Import Bridge — Design Spec

**Date:** 2026-07-14
**Status:** Approved (design phase) → ready for implementation planning
**Scope:** codebase-memory-mcp source — extend cross-repo intelligence to npm package dependencies

## Problem

codebase-memory-mcp builds one independent knowledge graph per repository. Two
graphs default to **zero edges** between them. The only existing cross-repo
mechanism is `cross-repo-intelligence` mode (`pass_cross_repo.c`), which
matches microservice **Routes / Channels / async topics** — it does not
recognize npm package imports.

A frontend main project (TrainCRN) that consumes a shared component library
(`@ctrip/train_rn_common`) via `import { X } from '@ctrip/train_rn_common'` is
therefore **disconnected**:

- The component library graph has `X`'s real definition.
- The App graph has the import statement and calls to `X`, but neither resolves
  across the repo boundary.
- `cross-repo-intelligence` reports 0 CROSS_* edges (verified in a prior
  session against the actual TrainCRN / train_rn_common pair).

Worse, the consumer graph **currently drops** external-package imports and
their calls entirely (see §3 Enabler C), so there is no anchor to bridge from.

## Goal

Symbol-level cross-repo linkage for npm package dependencies, so that:

1. `trace_path` can follow a call chain from a business function in the App
   across the package boundary into the component library's real method
   implementation.
2. Reverse impact analysis: changing a component-library symbol shows which
   App call sites are affected.

## Constraints (decided in brainstorming)

| Decision | Choice |
|---|---|
| Linkage precision | **Symbol-level** (link to the real exported symbol) |
| Which edges | **Both** import statements (`CROSS_IMPORTS`) and call sites (`CROSS_CALLS`) — required for continuous trace |
| Ecosystem scope | **npm only** for v1, with an abstraction seam (`PkgBridge` strategy interface) for future Go/Rust/Python |
| Trigger | **Extend existing `cross-repo-intelligence` mode** (one command, both Route + Package) |
| Direction | **Bidirectional** (write edges into both DBs; supports reverse impact analysis) |

## Non-Goals (v1)

- Go modules / Cargo crates / pyproject / composer / pom cross-repo linking
  (seam reserved, not implemented).
- Cypher `query_graph` cross-DB traversal (too large; CROSS_* edges remain
  readable as in-DB edges + props metadata, not traversable in Cypher).
- `search_graph` cross-DB (returns only the local DB's hits).
- Automatic cross-repo edge invalidation on re-index (v1: re-run the bridge
  manually after re-indexing either side).

## Architecture Context (verified in source)

- `pass_cross_repo.c` — existing CROSS_* pass; `build_cross_props` /
  `insert_cross_edge` / `emit_cross_route_bidirectional` define the storage
  pattern. Each CROSS_* edge connects **two real nodes within one DB**; the
  cross-project counterpart is carried only in `properties_json`
  (`target_project` / `target_function` / `target_file`).
- `pass_pkgmap.c` — maps a manifest's **own** `name` field → entry module QN
  (within the same repo). External dependencies listed in `dependencies` are
  **not** registered, so bare external imports do not resolve via pkgmap.
- `pass_calls.c` — `resolve_single_call` returns 0 (drops the call, no edge)
  when the callee cannot be resolved in-project and is not an HTTP/async
  service pattern. External-library calls are dropped.
- `pass_parallel.c:create_imports_edges` — creates an IMPORTS edge only when
  `cbm_pipeline_resolve_import_node` returns a non-NULL target. External
  packages resolve to a phantom QN that is **never materialized as a node**,
  so the import is dropped (or, via strategy-3 symbol fallback, mis-linked to
  an unrelated in-project same-name node — a false positive).
- `cbm_store_bfs` (store.c) — single-store recursive SQL CTE, filtered by
  `e.type IN (...)`. Cannot hop stores mid-CTE. `trace_path` operates on one
  store resolved from the `project` arg.
- `extract_imports.c:511` — re-exports (`export {x} from './m'`, `export *`)
  are captured as IMPORTS.
- `is_exported` is serialized into each node's `properties` JSON at dump time
  (`pass_definitions.c` / `pass_parallel.c` write `"is_exported":true/false`
  into `properties_json`, persisted via `cbm_store_upsert_node`). It is
  queryable post-index via `json_extract(properties,'$.is_exported')`. Caveat:
  `json_extract` aborts on malformed-JSON rows present in legacy DBs
  (store.c:302-307 documents a reverted `json_extract` partial index for this
  reason), so export-index queries must guard with `json_valid(properties)`.

## Design

### §1 Data Model & Edge Types

Two new edge types, mirroring the existing CROSS_* model (connect local real
nodes; cross-project counterpart in props; no stub nodes inserted):

| Edge | Forward edge (consumer DB) | Meaning |
|---|---|---|
| `CROSS_IMPORTS` | consumer File/Module node → local external-package phantom node | `import {X} from '@pkg'` linked to provider symbol X |
| `CROSS_CALLS` | consumer call-site function → local unresolved call-target node | `pkg.X()` linked to provider method X |

**Reverse edges** (provider DB): `CROSS_IMPORTED_BY` /
`CROSS_CALLED_BY`, from the provider symbol node to a local package-entry /
symbol node, props carrying the consumer call site — enables "change a library
symbol → who is affected" queries inside the provider DB.

**Props schema** (extends `build_cross_props`):

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

**No stub nodes.** The consumer graph gains no new nodes except the
`is_external=1` phantom import node (Enabler C), which represents real
information ("this file imports an external package"). `search_graph` may
filter on `is_external` if these become noisy.

**Lifecycle / idempotency:** `delete_cross_edges` is extended to clear
`CROSS_IMPORTS`, `CROSS_CALLS`, `CROSS_IMPORTED_BY`, `CROSS_CALLED_BY`
before recompute. Re-running the bridge does not stack edges.

### §2 Provider Export Index

Built per provider project **during the bridge pass** (in-memory, transient —
not persisted, preserving per-DB self-containment), from the provider's
persisted SQLite DB:

```
1. Read <provider_root>/package.json → name (@ctrip/train_rn_common),
   entry (exports["."] | main | module).
2. Convert entry → provider Module/File node QN (cbm_pipeline_fqn_module,
   same algorithm as pass_pkgmap).
3. BFS from the entry node along IMPORTS edges (re-export chain:
   `export {x} from './y'` is already an IMPORTS edge), depth-bounded (e.g. 8)
   to break re-export cycles.
4. Collect is_exported=true nodes (Function/Class/Variable/Interface/Type)
   along the way.
5. Index: (package_name, symbol_name) → {provider_qn, node_id, file, label}.
```

**No schema change needed for `is_exported`.** It is already persisted inside
each node's `properties` JSON (see Architecture Context). The export-index
builder queries it with
`WHERE json_valid(properties) AND json_extract(properties,'$.is_exported')='true'`.
The `json_valid` guard is mandatory: legacy DBs contain malformed-JSON rows
that make bare `json_extract` abort (store.c:302-307 reverted a partial index
for this reason).

**Re-export over-approximation:** the re-export IMPORTS edge carries only the
target module path (not which names are re-exported), so the BFS collects
**all** `is_exported` nodes of re-export-target modules. This is acceptable
for linkage (over-link beats miss; `export *` genuinely exposes all).

**Same-name ambiguity:** when two reachable modules export the same symbol
name, prefer the one reachable from the entry (public API) over an internal
definition; on persistent conflict take the lexicographically smallest QN and
lower `confidence`. Rare; logged.

**Entry missing:** if `exports`/`main` is absent or the entry node QN is not
found, degrade to package-entry-level linkage (package name → any module),
lower `confidence`. Never abort the pass.

### §3 Consumer Anchor Discovery

#### Enabler B: record named imported symbols on IMPORTS edges
Enhance the TS/JS extractor so the IMPORTS edge props extend from
`{"local_name":"..."}` to
`{"local_name":"...", "imported_names":["TrainUBTLogUtil","Colors"]}`.
Without this, the consumer side cannot know which symbols an import statement
binds, and symbol-level `CROSS_IMPORTS` is impossible.

#### Enabler C: materialize external-package imports as phantom nodes + edges
In `create_imports_edges` (and the shared `cbm_pipeline_resolve_import_node`
fallthrough), when an import resolves to an external package (non-relative,
non-alias, absent from the local pkgmap), **upsert a phantom Module node**
(QN = `<project>.<module_path>`, `is_external=1`) and create the IMPORTS edge
to it — instead of the current drop (or strategy-3 false-positive). This
mirrors `pass_cross_repo`'s synthesis of route nodes for external libraries
(`create_svc_route_node`) and gives the bridge a deterministic consumer-side
anchor. The current drop is the **root cause** of the prior session's 0-edge
result.

#### CROSS_IMPORTS anchors
With Enablers B + C + the §2 export index: scan the consumer DB's IMPORTS
edges whose target is an external phantom; read `imported_names` from props;
match each against the provider export index → emit `CROSS_IMPORTS`
(consumer File node → local phantom, props carry the provider symbol).

#### CROSS_CALLS anchors
Call edges to library symbols are dropped by `pass_calls.c` (no anchor).
The bridge **re-derives** them: for each consumer file with a bridged
IMPORTS edge, run `cbm_extract_file` to obtain its calls; for calls whose
callee matches a provider-exported symbol (including `obj.method` where `obj`
is an imported, bridged library symbol), synthesize `CROSS_CALLS` from the
consumer call-site function to the local unresolved call-target (or to the
phantom if no call-target node exists), props carrying the provider method.

### §4 Query-Side Cross-DB Hop

Building edges is insufficient — `trace_path` / `get_code_snippet` must learn
to follow cross-project pointers. This is the v1 riskiest component.

#### 4.1 Trace must recognize CROSS_* edge types
`resolve_trace_edge_types` currently excludes CROSS_*. Add `CROSS_IMPORTS` /
`CROSS_CALLS` (and reverse types) via a new `mode="cross_repo"` (preferred —
opt-in, avoids polluting ordinary trace results).

#### 4.2 Multi-store stitch BFS
A single SQL CTE cannot hop stores. Two-phase stitch:

```
1. Run cbm_store_bfs on the origin store (with CROSS_* edge types) to the
   local reachable frontier.
2. Scan the frontier for CROSS_* edges carrying target_project:
   - resolve_store(target_project) → open the target store (cached).
   - cbm_store_find_node_by_qn(target_qn) in the target store.
   - Run cbm_store_bfs again from that node (same edge-type set, depth
     continued), marking results cross_project=true with target_project /
     target_symbol.
3. Recurse across stores with a total cross-hop budget (e.g. 3) and a
   cross-store visited(qn) set to prevent cycles / exponential blowup.
   Exceeding the budget truncates and labels cross_depth_capped (never
   silent).
```

A per-server **opened-store cache** keyed by project name avoids re-opening.

#### 4.3 get_code_snippet cross-DB
`get_code_snippet(qualified_name)`: parse the project prefix of the QN
(`<project>.<path>.<name>`); if it differs from the current project,
`resolve_store(prefix)` and fetch the snippet there. Lets the agent pull the
real provider source after trace reaches it.

#### 4.4 Scope bound
v1 changes only `trace_path` + `get_code_snippet`. `query_graph` (Cypher) and
`search_graph` do not cross DBs; CROSS_* edges appear as in-DB edges with
props metadata (readable, not traversed in Cypher) — documented.

### §5 Trigger, Integration, Bidirectionality, Lifecycle

**Trigger:** extend `handle_cross_repo_mode` in mcp.c — after the existing
Route/Channel match, call the new `cbm_cross_repo_package_bridge`. Result JSON
gains `cross_import_edges` / `cross_call_edges` fields.

**Bridge function:**
```c
cbm_cross_repo_result_t cbm_cross_repo_package_bridge(
    const char *project, const char **target_projects, int target_count);
```
Per target: read package.json `name`; if the consumer DB has external phantoms
pointing at it, build the export index, match named imports + re-derive calls,
write bidirectional edges.

**Bidirectionality:** forward (consumer DB) `CROSS_IMPORTS`/`CROSS_CALLS`;
reverse (provider DB) `CROSS_IMPORTED_BY`/`CROSS_CALLED_BY` — mirroring
`emit_cross_route_bidirectional`.

**Idempotency:** `delete_cross_edges` extended for the four new types;
recompute on each run.

**Invariant preserved:** each DB stays self-contained; cross-project info
lives in props. The only new nodes are `is_external=1` phantoms representing
real external-import facts.

### §6 Testing & Error Handling

#### Test matrix
| Layer | Test | Harness |
|---|---|---|
| is_exported query | `json_extract(properties,'$.is_exported')='true'` with `json_valid` guard returns exported defs; malformed-JSON rows skipped not aborted | `test_store_arch` style |
| Enabler B | IMPORTS `imported_names` persisted + parsed | `test_edge_imports` |
| Enabler C | external import materializes phantom + edge; no drop, no false positive | `test_edge_imports` |
| Export index | re-export BFS, depth cap breaks cycles, same-name prefers public | new `test_cross_pkg_export` |
| Bridge | `import {X}` → CROSS_IMPORTS hits X; `obj.X()` → CROSS_CALLS hits method; both sides written; idempotent re-run | new `test_cross_pkg_bridge` (two-project fixture) |
| Query-side | trace_path stitches to provider symbol; depth cap truncates; visited dedups; get_code_snippet switches DB by QN prefix | new `test_trace_cross_repo` |

Two-project fixture: two temp repos (fake App + fake component library),
index each, run the bridge, assert edges + trace results. `test_incremental`
provides a multi-project precedent.

#### Error handling / degradation
- Provider has no package.json / name → skip target, log, do not abort.
- Entry module QN not found → degrade to package-entry-level, lower confidence.
- Re-export cycle → depth cap, label `reexport_depth_capped`.
- Target store unopenable (not indexed) → skip the cross-hop for that edge
  (local edge remains); trace truncates at that point, labelled
  `target_not_indexed`.
- Cross-hop budget exceeded → truncate, label `cross_depth_capped`, never
  silent.

#### Regression protection for the enablers
B and C modify existing extraction behavior (A needs no code change —
`is_exported` is already in node `properties` JSON). Full existing suite (5604
tests) must pass. Each enabler gets a "behavior unchanged" regression
assertion: B — existing IMPORTS resolution unaffected by the new
`imported_names` prop; C — phantom materialization introduces no
false-positive IMPORTS edges for in-project imports and external imports that
previously resolved via strategy-3 now resolve to the phantom instead.

## Open Risks

- **CROSS_CALLS re-derivation cost:** the bridge re-runs `cbm_extract_file` on
  every consumer file that imports a bridged package. For large App repos
  (TrainCRN: 2088 import statements in train_main alone) this is non-trivial.
  Mitigation: only re-extract files whose IMPORTS edges target a bridged
  provider phantom (bounded subset), and reuse the existing result cache.
- **Re-export over-approximation** can over-link symbols with common names
  (e.g. a provider `Colors` exported from an unrelated re-exported module).
  Confidence scoring + preferring entry-reachable defs mitigates; documented
  as a known approximation.
- **Multi-store BFS performance/correctness:** the stitch loop is the riskiest
  new query-side code. Cross-hop budget + visited set are the primary guards;
  a dedicated `test_trace_cross_repo` suite covers cycles, depth caps, and
  unindexed targets.

## File Touch Summary (for the implementation plan)

- `src/store/store.c` (+ `.h`) — `cbm_store_bfs` unchanged (single-store); new
  cross-store stitch helpers likely in the mcp layer. **No `is_exported` column**
  — it already lives in each node's `properties` JSON, queried via
  `json_extract` + `json_valid` guard.
- `src/pipeline/pass_pkgmap.c` — external-package phantom materialization in
  the import resolver path (Enabler C).
- `src/pipeline/pass_parallel.c:create_imports_edges` — `imported_names` prop
  (Enabler B), phantom upsert (Enabler C).
- `internal/cbm/extract_imports.c` — capture named imported symbols per
  import statement (Enabler B source). (`extract_defs.c` /
  `pass_definitions.c` need **no change** — `is_exported` is already emitted
  into node `properties` JSON.)
- `src/pipeline/pass_cross_repo.c` (+ `.h`) — new
  `cbm_cross_repo_package_bridge`, export-index builder, bidirectional edge
  emission, `delete_cross_edges` extension.
- `src/mcp/mcp.c` — `handle_cross_repo_mode` extension; `trace_path`
  cross-store stitch + `mode="cross_repo"` edge-type set; `get_code_snippet`
  QN-prefix store switch; opened-store cache.
- `tests/` — `test_cross_pkg_export`, `test_cross_pkg_bridge`,
  `test_trace_cross_repo`, plus enabler regression assertions in existing
  suites.
