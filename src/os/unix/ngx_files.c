
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


#if (NGX_THREADS)
#include <ngx_thread_pool.h>
static void ngx_thread_read_handler(void *data, ngx_log_t *log);
static void ngx_thread_write_chain_to_file_handler(void *data, ngx_log_t *log);
#endif

static ngx_chain_t *ngx_chain_to_iovec(ngx_iovec_t *vec, ngx_chain_t *cl);
static ssize_t ngx_writev_file(ngx_file_t *file, ngx_iovec_t *vec,
    off_t offset);


#if (NGX_HAVE_FILE_AIO)

ngx_uint_t  ngx_file_aio = 1;

#endif


ssize_t
ngx_read_file(ngx_file_t *file, u_char *buf, size_t size, off_t offset)
{
    ssize_t  n;

    ngx_log_debug4(NGX_LOG_DEBUG_CORE, file->log, 0,
                   "read: %d, %p, %uz, %O", file->fd, buf, size, offset);

#if (NGX_HAVE_PREAD)

    n = pread(file->fd, buf, size, offset);

    if (n == -1) {
        ngx_log_error(NGX_LOG_CRIT, file->log, ngx_errno,
                      "pread() \"%s\" failed", file->name.data);
        return NGX_ERROR;
    }

#else

    if (file->sys_offset != offset) {
        if (lseek(file->fd, offset, SEEK_SET) == -1) {
            ngx_log_error(NGX_LOG_CRIT, file->log, ngx_errno,
                          "lseek() \"%s\" failed", file->name.data);
            return NGX_ERROR;
        }

        file->sys_offset = offset;
    }

    n = read(file->fd, buf, size);

    if (n == -1) {
        ngx_log_error(NGX_LOG_CRIT, file->log, ngx_errno,
                      "read() \"%s\" failed", file->name.data);
        return NGX_ERROR;
    }

    file->sys_offset += n;

#endif

    file->offset += n;

    return n;
}


#if (NGX_THREADS)

typedef struct {
    ngx_fd_t       fd;
    ngx_uint_t     write;   /* unsigned  write:1; */

    u_char        *buf;
    size_t         size;
    ngx_chain_t   *chain;
    off_t          offset;

    size_t         nbytes;
    ngx_err_t      err;
} ngx_thread_file_ctx_t;


ssize_t
ngx_thread_read(ngx_file_t *file, u_char *buf, size_t size, off_t offset,
    ngx_pool_t *pool)
{
    ngx_thread_task_t      *task;
    ngx_thread_file_ctx_t  *ctx;

    ngx_log_debug4(NGX_LOG_DEBUG_CORE, file->log, 0,
                   "thread read: %d, %p, %uz, %O",
                   file->fd, buf, size, offset);

    task = file->thread_task;

    if (task == NULL) {
        task = ngx_thread_task_alloc(pool, sizeof(ngx_thread_file_ctx_t));
        if (task == NULL) {
            return NGX_ERROR;
        }

        file->thread_task = task;
    }

    ctx = task->ctx;

    if (task->event.complete) {
        task->event.complete = 0;

        if (ctx->write) {
            ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                          "invalid thread call, read instead of write");
            return NGX_ERROR;
        }

        if (ctx->err) {
            ngx_log_error(NGX_LOG_CRIT, file->log, ctx->err,
                          "pread() \"%s\" failed", file->name.data);
            return NGX_ERROR;
        }

        return ctx->nbytes;
    }

    task->handler = ngx_thread_read_handler;

    ctx->write = 0;

    ctx->fd = file->fd;
    ctx->buf = buf;
    ctx->size = size;
    ctx->offset = offset;

    if (file->thread_handler(task, file) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_AGAIN;
}


#if (NGX_HAVE_PREAD)

static void
ngx_thread_read_handler(void *data, ngx_log_t *log)
{
    ngx_thread_file_ctx_t *ctx = data;

    ssize_t  n;

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, log, 0, "thread read handler");

    n = pread(ctx->fd, ctx->buf, ctx->size, ctx->offset);

    if (n == -1) {
        ctx->err = ngx_errno;

    } else {
        ctx->nbytes = n;
        ctx->err = 0;
    }

