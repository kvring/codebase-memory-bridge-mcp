# Cross-Repo Package-Import Bridge — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add symbol-level cross-repo linkage for npm package dependencies so `trace_path` follows a call chain from a consumer (App) across the package boundary into a provider (component library) real method, with bidirectional impact-analysis edges.

**Architecture:** Extend the existing `cross-repo-intelligence` model (per-DB CROSS_* edges with cross-project counterpart carried in `properties_json`). Three enablers fix the consumer-side gaps that currently drop external-package imports/calls; an in-memory provider export index (built from the persisted graph) resolves named imports/calls to real provider symbols; the bridge writes bidirectional `CROSS_IMPORTS`/`CROSS_CALLS` edges; `trace_path`/`get_code_snippet` learn to stitch across project DBs.

**Tech Stack:** C99, SQLite (FTS5, json_extract), yyjson, tree-sitter (TS/JS grammar), the project's own gbuf→store pipeline + C test harness (`tests/test_*.c` using `TEST()`/`ASSERT_*` macros).

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-14-cross-repo-package-bridge-design.md` (EN) + `.zh.md` (ZH) are the source of truth.
- **`is_exported` is already persisted** in each node's `properties` JSON (`pass_definitions.c:232`, `pass_parallel.c:276`). Query it with `WHERE json_valid(properties) AND json_extract(properties,'$.is_exported')='true'`. The `json_valid` guard is **mandatory** — legacy DBs contain malformed-JSON rows that make bare `json_extract` abort (store.c:302-307 reverted a partial index for this reason).
- **No new SQLite columns** for `is_exported` or `is_external`. Both live in node `properties_json` (the `cbm_gbuf_node_t` struct has no `is_external` field — use `properties_json`).
- **Per-DB self-containment invariant:** CROSS_* edges connect two real nodes **within one DB**; the cross-project counterpart is carried only in `properties_json`. Mirror `emit_cross_route_bidirectional` exactly.
- **Idempotency:** `delete_cross_edges` must clear the four new edge types before recompute.
- **No `--no-verify`** on commits; hooks must pass.
- Build/test commands: `scripts/build.sh` (binary at `build/c/codebase-memory-mcp`); run a single test suite via the built test binary (see `tests/` Makefile target — match the existing pattern in `Makefile.cbm`).
- Commit messages follow the repo style (`type(scope): desc`, e.g. `feat(crossrepo): ...`). The user's global CLAUDE.md mandates the `uniai-commit` skill for commits — confirm with the user before committing.

## Spec refinements discovered during planning (override the spec)

1. **Enabler A is removed.** The spec's "persist `is_exported` as a column" is unnecessary — it is already in `properties` JSON. No `extract_defs.c`/`pass_definitions.c`/`store.c` change for `is_exported`.
2. **Enabler B is smaller.** `process_named_imports` (`internal/cbm/extract_imports.c`) already pushes one `CBMImport` per imported symbol with `local_name`, but for `import {A as B}` it stores `B` (alias) and **discards `A`** (the exported name). Enabler B = add `exported_name` (= `orig`) to `CBMImport` + the IMPORTS edge `properties_json`. For non-alias imports `exported_name == local_name`.

---

## File Structure

| File | Responsibility | Phase |
|---|---|---|
| `internal/cbm/cbm.h` | Add `exported_name` to `CBMImport` | P2 |
| `internal/cbm/extract_imports.c` | Set `exported_name` for each pushed import (alias + non-alias) | P2 |
| `src/pipeline/pass_parallel.c` (`create_imports_edges`) | Phantom upsert (Enabler C) + `exported_name`/`is_external` in edge/node props (Enabler B/C) | P1, P2 |
| `src/pipeline/pass_pkgmap.c` (`cbm_pipeline_resolve_import_node`) | Mark the external-import fallthrough so `create_imports_edges` can detect "external" | P1 |
| `src/pipeline/pass_cross_repo.c` (+ `.h`) | `cbm_cross_repo_package_bridge`, export-index builder, bidirectional emit, `delete_cross_edges` extension, CROSS_CALLS re-derivation | P3, P4 |
| `src/mcp/mcp.c` | `handle_cross_repo_mode` extension; `trace_path` `mode=cross_repo` edge types + multi-store stitch; `get_code_snippet` QN-prefix switch; opened-store cache | P3, P5 |
| `tests/test_edge_imports.c` | Enabler B/C regression + new assertions | P1, P2 |
| `tests/test_cross_pkg_export.c` (new) | Export-index builder | P3 |
| `tests/test_cross_pkg_bridge.c` (new) | Bridge + bidirectional + idempotency (two-project fixture) | P3, P4 |
| `tests/test_trace_cross_repo.c` (new) | trace cross-DB stitch + get_code_snippet switch | P5 |

---

## Phase 1 — Enabler C: materialize external-package imports as phantom nodes + IMPORTS edges

**Why first:** This fixes the root cause of the prior session's 0-edge result. Without it there is no consumer-side anchor for any later phase. Independently testable: after this phase, `import {X} from '@pkg'` produces a phantom `Module` node (QN `<project>.@pkg`, `is_external` in properties) + an IMPORTS edge to it.

### Task 1.1: Failing test — external import produces a phantom node + IMPORTS edge

**Files:**
- Test: `tests/test_edge_imports.c` (append a new `TEST(edge_imports_external_phantom)`)

**Interfaces:**
- Consumes: the existing indexing entrypoint used by `test_edge_imports.c` (a `cbm_gbuf_t` + `cbm_pipeline_pass_*` over a temp repo). Match the setup idiom already used in this file's other tests.
- Produces: an assertion pattern later tasks extend (phantom node exists, `is_external` true, IMPORTS edge present).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_edge_imports.c`:

