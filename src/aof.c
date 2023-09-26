/*
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

#include "server.h"
#include "bio.h"
#include "rio.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>

void aofUpdateCurrentSize(void);
void aofClosePipes(void);

/* ----------------------------------------------------------------------------
 * AOF rewrite buffer implementation.
 *
 * The following code implement a simple buffer used in order to accumulate
 * changes while the background process is rewriting the AOF file.
 *
 * 这个缓存用于在 AOF 文件进行重写时暂存新执行的命令。
 *
 * We only need to append, but can't just use realloc with a large block
 * because 'huge' reallocs are not always handled as one could expect
 * (via remapping of pages at OS level) but may involve copying data.
 *
 * For this reason we use a list of blocks, every block is
 * AOF_RW_BUF_BLOCK_SIZE bytes.
 *
 * 因为分配一个非常大的空间并不总是可能的，
 * 也可能产生大量的复制工作，
 * 所以这里使用多个 AOF_RW_BUF_BLOCK_SIZE 空间来保存命令。
 *
 * ------------------------------------------------------------------------- */

#define AOF_RW_BUF_BLOCK_SIZE (1024*1024*10)    /* 10 MB per block 
                                                 *
                                                 * 每个块10 MB
                                                 * */

typedef struct aofrwblock {
    unsigned long used,  // 已使用字节
                  free;  // 剩余可用字节
    char buf[AOF_RW_BUF_BLOCK_SIZE];
} aofrwblock;

/* This function free the old AOF rewrite buffer if needed, and initialize
 * a fresh new one. It tests for server.aof_rewrite_buf_blocks equal to NULL
 * so can be used for the first initialization as well. 
 *
 * 如果需要，此函数将释放旧的AOF重写缓冲区，并初始化一个新的缓冲区。
 * 它测试server.aof_rewrite_buf_blocks是否等于NULL，因此也可以
 * 用于第一次初始化。
 * */
void aofRewriteBufferReset(void) {
    if (server.aof_rewrite_buf_blocks)
        listRelease(server.aof_rewrite_buf_blocks);

    server.aof_rewrite_buf_blocks = listCreate();
    listSetFreeMethod(server.aof_rewrite_buf_blocks,zfree);
}

/* Return the current size of the AOF rewrite buffer. 
 *
 * 返回AOF重写缓冲区的当前大小。
 * */
unsigned long aofRewriteBufferSize(void) {
    listNode *ln;
    listIter li;
    unsigned long size = 0;

    listRewind(server.aof_rewrite_buf_blocks,&li);
    while((ln = listNext(&li))) {
        aofrwblock *block = listNodeValue(ln);
        size += block->used;
    }
    return size;
}

/* Event handler used to send data to the child process doing the AOF
 * rewrite. We send pieces of our AOF differences buffer so that the final
 * write when the child finishes the rewrite will be small. 
 *
 * 用于将数据发送到执行AOF重写的子进程的事件处理程序。我们发送AOF差异缓冲区的
 * 片段，这样当子进程完成重写时，最终的写入将很小。
 * */
void aofChildWriteDiffData(aeEventLoop *el, int fd, void *privdata, int mask) {
    listNode *ln;
    aofrwblock *block;
    ssize_t nwritten;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(privdata);
    UNUSED(mask);

    while(1) {
        ln = listFirst(server.aof_rewrite_buf_blocks);
        block = ln ? ln->value : NULL;
        if (server.aof_stop_sending_diff || !block) {
            aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,
                              AE_WRITABLE);
            return;
        }
        if (block->used > 0) {
            nwritten = write(server.aof_pipe_write_data_to_child,
                             block->buf,block->used);
            if (nwritten <= 0) return;
            memmove(block->buf,block->buf+nwritten,block->used-nwritten);
            block->used -= nwritten;
            block->free += nwritten;
        }
        if (block->used == 0) listDelNode(server.aof_rewrite_buf_blocks,ln);
    }
}

/* Append data to the AOF rewrite buffer, allocating new blocks if needed.
 *
 * 将数组 s 追加到 AOF 缓存的末尾。
 * 如果有需要的话，分配一个新的缓存块。
 */
void aofRewriteBufferAppend(unsigned char *s, unsigned long len) {
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    while(len) {
        /* If we already got at least an allocated block, try appending
         * at least some piece into it. 
         *
         * 如果我们已经获得了至少一个已分配的块，请尝试在其中添加至少一些片段。
         * */

        // 将数据保存到最后一个块里，数据保存的数量是不定的，可能需要创建一个新块来保存数据
        if (block) {
            unsigned long thislen = (block->free < len) ? block->free : len;
            if (thislen) {  /* The current block is not already full. 
                             *
                             * 当前块尚未满。
                             * */
                memcpy(block->buf+block->used, s, thislen);
                block->used += thislen;
                block->free -= thislen;
                s += thislen;
                len -= thislen;
            }
        }

        // 分配第一个块，
        // 或者创建另一个块来保存数据
        if (len) { /* First block to allocate, or need another block. 
                    *
                    * 第一个要分配的块，或者需要另一个块。
                    * */
            int numblocks;

            block = zmalloc(sizeof(*block));
            block->free = AOF_RW_BUF_BLOCK_SIZE;
            block->used = 0;
            listAddNodeTail(server.aof_rewrite_buf_blocks,block);

            /* Log every time we cross more 10 or 100 blocks, respectively
             * as a notice or warning. 
             *
             * 每次我们穿过10个或100个以上的街区时，都要记录下来，分别作为通知或警告。
             * */
            numblocks = listLength(server.aof_rewrite_buf_blocks);
            if (((numblocks+1) % 10) == 0) {
                int level = ((numblocks+1) % 100) == 0 ? LL_WARNING :
                                                         LL_NOTICE;
                serverLog(level,"Background AOF buffer size: %lu MB",
                    aofRewriteBufferSize()/(1024*1024));
            }
        }
    }

    /* Install a file event to send data to the rewrite child if there is
     * not one already. 
     *
     * 安装一个文件事件以将数据发送到重写子进程（如果还没有）。
     * */
    if (!server.aof_stop_sending_diff &&
        aeGetFileEvents(server.el,server.aof_pipe_write_data_to_child) == 0)
    {
        aeCreateFileEvent(server.el, server.aof_pipe_write_data_to_child,
            AE_WRITABLE, aofChildWriteDiffData, NULL);
    }
}

/* Write the buffer (possibly composed of multiple blocks) into the specified
 * fd. If a short write or any other error happens -1 is returned,
 * otherwise the number of bytes written is returned. 
 *
 * 将缓冲区（可能由多个块组成）写入指定的fd。如果发生短写入或任何其他错误，则返回
 * -1，否则返回写入的字节数。
 * */
ssize_t aofRewriteBufferWrite(int fd) {
    listNode *ln;
    listIter li;
    ssize_t count = 0;

    // 遍历所有块，并写入
    listRewind(server.aof_rewrite_buf_blocks,&li);
    while((ln = listNext(&li))) {
        aofrwblock *block = listNodeValue(ln);
        ssize_t nwritten;

        if (block->used) {
            nwritten = write(fd,block->buf,block->used);
            if (nwritten != (ssize_t)block->used) {
                if (nwritten == 0) errno = EIO;
                return -1;
            }
            count += nwritten;
        }
    }
    return count;
}

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * AOF文件实现
 * ------------------------------------------------------------------------- */

/* Return true if an AOf fsync is currently already in progress in a
 * BIO thread. 
 *
 * 如果AOf-fsync当前已在BIO线程中进行，则返回true。
 * */
int aofFsyncInProgress(void) {
    return bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;
}

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. 
 *
 * 启动一个后台任务，对另一个线程中指定的文件描述符（AOF文件的描述符）执行fsync（）。
 * */
void aof_background_fsync(int fd) {
    // 异步同步 AOF 文件，持久化
    bioCreateBackgroundJob(BIO_AOF_FSYNC,(void*)(long)fd,NULL,NULL);
}

/* Kills an AOFRW child process if exists 
 *
 * 如果存在，则终止AOFRW子进程
 * */