#if 0
    ngx_time_update();
#endif

    ngx_log_debug4(NGX_LOG_DEBUG_CORE, log, 0,
                   "pread: %z (err: %d) of %uz @%O",
                   n, ctx->err, ctx->size, ctx->offset);
}

#else

#error pread() is required!

#endif

#endif /* NGX_THREADS */


ssize_t
ngx_write_file(ngx_file_t *file, u_char *buf, size_t size, off_t offset)
{
    ssize_t    n, written;
    ngx_err_t  err;

    ngx_log_debug4(NGX_LOG_DEBUG_CORE, file->log, 0,
                   "write: %d, %p, %uz, %O", file->fd, buf, size, offset);

    written = 0;

#if (NGX_HAVE_PWRITE)

    for ( ;; ) {
        n = pwrite(file->fd, buf + written, size, offset);

        if (n == -1) {
            err = ngx_errno;

            if (err == NGX_EINTR) {
                ngx_log_debug0(NGX_LOG_DEBUG_CORE, file->log, err,
                               "pwrite() was interrupted");
                continue;
            }

            ngx_log_error(NGX_LOG_CRIT, file->log, err,
                          "pwrite() \"%s\" failed", file->name.data);
            return NGX_ERROR;
        }

        file->offset += n;
        written += n;

        if ((size_t) n == size) {
            return written;
        }

        offset += n;
        size -= n;
    }

#else

    if (file->sys_offset != offset) {
        if (lseek(file->fd, offset, SEEK_SET) == -1) {
            ngx_log_error(NGX_LOG_CRIT, file->log, ngx_errno,
                          "lseek() \"%s\" failed", file->name.data);
            return NGX_ERROR;
        }

        file->sys_offset = offset;
    }

    for ( ;; ) {
        n = write(file->fd, buf + written, size);

        if (n == -1) {
            err = ngx_errno;

            if (err == NGX_EINTR) {
                ngx_log_debug0(NGX_LOG_DEBUG_CORE, file->log, err,
                               "write() was interrupted");
                continue;
            }

            ngx_log_error(NGX_LOG_CRIT, file->log, err,
                          "write() \"%s\" failed", file->name.data);
            return NGX_ERROR;
        }

        file->sys_offset += n;
        file->offset += n;
        written += n;

        if ((size_t) n == size) {
            return written;
        }

        size -= n;
    }
#endif
}


ngx_fd_t
ngx_open_tempfile(u_char *name, ngx_uint_t persistent, ngx_uint_t access)
{
    ngx_fd_t  fd;

    fd = open((const char *) name, O_CREAT|O_EXCL|O_RDWR,
              access ? access : 0600);

    if (fd != -1 && !persistent) {
        (void) unlink((const char *) name);
    }

    return fd;
}


ssize_t
ngx_write_chain_to_file(ngx_file_t *file, ngx_chain_t *cl, off_t offset,
    ngx_pool_t *pool)
{
    ssize_t        total, n;
    ngx_iovec_t    vec;
    struct iovec   iovs[NGX_IOVS_PREALLOCATE];

    /* use pwrite() if there is the only buf in a chain */

    if (cl->next == NULL) {
        return ngx_write_file(file, cl->buf->pos,
                              (size_t) (cl->buf->last - cl->buf->pos),
                              offset);
    }

    total = 0;

    vec.iovs = iovs;
    vec.nalloc = NGX_IOVS_PREALLOCATE;

    do {
        /* create the iovec and coalesce the neighbouring bufs */
        cl = ngx_chain_to_iovec(&vec, cl);

        /* use pwrite() if there is the only iovec buffer */

        if (vec.count == 1) {
            n = ngx_write_file(file, (u_char *) iovs[0].iov_base,
                               iovs[0].iov_len, offset);

            if (n == NGX_ERROR) {
                return n;
            }

            return total + n;
        }

        n = ngx_writev_file(file, &vec, offset);

        if (n == NGX_ERROR) {
            return n;
        }

        offset += n;
        total += n;

    } while (cl);

    return total;
}