```c
/* External package import must materialize a phantom Module node + IMPORTS
 * edge (Enabler C). Before this, the import was silently dropped because the
 * external target QN was never upserted. */
TEST(edge_imports_external_phantom) {
    /* Fixture: one app.ts that imports an external package not in the repo. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_extimp_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmpdir));

    char app_path[512];
    snprintf(app_path, sizeof(app_path), "%s/app.ts", tmpdir);
    FILE *f = fopen(app_path, "w");
    fprintf(f, "import { TrainUBTLogUtil } from '@ctrip/train_rn_common';\n"
               "export function bookTicket() { TrainUBTLogUtil.ubtLog('x'); }\n");
    fclose(f);

    char *project = cbm_project_name_from_path(tmpdir);
    /* Index via the same helper the surrounding tests use (e.g. th_index_repo
     * or cbm_pipeline_run_full — match the file's existing helper). */
    cbm_gbuf_t *gb = th_index_repo(tmpdir, project);  /* match existing helper name */
    ASSERT_NOT_NULL(gb);

    /* Phantom Module node QN: <project>.@ctrip/train_rn_common */
    char phantom_qn[512];
    snprintf(phantom_qn, sizeof(phantom_qn), "%s.@ctrip/train_rn_common", project);
    const cbm_gbuf_node_t *phantom = cbm_gbuf_find_by_qn(gb, phantom_qn);
    ASSERT_NOT_NULL_MSG(phantom, "external phantom Module node must be materialized");

    /* is_external lives in node properties_json */
    ASSERT_NOT_NULL(strstr(phantom->properties_json, "\"is_external\":true"));

    /* app.ts __file__ node must have an IMPORTS edge to the phantom */
    char file_qn[512];
    snprintf(file_qn, sizeof(file_qn), "%s.app.__file__", project);
    const cbm_gbuf_node_t *appfile = cbm_gbuf_find_by_qn(gb, file_qn);
    ASSERT_NOT_NULL(appfile);
    const cbm_gbuf_edge_t **edges = NULL;
    int ec = 0;
    ASSERT_EQ(0, cbm_gbuf_find_edges_by_source_type(gb, appfile->id, "IMPORTS", &edges, &ec));
    ASSERT_GT(ec, 0);
    bool linked = false;
    for (int i = 0; i < ec; i++) {
        if (edges[i]->target_id == phantom->id) { linked = true; break; }
    }
    ASSERT_TRUE_MSG(linked, "IMPORTS edge must target the external phantom");

    cbm_gbuf_free(gb);
    free(project);
    th_rmtree(tmpdir);
    PASS();
}
```

Register it in the file's `SUITE(edge_imports) { ... RUN_TEST(edge_imports_external_phantom); }`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `scripts/build.sh && build/c/codebase-memory-mcp ... # run the test binary` (match the existing `test_edge_imports` run target in `Makefile.cbm`).
Expected: FAIL — `phantom` is NULL (the import is currently dropped).

- [ ] **Step 3: Implement Enabler C — phantom upsert in `create_imports_edges`**

Modify `src/pipeline/pass_parallel.c` `create_imports_edges` (around line 866). Currently it skips when `cbm_pipeline_resolve_import_node` returns NULL:

