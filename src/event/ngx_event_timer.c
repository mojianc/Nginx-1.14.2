
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


ngx_rbtree_t              ngx_event_timer_rbtree;
static ngx_rbtree_node_t  ngx_event_timer_sentinel;

/*
 * the event timer rbtree may contain the duplicate keys, however,
 * it should not be a problem, because we use the rbtree to find
 * a minimum timer value only
 */
//初始化定时器
ngx_int_t
ngx_event_timer_init(ngx_log_t *log)
{
    //创建红黑树
    ngx_rbtree_init(&ngx_event_timer_rbtree, &ngx_event_timer_sentinel,
                    ngx_rbtree_insert_timer_value);

    return NGX_OK;
}

//nginx 查找最早的定时器来判断时间差
ngx_msec_t
ngx_event_find_timer(void)
{
    ngx_msec_int_t      timer;
    ngx_rbtree_node_t  *node, *root, *sentinel;
    /*这里判断发现定时器的红黑树是空的,意味着没有定时事件需要处理 
      因此,返回的事件是无限长的 注意这个时间对事件处理非常关键
    */
    if (ngx_event_timer_rbtree.root == &ngx_event_timer_sentinel) {
        return NGX_TIMER_INFINITE;
    }
    /*得到红黑树的根节点和哨兵节点*/
    root = ngx_event_timer_rbtree.root;
    sentinel = ngx_event_timer_rbtree.sentinel;
    /*找到设置时间最小的定时器节点*/
    node = ngx_rbtree_min(root, sentinel);
    /*
      比较当前时间和这个定时器节点中保存的时间戳
      事件戳记录的是将要处理某个事件的时间点 当然也可能会小于当前的时间戳
    */
    timer = (ngx_msec_int_t) (node->key - ngx_current_msec);
     /*
      返回的时间差最小值不能超过0 如果为0 意味着事件必须立即进行处理（因为有超时的）
      如果大于0 则意味着事件有一段的处理富余时间
      这里的富余时间会在调用者那里进行纠正 不会让富余的时间超过500ms
    */
    return (ngx_msec_t) (timer > 0 ? timer : 0);
}

//nginx处理过期(可能有超时的事件)
void
ngx_event_expire_timers(void)
{
    ngx_event_t        *ev;
    ngx_rbtree_node_t  *node, *root, *sentinel;

    sentinel = ngx_event_timer_rbtree.sentinel;

    for ( ;; ) {
        root = ngx_event_timer_rbtree.root;
        /*红黑树是空的 不需要进行处理了*/
        if (root == sentinel) {
            return;
        }
        //找到最小时间戳的定时器
        node = ngx_rbtree_min(root, sentinel);

        /* node->key > ngx_current_msec */
        //这里发现最小时间戳的定时器的时间大于当前时间 说明红黑树中肯定没有超时事件需要处理直接返回即可
        if ((ngx_msec_int_t) (node->key - ngx_current_msec) > 0) {
            return;
        }
        //通过节点地址与节点在结构体中的偏移量算出事件的地址(也就是指针)
        ev = (ngx_event_t *) ((char *) node - offsetof(ngx_event_t, timer));

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "event timer del: %d: %M",
                       ngx_event_ident(ev->data), ev->timer.key);
        /*删除这个超时的定时器*/
        ngx_rbtree_delete(&ngx_event_timer_rbtree, &ev->timer);

#if (NGX_DEBUG)
        ev->timer.left = NULL;
        ev->timer.right = NULL;
        ev->timer.parent = NULL;
#endif
        /*设置没有设置过定时器*/
        ev->timer_set = 0;
        /*设置定时器发生超时标志*/
        ev->timedout = 1;
        /*交由事件处理函数处理*/
        ev->handler(ev);
    }
}

//判定红黑树中是否存在不可终止(cancel)的定时器
ngx_int_t
ngx_event_no_timers_left(void)
{
    ngx_event_t        *ev;
    ngx_rbtree_node_t  *node, *root, *sentinel;

    sentinel = ngx_event_timer_rbtree.sentinel;
    root = ngx_event_timer_rbtree.root;
    //红黑树是空的 直接返回OK 表示可终止
    if (root == sentinel) {
        return NGX_OK;
    }

    for (node = ngx_rbtree_min(root, sentinel);
         node;
         node = ngx_rbtree_next(&ngx_event_timer_rbtree, node))
    {
        /*这里遍历红黑树并得到 定时器对应的事件指针*/
        ev = (ngx_event_t *) ((char *) node - offsetof(ngx_event_t, timer));
        /*这里发现事件不可终止 需要进行重试*/
        if (!ev->cancelable) {
            return NGX_AGAIN;
        }
    }

    /* only cancelable timers left */
    /*遍历完成 发现定时器的事件均可终止*/
    return NGX_OK;
}