void killAppendOnlyChild(void) {
    int statloc;
    /* No AOFRW child? return. 
     *
     * 没有AOFRW 子进程？返回
     * */
    if (server.aof_child_pid == -1) return;
    /* Kill AOFRW child, wait for child exit. 
     *
     * 杀死 AOFRW 子进程，等待子级退出。
     * */
    serverLog(LL_NOTICE,"Killing running AOF rewrite child: %ld",
        (long) server.aof_child_pid);

    // 杀死正在执行的 AOF 重写进程
    if (kill(server.aof_child_pid,SIGUSR1) != -1) {
        while(wait3(&statloc,0,NULL) != server.aof_child_pid);
    }
    /* Reset the buffer accumulating changes while the child saves. 
     *
     * 在子进程保存时，重置累积更改的缓冲区。
     * */
    // 释放缓存
    aofRewriteBufferReset();
    // 移除临时文件
    aofRemoveTempFile(server.aof_child_pid);

    // 关闭服务器 flag
    server.aof_child_pid = -1;
    server.aof_rewrite_time_start = -1;

    /* Close pipes used for IPC between the two processes. 
     *
     * 关闭两个过程之间用于IPC的管道。
     * */
    aofClosePipes();
    closeChildInfoPipe();
    updateDictResizePolicy();
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. 
 *
 * 当用户在运行时使用CONFIG命令从“appendonly yes”切换到“appendonly no”时调用。
 * */
// 用户在运行时通过 CONFIG 命令关闭 AOF 模式时执行
void stopAppendOnly(void) {
    serverAssert(server.aof_state != AOF_OFF);

    // 强制冲洗缓存到 AOF 文件
    flushAppendOnlyFile(1);
    // fsync
    redis_fsync(server.aof_fd);
    // 关闭 AOF 文件
    close(server.aof_fd);

    // 重置 AOF 状态
    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_scheduled = 0;
    killAppendOnlyChild();
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. 
 *
 * 当用户在运行时使用CONFIG命令从“appendonly no”切换到“appendonly yes”时调用。
 * */
int startAppendOnly(void) {
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. 
                           *
                           * 错误消息的当前工作目录路径。
                           * */
    int newfd;

    newfd = open(server.aof_filename,O_WRONLY|O_APPEND|O_CREAT,0644);
    serverAssert(server.aof_state == AOF_OFF);
    if (newfd == -1) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);

        serverLog(LL_WARNING,
            "Redis needs to enable the AOF but can't open the "
            "append only file %s (in server root dir %s): %s",
            server.aof_filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        return C_ERR;
    }
    if (hasActiveChildProcess() && server.aof_child_pid == -1) {
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING,"AOF was enabled but there is already another background operation. An AOF background was scheduled to start when possible.");
    } else {
        /* If there is a pending AOF rewrite, we need to switch it off and
         * start a new one: the old one cannot be reused because it is not
         * accumulating the AOF buffer. 
         *
         * 如果有一个挂起的AOF重写，我们需要关闭它并启动一个新的：旧的不能重用，因为它没
         * 有累积AOF缓冲区。
         * */
        if (server.aof_child_pid != -1) {
            serverLog(LL_WARNING,"AOF was enabled but there is already an AOF rewriting in background. Stopping background AOF and starting a rewrite now.");
            killAppendOnlyChild();
        }
        if (rewriteAppendOnlyFileBackground() == C_ERR) {
            close(newfd);
            serverLog(LL_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
            return C_ERR;
        }
    }
    /* We correctly switched on AOF, now wait for the rewrite to be complete
     * in order to append data on disk. 
     *
     * 我们正确地打开了AOF，现在等待重写完成，以便将数据附加到磁盘上。
     * */
    server.aof_state = AOF_WAIT_REWRITE;
    server.aof_last_fsync = server.unixtime;
    server.aof_fd = newfd;
    return C_OK;
}

/* This is a wrapper to the write syscall in order to retry on short writes
 * or if the syscall gets interrupted. It could look strange that we retry
 * on short writes given that we are writing to a block device: normally if
 * the first call is short, there is a end-of-space condition, so the next
 * is likely to fail. However apparently in modern systems this is no longer
 * true, and in general it looks just more resilient to retry the write. If
 * there is an actual error condition we'll get it at the next try. 
 *
 * 这是写入系统调用的包装器，以便在短写入或系统调用中断时重试。考虑到我们正在向块设
 * 备写入，我们重试短写入可能看起来很奇怪：通常情况下，如果第一次调用很短，就会出现
 * 空间结束的情况，所以下一次很可能会失败。然而，在现代系统中，这显然不再是真的，而
 * 且通常情况下，重试写入看起来更有弹性。如果有实际的错误情况，我们将在下次尝试时得
 * 到它。
 * */
ssize_t aofWrite(int fd, const char *buf, size_t len) {
    ssize_t nwritten = 0, totwritten = 0;

    while(len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR) continue;
            return totwritten ? totwritten : -1;
        }

        len -= nwritten;
        buf += nwritten;
        totwritten += nwritten;
    }

    return totwritten;
}

/* Write the append only file buffer on disk.
 *
 * 将 AOF 缓存写到文件中
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when the
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 *
 * 因为程序需要在会回复客户端之前对 AOF 执行写操作。
 * 而客户端能执行写操作的唯一机会就是在事件 loop 中，
 * 因为程序将所有 AOF 写保存到缓存中，
 * 并在进入事件 loop 之前，将缓存写入到文件中。
 *
 * About the 'force' argument:
 * 关于 force 参数：
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 *
 * 当 fsync 策略是“每秒进行一次 fsync”时，后台队列里可能会有 fsync 等待执行并阻塞，
 * 这些 fsync 会在 serverCron() 中执行。
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync.
 *
 * 但是，如果 force 为 1 ，那么不管后台任务是否在 fsync ，程序都直接执行 fsync 。
 *
 * */
#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. 
                                     *
                                     * 错误日志记录之间的秒数。
                                     * */
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    mstime_t latency;

    // 没有缓存等待写入，直接返回
    if (sdslen(server.aof_buf) == 0) {
        /* Check if we need to do fsync even the aof buffer is empty,
         * because previously in AOF_FSYNC_EVERYSEC mode, fsync is
         * called only when aof buffer is not empty, so if users
         * stop write commands before fsync called in one second,
         * the data in page cache cannot be flushed in time. 
         *
         * 检查我们是否需要在aof缓冲区为空的情况下进行fsync，因为以前在 AOF_FSYNC_EVERYSEC 模式下，
         * 只有当aof缓冲区有数据时才会调用fsync，所以如果用户在一秒钟内调用fsync之前停止写入命令，
         * 则页面缓存中的数据无法及时刷新。
         * */

        // 如果每秒都刷盘
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC &&
            // 记录的已刷盘大小和文件大小不一样 [hhc] 什么场景下出现的？
            server.aof_fsync_offset != server.aof_current_size &&
            // 当前时间比上次的刷盘时间大
            server.unixtime > server.aof_last_fsync &&
            // 没有在异步刷盘
            !(sync_in_progress = aofFsyncInProgress())) {
            goto try_fsync;
        } else {
            return;
        }
    }

    // 返回后台正在等待执行的 fsync 数量
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        sync_in_progress = aofFsyncInProgress();

    // AOF 模式为每秒 fsync ，并且 force 不为 1
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. 
         *
         * 通过这个附加fsync策略，我们可以进行后台fsyncing。如果fsync仍在
         * 进行中，我们可以尝试将写入延迟几秒钟。
         * */
        // 如果 aof_fsync 队列里已经有正在等待的任务
        if (sync_in_progress) {
            // 推迟 aof 重写 ...
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponing, remember that we are
                 * postponing the flush and return. 
                 *
                 * 之前没有写延迟，记住我们是延迟刷新和返回的。
                 * */
                server.aof_flush_postponed_start = server.unixtime;
                return;
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. 
                 *
                 * 我们已经在等待fsync完成，但不到两秒钟就可以了。再次推迟。
                 * */
                return;
            }
            /* Otherwise fall trough, and go write since we can't wait
             * over two seconds. 
             *
             * 否则落空，继续写，因为我们不能等超过两秒钟。
             * */

            // 记录刷盘推延次数
            server.aof_delayed_fsync++;
            serverLog(LL_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike 
     *
     * 我们要执行一次写入。至少如果我们正在编写的文件系统是一个真正的物理文件系统，那么
     * 这应该是原子的。虽然这将使我们免于服务器被终结，但我认为对于整个服务器因电源问题或
     * 类似问题而停止运行，没有什么可做的
     * */

    if (server.aof_flush_sleep && sdslen(server.aof_buf)) {
        usleep(server.aof_flush_sleep);
    }

    latencyStartMonitor(latency);
    // 将 AOF 缓存写入到文件，如果一切幸运的话，写入会原子性地完成
    nwritten = aofWrite(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    latencyEndMonitor(latency);
    /* We want to capture different events for delayed writes:
     * when the delay happens with a pending fsync, or with a saving child
     * active, and when the above two conditions are missing.
     * We also use an additional event name to save all samples which is
     * useful for graphing / monitoring purposes. 
     *
     * 我们希望捕获延迟写入的不同事件：当延迟发生在挂起的fsync中时，或当保存子项处
     * 于活动状态时，以及当缺少上述两个条件时。我们还使用了一个额外的事件名称来保存所有
     * 样本，这对于绘图/监控非常有用。
     * */
    if (sync_in_progress) {
        latencyAddSampleIfNeeded("aof-write-pending-fsync",latency);
    } else if (hasActiveChildProcess()) {
        latencyAddSampleIfNeeded("aof-write-active-child",latency);
    } else {
        latencyAddSampleIfNeeded("aof-write-alone",latency);
    }
    latencyAddSampleIfNeeded("aof-write",latency);

    /* We performed the write so reset the postponed flush sentinel to zero. 
     *
     * 我们执行了写入操作，因此将延迟的刷新哨兵重置为零。
     * */
    server.aof_flush_postponed_start = 0;

    // 写入出错，停止 Redis 并报告错误
    if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
        static time_t last_write_error_log = 0;
        int can_log = 0;

        /* Limit logging rate to 1 line per AOF_WRITE_LOG_ERROR_RATE seconds. 
         *
         * 将日志记录速率限制为每 AOF_WRITE_LOG_ERROR_RATE 秒1行。
         * */
        if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
            can_log = 1;
            last_write_error_log = server.unixtime;
        }

        /* Log the AOF write error and record the error code. 
         *
         * 记录AOF写入错误并记录错误代码。
         * */
        if (nwritten == -1) {
            if (can_log) {
                serverLog(LL_WARNING,"Error writing to the AOF file: %s",
                    strerror(errno));
                server.aof_last_write_errno = errno;
            }
        } else {
            if (can_log) {
                serverLog(LL_WARNING,"Short write while writing to "
                                       "the AOF file: (nwritten=%lld, "
                                       "expected=%lld)",
                                       (long long)nwritten,
                                       (long long)sdslen(server.aof_buf));
            }

            if (ftruncate(server.aof_fd, server.aof_current_size) == -1) {
                if (can_log) {
                    serverLog(LL_WARNING, "Could not remove short write "
                             "from the append-only file.  Redis may refuse "
                             "to load the AOF the next time it starts.  "
                             "ftruncate: %s", strerror(errno));
                }
            } else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. 
                 *
                 * 如果 ftruncate（）成功，我们可以将nwriten设置为-1，因为AOF中
                 * 不再有部分数据。
                 * */
                nwritten = -1;
            }
            server.aof_last_write_errno = ENOSPC;
        }

        /* Handle the AOF write error. 
         *
         * 处理AOF写入错误。
         * */
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the
             * reply for the client is already in the output buffers, and we
             * have the contract with the user that on acknowledged write data
             * is synced on disk. 
             *
             * 当fsync策略为ALWAYS时，我们无法恢复，因为客户端的回复已经在输出缓冲区
             * 中，并且我们与用户签订了在确认写入数据时在磁盘上同步的合同。
             * */
            serverLog(LL_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. 
             *
             * 从写入失败中恢复，将数据留在缓冲区中。但是，只要未清除错误条件，就设置一个错误以
             * 停止接受写入。
             * */
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). 
             *
             * 如果存在部分写入，则修剪sds缓冲区，并且无法使用ftruncate（2）撤消它
             * 。
             * */
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                sdsrange(server.aof_buf,nwritten,-1);
            }
            return; /* We'll try again on the next call... 
                     *
                     * 我们将在下次通话中重试。。。
                     * */
        }
    } else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. 
         *
         * 成功写入（2）。如果AOF处于错误状态，请恢复正常状态并记录事件。
         * */
        if (server.aof_last_write_status == C_ERR) {
            serverLog(LL_WARNING,
                "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = C_OK;
        }
    }

    // 更新 AOF 文件的当前大小
    server.aof_current_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary). 
     *
     * 当AOF缓冲区足够小时，重新使用它。最大值来自4k的竞技场大小减去一些开销（但在
     * 其他方面是任意的）。
     * */
    // 如果 aof 缓存不是太大，那么重用它，否则，清空 aof 缓存
    if ((sdslen(server.aof_buf)+sdsavail(server.aof_buf)) < 4000) {
        sdsclear(server.aof_buf);
    } else {
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
    }