```c
/* Enabler C: when the import does not resolve to an in-project node, treat it
 * as an external package import: upsert a phantom Module node (QN =
 * <project>.<module_path>, is_external in properties) and link to it. This
 * mirrors pass_cross_repo's create_svc_route_node synthesis of external-lib
 * route nodes, and is the anchor the cross-repo package bridge matches on. */
const cbm_gbuf_node_t *target =
    cbm_pipeline_resolve_import_node(ctx, rel, file_qn, imp, namespace_map);
if (!target) {
    /* Only synthesize for bare/external specifiers (non-relative, non-alias).
     * Relative imports that miss are genuine missing files — do not phantom. */
    if (imp->module_path && imp->module_path[0] != '.' &&
        !cbm_pipeline_is_path_alias(ctx, imp->module_path)) {
        char *ext_qn = cbm_pipeline_fqn_module(ctx->project_name, imp->module_path);
        if (ext_qn) {
            target = cbm_gbuf_upsert_node(ctx->gbuf, "Module",
                                           imp->module_path, ext_qn, rel,
                                           0, 0, "{\"is_external\":true}");
            free(ext_qn);
        }
    }
}
if (target && target->id != source_node->id) {
    char esc_ln[CBM_SZ_128];
    cbm_json_escape(esc_ln, sizeof(esc_ln), imp->local_name ? imp->local_name : "");
    char imp_props[CBM_SZ_256];
    snprintf(imp_props, sizeof(imp_props), "{\"local_name\":\"%s\"}", esc_ln);
    cbm_gbuf_insert_edge(ctx->gbuf, source_node->id, target->id, "IMPORTS", imp_props);
    count++;
}
```

Notes:
- `cbm_pipeline_is_path_alias` may not exist yet — if not, gate the synthesis on `imp->module_path[0] != '.'` only (skip the alias check). Add a TODO only if you cannot resolve it; prefer the simpler gate. (The path-alias resolver is `ctx->path_aliases` used in `pass_pkgmap.c:1129`; an `is_path_alias` helper can be added there if needed — but for v1 the `!= '.'` gate covers external bare specifiers like `@ctrip/...`.)
- `cbm_gbuf_upsert_node` signature is `(gb, label, name, qualified_name, file_path, start_line, end_line, properties_json)` (verified in `graph_buffer.h:74`).

- [ ] **Step 4: Run the test to verify it passes**

Run the same test target.
Expected: PASS.

- [ ] **Step 5: Regression — run the full edge_imports suite + a broad run**

Run: the `test_edge_imports` suite (all existing tests must still pass — phantom synthesis must not introduce false-positive IMPORTS edges for in-project imports).
Expected: all PASS. If any existing test breaks (e.g. a test that asserted an external import produced *no* edge), update the test expectation and document why in the commit message.

- [ ] **Step 6: Commit**

```bash
git add src/pipeline/pass_parallel.c tests/test_edge_imports.c
# Use the uniai-commit skill per global CLAUDE.md; message e.g.:
# feat(crossrepo): materialize external-package imports as phantom nodes (#enabler-c)
```

---

## Phase 2 — Enabler B: capture `exported_name` for aliased imports

**Independently testable:** the IMPORTS edge `properties_json` carries `exported_name`; for `import {A as B}` it is `A`.

### Task 2.1: Failing test — aliased import records exported_name

**Files:**
- Test: `tests/test_edge_imports.c` (append `TEST(edge_imports_alias_exported_name)`)

**Interfaces:**
- Consumes: the phantom node + IMPORTS edge from Phase 1.
- Produces: IMPORTS edges whose `properties_json` includes `"exported_name":"<orig>"`.

- [ ] **Step 1: Write the failing test**

```c
TEST(edge_imports_alias_exported_name) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_alias_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmpdir));
    char app_path[512];
    snprintf(app_path, sizeof(app_path), "%s/app.ts", tmpdir);
    FILE *f = fopen(app_path, "w");
    fprintf(f, "import { CStyleNew as CStyle } from '@ctrip/train_rn_common';\n");
    fclose(f);

    char *project = cbm_project_name_from_path(tmpdir);
    cbm_gbuf_t *gb = th_index_repo(tmpdir, project);
    ASSERT_NOT_NULL(gb);

    char file_qn[512];
    snprintf(file_qn, sizeof(file_qn), "%s.app.__file__", project);
    const cbm_gbuf_node_t *appfile = cbm_gbuf_find_by_qn(gb, file_qn);
    ASSERT_NOT_NULL(appfile);
    const cbm_gbuf_edge_t **edges = NULL;
    int ec = 0;
    cbm_gbuf_find_edges_by_source_type(gb, appfile->id, "IMPORTS", &edges, &ec);
    bool found_exported = false;
    for (int i = 0; i < ec; i++) {
        if (strstr(edges[i]->properties_json, "\"exported_name\":\"CStyleNew\"")) {
            found_exported = true; break;
        }
    }
    ASSERT_TRUE_MSG(found_exported, "aliased import must record exported_name=CStyleNew");

    cbm_gbuf_free(gb);
    free(project);
    th_rmtree(tmpdir);
    PASS();
}
```
Register in `SUITE(edge_imports)`.

