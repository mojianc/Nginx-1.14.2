
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define DEFAULT_CONNECTIONS  512

//这里各个模块并不像phase_handler或者filter那样用某种结构来存储，而是直接外部声明（方便直接调用）
extern ngx_module_t ngx_kqueue_module;
extern ngx_module_t ngx_eventport_module;
extern ngx_module_t ngx_devpoll_module;
extern ngx_module_t ngx_epoll_module;
extern ngx_module_t ngx_select_module;


static char *ngx_event_init_conf(ngx_cycle_t *cycle, void *conf);
static ngx_int_t ngx_event_module_init(ngx_cycle_t *cycle);
static ngx_int_t ngx_event_process_init(ngx_cycle_t *cycle);
static char *ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static void *ngx_event_core_create_conf(ngx_cycle_t *cycle);
static char *ngx_event_core_init_conf(ngx_cycle_t *cycle, void *conf);


static ngx_uint_t     ngx_timer_resolution;
sig_atomic_t          ngx_event_timer_alarm;

static ngx_uint_t     ngx_event_max_module;

ngx_uint_t            ngx_event_flags;
ngx_event_actions_t   ngx_event_actions;


static ngx_atomic_t   connection_counter = 1;
ngx_atomic_t         *ngx_connection_counter = &connection_counter;


ngx_atomic_t         *ngx_accept_mutex_ptr;
ngx_shmtx_t           ngx_accept_mutex;
ngx_uint_t            ngx_use_accept_mutex;
ngx_uint_t            ngx_accept_events;
ngx_uint_t            ngx_accept_mutex_held;
ngx_msec_t            ngx_accept_mutex_delay;
ngx_int_t             ngx_accept_disabled;


#if (NGX_STAT_STUB)

static ngx_atomic_t   ngx_stat_accepted0;
ngx_atomic_t         *ngx_stat_accepted = &ngx_stat_accepted0;
static ngx_atomic_t   ngx_stat_handled0;
ngx_atomic_t         *ngx_stat_handled = &ngx_stat_handled0;
static ngx_atomic_t   ngx_stat_requests0;
ngx_atomic_t         *ngx_stat_requests = &ngx_stat_requests0;
static ngx_atomic_t   ngx_stat_active0;
ngx_atomic_t         *ngx_stat_active = &ngx_stat_active0;
static ngx_atomic_t   ngx_stat_reading0;
ngx_atomic_t         *ngx_stat_reading = &ngx_stat_reading0;
static ngx_atomic_t   ngx_stat_writing0;
ngx_atomic_t         *ngx_stat_writing = &ngx_stat_writing0;
static ngx_atomic_t   ngx_stat_waiting0;
ngx_atomic_t         *ngx_stat_waiting = &ngx_stat_waiting0;

#endif



