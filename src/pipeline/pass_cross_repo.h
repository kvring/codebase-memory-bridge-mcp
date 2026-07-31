/*
 * pass_cross_repo.h — Cross-repo intelligence: match Routes, Channels, and
 * async topics across indexed projects to create CROSS_* edges.
 */
#ifndef CBM_PASS_CROSS_REPO_H
#define CBM_PASS_CROSS_REPO_H

#include "store/store.h"

/* Result of a cross-repo matching run. */
typedef struct {
    int http_edges;    /* CROSS_HTTP_CALLS edges created */
    int async_edges;   /* CROSS_ASYNC_CALLS edges created */
    int channel_edges; /* CROSS_CHANNEL edges created */
    int grpc_edges;    /* CROSS_GRPC_CALLS edges created */
    int graphql_edges; /* CROSS_GRAPHQL_CALLS edges created */
    int trpc_edges;    /* CROSS_TRPC_CALLS edges created */
    int projects_scanned;
    double elapsed_ms;
} cbm_cross_repo_result_t;

/* Run cross-repo matching for `project` against `target_projects`.
 * If target_count == 1 and target_projects[0] == "*", matches against all
 * indexed projects. Writes CROSS_* edges bidirectionally into both the
 * source and target project DBs.
 *
 * `project` must already be indexed (its .db must exist).
 * Returns result with edge counts. */
cbm_cross_repo_result_t cbm_cross_repo_match(const char *project, const char **target_projects,
                                             int target_count);

/* ── Export index builder (Task 3.1) ────────────────────────────── */

/* Opaque export index handle. */
typedef struct cbm_pkg_export_index cbm_pkg_export_index_t;

/* Entry in the export index: maps (package_name, symbol_name) to provider details. */
typedef struct {
    const char *qn;        /* Qualified name of the exported symbol */
    int64_t node_id;       /* Node ID in the provider's DB */
    const char *file;      /* File path (relative) */
    const char *label;     /* Symbol type (Function, Class, etc.) */
} cbm_pkg_export_entry;

/* Build an export index from a provider's package.json entry point.
 * Reads provider_root/package.json to determine the entry point, then BFS
 * along IMPORTS edges to collect all is_exported=true nodes.
 * Returns a non-NULL index (even if empty) on success, NULL on setup failure. */
cbm_pkg_export_index_t *cbm_cross_pkg_build_export_index(cbm_store_t *provider_store,
                                                         const char *provider_project,
                                                         const char *provider_root);

/* Look up an exported symbol in the index.
 * Returns 0 if found, fills out; returns non-zero if not found. */
int cbm_cross_pkg_export_lookup(const cbm_pkg_export_index_t *ix, const char *pkg_name,
                                const char *symbol_name, cbm_pkg_export_entry *out);

/* Free the export index. */
void cbm_cross_pkg_export_index_free(cbm_pkg_export_index_t *ix);

#endif /* CBM_PASS_CROSS_REPO_H */
