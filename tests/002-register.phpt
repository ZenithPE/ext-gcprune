--TEST--
ext-gcprune class registration
--SKIPIF--
<?php if (!extension_loaded('gcprune')) die('skip ext-gcprune required'); ?>
--FILE--
<?php
class Bucket   { public array $data = []; }
class Critical { public mixed $ref = null; }

var_dump(gcprune_register_container(Bucket::class));
var_dump(gcprune_register_critical(Critical::class));
var_dump(gcprune_register_container("Nope\\DoesNotExist"));
var_dump(gcprune_register_container(stdClass::class));
?>
--EXPECT--
bool(true)
bool(true)
bool(false)
bool(true)
