--TEST--
ext-gcprune: pause/resume toggles pruning path
--SKIPIF--
<?php if (!extension_loaded('gcprune')) die('skip ext-gcprune required'); ?>
--INI--
gcprune.enabled=1
--FILE--
<?php
class Box { public array $items = []; }
gcprune_register_container(Box::class);

$b = new Box;
for ($i = 0; $i < 300; $i++) $b->items[] = new stdClass;

gcprune_pause();
$s1 = gcprune_stats();
var_dump($s1['paused']);

gc_collect_cycles();
gc_collect_cycles();

gcprune_resume();
$s2 = gcprune_stats();
var_dump($s2['paused']);

echo "ok\n";
?>
--EXPECT--
bool(true)
bool(false)
ok
