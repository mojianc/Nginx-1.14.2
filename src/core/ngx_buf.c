
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>

/**
 * 创建一个缓冲区。需要传入pool和buf的大小
 */
ngx_buf_t *
ngx_create_temp_buf(ngx_pool_t *pool, size_t size)
{
    ngx_buf_t *b;
    /* 最终调用的是内存池pool，开辟一段内存用作缓冲区，主要放置ngx_buf_t结构体 */
    b = ngx_calloc_buf(pool);
    if (b == NULL) {
        return NULL;
    }
    /* 分配缓冲区内存;  pool为内存池，size为buf的大小*/
    b->start = ngx_palloc(pool, size);
    if (b->start == NULL) {
        return NULL;
    }

    /*
     * set by ngx_calloc_buf():
     *
     *     b->file_pos = 0;
     *     b->file_last = 0;
     *     b->file = NULL;
     *     b->shadow = NULL;
     *     b->tag = 0;
     *     and flags
     */

    b->pos = b->start;         //待处理数据的标记指针
    b->last = b->start;        //待处理数据的结尾标记指针
    b->end = b->last + size;   //缓冲区结尾地址
    b->temporary = 1;

    return b;
}


/**
 * 创建一个缓冲区的链表结构
**/
ngx_chain_t *
ngx_alloc_chain_link(ngx_pool_t *pool)
{
    ngx_chain_t  *cl;

    cl = pool->chain;
    /*
	 * 首先从内存池中去取ngx_chain_t，
	 * 被清空的ngx_chain_t结构都会放在pool->chain 缓冲链上
     */
    if (cl) {
        pool->chain = cl->next;
        return cl;
    }

    /* 如果取不到，则从内存池pool上分配一个数据结构  */
    cl = ngx_palloc(pool, sizeof(ngx_chain_t));
    if (cl == NULL) {
        return NULL;
    }

    return cl;
}


/**
 * 批量创建多个buf，并且用链表串起来
 **/
ngx_chain_t *
ngx_create_chain_of_bufs(ngx_pool_t *pool, ngx_bufs_t *bufs)
{
    u_char       *p;
    ngx_int_t     i;
    ngx_buf_t    *b;
    ngx_chain_t  *chain, *cl, **ll;
    /* 在内存池pool上分配bufs->num个 buf缓冲区 ，每个大小为bufs->size */
    p = ngx_palloc(pool, bufs->num * bufs->size);
    if (p == NULL) {
        return NULL;
    }

    ll = &chain;
    /* 循环创建BUF，并且将ngx_buf_t挂载到ngx_chain_t链表上，并且返回链表*/
    for (i = 0; i < bufs->num; i++) {
        /* 最终调用的是内存池pool，开辟一段内存用作缓冲区，主要放置ngx_buf_t结构体 */
        b = ngx_calloc_buf(pool);
        if (b == NULL) {
            return NULL;
        }

        /*
         * set by ngx_calloc_buf():
         *
         *     b->file_pos = 0;
         *     b->file_last = 0;
         *     b->file = NULL;
         *     b->shadow = NULL;
         *     b->tag = 0;
         *     and flags
         *
         */

        b->pos = p;
        b->last = p;
        b->temporary = 1;

        b->start = p;
        p += bufs->size; //p往前增
        b->end = p;
        /* 分配一个ngx_chain_t */
        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NULL;
        }
        /* 将buf，都挂载到ngx_chain_t链表上，最终返回ngx_chain_t链表 */
        cl->buf = b;
        *ll = cl;
        ll = &cl->next;
    }

    *ll = NULL;
    /* 最终得到一个分配了bufs->num的缓冲区链表  */
    return chain;
}


/**
 * 将其它缓冲区链表放到已有缓冲区链表结构的尾部
 **/
ngx_int_t
ngx_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain, ngx_chain_t *in)
{
    ngx_chain_t  *cl, **ll;

    ll = chain;    //chain 指向指针的指针，很绕
    /* 找到缓冲区链表结尾部分，cl->next== NULL；cl = *chain既为指针链表地址*/
    for (cl = *chain; cl; cl = cl->next) {
        ll = &cl->next;
    }
    /* 遍历in */
    while (in) {
        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = in->buf; //in上的buf拷贝到cl上面
        *ll = cl;          //并且放到chain链表上
        ll = &cl->next;    //链表往下走
        in = in->next;     //遍历，直到NULL
    }

    *ll = NULL;

    return NGX_OK;
}