static ngx_command_t  ngx_events_commands[] = {

    { ngx_string("events"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_events_block,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_events_module_ctx = {
    ngx_string("events"),
    NULL,
    ngx_event_init_conf
};


ngx_module_t  ngx_events_module = {
    NGX_MODULE_V1,
    &ngx_events_module_ctx,                /* module context */
    ngx_events_commands,                   /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_str_t  event_core_name = ngx_string("event_core");


static ngx_command_t  ngx_event_core_commands[] = {
    /* 连接池的大小，也就是每个 worker 进程中支持的 TCP 最大连接数，默认为512 */
    { ngx_string("worker_connections"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_connections,
      0,
      0,
      NULL },
    /* 指定使用哪个事件模型（epoll，select，kequeue等等）模块来处理事件，默认由nginx在configure时根据配置项决定 */
    { ngx_string("use"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_use,
      0,
      0,
      NULL },
    /* 对应事件定义 ngx_event_s 结构体的成员 available 字段。用于设置尽可能多的接收连接，
       即在某监听套接字上接收到新连接时循环调用accpet接收所有的新连接，默认关闭 */
    { ngx_string("multi_accept"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, multi_accept),
      NULL },
    /* 确定是否使用 accept_mutex 负载均衡锁，默认为开启 */
    { ngx_string("accept_mutex"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex),
      NULL },
     /* 启用 accept_mutex 负载均衡锁后，延迟 accept_mutex_delay 毫秒后再试图处理新连接事件，默认500ms */
    { ngx_string("accept_mutex_delay"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex_delay),
      NULL },
    /* 需要对来自指定 IP地址 的 TCP 连接打印 debug 级别的调试日志，此功能需要在configure时增加 --with-debug选项 */
    { ngx_string("debug_connection"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_debug_connection,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_event_module_t  ngx_event_core_module_ctx = {
    &event_core_name,
    //在配置项解析前调用分配保存配置项的内存空间
    ngx_event_core_create_conf,            /* create configuration */
    //在配置项解析完后调用对配置文件中不存在的事件配置进行初始化
    ngx_event_core_init_conf,              /* init configuration */

    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/* 该模块会决定使用哪种事件驱动机制，以及如何管理事件 */
ngx_module_t  ngx_event_core_module = {
    NGX_MODULE_V1,
    &ngx_event_core_module_ctx,            /* module context */
    ngx_event_core_commands,               /* module directives */
    NGX_EVENT_MODULE,                      /* module type */
    NULL,                                  /* init master */
    /* 没有 fork 出 worker 子进程时，会调用该函数 */
    ngx_event_module_init,                 /* init module */
    /* fork出子进程后，每一个worker进程会在用 ngx_event_core_module 模块的 ngx_event_process_init 方法后才会进入正式的工作循环 */
    ngx_event_process_init,                /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

/**
 * 在开启负载均衡的情况下，在ngx_event_process_init()函数中跳过了将监听套接口加入到
 * 事件监控机制，真正将监听套接口加入到事件监控机制是在ngx_process_events_and_timers()
 * 里。工作进程的主要执行体是一个无限的for循环，而在该循环内最重要的函数调用就是
 * ngx_process_events_and_timers()，所以在该函数内动态添加或删除监听套接口是一种很灵活
 * 的方式。如果当前工作进程负载比较小，就将监听套接口加入到自身的事件监控机制里，从而
 * 带来新的客户端请求；而如果当前工作进程负载比较大，就将监听套接口从自身的事件监控机制里
 * 删除，避免引入新的客户端请求而带来更大的负载。
 */
 /*
  * 参数含义：
  * - cycle是当前进程的ngx_cycle_t结构体指针
  * 
  * 执行意义:
  * 使用事件模块处理截止到现在已经收集到的事件.
  */
void
ngx_process_events_and_timers(ngx_cycle_t *cycle)
{
    ngx_uint_t  flags;
    ngx_msec_t  timer, delta;
    /*
     * Nginx具体使用哪种超时检测方案主要取决于一个nginx.conf的配置指令timer_resolution，即对应
     * 的全局变量 ngx_timer_resolution。 */
    
    /* 如果配置文件中使用了 timer_resolution 配置项，也就是 ngx_timer_resolution 值大于 0，
     * 则说明用户希望服务器时间精确度为 ngx_timer_resolution 毫秒。这时，将 ngx_process_events 的 
     *  timer 参数设置为 -1，告诉 ngx_process_events 方法在检测事件时不要等待，直接收集所有已经
     * 就绪的事件然后返回；同时将 flags 参数置为 0，即告诉 ngx_process_events 没有任何附加动作。
     */
    if (ngx_timer_resolution) {
        timer = NGX_TIMER_INFINITE;
        flags = 0;

    } else {
        /* 如果没有使用 timer_resolution，那么将调用 ngx_event_find_timer() 方法获取最近一个将要
         * 触发的事件距离现在有多少毫秒，然后把这个值赋予 timer 参数，告诉 ngx_process_events 
         * 方法在检测事件时如果没有任何事件，最多等待 timer 毫秒就返回；将 flags 参数设置为 
         * NGX_UPDATE_TIME，告诉 ngx_process_events 方法更新缓存的时间 */
        timer = ngx_event_find_timer();
        flags = NGX_UPDATE_TIME;

#if (NGX_WIN32)

        /* handle signals from master in case of network inactivity */

        if (timer == NGX_TIMER_INFINITE || timer > 500) {
            timer = 500;
        }

#endif
    }
    /* 开启了负载均衡的情况下，若当前使用的连接到达总连接数的7/8时，就不会再处理
     * 新连接了，同时，在每次调用process_events时都会将ngx_accept_disabled减1，
     * 直到ngx_accept_disabled降到总连接数的7/8以下时，才会调用ngx_trylock_accept_mutex
     * 试图去处理新连接事件 */
    if (ngx_use_accept_mutex) {
        /**
		 * 	ngx_accept_disabled = ngx_cycle->connection_n / 8 - ngx_cycle->free_connection_n;
		 * 	当空闲连接数小于connection达到连接总数的1/8的时候，就不再处理新的连接accept事件，只处理当前连接的read事件
		 * 	这个是比较简单的一种负载均衡方法
         * 检测变量 ngx_accept_disabled 值是否大于0来判断当前进程是否
         * 已经过载，为什么可以这样判断需要理解变量ngx_accept_disabled
         * 值的含义，这在accept()接受新连接请求的处理函数ngx_event_accept()
         * 内可以看到。
         * 当ngx_accept_disabled大于0，表示处于过载状态，因为仅仅是自减一，
         * 当经过一段时间又降到0以下，便可争用锁获取新的请求连接。
         */
        if (ngx_accept_disabled > 0) {
            ngx_accept_disabled--;

        } else {
            /* 
             * 若进程没有处于过载状态，那么就会尝试争用该锁获取新的请求连接。
             * 实际上是争用监听套接口的监控权，争锁成功就会把所有监听套接口
             * (注意，是所有的监听套接口，它们总是作为一个整体被加入或删除)
             * 加入到自身的事件监控机制里（如果原本不在）；争锁失败就会把监听
             * 套接口从自身的事件监控机制里删除（如果原本就在）。从下面的函数
             * 可以看到这点。 
             */
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) {
                /* 发生错误则直接返回 */
                return;
            }
            /* 若获取到锁，则给flags添加NGX_POST_EVENTS标记，表示所有发生的事件都将延后
             * 处理。这是任何架构设计都必须遵守的一个约定，即持锁者必须尽量缩短自身持锁的时
             * 间，Nginx亦如此，所以照此把大部分事件延迟到释放锁之后再去处理，把锁尽快释放，
             * 缩短自身持锁的时间能让其他进程尽可能的有机会获取到锁。*/
            if (ngx_accept_mutex_held) {
                /**
				 * 给flags增加标记NGX_POST_EVENTS，这个标记作为处理时间核心函数ngx_process_events的一个参数，这个函数中所有事件将延后处理。
				 * accept事件都放到ngx_posted_accept_events链表中，
				 * epollin|epollout普通事件都放到ngx_posted_events链表中
                 */
                flags |= NGX_POST_EVENTS;

            } else {
                /**
				 * 1. 获取锁失败，意味着既不能让当前worker进程频繁的试图抢锁，也不能让它经过太长事件再去抢锁
				 * 2. 开启了timer_resolution时间精度，需要让ngx_process_change方法在没有新事件的时候至少等待ngx_accept_mutex_delay毫秒之后再去试图抢锁
				 * 3. 没有开启时间精度时，如果最近一个定时器事件的超时时间距离现在超过了ngx_accept_mutex_delay毫秒，也要把timer设置为ngx_accept_mutex_delay毫秒
				 * 4. 不能让ngx_process_change方法在没有新事件的时候等待的时间超过ngx_accept_mutex_delay，这会影响整个负载均衡机制
				 * 5. 如果拿到锁的进程能很快处理完accpet，而没拿到锁的一直在等待，容易造成进程忙的很忙，空的很空
                 */
                if (timer == NGX_TIMER_INFINITE
                    || timer > ngx_accept_mutex_delay)
                {
                    timer = ngx_accept_mutex_delay;
                }
            }
        }
    }
    /* 调用 ngx_process_events 方法，并计算 ngx_process_events 执行时消耗的时间 */
    delta = ngx_current_msec;
    /**
	 * 事件调度函数
	 * 1. 当拿到锁，flags=NGX_POST_EVENTS的时候，不会直接处理事件，
	 * 将accept事件放到ngx_posted_accept_events，read事件放到ngx_posted_events队列
	 * 2. 当没有拿到锁，则处理的全部是read事件，直接进行回调函数处理
	 * 参数：timer-epoll_wait超时时间  (ngx_accept_mutex_delay-延迟拿锁事件   NGX_TIMER_INFINITE-正常的epollwait等待事件)
     */
    (void) ngx_process_events(cycle, timer, flags); //ngx_event_actions.process_events()
                                                    //   |--ngx_epoll_module->ngx_event_actions_t->ngx_epoll_process_events()
                                                    //      |--epoll_wait()
    /* delta 的值即为 ngx_process_events 执行时消耗的毫秒数 */
    delta = ngx_current_msec - delta;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "timer delta: %M", delta);
    /**
	 * 1. ngx_posted_accept_events是一个事件队列，暂存epoll从监听套接口wait到的accept事件
	 * 2. 这个方法是循环处理accpet事件列队上的accpet事件
     * 接下来先处理新建连接缓存事件ngx_posted_accept_events，此时还不能释放锁，因为我们还在处理
     * 监听套接口上的事件，还要读取上面的请求数据，所以必须独占，一旦缓存的新建连接事件全部被处
     * 理完就必须马上释放持有的锁了，因为连接套接口只可能被某一个进程至始至终的占有，不会出现多
     * 进程之间的相互冲突，所以对于连接套接口上事件ngx_posted_events的处理可以在释放锁之后进行，
     * 虽然对于它们的具体处理与响应是最消耗时间的，不过在此之前已经释放了持有的锁，所以即使慢一点
     * 也不会影响到其他进程。
     */
    ngx_event_process_posted(cycle, &ngx_posted_accept_events);
    /**
	 * 如果拿到锁，处理完accept事件后，则释放锁
     */
    if (ngx_accept_mutex_held) {
        ngx_shmtx_unlock(&ngx_accept_mutex);
    }
    /* 若ngx_process_events方法执行时消耗的时间delta大于0，这时可能有新的定时器事件被触发，
     * 因此需要调用下面该函数处理所有满足条件的定时器事件 */
    if (delta) {
        /* 处理所有的超时事件 */
        ngx_event_expire_timers();
    }
    /**
     * 释放锁后再处理耗时长的连接套接口上的事件
	 *1. 普通事件都会存放在ngx_posted_events队列上
	 *2. 这个方法是循环处理read事件列队上的read事件
     */
    ngx_event_process_posted(cycle, &ngx_posted_events);
     /*
     * 补充两点。
     * 一：如果在处理新建连接事件的过程中，在监听套接口上又来了新的请求会怎么样？这没有关系，当前
     *     进程只处理已缓存的事件，新的请求将被阻塞在监听套接口上，而前面曾提到监听套接口是以 ET
     *     方式加入到事件监控机制里的，所以等到下一轮被哪个进程争取到锁并加到事件监控机制里时才会
     *     触发而被抓取出来。
     * 二：上面的代码中进行ngx_process_events()处理并处理完新建连接事件后，只是释放锁而并没有将监听
     *     套接口从事件监控机制里删除，所以有可能在接下来处理ngx_posted_events缓存事件的过程中，互斥
     *     锁被另外一个进程争抢到并把所有监听套接口加入到它的事件监控机制里。因此严格说来，在同一
     *     时刻，监听套接口只可能被一个进程监控（也就是epoll_wait()这种），因此进程在处理完
     *     ngx_posted_event缓存事件后去争用锁，发现锁被其他进程占有而争用失败，会把所有监听套接口从
     *     自身的事件监控机制里删除，然后才进行事件监控。在同一时刻，监听套接口只可能被一个进程
     *     监控，这就意味着Nginx根本不会受到惊群的影响，而不论Linux内核是否已经解决惊群问题。
     */
}

/* 
 * @rev: 要操作的事件
 * @flags：指定事件的驱动方式。对于不同的事件驱动模块，flags的取值范围并不同，对于epoll来说，flags
 *         的取值返回可以是 0 或者 NGX_CLOSE_EVENT(NGX_CLOSE_EVENT仅在epoll的LT水平触发模式
 *         下有效),Nginx主要工作在ET模式下，一般可以忽略flags这个参数
 * 
 * 将读事件添加到事件驱动模块中，这样该事件对应的TCP连接上一旦出现可读事件(如接收到
 * TCP连接另一端发送来的字符流)就会回调该事件的handler方法 
 */
ngx_int_t
ngx_handle_read_event(ngx_event_t *rev, ngx_uint_t flags)
{
    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        /* kqueue, epoll */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_CLEAR_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }
        }

        return NGX_OK;

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {

        /* select, poll, /dev/poll */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (rev->active && (rev->ready || (flags & NGX_CLOSE_EVENT))) {
            if (ngx_del_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT | flags)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

    } else if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT) {

        /* event ports */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (rev->oneshot && !rev->ready) {
            if (ngx_del_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }

    /* iocp */

    return NGX_OK;
}

/*
 * @wev: 要操作的写事件
 * @lowat: 表示当连接对应的套接字缓冲区中必须有lowat大小的可用空间时，事件收集器(如select或者
 *         epoll_wait调用)才能处理这个可写事件(lowat为0表示不考虑可写缓冲区的大小)
 */
ngx_int_t
ngx_handle_write_event(ngx_event_t *wev, size_t lowat)
{
    ngx_connection_t  *c;

    if (lowat) {
        c = wev->data;
        //设置只有epoll缓冲区大小为lowat，只有lowat大小的缓冲区才能写入
        if (ngx_send_lowat(c, lowat) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        /* kqueue, epoll */

        if (!wev->active && !wev->ready) {
            //添加可写事件，设置水平触发
            if (ngx_add_event(wev, NGX_WRITE_EVENT,
                              NGX_CLEAR_EVENT | (lowat ? NGX_LOWAT_EVENT : 0))
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }
        }

        return NGX_OK;

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) { //一次性读取或写入所有数据

        /* select, poll, /dev/poll */

        if (!wev->active && !wev->ready) {
            //如果event未激活并且未ready，插入
            if (ngx_add_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (wev->active && wev->ready) {
            //如果event已经激活并且ready，删除
            if (ngx_del_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

    } else if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT) {

        /* event ports */

        if (!wev->active && !wev->ready) {
            if (ngx_add_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (wev->oneshot && wev->ready) {
            if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }

    /* iocp */

    return NGX_OK;
}


static char *
ngx_event_init_conf(ngx_cycle_t *cycle, void *conf)
{
    if (ngx_get_conf(cycle->conf_ctx, ngx_events_module) == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "no \"events\" section in configuration");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

//该函数在init_cycle中的init_module函数中被调用。调用时worker还没有启动
static ngx_int_t
ngx_event_module_init(ngx_cycle_t *cycle)
{
    void              ***cf;
    u_char              *shared;
    size_t               size, cl;
    ngx_shm_t            shm;
    ngx_time_t          *tp;
    ngx_core_conf_t     *ccf;
    ngx_event_conf_t    *ecf;
    /* 获取 ngx_events_module 模块持有的关于事件模块的总配置项结构体指针数组 */
    cf = ngx_get_conf(cycle->conf_ctx, ngx_events_module);
    /* 在总配置项结构体指针数组中获取 ngx_event_core_module 模块的配置项结构体 */
    ecf = (*cf)[ngx_event_core_module.ctx_index];

    if (!ngx_test_config && ngx_process <= NGX_PROCESS_MASTER) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "using the \"%s\" event method", ecf->name);
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    ngx_timer_resolution = ccf->timer_resolution;

#if !(NGX_WIN32)
    {
    ngx_int_t      limit;
    struct rlimit  rlmt;

    if (getrlimit(RLIMIT_NOFILE, &rlmt) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "getrlimit(RLIMIT_NOFILE) failed, ignored");

    } else {
        if (ecf->connections > (ngx_uint_t) rlmt.rlim_cur
            && (ccf->rlimit_nofile == NGX_CONF_UNSET
                || ecf->connections > (ngx_uint_t) ccf->rlimit_nofile))
        {
            limit = (ccf->rlimit_nofile == NGX_CONF_UNSET) ?
                         (ngx_int_t) rlmt.rlim_cur : ccf->rlimit_nofile;

            ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                          "%ui worker_connections exceed "
                          "open file resource limit: %i",
                          ecf->connections, limit);
        }
    }
    }
#endif /* !(NGX_WIN32) */


    if (ccf->master == 0) {
        return NGX_OK;
    }

    if (ngx_accept_mutex_ptr) {
        return NGX_OK;
    }


    /* cl should be equal to or greater than cache line size */

    cl = 128;

    size = cl            /* ngx_accept_mutex */
           + cl          /* ngx_connection_counter */
           + cl;         /* ngx_temp_number */

#if (NGX_STAT_STUB)

    size += cl           /* ngx_stat_accepted */
           + cl          /* ngx_stat_handled */
           + cl          /* ngx_stat_requests */
           + cl          /* ngx_stat_active */
           + cl          /* ngx_stat_reading */
           + cl          /* ngx_stat_writing */
           + cl;         /* ngx_stat_waiting */

#endif

    shm.size = size;
    ngx_str_set(&shm.name, "nginx_shared_zone");
    shm.log = cycle->log;
    //内部调用mmap申请一块共享内存，这样fork出来的worker也能继承这块共享内存的地址
    //worker进程可以通过继承的共享内存地址来实现与master以及其他worker的通信
    if (ngx_shm_alloc(&shm) != NGX_OK) {
        return NGX_ERROR;
    }

    
    //函数ngx_shm_alloc是通过系统调用mmap分配的内存空间，首地址为shared；
    shared = shm.addr;
    //这里创建了三个共享变量ngx_accept_mutex、ngx_connection_counter和ngx_temp_number；
    //函数ngx_shmtx_create使得ngx_accept_mutex->lock变量指向shared；ngx_connection_counter指向shared+128字节位置处，ngx_temp_number指向shared+256字节位置处。
    ngx_accept_mutex_ptr = (ngx_atomic_t *) shared;
    ngx_accept_mutex.spin = (ngx_uint_t) -1;
    //将accept锁放到共享内存中，这样就实现了对多个进程的互斥、同步操作
    if (ngx_shmtx_create(&ngx_accept_mutex, (ngx_shmtx_sh_t *) shared,
                         cycle->lock_file.data)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_connection_counter = (ngx_atomic_t *) (shared + 1 * cl);

    (void) ngx_atomic_cmp_set(ngx_connection_counter, 0, 1);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "counter: %p, %uA",
                   ngx_connection_counter, *ngx_connection_counter);

    ngx_temp_number = (ngx_atomic_t *) (shared + 2 * cl);

    tp = ngx_timeofday();

    ngx_random_number = (tp->msec << 16) + ngx_pid;

#if (NGX_STAT_STUB)

    ngx_stat_accepted = (ngx_atomic_t *) (shared + 3 * cl);
    ngx_stat_handled = (ngx_atomic_t *) (shared + 4 * cl);
    ngx_stat_requests = (ngx_atomic_t *) (shared + 5 * cl);
    ngx_stat_active = (ngx_atomic_t *) (shared + 6 * cl);
    ngx_stat_reading = (ngx_atomic_t *) (shared + 7 * cl);
    ngx_stat_writing = (ngx_atomic_t *) (shared + 8 * cl);
    ngx_stat_waiting = (ngx_atomic_t *) (shared + 9 * cl);

#endif

    return NGX_OK;
}


#if !(NGX_WIN32)

static void
ngx_timer_signal_handler(int signo)
{
    ngx_event_timer_alarm = 1;

#if 1
    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ngx_cycle->log, 0, "timer signal");
#endif
}

#endif

/**该函数ngx_worker_process_init中的init_process函数中被调用，因为执行阶段位于
 *worker进程初始化时，所以这里申请的资源都属于各个worker独有，不用担心因被继承而导致的资源浪费
 *在这个函数中完成了对从master进程继承的listen fd的初始化工作
 */
static ngx_int_t
ngx_event_process_init(ngx_cycle_t *cycle)
{
    ngx_uint_t           m, i;
    ngx_event_t         *rev, *wev;
    ngx_listening_t     *ls;
    ngx_connection_t    *c, *next, *old;
    ngx_core_conf_t     *ccf;
    ngx_event_conf_t    *ecf;
    ngx_event_module_t  *module;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);
    /* 当打开 accept_mutex 负载均衡锁，同时使用了 master 模式且 worker 进行数量大于 1 时，
     * 才正式确定了进程将使用 accept_mutex 负载均衡锁。因此，即使我们在配置文件中指定打开
     * accept_mutex 锁，如果没有使用 master 模式或者 worker 进程数量等于 1，进程在运行时
     * 还是不会使用负载均衡锁（既然不存在多个进程去抢一个监听端口上的连接的情况，自然就不
     * 需要均衡多个 worker 进程的负载）*/
    if (ccf->master && ccf->worker_processes > 1 && ecf->accept_mutex) {
        /* 这里才置位了才明确表示使用负载均衡锁 */
        ngx_use_accept_mutex = 1;
        ngx_accept_mutex_held = 0;
        ngx_accept_mutex_delay = ecf->accept_mutex_delay;

    } else {
         /* 关闭负载均衡锁 */
        ngx_use_accept_mutex = 0;
    }

#if (NGX_WIN32)

    /*
     * disable accept mutex on win32 as it may cause deadlock if
     * grabbed by a process which can't accept connections
     */

    ngx_use_accept_mutex = 0;

#endif
    //创建保存event的队列用于保存accept、普通事件
    ngx_queue_init(&ngx_posted_accept_events);
    ngx_queue_init(&ngx_posted_events);
    /*初始化计时器，此处将会创建起一颗红黑色，来维护计时器。*/   
    if (ngx_event_timer_init(cycle->log) == NGX_ERROR) {
        return NGX_ERROR;
    }
    /* 找到事件模型的模块，例如epoll/kqueue */
    /* 在调用 use 配置项指定的事件模块中，在 ngx_event_module_t 接口下，ngx_event_actions_t 中的 init 方法进行这个事件模块的初始化工作 */
    for (m = 0; cycle->modules[m]; m++) {
        if (cycle->modules[m]->type != NGX_EVENT_MODULE) {
            continue;
        }

        if (cycle->modules[m]->ctx_index != ecf->use) {
            continue;
        }

        module = cycle->modules[m]->ctx;
        /* 调用epoll/kqueue等模型模块的init初始化函数,epoll调用的是ngx_epoll_init这个方法 */
        if (module->actions.init(cycle, ngx_timer_resolution) != NGX_OK) {  // ngx_epoll_module->ngx_epoll_module_ctx->action->ngx_epoll_init()
                                                                            // ngx_event_core_module->NULL
            /* fatal */
            exit(2);
        }

        break;
    }

#if !(NGX_WIN32)
     /* 如果 nginx.conf 配置文件中设置了 timer_resolution 配置项，即表明需要控制时间
     * 精度，这时会调用 setitimer 方法，设置时间间隔为 timer_resolution 毫秒来回调
     * ngx_timer_signal_handler 方法 */
    if (ngx_timer_resolution && !(ngx_event_flags & NGX_USE_TIMER_EVENT)) {
        struct sigaction  sa;
        struct itimerval  itv;

        ngx_memzero(&sa, sizeof(struct sigaction));
        /* 在 ngx_timer_signal_handler 方法中仅是对全局变量 ngx_event_timer_alarm 
         * 置 1，表示需要更新时间，在 ngx_event_actions_t 的 process_events 方法中，
         * 每一个事件驱动模块都需要在 ngx_event_timer_alarm 为 1 时调用 
         * ngx_time_update 方法更新系统时间，在更新系统时间结束后需要将 
         * ngx_event_timer_alarm 置为 0 */
        sa.sa_handler = ngx_timer_signal_handler;
        sigemptyset(&sa.sa_mask);
        /*SIGALRM信号成功安装后，在什么情况下进程会收到该信号呢？这就要依赖于Linux提供的定时器功能。
        在Linux系统下，每一个进程都有惟一的一个定时器，该定时器提供了以秒为单位的定时功能。
        在定时器设置的超时时间到达后，调用 setitimer()的进程将收到SIGALRM信号。
        */
        if (sigaction(SIGALRM, &sa, NULL) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "sigaction(SIGALRM) failed");
            return NGX_ERROR;
        }

        itv.it_interval.tv_sec = ngx_timer_resolution / 1000;
        itv.it_interval.tv_usec = (ngx_timer_resolution % 1000) * 1000;
        itv.it_value.tv_sec = ngx_timer_resolution / 1000;
        itv.it_value.tv_usec = (ngx_timer_resolution % 1000 ) * 1000;
        //setitimer()可用来实现延时和定时的功能;ITIMER_REAL：以系统真实的时间来计算，它送出SIGALRM信号。
        if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "setitimer() failed");
        }
    }

    if (ngx_event_flags & NGX_USE_FD_EVENT) {
        struct rlimit  rlmt;
        //获取指定比进程可打开的最大文件描述符大1的值，超出此值，将会产生EMFILE错误。
        if (getrlimit(RLIMIT_NOFILE, &rlmt) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "getrlimit(RLIMIT_NOFILE) failed");
            return NGX_ERROR;
        }

        cycle->files_n = (ngx_uint_t) rlmt.rlim_cur;

        cycle->files = ngx_calloc(sizeof(ngx_connection_t *) * cycle->files_n,
                                  cycle->log);
        if (cycle->files == NULL) {
            return NGX_ERROR;
        }
    }

#else

    if (ngx_timer_resolution && !(ngx_event_flags & NGX_USE_TIMER_EVENT)) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "the \"timer_resolution\" directive is not supported "
                      "with the configured event method, ignored");
        ngx_timer_resolution = 0;
    }

#endif
    /* 预分配 ngx_connection_t 数组作为连接池，同时将 ngx_cycle_t 结构体中的
     * connections 成员指向该数组。数组的个数为 nginx.conf 配置文件中 
     * worker_connections 中配置的连接数 */
    cycle->connections =
        ngx_alloc(sizeof(ngx_connection_t) * cycle->connection_n, cycle->log);
    if (cycle->connections == NULL) {
        return NGX_ERROR;
    }

    c = cycle->connections;
    /* 预分配 ngx_event_t 事件数组作为读事件池，同时将 ngx_cycle_t 结构体中的 
     * read_events 成员指向该数组。数组的个数为 nginx.conf 配置文件中 
     * worker_connections 里配置的连接数 */
    cycle->read_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                   cycle->log);
    if (cycle->read_events == NULL) {
        return NGX_ERROR;
    }
 
    rev = cycle->read_events;
    for (i = 0; i < cycle->connection_n; i++) {
        rev[i].closed = 1;
        rev[i].instance = 1;
    }
    /* 预分配 ngx_event_t 事件数组作为写事件池，同时将 ngx_cycle_t 结构体中的 
     * write_events 成员指向该数组。数组的个数为 nginx.conf 配置文件中 
     * worker_connections 里配置的连接数 */
    cycle->write_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                    cycle->log);
    if (cycle->write_events == NULL) {
        return NGX_ERROR;
    }

    wev = cycle->write_events;
    /* 将所有写事件池的写事件置为关闭状态，即未使用 */
    for (i = 0; i < cycle->connection_n; i++) {
        wev[i].closed = 1;
    }

    i = cycle->connection_n;
    next = NULL;
    /* 按照序号，将上述 3 个数组相应的读/写事件设置到每一个 ngx_connection_t 连接
     * 对象中，同时把这些连接以 ngx_connection_t 中的 data 成员作为 next 指针串联
     * 成链表，为下一步设置空闲连接链表做好准备 */
    do {
        i--;

        c[i].data = next;
        c[i].read = &cycle->read_events[i];
        c[i].write = &cycle->write_events[i];
        c[i].fd = (ngx_socket_t) -1;

        next = &c[i];
    } while (i);
    /* 将 ngx_cycle_t 结构体中的空闲连接链表 free_connections 指向 connections 数组
     * 的第 1 个元素，也就是上一步所有 ngx_connection_t 连接通过 data 成员组成的
     * 单链表的首部 */
    cycle->free_connections = next;
    cycle->free_connection_n = cycle->connection_n;

    /* for each listening socket */
    /* 在刚刚建立好的连接池中，为所有 ngx_listening_t 监听对象中 connections 成员
     * 分配连接，同时对监听端口的读事件设置处理方法为 ngx_event_accept，也就是说，
     * 有新连接事件时将调用 ngx_event_accept 方法建立新连接 */
    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

#if (NGX_HAVE_REUSEPORT)
        //如果支持REUSEPORT选项，那么socket只能在一个worker上工作
        if (ls[i].reuseport && ls[i].worker != ngx_worker) {
            continue;
        }
#endif
        //从连接池中获取一个连接实例并进行部分初始化
        c = ngx_get_connection(ls[i].fd, cycle->log);

        if (c == NULL) {
            return NGX_ERROR;
        }

        c->type = ls[i].type;
        c->log = &ls[i].log;

        c->listening = &ls[i];
        ls[i].connection = c;

        rev = c->read;

        rev->log = c->log;
        rev->accept = 1;

#if (NGX_HAVE_DEFERRED_ACCEPT)
        rev->deferred_accept = ls[i].deferred_accept;
#endif

        if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) {
            if (ls[i].previous) {

                /*
                 * delete the old accept events that were bound to
                 * the old cycle read events array
                 */

                old = ls[i].previous->connection;

                if (ngx_del_event(old->read, NGX_READ_EVENT, NGX_CLOSE_EVENT)
                    == NGX_ERROR)
                {
                    return NGX_ERROR;
                }

                old->fd = (ngx_socket_t) -1;
            }
        }

#if (NGX_WIN32)

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
            ngx_iocp_conf_t  *iocpcf;

            rev->handler = ngx_event_acceptex;

            if (ngx_use_accept_mutex) {
                continue;
            }

            if (ngx_add_event(rev, 0, NGX_IOCP_ACCEPT) == NGX_ERROR) {
                return NGX_ERROR;
            }

            ls[i].log.handler = ngx_acceptex_log_error;

            iocpcf = ngx_event_get_conf(cycle->conf_ctx, ngx_iocp_module);
            if (ngx_event_post_acceptex(&ls[i], iocpcf->post_acceptex)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

        } else {
            rev->handler = ngx_event_accept;

            if (ngx_use_accept_mutex) {
                continue;
            }

            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

#else
        /* 设置读事件的回调方法 */
        rev->handler = (c->type == SOCK_STREAM) ? ngx_event_accept
                                                : ngx_event_recvmsg;

#if (NGX_HAVE_REUSEPORT)

        if (ls[i].reuseport) {
            /* 将监听对象连接的读事件添加到事件驱动模块中，这样，epoll 等事件模块就开始检测监听服务，并开始向用户提供服务了 */
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            continue;
        }

#endif

        if (ngx_use_accept_mutex) {
            continue;
        }

#if (NGX_HAVE_EPOLLEXCLUSIVE)

        if ((ngx_event_flags & NGX_USE_EPOLL_EVENT)
            && ccf->worker_processes > 1)
        {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_EXCLUSIVE_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            continue;
        }

#endif
        /* 将监听对象连接的读事件添加到事件驱动模块中，这样，epoll 等事件模块
         * 就开始检测监听服务，并开始向用户提供服务了 */
        if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }

#endif

    }

    return NGX_OK;
}


ngx_int_t
ngx_send_lowat(ngx_connection_t *c, size_t lowat)
{
    int  sndlowat;

#if (NGX_HAVE_LOWAT_EVENT)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
        c->write->available = lowat;
        return NGX_OK;
    }

#endif

    if (lowat == 0 || c->sndlowat) {
        return NGX_OK;
    }

    sndlowat = (int) lowat;

    if (setsockopt(c->fd, SOL_SOCKET, SO_SNDLOWAT,
                   (const void *) &sndlowat, sizeof(int))
        == -1)
    {
        ngx_connection_error(c, ngx_socket_errno,
                             "setsockopt(SO_SNDLOWAT) failed");
        return NGX_ERROR;
    }

    c->sndlowat = 1;

    return NGX_OK;
}

/*epool模块属于Event模块下面的子模块，配置文件初始化的时候，在Event解析配置文件的核心函数*/
//ngx_event_block这个函数与上面的ngx_init_cycle以及ngx_http_block函数的结构类似，都是
//先调用特定模块的create_conf函数，然后解析对应level的配置项（调用配置注册的函数），接着
//调用init_conf函数实现完成模块对应配置的初始化
static char *
ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                 *rv;
    void               ***ctx;
    ngx_uint_t            i;
    ngx_conf_t            pcf;
    ngx_event_module_t   *m;
    /* 检测配置项结构体是否已经存在 */
    if (*(void **) conf) {
        return "is duplicate";
    }

    /* count the number of the event modules and set up their indices */
    /* 计算出编译进 Nginx 的所有事件模块的总个数， */
    ngx_event_max_module = ngx_count_modules(cf->cycle, NGX_EVENT_MODULE);
    /* 创建该核心事件存储所有事件模块的总配置项结构体指针 */
    ctx = ngx_pcalloc(cf->pool, sizeof(void *));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }
    /* 为每个事件模块都分配一个空间用于放置指向该事件模块的配置项结构体指针 */
    *ctx = ngx_pcalloc(cf->pool, ngx_event_max_module * sizeof(void *));
    if (*ctx == NULL) {
        return NGX_CONF_ERROR;
    }
    /* conf 其实就是核心模块 ngx_events_module 在 ngx_cycle_t 核心结构体的成员 conf_ctx 指针数组
     * 相应位置的指针 */
    *(void **) conf = ctx;
    /* 模块初始化，如果是NGX_EVENT_MODULE，则调用模块的create_conf方法 */
    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = cf->cycle->modules[i]->ctx;
        //调用其他事件模块的create_conf函数
        if (m->create_conf) {
            (*ctx)[cf->cycle->modules[i]->ctx_index] =
                                                     m->create_conf(cf->cycle);//ngx_epoll_module->ngx_epoll_module_ctx->ngx_epoll_create_conf()
                                                                               //ngx_event_core_module->ngx_event_core_module_ctx->ngx_event_core_create_conf
            if ((*ctx)[cf->cycle->modules[i]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }

    pcf = *cf;
    //在解析events{}内的配置项时，需要用到上面创建的ctx指针数组。
    cf->ctx = ctx;
    cf->module_type = NGX_EVENT_MODULE;
    cf->cmd_type = NGX_EVENT_CONF;
    /* 调用配置解析，这次解析的是 递归解析events{}块中配置项，非文件内容 */
    //解析配置文件中涉及NGX_EVENT_MODULE模块并执行相关配置注册的相关函数
    rv = ngx_conf_parse(cf, NULL);
    //恢复配置结构
    *cf = pcf;

    if (rv != NGX_CONF_OK) {
        return rv;
    }
    /* 初始化 调用其他模块的init_conf 方法*/
    //执行NGX_EVENT_MODULE类型的模块注册的init_conf函数完成模块conf的初始化
    //在event_core_module模块的init_conf阶段，即ngx_event_core_init_conf函数中完成了IO复用模型的选择
    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = cf->cycle->modules[i]->ctx;

        if (m->init_conf) {
            rv = m->init_conf(cf->cycle,
                              (*ctx)[cf->cycle->modules[i]->ctx_index]);   //ngx_event_core_module->ngx_event_core_module_ctx->ngx_event_core_init_conf()
                                                                           //                                                 |--epoll_create()  根据平台初始化epoll
                                                                           //ngx_epoll_module->ngx_epoll_module_ctx->ngx_epoll_init_conf()
            if (rv != NGX_CONF_OK) {
                return rv;
            }
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

    ngx_str_t  *value;

    if (ecf->connections != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    value = cf->args->elts;
    ecf->connections = ngx_atoi(value[1].data, value[1].len);
    if (ecf->connections == (ngx_uint_t) NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number \"%V\"", &value[1]);

        return NGX_CONF_ERROR;
    }

    cf->cycle->connection_n = ecf->connections;

    return NGX_CONF_OK;
}


static char *
ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

    ngx_int_t             m;
    ngx_str_t            *value;
    ngx_event_conf_t     *old_ecf;
    ngx_event_module_t   *module;

    if (ecf->use != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (cf->cycle->old_cycle->conf_ctx) {
        old_ecf = ngx_event_get_conf(cf->cycle->old_cycle->conf_ctx,
                                     ngx_event_core_module);
    } else {
        old_ecf = NULL;
    }


    for (m = 0; cf->cycle->modules[m]; m++) {
        if (cf->cycle->modules[m]->type != NGX_EVENT_MODULE) {
            continue;
        }

        module = cf->cycle->modules[m]->ctx;
        if (module->name->len == value[1].len) {
            if (ngx_strcmp(module->name->data, value[1].data) == 0) {
                ecf->use = cf->cycle->modules[m]->ctx_index;
                ecf->name = module->name->data;

                if (ngx_process == NGX_PROCESS_SINGLE
                    && old_ecf
                    && old_ecf->use != ecf->use)
                {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "when the server runs without a master process "
                               "the \"%V\" event type must be the same as "
                               "in previous configuration - \"%s\" "
                               "and it cannot be changed on the fly, "
                               "to change it you need to stop server "
                               "and start it again",
                               &value[1], old_ecf->name);

                    return NGX_CONF_ERROR;
                }

                return NGX_CONF_OK;
            }
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid event type \"%V\"", &value[1]);

    return NGX_CONF_ERROR;
}


static char *
ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
#if (NGX_DEBUG)
    ngx_event_conf_t  *ecf = conf;

    ngx_int_t             rc;
    ngx_str_t            *value;
    ngx_url_t             u;
    ngx_cidr_t            c, *cidr;
    ngx_uint_t            i;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    value = cf->args->elts;

#if (NGX_HAVE_UNIX_DOMAIN)

    if (ngx_strcmp(value[1].data, "unix:") == 0) {
        cidr = ngx_array_push(&ecf->debug_connection);
        if (cidr == NULL) {
            return NGX_CONF_ERROR;
        }

        cidr->family = AF_UNIX;
        return NGX_CONF_OK;
    }

#endif

    rc = ngx_ptocidr(&value[1], &c);

    if (rc != NGX_ERROR) {
        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "low address bits of %V are meaningless",
                               &value[1]);
        }

        cidr = ngx_array_push(&ecf->debug_connection);
        if (cidr == NULL) {
            return NGX_CONF_ERROR;
        }

        *cidr = c;

        return NGX_CONF_OK;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));
    u.host = value[1];

    if (ngx_inet_resolve_host(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "%s in debug_connection \"%V\"",
                               u.err, &u.host);
        }

        return NGX_CONF_ERROR;
    }

    cidr = ngx_array_push_n(&ecf->debug_connection, u.naddrs);
    if (cidr == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(cidr, u.naddrs * sizeof(ngx_cidr_t));

    for (i = 0; i < u.naddrs; i++) {
        cidr[i].family = u.addrs[i].sockaddr->sa_family;

        switch (cidr[i].family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) u.addrs[i].sockaddr;
            cidr[i].u.in6.addr = sin6->sin6_addr;
            ngx_memset(cidr[i].u.in6.mask.s6_addr, 0xff, 16);
            break;
#endif

        default: /* AF_INET */
            sin = (struct sockaddr_in *) u.addrs[i].sockaddr;
            cidr[i].u.in.addr = sin->sin_addr.s_addr;
            cidr[i].u.in.mask = 0xffffffff;
            break;
        }
    }

