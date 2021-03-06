
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HASH_H_INCLUDED_
#define _NGX_HASH_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
/**
 * Nginx的hash表结构主要几个特点：
   (1)静态只读。当初始化生成hash表结构后，是不能动态修改这个hash表结构的内容。
   (2)将内存利用最大化。Nginx的hash表，将内存利用率发挥到了极致，并且很多设计上面都是可以供我们学习和参考的。
   (3)查询速度快。Nginx的hash表做了内存对齐等优化。
   (4)主要解析配置数据。

   Nginx 的散列表使用的是开放寻址法。
   开放寻址法有许多种实现方法，Nginx 使用的是连续非空槽存储碰撞元素的方法。例如，当插入一个元素时，可以按照散列方
   法找到指定槽，如果该槽非空且其存储的元素与待插入元素并非同一个元素，则依次检查其后连续的槽，直到找到一个空槽来
   放置这个元素为止。查询元素时也是使用类似的方法，即从散列方法指定的位置起检查连续的非空槽中的元素。
*/

/**
 * 存储hash的元素
 */
typedef struct {
    /* 指向用户自定义元素数据的指针，如果当前 ngx_hash_elt_t 槽为空，则 value 的值为 0 */
    void             *value;    /* 指向value的指针 */
    u_short           len;      /* key的长度 */
    u_char            name[1];  /* 指向key的第一个地址，key长度为变长(设计上的亮点,柔性数组)*/
} ngx_hash_elt_t;

/**
 * Hash的桶
 */
typedef struct {
    ngx_hash_elt_t  **buckets;   /* hash表的桶指针地址值 */
    ngx_uint_t        size;      /* hash表的桶的个数*/
} ngx_hash_t;


typedef struct {
    ngx_hash_t        hash;     
    void             *value; 
} ngx_hash_wildcard_t;


typedef struct {
    ngx_str_t         key;       /* 元素关键字 */
    ngx_uint_t        key_hash;  /* 由散列方法算出来的关键码 */
    void             *value;     /* 指向实际的用户数据 */
} ngx_hash_key_t;


typedef ngx_uint_t (*ngx_hash_key_pt) (u_char *data, size_t len);


typedef struct {
    ngx_hash_t            hash;
    ngx_hash_wildcard_t  *wc_head;
    ngx_hash_wildcard_t  *wc_tail;
} ngx_hash_combined_t;


/**
 * hash表主体结构
 */
typedef struct {
    /* 指向普通的完全匹配散列表 */
    ngx_hash_t       *hash;         
    /* 用于初始化添加元素的散列方法 */
    ngx_hash_key_pt   key;           /* 计算key散列的方法 */
    /* 散列表中槽的最大数目 */
    ngx_uint_t        max_size;     
    /* 散列表中一个槽的大小，它限制了每个散列表元素关键字的最大长度 */
    ngx_uint_t        bucket_size;   

    char             *name;          /* hash表名称 */
     /* 内存池，用于分配散列表（最多3个，包括1个普通散列表、1个前置通配符散列表、1个后置通配符散列表）
     * 中的所有槽 */
    ngx_pool_t       *pool;          
    /* 临时内存池，仅存在于初始化散列表之前。它主要用于分配一些临时的动态数组，
     * 带通配符的元素在初始化时需要用到这些数组 */
    ngx_pool_t       *temp_pool;    
} ngx_hash_init_t;


#define NGX_HASH_SMALL            1
#define NGX_HASH_LARGE            2

#define NGX_HASH_LARGE_ASIZE      16384
#define NGX_HASH_LARGE_HSIZE      10007

#define NGX_HASH_WILDCARD_KEY     1
#define NGX_HASH_READONLY_KEY     2


typedef struct {
    ngx_uint_t        hsize;

    ngx_pool_t       *pool;
    ngx_pool_t       *temp_pool;

    ngx_array_t       keys;
    ngx_array_t      *keys_hash;

    ngx_array_t       dns_wc_head;
    ngx_array_t      *dns_wc_head_hash;

    ngx_array_t       dns_wc_tail;
    ngx_array_t      *dns_wc_tail_hash;
} ngx_hash_keys_arrays_t;


typedef struct {
    ngx_uint_t        hash;
    ngx_str_t         key;
    ngx_str_t         value;
    u_char           *lowcase_key;
} ngx_table_elt_t;


void *ngx_hash_find(ngx_hash_t *hash, ngx_uint_t key, u_char *name, size_t len);
void *ngx_hash_find_wc_head(ngx_hash_wildcard_t *hwc, u_char *name, size_t len);
void *ngx_hash_find_wc_tail(ngx_hash_wildcard_t *hwc, u_char *name, size_t len);
void *ngx_hash_find_combined(ngx_hash_combined_t *hash, ngx_uint_t key,
    u_char *name, size_t len);

ngx_int_t ngx_hash_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts);
ngx_int_t ngx_hash_wildcard_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts);

#define ngx_hash(key, c)   ((ngx_uint_t) key * 31 + c)
ngx_uint_t ngx_hash_key(u_char *data, size_t len);
ngx_uint_t ngx_hash_key_lc(u_char *data, size_t len);
ngx_uint_t ngx_hash_strlow(u_char *dst, u_char *src, size_t n);


ngx_int_t ngx_hash_keys_array_init(ngx_hash_keys_arrays_t *ha, ngx_uint_t type);
ngx_int_t ngx_hash_add_key(ngx_hash_keys_arrays_t *ha, ngx_str_t *key,
    void *value, ngx_uint_t flags);


#endif /* _NGX_HASH_H_INCLUDED_ */