- [ ] **Step 2: Run to verify it fails** — FAIL (no `exported_name` prop yet).

- [ ] **Step 3: Add `exported_name` to `CBMImport` + populate it**

3a. `internal/cbm/cbm.h` — extend the struct (currently `{ const char *local_name; const char *module_path; }`):

```c
typedef struct {
    const char *local_name;    // local alias or name
    const char *module_path;   // resolved module path / QN
    const char *exported_name; // original exported name (== local_name when no alias)
} CBMImport;
```

3b. `internal/cbm/extract_imports.c` — set `exported_name` everywhere a `CBMImport` is pushed:

In `process_named_imports` (verified body above), capture `orig` (currently discarded):

```c
if (!ts_node_is_null(orig)) {
    char *local_name = !ts_node_is_null(local) ? cbm_node_text(a, local, ctx->source)
                                               : cbm_node_text(a, orig, ctx->source);
    char *exp = cbm_node_text(a, orig, ctx->source);  /* exported name A */
    CBMImport imp = {.local_name = local_name, .module_path = path, .exported_name = exp};
    cbm_imports_push(&ctx->result->imports, a, imp);
    found = true;
}
```

In `process_es_import_statement` and `process_import_clause`, for the plain `identifier` branch set `exported_name = name` (== local_name):

```c
CBMImport imp = {.local_name = name, .module_path = path, .exported_name = name};
```

Do the same for the namespace import branch and the `export {x} from './m'` re-export push (there `exported_name` = `path_last(path)` to match `local_name`).

3c. `src/pipeline/pass_parallel.c` `create_imports_edges` — emit `exported_name` on the edge props:

```c
char esc_ln[CBM_SZ_128], esc_en[CBM_SZ_128];
cbm_json_escape(esc_ln, sizeof(esc_ln), imp->local_name ? imp->local_name : "");
cbm_json_escape(esc_en, sizeof(esc_en), imp->exported_name ? imp->exported_name : "");
char imp_props[CBM_SZ_384];
snprintf(imp_props, sizeof(imp_props),
         "{\"local_name\":\"%s\",\"exported_name\":\"%s\"}", esc_ln, esc_en);
cbm_gbuf_insert_edge(ctx->gbuf, source_node->id, target->id, "IMPORTS", imp_props);
```

- [ ] **Step 4: Run to verify it passes** — PASS.
- [ ] **Step 5: Regression** — run `test_edge_imports` + `test_extraction` (existing IMPORTS parsing must be unaffected). Expected: all PASS.
- [ ] **Step 6: Commit** (`feat(crossrepo): record exported_name on IMPORTS edges for alias resolution`).

---

## Phase 3 — Provider export index + CROSS_IMPORTS bridge (bidirectional) + trigger

### Task 3.1: Export-index builder + unit test

**Files:**
- Create: `src/pipeline/cross_pkg_export.c` (+ declare the API in `pass_cross_repo.h`)
- Test: `tests/test_cross_pkg_export.c` (new)

**Interfaces:**
- Consumes: a provider `cbm_store_t*` + the provider root path (to read `package.json`).
- Produces: `cbm_pkg_export_index_t*` — a hash map `(package_name, symbol_name) → {qn, node_id, file, label}`.

- [ ] **Step 1: Write the failing test**

`tests/test_cross_pkg_export.c`:

```c
/* Provider export index: from package.json entry, BFS IMPORTS re-export chain,
 * collect is_exported=true nodes, key (pkg,symbol)->node. json_valid guard. */
TEST(cross_pkg_export_resolves_reexport) {
    /* Fixture: a component-library repo:
     *   package.json  {"name":"@ctrip/lib","main":"src/index.ts"}
     *   src/index.ts  export { TrainUBTLogUtil } from './utils';
     *   src/utils.ts  export function TrainUBTLogUtil() {}
     * After indexing, the export index must map ("@ctrip/lib","TrainUBTLogUtil")
     * to the utils.ts def node. */
    /* ... build tmpdir, index via th_index_repo, open store, build index ... */
    cbm_pkg_export_index_t *ix = cbm_cross_pkg_build_export_index(store, tmpdir);
    ASSERT_NOT_NULL(ix);
    cbm_pkg_export_entry e;
    ASSERT_EQ(0, cbm_cross_pkg_export_lookup(ix, "@ctrip/lib", "TrainUBTLogUtil", &e));
    ASSERT_NOT_NULL(strstr(e.qn, "utils.TrainUBTLogUtil"));
    cbm_cross_pkg_export_index_free(ix);
    PASS();
}
```

