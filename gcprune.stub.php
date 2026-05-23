<?php

/** @generate-class-entries */

function gcprune_register_container(string $class_name, int $budget_override = 0): bool {}

function gcprune_register_critical(string $class_name): bool {}

function gcprune_register_leaf(string $class_name): bool {}

function gcprune_pause(): void {}

function gcprune_resume(): void {}

function gcprune_enable(): void {}

function gcprune_disable(): void {}

function gcprune_is_enabled(): bool {}

function gcprune_reset(): void {}

function gcprune_flush(): void {}

function gcprune_testing_mode(bool $enabled): void {}

function gcprune_tick(): void {}

function gcprune_set_logger(?callable $callback): void {}

/**
 * @return array{
 *   runs: int, full_cycles: int, pruned_cycles_emit: int, forced_fulls: int,
 *   nodes_tracked: int, nodes_visited_total: int, nodes_pruned_total: int,
 *   nodes_evicted_total: int, run_id: int, node_budget_remain: int,
 *   enabled: bool, paused: bool,
 * }
 */
function gcprune_stats(): array {}

/**
 * @return array{
 *   handle: int, cached_run: int, buf_len: int, buf_cap: int,
 *   stable_streak: int, last_full_run: int, last_cost: int, age: int,
 *   depth_hint: int, stability: float, kind: string,
 * }|null
 */
function gcprune_node_info(object $object): ?array {}

/**
 * @return array<string, array{kind: string, budget_override: int}>
 */
function gcprune_get_registered(): array {}
