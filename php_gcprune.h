#ifndef PHP_GCPRUNE_H
#define PHP_GCPRUNE_H

extern zend_module_entry gcprune_module_entry;
#define phpext_gcprune_ptr &gcprune_module_entry

#define PHP_GCPRUNE_VERSION "0.1.0"

#ifdef PHP_WIN32
# define PHP_GCPRUNE_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PHP_GCPRUNE_API __attribute__ ((visibility("default")))
#else
# define PHP_GCPRUNE_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

typedef enum {
    GCP_KIND_UNKNOWN   = 0,
    GCP_KIND_CRITICAL  = 1,
    GCP_KIND_CONTAINER = 2,
    GCP_KIND_LEAF      = 3
} gcp_kind;

typedef enum {
    GCP_FULL    = 0,
    GCP_SHALLOW = 1,
    GCP_SAMPLED = 2
} gcp_strategy;

typedef struct _gcp_class_info {
    gcp_kind kind;
    int32_t  budget_override;
} gcp_class_info;

typedef struct _gcp_node {
    uint32_t  obj_handle;
    uint32_t  cached_run;
    int32_t   buf_len;
    int32_t   buf_cap;
    zval     *buf;

    uint64_t  fp;
    uint32_t  stable_streak;
    uint32_t  last_full_run;
    uint32_t  last_cost;
    uint32_t  age;
    uint16_t  depth_hint;
    uint16_t  flags;
    double    stability;
} gcp_node;

typedef struct _gcp_stats {
    uint64_t runs;
    uint64_t full_cycles;
    uint64_t pruned_cycles_emit;
    uint64_t forced_fulls;
    uint64_t nodes_visited_total;
    uint64_t nodes_pruned_total;
    uint64_t nodes_evicted_total;
} gcp_stats;

ZEND_BEGIN_MODULE_GLOBALS(gcprune)
    zend_bool   enabled;
    zend_bool   paused;
    zend_bool   testing_mode;
    zend_bool   has_logger;
    uint32_t    run_id;
    uint32_t    last_seen_runs;
    uint32_t    forced_offset;
    int32_t     node_budget;
    HashTable   nodes;
    HashTable   classmap;
    gcp_stats   stats;
    zval        logger_callable;

    zend_bool   ini_enabled;
    zend_long   ini_node_budget;
    zend_long   ini_large_threshold;
    zend_long   ini_full_scan_interval;
    zend_long   ini_depth_full;
    zend_long   ini_depth_partial;
    zend_long   ini_sample_stride;
    zend_long   ini_cost_gate;
    zend_long   ini_max_node_age;
    double      ini_stability_gate;
ZEND_END_MODULE_GLOBALS(gcprune)

ZEND_EXTERN_MODULE_GLOBALS(gcprune)

#define GCPRUNE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(gcprune, v)

#if defined(ZTS) && defined(COMPILE_DL_GCPRUNE)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

PHP_MINIT_FUNCTION(gcprune);
PHP_MSHUTDOWN_FUNCTION(gcprune);
PHP_RINIT_FUNCTION(gcprune);
PHP_RSHUTDOWN_FUNCTION(gcprune);
PHP_MINFO_FUNCTION(gcprune);
PHP_GINIT_FUNCTION(gcprune);

#endif /* PHP_GCPRUNE_H */