static ngx_chain_t *
ngx_chain_to_iovec(ngx_iovec_t *vec, ngx_chain_t *cl)
{
    size_t         total, size;
    u_char        *prev;
    ngx_uint_t     n;
    struct iovec  *iov;

    iov = NULL;
    prev = NULL;
    total = 0;
    n = 0;

    for ( /* void */ ; cl; cl = cl->next) {

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        size = cl->buf->last - cl->buf->pos;

        if (prev == cl->buf->pos) {
            iov->iov_len += size;

        } else {
            if (n == vec->nalloc) {
                break;
            }

            iov = &vec->iovs[n++];

            iov->iov_base = (void *) cl->buf->pos;
            iov->iov_len = size;
        }

        prev = cl->buf->pos + size;
        total += size;
    }

    vec->count = n;
    vec->size = total;

    return cl;
}


static ssize_t
ngx_writev_file(ngx_file_t *file, ngx_iovec_t *vec, off_t offset)
{
    ssize_t    n;
    ngx_err_t  err;

    ngx_log_debug3(NGX_LOG_DEBUG_CORE, file->log, 0,
                   "writev: %d, %uz, %O", file->fd, vec->size, offset);

#if (NGX_HAVE_PWRITEV)

eintr:

    n = pwritev(file->fd, vec->iovs, vec->count, offset);

    if (n == -1) {
        err = ngx_errno;

        if (err == NGX_EINTR) {
            ngx_log_debug0(NGX_LOG_DEBUG_CORE, file->log, err,
                           "pwritev() was interrupted");
            goto eintr;
        }

        ngx_log_error(NGX_LOG_CRIT, file->log, err,
                      "pwritev() \"%s\" failed", file->name.data);
        return NGX_ERROR;
    }

    if ((size_t) n != vec->size) {
        ngx_log_error(NGX_LOG_CRIT, file->log, 0,
                      "pwritev() \"%s\" has written only %z of %uz",
                      file->name.data, n, vec->size);
        return NGX_ERROR;
    }

#else

    if (file->sys_offset != offset) {
        if (lseek(file->fd, offset, SEEK_SET) == -1) {
            ngx_log_error(NGX_LOG_CRIT, file->log, ngx_errno,
                          "lseek() \"%s\" failed", file->name.data);
            return NGX_ERROR;
        }

        file->sys_offset = offset;
    }

eintr:

    n = writev(file->fd, vec->iovs, vec->count);

    if (n == -1) {
        err = ngx_errno;

        if (err == NGX_EINTR) {
            ngx_log_debug0(NGX_LOG_DEBUG_CORE, file->log, err,
                           "writev() was interrupted");
            goto eintr;
        }

        ngx_log_error(NGX_LOG_CRIT, file->log, err,
                      "writev() \"%s\" failed", file->name.data);
        return NGX_ERROR;
    }

    if ((size_t) n != vec->size) {
        ngx_log_error(NGX_LOG_CRIT, file->log, 0,
                      "writev() \"%s\" has written only %z of %uz",
                      file->name.data, n, vec->size);
        return NGX_ERROR;
    }

    file->sys_offset += n;

#endif

    file->offset += n;

    return n;
}


#if (NGX_THREADS)

ssize_t
ngx_thread_write_chain_to_file(ngx_file_t *file, ngx_chain_t *cl, off_t offset,
    ngx_pool_t *pool)
{
    ngx_thread_task_t      *task;
    ngx_thread_file_ctx_t  *ctx;

    ngx_log_debug3(NGX_LOG_DEBUG_CORE, file->log, 0,
                   "thread write chain: %d, %p, %O",
                   file->fd, cl, offset);

    task = file->thread_task;

    if (task == NULL) {
        task = ngx_thread_task_alloc(pool,
                                     sizeof(ngx_thread_file_ctx_t));
        if (task == NULL) {
            return NGX_ERROR;
        }

        file->thread_task = task;
    }

    ctx = task->ctx;

    if (task->event.complete) {
        task->event.complete = 0;

        if (!ctx->write) {
            ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                          "invalid thread call, write instead of read");
            return NGX_ERROR;
        }

        if (ctx->err || ctx->nbytes == 0) {
            ngx_log_error(NGX_LOG_CRIT, file->log, ctx->err,
                          "pwritev() \"%s\" failed", file->name.data);
            return NGX_ERROR;
        }

        file->offset += ctx->nbytes;
        return ctx->nbytes;
    }

    task->handler = ngx_thread_write_chain_to_file_handler;

    ctx->write = 1;

    ctx->fd = file->fd;
    ctx->chain = cl;
    ctx->offset = offset;

    if (file->thread_handler(task, file) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_AGAIN;
}


