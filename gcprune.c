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
#include <stdlib.h>
#include <time.h>

ZEND_DECLARE_MODULE_GLOBALS(gcprune)

static zend_object_handlers gcprune_handlers;

static HashTable *gcprune_get_gc(zend_object *obj, zval **table, int *n);
static void       gcprune_free_obj(zend_object *obj);
static zend_object *gcprune_create_object(zend_class_entry *ce);

static void       gcprune_run_sync(void);
static gcp_node  *gcp_node_get_or_create(zend_object *obj);
static void       gcp_node_dtor(zval *zv);
static void       gcp_class_info_dtor(zval *zv);

static void       gcp_classmap_register(zend_class_entry *ce, gcp_kind kind, int32_t budget_override);
static gcp_class_info *gcp_classmap_get(zend_class_entry *ce);
static gcp_kind   gcp_classmap_kind(zend_class_entry *ce);
static int32_t    gcp_classmap_budget(zend_class_entry *ce);

static uint64_t   gcp_fingerprint(HashTable *ht);
static HashTable *gcp_find_largest_container(zend_object *obj, uint32_t *size_out);
static gcp_strategy gcp_pick_strategy(zend_object *obj, gcp_node *node);
static void       gcp_build_pruned_buffer(zend_object *obj, gcp_node *node, gcp_strategy strat);
static void       gcp_sweep_old_nodes(void);
static void       gcp_invoke_logger_with_stats(void);

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
    STD_PHP_INI_ENTRY  ("gcprune.max_node_age",       "1024",  PHP_INI_ALL, OnUpdateLong, ini_max_node_age,       zend_gcprune_globals, gcprune_globals)
PHP_INI_END()

static void gcp_node_dtor(zval *zv)
{
    gcp_node *n = (gcp_node *) Z_PTR_P(zv);
    if (!n) return;
    if (n->buf) efree(n->buf);
    efree(n);
}