Also add a cycle test (`export * from './a'; a: export * from './b'; b: export * from './a'`) asserting the BFS terminates (depth cap) and does not hang.

- [ ] **Step 2: Run to verify it fails** — FAIL (symbols undefined).

- [ ] **Step 3: Implement the export-index builder**

Create `src/pipeline/cross_pkg_export.c`. The builder runs **against the provider's persisted SQLite DB** (post-index). Key SQL — note the `json_valid` guard:

```c
/* Collect is_exported defs reachable from a module node via IMPORTS BFS.
 * Depth-bounded to break re-export cycles. */
static int collect_exported(cbm_store_t *s, const char *project,
                            int64_t entry_id, int depth, int max_depth,
                            cbm_pkg_export_index_t *ix) {
    if (depth >= max_depth) return 0;
    /* 1. Collect this module's own is_exported nodes (the module node itself
     *    is a File/Module; its exported defs are nodes in the same file). */
    sqlite3_stmt *st;
    sqlite3_prepare_v2(cbm_store_get_db(s),
        "SELECT id, name, qualified_name, file_path, label FROM nodes "
        "WHERE project=?1 AND file_path=(SELECT file_path FROM nodes WHERE id=?2) "
        "AND json_valid(properties) "
        "AND json_extract(properties,'$.is_exported')='true' "
        "AND label IN ('Function','Class','Variable','Interface','Type','Method')", -1, &st, NULL);
    /* bind, step, add to index keyed by (pkg,name) with conflict policy below */
    /* 2. Follow outgoing IMPORTS edges to re-exported modules, recurse. */
    sqlite3_prepare_v2(cbm_store_get_db(s),
        "SELECT target_id FROM edges WHERE source_id=?1 AND type='IMPORTS'", -1, &st, NULL);
    /* for each target_id, recurse collect_exported(..., target_id, depth+1 ...) */
    return 0;
}
```

Entry resolution: read `<provider_root>/package.json` with the same yyjson pattern as `pass_pkgmap.c:parse_package_json` to get `name` + `resolve_pkg_entry`; convert entry to the provider Module/File QN via `cbm_pipeline_fqn_module(project, entry)`; look up that node id; BFS.

Conflict policy: on duplicate `(pkg, name)`, keep the entry reachable at the **shallowest** depth (public API); on persistent tie take lexicographically smallest QN, lower confidence.

- [ ] **Step 4: Run to verify it passes** — PASS.
- [ ] **Step 5: Commit** (`feat(crossrepo): provider export-index builder`).

### Task 3.2: CROSS_IMPORTS bridge (bidirectional) + `delete_cross_edges` extension

**Files:**
- Modify: `src/pipeline/pass_cross_repo.c` (+ `.h`)
- Test: `tests/test_cross_pkg_bridge.c` (new, two-project fixture)

**Interfaces:**
- Consumes: `cbm_cross_pkg_build_export_index` (Task 3.1); `cbm_store_open_path_query` / `resolve_store` pattern from mcp.c; `build_cross_props`/`insert_cross_edge` (existing).
- Produces: `cbm_cross_repo_result_t cbm_cross_repo_package_bridge(const char *project, const char **target_projects, int target_count);` (extend `cbm_cross_repo_result_t` with `cross_import_edges`).

- [ ] **Step 1: Write the failing test (two-project fixture)**

`tests/test_cross_pkg_bridge.c`:

