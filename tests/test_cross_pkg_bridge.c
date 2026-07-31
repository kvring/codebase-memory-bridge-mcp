/*
 * test_cross_pkg_bridge.c — CROSS_IMPORTS / CROSS_IMPORTED_BY bridge tests
 * (Task 3.2). Two-project fixture: an App that imports an external npm package
 * and a Lib that exports it; the bridge links them bidirectionally.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "cbm.h"
#include "pipeline/pass_cross_repo.h"
#include <mcp/mcp.h>
#include <store/store.h>
#include <pipeline/pipeline.h>
#include <foundation/log.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Harness: index a temp repo, return its store + metadata ─────── */

typedef struct {
    char tmpdir[256];
    char dbpath[512];
    char *project;
    cbm_mcp_server_t *srv;
} BridgeProj;

typedef struct {
    const char *name;
    const char *content;
} BridgeFile;

static cbm_store_t *bridge_index(BridgeProj *bp, const char *tmpdir_root,
                                 const BridgeFile *files, int nfiles) {
    memset(bp, 0, sizeof(*bp));
    snprintf(bp->tmpdir, sizeof(bp->tmpdir), "%s", tmpdir_root);
    /* Create intermediate dirs + write files. */
    for (int i = 0; i < nfiles; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", bp->tmpdir, files[i].name);
        char *slash = strrchr(path, '/');
        if (slash && slash > path + strlen(bp->tmpdir)) {
            *slash = '\0';
            cbm_mkdir_p(path, 0755);
            *slash = '/';
        }
        FILE *f = fopen(path, "wb");
        if (!f) return NULL;
        fputs(files[i].content, f);
        fclose(f);
    }

    bp->project = cbm_project_name_from_path(bp->tmpdir);
    if (!bp->project) return NULL;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/codebase-memory-mcp", home);
    cbm_mkdir(cache_dir);
    snprintf(bp->dbpath, sizeof(bp->dbpath), "%s/%s.db", cache_dir, bp->project);
    unlink(bp->dbpath);

    bp->srv = cbm_mcp_server_new(NULL);
    if (!bp->srv) return NULL;

    char args[700];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", bp->tmpdir);
    char *resp = cbm_mcp_handle_tool(bp->srv, "index_repository", args);
    if (resp) free(resp);

    return cbm_store_open_path(bp->dbpath);
}

static void bridge_cleanup(BridgeProj *bp, cbm_store_t *store) {
    if (store) cbm_store_close(store);
    if (bp->srv) { cbm_mcp_server_free(bp->srv); bp->srv = NULL; }
    free(bp->project);
    bp->project = NULL;
    th_rmtree(bp->tmpdir);
    unlink(bp->dbpath);
    char wal[600], shm[600];
    snprintf(wal, sizeof(wal), "%s-wal", bp->dbpath);
    unlink(wal);
    snprintf(shm, sizeof(shm), "%s-shm", bp->dbpath);
    unlink(shm);
}

/* ── Test: bidirectional CROSS_IMPORTS + CROSS_IMPORTED_BY ───────── */

TEST(cross_pkg_bridge_imports_bidirectional) {
    /* Lib: package.json name=@ctrip/lib, main=src/index.ts; index.ts re-exports
     * TrainUBTLogUtil from utils; utils defines it. */
    const BridgeFile lib_files[] = {
        {"package.json", "{\"name\":\"@ctrip/lib\",\"main\":\"src/index.ts\"}\n"},
        {"src/index.ts", "export { TrainUBTLogUtil } from './utils';\n"},
        {"src/utils.ts", "export function TrainUBTLogUtil() { return 1; }\n"}};
    char lib_dir[256];
    snprintf(lib_dir, sizeof(lib_dir), "/tmp/cbm_bridgelib_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(lib_dir));
    BridgeProj lib_proj;
    cbm_store_t *lib_store = bridge_index(&lib_proj, lib_dir, lib_files, 3);
    ASSERT_NOT_NULL(lib_store);

    /* App: imports TrainUBTLogUtil from @ctrip/lib. */
    const BridgeFile app_files[] = {
        {"app.ts", "import { TrainUBTLogUtil } from '@ctrip/lib';\n"
                   "export function bookTicket() { return TrainUBTLogUtil(); }\n"}};
    char app_dir[256];
    snprintf(app_dir, sizeof(app_dir), "/tmp/cbm_bridgeapp_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(app_dir));
    BridgeProj app_proj;
    cbm_store_t *app_store = bridge_index(&app_proj, app_dir, app_files, 1);
    ASSERT_NOT_NULL(app_store);

    /* Close stores (the bridge opens its own). */
    cbm_store_close(app_store);
    cbm_store_close(lib_store);

    /* Run the bridge: App → Lib. */
    const char *targets[] = {lib_proj.project};
    cbm_cross_repo_result_t r =
        cbm_cross_repo_package_bridge(app_proj.project, targets, 1);
    ASSERT_GT(r.cross_import_edges, 0);

    /* Reopen App store; assert CROSS_IMPORTS edge exists. */
    cbm_store_t *app_check = cbm_store_open_path(app_proj.dbpath);
    ASSERT_NOT_NULL(app_check);
    int app_imports = cbm_store_count_edges_by_type(app_check, app_proj.project, "CROSS_IMPORTS");
    ASSERT_GT(app_imports, 0);

    /* Reopen Lib store; assert CROSS_IMPORTED_BY edge exists. */
    cbm_store_t *lib_check = cbm_store_open_path(lib_proj.dbpath);
    ASSERT_NOT_NULL(lib_check);
    int lib_imported_by =
        cbm_store_count_edges_by_type(lib_check, lib_proj.project, "CROSS_IMPORTED_BY");
    ASSERT_GT(lib_imported_by, 0);

    /* Idempotent: re-run; counts unchanged. */
    cbm_store_close(app_check);
    cbm_store_close(lib_check);
    cbm_cross_repo_result_t r2 =
        cbm_cross_repo_package_bridge(app_proj.project, targets, 1);
    app_check = cbm_store_open_path(app_proj.dbpath);
    lib_check = cbm_store_open_path(lib_proj.dbpath);
    ASSERT_EQ(app_imports, cbm_store_count_edges_by_type(app_check, app_proj.project, "CROSS_IMPORTS"));
    ASSERT_EQ(lib_imported_by,
               cbm_store_count_edges_by_type(lib_check, lib_proj.project, "CROSS_IMPORTED_BY"));
    (void)r2;

    cbm_store_close(app_check);
    cbm_store_close(lib_check);
    bridge_cleanup(&app_proj, NULL);
    bridge_cleanup(&lib_proj, NULL);
    PASS();
}

SUITE(cross_pkg_bridge) {
    RUN_TEST(cross_pkg_bridge_imports_bidirectional);
}
