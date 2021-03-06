/**
 * @file libgrocket/gr_worker.c
 * @author zouyueming(da_ming at hotmail.com)
 * @date 2013/10/05
 * @version $Revision$ 
 * @brief   worker
 * Revision History
 *
 * @if  ID       Author       Date          Major Change       @endif
 *  ---------+------------+------------+------------------------------+
 *       1     zouyueming   2013-10-05    Created.
 *       2     zouyueming   2013-10-26    support disabled worker thread
 *       3     zouyueming   2013-10-30    optimize single connection performance
 *                                        add try_to_send_tcp_rsp function
 **/
/* 
 *
 * Copyright (C) 2013-now da_ming at hotmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// 别忘了worker的队列只能一个线程push

#include "gr_worker.h"
#include "gr_thread.h"
#include "gr_log.h"
#include "gr_global.h"
#include "gr_errno.h"
#include "gr_tools.h"
#include "gr_mem.h"
#include "gr_config.h"
#include "gr_module.h"
#include "gr_conn.h"
#include "gr_tcp_in.h"
#include "gr_tcp_out.h"
#include "gr_udp_out.h"
#include "gr_event.h"

        struct gr_worker_t;
typedef struct gr_worker_t      gr_worker_t;
        struct gr_worker_item_t;
typedef struct gr_worker_item_t gr_worker_item_t;

typedef struct
{
    // 二进制包处理上下文
    gr_proc_ctxt_t  bin_ctxt;
    // HTTP包处理上下文
    gr_proc_http_t  http_ctxt;
    
} per_thread_t;

struct gr_worker_t
{
    // Worker线程组
    gr_threads_t        threads;

    // 每个Worker对应的的数据结构
    gr_worker_item_t *  items;

    bool                worker_disabled;
    bool                tcp_out_disabled;
};

struct gr_worker_item_t
{
    // 待处理请求队列头，压入方写
    gr_queue_item_t *   head;
    // 待处理请求队列尾，压入方写
    gr_queue_item_t *   tail;
    // 待处理请求队列中，下一次即将处理的请求，处理方写，压入方重置
    gr_queue_item_t *   curr;

#ifdef GR_DEBUG_CONN
    // 不用事件方式等，等到事件次数
    unsigned long               non_event_wait_ok_count;
    // 不用事件方式等，没等到事件次数
    unsigned long               non_event_wait_timeout_count;
#endif

    // 实在没工作可做时，等待事件
    gr_event_t                  event;
    // 进入event等待次数, 今后优化性能时用
    unsigned long               event_wait_count;
    // 进入event等待，同时等到事件的次数
    unsigned long               event_wait_ok_count;
    // 进入event等待，但等待超时的次数
    unsigned long               event_wait_timeout_count;
    // event唤醒次数, 今后优化性能时用
    unsigned long               event_alarm_count;
    // 在等待事件时，本值为true；没等待事件时，本值为false。
    volatile bool               in_event;
};

static inline
bool is_worker_disabled()
{
    return ((gr_worker_t *)g_ghost_rocket_global.worker)->worker_disabled;
}

static_inline
void worker_free_queue_item( int thread_id, gr_queue_item_t * queue_item )
{
    if ( queue_item->is_tcp ) {
        gr_worker_t *   self;
        self = (gr_worker_t *)g_ghost_rocket_global.worker;
        gr_tcp_req_free( (gr_tcp_req_t *)queue_item, is_worker_disabled() );
    } else {
        gr_udp_req_free( (gr_udp_req_t *)queue_item );
    }
}

static_inline
void alarm_event_if_need(
    gr_worker_item_t *          worker
)
{
    if ( worker->in_event ) {
        ++ worker->event_alarm_count;
        worker->in_event = false;
        gr_event_alarm( & worker->event );
    }
}

static_inline
void worker_queue_push(
    int                         thread_id,
    gr_worker_item_t *          worker,
    gr_queue_item_t *   item
)
{
    gr_queue_item_t * will_del = NULL;
    gr_queue_item_t * t;
    gr_queue_item_t * curr;
    
    curr = worker->curr;
    if ( QUEUE_ALL_DONE == curr ) {
        curr = NULL;
    }

    // worker->curr 为空说明没有任何表项被处理, 所以必须 worker->curr 非空才进循环
    if ( NULL != worker->curr ) {
        // 从队列头找到要删除的所有数据项,删除
        t = NULL;
        while ( NULL != worker->head && worker->head != curr ) {
            t = worker->head;
            // 向后移动worker->head指针
            gr_debug( "[svr.worker][to_del=%p][next=%p][curr=%p] will del req",
                worker->head, worker->head->next, curr );
            worker->head = worker->head->next;

            assert( t != item );
            worker_free_queue_item( thread_id, t );
        }
        // 注意,如果worker->head为NULL,则此时worker->tail还非NULL呢
    }

    // 将节点插入队列
    item->next = NULL;
    if ( NULL == worker->head ) {
        // 前面说的应该为NULL的worker->tail被一块儿赋了值
        worker->head = worker->tail = item;
        gr_debug( "[svr.worker]insert req %p to empty", item );
    } else if ( worker->head == worker->tail ) {
        // 更新尾节点
        worker->tail = item;
        worker->head->next = item;
        gr_debug( "[svr.worker]insert req %p to single node list, head=%p, tail=%p",
            item, worker->head, worker->tail );
    } else {
        // 记住当前链表尾节点
        t = worker->tail;
        // 更新尾节点
        worker->tail = item;
        // 将原先尾节点的指针指向新增节点
        t->next = item;
        gr_debug( "[svr.worker]insert req %p to after %p, head = %p", item, t, worker->head );
    }

    if ( QUEUE_ALL_DONE == worker->curr ) {
        // 如果发现工作线程已经处理完了, 则重置 worker->curr 指针
        gr_debug( "[svr.worker][head=%p]req reset worker->curr to worker->head", worker->head );
        worker->curr = worker->head;
        // 如果 in_event 为 false, 这儿就可能已经在处理刚压进去的节点了

        // 只有在工作线程确实没事儿干时才需要判断是否在事件里
        // 如果 in_event 为 true, 这儿唤醒工作线程
        alarm_event_if_need( worker );
    }
}

static_inline
void worker_queue_destroy(
    gr_worker_item_t *          worker
)
{
    gr_queue_item_t *   item;

    worker->tail = NULL;
    while ( NULL != ( item = worker->head ) ) {
        worker->head = item->next;
        worker_free_queue_item( -1, item );
    }

    gr_event_destroy( & worker->event );

}

static_inline
gr_queue_item_t * worker_queue_top_inner(
    gr_worker_item_t *      worker
)
{
    if ( NULL == worker->curr ) {
        worker->curr = worker->head;
    } else if ( QUEUE_ALL_DONE == worker->curr ) {
        return NULL;
    }

    return worker->curr;
}

static_inline
gr_queue_item_t * worker_queue_top(
    gr_worker_item_t *      worker
)
{
    gr_queue_item_t * p = NULL;

    // 2013-10-26 07:32 这个优化基本是无效的, non_event_wait_ok_count 的比例太小了
    /*
    size_t i;
    size_t j;

    // 高并发时,用类似自旋锁的思想,尽量不进内核事件
    for ( j = 0; j < 4; ++ j ) {

        for ( i = 0; i < 3; ++ i ) {
            p = worker_queue_top_inner( worker );
            if ( NULL != p ) {
#ifdef GR_DEBUG_CONN
                if ( 0 != i || 0 != j ) {
                    ++ worker->non_event_wait_ok_count;
                }
#endif
                gr_debug( "top req %p", p );
                return p;
            }
        }

        gr_yield();
    }

    ++ worker->non_event_wait_timeout_count;
    return NULL;
    */

    p = worker_queue_top_inner( worker );
    if ( NULL != p ) {
#ifdef GR_DEBUG_CONN
        ++ worker->non_event_wait_ok_count;
#endif
        gr_debug( "[svr.worker]top req %p", p );
        return p;
    }

