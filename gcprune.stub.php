<?php

/** @generate-class-entries */

/**
 * Register a class as a GC container (large array holder).
 * Enables pruned GC buffer for instances of this class.
 */
function gcprune_register_container(string $class_name): bool {}

/**
 * Register a class as GC-critical.
 * Instances are always retained in pruned buffers.
 */
function gcprune_register_critical(string $class_name): bool {}

/**
 * Pause gcprune pruning for the current request.
 * get_gc falls back to standard zend_std_get_gc.
 */
function gcprune_pause(): void {}

/**
 * Resume gcprune pruning after gcprune_pause().
 */
function gcprune_resume(): void {}

/**
 * Return runtime statistics.
 *
 * Keys:
 *   runs                 – number of GC cycles seen
 *   full_cycles          – get_gc calls that fell back to full scan
 *   pruned_cycles_emit   – get_gc calls that returned a pruned buffer
 *   forced_fulls         – periodic forced full scans
 *   nodes_tracked        – objects currently in the node map
 *   nodes_visited_total  – cumulative nodes emitted
 *   nodes_pruned_total   – cumulative nodes dropped
 *   run_id               – monotonic GC run counter
 *   node_budget_remain   – budget left in current run
 *   enabled              – whether pruning is active
 *   paused               – whether pruning is paused
 *
 * @return array{
 *   runs: int,
 *   full_cycles: int,
 *   pruned_cycles_emit: int,
 *   forced_fulls: int,
 *   nodes_tracked: int,
 *   nodes_visited_total: int,
 *   nodes_pruned_total: int,
 *   run_id: int,
 *   node_budget_remain: int,
 *   enabled: bool,
 *   paused: bool,
 * }
 */
function gcprune_stats(): array {}
