--TEST--
ext-gcprune loads and exports userland API
--SKIPIF--
<?php if (!extension_loaded('gcprune')) die('skip ext-gcprune required'); ?>
--FILE--
<?php
var_dump(extension_loaded('gcprune'));
var_dump(function_exists('gcprune_stats'));
var_dump(function_exists('gcprune_register_container'));
var_dump(function_exists('gcprune_register_critical'));
var_dump(function_exists('gcprune_pause'));
var_dump(function_exists('gcprune_resume'));

$s = gcprune_stats();
var_dump(is_array($s));
var_dump(isset($s['runs'], $s['full_cycles'], $s['pruned_cycles_emit'], $s['forced_fulls']));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