#ifdef GR_DEBUG_CONN
    ++ worker->non_event_wait_timeout_count;
#endif
    return NULL;
}

static_inline
void worker_queue_pop_top(
    gr_worker_item_t *          worker,
    gr_queue_item_t *   item
)
{
    gr_queue_item_t *  curr;
    gr_queue_item_t *  next;

    curr = worker->curr;
    assert( curr == item );
    next = (gr_queue_item_t *)curr->next;

    if ( NULL == next ) {
        worker->curr = QUEUE_ALL_DONE;
    } else {
        worker->curr = next;
    }

    gr_debug( "[svr.worker][before_pop_curr=%p][next=%p][after_pop_curr=%p]pop req", curr, next, worker->curr );
}

static_inline
int try_to_send_tcp_rsp(
    gr_tcp_req_t *      req,
    gr_proc_ctxt_t *    ctxt,
    int *               sent
)
{
    int r;
    int need_send;

    // 试图直接发送回复包, 这会减少一部分系统调用和线程切换  

    if ( gr_tcp_conn_has_pending_rsp( req->parent ) ) {
        // 如果已经有了等待的回复包, 则必须排队发, 没办法了  
        * sent = 0;
        return 0;
    }

    // 直接发!

    * sent = 0;
    while ( * sent < ctxt->pc_result_buf_len ) {

        need_send = ctxt->pc_result_buf_len - * sent;
        r = send(
            req->parent->fd,
            & ctxt->pc_result_buf[ * sent ],
            need_send,
            MSG_NOSIGNAL
        );
        if ( r >= 0 ) {
            * sent += r;
#ifdef GR_DEBUG_CONN
            req->parent->send_bytes += r;
#endif
            gr_debug( "[tcp.output] send %d bytes", r );

            if ( r < need_send ) {
                // 缓冲区发满了, 没出错。不要再调一次 send 了，如果进系统调用后发现不能发就赔了  
                if ( 0 == r ) {
                    gr_warning( "sent return 0, err = %d:%s!!!!!!!!!!!!!", errno, strerror( errno ) );
                }
                return 0;
            }

            continue;
        }

        if ( EINTR == errno ) {
            continue;
        }
        if ( EAGAIN == errno ) {
            // 缓冲区发满了, 没出错  
            return 0;
        }

        // 发失败了
        gr_error( "[tcp.output]send failed" );

        req->parent->is_network_error = 1;
        if ( req->parent->close_type > GR_NEED_CLOSE ) {
            req->parent->close_type = GR_NEED_CLOSE;
        }

        return -1;
    }

    // 会有这事儿? 
    return 0;
}

