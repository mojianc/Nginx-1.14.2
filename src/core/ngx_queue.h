
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


#ifndef _NGX_QUEUE_H_INCLUDED_
#define _NGX_QUEUE_H_INCLUDED_


typedef struct ngx_queue_s  ngx_queue_t;

/**
 * 链表的数据结构非常简单，ngx_queue_s会挂载到实体
 * 结构上。然后通过ngx_queue_s来做成链表
 */
struct ngx_queue_s {
    ngx_queue_t  *prev;
    ngx_queue_t  *next;
};


/**
 * 初始化一个Q
 */
#define ngx_queue_init(q)                                                     \
    (q)->prev = q;                                                            \
    (q)->next = q


/**
 * 判断是否是空Q
 */
#define ngx_queue_empty(h)                                                    \
    (h == (h)->prev)


/**
 * 向链表H后面插入一个x的Q，支持中间插入
 */
#define ngx_queue_insert_head(h, x)                                           \
    (x)->next = (h)->next;                                                    \
    (x)->next->prev = x;                                                      \
    (x)->prev = h;                                                            \
    (h)->next = x


#define ngx_queue_insert_after   ngx_queue_insert_head

/**
 * 向链表H前面插入一个x的Q，支持中间插入
 */
#define ngx_queue_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x

/**
 * h是尾部，链表的第一个元素
 */
#define ngx_queue_head(h)                                                     \
    (h)->next

// h 是头，h 的上一个就是尾
#define ngx_queue_last(h)                                                     \
    (h)->prev


#define ngx_queue_sentinel(h)                                                 \
    (h)


/**
 * 返回节点Q的下一个元素
 */
#define ngx_queue_next(q)                                                     \
    (q)->next


/**
 * 返回节点Q的上一个元素
 */
#define ngx_queue_prev(q)                                                     \
    (q)->prev


#if (NGX_DEBUG)

/**
 * 移除某一个节点
 */
#define ngx_queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next;                                              \
    (x)->prev = NULL;                                                         \
    (x)->next = NULL

#else

#define ngx_queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next

#endif


//分割一个链表
#define ngx_queue_split(h, q, n)                                              \
    (n)->prev = (h)->prev;                                                    \
    (n)->prev->next = n;                                                      \
    (n)->next = q;                                                            \
    (h)->prev = (q)->prev;                                                    \
    (h)->prev->next = h;                                                      \
    (q)->prev = n;


#define ngx_queue_add(h, n)                                                   \
    (h)->prev->next = (n)->next;                                              \
    (n)->next->prev = (h)->prev;                                              \
    (h)->prev = (n)->prev;                                                    \
    (h)->prev->next = h;

/**
 * 通过业务结构体中ngx_queue_t数据结构的偏移量来得到业务主体的数据结构的指针地址
 * 此函数，是Nginx 链表设计的关键点!!!
 * 通过链表可以找到结构体所在的指针
 * 例如:
 * typedef struct
 {
    int key;
    char name[32];
    ngx_queue_t link;
 }ngx_qTest_t;
 // 通过ngx_queue_t指针，来比较对应结构体ngx_qTest_t的大小
 ngx_int_t cmp(const ngx_queue_t *a, const ngx_queue_t *b)
 {
    ngx_qTest_t *aTest = ngx_queue_data(a, ngx_qTest_t, link); //ngx_queue_data返回的是链表结点所在结构体的指针，绝了！
    ngx_qTest_t *bTest = ngx_queue_data(b, ngx_qTest_t, link);
    return aTest->key < bTest->key;
 }
 * 可以参考：https://blog.csdn.net/daniel_ustc/article/details/19008263
 * 例子来源:https://blog.csdn.net/daniel_ustc/article/details/17094285#t1
 */
#define ngx_queue_data(q, type, link)                                         \
    (type *) ((u_char *) q - offsetof(type, link))


ngx_queue_t *ngx_queue_middle(ngx_queue_t *queue);
void ngx_queue_sort(ngx_queue_t *queue,
    ngx_int_t (*cmp)(const ngx_queue_t *, const ngx_queue_t *));


#endif /* _NGX_QUEUE_H_INCLUDED_ */