```c
TEST(cross_pkg_bridge_imports_bidirectional) {
    /* Build two temp repos: App (imports @ctrip/lib) + Lib (exports TrainUBTLogUtil).
     * Index both. Run cbm_cross_repo_package_bridge(app, [lib]). Assert:
     *  - App DB has a CROSS_IMPORTS edge from app.ts __file__ -> phantom, props
     *    target_project=lib, target_symbol=TrainUBTLogUtil.
     *  - Lib DB has a CROSS_IMPORTED_BY edge from TrainUBTLogUtil -> its module,
     *    props target_project=app, target_symbol=bookTicket (or app file).
     * Re-run the bridge; assert edge counts unchanged (idempotent). */
    PASS();
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL.

- [ ] **Step 3: Extend `delete_cross_edges` + `cbm_cross_repo_result_t`**

In `pass_cross_repo.c`, add the four new types to `delete_cross_edges`:

```c
static void delete_cross_edges(cbm_store_t *store, const char *project) {
    cbm_store_delete_edges_by_type(store, project, "CROSS_HTTP_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_ASYNC_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_CHANNEL");
    cbm_store_delete_edges_by_type(store, project, "CROSS_GRPC_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_GRAPHQL_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_TRPC_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_IMPORTS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_CALLS");
    cbm_store_delete_edges_by_type(store, project, "CROSS_IMPORTED_BY");
    cbm_store_delete_edges_by_type(store, project, "CROSS_CALLED_BY");
}
```

In `pass_cross_repo.h`, extend the result struct:

```c
typedef struct {
    int http_edges, async_edges, channel_edges, grpc_edges, graphql_edges, trpc_edges;
    int cross_import_edges;   /* NEW */
    int cross_call_edges;     /* NEW (Phase 4) */
    int projects_scanned;
    double elapsed_ms;
} cbm_cross_repo_result_t;
```

- [ ] **Step 4: Implement `cbm_cross_repo_package_bridge`**

Mirror `cbm_cross_repo_match`'s structure: open source store, `delete_cross_edges`, iterate targets, open target store, build the target's export index, then scan source IMPORTS edges whose target is an `is_external` phantom matching the target's package name. For each, read `exported_name` from the edge props, look up the provider symbol in the export index, emit bidirectional edges via a new `emit_cross_import_bidirectional` that mirrors `emit_cross_route_bidirectional`:

```c
/* Forward (source DB): importer file -> phantom. Props carry provider symbol. */
char fwd[CR_PROPS_BUF];
snprintf(fwd, sizeof(fwd),
  "{\"target_project\":\"%s\",\"target_symbol\":\"%s\",\"target_file\":\"%s\","
  "\"target_qn\":\"%s\",\"imported_name\":\"%s\",\"pkg\":\"%s\","
  "\"strategy\":\"npm_export\",\"confidence\":%.2f}",
  tgt_project, sym->name, sym->file, sym->qn, imp_name, pkg, conf);
insert_cross_edge(src_store, src_project, file_id, phantom_id, "CROSS_IMPORTS", fwd);

/* Reverse (target DB): provider symbol -> its module. Props carry consumer. */
/* Look up a local node in target DB to anchor (the provider symbol's file
 * __file__ node, or the symbol's parent module). */
insert_cross_edge(tgt_store, tgt_project, sym_id, anchor_id, "CROSS_IMPORTED_BY", rev);
```

Package-name matching: the phantom QN is `<project>.@ctrip/train_rn_common`; the target's package name is read from its `package.json`. Match when the phantom's name/module_path equals (or starts with) the target package name.

- [ ] **Step 5: Run to verify it passes** — PASS (forward + reverse + idempotency).
- [ ] **Step 6: Commit** (`feat(crossrepo): bidirectional CROSS_IMPORTS bridge`).

### Task 3.3: Trigger integration — extend `handle_cross_repo_mode`

**Files:**
- Modify: `src/mcp/mcp.c` (`handle_cross_repo_mode` ~line 3005)

**Interfaces:**
- Consumes: `cbm_cross_repo_package_bridge` (Task 3.2).
- Produces: `index_repository(mode="cross-repo-intelligence")` JSON gains `cross_import_edges`.

- [ ] **Step 1: Write the failing test** — call the MCP tool `index_repository` with `mode="cross-repo-intelligence"` on a two-project fixture; assert the result JSON contains `cross_import_edges > 0`.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Extend `handle_cross_repo_mode`** — after the existing `cbm_cross_repo_match` call, call `cbm_cross_repo_package_bridge` with the same project + targets; merge its `cross_import_edges` into the result JSON.
- [ ] **Step 4: Run to verify it passes.**
- [ ] **Step 5: Commit** (`feat(mcp): wire package bridge into cross-repo-intelligence mode`).

---

## Phase 4 — CROSS_CALLS re-derivation (call-site → provider method)

### Task 4.1: Re-derive calls in bridged consumer files → CROSS_CALLS (bidirectional)

**Files:**
- Modify: `src/pipeline/pass_cross_repo.c`
- Test: `tests/test_cross_pkg_bridge.c` (append `cross_pkg_bridge_calls`)

**Interfaces:**
- Consumes: the bridged IMPORTS set from Phase 3 (to know which consumer files import a bridged package + the `exported_name`s bound); `cbm_extract_file` (re-extract calls, as `pass_calls.c` does); the export index.
- Produces: `CROSS_CALLS` (consumer call-site function → local unresolved call-target/phantom, props carry provider method) + `CROSS_CALLED_BY` reverse.

- [ ] **Step 1: Write the failing test**

```c
TEST(cross_pkg_bridge_calls) {
    /* Fixture: app.ts: import { TrainUBTLogUtil } from '@ctrip/lib';
     *          export function bookTicket() { TrainUBTLogUtil.ubtLog('x'); }
     * lib exports class/function TrainUBTLogUtil with method ubtLog.
     * After bridge: app DB has CROSS_CALLS from bookTicket -> (local unresolved
     * target), props target_symbol=ubtLog (or TrainUBTLogUtil.ubtLog). */
    PASS();
}
```

- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement CROSS_CALLS re-derivation**

In `cbm_cross_repo_package_bridge`, after the IMPORTS pass, for each consumer file with a bridged IMPORTS edge:

```c
/* Re-extract the file's calls (pass_calls.c does this with cbm_extract_file). */
CBMFileResult *r = cbm_extract_file(src, src_len, CBM_LANG_TYPESCRIPT,
                                    src_project, rel, CBM_EXTRACT_BUDGET, NULL, NULL);
