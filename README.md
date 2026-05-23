# ext-gcprune

Adaptive cycle-GC traversal optimizer for PHP 8.2 / 8.3. Targeted at long-running objects where each `gc_collect_cycles()` re-walks a huge, mostly-stable object graph and produces tick-time spikes.

## What it does

Hooks `zend_object_handlers.get_gc` for classes you register as containers. For each tracked object the dispatcher:

1. Detects the GC run boundary via `zend_gc_get_status().runs`.
2. Picks a per-run strategy `{FULL, SHALLOW, SAMPLED}` from object class, container fingerprint stability, last traversal cost, and a cross-cycle learned depth hint.
3. Caches the chosen edge set by `(object, run_id)` so the GC's mark, scan-black, and collect phases see an identical view - the only thing standing between correct trial-deletion accounting and a use-after-free.
4. For pruned strategies, runs a bounded DFS over the object's owned large containers (depth-limited, stride-sampled, node-budgeted) and emits a flat `zval` buffer instead of the raw `HashTable`.
5. Always emits edges whose target is a **critical** class
6. Forces a full traversal every Kth run, staggered by `obj->handle % K`, so any cycle that hides behind a pruned-out edge is collected within at most K runs.

It does **not** hide objects from the GC root buffer. Every object is still tracked; only the per-cycle traversal shape changes.

## Safety in one paragraph

The correctness core is: (a) the emitted edge set is always a subset of the real outgoing edges - never invented; (b) the same set is returned for every `get_gc` call within one GC run (per-run cache); (c) sampling is deterministic over a `HashTable` that is treated as frozen during a GC run; (d) periodic forced full traversal bounds any leak from pruned-out edges. The failure mode by construction is "leak a bit longer," never "crash." Naïve sampling without (b) or (c) corrupts refcounts and crashes - do not strip those guards.

## Build

Linux:

```sh
phpize
./configure --enable-gcprune
make
make test
```

Windows:

```powershell
phpize
configure --enable-gcprune
nmake
```

Load:

```ini
extension=gcprune.so
```

## Configuration (`php.ini`)

```ini
gcprune.enabled            = 1
gcprune.node_budget        = 5000   ; max emitted edges per GC run
gcprune.large_threshold    = 256    ; HT element count to treat as "large"
gcprune.full_scan_interval = 8      ; K - every K-th run is forced full, staggered
gcprune.depth_full         = 2      ; depth < this : emit everything
gcprune.depth_partial      = 5      ; depth < this : stride sampling
gcprune.sample_stride      = 16     ; base stride at depth == depth_full
gcprune.cost_gate          = 2000   ; minimum last-cost to bother pruning
gcprune.stability_gate     = 0.85   ; EWMA stability required to prune
```

## Userland API

```php
gcprune_register_container(string $class): bool   // class is prune-eligible
gcprune_register_critical (string $class): bool   // target class is always kept
gcprune_pause (): void
gcprune_resume(): void
gcprune_stats (): array
```

`Closure`, `WeakMap`, and `WeakReference` are registered critical by default.

Register **after** the class is loaded and **before** instances are created - registration patches `ce->create_object`, which only affects new instances.

## Stats

```php
[
  'runs'                => int,  // observed GC runs
  'full_cycles'         => int,
  'pruned_cycles_emit'  => int,
  'forced_fulls'        => int,
  'nodes_tracked'       => int,
  'nodes_visited_total' => int,
  'nodes_pruned_total'  => int,
  'run_id'              => int,
  'node_budget_remain'  => int,
  'enabled'             => bool,
  'paused'              => bool,
]
```

## Limitations

- Top-level arrays not owned by a tracked object are not hooked - `get_gc` is per-object only.
- Object→object depth is approximate (cross-cycle learned, lags by 1–2 runs for new graphs).
- `SplObjectStorage` and other internal classes are not wrapped in v0.0.1.
- Pruning trades a bounded periodic leak for lower pause time. If your workload produces large garbage cycles entirely inside cold containers at high rate, lower `full_scan_interval` or mark those classes critical.

## Benchmarks

`bench/synthetic.php` builds a synthetic chunk / branch / depth graph and times steady-state `gc_collect_cycles()`. Compare with and without the extension:

```sh
php                                   bench/synthetic.php 512 4 5 100
php -d extension=./modules/gcprune.so bench/synthetic.php 512 4 5 100
```

## License

MIT. See [`LICENSE`](LICENSE).
