/* Linux epoll(2) based ae.c module
 *
 * ae_epoll是一个在Linux下使用epoll进行事件驱动封装的库。
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/epoll.h>

typedef struct aeApiState {
    /* 该 epoll 事件对应的文件描述符 fd */
    int epfd;
    /* 该 fd 对应的 epoll 事件结构体（sys/epoll.h）*/
    struct epoll_event *events;
} aeApiState;

// 创建 epoll 对应的 fd， redis 统一把 网络nio事件处理对象称为为 aeApi
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    // 创建底层事件集合
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    // 创建epoll对应的fd
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel ， 1024 只是提示内核*/
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    // 把 epoll 对象赋值给 eventLoop 的 apidata 属性
    eventLoop->apidata = state;
    return 0;
}

// 修改 aeApi 的大小
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    // 重新分配events的内存至新size
    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}

// 释放 aeApi
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    // 由epoll打开的fd一定要调用close，这是epoll规定的
    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

// aeApi 添加需要监听的事件
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    struct epoll_event ee = {0}; /* avoid valgrind warning 为了绕开 valgrind 告警 */

    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    // 如果 fd 已经被监控了某个事件，我们需要一个 MOD 操作。否则，我们需要一个 ADD 操作。
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events 合并久的事件 */
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}

// aeApi 删除监听的事件
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        // 请注意，内核 < 2.6.9 需要非空事件指针，即使对于EPOLL_CTL_DEL也是如此。
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}


// 核心方法，调用 epoll 的 epoll_wait 等待各种事件的触发
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    // 调用 epoll 的 epoll_wait 等待各种事件的触发
    // tvp 等待的时间
    // epoll_wait是一个最大阻塞指定毫秒数以监听该epfd是否有事件被触发的函数
    // state->events储存被触发的事件
    // eventLoop->setsize为最大监听事件数，retval不会超过这个值
    // (tvp->tv_sec*1000 + tvp->tv_usec/1000)为最大阻塞毫秒数，-1为持续阻塞（直到epfd有事件被触发）
    // 所以如果要一直挂起等待有事件就绪，应该传进去tvp=NULL
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            // events是一个指针，+j会执行正确的偏移，偏移到对应的第j个事件
            struct epoll_event *e = state->events+j;

            // 除EPOLLIN均映射到AE_WRITABLE
            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE|AE_READABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE|AE_READABLE;
            // 赋值被触发的事件的fd和mask
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "epoll";
}
