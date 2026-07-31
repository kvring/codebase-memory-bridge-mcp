/*
 * cross_pkg_export.c — Provider export-index builder.
 *
 * Reads a provider's package.json to determine the entry point, then BFS along
 * IMPORTS edges to collect all is_exported=true nodes. Maps (package_name, symbol_name)
 * to {qn, node_id, file, label}.
 *
 * The index is built from the provider's persisted SQLite DB (post-index) and is
 * held in-memory (transient, not persisted).
 */
#include "pipeline/pass_cross_repo.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "pipeline/pipeline.h"

#include <yyjson/yyjson.h>
#include <sqlite3/sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    PKG_EXPORT_BUF_PATH = 1024,
    PKG_EXPORT_BUF_QN = 512,
    PKG_EXPORT_BUF_KEY = 256,
    PKG_EXPORT_MAX_DEPTH = 8,    /* BFS depth cap to break cycles */
    PKG_EXPORT_MAX_NODES = 2048, /* Hard limit on nodes per index */
};

/* ── Data structures ────────────────────────────────────────────── */

/* Internal entry struct with depth tracking and heap-allocated strings */
typedef struct {
    char *qn;              /* Heap-allocated copy */
    int64_t node_id;
    char *file;            /* Heap-allocated copy */
    char *label;           /* Heap-allocated copy */
    int depth;             /* Depth at which this entry was added (shallower = higher priority) */
} internal_entry_t;

/* The index: hash table keyed by "pkg_name\0symbol_name", values are internal_entry_t* */
struct cbm_pkg_export_index {
    CBMHashTable *ht;
    int count;
};

/* ── Helpers ────────────────────────────────────────────────────── */

/* Build a composite key: "pkg_name\0symbol_name" */
static char *build_key(const char *pkg_name, const char *symbol_name) {
    size_t pkg_len = strlen(pkg_name);
    size_t sym_len = strlen(symbol_name);
    char *key = (char *)malloc(pkg_len + sym_len + 2);
    if (!key) return NULL;
    memcpy(key, pkg_name, pkg_len);
    key[pkg_len] = '\0';
    memcpy(key + pkg_len + 1, symbol_name, sym_len);
    key[pkg_len + sym_len + 1] = '\0';
    return key;
}

/* Extract symbol name from a qualified_name (last component after the last dot).
 * Returns pointer into qn, or qn itself if no dots. */
static const char *extract_symbol_name(const char *qn) {
    const char *last_dot = strrchr(qn, '.');
    return last_dot ? last_dot + 1 : qn;
}

/* Free an internal entry */
static void free_internal_entry(internal_entry_t *entry) {
    if (!entry) return;
    free(entry->qn);
    free(entry->file);
    free(entry->label);
    free(entry);
}

/* ── BFS collection ────────────────────────────────────────────── */

/* Forward declaration for recursive BFS */
static void collect_exported_defs(struct sqlite3 *db, const char *project,
                                  int64_t entry_id, int depth, int max_depth,
                                  const char *pkg_name, cbm_pkg_export_index_t *ix);

