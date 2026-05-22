#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_API.h"
#include "Zend/zend_gc.h"
#include "Zend/zend_objects.h"
#include "Zend/zend_objects_API.h"
#include "Zend/zend_object_handlers.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_hash.h"
#include "php_gcprune.h"

#include <string.h>
#include <stdint.h>

ZEND_DECLARE_MODULE_GLOBALS(gcprune)

static zend_object_handlers gcprune_handlers;

static HashTable *gcprune_get_gc(zend_object *obj, zval **table, int *n);
static void       gcprune_free_obj(zend_object *obj);
static zend_object *gcprune_create_object(zend_class_entry *ce);

static void       gcprune_run_sync(void);
static gcp_node  *gcp_node_get_or_create(zend_object *obj);
static void       gcp_node_dtor(zval *zv);

static void       gcp_classmap_set(zend_class_entry *ce, gcp_kind kind);
static gcp_kind   gcp_classmap_kind(zend_class_entry *ce);

static uint64_t   gcp_fingerprint(HashTable *ht);
static HashTable *gcp_find_largest_container(zend_object *obj, uint32_t *size_out);
static gcp_strategy gcp_pick_strategy(zend_object *obj, gcp_node *node);
static void       gcp_build_pruned_buffer(zend_object *obj, gcp_node *node, gcp_strategy strat);

PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("gcprune.enabled",            "1",     PHP_INI_ALL, OnUpdateBool, ini_enabled,            zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.node_budget",        "5000",  PHP_INI_ALL, OnUpdateLong, ini_node_budget,        zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.large_threshold",    "256",   PHP_INI_ALL, OnUpdateLong, ini_large_threshold,    zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.full_scan_interval", "8",     PHP_INI_ALL, OnUpdateLong, ini_full_scan_interval, zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.depth_full",         "2",     PHP_INI_ALL, OnUpdateLong, ini_depth_full,         zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.depth_partial",      "5",     PHP_INI_ALL, OnUpdateLong, ini_depth_partial,      zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.sample_stride",      "16",    PHP_INI_ALL, OnUpdateLong, ini_sample_stride,      zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.cost_gate",          "2000",  PHP_INI_ALL, OnUpdateLong, ini_cost_gate,          zend_gcprune_globals, gcprune_globals)
    STD_PHP_INI_ENTRY  ("gcprune.stability_gate",     "0.85",  PHP_INI_ALL, OnUpdateReal, ini_stability_gate,     zend_gcprune_globals, gcprune_globals)
PHP_INI_END()

static void gcp_node_dtor(zval *zv)
{
    gcp_node *n = (gcp_node *) Z_PTR_P(zv);
    if (!n) return;
    if (n->buf) efree(n->buf);
    efree(n);
}

static gcp_node *gcp_node_get_or_create(zend_object *obj)
{
    gcp_node *n = (gcp_node *) zend_hash_index_find_ptr(&GCPRUNE_G(nodes), (zend_ulong) obj->handle);
    if (n) return n;
    n = (gcp_node *) ecalloc(1, sizeof(*n));
    n->obj_handle = obj->handle;
    n->depth_hint = UINT16_MAX;
    n->stability  = 0.0;
    n->buf_len    = 0;
    zend_hash_index_add_new_ptr(&GCPRUNE_G(nodes), (zend_ulong) obj->handle, n);
    return n;
}

static void gcp_node_evict(zend_object *obj)
{
    zend_hash_index_del(&GCPRUNE_G(nodes), (zend_ulong) obj->handle);
}

static void gcp_classmap_set(zend_class_entry *ce, gcp_kind kind)
{
    zend_hash_index_update_ptr(&GCPRUNE_G(classmap),
        (zend_ulong)(uintptr_t) ce,
        (void *)(uintptr_t) kind);
}

static gcp_kind gcp_classmap_kind(zend_class_entry *ce)
{
    void *v = zend_hash_index_find_ptr(&GCPRUNE_G(classmap),
        (zend_ulong)(uintptr_t) ce);
    return v ? (gcp_kind)(uintptr_t) v : GCP_KIND_UNKNOWN;
}

static uint64_t gcp_fingerprint(HashTable *ht)
{
    uint64_t h = 14695981039346656037ULL;
    #define MIX(x) do { h ^= (uint64_t)(uintptr_t)(x); h *= 1099511628211ULL; } while (0)

    MIX(zend_hash_num_elements(ht));
    MIX(ht->nNumUsed);
    MIX(ht->arData);

    uint32_t nu   = ht->nNumUsed;
    uint32_t step = nu > 256 ? (nu / 256) : 1;
    uint32_t i    = 0;
    zval    *v;
    ZEND_HASH_FOREACH_VAL(ht, v) {
        if ((i % step) == 0 && Z_COLLECTABLE_P(v)) {
            MIX(Z_COUNTED_P(v));
        }
        i++;
    } ZEND_HASH_FOREACH_END();

    #undef MIX
    return h;
}

static HashTable *gcp_find_largest_container(zend_object *obj, uint32_t *size_out)
{
    HashTable *largest   = NULL;
    uint32_t   largest_n = 0;

    zval *p   = obj->properties_table;
    zval *end = p + obj->ce->default_properties_count;
    for (; p < end; p++) {
        zval *zv = p;
        if (Z_TYPE_P(zv) == IS_UNDEF) continue;
        ZVAL_DEREF(zv);
        if (Z_TYPE_P(zv) == IS_ARRAY) {
            uint32_t n = zend_hash_num_elements(Z_ARRVAL_P(zv));
            if (n > largest_n) { largest_n = n; largest = Z_ARRVAL_P(zv); }
        }
    }
    if (obj->properties) {
        zval *zv;
        ZEND_HASH_FOREACH_VAL(obj->properties, zv) {
            ZVAL_DEREF(zv);
            if (Z_TYPE_P(zv) == IS_ARRAY) {
                uint32_t n = zend_hash_num_elements(Z_ARRVAL_P(zv));
                if (n > largest_n) { largest_n = n; largest = Z_ARRVAL_P(zv); }
            }
        } ZEND_HASH_FOREACH_END();
    }
    *size_out = largest_n;
    return largest;
}

typedef struct {
    zval     *buf;
    int       cap;
    int       len;
    uint32_t  depth_full;
    uint32_t  depth_partial;
    uint32_t  base_stride;
} gcp_ctx;

static inline void gcp_emit(gcp_ctx *c, zval *zv)
{
    if (c->len == c->cap) {
        int new_cap = c->cap ? c->cap * 2 : 64;
        c->buf = (zval *) erealloc(c->buf, (size_t) new_cap * sizeof(zval));
        c->cap = new_cap;
    }
    ZVAL_COPY_VALUE(&c->buf[c->len], zv);
    c->len++;
}

static void gcp_walk_ht(gcp_ctx *c, HashTable *ht, uint32_t depth)
{
    uint32_t shift  = (depth > c->depth_full) ? (depth - c->depth_full) : 0;
    uint32_t stride = c->base_stride << shift;
    if (stride == 0) stride = 1;

    uint32_t live_idx = 0;
    zval    *v;

    ZEND_HASH_FOREACH_VAL(ht, v) {
        zval *zv = v;
        ZVAL_DEREF(zv);
        if (!Z_COLLECTABLE_P(zv)) continue;

        zend_bool critical = (Z_TYPE_P(zv) == IS_OBJECT)
            && (gcp_classmap_kind(Z_OBJCE_P(zv)) == GCP_KIND_CRITICAL);

        zend_bool keep;
        if (critical) {
            keep = 1;
        } else if (depth < c->depth_full) {
            keep = 1;
        } else if (depth < c->depth_partial) {
            keep = (GCPRUNE_G(node_budget) > 0) && ((live_idx % stride) == 0);
        } else {
            keep = 0;
        }

        if (keep) {
            if (Z_TYPE_P(zv) == IS_ARRAY) {
                HashTable *sub = Z_ARRVAL_P(zv);
                uint32_t   sn  = zend_hash_num_elements(sub);
                if (sn < (uint32_t) GCPRUNE_G(ini_large_threshold)) {
                    gcp_emit(c, zv);
                } else if (depth + 1 < c->depth_partial && GCPRUNE_G(node_budget) > 0) {
                    gcp_walk_ht(c, sub, depth + 1);
                } else {
                    GCPRUNE_G(stats).nodes_pruned_total++;
                }
            } else {
                gcp_emit(c, zv);
                if (GCPRUNE_G(node_budget) > 0) GCPRUNE_G(node_budget)--;
                GCPRUNE_G(stats).nodes_visited_total++;
            }
        } else {
            GCPRUNE_G(stats).nodes_pruned_total++;
        }
        live_idx++;
    } ZEND_HASH_FOREACH_END();
}

static void gcp_build_pruned_buffer(zend_object *obj, gcp_node *node, gcp_strategy strat)
{
    gcp_ctx c;
    c.buf = node->buf;
    c.cap = node->buf_cap;
    c.len = 0;
    c.depth_full    = (strat == GCP_SHALLOW) ? 0 : (uint32_t) GCPRUNE_G(ini_depth_full);
    c.depth_partial = (strat == GCP_SHALLOW) ? 1 : (uint32_t) GCPRUNE_G(ini_depth_partial);
    c.base_stride   = (uint32_t) GCPRUNE_G(ini_sample_stride);
    if (c.base_stride == 0) c.base_stride = 1;

    zval *p   = obj->properties_table;
    zval *end = p + obj->ce->default_properties_count;
    for (; p < end; p++) {
        zval *zv = p;
        if (Z_TYPE_P(zv) == IS_UNDEF) continue;
        ZVAL_DEREF(zv);
        if (!Z_COLLECTABLE_P(zv)) continue;

        if (Z_TYPE_P(zv) == IS_ARRAY
            && zend_hash_num_elements(Z_ARRVAL_P(zv)) >= (uint32_t) GCPRUNE_G(ini_large_threshold)) {
            gcp_walk_ht(&c, Z_ARRVAL_P(zv), 1);
        } else {
            gcp_emit(&c, zv);
        }
    }
    if (obj->properties) {
        zval *zv;
        ZEND_HASH_FOREACH_VAL(obj->properties, zv) {
            ZVAL_DEREF(zv);
            if (!Z_COLLECTABLE_P(zv)) continue;
            if (Z_TYPE_P(zv) == IS_ARRAY
                && zend_hash_num_elements(Z_ARRVAL_P(zv)) >= (uint32_t) GCPRUNE_G(ini_large_threshold)) {
                gcp_walk_ht(&c, Z_ARRVAL_P(zv), 1);
            } else {
                gcp_emit(&c, zv);
            }
        } ZEND_HASH_FOREACH_END();
    }

    node->buf       = c.buf;
    node->buf_cap   = c.cap;
    node->buf_len   = c.len;
    node->last_cost = (uint32_t) c.len;

    for (int i = 0; i < c.len; i++) {
        if (Z_TYPE(c.buf[i]) == IS_OBJECT) {
            zend_object *child = Z_OBJ(c.buf[i]);
            gcp_node *cn = (gcp_node *) zend_hash_index_find_ptr(
                &GCPRUNE_G(nodes), (zend_ulong) child->handle);
            if (cn) {
                uint32_t next = (uint32_t) node->depth_hint + 1u;
                if (next > UINT16_MAX) next = UINT16_MAX;
                if ((uint32_t) cn->depth_hint > next) {
                    cn->depth_hint = (uint16_t) next;
                }
            }
        }
    }
}

static gcp_strategy gcp_pick_strategy(zend_object *obj, gcp_node *node)
{
    gcp_kind kind = gcp_classmap_kind(obj->ce);
    if (kind != GCP_KIND_CONTAINER) return GCP_FULL;

    uint32_t   big_n = 0;
    HashTable *big   = gcp_find_largest_container(obj, &big_n);
    if (!big || big_n < (uint32_t) GCPRUNE_G(ini_large_threshold)) return GCP_FULL;

    uint64_t fp = gcp_fingerprint(big);
    if (fp != node->fp) {
        node->fp            = fp;
        node->stability    *= 0.30;
        node->stable_streak = 0;
        return GCP_FULL;
    }
    node->stability     = node->stability * 0.90 + 0.10;
    node->stable_streak++;
    node->age++;

    uint32_t K = (uint32_t) GCPRUNE_G(ini_full_scan_interval);
    if (K > 0 && (GCPRUNE_G(run_id) % K) == (obj->handle % K)) {
        node->last_full_run = GCPRUNE_G(run_id);
        GCPRUNE_G(stats).forced_fulls++;
        return GCP_FULL;
    }

    if (node->last_cost < (uint32_t) GCPRUNE_G(ini_cost_gate)) return GCP_FULL;
    if (node->stability < GCPRUNE_G(ini_stability_gate))      return GCP_FULL;

    return (node->depth_hint >= 2) ? GCP_SAMPLED : GCP_SHALLOW;
}

static void gcprune_run_sync(void)
{
    zend_gc_status st;
    zend_gc_get_status(&st);
    if (st.runs != GCPRUNE_G(last_seen_runs)) {
        GCPRUNE_G(last_seen_runs) = st.runs;
        GCPRUNE_G(run_id)++;
        GCPRUNE_G(node_budget)    = (int32_t) GCPRUNE_G(ini_node_budget);
        GCPRUNE_G(stats).runs++;
    }
}

static HashTable *gcprune_get_gc(zend_object *obj, zval **table, int *n)
{
    if (UNEXPECTED(!GCPRUNE_G(enabled) || GCPRUNE_G(paused))) {
        return zend_std_get_gc(obj, table, n);
    }

    gcprune_run_sync();

    gcp_node *node = gcp_node_get_or_create(obj);

    /* mark/scan/collect consistency: identical edge set every call within a run */
    if (node->cached_run == GCPRUNE_G(run_id)) {
        if (node->buf_len < 0) {
            return zend_std_get_gc(obj, table, n);
        }
        *table = node->buf;
        *n     = node->buf_len;
        return NULL;
    }

    gcp_strategy strat = gcp_pick_strategy(obj, node);

    if (strat == GCP_FULL) {
        node->cached_run    = GCPRUNE_G(run_id);
        node->buf_len       = -1;
        node->last_full_run = GCPRUNE_G(run_id);
        GCPRUNE_G(stats).full_cycles++;
        return zend_std_get_gc(obj, table, n);
    }

    gcp_build_pruned_buffer(obj, node, strat);
    node->cached_run = GCPRUNE_G(run_id);
    GCPRUNE_G(stats).pruned_cycles_emit++;

    *table = node->buf;
    *n     = node->buf_len;
    return NULL;
}

static void gcprune_free_obj(zend_object *obj)
{
    gcp_node_evict(obj);
    zend_object_std_dtor(obj);
}

static zend_object *gcprune_create_object(zend_class_entry *ce)
{
    zend_object *obj = zend_objects_new(ce);
    object_properties_init(obj, ce);
    obj->handlers = &gcprune_handlers;
    return obj;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_register, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, class_name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(gcprune_register_container)
{
    zend_string *name;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    zend_class_entry *ce = zend_lookup_class(name);
    if (!ce) RETURN_FALSE;

    gcp_classmap_set(ce, GCP_KIND_CONTAINER);
    if (ce->create_object == NULL) {
        ce->create_object = gcprune_create_object;
    }
    RETURN_TRUE;
}

PHP_FUNCTION(gcprune_register_critical)
{
    zend_string *name;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    zend_class_entry *ce = zend_lookup_class(name);
    if (!ce) RETURN_FALSE;

    gcp_classmap_set(ce, GCP_KIND_CRITICAL);
    RETURN_TRUE;
}

PHP_FUNCTION(gcprune_pause)
{
    ZEND_PARSE_PARAMETERS_NONE();
    GCPRUNE_G(paused) = 1;
}

PHP_FUNCTION(gcprune_resume)
{
    ZEND_PARSE_PARAMETERS_NONE();
    GCPRUNE_G(paused) = 0;
}

PHP_FUNCTION(gcprune_stats)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    add_assoc_long(return_value, "runs",                (zend_long) GCPRUNE_G(stats).runs);
    add_assoc_long(return_value, "full_cycles",         (zend_long) GCPRUNE_G(stats).full_cycles);
    add_assoc_long(return_value, "pruned_cycles_emit",  (zend_long) GCPRUNE_G(stats).pruned_cycles_emit);
    add_assoc_long(return_value, "forced_fulls",        (zend_long) GCPRUNE_G(stats).forced_fulls);
    add_assoc_long(return_value, "nodes_tracked",       (zend_long) zend_hash_num_elements(&GCPRUNE_G(nodes)));
    add_assoc_long(return_value, "nodes_visited_total", (zend_long) GCPRUNE_G(stats).nodes_visited_total);
    add_assoc_long(return_value, "nodes_pruned_total",  (zend_long) GCPRUNE_G(stats).nodes_pruned_total);
    add_assoc_long(return_value, "run_id",              (zend_long) GCPRUNE_G(run_id));
    add_assoc_long(return_value, "node_budget_remain",  (zend_long) GCPRUNE_G(node_budget));
    add_assoc_bool(return_value, "enabled",             (int)       GCPRUNE_G(enabled));
    add_assoc_bool(return_value, "paused",              (int)       GCPRUNE_G(paused));
}

static const zend_function_entry gcprune_functions[] = {
    PHP_FE(gcprune_register_container, arginfo_gcprune_register)
    PHP_FE(gcprune_register_critical,  arginfo_gcprune_register)
    PHP_FE(gcprune_pause,              arginfo_gcprune_void)
    PHP_FE(gcprune_resume,             arginfo_gcprune_void)
    PHP_FE(gcprune_stats,              arginfo_gcprune_stats)
    PHP_FE_END
};

PHP_GINIT_FUNCTION(gcprune)
{
#if defined(COMPILE_DL_GCPRUNE) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    memset(gcprune_globals, 0, sizeof(*gcprune_globals));
}

PHP_MINIT_FUNCTION(gcprune)
{
    REGISTER_INI_ENTRIES();

    memcpy(&gcprune_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    gcprune_handlers.get_gc   = gcprune_get_gc;
    gcprune_handlers.free_obj = gcprune_free_obj;

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(gcprune)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(gcprune)
{
    GCPRUNE_G(enabled)        = GCPRUNE_G(ini_enabled);
    GCPRUNE_G(paused)         = 0;
    GCPRUNE_G(run_id)         = 0;
    GCPRUNE_G(last_seen_runs) = 0;
    GCPRUNE_G(node_budget)    = (int32_t) GCPRUNE_G(ini_node_budget);
    memset(&GCPRUNE_G(stats), 0, sizeof(gcp_stats));

    zend_hash_init(&GCPRUNE_G(nodes),    256, NULL, gcp_node_dtor, 0);
    zend_hash_init(&GCPRUNE_G(classmap),  64, NULL, NULL,          0);

    if (zend_ce_closure) {
        gcp_classmap_set(zend_ce_closure, GCP_KIND_CRITICAL);
    }
    zend_class_entry *wm = (zend_class_entry *) zend_hash_str_find_ptr(
        CG(class_table), "weakmap", sizeof("weakmap") - 1);
    if (wm) gcp_classmap_set(wm, GCP_KIND_CRITICAL);

    zend_class_entry *wr = (zend_class_entry *) zend_hash_str_find_ptr(
        CG(class_table), "weakreference", sizeof("weakreference") - 1);
    if (wr) gcp_classmap_set(wr, GCP_KIND_CRITICAL);

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(gcprune)
{
    zend_hash_destroy(&GCPRUNE_G(nodes));
    zend_hash_destroy(&GCPRUNE_G(classmap));
    return SUCCESS;
}

PHP_MINFO_FUNCTION(gcprune)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "ext-gcprune support", "enabled");
    php_info_print_table_row(2, "version", PHP_GCPRUNE_VERSION);
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

zend_module_entry gcprune_module_entry = {
    STANDARD_MODULE_HEADER,
    "gcprune",
    gcprune_functions,
    PHP_MINIT(gcprune),
    PHP_MSHUTDOWN(gcprune),
    PHP_RINIT(gcprune),
    PHP_RSHUTDOWN(gcprune),
    PHP_MINFO(gcprune),
    PHP_GCPRUNE_VERSION,
    PHP_MODULE_GLOBALS(gcprune),
    PHP_GINIT(gcprune),
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_GCPRUNE
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(gcprune)
#endif
