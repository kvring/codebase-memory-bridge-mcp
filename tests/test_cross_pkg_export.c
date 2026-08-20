/*
 * test_cross_pkg_export.c — Tests for provider export-index builder.
 *
 * Tests the export-index builder that maps (package_name, symbol_name) to
 * exported definitions reachable from a package's entry point.
 */

#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "cbm.h"
#include <mcp/mcp.h>
#include <store/store.h>
#include <pipeline/pipeline.h>
#include <pipeline/pass_cross_repo.h>
#include <foundation/log.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Test harness (mirrors test_edge_imports.c) ────────────────────── */

typedef struct {
    char tmpdir[256];
    char dbpath[512];
    char *project;
    cbm_mcp_server_t *srv;
} CPELangProj;

typedef struct {
    const char *name;    /* relative filename, may include '/' for subdirs */
    const char *content;
} CPEFile;

static void cpe_to_fwd_slashes(char *p) {
    for (; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* Write files, run index_repository, open graph DB. Returns NULL on failure. */
static cbm_store_t *cpe_index_files(CPELangProj *lp, const CPEFile *files, int nfiles) {
    memset(lp, 0, sizeof(*lp));
    snprintf(lp->tmpdir, sizeof(lp->tmpdir), "/tmp/cbm_cpe_XXXXXX");
    if (!cbm_mkdtemp(lp->tmpdir)) return NULL;
    cpe_to_fwd_slashes(lp->tmpdir);

    for (int i = 0; i < nfiles; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", lp->tmpdir, files[i].name);
        /* Create intermediate directories for sub-path fixtures. */
        char *slash = strrchr(path, '/');
        if (slash && slash > path + strlen(lp->tmpdir)) {
            *slash = '\0';
            cbm_mkdir_p(path, 0755);
            *slash = '/';
        }
        FILE *f = fopen(path, "wb");
        if (!f) return NULL;
        fputs(files[i].content, f);
        fclose(f);
    }

    lp->project = cbm_project_name_from_path(lp->tmpdir);
    if (!lp->project) return NULL;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/codebase-memory-mcp", home);
    cbm_mkdir(cache_dir);
    snprintf(lp->dbpath, sizeof(lp->dbpath), "%s/%s.db", cache_dir, lp->project);
    unlink(lp->dbpath);

    lp->srv = cbm_mcp_server_new(NULL);
    if (!lp->srv) return NULL;

    char args[700];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", lp->tmpdir);
    char *resp = cbm_mcp_handle_tool(lp->srv, "index_repository", args);
    if (resp) free(resp);

    return cbm_store_open_path(lp->dbpath);
}

static void cpe_cleanup(CPELangProj *lp, cbm_store_t *store) {
    if (store) cbm_store_close(store);
    if (lp->srv) { cbm_mcp_server_free(lp->srv); lp->srv = NULL; }
    free(lp->project);
    lp->project = NULL;
    th_rmtree(lp->tmpdir);
    unlink(lp->dbpath);
    char wal[600], shm[600];
    snprintf(wal, sizeof(wal), "%s-wal", lp->dbpath);
    unlink(wal);
    snprintf(shm, sizeof(shm), "%s-shm", lp->dbpath);
    unlink(shm);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

TEST(cross_pkg_export_resolves_reexport) {
    /* Fixture: a component-library repo:
     *   package.json  {"name":"@ctrip/lib","main":"src/index.ts"}
     *   src/index.ts  export { TrainUBTLogUtil } from './utils';
     *   src/utils.ts  export function TrainUBTLogUtil() {}
     * After indexing, the export index must map ("@ctrip/lib","TrainUBTLogUtil")
     * to the utils.ts def node. */

    const CPEFile files[] = {
        {
            "package.json",
            "{\"name\":\"@ctrip/lib\",\"main\":\"src/index.ts\"}\n",
        },
        {
            "src/index.ts",
            "export { TrainUBTLogUtil } from './utils';\n",
        },
        {
            "src/utils.ts",
            "export function TrainUBTLogUtil() {}\n",
        },
    };

    CPELangProj lp;
    cbm_store_t *store = cpe_index_files(&lp, files, 3);
    ASSERT_NOT_NULL(store);

    /* Build the export index */
    cbm_pkg_export_index_t *ix = cbm_cross_pkg_build_export_index(store, lp.project, lp.tmpdir);
    ASSERT_NOT_NULL(ix);

    /* Look up the exported symbol */
    cbm_pkg_export_entry e;
    ASSERT_EQ(0, cbm_cross_pkg_export_lookup(ix, "@ctrip/lib", "TrainUBTLogUtil", &e));

    /* Verify the entry points to the utils.ts definition */
    ASSERT_NOT_NULL(e.qn);
    ASSERT(strstr(e.qn, "TrainUBTLogUtil") != NULL);
    ASSERT_NOT_NULL(e.file);
    ASSERT(strstr(e.file, "utils") != NULL);
    ASSERT_NOT_NULL(e.label);
    ASSERT_STR_EQ(e.label, "Function");

    cbm_cross_pkg_export_index_free(ix);
    cpe_cleanup(&lp, store);
    PASS();
}

/* npm convention: main/exports carry a "./" prefix ("./src/index.ts").
 * The entry QN must strip it, else tokenize_path keeps "." as a literal
 * segment and the entry node is never found (empty index). */
TEST(cross_pkg_export_dot_slash_entry) {
    const CPEFile files[] = {
        {
            "package.json",
            "{\"name\":\"@ctrip/lib\",\"main\":\"./src/index.ts\"}\n",
        },
        {
            "src/index.ts",
            "export { TrainUBTLogUtil } from './utils';\n",
        },
        {
            "src/utils.ts",
            "export function TrainUBTLogUtil() {}\n",
        },
    };

    CPELangProj lp;
    cbm_store_t *store = cpe_index_files(&lp, files, 3);
    ASSERT_NOT_NULL(store);

    cbm_pkg_export_index_t *ix = cbm_cross_pkg_build_export_index(store, lp.project, lp.tmpdir);
    ASSERT_NOT_NULL(ix);

    cbm_pkg_export_entry e;
    if (cbm_cross_pkg_export_lookup(ix, "@ctrip/lib", "TrainUBTLogUtil", &e) != 0) {
        FAIL("dot-slash main entry must resolve like a bare path");
    }
    ASSERT_NOT_NULL(e.qn);
    ASSERT(strstr(e.qn, "TrainUBTLogUtil") != NULL);

    cbm_cross_pkg_export_index_free(ix);
    cpe_cleanup(&lp, store);
    PASS();
}

TEST(cross_pkg_export_cycle_terminates) {
    /* Fixture with a re-export cycle:
     *   package.json  {"name":"@ctrip/cyclic","main":"src/a.ts"}
     *   src/a.ts      export * from './b';
     *   src/b.ts      export * from './c';
     *   src/c.ts      export * from './a';
     *   src/item.ts   export function Item() {}
     * The BFS should terminate due to depth cap without hanging. */

    const CPEFile files[] = {
        {
            "package.json",
            "{\"name\":\"@ctrip/cyclic\",\"main\":\"src/a.ts\"}\n",
        },
        {
            "src/a.ts",
            "export * from './b';\n",
        },
        {
            "src/b.ts",
            "export * from './c';\n",
        },
        {
            "src/c.ts",
            "export * from './a';\nexport function Item() {}\n",
        },
    };

    CPELangProj lp;
    cbm_store_t *store = cpe_index_files(&lp, files, 4);
    ASSERT_NOT_NULL(store);

    /* Build the export index — this should not hang */
    cbm_pkg_export_index_t *ix = cbm_cross_pkg_build_export_index(store, lp.project, lp.tmpdir);
    ASSERT_NOT_NULL(ix);

    /* The index should have terminated successfully */
    ASSERT(ix != NULL); /* Already tested, but explicit here */

    cbm_cross_pkg_export_index_free(ix);
    cpe_cleanup(&lp, store);
    PASS();
}

/* ── Suite registration ──────────────────────────────────────────── */

SUITE(cross_pkg_export) {
    RUN_TEST(cross_pkg_export_resolves_reexport);
    RUN_TEST(cross_pkg_export_dot_slash_entry);
    RUN_TEST(cross_pkg_export_cycle_terminates);
}