try_fsync:
    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. 
     *
     * 如果重写时 no-appendfsync-on-rewrite 设置为yes，并且有子进程在后台执行I/O，则
     * 不要执行fsync。
     * */
    if (server.aof_no_fsync_on_rewrite && hasActiveChildProcess())
        return;

    /* Perform the fsync if needed. 
     *
     * 如果需要， AOF_FSYNC_ALWAYS ，请执行fsync。
     * */
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* redis_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. 
         *
         * redis_fsync在Linux中被定义为fdatasync（），以避免刷新元 数据。
         * */
        latencyStartMonitor(latency);
        redis_fsync(server.aof_fd); /* Let's try to get this data on the disk 
                                     *
                                     * 让我们试着在磁盘上获取这些数据
                                     * */
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-fsync-always",latency);
        server.aof_fsync_offset = server.aof_current_size;
        // 更新对 AOF 文件最后一次进行 fsync 的时间
        server.aof_last_fsync = server.unixtime;

        // AOF 模式为每秒一次，并且距离上次写 AOF 文件已经超过 1 秒
    } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                server.unixtime > server.aof_last_fsync)) {
        // 仅在没有 fsync 在后台进行时，才将新的 fsync 任务放到后台执行
        if (!sync_in_progress) {
            aof_background_fsync(server.aof_fd);
            server.aof_fsync_offset = server.aof_current_size;
        }
        // 更新对 AOF 文件最后一次进行 fsync 时间
        server.aof_last_fsync = server.unixtime;
    }
}

// 根据输入命令的对象，将命令还原成字符串形式
sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;

    // 记录协议的参数个数部分（argc）
    // 比如 argc == 4 时，生成字符串 "*4\r\n"
    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);

    // 遍历所有参数（argv），并生成协议
    for (j = 0; j < argc; j++) {
        // 取出字符串形式的参数
        o = getDecodedObject(argv[j]);
        // 记录参数字符串长度的协议
        // 比如，对长度为 3 的命令 SET ，生成字符串
        // "$3\r\n"
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);

        // 根据参数，生成协议
        // 比如，对于字符串 SET ，生成以下字符串：
        // "SET\r\n"
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst;
}

/* Create the sds representation of a PEXPIREAT command, using
 * 'seconds' as time to live and 'cmd' to understand what command
 * we are translating into a PEXPIREAT.
 *
 * 创建 PEXPIREAT 命令的 sds 表示，
 * cmd 参数用于指定转换的源指令， seconds 为 TTL （剩余生存时间）。
 *
 * This command is used in order to translate EXPIRE and PEXPIRE commands
 * into PEXPIREAT command so that we retain precision in the append only
 * file, and the time is always absolute and not relative.
 *
 * 个函数用于将 EXPIRE 和 PEXPIRE 转换为 PEXPIREAT
 * 从而在保证精确度不变的情况下，将过期时间从相对值转换为绝对值（一个 UNIX 时间戳）。
 *
 * （当过期时间是确定的，那么不管 AOF 文件何时被载入，该过期的 key 都会正确地过期。）
 *
 * */
sds catAppendOnlyExpireAtCommand(sds buf, struct redisCommand *cmd, robj *key, robj *seconds) {
    long long when;
    robj *argv[3];

    /* Make sure we can use strtoll 
     *
     * 确保我们可以使用strtoll
     * */
    // 取出相对过期值
    seconds = getDecodedObject(seconds);
    when = strtoll(seconds->ptr,NULL,10);
    /* Convert argument into milliseconds for EXPIRE, SETEX, EXPIREAT 
     *
     * 将EXPIRE、SETEX、EXPIREAT的参数转换为毫秒
     * */
    // 选择精度，秒或毫秒
    if (cmd->proc == expireCommand || cmd->proc == setexCommand ||
        cmd->proc == expireatCommand)
    {
        when *= 1000;
    }
    /* Convert into absolute time for EXPIRE, PEXPIRE, SETEX, PSETEX 
     *
     * 转换为EXPIRE、PEXPIRE、SETEX、PSETEX的绝对时间
     * */
    // 根据相对值计算出绝对值
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == setexCommand || cmd->proc == psetexCommand)
    {
        when += mstime();
    }
    decrRefCount(seconds);

    // 转换为 PEXPIREAT 命令
    argv[0] = createStringObject("PEXPIREAT",9);
    argv[1] = key;
    argv[2] = createStringObjectFromLongLong(when);
    buf = catAppendOnlyGenericCommand(buf, 3, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    return buf;
}

// 将给定命令追加到 AOF 文件/缓存中
void feedAppendOnlyFile(
                struct redisCommand *cmd,   // 命令
                int dictid,                 // 数据库的id
                robj **argv,                // 参数指针
                int argc                    // 参数个数
                ) {
    sds buf = sdsempty();
    robj *tmpargv[3];

    /* The DB this command was targeting is not the same as the last command
     * we appended. To issue a SELECT command is needed. 
     *
     * 当前 db 不是指定的 aof db。通过创建 SELECT 命令来切换数据库。
     * */
    if (dictid != server.aof_selected_db) {
        char seldb[64];

        // 让 AOF 文件切换 DB
        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);

        // 程序切换 DB
        server.aof_selected_db = dictid;
    }

    // 将 EXPIRE / PEXPIRE / EXPIREAT 命令翻译为 PEXPIREAT 命令
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == expireatCommand) {
        /* Translate EXPIRE/PEXPIRE/EXPIREAT into PEXPIREAT 
         *
         * 将EXPIRE/PEXPIRE/EXPIREAT转换为PEXPIREAT
         * */
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);

        // 将 SETEX / PSETEX 命令翻译为 SET 和 PEXPIREAT 命令
    } else if (cmd->proc == setexCommand || cmd->proc == psetexCommand) {
        /* Translate SETEX/PSETEX to SET and PEXPIREAT 
         *
         * 将SETEX/PSETEX转换为SET和PEXPIREAT
         * */
        tmpargv[0] = createStringObject("SET",3);
        tmpargv[1] = argv[1];
        tmpargv[2] = argv[3];
        buf = catAppendOnlyGenericCommand(buf,3,tmpargv);
        decrRefCount(tmpargv[0]);
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);
    } else if (cmd->proc == setCommand && argc > 3) {
        int i;
        robj *exarg = NULL, *pxarg = NULL;
        for (i = 3; i < argc; i ++) {
            if (!strcasecmp(argv[i]->ptr, "ex")) exarg = argv[i+1];
            if (!strcasecmp(argv[i]->ptr, "px")) pxarg = argv[i+1];
        }
        serverAssert(!(exarg && pxarg));

        if (exarg || pxarg) {
            /* Translate SET [EX seconds][PX milliseconds] to SET and PEXPIREAT 
             *
             * 将SET[EX秒][PX毫秒]转换为SET和PEXPIREAT
             * */
            buf = catAppendOnlyGenericCommand(buf,3,argv);
            if (exarg)
                buf = catAppendOnlyExpireAtCommand(buf,server.expireCommand,argv[1],
                                                   exarg);
            if (pxarg)
                buf = catAppendOnlyExpireAtCommand(buf,server.pexpireCommand,argv[1],
                                                   pxarg);
        } else {
            buf = catAppendOnlyGenericCommand(buf,argc,argv);
        }
    } else {
        // 其他命令直接追加到 buf 末尾
        /* All the other commands don't need translation or need the
         * same translation already operated in the command vector
         * for the replication itself. 
         *
         * 所有其他命令都不需要翻译，或者需要在复制本身的命令向量中已经操作的相同翻译。
         * */
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. 
     *
     * 附加到AOF缓冲区。这将在重新进入事件循环之前在磁盘上刷新，因此在客户端获得有关
     * 所执行操作的肯定答复之前。
     * */
    // 将 buf 追加到服务器的 aof_buf 末尾
    // 下次 AOF 写入执行时，这些数据就会被写入
    if (server.aof_state == AOF_ON)
        server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));

    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. 
     *
     * 如果正在进行后台仅追加文件重写，我们希望将子数据库和当前数据库之间的差异累积在缓
     * 冲区中，以便在子进程完成工作时，我们可以将差异附加到新的仅追加文件中。
     * */
    // 如果 AOF 重写正在执行，那么也将新 buf 追加到 AOF 重写缓存中
    // 等 AOF 重写完之前的数据之后，新输入的命令也会追加到新 AOF 文件中。
    if (server.aof_child_pid != -1)
        aofRewriteBufferAppend((unsigned char*)buf,sdslen(buf));

    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * AOF加载
 * -------------------------------------------------------------------------*/

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. 
 *
 * 在Redis中，命令总是在客户端的上下文中执行，因此为了加载仅追加文件，我们需要创建一个伪客户端。
 * */