/**
 * 冲空闲的buf链表上，获取一个未使用的buf链表
 */
ngx_chain_t *
ngx_chain_get_free_buf(ngx_pool_t *p, ngx_chain_t **free)
{
    ngx_chain_t  *cl;
    /* 空闲列表中有数据，则直接返回 */
    if (*free) {
        cl = *free;
        *free = cl->next;
        cl->next = NULL;
        return cl;
    }
    /* 否则分配一个新的buf */
    cl = ngx_alloc_chain_link(p);
    if (cl == NULL) {
        return NULL;
    }

    cl->buf = ngx_calloc_buf(p);
    if (cl->buf == NULL) {
        return NULL;
    }

    cl->next = NULL;

    return cl;
}

/**
 * 释放BUF
 * 1. 如果buf不为空，则不释放
 * 2. 如果cl->buf->tag标记不一样，则直接还给Nginx的pool->chain链表
 * 3. 如果buf为空，并且需要释放，则直接释放buf，并且放到free的空闲列表上
 * 总结：（1）清空out_bufs链表；
 * （2）把out_bufs中已经发送完的ngx_buf_t结构体清空重置（即把pos和last成员指向start），同时把它们追加到free_bufs链表中；
 * （3）如果out_bufs中还有未发送完的ngx_buf_t结构体，那么添加到busy_bufs链表中
 */
void
ngx_chain_update_chains(ngx_pool_t *p, ngx_chain_t **free, ngx_chain_t **busy,
    ngx_chain_t **out, ngx_buf_tag_t tag)
{
    ngx_chain_t  *cl;
    
    if (*out) {
        /* *busy 指向OUT，将已经输出的out放到busy链表上 */
        if (*busy == NULL) {
            *busy = *out;

        } else {
            for (cl = *busy; cl->next; cl = cl->next) { /* void */ }

            cl->next = *out;
        }

        *out = NULL;
    }
    /* 遍历 busy链表 */
    while (*busy) {
        cl = *busy;
        /* 如果buf不为空，则继续遍历 */
        if (ngx_buf_size(cl->buf) != 0) {
            break;
        }
        /* 如果标识一样，则释放这个BUF */
        if (cl->buf->tag != tag) {
            *busy = cl->next;
            ngx_free_chain(p, cl);
            continue;
        }
        /* 直接将buf使用的部分回归到 起点指针地址 */
        cl->buf->pos = cl->buf->start;
        cl->buf->last = cl->buf->start;

        *busy = cl->next;   //继续往后遍历
        /* 并且将cl放到free列表上 */
        cl->next = *free;
        *free = cl;
    }
}


off_t
ngx_chain_coalesce_file(ngx_chain_t **in, off_t limit)
{
    off_t         total, size, aligned, fprev;
    ngx_fd_t      fd;
    ngx_chain_t  *cl;

    total = 0;

    cl = *in;
    fd = cl->buf->file->fd;

    do {
        size = cl->buf->file_last - cl->buf->file_pos;

        if (size > limit - total) {
            size = limit - total;

            aligned = (cl->buf->file_pos + size + ngx_pagesize - 1)
                       & ~((off_t) ngx_pagesize - 1);

            if (aligned <= cl->buf->file_last) {
                size = aligned - cl->buf->file_pos;
            }

            total += size;
            break;
        }

        total += size;
        fprev = cl->buf->file_pos + size;
        cl = cl->next;

    } while (cl
             && cl->buf->in_file
             && total < limit
             && fd == cl->buf->file->fd
             && fprev == cl->buf->file_pos);

    *in = cl;

    return total;
}


ngx_chain_t *
ngx_chain_update_sent(ngx_chain_t *in, off_t sent)
{
    off_t  size;

    for ( /* void */ ; in; in = in->next) {

        if (ngx_buf_special(in->buf)) {
            continue;
        }

        if (sent == 0) {
            break;
        }

        size = ngx_buf_size(in->buf);

        if (sent >= size) {
            sent -= size;

            if (ngx_buf_in_memory(in->buf)) {
                in->buf->pos = in->buf->last;
            }

            if (in->buf->in_file) {
                in->buf->file_pos = in->buf->file_last;
            }

            continue;
        }

        if (ngx_buf_in_memory(in->buf)) {
            in->buf->pos += (size_t) sent;
        }

        if (in->buf->in_file) {
            in->buf->file_pos += sent;
        }

        break;
    }

    return in;
}
