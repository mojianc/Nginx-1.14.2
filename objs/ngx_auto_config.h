#define NGX_CONFIGURE ""

#ifndef NGX_COMPILER
#define NGX_COMPILER  "gcc 8.2.1 20180905 (Red Hat 8.2.1-3) (GCC) "
#endif


#ifndef NGX_HAVE_NONALIGNED
#define NGX_HAVE_NONALIGNED  1
#endif


#ifndef NGX_CPU_CACHE_LINE
#define NGX_CPU_CACHE_LINE  64
#endif


#define NGX_KQUEUE_UDATA_T  (void *)


#ifndef NGX_HAVE_UNIX_DOMAIN
#define NGX_HAVE_UNIX_DOMAIN  1
#endif

