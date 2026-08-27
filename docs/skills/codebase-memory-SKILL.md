---
name: codebase-memory
description: Use the codebase knowledge graph for structural code queries. Triggers on: explore the codebase, understand the architecture, what functions exist, show me the structure, who calls this function, what does X call, trace the call chain, find callers of, show dependencies, impact analysis, dead code, unused functions, high fan-out, refactor candidates, code quality audit, graph query syntax, Cypher query examples, edge types, how to use search_graph, cross-repo, cross-repository, npm package dependency, component library.
---

# Codebase Memory — Knowledge Graph Tools

Graph tools return precise structural results in ~500 tokens vs ~80K for grep.

## Quick Decision Matrix

| Question | Tool call |
|----------|----------|
| Who calls X? | `trace_path(direction="inbound")` |
| What does X call? | `trace_path(direction="outbound")` |
| Full call context | `trace_path(direction="both")` |
| **Cross-repo trace** (App → component library) | `trace_path(mode="cross_repo", direction="outbound")` |
| **Cross-repo impact** (library → who uses it) | `trace_path(mode="cross_repo", direction="inbound")` |
| Find by name pattern | `search_graph(name_pattern="...")` |
| Dead code | `search_graph(max_degree=0, exclude_entry_points=true)` |
| Cross-service edges | `query_graph` with Cypher |
| Impact of local changes | `detect_changes()` |
| Risk-classified trace | `trace_path(risk_labels=true)` |
| Text search | `search_code` or Grep |

## Indexing Modes

When indexing a project, ask the user which mode:

- **Standard** — `index_repository(repo_path=...)` — structure + calls + imports only.
- **Cross-repo intelligence** — `index_repository(repo_path=..., mode="cross-repo-intelligence", target_projects=["*"])` — also runs the npm package-import bridge: links external-package imports (`import {X} from '@scope/pkg'`) to the provider repo's exported symbols, creating `CROSS_IMPORTS` / `CROSS_CALLS` edges. Required for `trace_path(mode="cross_repo")`.
  **IMPORTANT: run the bridge on the CONSUMER (App) repo, not the PROVIDER (component library).**
  The consumer is the repo that `import`s the package; the provider is the repo whose `package.json` declares the package `name`.
  - ✅ `index_repository(repo_path="/path/to/App", mode="cross-repo-intelligence")` — App imports the lib
  - ❌ `index_repository(repo_path="/path/to/lib", mode="cross-repo-intelligence")` — lib is the provider, running the bridge here deletes its reverse edges (CROSS_IMPORTED_BY / CROSS_CALLED_BY) without rebuilding them
  - If both repos need cross-repo edges, run the bridge on the consumer repo only. The reverse edges are automatically written to the provider's DB.

**Recommend cross-repo-intelligence when multiple repos are indexed** (e.g. an App + a component library). Re-run after re-indexing either repo.

## Exploration Workflow
1. `list_projects` — check if project is indexed
2. `get_graph_schema` — understand node/edge types
3. `search_graph(label="Function", name_pattern=".*Pattern.*")` — find code
4. `get_code_snippet(qualified_name="project.path.FuncName")` — read source

## Tracing Workflow
1. `search_graph(name_pattern=".*FuncName.*")` — discover exact name
2. `trace_path(function_name="FuncName", direction="both", depth=3)` — trace
3. For cross-repo: add `mode="cross_repo"` — follows CROSS_IMPORTS/CROSS_CALLS edges across project DBs
4. `detect_changes()` — map git diff to affected symbols

## Quality Analysis
- Dead code: `search_graph(max_degree=0, exclude_entry_points=true)`
- High fan-out: `search_graph(min_degree=10, relationship="CALLS", direction="outbound")`
- High fan-in: `search_graph(min_degree=10, relationship="CALLS", direction="inbound")`

## 14 MCP Tools
`index_repository`, `index_status`, `list_projects`, `delete_project`,
`search_graph`, `search_code`, `trace_path`, `detect_changes`,
`query_graph`, `get_graph_schema`, `get_code_snippet`, `get_architecture`,
`manage_adr`, `ingest_traces`

## Edge Types
CALLS, HTTP_CALLS, ASYNC_CALLS, IMPORTS, DEFINES, DEFINES_METHOD,
HANDLES, IMPLEMENTS, OVERRIDE, USAGE, FILE_CHANGES_WITH,
CONTAINS_FILE, CONTAINS_FOLDER, CONTAINS_PACKAGE,
CROSS_IMPORTS, CROSS_CALLS, CROSS_IMPORTED_BY, CROSS_CALLED_BY

## Cross-Repo Package Bridge

When multiple repos are indexed and the bridge has run (`mode="cross-repo-intelligence"`):

- **CROSS_IMPORTS** — consumer file → provider exported symbol (npm import link)
- **CROSS_CALLS** — consumer call site → provider method (call link)
- **CROSS_IMPORTED_BY** — reverse: provider symbol → consumer file (impact analysis)
- **CROSS_CALLED_BY** — reverse: provider method → consumer call site

`trace_path(mode="cross_repo")` follows these edges across project DBs via multi-store stitch BFS (budget=3 hops, visited dedup). `get_code_snippet` auto-switches store by QN project prefix.

Supports babel module-resolver aliases (scope stripping: `train_rn_common` matches `@ctrip/train_rn_common`).

## Cypher Examples (for query_graph)
```
MATCH (a)-[r:HTTP_CALLS]->(b) RETURN a.name, b.name, r.url_path, r.confidence LIMIT 20
MATCH (f:Function) WHERE f.name =~ '.*Handler.*' RETURN f.name, f.file_path
MATCH (a)-[r:CALLS]->(b) WHERE a.name = 'main' RETURN b.name
```

## Gotchas
1. `search_graph(relationship="HTTP_CALLS")` filters nodes by degree — use `query_graph` with Cypher to see actual edges.
2. `query_graph` has a 200-row cap — use `search_graph` with degree filters for counting.
3. `trace_path` needs exact names — use `search_graph(name_pattern=...)` first.
4. `direction="outbound"` misses cross-service callers — use `direction="both"`.
5. Results default to 10 per page — check `has_more` and use `offset`.
6. `mode="cross_repo"` is opt-in — without it, trace stays within the current project's DB.
7. Cross-repo bridge edges must be (re)built after indexing — run `index_repository(mode="cross-repo-intelligence")`.