static_inline
void process_tcp(
    gr_worker_t *   self,
    gr_thread_t *   thread,
    gr_tcp_req_t *  req
)
{
    per_thread_t *      per_thread      = (per_thread_t *)thread->cookie;
    gr_proc_ctxt_t *    ctxt            = & per_thread->bin_ctxt;
    gr_tcp_req_t *      rsp             = NULL;
    // 默认已处理字节数是输入字节数
    int                 processed_len   = req->buf_len;
    // 记录一下原始的请求包
    char *              req_buf         = req->buf;
    int                 req_buf_max     = req->buf_max;
    int                 req_buf_len     = req->buf_len;
    int                 r;
    int                 sent;

    assert( req->buf_len > 0 && req->buf );
    req->parent->worker_locked = 1;

    ctxt->check_ctxt    = & req->check_ctxt;
    ctxt->port          = req->parent->port_item->port;
    ctxt->fd            = req->parent->fd;
    ctxt->thread_id     = thread->id;

    // 调用模块处理函数处理TCP请求
    gr_module_proc_tcp( req, ctxt, & processed_len );

    // 检查模块处理函数处理结果
    if ( processed_len < 0 ) {
        // processed_len 小于0表示需要服务器断掉连接，返回数据包也不要发
        gr_warning( "[%s][req=%p][buf_len=%d]processed_len = %d, we want close connection",
            thread->name, req, req->buf_len, processed_len );
        ctxt->pc_result_buf_len    = 0;

        if ( req->parent->close_type > GR_NEED_CLOSE ) {
            req->parent->close_type = GR_NEED_CLOSE;
        }
    } else if ( 0 == processed_len ) {
        // processed_len 等于0表示需要服务器断掉连接,但当前返回数据包继续发
        gr_warning( "[%s][req=%p]processed_len = %d, we want close connection",
            thread->name, req, processed_len );
        if ( req->parent->close_type > GR_NEED_CLOSE ) {
            req->parent->close_type = GR_NEED_CLOSE;
        }
    } else {
        gr_debug( "[%s][req=%p]gr_module_proc_tcp rsp %d",
            thread->name, req, ctxt->pc_result_buf_len );
    }

    do {

        if ( ctxt->pc_result_buf_len <= 0 ) {
            // 没有回复数据包
            break;
        }

        // 有回复数据包

        // 试着直接发走
        r = try_to_send_tcp_rsp( req, ctxt, & sent );
        if ( 0 != r || 0 == ctxt->pc_result_buf_len ) {
            // 如果网络异常，则不需要发送数据
            // 把数据长度清0即可，等下次用。
            if ( 0 != ctxt->pc_result_buf_len ) {
                ctxt->pc_result_buf_len = 0;
            }
            break;
        }

        assert( sent <= ctxt->pc_result_buf_len );
        if ( sent == ctxt->pc_result_buf_len ) {
            // 前面 try_to_send_tcp_rsp 已经把整个返回包都发出去了，给力！  
            // 把数据长度清0即可，等下次用。
            ctxt->pc_result_buf_len = 0;
            break;
        }

        // 检查网络是否异常
        if ( req->parent->close_type < GR_NEED_CLOSE || req->parent->is_network_error ) {
            // 如果状态已经进行到GR_NEED_CLOSE的下一步，则说明模块已经确认连接关闭，不需要再发数据包了。
            // 如果网络异常，则不需要发送数据
            // 把数据长度清0即可，等下次用。
            ctxt->pc_result_buf_len = 0;
            break;
        }

        // 分配一个返回包
        rsp = gr_tcp_rsp_alloc( req->parent, 0 );
        if ( NULL == rsp ) {
            // 把数据长度清0即可，等下次用。
            gr_error( "[%s]gr_tcp_rsp_alloc failed", thread->name );
            ctxt->pc_result_buf_len = 0;
            if ( req->parent->close_type > GR_NEED_CLOSE ) {
                req->parent->close_type = GR_NEED_CLOSE;
            }
            break;
        }

        // 将设置返回包缓冲区
        gr_tcp_rsp_set_buf( rsp, ctxt->pc_result_buf, ctxt->pc_result_buf_max, ctxt->pc_result_buf_len, sent );
        // 由于用户模块的返回数据已经从ctxt移动到返回包里了，所以要将ctxt中记录的返回数据信息清掉。
        ctxt->pc_result_buf        = NULL;
        ctxt->pc_result_buf_max    = 0;
        ctxt->pc_result_buf_len    = 0;

        // 将rsp加到连接的返回队列中
        gr_tcp_conn_add_rsp( rsp->parent, rsp );

        // 将rsp放入tcp_out中发送
        r = gr_tcp_out_add( rsp );
        if ( 0 != r ) {
            gr_fatal( "[%s]gr_tcp_out_add faiiled", thread->name );
            //TODO: 按理说，这个地方出错了应该断连接了。但这儿应该是机制出错了，不是断连接这么简单，可能需要重启服务器才能解决。
            // 所以干脆不处理了。理论上这种可能性出现的概率为0
            break;
        }
    } while ( false );

    // 将当前req已经处理完的消息告诉conn，该函数之后有可能连接对象就被删掉了
    gr_tcp_conn_pop_top_req( req->parent, self->tcp_out_disabled );
    // 这儿不能再碰 req 和 conn 了
}