// 创建一个伪终端，用于执行 AOF 中保存的命令
struct client *createAOFClient(void) {
    struct client *c = zmalloc(sizeof(*c));

    selectDb(c,0);
    c->id = CLIENT_ID_AOF; /* So modules can identify it's the AOF client. 
                            *
                            * 所以模块可以识别它是AOF客户端。
                            * */
    c->conn = NULL;
    c->name = NULL;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->argc = 0;
    c->argv = NULL;
    c->argv_len_sum = 0;
    c->bufpos = 0;
    c->flags = 0;
    c->btype = BLOCKED_NONE;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. 
     *
     * 我们将伪客户端设置为等待同步的从客户端，这样Redis就不会试图向该客户端发送回复。
     * */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    c->watched_keys = listCreate();
    c->peerid = NULL;
    c->resp = 2;
    c->user = NULL;
    listSetFreeMethod(c->reply,freeClientReplyValue);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

// 释放客户端的参数
void freeFakeClientArgv(struct client *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);
    c->argv_len_sum = 0;
}

// 释放伪终端
void freeFakeClient(struct client *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    listRelease(c->watched_keys);
    freeClientMultiState(c);
    zfree(c);
}

/* Replay the append log file. On success C_OK is returned. On non fatal
 * error (the append only file is zero-length) C_ERR is returned. On
 * fatal error an error message is logged and the program exists. 
 *
 * 加载AOP日志文件。成功时返回C_OK。在出现非致命错误（仅追加的文件为零长度）时，返回C_ERR。
 * 出现致命错误时，会记录一条错误消息，并且程序存在。
 * */