for (int c = 0; c < r->calls.count; c++) {
    CBMCall *call = &r->calls.items[c];
    /* Match obj.method where obj is an imported bridged symbol, or a bare
     * call to an imported bridged symbol. Resolve the imported symbol's
     * exported_name from the file's IMPORTS edges (Phase 3 data). */
    const char *callee = call->callee_name;   /* e.g. "TrainUBTLogUtil.ubtLog" */
    /* Split into object + method; look up object's exported_name in the export
     * index (provider class/function), then the method is a member of that. */
    /* Emit CROSS_CALLS forward + CROSS_CALLED_BY reverse, mirroring §3. */
}
cbm_extract_file_free(r);
```

Caveats (documented): member-method resolution requires the provider's `TrainUBTLogUtil` to be a Class/namespace with an `ubtLog` member node; if `TrainUBTLogUtil` is a plain function and `ubtLog` is a property, link to the function with a lower confidence and note `target_member` in props. Reuse the existing result cache (`ctx->result_cache`) to avoid re-extracting on re-runs.

- [ ] **Step 4: Run to verify it passes.**
- [ ] **Step 5: Regression** — `cbm_extract_file` re-extraction cost: assert the bridge on a large fixture (many files) completes within a budget; only files with bridged IMPORTS edges are re-extracted.
- [ ] **Step 6: Commit** (`feat(crossrepo): CROSS_CALLS re-derivation from consumer call sites`).

---

## Phase 5 — Query-side cross-DB hop

### Task 5.1: `trace_path` recognizes CROSS_* edge types via `mode="cross_repo"`

**Files:**
- Modify: `src/mcp/mcp.c` (`resolve_trace_edge_types` + `handle_trace_call_path`)
- Test: `tests/test_trace_cross_repo.c` (new)

**Interfaces:**
- Consumes: the `mode` arg of `trace_path`.
- Produces: when `mode="cross_repo"`, the BFS edge-type set includes `CROSS_IMPORTS`/`CROSS_CALLS`/`CROSS_IMPORTED_BY`/`CROSS_CALLED_BY`.

- [ ] **Step 1: Write the failing test** — index two repos, bridge, call `trace_path(function_name="bookTicket", mode="cross_repo")`; assert the result includes a node marked `cross_project=true` from the provider repo.
- [ ] **Step 2: Run to verify it fails** (trace stops at the local phantom).
- [ ] **Step 3: Extend `resolve_trace_edge_types`** to add the four CROSS_* types when `mode="cross_repo"`. Keep the default mode unchanged (opt-in).
- [ ] **Step 4: Run to verify it fails differently** — BFS now reaches the local frontier but still does not hop (next task).
- [ ] **Step 5: Commit** (`feat(mcp): trace_path accepts mode=cross_repo for CROSS_* edges`).

### Task 5.2: Multi-store stitch BFS

**Files:**
- Modify: `src/mcp/mcp.c` (new `bfs_cross_store_stitch` helper + a per-server opened-store cache)
- Test: `tests/test_trace_cross_repo.c` (extend)

**Interfaces:**
- Consumes: `resolve_store` (mcp.c, existing), `cbm_store_bfs` (store.c, unchanged single-store), `cbm_store_find_node_by_qn`.
- Produces: a stitched traverse result with `cross_project`-marked hops.

- [ ] **Step 1: Write the failing test** — assert trace reaches the provider's `TrainUBTLogUtil` node and is marked `cross_project`; assert a cycle fixture (two CROSS_* edges pointing at each other) terminates; assert an unindexed-target fixture truncates with `target_not_indexed`; assert a deep chain truncates with `cross_depth_capped` at the budget.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement the stitch**

Add an opened-store cache to `cbm_mcp_server_t` (keyed by project name; `resolve_store` already opens a store — wrap it with a small `cbm_ht` cache). Then:

```c
/* After the in-store BFS (tr_out/tr_in), scan the frontier for CROSS_* edges
 * carrying target_project. For each, open the target store, find the node by
 * target_qn, run cbm_store_bfs from it, mark results cross_project=true, and
 * recurse with cross_hop_budget-- and a cross-store visited set. */