static_inline
void process_udp(
    gr_worker_t *   self,
    gr_thread_t *   thread,
    gr_udp_req_t *  req
)
{
    // echo
    gr_udp_out_add( req );
}

static_inline
int hash_worker_tcp_by_conn(
    gr_tcp_conn_item_t * conn
)
{
    // TCP，按SOCKET描述符算HASH
    // 同一个IP的多个TCP连接可能会分配到不同的处理线程
    return conn->fd % ((gr_worker_t *)g_ghost_rocket_global.worker)->threads.thread_count;
}

static_inline
int hash_worker_tcp( gr_tcp_req_t * req )
{
    return hash_worker_tcp_by_conn( req->parent );
}

static_inline
int hash_worker_udp( gr_udp_req_t * req )
{
    // UDP, 按客户端IP算HASH
    // 同一个IP的不同UDP地址会影射到相同的处理线程，这也是合理的，UDP能够给服务器的压力会很大。
    if ( AF_INET == req->addr.sa_family ) {
        // IPV4
        return req->addr_v4.sin_addr.s_addr %
            ((gr_worker_t *)g_ghost_rocket_global.worker)->threads.thread_count;
    } else if ( AF_INET6 == req->addr.sa_family ) {
        // IPV6
        //TODO: 性能需要优化一下
        const unsigned char *   p = (const unsigned char *)& req->addr_v6.sin6_addr;
        int                     n = 0;
        size_t                  i;
        for ( i = 0; i < sizeof( req->addr_v6.sin6_addr ); ++ i ) {
            n = n * 13 + p[ i ];
        }
        return abs( n ) % ((gr_worker_t *)g_ghost_rocket_global.worker)->threads.thread_count;
    }

    return 0;
}