static void
ngx_thread_write_chain_to_file_handler(void *data, ngx_log_t *log)
{
    ngx_thread_file_ctx_t *ctx = data;

#if (NGX_HAVE_PWRITEV)

    off_t          offset;
    ssize_t        n;
    ngx_err_t      err;
    ngx_chain_t   *cl;
    ngx_iovec_t    vec;
    struct iovec   iovs[NGX_IOVS_PREALLOCATE];

    vec.iovs = iovs;
    vec.nalloc = NGX_IOVS_PREALLOCATE;

    cl = ctx->chain;
    offset = ctx->offset;

    ctx->nbytes = 0;
    ctx->err = 0;

    do {
        /* create the iovec and coalesce the neighbouring bufs */
        cl = ngx_chain_to_iovec(&vec, cl);

eintr:

        n = pwritev(ctx->fd, iovs, vec.count, offset);

        if (n == -1) {
            err = ngx_errno;

            if (err == NGX_EINTR) {
                ngx_log_debug0(NGX_LOG_DEBUG_CORE, log, err,
                               "pwritev() was interrupted");
                goto eintr;
            }

            ctx->err = err;
            return;
        }

        if ((size_t) n != vec.size) {
            ctx->nbytes = 0;
            return;
        }

        ctx->nbytes += n;
        offset += n;
    } while (cl);

#else

    ctx->err = NGX_ENOSYS;
    return;

#endif
}

#endif /* NGX_THREADS */


ngx_int_t
ngx_set_file_time(u_char *name, ngx_fd_t fd, time_t s)
{
    struct timeval  tv[2];

    tv[0].tv_sec = ngx_time();
    tv[0].tv_usec = 0;
    tv[1].tv_sec = s;
    tv[1].tv_usec = 0;

    if (utimes((char *) name, tv) != -1) {
        return NGX_OK;
    }

    return NGX_ERROR;
}


ngx_int_t
ngx_create_file_mapping(ngx_file_mapping_t *fm)
{
    fm->fd = ngx_open_file(fm->name, NGX_FILE_RDWR, NGX_FILE_TRUNCATE,
                           NGX_FILE_DEFAULT_ACCESS);

    if (fm->fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_CRIT, fm->log, ngx_errno,
                      ngx_open_file_n " \"%s\" failed", fm->name);
        return NGX_ERROR;
    }

    if (ftruncate(fm->fd, fm->size) == -1) {
        ngx_log_error(NGX_LOG_CRIT, fm->log, ngx_errno,
                      "ftruncate() \"%s\" failed", fm->name);
        goto failed;
    }

    fm->addr = mmap(NULL, fm->size, PROT_READ|PROT_WRITE, MAP_SHARED,
                    fm->fd, 0);
    if (fm->addr != MAP_FAILED) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_CRIT, fm->log, ngx_errno,
                  "mmap(%uz) \"%s\" failed", fm->size, fm->name);

failed:

    if (ngx_close_file(fm->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, fm->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", fm->name);
    }

    return NGX_ERROR;
}