// 载入 AOF 文件
int loadAppendOnlyFile(char *filename) {
    struct client *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    int old_aof_state = server.aof_state;
    long loops = 0;
    off_t valid_up_to = 0; /* Offset of latest well-formed command loaded. 
                            *
                            * 加载的最新格式良好的命令的偏移量。
                            * */
    off_t valid_before_multi = 0; /* Offset before MULTI command loaded. 
                                   *
                                   * 加载MULTI命令之前的偏移。
                                   * */

    // 打开文件失败
    if (fp == NULL) {
        serverLog(LL_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Handle a zero-length AOF file as a special case. An empty AOF file
     * is a valid AOF because an empty server with AOF enabled will create
     * a zero length file at startup, that will remain like that if no write
     * operation is received. 
     *
     * 作为特殊情况处理零长度的AOF文件。空的AOF文件是有效的AOF，因为启用了AOF的空服务器将在启动时创建一个零长度的文件，
     * 如果没有收到写操作，该文件将保持原样
     * 。
     * */
    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.aof_current_size = 0;
        server.aof_fsync_offset = server.aof_current_size;
        fclose(fp);
        return C_ERR;
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. 
     *
     * 暂时禁用AOF，以防止EXEC向我们要读取的同一文件提供MULTI。
     * */
    server.aof_state = AOF_OFF;

    // 创建伪终端
    fakeClient = createAOFClient();
    // 定义于 rdb.c ，更新服务器的载入状态
    startLoadingFile(fp, filename, RDBFLAGS_AOF_PREAMBLE);

    /* Check if this AOF file has an RDB preamble. In that case we need to
     * load the RDB file and later continue loading the AOF tail. 
     *
     * 检查这个AOF文件是否有RDB前导码。在这种情况下，我们需要加载RDB文件，然后
     * 继续加载AOF尾部。
     * */
    char sig[5]; /* "REDIS"
                  * */
    if (fread(sig,1,5,fp) != 5 || memcmp(sig,"REDIS",5) != 0) {
        /* No RDB preamble, seek back at 0 offset. 
         *
         * 没有RDB前导码，在0偏移处进行回寻。
         * */
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
    } else {
        /* RDB preamble. Pass loading the RDB functions. 
         *
         * RDB 序论;导言;开场白。通过加载RDB函数。
         * */
        rio rdb;

        serverLog(LL_NOTICE,"Reading RDB preamble from AOF file...");
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
        rioInitWithFile(&rdb,fp);
        if (rdbLoadRio(&rdb,RDBFLAGS_AOF_PREAMBLE,NULL) != C_OK) {
            serverLog(LL_WARNING,"Error reading the RDB preamble of the AOF file, AOF loading aborted");
            goto readerr;
        } else {
            serverLog(LL_NOTICE,"Reading the remaining AOF tail...");
        }
    }

    /* Read the actual AOF file, in REPL format, command by command. 
     *
     * 逐命令读取REPL格式的实际AOF文件。
     * */
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time 
         *
         * 不定期为客户服务
         * */
        // 有间隔地处理外部请求
        if (!(loops++ % 1000)) {
            loadingProgress(ftello(fp));
            processEventsWhileBlocked();
            processModuleLoadingProgressEvent(1);
        }

        // 出错或 EOF
        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }

        // 读入命令和命令参数
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;

        /* Load the next command in the AOF as our fake client
         * argv. 
         *
         * 在AOF中加载下一个命令作为我们的假客户端argv。
         * */
        argv = zmalloc(sizeof(robj*)*argc);
        fakeClient->argc = argc;
        fakeClient->argv = argv;

        for (j = 0; j < argc; j++) {
            /* Parse the argument len. 
             *
             * 分析 参数的长度 。
             * */
            char *readres = fgets(buf,sizeof(buf),fp);
            if (readres == NULL || buf[0] != '$') {
                fakeClient->argc = j; /* Free up to j-1. 
                                       *
                                       * 最多释放j-1。
                                       * */
                freeFakeClientArgv(fakeClient);
                if (readres == NULL)
                    goto readerr;
                else
                    goto fmterr;
            }
            len = strtol(buf+1,NULL,10);

            /* Read it into a string object. 
             *
             * 将其读入字符串对象。
             * */
            argsds = sdsnewlen(SDS_NOINIT,len);
            if (len && fread(argsds,len,1,fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. 
                                       *
                                       * 最多释放j-1。
                                       * */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
            argv[j] = createObject(OBJ_STRING,argsds);

            /* Discard CRLF. 
             *
             * 丢弃CRLF。
             * */
            if (fread(buf,2,1,fp) == 0) {
                fakeClient->argc = j+1; /* Free up to j. 
                                         *
                                         * 释放到j。
                                         * */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
        }

        /* Command lookup 
         *
         * 命令查找
         * */
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            serverLog(LL_WARNING,
                "Unknown command '%s' reading the append only file",
                (char*)argv[0]->ptr);
            exit(1);
        }

        if (cmd == server.multiCommand) valid_before_multi = valid_up_to;

        /* Run the command in the context of a fake client 
         *
         * 在伪客户端的上下文中运行命令
         * */
        fakeClient->cmd = fakeClient->lastcmd = cmd;
        if (fakeClient->flags & CLIENT_MULTI &&
            fakeClient->cmd->proc != execCommand)
        {
            queueMultiCommand(fakeClient);
        } else {
            // 在伪终端上下文里执行命令
            cmd->proc(fakeClient);
        }

        /* The fake client should not have a reply 
         *
         * 假客户不应该有回复
         * */
        serverAssert(fakeClient->bufpos == 0 &&
                     listLength(fakeClient->reply) == 0);

        /* The fake client should never get blocked 
         *
         * 假冒客户永远不应该被屏蔽
         * */
        serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. 
         *
         * 清理命令代码可能已经更改了argv/argc，因此我们使用客户端的argv/argc而不是本地变量。
         * */
        freeFakeClientArgv(fakeClient);
        fakeClient->cmd = NULL;
        if (server.aof_load_truncated) valid_up_to = ftello(fp);
        if (server.key_load_delay)
            usleep(server.key_load_delay);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, handle it as it was
     * a short read, even if technically the protocol is correct: we want
     * to remove the unprocessed tail and continue. 
     *
     * 只有在没有错误的情况下达到EOF时，才能达到此点。如果客户端处于MULTI/EXEC中间，
     * 则将其作为短读进行处理，即使从技术上讲协议是正确的：我们希望删除未处理的尾部并继续。
     * */
    if (fakeClient->flags & CLIENT_MULTI) {
        serverLog(LL_WARNING,
            "Revert incomplete MULTI/EXEC transaction in AOF file");
        valid_up_to = valid_before_multi;
        goto uxeof;
    }

loaded_ok: /* DB loaded, cleanup and return C_OK to the caller. 
            *
            * DB已加载，清理并向调用方返回C_OK。
            * */
    fclose(fp);
    freeFakeClient(fakeClient);
    server.aof_state = old_aof_state;
    stopLoading(1);
    aofUpdateCurrentSize();
    server.aof_rewrite_base_size = server.aof_current_size;
    server.aof_fsync_offset = server.aof_current_size;
    return C_OK;

readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. 
          *
          * 读取错误。如果feof（fp）为true，则进入意外EOF。
          * */
    if (!feof(fp)) {
        if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning 
                                                     *
                                                     * 避免valgrind警告
                                                     * */
        fclose(fp);
        serverLog(LL_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
        exit(1);
    }

uxeof: /* Unexpected AOF end of file. 
        *
        * 意外的AOF文件结尾。
        * */
    if (server.aof_load_truncated) {
        serverLog(LL_WARNING,"!!! Warning: short read while loading the AOF file !!!");
        serverLog(LL_WARNING,"!!! Truncating the AOF at offset %llu !!!",
            (unsigned long long) valid_up_to);
        if (valid_up_to == -1 || truncate(filename,valid_up_to) == -1) {
            if (valid_up_to == -1) {
                serverLog(LL_WARNING,"Last valid command offset is invalid");
            } else {
                serverLog(LL_WARNING,"Error truncating the AOF file: %s",
                    strerror(errno));
            }
        } else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. 
             *
             * 确保AOF文件描述符指向截断调用后的文件末尾。
             * */
            if (server.aof_fd != -1 && lseek(server.aof_fd,0,SEEK_END) == -1) {
                serverLog(LL_WARNING,"Can't seek the end of the AOF file: %s",
                    strerror(errno));
            } else {
                serverLog(LL_WARNING,
                    "AOF loaded anyway because aof-load-truncated is enabled");
                goto loaded_ok;
            }
        }
    }
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning 
                                                     *
                                                     * 避免valgrind警告
                                                     * */
    // 清理资源
    fclose(fp);
    serverLog(LL_WARNING,"Unexpected end of file reading the append only file. You can: 1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename>. 2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.");
    exit(1);

fmterr: /* Format error. 
         *
         * 格式错误。
         * */
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning 
                                                     *
                                                     * 避免valgrind警告
                                                     * */
    // 清理资源
    fclose(fp);
    serverLog(LL_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * AOF重写
 * ------------------------------------------------------------------------- */

/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the server.h dependency. 
 *
 * 将写入对象委派为写入大容量字符串或大容量long-long。这并没有放在rio.
 * c中，因为它增加了对server.h的依赖。
 * */
// 将 obj 所指向的整数或字符串写入到 r 当中。
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). 
     *
     * 避免使用getDecodedObject来帮助写时复制（当调用此函数时，我们通常
     * 处于子进程中）。
     * */
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    } else if (sdsEncodedObject(obj)) {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. 
 *
 * 发出重新生成列表对象所需的命令。函数在出错时返回0，在成功时返回1。
 * */
int rewriteListObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = listTypeLength(o);

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *list = o->ptr;
        quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
        quicklistEntry entry;

        while (quicklistNext(li,&entry)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;
                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"RPUSH",5) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }

            if (entry.value) {
                if (rioWriteBulkString(r,(char*)entry.value,entry.sz) == 0) return 0;
            } else {
                if (rioWriteBulkLongLong(r,entry.longval) == 0) return 0;
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        quicklistReleaseIterator(li);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. 
 *
 * 发出重新生成集合对象所需的命令。函数在出错时返回0，在成功时返回1。
 * */
int rewriteSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = setTypeSize(o);

    if (o->encoding == OBJ_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        while(intsetGet(o->ptr,ii++,&llval)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkLongLong(r,llval) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkString(r,ele,sdslen(ele)) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. 
 *
 * 发出重新生成排序集对象所需的命令。函数在出错时返回0，在成功时返回1。
 * */
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = zsetLength(o);

    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;

        eptr = ziplistIndex(zl,0);
        serverAssert(eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        while (eptr != NULL) {
            serverAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
            score = zzlGetScore(sptr);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkDouble(r,score) == 0) return 0;
            if (vstr != NULL) {
                if (rioWriteBulkString(r,(char*)vstr,vlen) == 0) return 0;
            } else {
                if (rioWriteBulkLongLong(r,vll) == 0) return 0;
            }
            zzlNext(zl,&eptr,&sptr);
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkDouble(r,*score) == 0) return 0;
            if (rioWriteBulkString(r,ele,sdslen(ele)) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of an hash.
 *
 * 将选定的 hash 的 key 或者 value 写入到给定 r 。
 *
 * The 'hi' argument passes a valid Redis hash iterator.
 *
 * hi 参数是 Redis 哈希迭代器。
 *
 * The 'what' filed specifies if to write a key or a value and can be
 * either REDIS_HASH_KEY or REDIS_HASH_VALUE.
 *
 * what 指定写入 key 还是写入 value ，
 * 它的值可以是 REDIS_HASH_KEY 或者 REDIS_HASH_VALUE 。
 *
 * The function returns 0 on error, non-zero on success.
 *
 * 出错返回 0 ，成功返回非 0 值。
 */
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            return rioWriteBulkString(r, (char*)vstr, vlen);
        else
            return rioWriteBulkLongLong(r, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        return rioWriteBulkString(r, value, sdslen(value));
    }

    serverPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. 
 *
 * 发出重新生成哈希对象所需的命令。函数在出错时返回0，在成功时返回1。
 * */
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != C_ERR) {
        if (count == 0) {
            int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                AOF_REWRITE_ITEMS_PER_CMD : items;

            if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
            if (rioWriteBulkString(r,"HMSET",5) == 0) return 0;
            if (rioWriteBulkObject(r,key) == 0) return 0;
        }

        if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) == 0) return 0;
        if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE) == 0) return 0;
        if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* Helper for rewriteStreamObject() that generates a bulk string into the
 * AOF representing the ID 'id'. 
 *
 * rewriteStreamObject（）的帮助程序，该帮助程序将大容量字符串生成到表示ID “ID” 的AOF中。
 * */
int rioWriteBulkStreamID(rio *r,streamID *id) {
    int retval;

    sds replyid = sdscatfmt(sdsempty(),"%U-%U",id->ms,id->seq);
    retval = rioWriteBulkString(r,replyid,sdslen(replyid));
    sdsfree(replyid);
    return retval;
}

/* Helper for rewriteStreamObject(): emit the XCLAIM needed in order to
 * add the message described by 'nack' having the id 'rawid', into the pending
 * list of the specified consumer. All this in the context of the specified
 * key and group. 
 *
 * rewriteStreamObject（）的帮助程序：发出所需的XCLAIM，以
 * 便将id为“rawid”的“nack”所描述的消息添加到指定使用者的挂起列表中。
 * 所有这些都是在指定的键和组的上下文中进行的。
 * */
int rioWriteStreamPendingEntry(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer, unsigned char *rawid, streamNACK *nack) {
     /* XCLAIM <key> <group> <consumer> 0 <id> TIME <milliseconds-unix-time>
               RETRYCOUNT <count> JUSTID FORCE. 
      *
      * XCLAIM＜key＞＜group＞＜consumer＞0＜id＞TIME＜毫秒
      * unix时间＞RETRYCOUNT＜count＞JUSTID FORCE。
      * */
    streamID id;
    streamDecodeID(rawid,&id);
    if (rioWriteBulkCount(r,'*',12) == 0) return 0;
    if (rioWriteBulkString(r,"XCLAIM",6) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    if (rioWriteBulkString(r,"0",1) == 0) return 0;
    if (rioWriteBulkStreamID(r,&id) == 0) return 0;
    if (rioWriteBulkString(r,"TIME",4) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_time) == 0) return 0;
    if (rioWriteBulkString(r,"RETRYCOUNT",10) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_count) == 0) return 0;
    if (rioWriteBulkString(r,"JUSTID",6) == 0) return 0;
    if (rioWriteBulkString(r,"FORCE",5) == 0) return 0;
    return 1;
}

/* Emit the commands needed to rebuild a stream object.
 * The function returns 0 on error, 1 on success. 
 *
 * 发出重新生成流对象所需的命令。函数在出错时返回0，在成功时返回1。
 * */
int rewriteStreamObject(rio *r, robj *key, robj *o) {
    stream *s = o->ptr;
    streamIterator si;
    streamIteratorStart(&si,s,NULL,NULL,0);
    streamID id;
    int64_t numfields;

    if (s->length) {
        /* Reconstruct the stream data using XADD commands. 
         *
         * 使用XADD命令重建流数据。
         * */
        while(streamIteratorGetID(&si,&id,&numfields)) {
            /* Emit a two elements array for each item. The first is
             * the ID, the second is an array of field-value pairs. 
             *
             * 为每个项目发出一个两元素数组。第一个是ID，第二个是字段值对的数组。
             * */

            /* Emit the XADD <key> <id> ...fields... command. 
             *
             * 发出XADD＜key＞＜id＞。。。字段。。。命令
             * */
            if (!rioWriteBulkCount(r,'*',3+numfields*2) || 
                !rioWriteBulkString(r,"XADD",4) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkStreamID(r,&id)) 
            {
                streamIteratorStop(&si);
                return 0;
            }
            while(numfields--) {
                unsigned char *field, *value;
                int64_t field_len, value_len;
                streamIteratorGetField(&si,&field,&value,&field_len,&value_len);
                if (!rioWriteBulkString(r,(char*)field,field_len) ||
                    !rioWriteBulkString(r,(char*)value,value_len)) 
                {
                    streamIteratorStop(&si);
                    return 0;                  
                }
            }
        }
    } else {
        /* Use the XADD MAXLEN 0 trick to generate an empty stream if
         * the key we are serializing is an empty string, which is possible
         * for the Stream type. 
         *
         * 如果我们正在序列化的密钥是空字符串，则使用XADD MAXLEN 0技巧生成空流
         * ，这对于stream类型是可能的。
         * */
        id.ms = 0; id.seq = 1; 
        if (!rioWriteBulkCount(r,'*',7) ||
            !rioWriteBulkString(r,"XADD",4) ||
            !rioWriteBulkObject(r,key) ||
            !rioWriteBulkString(r,"MAXLEN",6) ||
            !rioWriteBulkString(r,"0",1) ||
            !rioWriteBulkStreamID(r,&id) ||
            !rioWriteBulkString(r,"x",1) ||
            !rioWriteBulkString(r,"y",1))
        {
            streamIteratorStop(&si);
            return 0;     
        }
    }

    /* Append XSETID after XADD, make sure lastid is correct,
     * in case of XDEL lastid. 
     *
     * 在XADD之后附加XSETID，确保lastid是正确的，如果是XDEL las
     * tid。
     * */
    if (!rioWriteBulkCount(r,'*',3) ||
        !rioWriteBulkString(r,"XSETID",6) ||
        !rioWriteBulkObject(r,key) ||
        !rioWriteBulkStreamID(r,&s->last_id)) 
    {
        streamIteratorStop(&si);
        return 0; 
    }


    /* Create all the stream consumer groups. 
     *
     * 创建所有流使用者组。
     * */
    if (s->cgroups) {
        raxIterator ri;
        raxStart(&ri,s->cgroups);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            streamCG *group = ri.data;
            /* Emit the XGROUP CREATE in order to create the group. 
             *
             * 发出XGROUP CREATE以创建组。
             * */
            if (!rioWriteBulkCount(r,'*',5) ||
                !rioWriteBulkString(r,"XGROUP",6) ||
                !rioWriteBulkString(r,"CREATE",6) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkString(r,(char*)ri.key,ri.key_len) ||
                !rioWriteBulkStreamID(r,&group->last_id)) 
            {
                raxStop(&ri);
                streamIteratorStop(&si);
                return 0;
            }

            /* Generate XCLAIMs for each consumer that happens to
             * have pending entries. Empty consumers have no semantical
             * value so they are discarded. 
             *
             * 为每个碰巧有挂起节点的使用者生成XCLAIM。空的消费者没有语义价值，所以它们被
             * 丢弃了。
             * */
            raxIterator ri_cons;
            raxStart(&ri_cons,group->consumers);
            raxSeek(&ri_cons,"^",NULL,0);
            while(raxNext(&ri_cons)) {
                streamConsumer *consumer = ri_cons.data;
                /* For the current consumer, iterate all the PEL entries
                 * to emit the XCLAIM protocol. 
                 *
                 * 对于当前使用者，迭代所有PEL节点以发出XCLAIM协议。
                 * */
                raxIterator ri_pel;
                raxStart(&ri_pel,consumer->pel);
                raxSeek(&ri_pel,"^",NULL,0);
                while(raxNext(&ri_pel)) {
                    streamNACK *nack = ri_pel.data;
                    if (rioWriteStreamPendingEntry(r,key,(char*)ri.key,
                                                   ri.key_len,consumer,
                                                   ri_pel.key,nack) == 0)
                    {
                        raxStop(&ri_pel);
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                }
                raxStop(&ri_pel);
            }
            raxStop(&ri_cons);
        }
        raxStop(&ri);
    }

    streamIteratorStop(&si);
    return 1;
}

/* Call the module type callback in order to rewrite a data type
 * that is exported by a module and is not handled by Redis itself.
 * The function returns 0 on error, 1 on success. 
 *
 * 调用模块类型回调，以重写模块导出的、Redis本身不处理的数据类型。函数在出错时
 * 返回0，在成功时返回1。
 * */
int rewriteModuleObject(rio *r, robj *key, robj *o) {
    RedisModuleIO io;
    moduleValue *mv = o->ptr;
    moduleType *mt = mv->type;
    moduleInitIOContext(io,mt,r,key);
    mt->aof_rewrite(&io,key,mv->value);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    return io.error ? 0 : 1;
}

/* This function is called by the child rewriting the AOF file to read
 * the difference accumulated from the parent into a buffer, that is
 * concatenated at the end of the rewrite. 
 *
 * 该函数由重写AOF文件的子进程调用，以将从父进程累积的差异读取到缓冲区中，该缓冲区在
 * 重写结束时连接。
 * */
ssize_t aofReadDiffFromParent(void) {
    char buf[65536]; /* Default pipe buffer size on most Linux systems. 
                      *
                      * 大多数Linux系统上的默认管道缓冲区大小。
                      * */
    ssize_t nread, total = 0;

    while ((nread =
            read(server.aof_pipe_read_data_from_parent,buf,sizeof(buf))) > 0) {
        server.aof_child_diff = sdscatlen(server.aof_child_diff,buf,nread);
        total += nread;
    }
    return total;
}

int rewriteAppendOnlyFileRio(rio *aof) {
    dictIterator *di = NULL;
    dictEntry *de;
    size_t processed = 0;
    int j;

    // 遍历所有数据库
    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);

        /* SELECT the new DB 
         *
         * 切换到合适的数据库上
         * */
        if (rioWrite(aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry 
         *
         * 迭代此DB写入每个节点
         * */
        // 遍历数据库的所有 key-value 对
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            long long expiretime;

            keystr = dictGetKey(de);
            o = dictGetVal(de);
            initStaticStringObject(key,keystr);

            expiretime = getExpire(db,&key);

            /* Save the key and associated value 
             *
             * 保存 key 和 value
             * */

            if (o->type == OBJ_STRING) {
                /* Emit a SET command 
                 *
                 * 发出SET命令
                 * */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value 
                 *
                 * 键和值
                 * */
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(aof,o) == 0) goto werr;
            } else if (o->type == OBJ_LIST) {
                if (rewriteListObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_SET) {
                if (rewriteSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_ZSET) {
                if (rewriteSortedSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_HASH) {
                if (rewriteHashObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_STREAM) {
                if (rewriteStreamObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_MODULE) {
                if (rewriteModuleObject(aof,&key,o) == 0) goto werr;
            } else {
                serverPanic("Unknown object type");
            }
            /* Save the expire time 
             *
             * 保存过期时间
             * */
            if (expiretime != -1) {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(aof,expiretime) == 0) goto werr;
            }
            /* Read some diff from the parent process from time to time. 
             *
             * 不时从父进程中读取一些差异。
             * */
            if (aof->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES) {
                processed = aof->processed_bytes;
                aofReadDiffFromParent();
            }
        }
        dictReleaseIterator(di);
        di = NULL;
    }
    return C_OK;

werr:
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * 写一串足以还原数据集的命令到给定文件里。
 * 被 REWRITEAOF 和 BGREWRITEAOF 所使用。
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command.
 *
 * 为了减少重建数据集所需命令的数量，
 * 在可能时，Redis 会使用可变参数命令，比如 RPUSH 、 SADD 和 ZADD 。
 * 不过这些命令每次最多添加的元素不会超过 REDIS_AOF_REWRITE_ITEMS_PER_CMD 。
 *
 * 重写失败返回 REDIS_ERR ，成功返回 REDIS_OK 。
 *
 * */
int rewriteAppendOnlyFile(char *filename) {
    rio aof;
    FILE *fp = NULL;
    char tmpfile[256];
    char byte;

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. 
     *
     * 请注意，与rewriteAppendOnlyFileBackground（）函数
     * 使用的临时名称相比，我们在这里必须使用不同的临时名称。
     * */
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        serverLog(LL_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", strerror(errno));
        return C_ERR;
    }

    server.aof_child_diff = sdsempty();
    // 初始化文件流
    rioInitWithFile(&aof,fp);

    if (server.aof_rewrite_incremental_fsync)
        rioSetAutoSync(&aof,REDIS_AUTOSYNC_BYTES);

    startSaving(RDBFLAGS_AOF_PREAMBLE);

    if (server.aof_use_rdb_preamble) {
        int error;

        // 真正保存rdb文件的地方
        if (rdbSaveRio(&aof,&error,RDBFLAGS_AOF_PREAMBLE,NULL) == C_ERR) {
            errno = error;
            goto werr;
        }
    } else {
        if (rewriteAppendOnlyFileRio(&aof) == C_ERR) goto werr;
    }

    /* Do an initial slow fsync here while the parent is still sending
     * data, in order to make the next final fsync faster. 
     *
     * 在父进程仍在发送数据时，在此处执行初始的慢速fsync，以使下一个最终的fsync
     * 更快。
     * */
    // 重新文件流
    if (fflush(fp) == EOF) goto werr;
    // sync
    if (fsync(fileno(fp)) == -1) goto werr;

    /* Read again a few times to get more data from the parent.
     * We can't read forever (the server may receive data from clients
     * faster than it is able to send data to the child), so we try to read
     * some more data in a loop as soon as there is a good chance more data
     * will come. If it looks like we are wasting time, we abort (this
     * happens after 20 ms without new data). 
     *
     * 再读几次，从父进程获取更多数据。我们不可能永远读取（服务器从客户端接收数据的速度可
     * 能快于向子进程发送数据的速度），因此，一旦很有可能会有更多数据出现，我们就会尝试在
     * 循环中读取更多数据。如果看起来我们在浪费时间，我们会中止（在没有新数据的情况下，这种情况会在20ms后发生）。
     * */
    int nodata = 0;
    mstime_t start = mstime();
    //在1秒内，而且读取的数据次数少于20
    while(mstime()-start < 1000 && nodata < 20) {
        if (aeWait(server.aof_pipe_read_data_from_parent, AE_READABLE, 1) <= 0)
        {
            nodata++;
            continue;
        }
        nodata = 0; /* Start counting from zero, we stop on N *contiguous*
                       timeouts. 
                     *
                     * 从零开始计数，我们在N*连续超时时停止。
                     * */
        aofReadDiffFromParent();
    }

    /* Ask the master to stop sending diffs. 
     *
     * 请主节点停止发送diff。
     * */
    if (write(server.aof_pipe_write_ack_to_parent,"!",1) != 1) goto werr;
    if (anetNonBlock(NULL,server.aof_pipe_read_ack_from_parent) != ANET_OK)
        goto werr;
    /* We read the ACK from the server using a 10 seconds timeout. Normally
     * it should reply ASAP, but just in case we lose its reply, we are sure
     * the child will eventually get terminated. 
     *
     * 我们使用10秒的超时时间从服务器读取ACK。通常情况下，它应该尽快回复，但为了防
     * 止我们失去回复，我们确信子进程最终会被终止。
     * */
    if (syncRead(server.aof_pipe_read_ack_from_parent,&byte,1,5000) != 1 ||
        byte != '!') goto werr;
    serverLog(LL_NOTICE,"Parent agreed to stop sending diffs. Finalizing AOF...");

    /* Read the final diff if any. 
     *
     * 读取最终差异（如果有的话）。
     * */
    aofReadDiffFromParent();

    /* Write the received diff to the file. 
     *
     * 将收到的diff写入文件。
     * */
    serverLog(LL_NOTICE,
        "Concatenating %.2f MB of AOF diff received from parent.",
        (double) sdslen(server.aof_child_diff) / (1024*1024));
    if (rioWrite(&aof,server.aof_child_diff,sdslen(server.aof_child_diff)) == 0)
        goto werr;

    /* Make sure data will not remain on the OS's output buffers 
     *
     * 确保数据不会保留在操作系统的输出缓冲区中
     * */
    if (fflush(fp)) goto werr;
    if (fsync(fileno(fp))) goto werr;
    if (fclose(fp)) { fp = NULL; goto werr; }
    fp = NULL;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. 
     *
     * 使用RENAME确保只有在生成的DB文件正常的情况下才原子地更改DB文件。
     * */
    // 通过更名，用重写后的新 AOF 文件代替旧的 AOF 文件
    if (rename(tmpfile,filename) == -1) {
        serverLog(LL_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }
    serverLog(LL_NOTICE,"SYNC append only file rewrite performed");
    stopSaving(1);
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (fp) fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite pipes for IPC
 * IPC的AOF重写管道
 * --------------------------------------------------------------------------*/

/* This event handler is called when the AOF rewriting child sends us a
 * single '!' char to signal we should stop sending buffer diffs. The
 * parent sends a '!' as well to acknowledge. 
 *
 * 当AOF重写子级向我们发送单个“！”时，将调用此事件处理程序char来表示我们应
 * 该停止发送缓冲区diff。父级发送“！”以及承认。
 * */
void aofChildPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
    char byte;
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);

    if (read(fd,&byte,1) == 1 && byte == '!') {
        serverLog(LL_NOTICE,"AOF rewrite child asks to stop sending diffs.");
        server.aof_stop_sending_diff = 1;
        if (write(server.aof_pipe_write_ack_to_child,"!",1) != 1) {
            /* If we can't send the ack, inform the user, but don't try again
             * since in the other side the children will use a timeout if the
             * kernel can't buffer our write, or, the children was
             * terminated. 
             *
             * 如果我们不能发送ack，请通知用户，但不要重试，因为在另一端，如果内核不能缓冲我
             * 们的写入，或者子进程被终止，子进程将使用超时。
             * */
            serverLog(LL_WARNING,"Can't send ACK to AOF child: %s",
                strerror(errno));
        }
    }
    /* Remove the handler since this can be called only one time during a
     * rewrite. 
     *
     * 删除处理程序，因为在重写过程中只能调用一次。
     * */
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
}

/* Create the pipes used for parent - child process IPC during rewrite.
 * We have a data pipe used to send AOF incremental diffs to the child,
 * and two other pipes used by the children to signal it finished with
 * the rewrite so no more data should be written, and another for the
 * parent to acknowledge it understood this new condition. 
 *
 * 在重写期间创建用于父子进程IPC的管道。我们有一个数据管道用于向子级发送AOF增
 * 量diff，另外两个管道由子级用于发出重写完成的信号，因此不应再写入数据，另一个
 * 管道用于父级确认其收到此新状态。
 * */
int aofCreatePipes(void) {
    int fds[6] = {-1, -1, -1, -1, -1, -1};
    int j;

    if (pipe(fds) == -1) goto error; /* parent -> children data.
                                      * */
    if (pipe(fds+2) == -1) goto error; /* children -> parent ack.
                                        * */
    if (pipe(fds+4) == -1) goto error; /* parent -> children ack.
                                        * */
    /* Parent -> children data is non blocking. 
     *
     * 父->子数据是非阻塞的。
     * */
    if (anetNonBlock(NULL,fds[0]) != ANET_OK) goto error;
    if (anetNonBlock(NULL,fds[1]) != ANET_OK) goto error;
    if (aeCreateFileEvent(server.el, fds[2], AE_READABLE, aofChildPipeReadable, NULL) == AE_ERR) goto error;

    server.aof_pipe_write_data_to_child = fds[1];
    server.aof_pipe_read_data_from_parent = fds[0];
    server.aof_pipe_write_ack_to_parent = fds[3];
    server.aof_pipe_read_ack_from_child = fds[2];
    server.aof_pipe_write_ack_to_child = fds[5];
    server.aof_pipe_read_ack_from_parent = fds[4];
    server.aof_stop_sending_diff = 0;
    return C_OK;

error:
    serverLog(LL_WARNING,"Error opening /setting AOF rewrite IPC pipes: %s",
        strerror(errno));
    for (j = 0; j < 6; j++) if(fds[j] != -1) close(fds[j]);
    return C_ERR;
}

void aofClosePipes(void) {
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
    aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,AE_WRITABLE);
    close(server.aof_pipe_write_data_to_child);
    close(server.aof_pipe_read_data_from_parent);
    close(server.aof_pipe_write_ack_to_parent);
    close(server.aof_pipe_read_ack_from_child);
    close(server.aof_pipe_write_ack_to_child);
    close(server.aof_pipe_read_ack_from_parent);
}

/* ----------------------------------------------------------------------------
 * AOF background rewrite
 * AOF后台重写
 * -------------------------------------------------------------------------*/

/* This is how rewriting of the append only file in background works:
 *
 * 以下是后台重写 AOF 文件的工作步骤：
 *
 * 1) The user calls BGREWRITEAOF
 *    用户调用 BGREWRITEAOF
 *
 * 2) Redis calls this function, that forks():
 *    Redis 调用这个函数，它执行 fork() ：
 *
 *    2a) the child rewrite the append only file in a temp file.
 *        子进程在临时文件中对 AOF 文件进行重写
 *
 *    2b) the parent accumulates differences in server.aof_rewrite_buf.
 *        父进程将新输入的命令追加到 server.aof_rewrite_buf 中
 *
 * 3) When the child finished '2a' exists.
 *    当步骤 2a 执行完之后，子进程结束
 *
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.aof_rewrite_buf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 *
 *    如果子进程的退出状态是 OK 的话，那么父进程将新输入命令写入到临时文件，
 *    然后对临时文件改名，用它代替旧的 AOF 文件，至此，后台 AOF 重写完成。
 */
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;

    if (hasActiveChildProcess()) return C_ERR;
    if (aofCreatePipes() != C_OK) return C_ERR;
    openChildInfoPipe();
    if ((childpid = redisFork(CHILD_TYPE_AOF)) == 0) {
        char tmpfile[256];

        /* Child 
         *
         * 子进程
         * */
        redisSetProcTitle("redis-aof-rewrite");
        redisSetCpuAffinity(server.aof_rewrite_cpulist);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {
            sendChildCOWInfo(CHILD_TYPE_AOF, "AOF rewrite");
            exitFromChild(0);
        } else {
            exitFromChild(1);
        }
    } else {
        /* Parent 
         *
         * 父进程
         * */
        if (childpid == -1) {
            closeChildInfoPipe();
            serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            aofClosePipes();
            return C_ERR;
        }
        serverLog(LL_NOTICE,
            "Background append only file rewriting started by pid %d",childpid);
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);
        server.aof_child_pid = childpid;
        updateDictResizePolicy();
        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.aof_rewrite_buf will start
         * with a SELECT statement and it will be safe to merge. 
         *
         * 我们将appendseldb设置为-1，以强制下一次调用feedAppendOnlyFile（）来发出SELECT命令，
         * 因此父级累积到server.aof_rewrite_buf中的差异将以SELECT语句开始，并且可以安全地进行合并。
         * */
        server.aof_selected_db = -1;
        replicationScriptCacheFlush();
        return C_OK;
    }
    return C_OK; /* unreached 
                  *
                  * 未反应的
                  * */
}

// BGREWRITEAOF 命令的实现
void bgrewriteaofCommand(client *c) {
    if (server.aof_child_pid != -1) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (hasActiveChildProcess()) {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == C_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReplyError(c,"Can't execute an AOF background rewriting. "
                        "Please check the server logs for more information.");
    }
}

// 移除临时文件
void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    bg_unlink(tmpfile);

    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) childpid);
    bg_unlink(tmpfile);
}