static_inline
int worker_hash(
    gr_queue_item_t *   queue_item
)
{
    int             hash_id;

    if ( queue_item->is_tcp ) {
        gr_tcp_req_t *  req     = (gr_tcp_req_t *)queue_item;
        hash_id = hash_worker_tcp( req );

        if ( ! req->parent->worker_open ) {
            //gr_atomic_add( 1, & req->parent->thread_refs );
            // 该标记表示连接已经在工作线程里
            req->parent->worker_open = true;
        }
    } else {
        hash_id = hash_worker_udp( (gr_udp_req_t *)queue_item );
    }

    return hash_id;
}

static_inline
int gr_worker_add(
    int                         hash_id,
    gr_queue_item_t *   queue_item
)
{
    gr_worker_t *   self;
    
    self = (gr_worker_t *)g_ghost_rocket_global.worker;
    if ( NULL == self ) {
        return -1;
    }

    if ( queue_item->is_tcp ) {
        if ( ! ((gr_tcp_req_t *)queue_item)->parent->worker_open ) {
            //gr_atomic_add( 1, & req->parent->thread_refs );
            // 该标记表示连接已经在工作线程里
            ((gr_tcp_req_t *)queue_item)->parent->worker_open = true;
        }
    }

    worker_queue_push(
        hash_id,
        & self->items[ hash_id ],
        queue_item
    );

    return 0;
}

static
void worker_init_routine( gr_thread_t * thread )
{
    gr_module_worker_init( thread->id );
}

static
void worker_routine( gr_thread_t * thread )
{
#define     WORK_WAIT_TIMEOUT   100
    gr_worker_t *               self        = (gr_worker_t *)thread->param;
    gr_worker_item_t *          item        = & self->items[ thread->id ];
    gr_queue_item_t *   queue_item;
    int                         r;

    typedef int ( * func_proc_item_t )( gr_worker_t * self, gr_thread_t * thread, void * req );
    static func_proc_item_t  proc_item_funcs[ 2 ] =
    {
        (func_proc_item_t)process_udp,
        (func_proc_item_t)process_tcp
    };

    while ( ! thread->is_need_exit ) {

        // 取包
        queue_item = worker_queue_top( item );
        if ( NULL != queue_item ) {

            // 处理
            assert( 1 == queue_item->is_tcp || 0 == queue_item->is_tcp );
            proc_item_funcs[ queue_item->is_tcp ]( self, thread, queue_item );

            // 弹包
            worker_queue_pop_top( item, queue_item );

        } else {
            // 用事件等
            item->in_event = true;
            ++ item->event_wait_count;
            r = gr_event_wait( & item->event, 1 );
            item->in_event = false;

            if ( 1 == r ) {
                ++ item->event_wait_ok_count;
            } else if ( 0 == r ) {
                ++ item->event_wait_timeout_count;
            }
        }
    };

#ifdef GR_DEBUG_CONN
    gr_info( "[%s][wait=%lu][wait.ok=%lu][wait.timeout=%lu][wait.alarm=%lu]"
             "[nonwait.ok=%lu][nonwait.timeout=%lu]"
             "worker thread %d will exit"
             , thread->name, item->event_wait_count, item->event_wait_ok_count
             , item->event_wait_timeout_count, item->event_alarm_count
             , item->non_event_wait_ok_count, item->non_event_wait_timeout_count
             , thread->id );
#else
    gr_info( "[%s][wait=%lu][wait.ok=%lu][wait.timeout=%lu][wait.alarm=%lu]"
             "worker thread %d will exit"
             , thread->name, item->event_wait_count, item->event_wait_ok_count
             , item->event_wait_timeout_count, item->event_alarm_count
             , thread->id );
#endif

    gr_module_worker_term( thread->id );
}