static void gcp_class_info_dtor(zval *zv)
{
    gcp_class_info *info = (gcp_class_info *) Z_PTR_P(zv);
    if (info) efree(info);
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

static void gcp_classmap_register(zend_class_entry *ce, gcp_kind kind, int32_t budget_override)
{
    gcp_class_info *info = (gcp_class_info *) zend_hash_index_find_ptr(
        &GCPRUNE_G(classmap), (zend_ulong)(uintptr_t) ce);
    if (!info) {
        info = (gcp_class_info *) emalloc(sizeof(*info));
        zend_hash_index_add_new_ptr(&GCPRUNE_G(classmap),
            (zend_ulong)(uintptr_t) ce, info);
    }
    info->kind            = kind;
    info->budget_override = budget_override;
}

static gcp_class_info *gcp_classmap_get(zend_class_entry *ce)
{
    return (gcp_class_info *) zend_hash_index_find_ptr(
        &GCPRUNE_G(classmap), (zend_ulong)(uintptr_t) ce);
}

static gcp_kind gcp_classmap_kind(zend_class_entry *ce)
{
    gcp_class_info *info = gcp_classmap_get(ce);
    return info ? info->kind : GCP_KIND_UNKNOWN;
}

static int32_t gcp_classmap_budget(zend_class_entry *ce)
{
    gcp_class_info *info = gcp_classmap_get(ce);
    return info ? info->budget_override : 0;
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
    int32_t   local_budget;
    int       use_local;
} gcp_ctx;

static inline int32_t *gcp_budget_ptr(gcp_ctx *c)
{
    return c->use_local ? &c->local_budget : &GCPRUNE_G(node_budget);
}

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

        gcp_kind child_kind = (Z_TYPE_P(zv) == IS_OBJECT)
            ? gcp_classmap_kind(Z_OBJCE_P(zv))
            : GCP_KIND_UNKNOWN;

        if (child_kind == GCP_KIND_LEAF) {
            GCPRUNE_G(stats).nodes_pruned_total++;
            live_idx++;
            continue;
        }

        zend_bool critical = (child_kind == GCP_KIND_CRITICAL);
        int32_t *budget_ptr = gcp_budget_ptr(c);

        zend_bool keep;
        if (critical) {
            keep = 1;
        } else if (depth < c->depth_full) {
            keep = 1;
        } else if (depth < c->depth_partial) {
            keep = (*budget_ptr > 0) && ((live_idx % stride) == 0);
        } else {
            keep = 0;
        }

        if (keep) {
            if (Z_TYPE_P(zv) == IS_ARRAY) {
                HashTable *sub = Z_ARRVAL_P(zv);
                uint32_t   sn  = zend_hash_num_elements(sub);
                if (sn < (uint32_t) GCPRUNE_G(ini_large_threshold)) {
                    gcp_emit(c, zv);
                } else if (depth + 1 < c->depth_partial && *budget_ptr > 0) {
                    gcp_walk_ht(c, sub, depth + 1);
                } else {
                    GCPRUNE_G(stats).nodes_pruned_total++;
                }
            } else {
                gcp_emit(c, zv);
                if (*budget_ptr > 0) (*budget_ptr)--;
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

    int32_t override = gcp_classmap_budget(obj->ce);
    if (override > 0) {
        c.use_local    = 1;
        c.local_budget = override;
    } else {
        c.use_local    = 0;
        c.local_budget = 0;
    }

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
    if (K > 0) {
        uint32_t mix = (GCPRUNE_G(run_id) * 2654435761u)
                     ^ (obj->handle * 40503u)
                     ^ GCPRUNE_G(forced_offset);
        if ((mix % K) == 0) {
            node->last_full_run = GCPRUNE_G(run_id);
            GCPRUNE_G(stats).forced_fulls++;
            return GCP_FULL;
        }
    }

    if (node->last_cost < (uint32_t) GCPRUNE_G(ini_cost_gate)) return GCP_FULL;
    if (node->stability < GCPRUNE_G(ini_stability_gate))      return GCP_FULL;

    return (node->depth_hint >= 2) ? GCP_SAMPLED : GCP_SHALLOW;
}

static int gcp_sweep_apply(zval *zv, void *arg)
{
    gcp_node *n      = (gcp_node *) Z_PTR_P(zv);
    uint32_t  cutoff = *(uint32_t *) arg;
    if (n && n->cached_run < cutoff) {
        GCPRUNE_G(stats).nodes_evicted_total++;
        return ZEND_HASH_APPLY_REMOVE;
    }
    return ZEND_HASH_APPLY_KEEP;
}

static void gcp_sweep_old_nodes(void)
{
    zend_long max_age = GCPRUNE_G(ini_max_node_age);
    if (max_age <= 0) return;
    if ((GCPRUNE_G(run_id) & 0xFFu) != 0) return;
    if (GCPRUNE_G(run_id) <= (uint32_t) max_age) return;

    uint32_t cutoff = GCPRUNE_G(run_id) - (uint32_t) max_age;
    zend_hash_apply_with_argument(&GCPRUNE_G(nodes), gcp_sweep_apply, &cutoff);
}

static void gcprune_run_sync(void)
{
    if (GCPRUNE_G(testing_mode)) return;

    zend_gc_status st;
    zend_gc_get_status(&st);
    if (st.runs != GCPRUNE_G(last_seen_runs)) {
        GCPRUNE_G(last_seen_runs) = st.runs;
        GCPRUNE_G(run_id)++;
        GCPRUNE_G(node_budget)    = (int32_t) GCPRUNE_G(ini_node_budget);
        GCPRUNE_G(stats).runs++;
        gcp_sweep_old_nodes();
    }
}

static HashTable *gcprune_get_gc(zend_object *obj, zval **table, int *n)
{
    if (UNEXPECTED(!GCPRUNE_G(enabled) || GCPRUNE_G(paused))) {
        return zend_std_get_gc(obj, table, n);
    }

    gcprune_run_sync();

    gcp_kind kind = gcp_classmap_kind(obj->ce);
    if (kind == GCP_KIND_LEAF) {
        *table = NULL;
        *n     = 0;
        GCPRUNE_G(stats).pruned_cycles_emit++;
        return NULL;
    }

    gcp_node *node = gcp_node_get_or_create(obj);

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

static void gcp_fill_stats_array(zval *arr)
{
    array_init(arr);
    add_assoc_long(arr, "runs",                (zend_long) GCPRUNE_G(stats).runs);
    add_assoc_long(arr, "full_cycles",         (zend_long) GCPRUNE_G(stats).full_cycles);
    add_assoc_long(arr, "pruned_cycles_emit",  (zend_long) GCPRUNE_G(stats).pruned_cycles_emit);
    add_assoc_long(arr, "forced_fulls",        (zend_long) GCPRUNE_G(stats).forced_fulls);
    add_assoc_long(arr, "nodes_tracked",       (zend_long) zend_hash_num_elements(&GCPRUNE_G(nodes)));
    add_assoc_long(arr, "nodes_visited_total", (zend_long) GCPRUNE_G(stats).nodes_visited_total);
    add_assoc_long(arr, "nodes_pruned_total",  (zend_long) GCPRUNE_G(stats).nodes_pruned_total);
    add_assoc_long(arr, "nodes_evicted_total", (zend_long) GCPRUNE_G(stats).nodes_evicted_total);
    add_assoc_long(arr, "run_id",              (zend_long) GCPRUNE_G(run_id));
    add_assoc_long(arr, "node_budget_remain",  (zend_long) GCPRUNE_G(node_budget));
    add_assoc_bool(arr, "enabled",             (int)       GCPRUNE_G(enabled));
    add_assoc_bool(arr, "paused",              (int)       GCPRUNE_G(paused));
}

static void gcp_invoke_logger_with_stats(void)
{
    if (!GCPRUNE_G(has_logger)) return;
    if (Z_ISUNDEF(GCPRUNE_G(logger_callable))) return;

    zval args[1], retval;
    gcp_fill_stats_array(&args[0]);
    ZVAL_UNDEF(&retval);

    call_user_function(NULL, NULL, &GCPRUNE_G(logger_callable), &retval, 1, args);

    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&retval);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_register_container, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, class_name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, budget_override, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_register_simple, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, class_name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_bool, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_node_info, 0, 1, IS_ARRAY, 1)
    ZEND_ARG_TYPE_INFO(0, object, IS_OBJECT, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_set_logger, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gcprune_testing_mode, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(gcprune_register_container)
{
    zend_string *name;
    zend_long    budget = 0;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(name)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(budget)
    ZEND_PARSE_PARAMETERS_END();

    zend_class_entry *ce = zend_lookup_class(name);
    if (!ce) RETURN_FALSE;

    int32_t override = (budget > 0 && budget <= INT32_MAX) ? (int32_t) budget : 0;
    gcp_classmap_register(ce, GCP_KIND_CONTAINER, override);
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

    gcp_classmap_register(ce, GCP_KIND_CRITICAL, 0);
    RETURN_TRUE;
}

PHP_FUNCTION(gcprune_register_leaf)
{
    zend_string *name;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    zend_class_entry *ce = zend_lookup_class(name);
    if (!ce) RETURN_FALSE;

    gcp_classmap_register(ce, GCP_KIND_LEAF, 0);
    if (ce->create_object == NULL) {
        ce->create_object = gcprune_create_object;
    }
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

PHP_FUNCTION(gcprune_enable)
{
    ZEND_PARSE_PARAMETERS_NONE();
    GCPRUNE_G(enabled) = 1;
}

PHP_FUNCTION(gcprune_disable)
{
    ZEND_PARSE_PARAMETERS_NONE();
    GCPRUNE_G(enabled) = 0;
}

PHP_FUNCTION(gcprune_is_enabled)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(GCPRUNE_G(enabled) && !GCPRUNE_G(paused));
}

PHP_FUNCTION(gcprune_stats)
{
    ZEND_PARSE_PARAMETERS_NONE();
    gcp_fill_stats_array(return_value);
}

PHP_FUNCTION(gcprune_reset)
{
    ZEND_PARSE_PARAMETERS_NONE();
    zend_hash_clean(&GCPRUNE_G(nodes));
    memset(&GCPRUNE_G(stats), 0, sizeof(gcp_stats));
    GCPRUNE_G(run_id)         = 0;
    GCPRUNE_G(last_seen_runs) = 0;
    GCPRUNE_G(node_budget)    = (int32_t) GCPRUNE_G(ini_node_budget);
}

PHP_FUNCTION(gcprune_flush)
{
    ZEND_PARSE_PARAMETERS_NONE();
    uint32_t cleared = zend_hash_num_elements(&GCPRUNE_G(nodes));
    zend_hash_clean(&GCPRUNE_G(nodes));
    GCPRUNE_G(stats).nodes_evicted_total += cleared;
}

PHP_FUNCTION(gcprune_node_info)
{
    zval *obj_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT(obj_zv)
    ZEND_PARSE_PARAMETERS_END();

    zend_object *obj = Z_OBJ_P(obj_zv);
    gcp_node *n = (gcp_node *) zend_hash_index_find_ptr(
        &GCPRUNE_G(nodes), (zend_ulong) obj->handle);
    if (!n) RETURN_NULL();

    array_init(return_value);
    add_assoc_long  (return_value, "handle",        (zend_long) n->obj_handle);
    add_assoc_long  (return_value, "cached_run",    (zend_long) n->cached_run);
    add_assoc_long  (return_value, "buf_len",       (zend_long) n->buf_len);
    add_assoc_long  (return_value, "buf_cap",       (zend_long) n->buf_cap);
    add_assoc_long  (return_value, "stable_streak", (zend_long) n->stable_streak);
    add_assoc_long  (return_value, "last_full_run", (zend_long) n->last_full_run);
    add_assoc_long  (return_value, "last_cost",     (zend_long) n->last_cost);
    add_assoc_long  (return_value, "age",           (zend_long) n->age);
    add_assoc_long  (return_value, "depth_hint",    (zend_long) n->depth_hint);
    add_assoc_double(return_value, "stability",     n->stability);

    gcp_kind kind = gcp_classmap_kind(obj->ce);
    const char *kind_str;
    switch (kind) {
        case GCP_KIND_CRITICAL:  kind_str = "critical";  break;
        case GCP_KIND_CONTAINER: kind_str = "container"; break;
        case GCP_KIND_LEAF:      kind_str = "leaf";      break;
        default:                 kind_str = "unknown";   break;
    }
    add_assoc_string(return_value, "kind", (char *) kind_str);
}

PHP_FUNCTION(gcprune_get_registered)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);

    zend_ulong       h;
    gcp_class_info  *info;
    ZEND_HASH_FOREACH_NUM_KEY_PTR(&GCPRUNE_G(classmap), h, info) {
        if (!info) continue;
        zend_class_entry *ce = (zend_class_entry *)(uintptr_t) h;
        if (!ce || !ce->name) continue;

        const char *kind_str;
        switch (info->kind) {
            case GCP_KIND_CRITICAL:  kind_str = "critical";  break;
            case GCP_KIND_CONTAINER: kind_str = "container"; break;
            case GCP_KIND_LEAF:      kind_str = "leaf";      break;
            default:                 kind_str = "unknown";   break;
        }

        zval entry;
        array_init(&entry);
        add_assoc_string(&entry, "kind", (char *) kind_str);
        add_assoc_long  (&entry, "budget_override", (zend_long) info->budget_override);

        zend_hash_update(Z_ARRVAL_P(return_value), ce->name, &entry);
    } ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(gcprune_set_logger)
{
    zval *cb = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL_OR_NULL(cb)
    ZEND_PARSE_PARAMETERS_END();

    if (!Z_ISUNDEF(GCPRUNE_G(logger_callable))) {
        zval_ptr_dtor(&GCPRUNE_G(logger_callable));
        ZVAL_UNDEF(&GCPRUNE_G(logger_callable));
    }
    GCPRUNE_G(has_logger) = 0;

    if (cb == NULL || Z_TYPE_P(cb) == IS_NULL) {
        return;
    }

    char *err = NULL;
    if (!zend_is_callable_ex(cb, NULL, 0, NULL, NULL, &err)) {
        if (err) efree(err);
        zend_type_error("gcprune_set_logger(): argument must be a valid callable or null");
        RETURN_THROWS();
    }
    if (err) efree(err);

    ZVAL_COPY(&GCPRUNE_G(logger_callable), cb);
    GCPRUNE_G(has_logger) = 1;
}

PHP_FUNCTION(gcprune_testing_mode)
{
    zend_bool on;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(on)
    ZEND_PARSE_PARAMETERS_END();
    GCPRUNE_G(testing_mode) = on;
}

PHP_FUNCTION(gcprune_tick)
{
    ZEND_PARSE_PARAMETERS_NONE();
    GCPRUNE_G(run_id)++;
    GCPRUNE_G(node_budget) = (int32_t) GCPRUNE_G(ini_node_budget);
    GCPRUNE_G(stats).runs++;
    gcp_sweep_old_nodes();
}

static const zend_function_entry gcprune_functions[] = {
    PHP_FE(gcprune_register_container, arginfo_gcprune_register_container)
    PHP_FE(gcprune_register_critical,  arginfo_gcprune_register_simple)
    PHP_FE(gcprune_register_leaf,      arginfo_gcprune_register_simple)
    PHP_FE(gcprune_pause,              arginfo_gcprune_void)
    PHP_FE(gcprune_resume,             arginfo_gcprune_void)
    PHP_FE(gcprune_enable,             arginfo_gcprune_void)
    PHP_FE(gcprune_disable,            arginfo_gcprune_void)
    PHP_FE(gcprune_is_enabled,         arginfo_gcprune_bool)
    PHP_FE(gcprune_stats,              arginfo_gcprune_stats)
    PHP_FE(gcprune_reset,              arginfo_gcprune_void)
    PHP_FE(gcprune_flush,              arginfo_gcprune_void)
    PHP_FE(gcprune_node_info,          arginfo_gcprune_node_info)
    PHP_FE(gcprune_get_registered,     arginfo_gcprune_stats)
    PHP_FE(gcprune_set_logger,         arginfo_gcprune_set_logger)
    PHP_FE(gcprune_testing_mode,       arginfo_gcprune_testing_mode)
    PHP_FE(gcprune_tick,               arginfo_gcprune_void)
    PHP_FE_END
};

PHP_GINIT_FUNCTION(gcprune)
{
#if defined(COMPILE_DL_GCPRUNE) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    memset(gcprune_globals, 0, sizeof(*gcprune_globals));
    ZVAL_UNDEF(&gcprune_globals->logger_callable);
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
    GCPRUNE_G(testing_mode)   = 0;
    GCPRUNE_G(has_logger)     = 0;
    GCPRUNE_G(run_id)         = 0;
    GCPRUNE_G(last_seen_runs) = 0;
    GCPRUNE_G(node_budget)    = (int32_t) GCPRUNE_G(ini_node_budget);
    GCPRUNE_G(forced_offset)  = (uint32_t) ((uintptr_t) &GCPRUNE_G(forced_offset) ^ (uint32_t) time(NULL));
    memset(&GCPRUNE_G(stats), 0, sizeof(gcp_stats));
    ZVAL_UNDEF(&GCPRUNE_G(logger_callable));

    zend_hash_init(&GCPRUNE_G(nodes),    256, NULL, gcp_node_dtor,        0);
    zend_hash_init(&GCPRUNE_G(classmap),  64, NULL, gcp_class_info_dtor,  0);

    if (zend_ce_closure) {
        gcp_classmap_register(zend_ce_closure, GCP_KIND_CRITICAL, 0);
    }
    zend_class_entry *wm = (zend_class_entry *) zend_hash_str_find_ptr(
        CG(class_table), "weakmap", sizeof("weakmap") - 1);
    if (wm) gcp_classmap_register(wm, GCP_KIND_CRITICAL, 0);

    zend_class_entry *wr = (zend_class_entry *) zend_hash_str_find_ptr(
        CG(class_table), "weakreference", sizeof("weakreference") - 1);
    if (wr) gcp_classmap_register(wr, GCP_KIND_CRITICAL, 0);

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(gcprune)
{
    gcp_invoke_logger_with_stats();

    if (!Z_ISUNDEF(GCPRUNE_G(logger_callable))) {
        zval_ptr_dtor(&GCPRUNE_G(logger_callable));
        ZVAL_UNDEF(&GCPRUNE_G(logger_callable));
    }
    GCPRUNE_G(has_logger) = 0;

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