/* Update the server.aof_current_size field explicitly using stat(2)
 * to check the size of the file. This is useful after a rewrite or after
 * a restart, normally the size is updated just adding the write length
 * to the current length, that is much faster. 
 *
 * 使用stat（2）显式更新server.aof_current_size字段以检
 * 查文件的大小。这在重写后或重新启动后很有用，通常只需将写入长度添加到当前长度即可
 * 更新大小，这要快得多。
 * */
// 根据 aof 文件的大小，设置 aof 缓存的大小
void aofUpdateCurrentSize(void) {
    struct redis_stat sb;
    mstime_t latency;

    latencyStartMonitor(latency);
    //redis_fstat = fstat/stat 由文件描述符取得文件状态。
    if (redis_fstat(server.aof_fd,&sb) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the AOF file length. stat: %s",
            strerror(errno));
    } else {
        server.aof_current_size = sb.st_size;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fstat",latency);
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. 
 *
 * 后台仅追加文件重写（BGREWRITEOF）终止了其工作。处理好这个。
 * */
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        int newfd, oldfd;
        char tmpfile[256];
        long long now = ustime();
        mstime_t latency;

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");

        /* Flush the differences accumulated by the parent to the
         * rewritten AOF. 
         *
         * 将父项累积的差异刷新到重写的AOF。
         * */
        latencyStartMonitor(latency);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            (int)server.aof_child_pid);
        newfd = open(tmpfile,O_WRONLY|O_APPEND);
        if (newfd == -1) {
            serverLog(LL_WARNING,
                "Unable to open the temporary AOF produced by the child: %s", strerror(errno));
            goto cleanup;
        }

        // 将重写 AOF 文件时新请求的缓存写入到临时文件
        if (aofRewriteBufferWrite(newfd) == -1) {
            serverLog(LL_WARNING,
                "Error trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
            close(newfd);
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rewrite-diff-write",latency);

        serverLog(LL_NOTICE,
            "Residual parent diff successfully flushed to the rewritten AOF (%.2f MB)", (double) aofRewriteBufferSize() / (1024*1024));

        /* The only remaining thing to do is to rename the temporary file to
         * the configured file and switch the file descriptor used to do AOF
         * writes. We don't want close(2) or rename(2) calls to block the
         * server on old file deletion.
         *
         * There are two possible scenarios:
         *
         * 1) AOF is DISABLED and this was a one time rewrite. The temporary
         * file will be renamed to the configured file. When this file already
         * exists, it will be unlinked, which may block the server.
         *
         * 2) AOF is ENABLED and the rewritten AOF will immediately start
         * receiving writes. After the temporary file is renamed to the
         * configured file, the original AOF file descriptor will be closed.
         * Since this will be the last reference to that file, closing it
         * causes the underlying file to be unlinked, which may block the
         * server.
         *
         * To mitigate the blocking effect of the unlink operation (either
         * caused by rename(2) in scenario 1, or by close(2) in scenario 2), we
         * use a background thread to take care of this. First, we
         * make scenario 1 identical to scenario 2 by opening the target file
         * when it exists. The unlink operation after the rename(2) will then
         * be executed upon calling close(2) for its descriptor. Everything to
         * guarantee atomicity for this switch has already happened by then, so
         * we don't care what the outcome or duration of that close operation
         * is, as long as the file descriptor is released again. 
         *
         * 剩下的唯一一件事就是将临时文件重命名为已配置的文件，并切换用于执行AOF写入的文
         * 件描述符。我们不希望关闭（2）或重命名（2）调用来阻止删除旧文件的服务器。
         * 有两种可能的情况：
         *
         * 1）AOF被DISABLED，这是一次重写。临时文件将被重命名为已配置的文件。当该文件已经存在时，
         *    它将被取消链接，这可能会阻塞服务器。
         * 2） 启用AOF，并且重写的AOF将立即开始接收写入。将临时文件重命名为配置文件后，将关闭原始AOF文件描述符。
         * 由于这将是对该文件的最后一次引用，因此关闭它会导致基础文件被取消链接，这可能会阻塞服务器。
         *
         * 为了减轻取消链接操作的阻塞效应（在场景1中由rena
         * me（2）引起，或者在场景2中由close（2）造成），我们使用后台线程来处理这
         * 一问题。首先，我们通过在目标文件存在时打开它，使场景1与场景2相同。重命名（2）
         * 之后的取消链接操作将在为其描述符调用close（2）时执行。到那时，确保该切换的
         * 原子性的一切都已经发生了，所以我们不在乎关闭操作的结果或持续时间是什么，只要文件
         * 描述符再次释放即可。
         * */
        // 根据情况，选择该如何关闭旧 AOF 文件的 fd ，
        // 并用新 AOF 文件的 fd 来代替它。
        if (server.aof_fd == -1) {
            /* AOF disabled 
             *
             * AOF已禁用
             * */

            /* Don't care if this fails: oldfd will be -1 and we handle that.
             * One notable case of -1 return is if the old file does
             * not exist. 
             *
             * 不要在意这是否失败：oldfd将是-1，我们会处理它。-1返回的一个值得注意的情
             * 况是，如果旧文件不存在。
             * */
            oldfd = open(server.aof_filename,O_RDONLY|O_NONBLOCK);
        } else {
            /* AOF enabled 
             *
             * 启用AOF
             * */
            oldfd = -1; /* We'll set this to the current AOF filedes later. 
                         *
                         * 稍后我们将把它设置为当前的AOF文件。
                         * */
        }

        /* Rename the temporary file. This will not unlink the target file if
         * it exists, because we reference it with "oldfd". 
         *
         * 重命名临时文件。如果目标文件存在，这将不会取消链接，因为我们用“oldfd”引用它。
         * */
        latencyStartMonitor(latency);
        // 如果旧 aof 文件存在，通过对新 aof 文件改名来代替它
        if (rename(tmpfile,server.aof_filename) == -1) {
            serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF file %s into %s: %s",
                tmpfile,
                server.aof_filename,
                strerror(errno));
            close(newfd);
            if (oldfd != -1) close(oldfd);
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rename",latency);

        // 更新服务器的 aof_fd ，并根据旧的 fd 属性来进行 fsync 和 close 操作的策略
        if (server.aof_fd == -1) {
            /* AOF disabled, we don't need to set the AOF file descriptor
             * to this new file, so we can close it. 
             *
             * AOF被禁用，我们不需要将AOF文件描述符设置为这个新文件，所以我们可以关闭它。
             * */
            close(newfd);
        } else {
            /* AOF enabled, replace the old fd with the new one. 
             *
             * 启用AOF，用新fd替换旧fd。
             * */
            oldfd = server.aof_fd;
            server.aof_fd = newfd;
            if (server.aof_fsync == AOF_FSYNC_ALWAYS)
                // 调用一个 fsync ，确保新 AOF 文件已保存到磁盘
                redis_fsync(newfd);
            else if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
                aof_background_fsync(newfd);
            server.aof_selected_db = -1; /* Make sure SELECT is re-issued 
                                          *
                                          * 确保重新发布SELECT
                                          * */
            aofUpdateCurrentSize();
            server.aof_rewrite_base_size = server.aof_current_size;
            server.aof_fsync_offset = server.aof_current_size;

            /* Clear regular AOF buffer since its contents was just written to
             * the new AOF from the background rewrite buffer. 
             *
             * 清除常规AOF缓冲区，因为它的内容刚刚从后台重写缓冲区写入新的AOF。
             * */
            sdsfree(server.aof_buf);
            server.aof_buf = sdsempty();
        }

        server.aof_lastbgrewrite_status = C_OK;

        serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed 
         *
         * 如果需要，将状态从WAIT_REWRITE更改为ON
         * */
        if (server.aof_state == AOF_WAIT_REWRITE)
            server.aof_state = AOF_ON;

        /* Asynchronously close the overwritten AOF. 
         *
         * 异步关闭覆盖的AOF。
         * */
        if (oldfd != -1) bioCreateBackgroundJob(BIO_CLOSE_FILE,(void*)(long)oldfd,NULL,NULL);

        serverLog(LL_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    } else if (!bysignal && exitcode != 0) {
        server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. 
         *
         * SIGUSR1被列入白名单，因此我们有一种在不触发错误条件的情况下杀死孩子的方法
         * 。
         * */
        if (bysignal != SIGUSR1)
            server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }

cleanup:
    aofClosePipes();
    aofRewriteBufferReset();
    aofRemoveTempFile(server.aof_child_pid);
    server.aof_child_pid = -1;
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. 
     *
     * 如果我们正在等待它打开AOF，请安排一次新的重写。
     * */
    if (server.aof_state == AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
