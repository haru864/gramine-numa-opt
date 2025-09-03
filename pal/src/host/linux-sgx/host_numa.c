// numactl-devel / libnuma がある場合は -DHAVE_LIBNUMA と -lnuma を付けてビルド
// gcc main.c -o app -DHAVE_LIBNUMA -lnuma
#define _GNU_SOURCE
#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif

#include "host_internal.h"

static int parse_node_index(const char* path) {
    // path 例: "/sys/devices/system/node/node0"
    const char* p = strrchr(path, '/');
    if (!p)
        return -1;
    // "node0" の "0" 部分を読む
    int idx = -1;
    if (sscanf(p, "/node%d", &idx) == 1 && idx >= 0) {
        return idx;
    }
    return -1;
}

static int enumerate_nodes_sysfs(int** node_ids, int* node_count) {
    glob_t g = {0};
    int ret  = glob("/sys/devices/system/node/node*", 0, NULL, &g);
    if (ret != 0) {
        globfree(&g);
        return -1;
    }

    // 見つかったものから node 番号を抽出
    int* ids = malloc(sizeof(int) * g.gl_pathc);
    if (!ids) {
        globfree(&g);
        return -1;
    }
    int n = 0;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        int idx = parse_node_index(g.gl_pathv[i]);
        if (idx >= 0) {
            ids[n++] = idx;
        }
    }
    globfree(&g);

    if (n == 0) {
        free(ids);
        return -1;
    }

    // ソート（昇順）
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (ids[i] > ids[j]) {
                int tmp = ids[i];
                ids[i]  = ids[j];
                ids[j]  = tmp;
            }
        }
    }

    *node_ids   = ids;
    *node_count = n;
    return 0;
}

#ifdef HAVE_LIBNUMA
static int enumerate_nodes_numa(int** out_node_ids, int* out_count) {
    if (numa_available() < 0) {
        return -1;
    }

    // 推奨：/sys と同じく実在ノードを列挙。ここでは簡単化のため 0..numa_max_node()
    int maxn = numa_max_node();  // 最大ノード番号
    if (maxn < 0)
        return -1;

    // 実在性チェック用に bitmask を取得（無い環境もあるのでオプション）
    struct bitmask* bm = numa_allocate_nodemask();
    if (!bm)
        return -1;
    numa_bitmask_clearall(bm);
    // 可能なら、すべてのノードを一旦有効にして mask を使うが、
    // libnuma だけで “存在するノード” の正確な列挙が難しいこともあるため、
    // シンプルに 0..maxn を返し、足りなければ sysfs にフォールバックでも良い。

    int* ids = malloc(sizeof(int) * (maxn + 1));
    if (!ids) {
        numa_free_nodemask(bm);
        return -1;
    }
    int n = 0;
    for (int i = 0; i <= maxn; i++) {
        // 厳密にやるなら sysfs 確認か、numa_node_of_cpu() 等で存在性推定
        ids[n++] = i;
    }
    numa_free_nodemask(bm);

    *out_node_ids = ids;
    *out_count    = n;
    return 0;
}
#endif

int init_enclave_regions(char* manifest, struct pal_enclave* pe) {
    if (!pe)
        return -EINVAL;

    int* node_ids = NULL;
    int nnodes    = 0;

#ifdef HAVE_LIBNUMA
    // 1) libnuma で列挙を試みる
    if (enumerate_nodes_numa(&node_ids, &nnodes) != 0) {
        node_ids = NULL;
        nnodes   = 0;
    }
#endif
    // 2) 失敗 or 未対応なら sysfs グロブ
    if (!node_ids || nnodes <= 0) {
        if (enumerate_nodes_sysfs(&node_ids, &nnodes) != 0) {
            return -ENODEV;  // NUMA未対応 or 列挙失敗
        }
    }

    pe->regions_size = nnodes;

    pe->regions = calloc((size_t)pe->regions_size, sizeof(struct enclave_region*));
    if (!pe->regions) {
        free(node_ids);
        return -ENOMEM;
    }

    for (int i = 0; i < pe->regions_size; i++) {
        pe->regions[i] = calloc(1, sizeof(struct enclave_region));
        if (!pe->regions[i]) {
            for (int k = 0; k < i; k++) free(pe->regions[k]);
            free(pe->regions);
            pe->regions      = NULL;
            pe->regions_size = 0;
            free(node_ids);
            return -ENOMEM;
        }
        pe->regions[i]->node_id  = node_ids[i];
        pe->regions[i]->baseaddr = DEFAULT_ENCLAVE_BASE;
        pe->regions[i]->size     = 0;  // FIX
    }

    free(node_ids);
    return 0;
}

// 破棄用
void destroy_enclave_regions(struct pal_enclave* pe) {
    if (!pe || !pe->regions)
        return;
    for (int i = 0; i < pe->regions_size; i++) {
        free(pe->regions[i]);
    }
    free(pe->regions);
    pe->regions      = NULL;
    pe->regions_size = 0;
}