#else

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "\"debug_connection\" is ignored, you need to rebuild "
                       "nginx using --with-debug option to enable it");

#endif

    return NGX_CONF_OK;
}


static void *
ngx_event_core_create_conf(ngx_cycle_t *cycle)
{
    ngx_event_conf_t  *ecf;   //event的相关配置保存的数据

    ecf = ngx_palloc(cycle->pool, sizeof(ngx_event_conf_t));
    if (ecf == NULL) {
        return NULL;
    }

    //默认初始化
    ecf->connections = NGX_CONF_UNSET_UINT;
    ecf->use = NGX_CONF_UNSET_UINT;
    ecf->multi_accept = NGX_CONF_UNSET;
    ecf->accept_mutex = NGX_CONF_UNSET;
    ecf->accept_mutex_delay = NGX_CONF_UNSET_MSEC;
    ecf->name = (void *) NGX_CONF_UNSET;

#if (NGX_DEBUG)

    if (ngx_array_init(&ecf->debug_connection, cycle->pool, 4,
                       sizeof(ngx_cidr_t)) == NGX_ERROR)
    {
        return NULL;
    }

#endif

    return ecf;
}


static char *
ngx_event_core_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

#if (NGX_HAVE_EPOLL) && !(NGX_TEST_BUILD_EPOLL)
    int                  fd;