/* Collect is_exported nodes from a module and follow IMPORTS edges. */
static void collect_exported_defs(struct sqlite3 *db, const char *project,
                                  int64_t entry_id, int depth, int max_depth,
                                  const char *pkg_name, cbm_pkg_export_index_t *ix) {
    if (depth >= max_depth || ix->count >= PKG_EXPORT_MAX_NODES) {
        return;
    }

    /* 1. Collect is_exported nodes from the current module's file. */
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, name, qualified_name, file_path, label FROM nodes "
        "WHERE project=?1 AND file_path=(SELECT file_path FROM nodes WHERE id=?2) "
        "AND json_valid(properties) "
        "AND json_extract(properties,'$.is_exported')='true' "
        "AND label IN ('Function','Class','Variable','Interface','Type','Method')";

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(st, 1, project, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, entry_id);

    while (sqlite3_step(st) == SQLITE_ROW && ix->count < PKG_EXPORT_MAX_NODES) {
        int64_t node_id = sqlite3_column_int64(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        const char *qn = (const char *)sqlite3_column_text(st, 2);
        const char *file = (const char *)sqlite3_column_text(st, 3);
        const char *label = (const char *)sqlite3_column_text(st, 4);

        if (!name || !qn || !file) continue;

        /* Extract just the symbol name (last component of QN) */
        const char *symbol_name = extract_symbol_name(qn);

        /* Build composite key */
        char *key = build_key(pkg_name, symbol_name);
        if (!key) continue;

        /* Check for existing entry to apply conflict policy */
        internal_entry_t *existing = (internal_entry_t *)cbm_ht_get(ix->ht, key);
        if (existing != NULL) {
            /* Keep entry at shallower depth; on tie, prefer lex-smaller QN */
            if (depth < existing->depth ||
                (depth == existing->depth && strcmp(qn, existing->qn) < 0)) {
                /* Replace with new entry */
                free_internal_entry(existing);

                internal_entry_t *new_entry = (internal_entry_t *)malloc(sizeof(internal_entry_t));
                if (!new_entry) {
                    free(key);
                    continue;
                }
                new_entry->qn = strdup(qn);
                new_entry->node_id = node_id;
                new_entry->file = strdup(file);
                new_entry->label = strdup(label);
                new_entry->depth = depth;

                cbm_ht_set(ix->ht, key, new_entry);
            }
            free(key);
            continue;
        }

        /* New entry: allocate and insert */
        internal_entry_t *new_entry = (internal_entry_t *)malloc(sizeof(internal_entry_t));
        if (!new_entry) {
            free(key);
            continue;
        }
        new_entry->qn = strdup(qn);
        new_entry->node_id = node_id;
        new_entry->file = strdup(file);
        new_entry->label = strdup(label);
        new_entry->depth = depth;

        cbm_ht_set(ix->ht, key, new_entry);
        ix->count++;
        free(key);
    }

    sqlite3_finalize(st);

    /* 2. Follow outgoing IMPORTS edges to re-exported modules. */
    if (sqlite3_prepare_v2(db, "SELECT target_id FROM edges WHERE source_id=?1 AND type='IMPORTS'",
                          -1, &st, NULL) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_int64(st, 1, entry_id);

    while (sqlite3_step(st) == SQLITE_ROW && ix->count < PKG_EXPORT_MAX_NODES) {
        int64_t target_id = sqlite3_column_int64(st, 0);
        collect_exported_defs(db, project, target_id, depth + 1, max_depth, pkg_name, ix);
    }

    sqlite3_finalize(st);
}

/* ── Public API ────────────────────────────────────────────────── */

cbm_pkg_export_index_t *cbm_cross_pkg_build_export_index(cbm_store_t *provider_store,
                                                         const char *provider_project,
                                                         const char *provider_root) {
    if (!provider_store || !provider_project || !provider_root) {
        return NULL;
    }

    /* Allocate the index structure */
    cbm_pkg_export_index_t *ix = (cbm_pkg_export_index_t *)malloc(sizeof(*ix));
    if (!ix) {
        return NULL;
    }

    ix->ht = cbm_ht_create(64);
    if (!ix->ht) {
        free(ix);
        return NULL;
    }
    ix->count = 0;

    /* Read package.json from provider_root */
    char pkg_json_path[PKG_EXPORT_BUF_PATH];
    snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/package.json", provider_root);

    FILE *f = fopen(pkg_json_path, "rb");
    if (!f) {
        cbm_log_info("pkg_export: package.json not found", "path", pkg_json_path);
        return ix; /* Return empty index */
    }

    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > (1024 * 1024)) {
        fclose(f);
        return ix;
    }

    char *content = (char *)malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        return ix;
    }

    size_t nread = fread(content, 1, (size_t)size, f);
    fclose(f);
    content[nread] = '\0';

    /* Parse package.json */
    yyjson_doc *doc = yyjson_read(content, (size_t)nread, 0);
    if (!doc) {
        cbm_log_info("pkg_export: failed to parse package.json", "path", pkg_json_path);
        free(content);
        return ix;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        free(content);
        return ix;
    }

    /* Extract package name and make a copy */
    yyjson_val *name_val = yyjson_obj_get(root, "name");
    if (!yyjson_is_str(name_val)) {
        yyjson_doc_free(doc);
        free(content);
        return ix;
    }

    const char *pkg_name = yyjson_get_str(name_val);
    if (!pkg_name || !pkg_name[0]) {
        yyjson_doc_free(doc);
        free(content);
        return ix;
    }

    /* Make a copy of the package name since it will become invalid after yyjson_doc_free */
    char *pkg_name_copy = strdup(pkg_name);
    if (!pkg_name_copy) {
        yyjson_doc_free(doc);
        free(content);
        return ix;
    }

    /* Resolve entry point: try exports["."], then main, then module, then default */
    char entry_rel_buf[512] = {0};
    const char *entry_rel = "src/index.ts"; /* default */

    yyjson_val *exports = yyjson_obj_get(root, "exports");
    if (yyjson_is_obj(exports)) {
        yyjson_val *dot = yyjson_obj_get(exports, ".");
        if (yyjson_is_str(dot)) {
            entry_rel = yyjson_get_str(dot);
        } else if (yyjson_is_obj(dot)) {
            /* exports["."] is an object with import/require/types keys */
            yyjson_val *import_key = yyjson_obj_get(dot, "import");
            if (yyjson_is_str(import_key)) {
                entry_rel = yyjson_get_str(import_key);
            } else {
                yyjson_val *require_key = yyjson_obj_get(dot, "require");
                if (yyjson_is_str(require_key)) {
                    entry_rel = yyjson_get_str(require_key);
                }
            }
        }
    }

    if (!entry_rel || !entry_rel[0]) {
        yyjson_val *main_val = yyjson_obj_get(root, "main");
        if (yyjson_is_str(main_val)) {
            entry_rel = yyjson_get_str(main_val);
        }
    }

    if (!entry_rel || !entry_rel[0]) {
        yyjson_val *module_val = yyjson_obj_get(root, "module");
        if (yyjson_is_str(module_val)) {
            entry_rel = yyjson_get_str(module_val);
        }
    }

    /* Make a copy of entry_rel before freeing the yyjson doc */
    snprintf(entry_rel_buf, sizeof(entry_rel_buf), "%s", entry_rel && entry_rel[0] ? entry_rel : "src/index.ts");

    yyjson_doc_free(doc);
    free(content);

    /* Convert entry relative path to module QN */
    char *entry_qn = cbm_pipeline_fqn_module(provider_project, entry_rel_buf);
    if (!entry_qn) {
        cbm_log_info("pkg_export: failed to build entry QN", "entry", entry_rel_buf);
        free(pkg_name_copy);
        return ix;
    }

    /* Look up the entry node in the provider's DB */
    cbm_node_t entry_node;
    if (cbm_store_find_node_by_qn(provider_store, provider_project, entry_qn, &entry_node) !=
        CBM_STORE_OK) {
        free(entry_qn);
        cbm_log_info("pkg_export: entry node not found in DB", "qn", entry_qn);
        free(pkg_name_copy);
        return ix; /* Return empty index */
    }

    free(entry_qn);

    /* Get the SQLite DB handle and start BFS */
    struct sqlite3 *db = cbm_store_get_db(provider_store);
    if (!db) {
        cbm_log_info("pkg_export: failed to get DB handle", "project", provider_project);
        free(pkg_name_copy);
        return ix;
    }

    collect_exported_defs(db, provider_project, entry_node.id, 0, PKG_EXPORT_MAX_DEPTH, pkg_name_copy, ix);
    free(pkg_name_copy);

    return ix;
}

int cbm_cross_pkg_export_lookup(const cbm_pkg_export_index_t *ix, const char *pkg_name,
                                const char *symbol_name, cbm_pkg_export_entry *out) {
    if (!ix || !out) {
        return -1;
    }

    char *key = build_key(pkg_name, symbol_name);
    if (!key) {
        return -1;
    }

    internal_entry_t *entry = (internal_entry_t *)cbm_ht_get(ix->ht, key);
    free(key);

    if (!entry) {
        return -1;
    }

    out->qn = entry->qn;
    out->node_id = entry->node_id;
    out->file = entry->file;
    out->label = entry->label;
    return 0;
}

void cbm_cross_pkg_export_index_free(cbm_pkg_export_index_t *ix) {
    if (!ix) {
        return;
    }

    /* Free all entries in the hash table */
    if (ix->ht) {
        /* We can't iterate and delete safely with cbm_ht, so we just free the table.
         * This is a limitation of the hash table API, but the table itself should
         * be freed. For proper cleanup, we'd need cbm_ht_foreach with a free callback. */
        cbm_ht_free(ix->ht);
    }

    free(ix);
}