int gr_worker_init()
{
    gr_worker_t *  p;
    bool    worker_disabled = gr_config_worker_disabled();
    int     thread_count    = gr_config_worker_thread_count();
    int     tcp_in_count    = gr_config_tcp_in_thread_count();
    int     udp_in_count    = gr_config_udp_in_thread_count();
    int     r;
    int     i;

    if ( tcp_in_count < 0 || udp_in_count < 0 ) {
        gr_fatal( "[init]tcp_in_count %d or udp_in_count %d invalid",
            tcp_in_count, udp_in_count );
        return GR_ERR_INVALID_PARAMS;
    }

    if ( worker_disabled ) {
        // 如果禁用 worker，则修改线程数为 UDP + TCP 的和。
        thread_count = tcp_in_count + udp_in_count;
    }
    if ( thread_count < 1 ) {
        gr_fatal( "[init]gr_worker_init thread_count invalid" );
        return GR_ERR_INVALID_PARAMS;
    }

    if ( NULL != g_ghost_rocket_global.worker ) {
        gr_fatal( "[init]gr_work_init already called" );
        return GR_ERR_WRONG_CALL_ORDER;
    }

    p = (gr_worker_t *)gr_calloc( 1, sizeof( gr_worker_t ) );
    if ( NULL == p ) {
        gr_fatal( "[init]malloc %d bytes failed, errno=%d,%s",
            (int)sizeof(gr_worker_t), errno, strerror( errno ) );
        return GR_ERR_BAD_ALLOC;
    }

    r = GR_ERR_UNKNOWN;

    do {

        const char * name   = "svr.worker";

        p->worker_disabled  = worker_disabled;
        p->tcp_out_disabled = gr_config_tcp_out_disabled();

        p->items = (gr_worker_item_t *)gr_calloc( 1,
            sizeof( gr_worker_item_t ) * thread_count );
        if ( NULL == p->items ) {
            gr_fatal( "[init]gr_calloc %d bytes failed: %d",
                (int)sizeof( gr_worker_item_t ) * thread_count,
                get_errno() );
            r = GR_ERR_BAD_ALLOC;
            break;
        }

        // 初始化事件对象
        for ( i = 0; i < thread_count; ++ i ) {
            gr_event_create( & p->items[ i ].event );
        }

        r = gr_threads_start(
            & p->threads,
            thread_count,
            worker_init_routine,
            worker_routine,
            p,
            sizeof( per_thread_t ),
            true,
            p->worker_disabled ? DISABLE_THREAD : ENABLE_THREAD,
            name );
        if ( GR_OK != r ) {
            gr_fatal( "[init]gr_threads_start return error %d", r );
            break;
        }

        if ( p->worker_disabled ) {
            gr_info( "[init]worker.disabled = true" );
        } else {
            gr_info( "[init]worker.disabled = false, worker.thread_count = %d", thread_count );
        }

        gr_debug( "[init]worker_init OK" );

        r = GR_OK;
    } while ( false );

    if ( GR_OK != r ) {

        gr_threads_close( & p->threads );

        for ( i = 0; i < thread_count; ++ i ) {
            worker_queue_destroy( & p->items[ i ] );
        }

        if ( NULL != p->items ) {
            gr_free( p->items );
            p->items = NULL;
        }

        gr_free( p );
        return r;
    }

    g_ghost_rocket_global.worker = p;
    return GR_OK;
}

void gr_worker_term()
{
    gr_worker_t *  p = (gr_worker_t *)g_ghost_rocket_global.worker;
    if ( NULL != p ) {

        int i;

        gr_threads_close( & p->threads );

        for ( i = 0; i < p->threads.thread_count; ++ i ) {
            worker_queue_destroy( & p->items[ i ] );
        }

        if ( NULL != p->items ) {
            gr_free( p->items );
            p->items = NULL;
        }

        g_ghost_rocket_global.worker = NULL;
        gr_free( p );
    }
}