#endif
    ngx_int_t            i;
    ngx_module_t        *module;
    ngx_event_module_t  *event_module;

    module = NULL;
/* Nginx根据当前系统平台选择一个合适的事件处理模块 */
#if (NGX_HAVE_EPOLL) && !(NGX_TEST_BUILD_EPOLL)

    fd = epoll_create(100);

    if (fd != -1) {
        (void) close(fd);
        module = &ngx_epoll_module;

    } else if (ngx_errno != NGX_ENOSYS) {
        module = &ngx_epoll_module;
    }

#endif

#if (NGX_HAVE_DEVPOLL) && !(NGX_TEST_BUILD_DEVPOLL)

    module = &ngx_devpoll_module;

#endif

#if (NGX_HAVE_KQUEUE)

    module = &ngx_kqueue_module;

#endif

#if (NGX_HAVE_SELECT)

    if (module == NULL) {
        module = &ngx_select_module;
    }

#endif

    if (module == NULL) {
        for (i = 0; cycle->modules[i]; i++) {

            if (cycle->modules[i]->type != NGX_EVENT_MODULE) {
                continue;
            }

            event_module = cycle->modules[i]->ctx;

            if (ngx_strcmp(event_module->name->data, event_core_name.data) == 0)
            {
                continue;
            }

            module = cycle->modules[i];
            break;
        }
    }

    if (module == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0, "no events module found");
        return NGX_CONF_ERROR;
    }
    /* 把该事件处理模块序号记录在配置变量ecf->use中 */
    ngx_conf_init_uint_value(ecf->connections, DEFAULT_CONNECTIONS);
    cycle->connection_n = ecf->connections;

    ngx_conf_init_uint_value(ecf->use, module->ctx_index);

    event_module = module->ctx;
    ngx_conf_init_ptr_value(ecf->name, event_module->name->data);

    ngx_conf_init_value(ecf->multi_accept, 0);
    ngx_conf_init_value(ecf->accept_mutex, 0);
    ngx_conf_init_msec_value(ecf->accept_mutex_delay, 500);

    return NGX_CONF_OK;
}
