PHP_ARG_ENABLE([gcprune],
  [whether to enable gcprune support],
  [AS_HELP_STRING([--enable-gcprune],
    [Enable ext-gcprune adaptive GC traversal optimizer])],
  [no])

if test "$PHP_GCPRUNE" != "no"; then
  AC_DEFINE(HAVE_GCPRUNE, 1, [Have gcprune])
  PHP_NEW_EXTENSION(gcprune, gcprune.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
fi
