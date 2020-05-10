
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_EVENT_TIMER_H_INCLUDED_
#define _NGX_EVENT_TIMER_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define NGX_TIMER_INFINITE  (ngx_msec_t) -1

#define NGX_TIMER_LAZY_DELAY  300


ngx_int_t ngx_event_timer_init(ngx_log_t *log);
ngx_msec_t ngx_event_find_timer(void);
void ngx_event_expire_timers(void);
ngx_int_t ngx_event_no_timers_left(void);


extern ngx_rbtree_t  ngx_event_timer_rbtree;

//nginx 删除定时器
static ngx_inline void
ngx_event_del_timer(ngx_event_t *ev)
{
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "event timer del: %d: %M",
                    ngx_event_ident(ev->data), ev->timer.key);
    //通过事件的timer(实际上是红黑树的节点)来删除红黑树的对应的节点
    ngx_rbtree_delete(&ngx_event_timer_rbtree, &ev->timer);

#if (NGX_DEBUG)
    ev->timer.left = NULL;
    ev->timer.right = NULL;
    ev->timer.parent = NULL;
#endif
    //设置这个事件没有设置过定时器
    ev->timer_set = 0;
}

//nginx添加定时器
static ngx_inline void
ngx_event_add_timer(ngx_event_t *ev, ngx_msec_t timer)
{
    ngx_msec_t      key;
    ngx_msec_int_t  diff;
    //用设置的定时timer和当前的时间戳之和作为红黑树节点的key值
    key = ngx_current_msec + timer;

    if (ev->timer_set) {

        /*
         * Use a previous timer value if difference between it and a new
         * value is less than NGX_TIMER_LAZY_DELAY milliseconds: this allows
         * to minimize the rbtree operations for fast connections.
         */
        //这里发现事件的定时器已经被设置过这里会用已经设置过的key与现在的取差值
        diff = (ngx_msec_int_t) (key - ev->timer.key);
        //这里的延时时间间隔是300毫秒, 这里发现差值 小于设置的延时事件间隔 则仍旧使用之前设置的定时器
        if (ngx_abs(diff) < NGX_TIMER_LAZY_DELAY) {
            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                           "event timer: %d, old: %M, new: %M",
                            ngx_event_ident(ev->data), ev->timer.key, key);
            return;
        }
        /*否则 就会删除之前设置的定时器 */
        ngx_del_timer(ev);
    }
    /*设置新的定时器的key值*/
    ev->timer.key = key;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "event timer add: %d: %M:%M",
                    ngx_event_ident(ev->data), timer, ev->timer.key);
    /*将新的定时器插入到 红黑树中*/
    ngx_rbtree_insert(&ngx_event_timer_rbtree, &ev->timer);

    ev->timer_set = 1;
}


#endif /* _NGX_EVENT_TIMER_H_INCLUDED_ */