void
ngx_close_file_mapping(ngx_file_mapping_t *fm)
{
    if (munmap(fm->addr, fm->size) == -1) {
        ngx_log_error(NGX_LOG_CRIT, fm->log, ngx_errno,
                      "munmap(%uz) \"%s\" failed", fm->size, fm->name);
    }

    if (ngx_close_file(fm->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, fm->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", fm->name);
    }
}


ngx_int_t
ngx_open_dir(ngx_str_t *name, ngx_dir_t *dir)
{
    dir->dir = opendir((const char *) name->data);

    if (dir->dir == NULL) {
        return NGX_ERROR;
    }

    dir->valid_info = 0;

    return NGX_OK;
}


ngx_int_t
ngx_read_dir(ngx_dir_t *dir)
{
    dir->de = readdir(dir->dir);

    if (dir->de) {
#if (NGX_HAVE_D_TYPE)
        dir->type = dir->de->d_type;
#else
        dir->type = 0;
#endif
        return NGX_OK;
    }

    return NGX_ERROR;
}


ngx_int_t
ngx_open_glob(ngx_glob_t *gl)
{
    int  n;

    n = glob((char *) gl->pattern, 0, NULL, &gl->pglob);

    if (n == 0) {
        return NGX_OK;
    }

#ifdef GLOB_NOMATCH

    if (n == GLOB_NOMATCH && gl->test) {
        return NGX_OK;
    }

#endif

    return NGX_ERROR;
}


ngx_int_t
ngx_read_glob(ngx_glob_t *gl, ngx_str_t *name)
{
    size_t  count;

#ifdef GLOB_NOMATCH
    count = (size_t) gl->pglob.gl_pathc;
#else
    count = (size_t) gl->pglob.gl_matchc;
#endif

    if (gl->n < count) {

        name->len = (size_t) ngx_strlen(gl->pglob.gl_pathv[gl->n]);
        name->data = (u_char *) gl->pglob.gl_pathv[gl->n];
        gl->n++;

        return NGX_OK;
    }

    return NGX_DONE;
}


void
ngx_close_glob(ngx_glob_t *gl)
{
    globfree(&gl->pglob);
}

/**
 * @brief 尝试获取文件锁
 * @details 获取文件锁时不会阻塞进程, 获取不到锁时，立即返回不会等待
 * @param fd 文件描述符
 * @return 是否成功获取文件锁
 *   @retval TRUE 获取锁成功
 *   @retval FALSE 获取锁失败
 * @attention 这里只是建议性锁，每个使用上锁文件的进程都要检查是否有锁存在，
 * 内核不对读写操作做内部检查和强制保护
 * 注释来源：https://www.jianshu.com/p/5833e761db29
 */
ngx_err_t
ngx_trylock_fd(ngx_fd_t fd)
{
    /*
    struct flock {  
     short l_type;   //锁类型F_RDLCK 读锁, F_WRLCK写锁, or F_UNLCK解锁   
     off_t l_start;  //offset in bytes, relative to l_whence 偏移的字节  
     short l_whence; //SEEK_SET, SEEK_CUR, or SEEK_END    加锁的起始位置  
     off_t l_len;  //length, in bytes; 0 means lock to EOF  长度  
     pid_t l_pid;  //returned with F_GETLK   进程ip
     };
    */   
    
    struct flock  fl;

    ngx_memzero(&fl, sizeof(struct flock));
    fl.l_type = F_WRLCK;     // 建立一个供写入用的锁定
    fl.l_whence = SEEK_SET;  //以文件开头为锁定的起始位置
    //函数原型：int fcntl(int fd, int cmd, struct flock *lock);
    //cmd的可选值如下：
    //F_GETLK   获取文件锁的状态
    //F_SETLK   设置文件锁定的状态。此时flcok 结构的l_type 值必须是F_RDLCK、F_WRLCK或F_UNLCK。如果无法建立锁定，则返回-1，错误代码为EACCES 或EAGAIN
    //F_SETLKW  同F_SETLK 作用相同，但是无法建立锁定时，此调用会一直等到锁定动作成功为止。若在等待锁定的过程中被信号中断时，会立即返回-1，错误代码为EINTR
    if (fcntl(fd, F_SETLK, &fl) == -1) {
        return ngx_errno;
    }

    return 0;
}

/**
 * @brief 获取锁或等待
 * @details 获取文件锁时会阻塞进程, 获取不到锁时，一直等到获取成功
 * @param fd 文件描述符
 * @return 是否成功获取文件锁
 *   @retval TRUE 获取锁成功
 *   @retval FALSE 获取锁失败
 * @attention 这里只是建议性锁，每个使用上锁文件的进程都要检查是否有锁存在，
 * 内核不对读写操作做内部检查和强制保护
 * 注释来源：https://www.jianshu.com/p/5833e761db29
 */
ngx_err_t
ngx_lock_fd(ngx_fd_t fd)
{
    struct flock  fl;

    ngx_memzero(&fl, sizeof(struct flock));
    fl.l_type = F_WRLCK;    // 建立一个供写入用的锁定
    fl.l_whence = SEEK_SET; //以文件开头为锁定的起始位置
    //函数原型：int fcntl(int fd, int cmd, struct flock *lock);
    //F_SETLKW 设置记录锁
    //F_SETLKW   除了共享锁或独占锁被其他的锁阻塞这种情况外，这个命令和F_SETLK是一样的。如果共享锁或独占锁被其他的锁阻塞，进程将等待直到这个请求能够完成。
    //当fcntl()正在等待文件的某个区域的时候捕捉到一个信号，如果这个信号没有被指定SA_RESTART, fcntl将被中断
    if (fcntl(fd, F_SETLKW, &fl) == -1) {
        return ngx_errno;
    }

    return 0;
}

/**
 * @brief 释放文件锁
 * @param fd 文件描述符
 * @return 是否成功释放文件锁
 *   @retval TRUE 释放锁成功
 *   @retval FALSE 释放锁失败
 * 注释来源：https://www.jianshu.com/p/5833e761db29
 */
ngx_err_t
ngx_unlock_fd(ngx_fd_t fd)
{
    struct flock  fl;

    ngx_memzero(&fl, sizeof(struct flock));
    fl.l_type = F_UNLCK;      //删除之前建立的锁定
    fl.l_whence = SEEK_SET;   ////以文件开头为锁定的起始位置
    //函数原型：int fcntl(int fd, int cmd, struct flock *lock);
    //F_SETLKW 设置记录锁
    if (fcntl(fd, F_SETLK, &fl) == -1) {
        return  ngx_errno;
    }

    return 0;
}


#if (NGX_HAVE_POSIX_FADVISE) && !(NGX_HAVE_F_READAHEAD)

ngx_int_t
ngx_read_ahead(ngx_fd_t fd, size_t n)
{
    int  err;

    err = posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    if (err == 0) {
        return 0;
    }

    ngx_set_errno(err);
    return NGX_FILE_ERROR;
}

#endif


#if (NGX_HAVE_O_DIRECT)

ngx_int_t
ngx_directio_on(ngx_fd_t fd)
{
    int  flags;

    flags = fcntl(fd, F_GETFL);

    if (flags == -1) {
        return NGX_FILE_ERROR;
    }

    return fcntl(fd, F_SETFL, flags | O_DIRECT);
}


ngx_int_t
ngx_directio_off(ngx_fd_t fd)
{
    int  flags;

    flags = fcntl(fd, F_GETFL);

    if (flags == -1) {
        return NGX_FILE_ERROR;
    }

    return fcntl(fd, F_SETFL, flags & ~O_DIRECT);
}

#endif


#if (NGX_HAVE_STATFS)

size_t
ngx_fs_bsize(u_char *name)
{
    struct statfs  fs;

    if (statfs((char *) name, &fs) == -1) {
        return 512;
    }

    if ((fs.f_bsize % 512) != 0) {
        return 512;
    }

    return (size_t) fs.f_bsize;
}

#elif (NGX_HAVE_STATVFS)

size_t
ngx_fs_bsize(u_char *name)
{
    struct statvfs  fs;

    if (statvfs((char *) name, &fs) == -1) {
        return 512;
    }

    if ((fs.f_frsize % 512) != 0) {
        return 512;
    }

    return (size_t) fs.f_frsize;
}

#else

size_t
ngx_fs_bsize(u_char *name)
{
    return 512;
}

#endif
