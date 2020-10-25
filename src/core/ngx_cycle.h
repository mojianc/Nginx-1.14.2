
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_CYCLE_H_INCLUDED_
#define _NGX_CYCLE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#ifndef NGX_CYCLE_POOL_SIZE
#define NGX_CYCLE_POOL_SIZE     NGX_DEFAULT_POOL_SIZE
#endif


#define NGX_DEBUG_POINTS_STOP   1
#define NGX_DEBUG_POINTS_ABORT  2


typedef struct ngx_shm_zone_s  ngx_shm_zone_t;

typedef ngx_int_t (*ngx_shm_zone_init_pt) (ngx_shm_zone_t *zone, void *data);

struct ngx_shm_zone_s {
    void                     *data;
    ngx_shm_t                 shm;
    ngx_shm_zone_init_pt      init;
    void                     *tag;
    void                     *sync;
    ngx_uint_t                noreuse;  /* unsigned  noreuse:1; */
};


/**
 * Nginx全局变量cycle
 */
struct ngx_cycle_s {
    void                  ****conf_ctx;         /* 配置文件 上下文的数组，每个模块的配置信息*/
    ngx_pool_t               *pool;             /* 内存池地址 */

    ngx_log_t                *log;              /* 日志 */
    ngx_log_t                 new_log;          

    ngx_uint_t                log_use_stderr;   /* unsigned  log_use_stderr:1; */

    ngx_connection_t        **files;             /* 连接文件句柄 */
    ngx_connection_t         *free_connections;  /* 空闲连接 */
    ngx_uint_t                free_connection_n; /* 空闲连接个数 */

    ngx_module_t            **modules;           /* 模块数组 */
    ngx_uint_t                modules_n;
    ngx_uint_t                modules_used;      /* unsigned  modules_used:1; */

    ngx_queue_t               reusable_connections_queue;
    ngx_uint_t                reusable_connections_n;

    ngx_array_t               listening;          /* 监听数组 */
    ngx_array_t               paths;              /* 路径数组 */

    ngx_array_t               config_dump;
    ngx_rbtree_t              config_dump_rbtree;
    ngx_rbtree_node_t         config_dump_sentinel;

    ngx_list_t                open_files;          /* 打开的文件 */
    ngx_list_t                shared_memory;       /* 共享内存链表*/

    ngx_uint_t                connection_n;        /* 连接的个数*/
    ngx_uint_t                files_n;             /* 打开文件的个数 */

    ngx_connection_t         *connections;         /* 连接事件*/
    ngx_event_t              *read_events;         /* 读取事件*/
    ngx_event_t              *write_events;        /* 写入事件*/

    ngx_cycle_t              *old_cycle;

    ngx_str_t                 conf_file;           /* 配置文件 */
    ngx_str_t                 conf_param;          /* 配置参数 */
    ngx_str_t                 conf_prefix;         /* 配置文件前缀*/
    ngx_str_t                 prefix;              /* 前缀*/
    ngx_str_t                 lock_file;           /* 锁文件*/
    ngx_str_t                 hostname;            /* 主机名称*/
};


/**
 * 核心配置文件信息
 * 对应nginx.conf的
 * #user  nobody;
	worker_processes  1;
	#error_log  logs/error.log;
	#error_log  logs/error.log  notice;
	#error_log  logs/error.log  info;
	#pid        logs/nginx.pid;
 */
typedef struct {
    ngx_flag_t                daemon;  //是否以守护进程方式开启nginx [on|off] 默认：deamon on
    ngx_flag_t                master;  //master_process[on|off] 是否以master/worker方式运行nginx，当值为off时就不会fork出worker进程。默认：mater_process on

    ngx_msec_t                timer_resolution; //timer_resolution [interval]   worker进程中gettimeofday函数调用的时间间隔，gettimeofday函数每interval调用一次，
                                                //这样降低了缓存时间的更新频率，默认没接收到一个kernel进程发送到用户态的事件就调用一次。
    ngx_msec_t                shutdown_timeout;

    ngx_int_t                 worker_processes; //worker_processes [number | auto] 设置worker进程的个数，值为auto时表示worker进程数等于当前的CPU个数。默认：work_process 1
    ngx_int_t                 debug_points; //debug_points [abort | stop] 该配置项用于调试，Abort表示当前进程出现异常时产生一个core dump文件，stop表示当前进程出现异常时停止当前进程。默认此配置项未生效。

    ngx_int_t                 rlimit_nofile; //worker_rlimit_nofile [number]  worker进程打开的最大文件描述符
    off_t                     rlimit_core;   //worker_rlimit_core [size]  worker进程生成的coredump文件的最大大小

    int                       priority; //worker_priority [number]  设置worker进程的优先级（参考nice命令），默认：worker_priority 1

    ngx_uint_t                cpu_affinity_auto;
    ngx_uint_t                cpu_affinity_n; //绑定cpu个数
    ngx_cpuset_t             *cpu_affinity;

    char                     *username; //对应user配置项中指定的用户名 [group] worker进程所属的用户及组，当group参数没有配置时，group等于user。默认：usernobody nobody
    ngx_uid_t                 user;  //保存用户ID
    ngx_gid_t                 group; //保存组ID

    ngx_str_t                 working_directory;//worker_directory [directory] 定义worker进程的工作目录
    ngx_str_t                 lock_file;  //设置文件锁的路径。 默认：lock_filelogs/nginx.lock

    ngx_str_t                 pid; //设置保存进程的ID的文件名。默认：pid nginx.pid
    ngx_str_t                 oldpid;

    ngx_array_t               env; //env variable[=value] 默认情况下，worker子进程会删除所有从父进程继承过来的环境变量（除TZ变量除外）。env配置项用于保存一些继承过来的环境变量而不被删除。默认： evn TZ
    char                    **environment;

    ngx_uint_t                transparent;  /* unsigned  transparent:1; */
} ngx_core_conf_t;


#define ngx_is_init_cycle(cycle)  (cycle->conf_ctx == NULL)


ngx_cycle_t *ngx_init_cycle(ngx_cycle_t *old_cycle);
ngx_int_t ngx_create_pidfile(ngx_str_t *name, ngx_log_t *log);
void ngx_delete_pidfile(ngx_cycle_t *cycle);
ngx_int_t ngx_signal_process(ngx_cycle_t *cycle, char *sig);
void ngx_reopen_files(ngx_cycle_t *cycle, ngx_uid_t user);
char **ngx_set_environment(ngx_cycle_t *cycle, ngx_uint_t *last);
ngx_pid_t ngx_exec_new_binary(ngx_cycle_t *cycle, char *const *argv);
ngx_cpuset_t *ngx_get_cpu_affinity(ngx_uint_t n);
ngx_shm_zone_t *ngx_shared_memory_add(ngx_conf_t *cf, ngx_str_t *name,
    size_t size, void *tag);
void ngx_set_shutdown_timer(ngx_cycle_t *cycle);


extern volatile ngx_cycle_t  *ngx_cycle;
extern ngx_array_t            ngx_old_cycles;
extern ngx_module_t           ngx_core_module;
extern ngx_uint_t             ngx_test_config;
extern ngx_uint_t             ngx_dump_config;
extern ngx_uint_t             ngx_quiet_mode;


#endif /* _NGX_CYCLE_H_INCLUDED_ */