static int stitch_cross_hops(cbm_mcp_server_t *srv, const char *origin_project,
                             const cbm_traverse_result_t *frontier,
                             int budget, CBMHashTable *visited,
                             yyjson_mut_doc *doc, yyjson_mut_val *out_arr) {
    if (budget <= 0) { /* label cross_depth_capped */ return 0; }
    /* for each frontier edge with target_project in props:
     *   store = store_cache_get(srv, target_project);  // NULL -> target_not_indexed
     *   node = cbm_store_find_node_by_qn(store, target_project, target_qn);
     *   if (visited has target_qn) continue;
     *   cbm_store_bfs(store, node->id, direction, cross_edge_types, ...);
     *   mark cross_project=true; append to out_arr;
     *   recurse with frontier = this BFS result, budget-1. */
}
```

Wire it into `handle_trace_call_path` after the existing outbound/inbound BFS, only when `mode="cross_repo"`.

- [ ] **Step 4: Run to verify it passes** — all four cases (reach, cycle, unindexed, depth-cap).
- [ ] **Step 5: Commit** (`feat(mcp): multi-store stitch BFS for cross-repo trace`).

### Task 5.3: `get_code_snippet` switches store by QN project prefix

**Files:**
- Modify: `src/mcp/mcp.c` (`handle_get_code_snippet` ~line 3619)
- Test: `tests/test_trace_cross_repo.c` (extend)

- [ ] **Step 1: Write the failing test** — after trace reaches a provider symbol, call `get_code_snippet(qualified_name=<provider>.<...>.TrainUBTLogUtil)` from the App's MCP context; assert it returns the provider source snippet, not "not found".
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement the prefix switch**

In `handle_get_code_snippet`, parse the project prefix of `qualified_name` (the QN format is `<project>.<path.parts>.<name>` — the prefix is the longest leading segment that matches an indexed project name from `list_projects`). If it differs from the current `srv->store` project, `resolve_store(prefix)` and query there. Reuse the opened-store cache from Task 5.2.
- [ ] **Step 4: Run to verify it passes.**
- [ ] **Step 5: Commit** (`feat(mcp): get_code_snippet resolves cross-project QN prefixes`).

---

## Phase 6 — End-to-end integration test

### Task 6.1: Two-project fixture, full flow

**Files:**
- Test: `tests/test_cross_pkg_bridge.c` (final `cross_pkg_e2e`)

- [ ] **Step 1: Write the test** — build a realistic fake App + fake component library (mirroring TrainCRN/train_rn_common: App imports `@ctrip/lib`, calls `TrainUBTLogUtil.ubtLog()` from `bookTicket`), index both, run `index_repository(mode="cross-repo-intelligence")`, then `trace_path("bookTicket", mode="cross_repo")` and assert the chain reaches `ubtLog` in the provider; then `get_code_snippet` on the provider QN returns the real source.
- [ ] **Step 2: Run the full test suite** (`scripts/build.sh` + the test binary) — all PASS, including the existing 5604.
- [ ] **Step 3: Commit** (`test(crossrepo): two-project e2e fixture for cross-repo trace`).

---

## Self-Review (run after writing)

1. **Spec coverage:** §1 (edge types/props/idempotency) → Tasks 3.2, 3.3, 4.1. §2 (export index) → 3.1 (Enabler A removed, json_valid guard in 3.1). §3 (Enabler B exported_name, Enabler C phantom, CROSS_IMPORTS/CROSS_CALLS anchors) → 1.1, 2.1, 3.2, 4.1. §4 (trace edge types, multi-store stitch, get_code_snippet, scope bound) → 5.1, 5.2, 5.3. §5 (trigger/bidirectional/idempotency) → 3.2, 3.3. §6 (tests + regression) → every phase has TDD + the full-suite regression in 6.1. ✓
2. **Placeholder scan:** the `th_index_repo` helper name and the test-binary run command are flagged as "match the file's existing helper/pattern" — these are not placeholders but known-matching references; the executor confirms the exact symbol in `tests/test_helpers.h` before running. No TBD/TODO elsewhere.
3. **Type consistency:** `CBMImport.exported_name`, `cbm_pkg_export_index_t`, `cbm_cross_repo_package_bridge`, `CROSS_IMPORTS`/`CROSS_CALLS`/`CROSS_IMPORTED_BY`/`CROSS_CALLED_BY` — names used consistently across phases. ✓
