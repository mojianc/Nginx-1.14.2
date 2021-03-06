
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_CONFIG_H_INCLUDED_
#define _NGX_HTTP_CONFIG_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    /**
     * 指向一个指针数组，数组中的每个成员都是由所有http模块的create_main_conf方法创建的存放全局配置项的结构体，
     * 他们存放着配置直属http{}块内的main级别的配置项参数
    */
    void        **main_conf;
    /**
     * 指向一个指针数组，数组中的每个成员都是由所有http模块的create_srv_conf方法创建的与server相关的结构体，
     * 他们或存放main级别配置项，或存放srv级别配置项，这与当前的ngx_http_conf_ctx_t是在解析http{}或者server{}块时创建有关
    */
    void        **srv_conf;
    /**
     * 指向一个指针数组，数组中的每个成员都是由所有http模块的create_loc_conf方法创建的与location相关的结构体，他们可能存放着main、srv、loc级别的配置项，
     * 这与当前的ngx_http_conf_ctx_t是在解析http{},server{}或者location{}块时创建有关
    */
    void        **loc_conf;
} ngx_http_conf_ctx_t;


typedef struct {
    //解析配置http{...}文件前调用
    ngx_int_t   (*preconfiguration)(ngx_conf_t *cf);
    //完成配置文件http{...}的解析后调用
    ngx_int_t   (*postconfiguration)(ngx_conf_t *cf);
    //当需要创建数据结构用于存储main级别(直属于http{...}块的配置项)的全局配置项时，可以通过create_main_conf回调方法创建存储全局配置项的结构体
    void       *(*create_main_conf)(ngx_conf_t *cf);
    //常用于初始化main级别配置项
    char       *(*init_main_conf)(ngx_conf_t *cf, void *conf);
    //当需要创建数据结构用于存储srv级别(直属与虚拟主机server{...}块的配置项)的配置项时，可以通过实现create_srv_conf回调方法创建存储srv级别配置项的结构体
    void       *(*create_srv_conf)(ngx_conf_t *cf);
    //merge_srv_conf回调方法主要是用于合并main级别和srv级别下的同名配置项
    char       *(*merge_srv_conf)(ngx_conf_t *cf, void *prev, void *conf);
    //当需要创建数据结构用于存储loc级别(直属于location{...}块的配置项)的配置项时，可以实现create_loc_conf回调方法
    void       *(*create_loc_conf)(ngx_conf_t *cf);
    //merge_loc_conf回调方法主要是合并srv级别和loc级别下的同名配置项
    char       *(*merge_loc_conf)(ngx_conf_t *cf, void *prev, void *conf);
} ngx_http_module_t;


#define NGX_HTTP_MODULE           0x50545448   /* "HTTP" */

#define NGX_HTTP_MAIN_CONF        0x02000000
#define NGX_HTTP_SRV_CONF         0x04000000
#define NGX_HTTP_LOC_CONF         0x08000000
#define NGX_HTTP_UPS_CONF         0x10000000
#define NGX_HTTP_SIF_CONF         0x20000000
#define NGX_HTTP_LIF_CONF         0x40000000
#define NGX_HTTP_LMT_CONF         0x80000000


#define NGX_HTTP_MAIN_CONF_OFFSET  offsetof(ngx_http_conf_ctx_t, main_conf)
#define NGX_HTTP_SRV_CONF_OFFSET   offsetof(ngx_http_conf_ctx_t, srv_conf)
#define NGX_HTTP_LOC_CONF_OFFSET   offsetof(ngx_http_conf_ctx_t, loc_conf)


#define ngx_http_get_module_main_conf(r, module)                             \
    (r)->main_conf[module.ctx_index]
#define ngx_http_get_module_srv_conf(r, module)  (r)->srv_conf[module.ctx_index]
#define ngx_http_get_module_loc_conf(r, module)  (r)->loc_conf[module.ctx_index]


#define ngx_http_conf_get_module_main_conf(cf, module)                        \
    ((ngx_http_conf_ctx_t *) cf->ctx)->main_conf[module.ctx_index]
#define ngx_http_conf_get_module_srv_conf(cf, module)                         \
    ((ngx_http_conf_ctx_t *) cf->ctx)->srv_conf[module.ctx_index]
#define ngx_http_conf_get_module_loc_conf(cf, module)                         \
    ((ngx_http_conf_ctx_t *) cf->ctx)->loc_conf[module.ctx_index]

#define ngx_http_cycle_get_module_main_conf(cycle, module)                    \
    (cycle->conf_ctx[ngx_http_module.index] ?                                 \
        ((ngx_http_conf_ctx_t *) cycle->conf_ctx[ngx_http_module.index])      \
            ->main_conf[module.ctx_index]:                                    \
        NULL)


#endif /* _NGX_HTTP_CONFIG_H_INCLUDED_ */
