--TEST--
ext-gcprune: gc_collect_cycles survives under pruning load (no crash, stats advance)
--SKIPIF--
<?php if (!extension_loaded('gcprune')) die('skip ext-gcprune required'); ?>
--INI--
gcprune.enabled=1
gcprune.large_threshold=4
gcprune.cost_gate=1
gcprune.stability_gate=0.0
gcprune.full_scan_interval=4
gcprune.depth_full=1
gcprune.depth_partial=3
gcprune.sample_stride=4
--FILE--
<?php
class Holder { public array $items = []; }
class CNode  { public ?CNode $next = null; }

gcprune_register_container(Holder::class);

$h = new Holder;
for ($i = 0; $i < 100; $i++) $h->items[] = new stdClass;

for ($i = 0; $i < 50; $i++) {
    $a = new CNode;
    $b = new CNode;
    $a->next = $b;
    $b->next = $a;
    $h->items[] = $a;
    $h->items[] = $b;
    unset($a, $b);
    if (count($h->items) > 120) {
        array_splice($h->items, 0, 20);
    }
    gc_collect_cycles();
}

$s = gcprune_stats();
echo "runs>=1: ",         ($s['runs']        >= 1 ? "yes" : "no"), "\n";
echo "full+pruned>=1: ",  (($s['full_cycles'] + $s['pruned_cycles_emit']) >= 1 ? "yes" : "no"), "\n";
echo "alive\n";
?>
--EXPECT--
runs>=1: yes
full+pruned>=1: yes
alive
