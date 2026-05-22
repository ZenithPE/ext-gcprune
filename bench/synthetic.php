#!/usr/bin/env php
<?php
declare(strict_types=1);

class Node {
    public array  $children = [];
    public ?Node  $parent   = null;
    public string $payload;
    public function __construct(string $p) { $this->payload = $p; }
}

class World {
    public array $chunks = [];
}

function build(int $n_chunks, int $branch, int $depth): World {
    $w = new World;
    for ($i = 0; $i < $n_chunks; $i++) {
        $root  = new Node("chunk{$i}");
        $stack = [[$root, 0]];
        while ($stack) {
            [$node, $d] = array_pop($stack);
            if ($d >= $depth) continue;
            for ($b = 0; $b < $branch; $b++) {
                $c = new Node("n{$i}-{$d}-{$b}");
                $c->parent        = $node;
                $node->children[] = $c;
                $stack[]          = [$c, $d + 1];
            }
        }
        $w->chunks[] = $root;
    }
    return $w;
}

function bench(string $label, callable $fn): void {
    gc_collect_cycles();
    $rss0 = memory_get_usage(true);
    $t0   = hrtime(true);
    $fn();
    $dt   = (hrtime(true) - $t0) / 1e6;
    $rss1 = memory_get_usage(true);
    printf("%-24s wall=%9.2f ms   rss_delta=%+d KB\n",
        $label, $dt, ($rss1 - $rss0) >> 10);
}

$N = (int) ($argv[1] ?? 256);
$B = (int) ($argv[2] ?? 4);
$D = (int) ($argv[3] ?? 4);
$I = (int) ($argv[4] ?? 50);

printf("config: chunks=%d branch=%d depth=%d iters=%d   ext_loaded=%s\n",
    $N, $B, $D, $I,
    extension_loaded('gcprune') ? 'yes' : 'no');

$w = build($N, $B, $D);

if (extension_loaded('gcprune')) {
    gcprune_register_container(World::class);
    gcprune_register_container(Node::class);
    for ($i = 0; $i < 12; $i++) gc_collect_cycles();
}

bench("steady-state " . $I . "x gc", function () use ($I) {
    for ($i = 0; $i < $I; $i++) gc_collect_cycles();
});

if (extension_loaded('gcprune')) {
    $s = gcprune_stats();
    printf("\ngcprune_stats:\n");
    foreach ($s as $k => $v) printf("  %-22s %s\n", $k, (string) $v);
}