static_inline
int prepare_add_tcp_req(
    gr_tcp_req_t *  req,
    gr_tcp_req_t ** new_req,
    int *           package_len,
    int *           left_len
)
{
    * new_req = NULL;

    // 取得完整数据包长度
    * package_len = gr_tcp_req_package_length( req );
    // 计算请求对象里还剩多少字节数据
    * left_len    = req->buf_len - * package_len;
    if ( * left_len > 0 ) {

        gr_worker_t *   self;
        self = (gr_worker_t *)g_ghost_rocket_global.worker;

        // pipe line 请求支持

        // 缓冲区里是多包数据，剩余的数据不能放在当前请求中，要放回连接对象。
        // 从这儿可以看出，如果模块支持多包数据同时接收，最好是在判断包长度时判断多个包，
        // 让剩下来的半包数据最小，这样拷贝的开销才最小。
        // 必须将 req 扔给 worker，不能将新分配的 new_req 扔给 worker 因为老的 req 有很多状态
        // 要是新分配再拷贝，太不划算。

        // 分配个新的请求对象，将剩余数据拷贝到新分配的请求对象中。如果如果剩余的字节数较小会比较划算。  
        * new_req = gr_tcp_req_alloc(
            req->parent,
            req->buf_max
        );
        if ( NULL == * new_req ) {
            gr_fatal( "[tcp.input ]gr_tcp_req_alloc failed" );
            return -2;
        }

        // 将完整包以外的剩余数据拷贝到新分配的请求对象中
        (* new_req)->buf_len = * left_len;
        memcpy( (* new_req)->buf, & req->buf[ * package_len ], * left_len );

        // 将剩余的数据放回conn中
        req->parent->req = * new_req;

        // 修改请求包数据长度和实际包长度一致
        req->buf_len = * package_len;
    } else {
        // 这说明请求中存放的是单包数据
        // 将请求对象从conn中摘出来
        req->parent->req = NULL;
    }

    // 将请求放到连接的请求列表尾
    gr_tcp_conn_add_req( req->parent );

    return 0;
}

int gr_worker_add_tcp(
    gr_tcp_req_t *  req
)
{
    int             r;
    int             package_len;
    int             left_len;
    gr_tcp_req_t *  new_req;
    int             hash_id;

    hash_id = worker_hash( (gr_queue_item_t *)req );

    r = prepare_add_tcp_req( req, & new_req, & package_len, & left_len );
    if ( 0 != r ) {
        gr_fatal( "[tcp.input ]prepare_add_tcp_req failed" );
        return -2;
    }

    // 试图将请求加入worker
    r = gr_worker_add( hash_id, (gr_queue_item_t *)req );
    if ( 0 == r ) {
        return 0;
    }

    // 向worker中压包失败
    gr_error( "[tcp.input ]gr_worker_add return error %d", r );

    // 要将请求从请求列表尾重新摘出来
    gr_tcp_conn_pop_tail_req( req->parent );

    // 还要再把请求对象扔回连接对象
    if ( NULL != new_req ) {

        gr_worker_t *   self;
        self = (gr_worker_t *)g_ghost_rocket_global.worker;

        // 有多包数据，要删除刚分配出的存放剩余数据的请求
        // 恢复原来数据长度
        req->buf_len = package_len + left_len;

        // 删除刚刚分配的请求包
        gr_tcp_req_free( new_req, is_worker_disabled() );
    }
    req->parent->req = req;

    return r;
}

int gr_worker_add_udp(
    gr_tcp_req_t *  req
)
{
    return gr_worker_add(
        worker_hash( (gr_queue_item_t *)req ),
        (gr_queue_item_t *)req );
}

int gr_worker_process_tcp( gr_tcp_req_t * req )
{
    int             r;
    int             hash_id;
    int             package_len;
    int             left_len;
    gr_tcp_req_t *  new_req;
    gr_worker_t *   self;

    assert( ((gr_queue_item_t *)req)->is_tcp );
    self = (gr_worker_t *)g_ghost_rocket_global.worker;

    hash_id = worker_hash( (gr_queue_item_t *)req );

    r = prepare_add_tcp_req( req, & new_req, & package_len, & left_len );
    if ( 0 != r ) {
        gr_fatal( "[svr.worker]gr_worker_addprepare_add_tcp_req failed" );
        return -2;
    }

    process_tcp( self, & self->threads.threads[ hash_id ], req );
    gr_tcp_req_free( req, is_worker_disabled() );
    return 0;
}

gr_thread_t * gr_worker_get_thread_by_tcp_conn( gr_tcp_conn_item_t * conn )
{
    gr_worker_t * self = (gr_worker_t *)g_ghost_rocket_global.worker;
    if ( self->worker_disabled ) {
        return & self->threads.threads[ hash_worker_tcp_by_conn( conn ) ];
    }

    // 如果Worker没禁用，则conn和线程没有必然的关系。
    // 在 tcp_in 线程分配，在worker线程释放，总觉得有点儿蛋疼，更何况最快的模型是禁用worker
    return NULL;
}
