/* Asynchronous replication implementation.
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


#include "server.h"
#include "cluster.h"
#include "bio.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(connection *conn);
void replicationSendAck(void);
void putSlaveOnline(client *slave);
int cancelReplicationHandshake(void);

/* We take a global flag to remember if this instance generated an RDB
 * because of replication, so that we can remove the RDB file in case
 * the instance is configured to have no persistence. 
 *
 * 我们使用全局标志来记住这个节点是否因为主从同步而生成了RDB，
 * 以便在节点被配置为没有持久性的情况下删除RDB文件。
 * */
int RDBGeneratedByReplication = 0;

/* --------------------------- Utility functions ---------------------------- 
 *
 * ---------------------------实用程序功能-------------------------------------*/

/* Return the pointer to a string representing the slave ip:listening_port
 * pair. Mostly useful for logging, since we want to log a slave using its
 * IP address and its listening port which is more clear for the user, for
 * example: "Closing connection with replica 10.1.2.3:6380". 
 *
 * 将指针返回到表示从ip:listening_port对的字符串。最适用于日志记录，
 * 因为我们希望使用其IP地址和侦听端口来记录从节点设备，这对用户来说更清楚，例如：
 * “关闭与从节点10.1.2.3:6380的连接”。
 * */
// 通过客户端获取ip和port，主要用于打印
char *replicationGetSlaveName(client *c) {
    static char buf[NET_PEER_ID_LEN];
    char ip[NET_IP_STR_LEN];

    ip[0] = '\0';
    buf[0] = '\0';
    if (c->slave_ip[0] != '\0' ||
        connPeerToString(c->conn,ip,sizeof(ip),NULL) != -1)
    {
        /* Note that the 'ip' buffer is always larger than 'c->slave_ip' 
         *
         * 请注意，“ip”缓冲区始终大于“c->slave_ip”
         * */
        if (c->slave_ip[0] != '\0') memcpy(ip,c->slave_ip,sizeof(c->slave_ip));

        if (c->slave_listening_port)
            anetFormatAddr(buf,sizeof(buf),ip,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-replica-port>",ip);
    } else {
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}

/* Plain unlink() can block for quite some time in order to actually apply
 * the file deletion to the filesystem. This call removes the file in a
 * background thread instead. We actually just do close() in the thread,
 * by using the fact that if there is another instance of the same file open,
 * the foreground unlink() will only remove the fs name, and deleting the
 * file's storage space will only happen once the last reference is lost. 
 *
 * Plain unlink（）可能会阻塞相当长的一段时间，以便将文件删除实际应用于
 * 文件系统。此调用将删除后台线程中的文件。实际上，我们只是在线程中执行close（），
 * 因为如果打开了同一文件的另一个节点，那么前台unlink（）只会删除fs名称，
 * 并且只有在最后一个引用丢失后才会删除文件的存储空间。
 * */
int bg_unlink(const char *filename) {
    int fd = open(filename,O_RDONLY|O_NONBLOCK);
    if (fd == -1) {
        /* Can't open the file? Fall back to unlinking in the main thread. 
         *
         * 无法打开文件？回退到取消主线程中的链接。
         * */
        // unlink()函数: 并不一定会真正的删除文件，它先会检查文件系统中此文件的连接数是否为1，
        // 如果不是1说明此文件还有其他链接对象，因此只对此文件的连接数进行减1操作。若连接数为1，
        // 并且在此时没有任何进程打开该文件，此内容才会真正地被删除掉。在有进程打开此文件的情况下，
        // 则暂时不会删除，直到所有打开该文件的进程都结束时文件就会被删除。
        return unlink(filename);
    } else {
        /* The following unlink() removes the name but doesn't free the
         * file contents because a process still has it open. 
         *
         * 下面的unlink（）将删除名称，但不会释放文件内容，因为进程仍处于打开状态。
         * */
        int retval = unlink(filename);
        if (retval == -1) {
            /* If we got an unlink error, we just return it, closing the
             * new reference we have to the file. 
             *
             * 如果我们得到一个取消链接的错误，我们只是返回它，关闭我们对该文件的新引用。
             * */
            int old_errno = errno;
            close(fd);  /* This would overwrite our errno. So we saved it. 
                         *
                         * 这将覆盖我们的errno。所以我们救了它。
                         * */
            errno = old_errno;
            return -1;
        }
        // 异步关闭文件
        bioCreateBackgroundJob(BIO_CLOSE_FILE,(void*)(long)fd,NULL,NULL);
        return 0; /* Success. 
                   *
                   * 成功
                   * */
    }
}

/* ---------------------------------- MASTER -------------------------------- 
 *
 * ----------------------------------主节点-----------------------------------
 * */

void createReplicationBacklog(void) {
    serverAssert(server.repl_backlog == NULL);
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;

    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. 
     *
     * 我们的缓冲区中没有任何数据，但实际上，我们拥有的第一个字节是将为复制流生成的下一个字节。
     * */
    server.repl_backlog_off = server.master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to both update the
 * server.repl_backlog_size and to resize the buffer and setup it so that
 * it contains the same data as the previous one (possibly less data, but
 * the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). 
 *
 * 当用户在运行时修改同步缓冲区大小时，会调用此函数。该函数既可以更新server.repl_backlog_size，
 * 也可以调整缓冲区的大小并进行设置，使其包含与前一个相同的数据
 * （可能更少的数据，但最新的字节，或者在缓冲区扩大的情况下，包含相同的数据和更多的可用空间）。
 * */
void resizeReplicationBacklog(long long newsize) {
    // 保证不会低于 16k
    if (newsize < CONFIG_REPL_BACKLOG_MIN_SIZE)
        newsize = CONFIG_REPL_BACKLOG_MIN_SIZE;

    // 目前的值和设置的值相等，返回
    if (server.repl_backlog_size == newsize) return;

    server.repl_backlog_size = newsize;

    // 释放旧的，创建新的，并重置各个记录相关的值
    if (server.repl_backlog != NULL) {
        /* What we actually do is to flush the old buffer and realloc a new
         * empty one. It will refill with new data incrementally.
         * The reason is that copying a few gigabytes adds latency and even
         * worse often we need to alloc additional space before freeing the
         * old buffer. 
         *
         * 我们实际要做的是刷新旧的缓冲区并重新分配一个新的空缓冲区。它将以递增的方式重新填
         * 充新数据。原因是复制几GB会增加延迟，更糟糕的是，在释放旧缓冲区之前，我们通常需
         * 要分配额外的空间。
         * */
        zfree(server.repl_backlog);
        server.repl_backlog = zmalloc(server.repl_backlog_size);
        server.repl_backlog_histlen = 0;
        server.repl_backlog_idx = 0;
        /* Next byte we have is... the next since the buffer is empty. 
         *
         * 我们的下一个字节是…下一个，因为缓冲区是空的。
         * */
        server.repl_backlog_off = server.master_repl_offset+1;
    }
}

// 释放 backlog 的内存
void freeReplicationBacklog(void) {
    serverAssert(listLength(server.slaves) == 0);
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/* Add data to the replication backlog.
 * This function also increments the global replication offset stored at
 * server.master_repl_offset, because there is no case where we want to feed
 * the backlog without incrementing the offset. 
 *
 * 将数据添加到同步缓冲区中。此函数还增加存储在server.master_repl_offset中的全局复制偏移量，
 * 因为在不增加偏移量的情况下，我们不希望feed backlog。
 * */
void feedReplicationBacklog(void *ptr, // 待写入数据指针
                            size_t len // 待写入数据长度
                            ) {
    unsigned char *p = ptr;

    server.master_repl_offset += len;

    /* This is a circular buffer, so write as much data we can at every
     * iteration and rewind the "idx" index if we reach the limit. 
     *
     * 这是一个循环缓冲区，所以在每次迭代时尽可能多地写入数据，如果达到极限，则回退“idx”索引。
     * */
    while(len) {
        // 主从同步缓冲区的剩余长度
        size_t thislen = server.repl_backlog_size - server.repl_backlog_idx;

        // 如果剩余长度大于待写入长度
        if (thislen > len) thislen = len;
            // 由 参数2 指向地址为起始地址的连续 参数3 个字节的数据复制到以 参数1 指向地址为起始地址的空间内。
            memcpy(server.repl_backlog+server.repl_backlog_idx,p,thislen);

        // 写入索引往后移动
        server.repl_backlog_idx += thislen;

        // 如果剩余长度没了，索引位置置 0
        if (server.repl_backlog_idx == server.repl_backlog_size)
            server.repl_backlog_idx = 0;

        // 写入长度减去刚写入的长度
        len -= thislen;

        // 待写入数据指针移动 刚写入的数据的 长度
        p += thislen;
        server.repl_backlog_histlen += thislen;
    }

    // 如果记录的实际长度大于真实的总长度，修正
    if (server.repl_backlog_histlen > server.repl_backlog_size)
        server.repl_backlog_histlen = server.repl_backlog_size;
    /* Set the offset of the first byte we have in the backlog. 
     *
     * 设置积压工作中第一个字节的偏移量。
     * */
    server.repl_backlog_off = server.master_repl_offset -
                              server.repl_backlog_histlen + 1;
}

/* Wrapper for feedReplicationBacklog() that takes Redis string objects as input.
 *
 * feedReplicationBacklog（）的包装器，它将Redis字符串对象作为输入。
 * */
void feedReplicationBacklogWithObject(robj *o) {
    // LONG_STR_SIZE = 21
    char llstr[LONG_STR_SIZE];
    void *p;
    size_t len;

    // 如果是数字编码，转为字符，要不直接写入
    if (o->encoding == OBJ_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    feedReplicationBacklog(p,len);
}

/* Propagate write commands to slaves, and populate the replication backlog
 * as well. This function is used if the instance is a master: we use
 * the commands received by our clients in order to create the replication
 * stream. Instead if the instance is a slave and has sub-slaves attached,
 * we use replicationFeedSlavesFromMasterStream() 
 *
 * 将写入命令传播到从节点服务器，并填充同步缓冲区。如果节点是主节点，则使用此函数：我们
 * 使用客户端接收的命令来创建复制流。相反，如果节点是从节点并附加了子从节点，则使用
 * replicationFeedSlavesFromMasterStream（）
 * */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j, len;
    char llstr[LONG_STR_SIZE];

    /* If the instance is not a top level master, return ASAP: we'll just proxy
     * the stream of data we receive from our master instead, in order to
     * propagate *identical* replication stream. In this way this slave can
     * advertise the same replication ID as the master (since it shares the
     * master replication history and has the same backlog and offsets). 
     *
     * 如果节点不是顶级主节点，请尽快返回：我们只代理从主节点接收的数据流，以便传播*identicalreplication流。
     * 通过这种方式，该从节点服务器可以通告与主服务器相同的复制ID（因为它共享主服务器复制历史，并且具有相同的缓冲区和偏移）。
     * */
    if (server.masterhost != NULL) return;

    /* If there aren't slaves, and there is no backlog buffer to populate,
     * we can return ASAP. 
     *
     * 如果没有从节点，也没有积压缓冲区可以填充，我们可以尽快返回。
     * */
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;

    /* We can't have slaves attached and no backlog. 
     *
     * 我们不能有从节点附身，也不能有积压。
     * */
    serverAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));

    /* Send SELECT command to every slave if needed. 
     *
     * 如果需要，向每个从节点发送SELECT命令。
     * */
    if (server.slaveseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. 
         *
         * 对于一些DB，我们已经预先计算了SELECT命令。
         * */
        if (dictid >= 0 && dictid < PROTO_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        /* Add the SELECT command into the backlog. 
         *
         * 将SELECT命令添加到缓冲区中。
         * */
        if (server.repl_backlog) feedReplicationBacklogWithObject(selectcmd);

        /* Send it to slaves. 
         *
         * 把它送给从节点。
         * */
        listRewind(slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            // 不要向还在等待 BGSAVE 的附属节点发送命令
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
            addReply(slave,selectcmd);
        }

        if (dictid < 0 || dictid >= PROTO_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }
    server.slaveseldb = dictid;

    /* Write the command to the replication backlog if any. 
     *
     * 将命令写入同步缓冲区（如果有）。
     * */
    if (server.repl_backlog) {
        char aux[LONG_STR_SIZE+3];

        /* Add the multi bulk reply length. 
         *
         * 添加多批量回复长度。
         * */
        aux[0] = '*';
        len = ll2string(aux+1,sizeof(aux)-1,argc);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        feedReplicationBacklog(aux,len+3);

        for (j = 0; j < argc; j++) {
            long objlen = stringObjectLen(argv[j]);

            /* We need to feed the buffer with the object as a bulk reply
             * not just as a plain string, so create the $..CRLF payload len
             * and add the final CRLF 
             *
             * 我们需要将对象作为批量回复而不仅仅是纯字符串提供给缓冲区，因此创建$。。CRLF
             * 有效载荷len并添加最终CRLF
             * */
            aux[0] = '$';
            len = ll2string(aux+1,sizeof(aux)-1,objlen);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            feedReplicationBacklog(aux,len+3);
            feedReplicationBacklogWithObject(argv[j]);
            feedReplicationBacklog(aux+len+1,2);
        }
    }

    /* Write the command to every slave. 
     *
     * 将命令写入每个从节点。
     * */
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start. 
         *
         * 不要推送仍在等待BGSAVE启动的从节点。
         * */
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;

        /* Feed slaves that are waiting for the initial SYNC (so these commands
         * are queued in the output buffer until the initial SYNC completes),
         * or are already in sync with the master. 
         *
         * 正在等待初始SYNC（因此，这些命令在输出缓冲区中排队，直到初始SYNC完成）或
         * 已经与主同步的Feed从节点。
         * */

        /* Add the multi bulk length. 
         *
         * 添加多体积长度。
         * */
        addReplyArrayLen(slave,argc);

        /* Finally any additional argument that was not stored inside the
         * static buffer if any (from j to argc). 
         *
         * 最后，没有存储在静态缓冲区中的任何其他参数（如果有的话）（从j到argc）。
         * */
        for (j = 0; j < argc; j++)
            addReplyBulk(slave,argv[j]);
    }
}

/* This is a debugging function that gets called when we detect something
 * wrong with the replication protocol: the goal is to peek into the
 * replication backlog and show a few final bytes to make simpler to
 * guess what kind of bug it could be. 
 *
 * 这是一个调试函数，当我们检测到复制协议有问题时会调用它：目标是查看复制积压工作，
 * 并显示最后几个字节，以便更容易猜测它可能是什么样的错误。
 * */
void showLatestBacklog(void) {
    if (server.repl_backlog == NULL) return;

    long long dumplen = 256;
    if (server.repl_backlog_histlen < dumplen)
        dumplen = server.repl_backlog_histlen;

    /* Identify the first byte to dump. 
     *
     * 识别要转储的第一个字节。
     * */
    long long idx =
      (server.repl_backlog_idx + (server.repl_backlog_size - dumplen)) %
       server.repl_backlog_size;

    /* Scan the circular buffer to collect 'dumplen' bytes. 
     *
     * 扫描循环缓冲区以收集“dumplen”字节。
     * */
    sds dump = sdsempty();
    while(dumplen) {
        long long thislen =
            ((server.repl_backlog_size - idx) < dumplen) ?
            (server.repl_backlog_size - idx) : dumplen;

        dump = sdscatrepr(dump,server.repl_backlog+idx,thislen);
        dumplen -= thislen;
        idx = 0;
    }

    /* Finally log such bytes: this is vital debugging info to
     * understand what happened. 
     *
     * 最后记录这样的字节：这是了解发生了什么的重要调试信息。
     * */
    serverLog(LL_WARNING,"Latest backlog is: '%s'", dump);
    sdsfree(dump);
}

/* This function is used in order to proxy what we receive from our master
 * to our sub-slaves. 
 *
 * 此函数用于将我们从主节点接收的内容代理给子节点。
 * */
#include <ctype.h>
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen) {
    listNode *ln;
    listIter li;

    /* Debugging: this is handy to see the stream sent from master
     * to slaves. Disabled with if(0). 
     *
     * 调试：这可以方便地查看从主设备发送到从节点设备的流。已使用if（0）禁用。
     * */
    if (0) {
        printf("%zu:",buflen);
        for (size_t j = 0; j < buflen; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");
    }

    // 写入内容
    if (server.repl_backlog) feedReplicationBacklog(buf,buflen);
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start. 
         *
         * 不要推送仍在等待BGSAVE启动的从节点。
         * */
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
        addReplyProto(slave,buf,buflen);
    }
}

// 向 monitors 发送 client c 的类型 和 argv 数据。
void replicationFeedMonitors(client *c, 
                             list *monitors, 
                             int dictid, // 数据库的Id
                             robj **argv, 
                             int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (c->flags & CLIENT_LUA) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & CLIENT_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == OBJ_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(OBJ_STRING,cmdrepr);

    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        client *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

/* Feed the slave 'c' with the replication backlog starting from the
 * specified 'offset' up to the end of the backlog. 
 *
 * 将同步缓冲区从指定的 “offset” 开始一直提供给从节点“c”，直到缓冲区结束。
 * */
long long addReplyReplicationBacklog(client *c, long long offset) {
    long long j, skip, len;

    serverLog(LL_DEBUG, "[PSYNC] Replica request offset: %lld", offset);

    if (server.repl_backlog_histlen == 0) {
        serverLog(LL_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    serverLog(LL_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    serverLog(LL_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog_off);
    serverLog(LL_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog_histlen);
    serverLog(LL_DEBUG, "[PSYNC] Current index: %lld",
             server.repl_backlog_idx);

    /* Compute the amount of bytes we need to discard. 
     *
     * 计算我们需要丢弃的字节数。
     * */
    // 已经跳过了的字节数 = 想要发送数据开始位置的开始索引 - 真正待发送数据的开始索引
    skip = offset - server.repl_backlog_off;
    serverLog(LL_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* Point j to the oldest byte, that is actually our
     * server.repl_backlog_off byte. 
     *
     * 将j指向最旧的字节，它实际上是我们的server.repl_backlog_off字节。
     * */
    // 已发送的数据空间 + 剩余可写入空间 = server.repl_backlog_size-server.repl_backlog_histlen
    // j = server.repl_backlog_idx + 已发送的数据空间 + 剩余可写入空间
    // % 主要解决：
    //           server.repl_backlog_idx = server.repl_backlog_size
    //           server.repl_backlog_histlen = 0
    //           等的情况。

    // [hhc]
    j = (server.repl_backlog_idx +
        (server.repl_backlog_size-server.repl_backlog_histlen)) %
        server.repl_backlog_size;
    serverLog(LL_DEBUG, "[PSYNC] Index of first byte: %lld", j);

    /* Discard the amount of data to seek to the specified 'offset'. 
     *
     * 放弃要查找到指定“偏移量”的数据量。
     * */
    j = (j + skip) % server.repl_backlog_size;

    /* Feed slave with data. Since it is a circular buffer we have to
     * split the reply in two parts if we are cross-boundary. 
     *
     * 向从节点服务器提供数据。由于这是一个循环缓冲区，如果我们是跨区域的，我们必须将回复分为两部分。
     * */
    len = server.repl_backlog_histlen - skip;
    serverLog(LL_DEBUG, "[PSYNC] Reply total length: %lld", len);
    while(len) {
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;

        serverLog(LL_DEBUG, "[PSYNC] addReply() length: %lld", thislen);
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    return server.repl_backlog_histlen - skip;
}

/* Return the offset to provide as reply to the PSYNC command received
 * from the slave. The returned value is only valid immediately after
 * the BGSAVE process started and before executing any other command
 * from clients. 
 *
 * 对从「从节点」接收到的PSYNC命令的回复，返回偏移量。
 * 返回的值仅在BGSAVE进程启动后以及从客户端执行任何其他命令之前立即有效。
 * */
long long getPsyncInitialOffset(void) {
    return server.master_repl_offset;
}

/* Send a FULLRESYNC reply in the specific case of a full resynchronization,
 * as a side effect setup the slave for a full sync in different ways:
 *
 * 1) Remember, into the slave client structure, the replication offset
 *    we sent here, so that if new slaves will later attach to the same
 *    background RDB saving process (by duplicating this client output
 *    buffer), we can get the right offset from this slave.
 * 2) Set the replication state of the slave to WAIT_BGSAVE_END so that
 *    we start accumulating differences from this point.
 * 3) Force the replication stream to re-emit a SELECT statement so
 *    the new slave incremental differences will start selecting the
 *    right database number.
 *
 * Normally this function should be called immediately after a successful
 * BGSAVE for replication was started, or when there is one already in
 * progress that we attached our slave to. 
 *
 * 在重新全量同步的特定情况下，发送FULLRESYNC回复，
 * 作为副作用，以不同的方式设置从节点进行全量同步：
 *
 * 1） 记住，在从节点结构中，我们在这里发送的复制偏移量，这样，
 *     如果新的从节点稍后将连接到相同的后台RDB保存过程（通过复制此客户端输出缓
 *     冲区），我们可以从该从节点获得正确的偏移量。
 *
 * 2） 将从节点的复制状态设置为WAIT_BGSAVE_END，以便我们从此点开始累积差异。
 *
 * 3） 强制复制流重新发出SELECT语句，以便新的从节点增量差异将开始选择正确的数据库编号。通常，此函数应在成
 *     功启动用于复制的BGSAVE之后立即调用，或者当有一个正在进行的函数连接到我们的从节点服务器时。
 * */
int replicationSetupSlaveForFullResync(client *slave, long long offset) {
    char buf[128];
    int buflen;

    slave->psync_initial_offset = offset;
    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_END;
    /* We are going to accumulate the incremental changes for this
     * slave as well. Set slaveseldb to -1 in order to force to re-emit
     * a SELECT statement in the replication stream. 
     *
     * 我们也将为这个从节点积累增量变化。将slaveseldb设置为-1，以便强制在复制
     * 流中重新发出SELECT语句。
     * */
    server.slaveseldb = -1;

    /* Don't send this reply to slaves that approached us with
     * the old SYNC command. 
     *
     * 不要将此回复发送给使用旧SYNC命令接近我们的从节点。
     * */
    // 如果客户端没准备好 PSYNC，发送回复并关闭
    if (!(slave->flags & CLIENT_PRE_PSYNC)) {
        buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                          server.replid,offset);
        if (connWrite(slave->conn,buf,buflen) != buflen) {
            freeClientAsync(slave);
            return C_ERR;
        }
    }
    return C_OK;
}

/* This function handles the PSYNC command from the point of view of a
 * master receiving a request for partial resynchronization.
 *
 * On success return C_OK, otherwise C_ERR is returned and we proceed
 * with the usual full resync. 
 *
 * 此函数从接收部分重新同步请求的主节点的角度处理PSYNC命令。
 * 成功时返回C_OK，否则返回C_ERR，我们继续进行通常的重新全量同步。
 * */
int masterTryPartialResynchronization(client *c // c 是发送数据同步请求的 slave 客户端
) {
    long long psync_offset, psync_len;
    char *master_replid = c->argv[1]->ptr;
    char buf[128];
    int buflen;

    /* Parse the replication offset asked by the slave. Go to full sync
     * on parse error: this should never happen but we try to handle
     * it in a robust way compared to aborting. 
     *
     * 分析从节点请求的复制偏移量。如果分析失败就进行全量同步：
     * 这不应该发生，但与中止相比，我们试图以稳健的方式处理它。
     * */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       C_OK) goto need_full_resync;

    /* Is the replication ID of this master the same advertised by the wannabe
     * slave via PSYNC? If the replication ID changed this master has a
     * different replication history, and there is no way to continue.
     *
     * Note that there are two potentially valid replication IDs: the ID1
     * and the ID2. The ID2 however is only valid up to a specific offset. 
     *
     * 该主服务器的复制ID是否与想成为从节点服务器的用户通过PSYNC发布的相同？如果复
     * 制ID已更改，则此主节点具有不同的复制历史记录，并且无法继续。请注意，有两个可能有
     * 效的复制ID：ID1和ID2。然而，ID2仅在特定偏移量之前有效。
     * */
    
    // 想要同步的主节点就是我自己
    if (strcasecmp(master_replid, server.replid) &&
        
        // 如果从节点和我 在最后一次主从变更前都属于同一个主节点的从节点，并且偏移量在我作为 slave 之后的偏移量
        (strcasecmp(master_replid, server.replid2) ||
         psync_offset > server.second_replid_offset))
    {
        /* Replid "?" is used by slaves that want to force a full resync. 
         *
         * Replid“？”由想要强制重新全量同步的从节点设备使用。
         * */
        if (master_replid[0] != '?') {
            if (strcasecmp(master_replid, server.replid) &&
                strcasecmp(master_replid, server.replid2))
            {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Replication ID mismatch (Replica asked for '%s', my "
                    "replication IDs are '%s' and '%s')",
                    master_replid, server.replid, server.replid2);
            } else {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Requested offset for second ID was %lld, but I can reply "
                    "up to %lld", psync_offset, server.second_replid_offset);
            }
        } else {
            serverLog(LL_NOTICE,"Full resync requested by replica %s",
                replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* We still have the data our slave is asking for? 
     *
     * 我们还有从节点要求的数据吗？
     * */
    if (!server.repl_backlog ||  // backlog 没数据
        psync_offset < server.repl_backlog_off ||  // 偏移量在有效数据开始位置之前
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen)) // 偏移量在有效数据结束位置之后
    {
        serverLog(LL_NOTICE,
            "Unable to partial resync with replica %s for lack of backlog (Replica request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > server.master_repl_offset) {
            serverLog(LL_WARNING,
                "Warning: replica %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 1) Set client state to make it a slave.
     * 2) Inform the client we can continue with +CONTINUE
     * 3) Send the backlog data (from the offset to the end) to the slave. 
     *
     * 如果我们达到了这一点，我们就可以执行「部分同步」：
     *
     * 1）设置客户端状态，使其成为从节点状态。
     *
     * 2）通知客户端我们可以继续+continue
     *
     * 3）将积压数据（从偏移量到末尾）发送到从节点。
     * */
    c->flags |= CLIENT_SLAVE;
    c->replstate = SLAVE_STATE_ONLINE;
    c->repl_ack_time = server.unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(server.slaves,c);
    /* We can't use the connection buffers since they are used to accumulate
     * new commands at this stage. But we are sure the socket send buffer is
     * empty so this write will never fail actually. 
     *
     * 我们不能使用连接缓冲区，因为在这个阶段它们是用来累积新命令的。但我们确信套接字发
     * 送缓冲区是空的，所以这个写操作实际上永远不会失败。
     * */
    if (c->slave_capa & SLAVE_CAPA_PSYNC2) { // 如果支持 PSYNC2
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE %s\r\n", server.replid);
    } else {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    }
    if (connWrite(c->conn,buf,buflen) != buflen) {
        freeClientAsync(c);
        return C_OK;
    }
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    serverLog(LL_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    /* Note that we don't need to set the selected DB at server.slaveseldb
     * to -1 to force the master to emit SELECT, since the slave already
     * has this state from the previous connection with the master. 
     *
     * 请注意，我们不需要将server.slavesldb上的选定DB设置为-1来强制
     * 主节点发出SELECT，因为从节点在与主节点的上一次连接中已经具有这种状态。
     * */

    refreshGoodSlavesCount();

    /* Fire the replica change modules event. 
     *
     * 激发复制从节点更改模块事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                          REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE,
                          NULL);

    return C_OK; /* The caller can return, no full resync needed. 
                  *
                  * 调用方可以返回，不需要重新全量同步。
                  * */

need_full_resync:
    /* We need a full resync for some reason... Note that we can't
     * reply to PSYNC right now if a full SYNC is needed. The reply
     * must include the master offset at the time the RDB file we transfer
     * is generated, so we need to delay the reply to that moment. 
     *
     * 出于某种原因，我们需要重新全量同步。。。请注意，如果需要完整的SYNC，我们现在
     * 无法回复PSYNC。回复必须包括生成我们传输的RDB文件时的主偏移量，因此我们需
     * 要将回复延迟到该时刻。
     * */
    return C_ERR;
}

/* Start a BGSAVE for replication goals, which is, selecting the disk or
 * socket target depending on the configuration, and making sure that
 * the script cache is flushed before to start.
 *
 * The mincapa argument is the bitwise AND among all the slaves capabilities
 * of the slaves waiting for this BGSAVE, so represents the slave capabilities
 * all the slaves support. Can be tested via SLAVE_CAPA_* macros.
 *
 * Side effects, other than starting a BGSAVE:
 *
 * 1) Handle the slaves in WAIT_START state, by preparing them for a full
 *    sync if the BGSAVE was successfully started, or sending them an error
 *    and dropping them from the list of slaves.
 *
 * 2) Flush the Lua scripting script cache if the BGSAVE was actually
 *    started.
 *
 * Returns C_OK on success or C_ERR otherwise. 
 *
 * 为数据同步启动BGSAVE，即根据配置选择类型是磁盘还是套接字，并确保在启动之前刷新脚本缓存。
 *
 * mincapa 参数是等待此BGSAVE的「从节点」的所有「从节点」功能中的按位AND，
 * 因此表示所有从节点支持的从节点功能。可以通过SLAVE_CAPA_* 宏进行测试。
 *
 * 除了启动BGSAVE之外的副作用：
 *
 * 1）在WAIT_START状态下处理从节点，如果BGSAVE成功启动，则为其进行全量同步做好准备，或者向其发送错误并将其
 *    从从节点列表中删除。
 *
 * 2） 如果BGSAVE实际启动，则刷新Lua脚本缓存。成功时返回C_OK，否则返回C_ERR。
 * */
int startBgsaveForReplication(int mincapa) {
    int retval;
    int socket_target = server.repl_diskless_sync && (mincapa & SLAVE_CAPA_EOF);
    listIter li;
    listNode *ln;

    serverLog(LL_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        socket_target ? "replicas sockets" : "disk");

    rdbSaveInfo rsi, *rsiptr;
    // rdbPopulateSaveInfo 主要填充了当前选择同步的数据库的id
    rsiptr = rdbPopulateSaveInfo(&rsi);
    /* Only do rdbSave* when rsiptr is not NULL,
     * otherwise slave will miss repl-stream-db. 
     *
     * 只有在rsiptr不为NULL时才执行rdbSaver，否则slave将丢失repl流数据库。
     * */
    if (rsiptr) {
        if (socket_target)
            retval = rdbSaveToSlavesSockets(rsiptr);
        else
            retval = rdbSaveBackground(server.rdb_filename,rsiptr);
    } else {
        serverLog(LL_WARNING,"BGSAVE for replication: replication information not available, can't generate the RDB file right now. Try later.");
        retval = C_ERR;
    }

    /* If we succeeded to start a BGSAVE with disk target, let's remember
     * this fact, so that we can later delete the file if needed. Note
     * that we don't set the flag to 1 if the feature is disabled, otherwise
     * it would never be cleared: the file is not deleted. This way if
     * the user enables it later with CONFIG SET, we are fine. 
     *
     * 如果我们成功地启动了带有磁盘目标的BGSAVE，让我们记住这个情况，以便以后在需要时删除该文件。
     *
     * 请注意，如果该功能被禁用，我们不会将标志设置为1，否则它将永远不会被清除：文件不会被删除。
     *
     * 这样，如果用户稍后使用CONFIG SET启用它，我们就可以了。
     * */
    if (retval == C_OK && !socket_target && server.rdb_del_sync_files)
        RDBGeneratedByReplication = 1;

    /* If we failed to BGSAVE, remove the slaves waiting for a full
     * resynchronization from the list of slaves, inform them with
     * an error about what happened, close the connection ASAP. 
     *
     * 如果我们未能BGSAVE，请从从节点服务器列表中删除等待重新全量同步的从节点服务器，
     * 向他们通知发生的错误，并尽快关闭连接。
     * */
    if (retval == C_ERR) {
        serverLog(LL_WARNING,"BGSAVE for replication failed");
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                slave->replstate = REPL_STATE_NONE;
                slave->flags &= ~CLIENT_SLAVE;
                listDelNode(server.slaves,ln);
                addReplyError(slave,
                    "BGSAVE failed, replication can't continue");
                slave->flags |= CLIENT_CLOSE_AFTER_REPLY;
            }
        }
        return retval;
    }

    /* If the target is socket, rdbSaveToSlavesSockets() already setup
     * the slaves for a full resync. Otherwise for disk target do it now.
     *
     * 如果目标是套接字，那么rdbSaveToSlavesSockets（）已经为重新全量同步设置了从节点。否则，对于磁盘目标，请立即执行。
     * */
    if (!socket_target) {
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                    replicationSetupSlaveForFullResync(slave,
                            getPsyncInitialOffset());
            }
        }
    }

    /* Flush the script cache, since we need that slave differences are
     * accumulated without requiring slaves to match our cached scripts. 
     *
     * 刷新脚本缓存，因为我们需要在不需要从程序匹配缓存脚本的情况下积累从程序差异。
     * */
    if (retval == C_OK) replicationScriptCacheFlush();
    return retval;
}

/* SYNC and PSYNC command implementation. 
 *
 * SYNC和PSYNC命令实现。
 * */
void syncCommand(client *c) {
    /* ignore SYNC if already slave or in monitor mode 
     *
     * 如果已经从节点或处于监视模式，则忽略SYNC
     * */
    if (c->flags & CLIENT_SLAVE) return;

    /* Refuse SYNC requests if we are a slave but the link with our master
     * is not ok... 
     *
     * 如果我们是从节点，但与主节点的链接不正常，则拒绝SYNC请求。。。
     * */
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED) {
        addReplySds(c,sdsnew("-NOMASTERLINK Can't SYNC while not connected with my master\r\n"));
        return;
    }

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. 
     *
     * 当服务器有关于已发出命令的挂起数据要发送给客户端时，无法发出SYNC。我们需要一
     * 个新的回复缓冲区来记录BGSAVE和当前数据集之间的差异，这样我们就可以在需要时
     * 复制到其他从节点数据集。
     * */
    // 还有数据没发送完给这个客户端
    if (clientHasPendingReplies(c)) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    serverLog(LL_NOTICE,"Replica %s asks for synchronization",
        replicationGetSlaveName(c));

    /* Try a partial resynchronization if this is a PSYNC command.
     * If it fails, we continue with usual full resynchronization, however
     * when this happens masterTryPartialResynchronization() already
     * replied with:
     *
     * +FULLRESYNC <replid> <offset>
     *
     * So the slave knows the new replid and offset to try a PSYNC later
     * if the connection with the master is lost. 
     *
     * 如果这是一个PSYNC命令，请尝试部分重新同步。如果它失败了，我们继续进行通常的
     * 重新全量同步，但是当这种情况发生时，master TryPartialResynchronization（）已经用：
     *
     * +FULLRESYNC＜replid＞＜offset＞
     *
     * 进行了回复。因此，如果与master的连接丢失，从节点知道新的replid和offset，可以稍后尝试PSYNC。
     * */
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        if (masterTryPartialResynchronization(c) == C_OK) {
            server.stat_sync_partial_ok++;
            return; /* No full resync needed, return. 
                     *
                     * 不需要重新全量同步，返回。
                     * */
        } else {
            char *master_replid = c->argv[1]->ptr;

            /* Increment stats for failed PSYNCs, but only if the
             * replid is not "?", as this is used by slaves to force a full
             * resync on purpose when they are not albe to partially
             * resync. 
             *
             * 增加失败的PSYNC的统计信息，但前提是replid不是“？”，因为当从节点设备不
             * 能部分重新同步时，从节点设备会故意使用它来强制进行重新全量同步。
             * */
            if (master_replid[0] != '?') server.stat_sync_partial_err++;
        }
    } else {
        /* If a slave uses SYNC, we are dealing with an old implementation
         * of the replication protocol (like redis-cli --slave). Flag the client
         * so that we don't expect to receive REPLCONF ACK feedbacks. 
         *
         * 如果一个slave使用SYNC，那么我们处理的是复制协议的旧实现（比如redis-cli-slave）。
         * 标记客户端，这样我们就不会收到REPLCONF ACK反馈。
         * */
        c->flags |= CLIENT_PRE_PSYNC;
    }

    /* Full resynchronization. 
     *
     * 重新全量同步。
     * */
    server.stat_sync_full++;

    /* Setup the slave as one waiting for BGSAVE to start. The following code
     * paths will change the state if we handle the slave differently. 
     *
     * 将从节点设置为等待BGSAVE启动的设备。如果我们以不同的方式处理从节点程序，以下
     * 代码路径将更改状态。
     * */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    if (server.repl_disable_tcp_nodelay)
        connDisableTcpNoDelay(c->conn);   /* Non critical if it fails.
                                                 *
                                                 * 如果失败，则为非关键。
                                                 * */
    c->repldbfd = -1;
    c->flags |= CLIENT_SLAVE;
    listAddNodeTail(server.slaves,c);

    /* Create the replication backlog if needed. 
     *
     * 如果需要，请创建同步缓冲区。
     * */
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
        /* When we create the backlog from scratch, we always use a new
         * replication ID and clear the ID2, since there is no valid
         * past history. 
         *
         * 当我们从头开始创建缓冲区时，我们总是使用一个新的复制ID并清除ID2，因为没有有效的过去历史记录。
         * */
        changeReplicationId();
        clearReplicationId2();
        createReplicationBacklog();
        serverLog(LL_NOTICE,"Replication backlog created, my new "
                            "replication IDs are '%s' and '%s'",
                            server.replid, server.replid2);
    }

    /* CASE 1: BGSAVE is in progress, with disk target. 
     *
     * 情况1:BGSAVE正在进行中，具有磁盘目标。
     * */
    // 检查是否已经有 BGSAVE 在执行，否则就创建一个新的 BGSAVE 任务
    if (server.rdb_child_pid != -1 &&
        server.rdb_child_type == RDB_CHILD_TYPE_DISK)
    {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save. 
         *
         * 好的，正在进行后台保存。让我们检查一下它是否适合复制，也就是说，自服务器分叉保存
         * 以来，是否有另一个从节点服务器正在记录差异。
         * */
        // 已有 BGSAVE 在执行，检查它能否用于当前客户端的 SYNC 操作
        client *slave;
        listNode *ln;
        listIter li;

        // 检查是否有其他客户端在等待 SYNC 进行
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) break;
        }
        /* To attach this slave, we check that it has at least all the
         * capabilities of the slave that triggered the current BGSAVE. 
         *
         * 要连接此从节点，我们检查它是否至少具有触发当前BGSAVE的从节点的所有功能。
         * */
        if (ln && ((c->slave_capa & slave->slave_capa) == slave->slave_capa)) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. 
             *
             * 完美，服务器已经在为另一个从节点服务器注册差异。设置正确的状态，然后复制缓冲区。
             * */
            // 找到一个同样在等到 SYNC 的客户端
            // 设置当前客户端的状态，并复制 buffer 。
            copyClientOutputBuffer(c,slave);
            replicationSetupSlaveForFullResync(c,slave->psync_initial_offset);
            serverLog(LL_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences. 
             *
             * 不可能，我们需要等待下一个BGSAVE来登记差异。
             * */
            serverLog(LL_NOTICE,"Can't attach the replica to the current BGSAVE. Waiting for next BGSAVE for SYNC");
        }

    /* CASE 2: BGSAVE is in progress, with socket target. 
     *
     * 情况2:BGSAVE正在进行中，带有套接字目标。
     * */
    } else if (server.rdb_child_pid != -1 &&
               server.rdb_child_type == RDB_CHILD_TYPE_SOCKET)
    {
        /* There is an RDB child process but it is writing directly to
         * children sockets. We need to wait for the next BGSAVE
         * in order to synchronize. 
         *
         * 有一个RDB子进程，但它直接写入子套接字。我们需要等待下一个BGSAVE才能同步
         * 。
         * */
        serverLog(LL_NOTICE,"Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC");

    /* CASE 3: There is no BGSAVE is progress. 
     *
     * 情况3：没有BGSAVE是进度。
     * */
    } else {
        // 没有 BGSAVE 在进行，自己启动一个。
        if (server.repl_diskless_sync && (c->slave_capa & SLAVE_CAPA_EOF)) {
            /* Diskless replication RDB child is created inside
             * replicationCron() since we want to delay its start a
             * few seconds to wait for more slaves to arrive. 
             *
             * 无盘复制RDB子进程是在replicationCron（）中创建的，因为我们希望将
             * 其启动延迟几秒钟，以等待更多的从节点到达。
             * */
            if (server.repl_diskless_sync_delay)
                serverLog(LL_NOTICE,"Delay next BGSAVE for diskless SYNC");
        } else {
            /* Target is disk (or the slave is not capable of supporting
             * diskless replication) and we don't have a BGSAVE in progress,
             * let's start one. 
             *
             * 目标是磁盘（或者从节点服务器无法支持无盘复制），并且我们没有正在进行的BGSAVE，让我们启动一个。
             * */
            if (!hasActiveChildProcess()) {
                startBgsaveForReplication(c->slave_capa);
            } else {
                serverLog(LL_NOTICE,
                    "No BGSAVE in progress, but another BG operation is active. "
                    "BGSAVE for replication delayed");
            }
        }
    }
    return;
}

/* REPLCONF <option> <value> <option> <value> ...
 * This command is used by a slave in order to configure the replication
 * process before starting it with the SYNC command.
 *
 * Currently the only use of this command is to communicate to the master
 * what is the listening port of the Slave redis instance, so that the
 * master can accurately list slaves and their listening ports in
 * the INFO output.
 *
 * In the future the same command can be used in order to configure
 * the replication to initiate an incremental replication instead of a
 * full resync. 
 *
 * REPLCONF＜option＞＜value＞＜option＜value＞。。。
 * 从节点使用此命令，以便在使用SYNC命令启动复制过程之前配置复制过程。目前，该命
 * 令的唯一用途是与主节点通信Slave redis节点的侦听端口，以便主节点可以在INFO输出中准确列出Slave及其侦听端口。
 * 将来可以使用相同的命令来配置复制以启动增量复制，而不是重新全量同步。
 * */
void replconfCommand(client *c) {
    int j;

    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. 
         *
         * 参数的数量必须是奇数，以确保每个选项都有相应的值。
         * */
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. 
     *
     * 处理每个选项值对。
     * */
    for (j = 1; j < c->argc; j+=2) {
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;

            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != C_OK))
                return;
            c->slave_listening_port = port;
        } else if (!strcasecmp(c->argv[j]->ptr,"ip-address")) {
            sds ip = c->argv[j+1]->ptr;
            if (sdslen(ip) < sizeof(c->slave_ip)) {
                memcpy(c->slave_ip,ip,sdslen(ip)+1);
            } else {
                addReplyErrorFormat(c,"REPLCONF ip-address provided by "
                    "replica instance is too long: %zd bytes", sdslen(ip));
                return;
            }
        } else if (!strcasecmp(c->argv[j]->ptr,"capa")) {
            /* Ignore capabilities not understood by this master. 
             *
             * 忽略此主节点不理解的功能。
             * */
            if (!strcasecmp(c->argv[j+1]->ptr,"eof"))
                c->slave_capa |= SLAVE_CAPA_EOF;
            else if (!strcasecmp(c->argv[j+1]->ptr,"psync2"))
                c->slave_capa |= SLAVE_CAPA_PSYNC2;
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) {
            /* REPLCONF ACK is used by slave to inform the master the amount
             * of replication stream that it processed so far. It is an
             * internal only command that normal clients should never use. 
             *
             * REPLCONF ACK由slave用来通知master它迄今为止处理的复制流的
             * 数量。这是一个仅限内部的命令，普通客户端永远不应该使用。
             * */
            long long offset;

            if (!(c->flags & CLIENT_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != C_OK))
                return;
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            c->repl_ack_time = server.unixtime;
            /* If this was a diskless replication, we need to really put
             * the slave online when the first ACK is received (which
             * confirms slave is online and ready to get more data). This
             * allows for simpler and less CPU intensive EOF detection
             * when streaming RDB files.
             * There's a chance the ACK got to us before we detected that the
             * bgsave is done (since that depends on cron ticks), so run a
             * quick check first (instead of waiting for the next ACK. 
             *
             * 如果这是一个无盘复制，我们需要在接收到第一个ACK时真正使从节点联机（这确认了从节点
             * 联机并准备好获取更多数据）。这允许在流式传输RDB文件时进行更简单、CPU占用更
             * 少的EOF检测。在我们检测到bgsave完成之前，ACK有可能到达我们（因为这取
             * 决于cron ticks），所以首先运行快速检查（而不是等待下一个ACK）。
             * */
            if (server.rdb_child_pid != -1 && c->replstate == SLAVE_STATE_WAIT_BGSAVE_END)
                checkChildrenDone();
            if (c->repl_put_online_on_ack && c->replstate == SLAVE_STATE_ONLINE)
                putSlaveOnline(c);
            /* Note: this command does not reply anything! 
             *
             * 注意：此命令不会回复任何内容！
             * */
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the slave. 
             *
             * REPLCONF GETACK用于尽快向从节点请求ACK。
             * */
            if (server.masterhost && server.master) replicationSendAck();
            return;
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}

/* This function puts a replica in the online state, and should be called just
 * after a replica received the RDB file for the initial synchronization, and
 * we are finally ready to send the incremental stream of commands.
 *
 * It does a few things:
 *
 * 1) Put the slave in ONLINE state. Note that the function may also be called
 *    for a replicas that are already in ONLINE state, but having the flag
 *    repl_put_online_on_ack set to true: we still have to install the write
 *    handler in that case. This function will take care of that.
 * 2) Make sure the writable event is re-installed, since calling the SYNC
 *    command disables it, so that we can accumulate output buffer without
 *    sending it to the replica.
 * 3) Update the count of "good replicas". 
 *
 * 这个函数将从节点置于联机状态，并且应该在从节点接收到用于初始同步的RDB文件之后立即
 * 调用，并且我们终于可以发送增量命令流了。
 *
 * 它做了几件事：
 *
 * 1）将从节点服务器置于联机状态。请注意，对于已经处于ONLINE状态但将标志repl_put_ONLINE_on_ack设置为true的从节点，
 * 也可能会调用该函数：在这种情况下，我们仍然需要安装写处理程序。这个函数会处理这个问题。
 *
 * 2）请确保可写事件已重新安装，因为调用
 * SYNC命令会禁用它，这样我们就可以累积输出缓冲区，而无需将其发送到从节点。
 *
 * 3）更新“良好复制从节点”的计数。
 * */
void putSlaveOnline(client *slave) {
    slave->replstate = SLAVE_STATE_ONLINE;
    slave->repl_put_online_on_ack = 0;
    slave->repl_ack_time = server.unixtime; /* Prevent false timeout. 
                                             *
                                             * 防止错误超时。
                                             * */
    if (connSetWriteHandler(slave->conn, sendReplyToClient) == C_ERR) {
        serverLog(LL_WARNING,"Unable to register writable event for replica bulk transfer: %s", strerror(errno));
        freeClient(slave);
        return;
    }
    refreshGoodSlavesCount();
    /* Fire the replica change modules event. 
     *
     * 激发复制从节点更改模块事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                          REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE,
                          NULL);
    serverLog(LL_NOTICE,"Synchronization with replica %s succeeded",
        replicationGetSlaveName(slave));
}

/* We call this function periodically to remove an RDB file that was
 * generated because of replication, in an instance that is otherwise
 * without any persistence. We don't want instances without persistence
 * to take RDB files around, this violates certain policies in certain
 * environments. 
 *
 * 我们周期性地调用这个函数来删除由于复制而生成的RDB文件，否则节点就没有任何持久性。
 * 我们不希望没有持久性的节点携带RDB文件，这违反了某些环境中的某些策略。
 * */
void removeRDBUsedToSyncReplicas(void) {
    /* If the feature is disabled, return ASAP but also clear the
     * RDBGeneratedByReplication flag in case it was set. Otherwise if the
     * feature was enabled, but gets disabled later with CONFIG SET, the
     * flag may remain set to one: then next time the feature is re-enabled
     * via CONFIG SET we have have it set even if no RDB was generated
     * because of replication recently. 
     *
     * 如果该功能被禁用，请返回ASAP，但也要清除RDBGeneratedByReplication标志（如果已设置）。
     * 否则，如果该功能已启用，但稍后使用CONFIG SET禁用，则该标志可能会保持为1：
     * 然后，下次通过CONFIG SET重新启用该功能时，即使最近由于复制而没有生成RDB，我们也会将其设置为1。
     * */
    if (!server.rdb_del_sync_files) {
        RDBGeneratedByReplication = 0;
        return;
    }

    if (allPersistenceDisabled() && RDBGeneratedByReplication) {
        client *slave;
        listNode *ln;
        listIter li;

        int delrdb = 1;
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
                slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END ||
                slave->replstate == SLAVE_STATE_SEND_BULK)
            {
                delrdb = 0;
                break; /* No need to check the other replicas. 
                        *
                        * 无需检查其他复制从节点。
                        * */
            }
        }
        if (delrdb) {
            struct stat sb;
            if (lstat(server.rdb_filename,&sb) != -1) {
                RDBGeneratedByReplication = 0;
                serverLog(LL_NOTICE,
                    "Removing the RDB file used to feed replicas "
                    "in a persistence-less instance");
                bg_unlink(server.rdb_filename);
            }
        }
    }
}

/*
 * 将主节点的 .rdb 文件内容发送到附属节点
 *
 * 每次最大发送的字节数量有 REDIS_IOBUF_LEN = 默认16k 决定，
 * 根据文件的大小和服务器的状态，整个发送过程可能会执行多次
 */
void sendBulkToSlave(connection *conn) {
    client *slave = connGetPrivateData(conn);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* Before sending the RDB file, we send the preamble as configured by the
     * replication process. Currently the preamble is just the bulk count of
     * the file in the form "$<length>\r\n". 
     *
     * 在发送RDB文件之前，我们按照复制过程的配置发送前导码。目前，前导码只是文件的批
     * 量计数，格式为“$<length>\r”。
     * */
    if (slave->replpreamble) {
        nwritten = connWrite(conn,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) {
            serverLog(LL_VERBOSE,
                "Write error sending RDB preamble to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
            return;
        }
        server.stat_net_output_bytes += nwritten;
        sdsrange(slave->replpreamble,nwritten,-1);
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
            /* fall through sending data. 
             *
             * 发送数据失败。
             * */
        } else {
            return;
        }
    }

    /* If the preamble was already transferred, send the RDB bulk data. 
     *
     * 如果前导码已经传输，则发送RDB批量数据。
     * */
    // 设置主节点 .rdb 文件的偏移量
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    // 读取主节点 .rdb 文件的数据到 buf
    buflen = read(slave->repldbfd,buf,PROTO_IOBUF_LEN);
    if (buflen <= 0) {
        // 主节点 .rdb 文件读取错误，返回
        serverLog(LL_WARNING,"Read error sending DB to replica: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    // 将 buf 发送给附属节点
    if ((nwritten = connWrite(conn,buf,buflen)) == -1) {
        if (connGetState(conn) != CONN_STATE_CONNECTED) {
            // 附属节点写入出错，返回
            serverLog(LL_WARNING,"Write error sending DB to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
        }
        return;
    }
    // 更新偏移量
    slave->repldboff += nwritten;
    server.stat_net_output_bytes += nwritten;
    // .rdb 文件全部发送完毕
    if (slave->repldboff == slave->repldbsize) {
        // 关闭 .rdb 文件
        close(slave->repldbfd);
        // 重置
        slave->repldbfd = -1;
        connSetWriteHandler(slave->conn,NULL);
        putSlaveOnline(slave);
    }
}

/* Remove one write handler from the list of connections waiting to be writable
 * during rdb pipe transfer. 
 *
 * 从rdb管道传输期间等待写入的连接列表中删除一个写入处理程序。
 * */
void rdbPipeWriteHandlerConnRemoved(struct connection *conn) {
    if (!connHasWriteHandler(conn))
        return;
    connSetWriteHandler(conn, NULL);
    client *slave = connGetPrivateData(conn);
    slave->repl_last_partial_write = 0;
    server.rdb_pipe_numconns_writing--;
    /* if there are no more writes for now for this conn, or write error: 
     *
     * 如果此conn目前没有更多写入操作，或写入错误：
     * */
    if (server.rdb_pipe_numconns_writing == 0) {
        if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
            serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
        }
    }
}

/* Called in diskless master during transfer of data from the rdb pipe, when
 * the replica becomes writable again. 
 *
 * 在rdb socket 传输数据期间，当从节点再次变为可写时，在 diskless 主节点中调用。
 * */
void rdbPipeWriteHandler(struct connection *conn) {
    serverAssert(server.rdb_pipe_bufflen>0);
    client *slave = connGetPrivateData(conn);
    int nwritten;
    if ((nwritten = connWrite(conn, server.rdb_pipe_buff + slave->repldboff,
                              server.rdb_pipe_bufflen - slave->repldboff)) == -1)
    {
        if (connGetState(conn) == CONN_STATE_CONNECTED)
            return; /* equivalent to EAGAIN 
                     *
                     * 相当于EAGAIN
                     * */
        serverLog(LL_WARNING,"Write error sending DB to replica: %s",
            connGetLastError(conn));
        freeClient(slave);
        return;
    } else {
        slave->repldboff += nwritten;
        server.stat_net_output_bytes += nwritten;
        // 如果还没写完，那就是标记了一下只是写了部分
        if (slave->repldboff < server.rdb_pipe_bufflen) {
            slave->repl_last_partial_write = server.unixtime;
            return; /* more data to write.. 
                     *
                     * 要写入的更多数据。。
                     * */
        }
    }
    rdbPipeWriteHandlerConnRemoved(conn);
}

/* Called in diskless master, when there's data to read from the child's rdb pipe 
 *
 * 当有数据要从子进程的rdb管道读取时，在 diskless 主节点中调用
 * */
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    UNUSED(mask);
    UNUSED(clientData);
    UNUSED(eventLoop);
    int i;
    if (!server.rdb_pipe_buff)
        server.rdb_pipe_buff = zmalloc(PROTO_IOBUF_LEN); // PROTO_IOBUF_LEN = 16K
    serverAssert(server.rdb_pipe_numconns_writing==0);

    while (1) {
        server.rdb_pipe_bufflen = read(fd, server.rdb_pipe_buff, PROTO_IOBUF_LEN);

        // 如果读异常，关闭 rdb_pipe_conns 上的客户端链接，关闭RDB子进程
        if (server.rdb_pipe_bufflen < 0) {
            // errno 其实是个 #define，实际是个函数调用
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            serverLog(LL_WARNING,"Diskless rdb transfer, read error sending DB to replicas: %s", strerror(errno));
            for (i=0; i < server.rdb_pipe_numconns; i++) {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                client *slave = connGetPrivateData(conn);
                freeClient(slave);
                server.rdb_pipe_conns[i] = NULL;
            }
            killRDBChild();
            return;
        }

        if (server.rdb_pipe_bufflen == 0) {
            /* EOF - write end was closed. 
             *
             * EOF-写入结束已关闭。
             * */
            int stillUp = 0;
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            for (i=0; i < server.rdb_pipe_numconns; i++)
            {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                stillUp++;
            }
            serverLog(LL_WARNING,"Diskless rdb transfer, done reading from pipe, %d replicas still up.", stillUp);
            /* Now that the replicas have finished reading, notify the child that it's safe to exit. 
             * When the server detectes the child has exited, it can mark the replica as online, and
             * start streaming the replication buffers. 
             *
             * 现在从节点已经完成读取，请通知子进程可以安全退出。当服务器检测到子进程已退出时，它可以
             * 将复制从节点标记为联机，并开始流式传输复制缓冲区。
             * */
            close(server.rdb_child_exit_pipe);
            server.rdb_child_exit_pipe = -1;
            return;
        }

        int stillAlive = 0;
        for (i=0; i < server.rdb_pipe_numconns; i++)
        {
            int nwritten;
            connection *conn = server.rdb_pipe_conns[i];
            if (!conn)
                continue;

            client *slave = connGetPrivateData(conn);
            if ((nwritten = connWrite(conn, server.rdb_pipe_buff, server.rdb_pipe_bufflen)) == -1) {
                if (connGetState(conn) != CONN_STATE_CONNECTED) {
                    serverLog(LL_WARNING,"Diskless rdb transfer, write error sending DB to replica: %s",
                        connGetLastError(conn));
                    freeClient(slave);
                    server.rdb_pipe_conns[i] = NULL;
                    continue;
                }
                /* An error and still in connected state, is equivalent to EAGAIN 
                 *
                 * 错误且仍处于连接状态，相当于EAGAIN
                 * */
                slave->repldboff = 0;
            } else {
                slave->repldboff = nwritten;
                server.stat_net_output_bytes += nwritten;
            }
            /* If we were unable to write all the data to one of the replicas,
             * setup write handler (and disable pipe read handler, below) 
             *
             * 如果我们无法将所有数据写入其中一个从节点，请设置写入处理程序（并禁用管道读取处理程
             * 序，如下所示）
             * */
            if (nwritten != server.rdb_pipe_bufflen) {
                slave->repl_last_partial_write = server.unixtime;
                server.rdb_pipe_numconns_writing++;
                connSetWriteHandler(conn, rdbPipeWriteHandler);
            }
            stillAlive++;
        }

        if (stillAlive == 0) {
            serverLog(LL_WARNING,"Diskless rdb transfer, last replica dropped, killing fork child.");
            killRDBChild();
        }
        /*  Remove the pipe read handler if at least one write handler was set. 
         *
         * 如果至少设置了一个写入处理程序，请删除管道读取处理程序。
         * */
        if (server.rdb_pipe_numconns_writing || stillAlive == 0) {
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            break;
        }
    }
}

/* This function is called at the end of every background saving,
 * or when the replication RDB transfer strategy is modified from
 * disk to socket or the other way around.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization, and
 * to schedule a new BGSAVE if there are slaves that attached while a
 * BGSAVE was in progress, but it was not a good one for replication (no
 * other slave was accumulating differences).
 *
 * The argument bgsaveerr is C_OK if the background saving succeeded
 * otherwise C_ERR is passed to the function.
 *
 * 如果 BGSAVE 执行成功，那么 bgsaveerr 参数的值为 REDIS_OK ，
 * 否则为 REDIS_ERR 。
 *
 * The 'type' argument is the type of the child that terminated
 * (if it had a disk or socket target).
 *
 *
 * */
void updateSlavesWaitingBgsave(int bgsaveerr, int type) {
    listNode *ln;
    int startbgsave = 0;
    int mincapa = -1;
    listIter li;

    /* Note: there's a chance we got here from within the REPLCONF ACK command
     * so we must avoid using freeClient, otherwise we'll crash on our way up. 
     *
     * 注意：我们可能是从REPLCONF ACK命令中得到的，所以我们必须避免使用fr
     * eeClient，否则我们会在上升的过程中崩溃。
     * */

    // 遍历所有附属节点
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            // 告诉那些这次不能同步的客户端，可以等待下次 BGSAVE 了。
            startbgsave = 1;
            mincapa = (mincapa == -1) ? slave->slave_capa :
                                        (mincapa & slave->slave_capa);
        } else if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            // 这些是本次可以同步的客户端
            struct redis_stat buf;

            // 如果 BGSAVE 失败，释放 slave 节点
            if (bgsaveerr != C_OK) {
                freeClientAsync(slave);
                serverLog(LL_WARNING,"SYNC failed. BGSAVE child returned an error");
                continue;
            }

            /* If this was an RDB on disk save, we have to prepare to send
             * the RDB from disk to the slave socket. Otherwise if this was
             * already an RDB -> Slaves socket transfer, used in the case of
             * diskless replication, our work is trivial, we can just put
             * the slave online. 
             *
             * 如果这是磁盘上的RDB保存，我们必须准备将RDB从磁盘发送到从节点套接字。否则，如
             * 果这已经是一个RDB->Slaves套接字传输，在无盘复制的情况下使用，我们的工
             * 作是琐碎的，我们可以直接将Slaves联机。
             * */
            if (type == RDB_CHILD_TYPE_SOCKET) {
                serverLog(LL_NOTICE,
                    "Streamed RDB transfer with replica %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                        replicationGetSlaveName(slave));
                /* Note: we wait for a REPLCONF ACK message from the replica in
                 * order to really put it online (install the write handler
                 * so that the accumulated data can be transferred). However
                 * we change the replication state ASAP, since our slave
                 * is technically online now.
                 *
                 * So things work like that:
                 *
                 * 1. We end trasnferring the RDB file via socket.
                 * 2. The replica is put ONLINE but the write handler
                 *    is not installed.
                 * 3. The replica however goes really online, and pings us
                 *    back via REPLCONF ACK commands.
                 * 4. Now we finally install the write handler, and send
                 *    the buffers accumulated so far to the replica.
                 *
                 * But why we do that? Because the replica, when we stream
                 * the RDB directly via the socket, must detect the RDB
                 * EOF (end of file), that is a special random string at the
                 * end of the RDB (for streamed RDBs we don't know the length
                 * in advance). Detecting such final EOF string is much
                 * simpler and less CPU intensive if no more data is sent
                 * after such final EOF. So we don't want to glue the end of
                 * the RDB trasfer with the start of the other replication
                 * data. 
                 *
                 * 注意：我们等待来自复制从节点的REPLCONF ACK消息，以便真正将其联机（安装
                 * 写处理程序，以便可以传输累积的数据）。然而，我们会尽快更改复制状态，因为我们的从
                 * 属服务器现在在技术上是在线的。所以事情是这样运作的：
                 * 
                 * 1。我们结束了通过套接字传输RDB文件。
                 * 
                 * 2.复制从节点处于联机状态，但未安装写入处理程序。
                 * 
                 * 3.然而，复制从节点会真正联机，并通过REPLCONF ACK命令将我们ping回来。
                 * 
                 * 4.现在我们终于安装了写处理程序，并将迄今为止累积的缓冲区发送到从节点。
                 * 
                 * 但我们为什么这么做？因为当
                 * 我们通过套接字直接流式传输RDB时，从节点必须检测RDB EOF（文件末尾），这是
                 * RDB末尾的一个特殊随机字符串（对于流式传输的RDB，我们事先不知道长度）。如果
                 * 在这样的最终EOF之后没有发送更多的数据，则检测这样的最终的EOF串要简单得多并
                 * 且CPU密集度低。因此，我们不想将RDB传输的结束与其他复制数据的开始粘在一起。
                 * */
                slave->replstate = SLAVE_STATE_ONLINE;
                slave->repl_put_online_on_ack = 1;
                slave->repl_ack_time = server.unixtime; /* Timeout otherwise. 
                                                         *
                                                         * 否则超时。
                                                         * */
            } else {
                if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(slave->repldbfd,&buf) == -1) {
                    freeClientAsync(slave);
                    serverLog(LL_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                // 偏移量
                slave->repldboff = 0;
                // 数据库大小（.rdb 文件的大小）
                slave->repldbsize = buf.st_size;
                // 状态
                slave->replstate = SLAVE_STATE_SEND_BULK;
                slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                    (unsigned long long) slave->repldbsize);

                connSetWriteHandler(slave->conn,NULL);
                if (connSetWriteHandler(slave->conn,sendBulkToSlave) == C_ERR) {
                    freeClientAsync(slave);
                    continue;
                }
            }
        }
    }
    if (startbgsave) startBgsaveForReplication(mincapa);
}

/* Change the current instance replication ID with a new, random one.
 * This will prevent successful PSYNCs between this master and other
 * slaves, so the command should be called when something happens that
 * alters the current story of the dataset. 
 *
 * 使用新的随机节点复制ID更改当前节点复制ID。这将阻止该主服务器和其他从节点服务器
 * 之间成功的PSYNC，因此当发生改变数据集当前情况的事情时，应该调用该命令。
 * */
void changeReplicationId(void) {
    getRandomHexChars(server.replid,CONFIG_RUN_ID_SIZE);
    server.replid[CONFIG_RUN_ID_SIZE] = '\0';
}

/* Clear (invalidate) the secondary replication ID. This happens, for
 * example, after a full resynchronization, when we start a new replication
 * history. 
 *
 * 清除（使）辅助复制ID无效。例如，在重新全量同步之后，当我们启动新的复制历史记录
 * 时，会发生这种情况。
 * */
void clearReplicationId2(void) {
    memset(server.replid2,'0',sizeof(server.replid));
    server.replid2[CONFIG_RUN_ID_SIZE] = '\0';
    server.second_replid_offset = -1;
}

/* Use the current replication ID / offset as secondary replication
 * ID, and change the current one in order to start a new history.
 * This should be used when an instance is switched from slave to master
 * so that it can serve PSYNC requests performed using the master
 * replication ID. 
 *
 * 使用当前复制ID/偏移量作为辅助复制ID，然后更改当前ID以启动新的历史记录。当
 * 节点从从节点节点切换到主节点时，应使用此选项，以便它可以为使用主复制ID执行的PSYNC请求提供服务。
 * */
void shiftReplicationId(void) {
    memcpy(server.replid2,server.replid,sizeof(server.replid));
    /* We set the second replid offset to the master offset + 1, since
     * the slave will ask for the first byte it has not yet received, so
     * we need to add one to the offset: for example if, as a slave, we are
     * sure we have the same history as the master for 50 bytes, after we
     * are turned into a master, we can accept a PSYNC request with offset
     * 51, since the slave asking has the same history up to the 50th
     * byte, and is asking for the new bytes starting at offset 51. 
     *
     * 我们将第二个replid偏移量设置为master偏移量+1，因为slave会请求
     * 它尚未接收的第一个字节，所以我们需要在偏移量上加一个：例如，如果作为slave，
     * 我们确信我们与master有50个字节的相同历史，在我们变成master后，我们
     * 可以接受偏移量为51的PSYNC请求，因为从请求具有直到第50字节的相同历史并且
     * 正在请求从偏移51开始的新字节。
     * */
    server.second_replid_offset = server.master_repl_offset+1;
    changeReplicationId();
    serverLog(LL_WARNING,"Setting secondary replication ID to %s, valid up to offset: %lld. New replication ID is %s", server.replid2, server.second_replid_offset, server.replid);
}

/* ----------------------------------- SLAVE --------------------------------*/

/* Returns 1 if the given replication state is a handshake state,
 * 0 otherwise. 
 *
 * 如果给定的复制状态是握手状态，则返回1，否则返回0。
 * */
int slaveIsInHandshakeState(void) {
    return server.repl_state >= REPL_STATE_RECEIVE_PONG &&
           server.repl_state <= REPL_STATE_RECEIVE_PSYNC;
}

/* Avoid the master to detect the slave is timing out while loading the
 * RDB file in initial synchronization. We send a single newline character
 * that is valid protocol but is guaranteed to either be sent entirely or
 * not, since the byte is indivisible.
 *
 * The function is called in two contexts: while we flush the current
 * data with emptyDb(), and while we load the new data received as an
 * RDB file from the master. 
 *
 * 在初始同步中加载RDB文件时，避免主节点检测到从节点超时。我们发送一个换行符，它是有
 * 效的协议，但由于字节是不可分割的，因此可以保证完全发送或不发送。该函数在两个上下
 * 文中被调用：当我们用emptyDb（）刷新当前数据时，以及当我们加载作为RDB文
 * 件从master接收的新数据时。
 * */
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        /* Pinging back in this stage is best-effort. 
         *
         * 回到这个阶段是最好的努力。
         * */
        if (server.repl_transfer_s) connWrite(server.repl_transfer_s, "\n", 1);
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master. 
 *
 * 清空旧数据以加载主节点接收的新数据集时，emptyDb（）使用的回调。
 * */
void replicationEmptyDbCallback(void *privdata) {
    UNUSED(privdata);
    if (server.repl_state == REPL_STATE_TRANSFER)
        replicationSendNewlineToMaster();
}

/* Once we have a link with the master and the synchronization was
 * performed, this function materializes the master client we store
 * at server.master, starting from the specified file descriptor. 
 *
 * 一旦我们与master建立了链接并执行了同步，这个函数就会从指定的文件描述符开始，具体化我们存储在server.master中的master客户端。
 * */
void replicationCreateMasterClient(connection *conn, int dbid) {
    server.master = createClient(conn);
    if (conn)
        connSetReadHandler(server.master->conn, readQueryFromClient);
    server.master->flags |= CLIENT_MASTER;
    server.master->authenticated = 1;
    server.master->reploff = server.master_initial_offset;
    server.master->read_reploff = server.master->reploff;
    server.master->user = NULL; /* This client can do everything. 
                                 *
                                 * 这个客户可以做任何事情。
                                 * */
    memcpy(server.master->replid, server.master_replid,
        sizeof(server.master_replid));
    /* If master offset is set to -1, this master is old and is not
     * PSYNC capable, so we flag it accordingly. 
     *
     * 如果主偏移设置为-1，则该主偏移是旧的，并且不支持PSYNC，因此我们相应地对其
     * 进行标记。
     * */
    if (server.master->reploff == -1)
        server.master->flags |= CLIENT_PRE_PSYNC;
    if (dbid != -1) selectDb(server.master,dbid);
}

/* This function will try to re-enable the AOF file after the
 * master-replica synchronization: if it fails after multiple attempts
 * the replica cannot be considered reliable and exists with an
 * error. 
 *
 * 此功能将尝试在主从节点同步后重新启用AOF文件：如果多次尝试后失败，则无法认为从节点
 * 是可靠的，并且存在错误。
 * */
void restartAOFAfterSYNC() {
    unsigned int tries, max_tries = 10;
    for (tries = 0; tries < max_tries; ++tries) {
        if (startAppendOnly() == C_OK) break;
        serverLog(LL_WARNING,
            "Failed enabling the AOF after successful master synchronization! "
            "Trying it again in one second.");
        sleep(1);
    }
    if (tries == max_tries) {
        serverLog(LL_WARNING,
            "FATAL: this replica instance finished the synchronization with "
            "its master, but the AOF can't be turned on. Exiting now.");
        exit(1);
    }
}

static int useDisklessLoad() {
    /* compute boolean decision to use diskless load 
     *
     * 使用无盘加载的计算布尔决策
     * */
    int enabled = server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB ||
           (server.repl_diskless_load == REPL_DISKLESS_LOAD_WHEN_DB_EMPTY && dbTotalServerKeyCount()==0);
    /* Check all modules handle read errors, otherwise it's not safe to use diskless load. 
     *
     * 检查所有模块是否处理读取错误，否则使用无盘加载是不安全的。
     * */
    if (enabled && !moduleAllDatatypesHandleErrors()) {
        serverLog(LL_WARNING,
            "Skipping diskless-load because there are modules that don't handle read errors.");
        enabled = 0;
    }
    return enabled;
}

/* Helper function for readSyncBulkPayload() to make backups of the current
 * databases before socket-loading the new ones. The backups may be restored
 * by disklessLoadRestoreBackup or freed by disklessLoadDiscardBackup later. 
 *
 * readSyncBulkPayload（）的Helper函数，用于在套接字加载新
 * 数据库之前备份当前数据库。备份可以由disklessLoadRestoreBackup还原，也可以稍后由diskless LoadDiscardBackup释放
 * */
dbBackup *disklessLoadMakeBackup(void) {
    return backupDb();
}

/* Helper function for readSyncBulkPayload(): when replica-side diskless
 * database loading is used, Redis makes a backup of the existing databases
 * before loading the new ones from the socket.
 *
 * If the socket loading went wrong, we want to restore the old backups
 * into the server databases. 
 *
 * readSyncBulkPayload（）的Helper函数：当使用从节点端无盘数
 * 据库加载时，Redis会先备份现有数据库，然后再从套接字加载新数据库。如果套接字
 * 加载出错，我们希望将旧备份恢复到服务器数据库中。
 * */
void disklessLoadRestoreBackup(dbBackup *buckup) {
    restoreDbBackup(buckup);
}

/* Helper function for readSyncBulkPayload() to discard our old backups
 * when the loading succeeded. 
 *
 * readSyncBulkPayload（）的Helper函数，以便在加载成功时丢
 * 弃旧备份。
 * */
void disklessLoadDiscardBackup(dbBackup *buckup, int flag) {
    discardDbBackup(buckup, flag, replicationEmptyDbCallback);
}

/* Asynchronously read the SYNC payload we receive from a master 
 *
 * 异步读取我们从主节点接收的SYNC负载
 * */
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB 
                                                     *
                                                     * 800万
                                                     * */
void readSyncBulkPayload(connection *conn) {
    char buf[PROTO_IOBUF_LEN];
    ssize_t nread, readlen, nwritten;
    int use_diskless_load = useDisklessLoad();
    dbBackup *diskless_load_backup = NULL;
    int empty_db_flags = server.repl_slave_lazy_flush ? EMPTYDB_ASYNC :
                                                        EMPTYDB_NO_FLAGS;
    off_t left;

    /* Static vars used to hold the EOF mark, and the last bytes received
     * from the server: when they match, we reached the end of the transfer. 
     *
     * 静态变量用于保存EOF标记和从服务器接收的最后一个字节：当它们匹配时，我们到达传
     * 输的末尾。
     * */
    static char eofmark[CONFIG_RUN_ID_SIZE];
    static char lastbytes[CONFIG_RUN_ID_SIZE];
    static int usemark = 0;

    /* If repl_transfer_size == -1 we still have to read the bulk length
     * from the master reply. 
     *
     * 如果repl_transfer_size==-1，我们仍然需要从主回复中读取批量
     * 长度。
     * */
    if (server.repl_transfer_size == -1) {
        if (connSyncReadLine(conn,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        }

        if (buf[0] == '-') {
            serverLog(LL_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') {
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. 
             *
             * 在这个阶段，只有一条换行符作为PING，以便使连接生效。因此，我们刷新上次交互时
             * 间戳。
             * */
            server.repl_transfer_lastio = server.unixtime;
            return;
        } else if (buf[0] != '$') {
            serverLog(LL_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* There are two possible forms for the bulk payload. One is the
         * usual $<count> bulk format. The other is used for diskless transfers
         * when the master does not know beforehand the size of the file to
         * transfer. In the latter case, the following format is used:
         *
         * $EOF:<40 bytes delimiter>
         *
         * At the end of the file the announced delimiter is transmitted. The
         * delimiter is long and random enough that the probability of a
         * collision with the actual file content can be ignored. 
         *
         * 批量有效载荷有两种可能的形式。一种是常见的$<count>批量格式。另一个用于无
         * 盘传输，当主节点事先不知道要传输的文件的大小时。在后一种情况下，使用以下格式：
         * 
         * $EOF:<40 bytes delimiter>
         * 
         * 在文件的末尾传输所宣布的分隔符。分隔符足够长且随机，因此
         * 可以忽略与实际文件内容发生冲突的概率。
         * */
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= CONFIG_RUN_ID_SIZE) {
            usemark = 1;
            memcpy(eofmark,buf+5,CONFIG_RUN_ID_SIZE);
            memset(lastbytes,0,CONFIG_RUN_ID_SIZE);
            /* Set any repl_transfer_size to avoid entering this code path
             * at the next call. 
             *
             * 设置任何repl_transfer_size以避免在下次调用时输入此代码路径。
             * */
            server.repl_transfer_size = 0;
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving streamed RDB from master with EOF %s",
                use_diskless_load? "to parser":"to disk");
        } else {
            usemark = 0;
            server.repl_transfer_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving %lld bytes from master %s",
                (long long) server.repl_transfer_size,
                use_diskless_load? "to parser":"to disk");
        }
        return;
    }

    if (!use_diskless_load) {
        /* Read the data from the socket, store it to a file and search
         * for the EOF. 
         *
         * 从套接字读取数据，将其存储到文件中，然后搜索EOF。
         * */
        if (usemark) {
            readlen = sizeof(buf);
        } else {
            left = server.repl_transfer_size - server.repl_transfer_read;
            readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
        }

        nread = connRead(conn,buf,readlen);
        if (nread <= 0) {
            if (connGetState(conn) == CONN_STATE_CONNECTED) {
                /* equivalent to EAGAIN 
                     *
                     * 相当于EAGAIN
                     * */
                return;
            }
            serverLog(LL_WARNING,"I/O error trying to sync with MASTER: %s",
                (nread == -1) ? strerror(errno) : "connection lost");
            cancelReplicationHandshake();
            return;
        }
        server.stat_net_input_bytes += nread;

        /* When a mark is used, we want to detect EOF asap in order to avoid
         * writing the EOF mark into the file... 
         *
         * 当使用标记时，我们希望尽快检测EOF，以避免将EOF标记写入文件。。。
         * */
        int eof_reached = 0;

        if (usemark) {
            /* Update the last bytes array, and check if it matches our
             * delimiter. 
             *
             * 更新最后一个字节数组，并检查它是否与我们的分隔符匹配。
             * */
            if (nread >= CONFIG_RUN_ID_SIZE) {
                memcpy(lastbytes,buf+nread-CONFIG_RUN_ID_SIZE,
                       CONFIG_RUN_ID_SIZE);
            } else {
                int rem = CONFIG_RUN_ID_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,CONFIG_RUN_ID_SIZE) == 0)
                eof_reached = 1;
        }

        /* Update the last I/O time for the replication transfer (used in
         * order to detect timeouts during replication), and write what we
         * got from the socket to the dump file on disk. 
         *
         * 更新复制传输的最后一次I/O时间（用于检测复制期间的超时），并将从套接字获得的内
         * 容写入磁盘上的转储文件。
         * */
        server.repl_transfer_lastio = server.unixtime;
        if ((nwritten = write(server.repl_transfer_fd,buf,nread)) != nread) {
            serverLog(LL_WARNING,
                "Write error or short write writing to the DB dump file "
                "needed for MASTER <-> REPLICA synchronization: %s",
                (nwritten == -1) ? strerror(errno) : "short write");
            goto error;
        }
        server.repl_transfer_read += nread;

        /* Delete the last 40 bytes from the file if we reached EOF. 
         *
         * 如果达到EOF，请删除文件中的最后40个字节。
         * */
        if (usemark && eof_reached) {
            if (ftruncate(server.repl_transfer_fd,
                server.repl_transfer_read - CONFIG_RUN_ID_SIZE) == -1)
            {
                serverLog(LL_WARNING,
                    "Error truncating the RDB file received from the master "
                    "for SYNC: %s", strerror(errno));
                goto error;
            }
        }

        /* Sync data on disk from time to time, otherwise at the end of the
         * transfer we may suffer a big delay as the memory buffers are copied
         * into the actual disk. 
         *
         * 不时在磁盘上同步数据，否则在传输结束时，由于内存缓冲区被复制到实际磁盘中，我们可
         * 能会遇到很大的延迟。
         * */
        if (server.repl_transfer_read >=
            server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
        {
            off_t sync_size = server.repl_transfer_read -
                              server.repl_transfer_last_fsync_off;
            rdb_fsync_range(server.repl_transfer_fd,
                server.repl_transfer_last_fsync_off, sync_size);
            server.repl_transfer_last_fsync_off += sync_size;
        }

        /* Check if the transfer is now complete 
         *
         * 检查传输现在是否完成
         * */
        if (!usemark) {
            if (server.repl_transfer_read == server.repl_transfer_size)
                eof_reached = 1;
        }

        /* If the transfer is yet not complete, we need to read more, so
         * return ASAP and wait for the handler to be called again. 
         *
         * 如果传输尚未完成，我们需要读取更多信息，因此请尽快返回并等待再次调用处理程序。
         * */
        if (!eof_reached) return;
    }

    /* We reach this point in one of the following cases:
     *
     * 1. The replica is using diskless replication, that is, it reads data
     *    directly from the socket to the Redis memory, without using
     *    a temporary RDB file on disk. In that case we just block and
     *    read everything from the socket.
     *
     * 2. Or when we are done reading from the socket to the RDB file, in
     *    such case we want just to read the RDB file in memory. 
     *
     * 我们在以下情况之一中达到了这一点：
     * 
     * 1。复制从节点使用无盘复制，也就是说，它直接从套
     * 接字读取数据到Redis内存，而不使用磁盘上的临时RDB文件。在这种情况下，我们
     * 只是阻塞并读取套接字中的所有内容。
     * 
     * 2.或者，当我们完成了从套接字到RDB文件的读
     * 取时，在这种情况下，我们只想读取内存中的RDB文件。
     * */
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Flushing old data");

    /* We need to stop any AOF rewriting child before flusing and parsing
     * the RDB, otherwise we'll create a copy-on-write disaster. 
     *
     * 在刷新和解析RDB之前，我们需要停止任何AOF重写子进程，否则我们将创建一个写时拷贝灾难。
     * */
    if (server.aof_state != AOF_OFF) stopAppendOnly();

    /* When diskless RDB loading is used by replicas, it may be configured
     * in order to save the current DB instead of throwing it away,
     * so that we can restore it in case of failed transfer. 
     *
     * 当从节点使用无盘RDB加载时，可以对其进行配置，以保存当前数据库，而不是将其丢弃，
     * 这样我们就可以在传输失败的情况下恢复它。
     * */
    if (use_diskless_load &&
        server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB)
    {
        /* Create a backup of server.db[] and initialize to empty
         * dictionaries. 
         *
         * 创建server.db[]的备份并初始化为空字典。
         * */
        diskless_load_backup = disklessLoadMakeBackup();
    }
    /* We call to emptyDb even in case of REPL_DISKLESS_LOAD_SWAPDB
     * (Where disklessLoadMakeBackup left server.db empty) because we
     * want to execute all the auxiliary logic of emptyDb (Namely,
     * fire module events) 
     *
     * 即使在REPL_DISKLES_LOAD_SWAPDB（其中disklessLoadMakeBackup使server.db为空）的情况下，
     * 我们也会调用emptyDb，因为我们希望执行emptyDb的所有辅助逻辑（即激发模块事件）
     * */
    emptyDb(-1,empty_db_flags,replicationEmptyDbCallback);

    /* Before loading the DB into memory we need to delete the readable
     * handler, otherwise it will get called recursively since
     * rdbLoad() will call the event loop to process events from time to
     * time for non blocking loading. 
     *
     * 在将DB加载到内存中之前，我们需要删除可读处理程序，否则它将被递归调用，因为rdbLoad（）将调用事件循环来不时处理事件以进行非阻塞加载。
     * */
    connSetReadHandler(conn, NULL);
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Loading DB in memory");
    rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
    if (use_diskless_load) {
        rio rdb;
        rioInitWithConn(&rdb,conn,server.repl_transfer_size);

        /* Put the socket in blocking mode to simplify RDB transfer.
         * We'll restore it when the RDB is received. 
         *
         * 将套接字置于阻塞模式以简化RDB传输。我们将在收到RDB时恢复它。
         * */
        connBlock(conn);
        connRecvTimeout(conn, server.repl_timeout*1000);
        startLoading(server.repl_transfer_size, RDBFLAGS_REPLICATION);

        if (rdbLoadRio(&rdb,RDBFLAGS_REPLICATION,&rsi) != C_OK) {
            /* RDB loading failed. 
             *
             * RDB加载失败。
             * */
            serverLog(LL_WARNING,
                      "Failed trying to load the MASTER synchronization DB "
                      "from socket: %s", strerror(errno));
            stopLoading(0);
            cancelReplicationHandshake();
            rioFreeConn(&rdb, NULL);

            /* Remove the half-loaded data in case we started with
             * an empty replica. 
             *
             * 删除一半加载的数据，以防我们从一个空的复制从节点开始。
             * */
            emptyDb(-1,empty_db_flags,replicationEmptyDbCallback);

            if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
                /* Restore the backed up databases. 
                 *
                 * 还原备份的数据库。
                 * */
                disklessLoadRestoreBackup(diskless_load_backup);
            }

            /* Note that there's no point in restarting the AOF on SYNC
             * failure, it'll be restarted when sync succeeds or the replica
             * gets promoted. 
             *
             * 请注意，在SYNC失败时重新启动AOF没有意义，它将在同步成功或复制从节点升级时重
             * 新启动。
             * */
            return;
        }
        stopLoading(1);

        /* RDB loading succeeded if we reach this point. 
         *
         * 如果我们达到这一点，RDB加载就成功了。
         * */
        if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
            /* Delete the backup databases we created before starting to load
             * the new RDB. Now the RDB was loaded with success so the old
             * data is useless. 
             *
             * 在开始加载新的RDB之前，请删除我们创建的备份数据库。现在RDB加载成功了，所以
             * 旧数据是无用的。
             * */
            disklessLoadDiscardBackup(diskless_load_backup, empty_db_flags);
        }

        /* Verify the end mark is correct. 
         *
         * 验证结束标记是否正确。
         * */
        if (usemark) {
            if (!rioRead(&rdb,buf,CONFIG_RUN_ID_SIZE) ||
                memcmp(buf,eofmark,CONFIG_RUN_ID_SIZE) != 0)
            {
                serverLog(LL_WARNING,"Replication stream EOF marker is broken");
                cancelReplicationHandshake();
                rioFreeConn(&rdb, NULL);
                return;
            }
        }

        /* Cleanup and restore the socket to the original state to continue
         * with the normal replication. 
         *
         * 清理套接字并将其恢复到原始状态以继续正常复制。
         * */
        rioFreeConn(&rdb, NULL);
        connNonBlock(conn);
        connRecvTimeout(conn,0);
    } else {
        /* Ensure background save doesn't overwrite synced data 
         *
         * 确保后台保存不会覆盖同步数据
         * */
        if (server.rdb_child_pid != -1) {
            serverLog(LL_NOTICE,
                "Replica is about to load the RDB file received from the "
                "master, but there is a pending RDB child running. "
                "Killing process %ld and removing its temp file to avoid "
                "any race",
                    (long) server.rdb_child_pid);
            killRDBChild();
        }

        /* Make sure the new file (also used for persistence) is fully synced
         * (not covered by earlier calls to rdb_fsync_range). 
         *
         * 确保新文件（也用于持久性）已全量同步（以前对rdb_fsync_range的调用
         * 未涵盖）。
         * */
        if (fsync(server.repl_transfer_fd) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to sync the temp DB to disk in "
                "MASTER <-> REPLICA synchronization: %s",
                strerror(errno));
            cancelReplicationHandshake();
            return;
        }

        /* Rename rdb like renaming rewrite aof asynchronously. 
         *
         * 像异步重命名重写aof一样重命名rdb。
         * */
        int old_rdb_fd = open(server.rdb_filename,O_RDONLY|O_NONBLOCK);
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to rename the temp DB into %s in "
                "MASTER <-> REPLICA synchronization: %s",
                server.rdb_filename, strerror(errno));
            cancelReplicationHandshake();
            if (old_rdb_fd != -1) close(old_rdb_fd);
            return;
        }
        /* Close old rdb asynchronously. 
         *
         * 异步关闭旧的rdb。
         * */
        if (old_rdb_fd != -1) bioCreateBackgroundJob(BIO_CLOSE_FILE,(void*)(long)old_rdb_fd,NULL,NULL);

        if (rdbLoad(server.rdb_filename,&rsi,RDBFLAGS_REPLICATION) != C_OK) {
            serverLog(LL_WARNING,
                "Failed trying to load the MASTER synchronization "
                "DB from disk");
            cancelReplicationHandshake();
            if (server.rdb_del_sync_files && allPersistenceDisabled()) {
                serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                    "the master. This replica has persistence "
                                    "disabled");
                bg_unlink(server.rdb_filename);
            }
            /* Note that there's no point in restarting the AOF on sync failure,
               it'll be restarted when sync succeeds or replica promoted. 
             *
             * 请注意，在同步失败时重新启动AOF没有意义，它将在同步成功或复制从节点升级时重新启动。
             * */
            return;
        }

        /* Cleanup. 
         *
         * 清理。
         * */
        if (server.rdb_del_sync_files && allPersistenceDisabled()) {
            serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                "the master. This replica has persistence "
                                "disabled");
            bg_unlink(server.rdb_filename);
        }

        zfree(server.repl_transfer_tmpfile);
        close(server.repl_transfer_fd);
        server.repl_transfer_fd = -1;
        server.repl_transfer_tmpfile = NULL;
    }

    /* Final setup of the connected slave <- master link 
     *
     * 连接的从<-主链路的最终设置
     * */
    replicationCreateMasterClient(server.repl_transfer_s,rsi.repl_stream_db);
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;

    /* Fire the master link modules event. 
     *
     * 激发主链节模块事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                          REDISMODULE_SUBEVENT_MASTER_LINK_UP,
                          NULL);

    /* After a full resynchronization we use the replication ID and
     * offset of the master. The secondary ID / offset are cleared since
     * we are starting a new history. 
     *
     * 在重新全量同步之后，我们使用主服务器的复制ID和偏移量。由于我们正在启动一个新的
     * 历史记录，因此会清除次要ID/偏移量。
     * */
    memcpy(server.replid,server.master->replid,sizeof(server.replid));
    server.master_repl_offset = server.master->reploff;
    clearReplicationId2();

    /* Let's create the replication backlog if needed. Slaves need to
     * accumulate the backlog regardless of the fact they have sub-slaves
     * or not, in order to behave correctly if they are promoted to
     * masters after a failover. 
     *
     * 如果需要，让我们创建同步缓冲区。从节点服务器需要积累积压工作，无论它们是否有子从节点服务器，以便在故障转移后升级为主服务器时能够正常工作。
     * */
    if (server.repl_backlog == NULL) createReplicationBacklog();
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Finished with success");

    if (server.supervised_mode == SUPERVISED_SYSTEMD) {
        redisCommunicateSystemd("STATUS=MASTER <-> REPLICA sync: Finished with success. Ready to accept connections.\n");
        redisCommunicateSystemd("READY=1\n");
    }

    /* Restart the AOF subsystem now that we finished the sync. This
     * will trigger an AOF rewrite, and when done will start appending
     * to the new file. 
     *
     * 完成同步后，重新启动AOF子系统。这将触发AOF重写，完成后将开始追加到新文件中。
     *
     * */
    if (server.aof_enabled) restartAOFAfterSYNC();
    return;

error:
    cancelReplicationHandshake();
    return;
}

/* Send a synchronous command to the master. Used to send AUTH and
 * REPLCONF commands before starting the replication with SYNC.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 
 *
 * 向主节点发送同步命令。用于在使用SYNC启动复制之前发送AUTH和REPLCONF
 * 命令。该命令返回一个代表操作结果的sds字符串。出现错误时，第一个字节为“-”。
 * */
#define SYNC_CMD_READ (1<<0)
#define SYNC_CMD_WRITE (1<<1)
#define SYNC_CMD_FULL (SYNC_CMD_READ|SYNC_CMD_WRITE)
char *sendSynchronousCommand(int flags, connection *conn, ...) {

    /* Create the command to send to the master, we use redis binary
     * protocol to make sure correct arguments are sent. This function
     * is not safe for all binary data. 
     *
     * 创建要发送到master的命令，我们使用redis二进制协议来确保发送正确的参数。此函数对所有二进制数据都不安全。
     * */
    if (flags & SYNC_CMD_WRITE) {
        char *arg;
        va_list ap;
        sds cmd = sdsempty();
        sds cmdargs = sdsempty();
        size_t argslen = 0;
        va_start(ap,conn);

        while(1) {
            arg = va_arg(ap, char*);
            if (arg == NULL) break;

            cmdargs = sdscatprintf(cmdargs,"$%zu\r\n%s\r\n",strlen(arg),arg);
            argslen++;
        }

        va_end(ap);

        cmd = sdscatprintf(cmd,"*%zu\r\n",argslen);
        cmd = sdscatsds(cmd,cmdargs);
        sdsfree(cmdargs);

        /* Transfer command to the server. 
         *
         * 将命令传输到服务器。
         * */
        if (connSyncWrite(conn,cmd,sdslen(cmd),server.repl_syncio_timeout*1000)
            == -1)
        {
            sdsfree(cmd);
            return sdscatprintf(sdsempty(),"-Writing to master: %s",
                    connGetLastError(conn));
        }
        sdsfree(cmd);
    }

    /* Read the reply from the server. 
     *
     * 从服务器上读取回复。
     * */
    if (flags & SYNC_CMD_READ) {
        char buf[256];

        if (connSyncReadLine(conn,buf,sizeof(buf),server.repl_syncio_timeout*1000)
            == -1)
        {
            return sdscatprintf(sdsempty(),"-Reading from master: %s",
                    strerror(errno));
        }
        server.repl_transfer_lastio = server.unixtime;
        return sdsnew(buf);
    }
    return NULL;
}

/* Try a partial resynchronization with the master if we are about to reconnect.
 * If there is no cached master structure, at least try to issue a
 * "PSYNC ? -1" command in order to trigger a full resync using the PSYNC
 * command in order to obtain the master replid and the master replication
 * global offset.
 *
 * This function is designed to be called from syncWithMaster(), so the
 * following assumptions are made:
 *
 * 1) We pass the function an already connected socket "fd".
 * 2) This function does not close the file descriptor "fd". However in case
 *    of successful partial resynchronization, the function will reuse
 *    'fd' as file descriptor of the server.master client structure.
 *
 * The function is split in two halves: if read_reply is 0, the function
 * writes the PSYNC command on the socket, and a new function call is
 * needed, with read_reply set to 1, in order to read the reply of the
 * command. This is useful in order to support non blocking operations, so
 * that we write, return into the event loop, and read when there are data.
 *
 * When read_reply is 0 the function returns PSYNC_WRITE_ERR if there
 * was a write error, or PSYNC_WAIT_REPLY to signal we need another call
 * with read_reply set to 1. However even when read_reply is set to 1
 * the function may return PSYNC_WAIT_REPLY again to signal there were
 * insufficient data to read to complete its work. We should re-enter
 * into the event loop and wait in such a case.
 *
 * The function returns:
 *
 * PSYNC_CONTINUE: If the PSYNC command succeeded and we can continue.
 * PSYNC_FULLRESYNC: If PSYNC is supported but a full resync is needed.
 *                   In this case the master replid and global replication
 *                   offset is saved.
 * PSYNC_NOT_SUPPORTED: If the server does not understand PSYNC at all and
 *                      the caller should fall back to SYNC.
 * PSYNC_WRITE_ERROR: There was an error writing the command to the socket.
 * PSYNC_WAIT_REPLY: Call again the function with read_reply set to 1.
 * PSYNC_TRY_LATER: Master is currently in a transient error condition.
 *
 * Notable side effects:
 *
 * 1) As a side effect of the function call the function removes the readable
 *    event handler from "fd", unless the return value is PSYNC_WAIT_REPLY.
 * 2) server.master_initial_offset is set to the right value according
 *    to the master reply. This will be used to populate the 'server.master'
 *    structure replication offset.
 
 *
 * 如果要重新连接，请尝试与主节点进行部分重新同步。如果没有缓存的主结构，至少尝试发出
 * “PSYNC？-1”命令，以便使用PSYNC命令触发重新全量同步，从而获得主re
 * plid和主复制全局偏移量。这个函数被设计为从syncWithMaster（）调
 * 用，因此做出了以下假设：
 * 
 * 1）我们向函数传递一个已经连接的套接字“fd”。
 * 2） 此函数不会关闭文件描述符“fd”。但是，如果部分重新同步成功，函数将重用“fd”作
 * 为server.master客户端结构的文件描述符。
 * 
 * 该函数分为两半：如果read_reply为0，则该函数将PSYNC命令写入套接字，并且需要一个新的函数调用，
 * 其中read_reply设置为1，以便读取命令的回复。这对于支持非阻塞操作非常有
 * 用，这样我们就可以在有数据时进行写入、返回事件循环和读取。
 * 
 * 当read_reply为0时，如果出现写入错误，函数将返回PSYNC_WRITE_ERR，或PSYNC_WAIT_reply，
 * 表示我们需要另一个read_reply设置为1的调用。然
 * 而，即使当read_reply设置为1时，该函数也可能再次返回PSYNC_WAIT_reply，
 * 以表示没有足够的数据可供读取以完成其工作。在这种情况下，我们应该
 * 重新进入事件循环并等待。函数返回：
 * 
 * PSYNC_CONTINUE：如果PSYNC命令成功，我们可以继续。
 * PSYNC_FULLRESYNC：如果支持PSYNC，但需要重新全量同步。在这种情况下，将保存主replid和全局复制偏移量。
 * PSYNC_NOT_SUPPORTED：如果服务器根本不理解PSYNC，那么调用者应该返回SYNC。
 * PSYNC_WRITE_ERROR:将命令写入套接字时出错。
 * PSYNC_WAIT_REPLY：再次调用read_REPLY设置为1的函数。
 * PSYNC_TRY_LATER:Master当前处于瞬态错误状态。值得注意的副作用：
 * 
 * 1）作为函数调用的副作用，函数从“fd”中删除可读的事件处理程序，除非返回值为PSYNC_WAIT_REPLY。
 * 2） server.master_initial_offset根据主回复设置为正确的值。这将用于填充“server.master”结构复制偏移量。
 * */

#define PSYNC_WRITE_ERROR 0
#define PSYNC_WAIT_REPLY 1
#define PSYNC_CONTINUE 2
#define PSYNC_FULLRESYNC 3
#define PSYNC_NOT_SUPPORTED 4
#define PSYNC_TRY_LATER 5
int slaveTryPartialResynchronization(connection *conn, int read_reply) {
    char *psync_replid;
    char psync_offset[32];
    sds reply;

    /* Writing half 
     *
     * 写一半
     * */
    if (!read_reply) {
        /* Initially set master_initial_offset to -1 to mark the current
         * master replid and offset as not valid. Later if we'll be able to do
         * a FULL resync using the PSYNC command we'll set the offset at the
         * right value, so that this information will be propagated to the
         * client structure representing the master into server.master. 
         *
         * 最初将master_initial_offset设置为-1，以将当前主replid和offset标记为无效。
         * 稍后，如果我们能够使用PSYNC命令进行FULL重新
         * 同步，我们将把偏移量设置为正确的值，这样这些信息将传播到表示主服务器的客户端结构
         * server.master中。
         * */
        server.master_initial_offset = -1;

        if (server.cached_master) {
            psync_replid = server.cached_master->replid;
            snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
            serverLog(LL_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_replid, psync_offset);
        } else {
            serverLog(LL_NOTICE,"Partial resynchronization not possible (no cached master)");
            psync_replid = "?";
            memcpy(psync_offset,"-1",3);
        }

        /* Issue the PSYNC command 
         *
         * 发出PSYNC命令
         * */
        reply = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"PSYNC",psync_replid,psync_offset,NULL);
        if (reply != NULL) {
            serverLog(LL_WARNING,"Unable to send PSYNC to master: %s",reply);
            sdsfree(reply);
            connSetReadHandler(conn, NULL);
            return PSYNC_WRITE_ERROR;
        }
        return PSYNC_WAIT_REPLY;
    }

    /* Reading half 
     *
     * 读一半
     * */
    reply = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
    if (sdslen(reply) == 0) {
        /* The master may send empty newlines after it receives PSYNC
         * and before to reply, just to keep the connection alive. 
         *
         * 主节点可以在接收到PSYNC之后和回复之前发送空的换行符，只是为了保持连接的有效性。
         * */
        sdsfree(reply);
        return PSYNC_WAIT_REPLY;
    }

    connSetReadHandler(conn, NULL);

    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *replid = NULL, *offset = NULL;

        /* FULL RESYNC, parse the reply in order to extract the replid
         * and the replication offset. 
         *
         * FULL RESYNC，解析应答以提取replid和复制偏移量。
         * */
        replid = strchr(reply,' ');
        if (replid) {
            replid++;
            offset = strchr(replid,' ');
            if (offset) offset++;
        }
        if (!replid || !offset || (offset-replid-1) != CONFIG_RUN_ID_SIZE) {
            serverLog(LL_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* This is an unexpected condition, actually the +FULLRESYNC
             * reply means that the master supports PSYNC, but the reply
             * format seems wrong. To stay safe we blank the master
             * replid to make sure next PSYNCs will fail. 
             *
             * 这是一个意外的情况，实际上+FULLRESYNC回复意味着主节点支持PSYNC，但
             * 回复格式似乎错误。为了确保安全，我们清空主回复以确保下一个PSYNC将失败。
             * */
            memset(server.master_replid,0,CONFIG_RUN_ID_SIZE+1);
        } else {
            memcpy(server.master_replid, replid, offset-replid-1);
            server.master_replid[CONFIG_RUN_ID_SIZE] = '\0';
            server.master_initial_offset = strtoll(offset,NULL,10);
            serverLog(LL_NOTICE,"Full resync from master: %s:%lld",
                server.master_replid,
                server.master_initial_offset);
        }
        /* We are going to full resync, discard the cached master structure. 
         *
         * 我们将进行重新全量同步，丢弃缓存的主结构。
         * */
        replicationDiscardCachedMaster();
        sdsfree(reply);
        return PSYNC_FULLRESYNC;
    }

    if (!strncmp(reply,"+CONTINUE",9)) {
        /* Partial resync was accepted. 
         *
         * 已接受部分重新同步。
         * */
        serverLog(LL_NOTICE,
            "Successful partial resynchronization with master.");

        /* Check the new replication ID advertised by the master. If it
         * changed, we need to set the new ID as primary ID, and set or
         * secondary ID as the old master ID up to the current offset, so
         * that our sub-slaves will be able to PSYNC with us after a
         * disconnection. 
         *
         * 检查主节点播发的新复制ID。如果它发生了变化，我们需要将新的ID设置为主ID，并将
         * 或辅助ID设置为旧的主ID，直到当前偏移量，这样我们的子从节点设备就可以在断开连接
         * 后与我们一起PSYNC。
         * */
        char *start = reply+10;
        char *end = reply+9;
        while(end[0] != '\r' && end[0] != '\n' && end[0] != '\0') end++;
        if (end-start == CONFIG_RUN_ID_SIZE) {
            char new[CONFIG_RUN_ID_SIZE+1];
            memcpy(new,start,CONFIG_RUN_ID_SIZE);
            new[CONFIG_RUN_ID_SIZE] = '\0';

            if (strcmp(new,server.cached_master->replid)) {
                /* Master ID changed. 
                 *
                 * 主ID已更改。
                 * */
                serverLog(LL_WARNING,"Master replication ID changed to %s",new);

                /* Set the old ID as our ID2, up to the current offset+1. 
                 *
                 * 将旧ID设置为我们的ID2，直到当前偏移量+1。
                 * */
                memcpy(server.replid2,server.cached_master->replid,
                    sizeof(server.replid2));
                server.second_replid_offset = server.master_repl_offset+1;

                /* Update the cached master ID and our own primary ID to the
                 * new one. 
                 *
                 * 将缓存的主ID和我们自己的主ID更新为新的主ID。
                 * */
                memcpy(server.replid,new,sizeof(server.replid));
                memcpy(server.cached_master->replid,new,sizeof(server.replid));

                /* Disconnect all the sub-slaves: they need to be notified. 
                 *
                 * 断开所有子从节点服务器的连接：需要通知它们。
                 * */
                disconnectSlaves();
            }
        }

        /* Setup the replication to continue. 
         *
         * 设置复制以继续。
         * */
        sdsfree(reply);
        replicationResurrectCachedMaster(conn);

        /* If this instance was restarted and we read the metadata to
         * PSYNC from the persistence file, our replication backlog could
         * be still not initialized. Create it. 
         *
         * 如果这个节点被重新启动，并且我们从持久性文件中将元数据读取到PSYNC，那么我们
         * 的同步缓冲区可能仍然没有初始化。创建它。
         * */
        if (server.repl_backlog == NULL) createReplicationBacklog();
        return PSYNC_CONTINUE;
    }

    /* If we reach this point we received either an error (since the master does
     * not understand PSYNC or because it is in a special state and cannot
     * serve our request), or an unexpected reply from the master.
     *
     * Return PSYNC_NOT_SUPPORTED on errors we don't understand, otherwise
     * return PSYNC_TRY_LATER if we believe this is a transient error. 
     *
     * 如果我们达到这一点，我们要么收到错误（因为主节点不理解PSYNC，或者因为它处于特
     * 殊状态，无法满足我们的请求），要么收到来自主节点的意外回复。对于我们不理解的错误，
     * 返回PSYNC_NOT_SUPPORTED，否则，如果我们认为这是暂时错误，则返
     * 回PSYNC_TRY_LATER。
     * */

    if (!strncmp(reply,"-NOMASTERLINK",13) ||
        !strncmp(reply,"-LOADING",8))
    {
        serverLog(LL_NOTICE,
            "Master is currently unable to PSYNC "
            "but should be in the future: %s", reply);
        sdsfree(reply);
        return PSYNC_TRY_LATER;
    }

    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. 
         *
         * 如果不是错误，请记录意外事件。
         * */
        serverLog(LL_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        serverLog(LL_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    replicationDiscardCachedMaster();
    return PSYNC_NOT_SUPPORTED;
}

/* This handler fires when the non blocking connect was able to
 * establish a connection with the master. 
 *
 * 当非阻塞连接能够与主节点建立连接时，此处理程序将激发。
 * */
void syncWithMaster(connection *conn) {
    char tmpfile[256], *err = NULL;
    int dfd = -1, maxtries = 5;
    int psync_result;

    /* If this event fired after the user turned the instance into a master
     * with SLAVEOF NO ONE we must just return ASAP. 
     *
     * 如果这个事件是在用户用SLAVEOF NONE将节点变成主节点后触发的，我们必须
     * 尽快返回。
     * */
    if (server.repl_state == REPL_STATE_NONE) {
        connClose(conn);
        return;
    }

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. 
     *
     * 检查套接字中的错误：在非阻塞连接（）之后，我们可能会发现套接字处于错误状态。
     * */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,"Error condition on socket for SYNC: %s",
                connGetLastError(conn));
        goto error;
    }

    /* Send a PING to check the master is able to reply without errors. 
     *
     * 发送PING以检查主节点是否能够无错误回复。
     * */
    if (server.repl_state == REPL_STATE_CONNECTING) {
        serverLog(LL_NOTICE,"Non blocking connect for SYNC fired the event.");
        /* Delete the writable event so that the readable event remains
         * registered and we can wait for the PONG reply. 
         *
         * 删除可写事件，以便可读事件保持注册状态，我们可以等待PONG回复。
         * */
        connSetReadHandler(conn, syncWithMaster);
        connSetWriteHandler(conn, NULL);
        server.repl_state = REPL_STATE_RECEIVE_PONG;
        /* Send the PING, don't check for errors at all, we have the timeout
         * that will take care about this. 
         *
         * 发送PING，根本不检查错误，我们有超时处理。
         * */
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"PING",NULL);
        if (err) goto write_error;
        return;
    }

    /* Receive the PONG command. 
     *
     * 接收PONG命令。
     * */
    if (server.repl_state == REPL_STATE_RECEIVE_PONG) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);

        /* We accept only two replies as valid, a positive +PONG reply
         * (we just check for "+") or an authentication error.
         * Note that older versions of Redis replied with "operation not
         * permitted" instead of using a proper error code, so we test
         * both. 
         *
         * 我们只接受两个有效的回复，一个是肯定的+PONG回复（我们只检查“+”）或身份验
         * 证错误。请注意，较旧版本的Redis回复为“不允许操作”，而不是使用正确的错误代
         * 码，因此我们对两者都进行了测试。
         * */
        if (err[0] != '+' &&
            strncmp(err,"-NOAUTH",7) != 0 &&
            strncmp(err,"-NOPERM",7) != 0 &&
            strncmp(err,"-ERR operation not permitted",28) != 0)
        {
            serverLog(LL_WARNING,"Error reply to PING from master: '%s'",err);
            sdsfree(err);
            goto error;
        } else {
            serverLog(LL_NOTICE,
                "Master replied to PING, replication can continue...");
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_AUTH;
    }

    /* AUTH with the master if required. 
     *
     * 如有需要，请与主节点联系。
     * */
    if (server.repl_state == REPL_STATE_SEND_AUTH) {
        if (server.masteruser && server.masterauth) {
            err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"AUTH",
                                         server.masteruser,server.masterauth,NULL);
            if (err) goto write_error;
            server.repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
        } else if (server.masterauth) {
            err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"AUTH",server.masterauth,NULL);
            if (err) goto write_error;
            server.repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
        } else {
            server.repl_state = REPL_STATE_SEND_PORT;
        }
    }

    /* Receive AUTH reply. 
     *
     * 接收AUTH回复。
     * */
    if (server.repl_state == REPL_STATE_RECEIVE_AUTH) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        if (err[0] == '-') {
            serverLog(LL_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_PORT;
    }

    /* Set the slave port, so that Master's INFO command can list the
     * slave listening port correctly. 
     *
     * 设置从端口，以便Master的INFO命令可以正确列出从侦听端口。
     * */
    if (server.repl_state == REPL_STATE_SEND_PORT) {
        int port;
        if (server.slave_announce_port) port = server.slave_announce_port;
        else if (server.tls_replication && server.tls_port) port = server.tls_port;
        else port = server.port;
        sds portstr = sdsfromlonglong(port);
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "listening-port",portstr, NULL);
        sdsfree(portstr);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_PORT;
        return;
    }

    /* Receive REPLCONF listening-port reply. 
     *
     * 接收REPLCONF侦听端口答复。
     * */
    if (server.repl_state == REPL_STATE_RECEIVE_PORT) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. 
         *
         * 忽略错误（如果有），并非所有Redis版本都支持REPLCONF侦听端口。
         * */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_IP;
    }

    /* Skip REPLCONF ip-address if there is no slave-announce-ip option set. 
     *
     * 如果没有设置从节点通告ip选项，则跳过REPLCONF ip地址。
     * */
    if (server.repl_state == REPL_STATE_SEND_IP &&
        server.slave_announce_ip == NULL)
    {
            server.repl_state = REPL_STATE_SEND_CAPA;
    }

    /* Set the slave ip, so that Master's INFO command can list the
     * slave IP address port correctly in case of port forwarding or NAT. 
     *
     * 设置从ip，以便在端口转发或NAT的情况下，Master的INFO命令可以正确列
     * 出从ip地址端口。
     * */
    if (server.repl_state == REPL_STATE_SEND_IP) {
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "ip-address",server.slave_announce_ip, NULL);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_IP;
        return;
    }

    /* Receive REPLCONF ip-address reply. 
     *
     * 接收REPLCONF ip地址回复。
     * */
    if (server.repl_state == REPL_STATE_RECEIVE_IP) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. 
         *
         * 忽略错误（如果有），并非所有Redis版本都支持REPLCONF侦听端口。
         * */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF ip-address: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_CAPA;
    }

    /* Inform the master of our (slave) capabilities.
     *
     * EOF: supports EOF-style RDB transfer for diskless replication.
     * PSYNC2: supports PSYNC v2, so understands +CONTINUE <new repl ID>.
     *
     * The master will ignore capabilities it does not understand. 
     *
     * 通知主节点我们的（从节点）能力。EOF：支持EOF风格的RDB传输，用于无盘复制。PSYNC2:支持PSYNC v2，因此理解+CONTINUE＜new replID＞。
     * 主节点会忽略它不理解的能力。
     * */
    if (server.repl_state == REPL_STATE_SEND_CAPA) {
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "capa","eof","capa","psync2",NULL);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_CAPA;
        return;
    }

    /* Receive CAPA reply. 
     *
     * 收到CAPA回复。
     * */
    if (server.repl_state == REPL_STATE_RECEIVE_CAPA) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF capa. 
         *
         * 忽略错误（如果有），并非所有Redis版本都支持REPLCONF capa。
         * */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                  "REPLCONF capa: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_PSYNC;
    }

    /* Try a partial resynchonization. If we don't have a cached master
     * slaveTryPartialResynchronization() will at least try to use PSYNC
     * to start a full resynchronization so that we get the master replid
     * and the global offset, to try a partial resync at the next
     * reconnection attempt. 
     *
     * 尝试部分重新同步。如果我们没有缓存的主slaveTryPartialResynchronization（）将至少尝试使用PSYNC启动重新全量同步，
     * 以便获得主replid和全局偏移量，以便在下一次重新连接尝试时尝试部分重新同步。
     * */
    if (server.repl_state == REPL_STATE_SEND_PSYNC) {
        if (slaveTryPartialResynchronization(conn,0) == PSYNC_WRITE_ERROR) {
            err = sdsnew("Write error sending the PSYNC command.");
            goto write_error;
        }
        server.repl_state = REPL_STATE_RECEIVE_PSYNC;
        return;
    }

    /* If reached this point, we should be in REPL_STATE_RECEIVE_PSYNC. 
     *
     * 如果达到这一点，我们应该处于REPL_STATE_RECEIVE_PSYNC。
     * */
    if (server.repl_state != REPL_STATE_RECEIVE_PSYNC) {
        serverLog(LL_WARNING,"syncWithMaster(): state machine error, "
                             "state should be RECEIVE_PSYNC but is %d",
                             server.repl_state);
        goto error;
    }

    psync_result = slaveTryPartialResynchronization(conn,1);
    if (psync_result == PSYNC_WAIT_REPLY) return; /* Try again later... 
                                                   *
                                                   * 稍后再试。。。
                                                   * */

    /* If the master is in an transient error, we should try to PSYNC
     * from scratch later, so go to the error path. This happens when
     * the server is loading the dataset or is not connected with its
     * master and so forth. 
     *
     * 如果主节点处于瞬态错误中，我们稍后应该尝试从头开始PSYNC，所以转到错误路径。当
     * 服务器正在加载数据集或未与其主节点连接时，就会发生这种情况，依此类推。
     * */
    if (psync_result == PSYNC_TRY_LATER) goto error;

    /* Note: if PSYNC does not return WAIT_REPLY, it will take care of
     * uninstalling the read handler from the file descriptor. 
     *
     * 注意：如果PSYNC不返回WAIT_REPLY，它将负责从文件描述符卸载读取处理
     * 程序。
     * */

    if (psync_result == PSYNC_CONTINUE) {
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Master accepted a Partial Resynchronization.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=MASTER <-> REPLICA sync: Partial Resynchronization accepted. Ready to accept connections.\n");
            redisCommunicateSystemd("READY=1\n");
        }
        return;
    }

    /* PSYNC failed or is not supported: we want our slaves to resync with us
     * as well, if we have any sub-slaves. The master may transfer us an
     * entirely different data set and we have no way to incrementally feed
     * our slaves after that. 
     *
     * PSYNC失败或不受支持：如果我们有任何子从节点，我们希望我们的从节点也能与我们重新
     * 同步。主可能会向我们传输一个完全不同的数据集，在那之后我们没有办法增量推送我们的
     * 从节点。
     * */
    disconnectSlaves(); /* Force our slaves to resync with us as well. 
                         *
                         * 强迫我们的从节点也与我们重新同步。
                         * */
    freeReplicationBacklog(); /* Don't allow our chained slaves to PSYNC. 
                               *
                               * 不要让我们的从节点被锁在PSYNC上。
                               * */

    /* Fall back to SYNC if needed. Otherwise psync_result == PSYNC_FULLRESYNC
     * and the server.master_replid and master_initial_offset are
     * already populated. 
     *
     * 如果需要，请回退到SYNC。否则，psync_result==psync_FULLRESYNC和server.master_replid和
     * master_initial_offset已填充。
     * */
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        serverLog(LL_NOTICE,"Retrying with SYNC...");
        if (connSyncWrite(conn,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    /* Prepare a suitable temp file for bulk transfer 
     *
     * 为批量传输准备合适的临时文件
     * */
    if (!useDisklessLoad()) {
        while(maxtries--) {
            snprintf(tmpfile,256,
                "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
            dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
            if (dfd != -1) break;
            sleep(1);
        }
        if (dfd == -1) {
            serverLog(LL_WARNING,"Opening the temp file needed for MASTER <-> REPLICA synchronization: %s",strerror(errno));
            goto error;
        }
        server.repl_transfer_tmpfile = zstrdup(tmpfile);
        server.repl_transfer_fd = dfd;
    }

    /* Setup the non blocking download of the bulk file. 
     *
     * 设置大容量文件的非阻塞下载。
     * */
    if (connSetReadHandler(conn, readSyncBulkPayload)
            == C_ERR)
    {
        char conninfo[CONN_INFO_LEN];
        serverLog(LL_WARNING,
            "Can't create readable event for SYNC: %s (%s)",
            strerror(errno), connGetInfo(conn, conninfo, sizeof(conninfo)));
        goto error;
    }

    server.repl_state = REPL_STATE_TRANSFER;
    server.repl_transfer_size = -1;
    server.repl_transfer_read = 0;
    server.repl_transfer_last_fsync_off = 0;
    server.repl_transfer_lastio = server.unixtime;
    return;

error:
    if (dfd != -1) close(dfd);
    connClose(conn);
    server.repl_transfer_s = NULL;
    if (server.repl_transfer_fd != -1)
        close(server.repl_transfer_fd);
    if (server.repl_transfer_tmpfile)
        zfree(server.repl_transfer_tmpfile);
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_state = REPL_STATE_CONNECT;
    return;

write_error: /* Handle sendSynchronousCommand(SYNC_CMD_WRITE) errors. 
              *
              * 处理sendSynchronousCommand（SYNC_CMD_WRITE）
              * 错误。
              * */
    serverLog(LL_WARNING,"Sending command to master in replication handshake: %s", err);
    sdsfree(err);
    goto error;
}

int connectWithMaster(void) {
    server.repl_transfer_s = server.tls_replication ? connCreateTLS() : connCreateSocket();
    if (connConnect(server.repl_transfer_s, server.masterhost, server.masterport,
                NET_FIRST_BIND_ADDR, syncWithMaster) == C_ERR) {
        serverLog(LL_WARNING,"Unable to connect to MASTER: %s",
                connGetLastError(server.repl_transfer_s));
        connClose(server.repl_transfer_s);
        server.repl_transfer_s = NULL;
        return C_ERR;
    }


    server.repl_transfer_lastio = server.unixtime;
    server.repl_state = REPL_STATE_CONNECTING;
    return C_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 
 *
 * 当当前正在进行非阻塞连接以撤消连接时，可以调用此函数。切勿直接调用此函数，请改用
 * cancelReplicationHandshake（）。
 * */
void undoConnectWithMaster(void) {
    connClose(server.repl_transfer_s);
    server.repl_transfer_s = NULL;
}

/* Abort the async download of the bulk dataset while SYNC-ing with master.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 
 *
 * 在与master同步时中止批量数据集的异步下载。永远不要直接调用此函数，而是使用cancelReplicationHandshake（）。
 * */
void replicationAbortSyncTransfer(void) {
    serverAssert(server.repl_state == REPL_STATE_TRANSFER);
    undoConnectWithMaster();
    if (server.repl_transfer_fd!=-1) {
        close(server.repl_transfer_fd);
        bg_unlink(server.repl_transfer_tmpfile);
        zfree(server.repl_transfer_tmpfile);
        server.repl_transfer_tmpfile = NULL;
        server.repl_transfer_fd = -1;
    }
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (server.repl_state) set to REPL_STATE_CONNECT.
 *
 * Otherwise zero is returned and no operation is performed at all. 
 *
 * 如果正在进行非阻塞复制尝试，此函数将通过取消非阻塞连接尝试或初始大容量传输来中止
 * 该尝试。如果正在进行复制握手，则返回1，并且复制状态（server.repl_state）设置为repl_state_CONNECT。
 * 否则返回零，并且根本不执行任何操作。
 * */
int cancelReplicationHandshake(void) {
    if (server.repl_state == REPL_STATE_TRANSFER) {
        replicationAbortSyncTransfer();
        server.repl_state = REPL_STATE_CONNECT;
    } else if (server.repl_state == REPL_STATE_CONNECTING ||
               slaveIsInHandshakeState())
    {
        undoConnectWithMaster();
        server.repl_state = REPL_STATE_CONNECT;
    } else {
        return 0;
    }
    return 1;
}

/* Set replication to the specified master address and port. 
 *
 * 将复制设置为指定的主地址和端口。
 * */
void replicationSetMaster(char *ip, int port) {
    int was_master = server.masterhost == NULL;

    sdsfree(server.masterhost);
    server.masterhost = sdsnew(ip);
    server.masterport = port;
    if (server.master) {
        freeClient(server.master);
    }
    disconnectAllBlockedClients(); /* Clients blocked in master, now slave. 
                                    *
                                    * 客户端在master中被阻止，现在是slave。
                                    * */

    /* Update oom_score_adj 
     *
     * 更新oom_score_adj
     * */
    setOOMScoreAdj(-1);

    /* Force our slaves to resync with us as well. They may hopefully be able
     * to partially resync with us, but we can notify the replid change. 
     *
     * 强迫我们的从节点也与我们重新同步。他们可能希望能够与我们部分重新同步，但我们可以通
     * 知replid的更改。
     * */
    disconnectSlaves();
    cancelReplicationHandshake();
    /* Before destroying our master state, create a cached master using
     * our own parameters, to later PSYNC with the new master. 
     *
     * 在破坏我们的master状态之前，使用我们自己的参数创建一个缓存的master，
     * 以便稍后使用新的master进行PSYNC。
     * */
    if (was_master) {
        replicationDiscardCachedMaster();
        replicationCacheMasterUsingMyself();
    }

    /* Fire the role change modules event. 
     *
     * 激发角色更改模块事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED,
                          REDISMODULE_EVENT_REPLROLECHANGED_NOW_REPLICA,
                          NULL);

    /* Fire the master link modules event. 
     *
     * 激发主链节模块事件。
     * */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    server.repl_state = REPL_STATE_CONNECT;
}

/* Cancel replication, setting the instance as a master itself. 
 *
 * 取消复制，将节点本身设置为主节点。
 * */
void replicationUnsetMaster(void) {
    if (server.masterhost == NULL) return; /* Nothing to do. 
                                            *
                                            * 没事可做。
                                            * */

    /* Fire the master link modules event. 
     *
     * 激发主链节模块事件。
     * */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    sdsfree(server.masterhost);
    server.masterhost = NULL;
    if (server.master) freeClient(server.master);
    replicationDiscardCachedMaster();
    cancelReplicationHandshake();
    /* When a slave is turned into a master, the current replication ID
     * (that was inherited from the master at synchronization time) is
     * used as secondary ID up to the current offset, and a new replication
     * ID is created to continue with a new replication history.
     *
     * NOTE: this function MUST be called after we call
     * freeClient(server.master), since there we adjust the replication
     * offset trimming the final PINGs. See Github issue #7320. 
     *
     * 当从节点变为主设备时，当前复制ID（在同步时从主设备继承的）将用作当前偏移量之前
     * 的辅助ID，并创建一个新的复制ID以继续使用新的复制历史记录。注意：这个函数必须
     * 在我们调用freeClient（server.master）之后调用，因为在那里
     * 我们会调整复制偏移量来修剪最终的PING。请参阅Github第7320期。
     * */
    shiftReplicationId();
    /* Disconnecting all the slaves is required: we need to inform slaves
     * of the replication ID change (see shiftReplicationId() call). However
     * the slaves will be able to partially resync with us, so it will be
     * a very fast reconnection. 
     *
     * 需要断开所有从节点服务器的连接：我们需要通知从节点服务器复制ID的更改（请参阅shiftReplicationId（）调用）。
     * 然而，从节点将能够部分与我们重新同步，因此这将是一次非常快速的重新连接。
     * */
    disconnectSlaves();
    server.repl_state = REPL_STATE_NONE;

    /* We need to make sure the new master will start the replication stream
     * with a SELECT statement. This is forced after a full resync, but
     * with PSYNC version 2, there is no need for full resync after a
     * master switch. 
     *
     * 我们需要确保新主节点将使用SELECT语句启动复制流。这是在重新全量同步之后强制执
     * 行的，但对于PSYNC版本2，在主交换机之后不需要重新全量同步。
     * */
    server.slaveseldb = -1;

    /* Update oom_score_adj 
     *
     * 更新oom_score_adj
     * */
    setOOMScoreAdj(-1);

    /* Once we turn from slave to master, we consider the starting time without
     * slaves (that is used to count the replication backlog time to live) as
     * starting from now. Otherwise the backlog will be freed after a
     * failover if slaves do not connect immediately. 
     *
     * 一旦我们从从节点转向主，我们就认为没有从节点的开始时间（用于计算同步缓冲区的生存时间）
     * 是从现在开始的。否则，如果从节点服务器没有立即连接，则在故障转移后将释放积压工作。
     * */
    server.repl_no_slaves_since = server.unixtime;

    /* Fire the role change modules event. 
     *
     * 激发角色更改模块事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED,
                          REDISMODULE_EVENT_REPLROLECHANGED_NOW_MASTER,
                          NULL);

    /* Restart the AOF subsystem in case we shut it down during a sync when
     * we were still a slave. 
     *
     * 重新启动AOF子系统，以防我们在同步期间关闭它，当时我们还是一个从节点系统。
     * */
    if (server.aof_enabled && server.aof_state == AOF_OFF) restartAOFAfterSYNC();
}

/* This function is called when the slave lose the connection with the
 * master into an unexpected way. 
 *
 * 当从节点以意外方式失去与主设备的连接时，会调用此函数。
 * */
void replicationHandleMasterDisconnection(void) {
    /* Fire the master link modules event. 
     *
     * 激发主链节模块事件。
     * */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    server.master = NULL;
    server.repl_state = REPL_STATE_CONNECT;
    server.repl_down_since = server.unixtime;
    /* We lost connection with our master, don't disconnect slaves yet,
     * maybe we'll be able to PSYNC with our master later. We'll disconnect
     * the slaves only if we'll have to do a full resync with our master. 
     *
     * 我们失去了与主节点的联系，现在还不要断开从节点的连接，也许我们稍后可以与主节点进行PSYNC。
     * 只有当我们必须与主设备进行重新全量同步时，我们才会断开从节点设备的连接。
     * */
}

void replicaofCommand(client *c) {
    /* SLAVEOF is not allowed in cluster mode as replication is automatically
     * configured using the current address of the master node. 
     *
     * 集群模式下不允许SLAVEOF，因为复制是使用主节点的当前地址自动配置的。
     * */
    if (server.cluster_enabled) {
        addReplyError(c,"REPLICAOF not allowed in cluster mode.");
        return;
    }

    /* The special host/port combination "NO" "ONE" turns the instance
     * into a master. Otherwise the new master address is set. 
     *
     * 特殊的主节点/端口组合“NO”“ONE”将节点变成主节点。否则，将设置新的主地址。
     * */
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            replicationUnsetMaster();
            sds client = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE,"MASTER MODE enabled (user request from '%s')",
                client);
            sdsfree(client);
        }
    } else {
        long port;

        if (c->flags & CLIENT_SLAVE)
        {
            /* If a client is already a replica they cannot run this command,
             * because it involves flushing all replicas (including this
             * client) 
             *
             * 如果客户端已经是复制从节点，则无法运行此命令，因为它涉及到刷新所有复制从节点（包括此
             * 客户端）
             * */
            addReplyError(c, "Command is not valid when client is a replica.");
            return;
        }

        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != C_OK))
            return;

        /* Check if we are already attached to the specified slave 
         *
         * 检查我们是否已经连接到指定的从节点服务器
         * */
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            serverLog(LL_NOTICE,"REPLICAOF would result into synchronization "
                                "with the master we are already connected "
                                "with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified "
                                 "master\r\n"));
            return;
        }
        /* There was no previous master or the user specified a different one,
         * we can continue. 
         *
         * 没有以前的master，或者用户指定了不同的master，我们可以继续。
         * */
        replicationSetMaster(c->argv[1]->ptr, port);
        sds client = catClientInfoString(sdsempty(),c);
        serverLog(LL_NOTICE,"REPLICAOF %s:%d enabled (user request from '%s')",
            server.masterhost, server.masterport, client);
        sdsfree(client);
    }
    addReply(c,shared.ok);
}

/* ROLE command: provide information about the role of the instance
 * (master or slave) and additional information related to replication
 * in an easy to process format. 
 *
 * ROLE命令：以易于处理的格式提供有关节点（主节点或从节点）角色的信息以及与复制
 * 相关的其他信息。
 * */
void roleCommand(client *c) {
    if (server.masterhost == NULL) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;

        addReplyArrayLen(c,3);
        addReplyBulkCBuffer(c,"master",6);
        addReplyLongLong(c,server.master_repl_offset);
        mbcount = addReplyDeferredLen(c);
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            char ip[NET_IP_STR_LEN], *slaveip = slave->slave_ip;

            if (slaveip[0] == '\0') {
                if (connPeerToString(slave->conn,ip,sizeof(ip),NULL) == -1)
                    continue;
                slaveip = ip;
            }
            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            addReplyArrayLen(c,3);
            addReplyBulkCString(c,slaveip);
            addReplyBulkLongLong(c,slave->slave_listening_port);
            addReplyBulkLongLong(c,slave->repl_ack_off);
            slaves++;
        }
        setDeferredArrayLen(c,mbcount,slaves);
    } else {
        char *slavestate = NULL;

        addReplyArrayLen(c,5);
        addReplyBulkCBuffer(c,"slave",5);
        addReplyBulkCString(c,server.masterhost);
        addReplyLongLong(c,server.masterport);
        if (slaveIsInHandshakeState()) {
            slavestate = "handshake";
        } else {
            switch(server.repl_state) {
            case REPL_STATE_NONE: slavestate = "none"; break;
            case REPL_STATE_CONNECT: slavestate = "connect"; break;
            case REPL_STATE_CONNECTING: slavestate = "connecting"; break;
            case REPL_STATE_TRANSFER: slavestate = "sync"; break;
            case REPL_STATE_CONNECTED: slavestate = "connected"; break;
            default: slavestate = "unknown"; break;
            }
        }
        addReplyBulkCString(c,slavestate);
        addReplyLongLong(c,server.master ? server.master->reploff : -1);
    }
}

/* Send a REPLCONF ACK command to the master to inform it about the current
 * processed offset. If we are not connected with a master, the command has
 * no effects. 
 *
 * 向主节点发送REPLCONF ACK命令，通知其当前处理的偏移量。如果我们没有与主
 * 机连接，则该命令没有任何效果。
 * */
void replicationSendAck(void) {
    client *c = server.master;

    if (c != NULL) {
        c->flags |= CLIENT_MASTER_FORCE_REPLY;
        addReplyArrayLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        c->flags &= ~CLIENT_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC --------------------------
 * ----------------------PSYNC的主缓存-----------------------------------------
 * */

/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * It is cached into server.cached_master and flushed away using the following
 * functions. 
 *
 * 为了实现部分同步，我们需要能够在短暂断开连接后缓存主节点的客户端结构。它被缓存到server.cached_master中，并使用以下函数清除。
 * */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destroying it. freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * The other functions that will deal with the cached master are:
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate the cached master.
 
 *
 * freeClient（）调用此函数是为了缓存主客户端结构，而不是破坏它。freeClient（）。
 * 其他将处理缓存主节点的函数有：replicationDiscardCachedMaster（），
 * 它将确保杀死客户端，因为出于某种原因，我们将来不想
 * 使用它。replicationResurrectCachedMaster（），
 * 用于在成功的PSYNC握手后重新激活缓存的主节点。
 * */
void replicationCacheMaster(client *c) {
    serverAssert(server.master != NULL && server.cached_master == NULL);
    serverLog(LL_NOTICE,"Caching the disconnected master state.");

    /* Unlink the client from the server structures. 
     *
     * 取消客户端与服务器结构的链接。
     * */
    unlinkClient(c);

    /* Reset the master client so that's ready to accept new commands:
     * we want to discard te non processed query buffers and non processed
     * offsets, including pending transactions, already populated arguments,
     * pending outputs to the master. 
     *
     * 重置主客户端，以便准备好接受新命令：我们希望丢弃未处理的查询缓冲区和未处理的偏移
     * 量，包括挂起的事务、已填充的参数、到主的挂起输出。
     * */
    sdsclear(server.master->querybuf);
    sdsclear(server.master->pending_querybuf);
    server.master->read_reploff = server.master->reploff;
    if (c->flags & CLIENT_MULTI) discardTransaction(c);
    listEmpty(c->reply);
    c->sentlen = 0;
    c->reply_bytes = 0;
    c->bufpos = 0;
    resetClient(c);

    /* Save the master. Server.master will be set to null later by
     * replicationHandleMasterDisconnection(). 
     *
     * 保存主控形状。Server.master稍后将由replicationHandl
     * eMasterDisconnection（）设置为null。
     * */
    server.cached_master = server.master;

    /* Invalidate the Peer ID cache. 
     *
     * 使对等ID缓存无效。
     * */
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set server.master to NULL. 
     *
     * 缓存master而不是实际的freeClient（）调用，因此请确保调整复制状态。此函数还将server.master设置为NULL。
     * */
    replicationHandleMasterDisconnection();
}

/* This function is called when a master is turend into a slave, in order to
 * create from scratch a cached master for the new client, that will allow
 * to PSYNC with the slave that was promoted as the new master after a
 * failover.
 *
 * Assuming this instance was previously the master instance of the new master,
 * the new master will accept its replication ID, and potentiall also the
 * current offset if no data was lost during the failover. So we use our
 * current replication ID and offset in order to synthesize a cached master. 
 *
 * 当一个主服务器被转换为一个从服务器时，会调用此函数，以便为新客户端从头开始创建一
 * 个缓存的主服务器，这将允许在故障转移后将被提升为新主服务器的从服务器进行PSYNC。
 * 假设此节点以前是新主节点的主节点，则新主节点将接受其复制ID，如果在故障转移期间
 * 没有丢失数据，则可能还会接受当前偏移量。因此，我们使用当前的复制ID和偏移量来合
 * 成缓存的master。
 * */
void replicationCacheMasterUsingMyself(void) {
    serverLog(LL_NOTICE,
        "Before turning into a replica, using my own master parameters "
        "to synthesize a cached master: I may be able to synchronize with "
        "the new master with just a partial transfer.");

    /* This will be used to populate the field server.master->reploff
     * by replicationCreateMasterClient(). We'll later set the created
     * master as server.cached_master, so the replica will use such
     * offset for PSYNC. 
     *
     * 这将用于通过replicationCreateMasterClient（）填充字
     * 段server.master->reploff。稍后我们将创建的master设置
     * 为server.cached_master，因此复制从节点将为PSYNC使用这样的
     * 偏移量。
     * */
    server.master_initial_offset = server.master_repl_offset;

    /* The master client we create can be set to any DBID, because
     * the new master will start its replication stream with SELECT. 
     *
     * 我们创建的主客户端可以设置为任何DBID，因为新的主客户端将使用SELECT启动其复制流。
     * */
    replicationCreateMasterClient(NULL,-1);

    /* Use our own ID / offset. 
     *
     * 使用我们自己的ID/偏移量。
     * */
    memcpy(server.master->replid, server.replid, sizeof(server.replid));

    /* Set as cached master. 
     *
     * 设置为缓存的主控形状。
     * */
    unlinkClient(server.master);
    server.cached_master = server.master;
    server.master = NULL;
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. 
 *
 * 释放缓存的master，当重新连接时不再有部分重新同步的条件时调用。
 * */
void replicationDiscardCachedMaster(void) {
    if (server.cached_master == NULL) return;

    serverLog(LL_NOTICE,"Discarding previously cached master state.");
    server.cached_master->flags &= ~CLIENT_MASTER;
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* Turn the cached master into the current master, using the file descriptor
 * passed as argument as the socket for the new master.
 *
 * This function is called when successfully setup a partial resynchronization
 * so the stream of data that we'll receive will start from were this
 * master left. 
 *
 * 使用作为参数传递的文件描述符作为新主节点的套接字，将缓存的主节点转换为当前主节点。
 *
 * 当成功设置部分重新同步时，会调用此函数，因此，如果保留此主节点，我们将从中开始接收数据流。
 * */
void replicationResurrectCachedMaster(connection *conn) {
    server.master = server.cached_master;
    server.cached_master = NULL;
    server.master->conn = conn;
    connSetPrivateData(server.master->conn, server.master);
    server.master->flags &= ~(CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP);
    server.master->authenticated = 1;
    server.master->lastinteraction = server.unixtime;
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;

    /* Fire the master link modules event. 
     *
     * 激发主链节模块事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                          REDISMODULE_SUBEVENT_MASTER_LINK_UP,
                          NULL);

    /* Re-add to the list of clients. 
     *
     * 重新添加到客户端列表中。
     * */
    linkClient(server.master);
    if (connSetReadHandler(server.master->conn, readQueryFromClient)) {
        serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master); /* Close ASAP. 
                                         *
                                         * 尽快关闭。
                                         * */
    }

    /* We may also need to install the write handler as well if there is
     * pending data in the write buffers. 
     *
     * 如果写缓冲区中有挂起的数据，我们可能还需要安装写处理程序。
     * */
    if (clientHasPendingReplies(server.master)) {
        if (connSetWriteHandler(server.master->conn, sendReplyToClient)) {
            serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master); /* Close ASAP. 
                                         *
                                         * 尽快关闭。
                                         * */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  ---------------------------
 * -------------------------写入的最小级别--------------------------------------
 * */

/* This function counts the number of slaves with lag <= min-slaves-max-lag.
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). 
 *
 * 此函数统计滞后的从节点数量<=最小从节点最大滞后。如果该选项处于活动状态，则如果没有
 * 足够的具有指定滞后（或更短）的连接从节点服务器，则服务器将阻止写入。
 * */
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;

    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        time_t lag = server.unixtime - slave->repl_ack_time;

        if (slave->replstate == SLAVE_STATE_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }
    server.repl_good_slaves_count = good;
}

/* ----------------------- REPLICATION SCRIPT CACHE --------------------------
 * The goal of this code is to keep track of scripts already sent to every
 * connected slave, in order to be able to replicate EVALSHA as it is without
 * translating it to EVAL every time it is possible.
 *
 * We use a capped collection implemented by a hash table for fast lookup
 * of scripts we can send as EVALSHA, plus a linked list that is used for
 * eviction of the oldest entry when the max number of items is reached.
 *
 * We don't care about taking a different cache for every different slave
 * since to fill the cache again is not very costly, the goal of this code
 * is to avoid that the same big script is transmitted a big number of times
 * per second wasting bandwidth and processor speed, but it is not a problem
 * if we need to rebuild the cache from scratch from time to time, every used
 * script will need to be transmitted a single time to reappear in the cache.
 *
 * This is how the system works:
 *
 * 1) Every time a new slave connects, we flush the whole script cache.
 * 2) We only send as EVALSHA what was sent to the master as EVALSHA, without
 *    trying to convert EVAL into EVALSHA specifically for slaves.
 * 3) Every time we transmit a script as EVAL to the slaves, we also add the
 *    corresponding SHA1 of the script into the cache as we are sure every
 *    slave knows about the script starting from now.
 * 4) On SCRIPT FLUSH command, we replicate the command to all the slaves
 *    and at the same time flush the script cache.
 * 5) When the last slave disconnects, flush the cache.
 * 6) We handle SCRIPT LOAD as well since that's how scripts are loaded
 *    in the master sometimes.
 
 *
 * -----------------------复制脚本缓存-------------------------
 * 此代码的目标是跟踪已发送到每个连接的从节点服务器的脚本，
 * 以便能够按原样复制EVALSHA，而无需每次都将其转换为EVAL。我们使用一个
 * 由哈希表实现的带帽集合来快速查找可以作为EVALSHA发送的脚本，再加上一个链表，
 * 当达到最大项目数时，该链表用于驱逐最旧的条目。我们不在乎为每个不同的从节点服务器
 * 使用不同的缓存，因为再次填充缓存的成本不是很高，这段代码的目标是避免同一个大脚本
 * 每秒传输大量，浪费带宽和处理器速度，但如果我们需要不时从头开始重建缓存，这不是问
 * 题，每个使用过的脚本都需要传输一次才能重新出现在缓存中。这就是系统的工作方式：
 *
 * 1）每次连接新的从节点服务器时，我们都会刷新整个脚本缓存。
 * 2） 我们只将发送给主设备的内容作为EVALSHA发送，而没有尝试将EVAL转换为专门针对从节点设备的EVALSHA。
 * 3） 每次我们将脚本作为EVAL传输到从节点服务器时，我们也会将脚本的相
 *     应SHA1添加到缓存中，因为我们确信每个从节点服务器从现在开始都知道该脚本。
 * 4） 在SCRIPT FLUSH命令中，我们将该命令复制到所有从节点服务器，同时刷新脚本缓存。
 * 5） 当最后一个从节点断开连接时，刷新缓存。6） 我们也处理脚本加载，因为
 *     有时这就是在master中加载脚本的方式。
 * */

/* Initialize the script cache, only called at startup. 
 *
 * 初始化脚本缓存，仅在启动时调用。
 * */
void replicationScriptCacheInit(void) {
    server.repl_scriptcache_size = 10000;
    server.repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    server.repl_scriptcache_fifo = listCreate();
}

/* Empty the script cache. Should be called every time we are no longer sure
 * that every slave knows about all the scripts in our set, or when the
 * current AOF "context" is no longer aware of the script. In general we
 * should flush the cache:
 *
 * 1) Every time a new slave reconnects to this master and performs a
 *    full SYNC (PSYNC does not require flushing).
 * 2) Every time an AOF rewrite is performed.
 * 3) Every time we are left without slaves at all, and AOF is off, in order
 *    to reclaim otherwise unused memory.
 
 *
 * 清空脚本缓存。每当我们不再确定每个从节点程序都知道我们集合中的所有脚本时，或者当当
 * 前AOF“上下文”不再知道该脚本时，都应该调用。通常，我们应该刷新缓存：
 * 1） 每次新的从节点重新连接到此主节点并执行完整的SYNC时（PSYNC不需要刷新）。
 * 2） 每次执行AOF重写时。
 * 3） 每次我们完全没有从节点，AOF关闭，以便回收未使用的内存
 * 。
 * */
void replicationScriptCacheFlush(void) {
    dictEmpty(server.repl_scriptcache_dict,NULL);
    listRelease(server.repl_scriptcache_fifo);
    server.repl_scriptcache_fifo = listCreate();
}

/* Add an entry into the script cache, if we reach max number of entries the
 * oldest is removed from the list. 
 *
 * 在脚本缓存中添加一个条目，如果达到最大条目数，则从列表中删除最旧的条目。
 * */
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);

    /* Evict oldest. 
     *
     * 驱逐最年长的人。
     * */
    if (listLength(server.repl_scriptcache_fifo) == server.repl_scriptcache_size)
    {
        listNode *ln = listLast(server.repl_scriptcache_fifo);
        sds oldest = listNodeValue(ln);

        retval = dictDelete(server.repl_scriptcache_dict,oldest);
        serverAssert(retval == DICT_OK);
        listDelNode(server.repl_scriptcache_fifo,ln);
    }

    /* Add current. 
     *
     * 添加当前。
     * */
    retval = dictAdd(server.repl_scriptcache_dict,key,NULL);
    listAddNodeHead(server.repl_scriptcache_fifo,key);
    serverAssert(retval == DICT_OK);
}

/* Returns non-zero if the specified entry exists inside the cache, that is,
 * if all the slaves are aware of this script SHA1. 
 *
 * 如果指定的条目存在于缓存中，即如果所有从节点设备都知道此脚本SHA1，则返回非零。
 * */
int replicationScriptCacheExists(sds sha1) {
    return dictFind(server.repl_scriptcache_dict,sha1) != NULL;
}

/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.
 
 *
 * -----------------------同步复制-------------------------
 * Redis同步复制设计可以概括为以下几点：
 * - Redis主节点有一个全局复制偏移量，由PSYNC使用。
 * - 每次向从节点设备发送新命令时，主设备都会增加偏移量。
 * - 到目前为止，从节点们用偏移量回击了主节点。
 *
 * 因此，同步复制添加了一个新 的WAIT命令，格式为：
 * WAIT＜num_replicas＞＜milliseconds_timeout＞，它返回当我们最终至少有num_replikas时或达到
 * 超时时处理查询的从节点数。
 *
 * 该命令是这样实现的：
 * - 每次客户端处理命令时，我们都会记住将该命令发送到从节点服务器后的复制偏移量。
 * - 当调用WAIT时，我们要求从节点尽快发送确认。客户端同时被阻止（请参阅blocked.c）。
 * - 一旦我们收到给定偏移量的足够ACK，或者当达到超时时，WAIT命令被阻止，并将回复发送给客户端。
 * */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronous replication
 * in a given event loop iteration, and send a single GETACK for them all. 
 *
 * 这只是设置了一个标志，以便我们向beforeSleep（）函数中的所有从节点对象广
 * 播REPLCONF GETACK命令。请注意，通过这种方式，我们将希望在给定的事
 * 件循环迭代中等待同步复制的所有客户端“分组”，并为它们发送一个GETACK。
 * */
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. 
 *
 * 返回已确认指定复制偏移量的从节点数量。
 * */
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate != SLAVE_STATE_ONLINE) continue;
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). 
 *
 * 等待N个从节点以确认我们最近的写入命令（以及所有以前的命令）的处理。
 * */
void waitCommand(client *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    if (server.masterhost) {
        addReplyError(c,"WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is configured to be writable (which is not the default) writes to replicas are just local and are not propagated.");
        return;
    }

    /* Argument parsing. 
     *
     * 参数分析。
     * */
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != C_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != C_OK) return;

    /* First try without blocking at all. 
     *
     * 第一次尝试完全不阻塞。
     * */
    ackreplicas = replicationCountAcksByOffset(c->woff);
    if (ackreplicas >= numreplicas || c->flags & CLIENT_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. 
     *
     * 否则，阻塞客户端，并将其放入等待从节点ack的客户端列表中。
     * */
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. 
     *
     * 在返回事件循环之前，请确保服务器将向所有从节点服务器发送ACK请求。
     * */
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. 
 *
 * 这是由unlockClient（）调用的，用于执行阻塞操作类型特定的清理。我们只
 * 是将客户端从等待从节点确认的客户端列表中删除。永远不要直接调用它，而是调用unlockClient（）。
 * */
void unblockClientWaitingReplicas(client *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    serverAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. 
 *
 * 检查WAIT中是否有被阻止的客户端可以被阻止，因为我们从从节点服务器收到了足够的ACK。
 * */
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;

    listRewind(server.clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        client *c = ln->value;

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. 
         *
         * 每次我们发现一个客户端对给定的偏移量和从节点数量感到满意时，我们都会记住它，这样，
         * 如果请求的偏移量/从节点数等于或小于，则下一个客户端可以在不调用replicationCountAcksByOffset（）的情况下被取消阻止。
         * */
        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}

/* Return the slave replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. 
 *
 * 返回此节点的从节点复制偏移量，即我们已经处理了主复制流的偏移量。
 * */
long long replicationGetSlaveOffset(void) {
    long long offset = 0;

    if (server.masterhost != NULL) {
        if (server.master) {
            offset = server.master->reploff;
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. 
     *
     * 当master根本不支持偏移量时，偏移量可能是-1，但是此函数旨在返回一个偏移量，
     * 该偏移量可以表示master处理的数据量，因此我们返回一个正整数。
     * */
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ----------------------------
 *                             复制CRON
 * */

/* Replication cron function, called 1 time per second. 
 *
 * 复制cron函数，每秒调用1次。
 * */
void replicationCron(void) {
    static long long replication_cron_loops = 0;

    /* Non blocking connection timeout? 
     *
     * 非阻塞连接超时？
     * */
    if (server.masterhost &&
        (server.repl_state == REPL_STATE_CONNECTING ||
         slaveIsInHandshakeState()) &&
         (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout connecting to the MASTER...");
        cancelReplicationHandshake();
    }

    /* Bulk transfer I/O timeout? 
     *
     * 批量传输I/O超时？
     * */
    if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        cancelReplicationHandshake();
    }

    /* Timed out master when we are an already connected slave? 
     *
     * 当我们是一个已经连接的从节点服务器时，主服务器超时？
     * */
    if (server.masterhost && server.repl_state == REPL_STATE_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"MASTER timeout: no data nor PING received...");
        freeClient(server.master);
    }

    /* Check if we should connect to a MASTER 
     *
     * 检查我们是否应该连接到MASTER
     * */
    if (server.repl_state == REPL_STATE_CONNECT) {
        serverLog(LL_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        if (connectWithMaster() == C_OK) {
            serverLog(LL_NOTICE,"MASTER <-> REPLICA sync started");
        }
    }

    /* Send ACK to master from time to time.
     * Note that we do not send periodic acks to masters that don't
     * support PSYNC and replication offsets. 
     *
     * 不时向主节点发送ACK。请注意，我们不会定期向不支持PSYNC和复制偏移的主节点发送
     * ack。
     * */
    if (server.masterhost && server.master &&
        !(server.master->flags & CLIENT_PRE_PSYNC))
        replicationSendAck();

    /* If we have attached slaves, PING them from time to time.
     * So slaves can implement an explicit timeout to masters, and will
     * be able to detect a link disconnection even if the TCP connection
     * will not actually go down. 
     *
     * 如果我们附属于从节点，不时地对他们进行PING。因此，从节点可以实现对主设备的显式
     * 超时，并且即使TCP连接实际上不会断开，也能够检测到链路断开。
     * */
    listIter li;
    listNode *ln;
    robj *ping_argv[1];

    /* First, send PING according to ping_slave_period. 
     *
     * 首先，根据PING_slave_period发送PING。
     * */
    if ((replication_cron_loops % server.repl_ping_slave_period) == 0 &&
        listLength(server.slaves))
    {
        /* Note that we don't send the PING if the clients are paused during
         * a Redis Cluster manual failover: the PING we send will otherwise
         * alter the replication offsets of master and slave, and will no longer
         * match the one stored into 'mf_master_offset' state. 
         *
         * 请注意，如果Redis Cluster手动故障转移期间客户端暂停，我们不会发送PING：
         * 否则，我们发送的PING将更改master和slave的复制偏移量，并且
         * 将不再与存储在“mf_master_offset”状态的偏移量相匹配。
         * */
        int manual_failover_in_progress =
            server.cluster_enabled &&
            server.cluster->mf_end &&
            clientsArePaused();

        if (!manual_failover_in_progress) {
            ping_argv[0] = createStringObject("PING",4);
            replicationFeedSlaves(server.slaves, server.slaveseldb,
                ping_argv, 1);
            decrRefCount(ping_argv[0]);
        }
    }

    /* Second, send a newline to all the slaves in pre-synchronization
     * stage, that is, slaves waiting for the master to create the RDB file.
     *
     * Also send the a newline to all the chained slaves we have, if we lost
     * connection from our master, to keep the slaves aware that their
     * master is online. This is needed since sub-slaves only receive proxied
     * data from top-level masters, so there is no explicit pinging in order
     * to avoid altering the replication offsets. This special out of band
     * pings (newlines) can be sent, they will have no effect in the offset.
     *
     * The newline will be ignored by the slave but will refresh the
     * last interaction timer preventing a timeout. In this case we ignore the
     * ping period and refresh the connection once per second since certain
     * timeouts are set at a few seconds (example: PSYNC response). 
     *
     * 第二，在预同步阶段向所有从节点发送一条换行符，即等待主节点创建RDB文件的从节点。
     *
     * 如果我们失去了与主节点的连接，也要向我们所有的被锁住的从节点发送一条换行符，让从节点知道他
     * 们的主节点在线。这是必要的，因为子从节点服务器只接收来自顶级主服务器的代理数据，因此
     * 没有显式ping以避免更改复制偏移。可以发送这种特殊的带外ping（换行），它们
     * 对偏移量没有影响。
     *
     * 换行符将被从节点忽略，但会刷新最后一个交互计时器以防止超时。在这种情况下，我们忽略ping周期，
     * 并每秒刷新一次连接，因为某些超时设置为几秒钟（例如：PSYNC响应）。
     * */
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        int is_presync =
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
             server.rdb_child_type != RDB_CHILD_TYPE_SOCKET));

        if (is_presync) {
            connWrite(slave->conn, "\n", 1);
        }
    }

    /* Disconnect timedout slaves. 
     *
     * 断开定时从节点的连接。
     * */
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_ONLINE) {
                if (slave->flags & CLIENT_PRE_PSYNC)
                    continue;
                if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout) {
                    serverLog(LL_WARNING, "Disconnecting timedout replica (streaming sync): %s",
                          replicationGetSlaveName(slave));
                    freeClient(slave);
                    continue;
                }
            }
            /* We consider disconnecting only diskless replicas because disk-based replicas aren't fed
             * by the fork child so if a disk-based replica is stuck it doesn't prevent the fork child
             * from terminating. 
             *
             * 我们考虑只断开无磁盘从节点的连接，因为基于磁盘的从节点不是由fork子代提供的，所以
             * 如果基于磁盘的复制从节点被卡住，它不会阻止fork子进程终止。
             * */
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END && server.rdb_child_type == RDB_CHILD_TYPE_SOCKET) {
                if (slave->repl_last_partial_write != 0 &&
                    (server.unixtime - slave->repl_last_partial_write) > server.repl_timeout)
                {
                    serverLog(LL_WARNING, "Disconnecting timedout replica (full sync): %s",
                          replicationGetSlaveName(slave));
                    freeClient(slave);
                    continue;
                }
            }
        }
    }

    /* If this is a master without attached slaves and there is a replication
     * backlog active, in order to reclaim memory we can free it after some
     * (configured) time. Note that this cannot be done for slaves: slaves
     * without sub-slaves attached should still accumulate data into the
     * backlog, in order to reply to PSYNC queries if they are turned into
     * masters after a failover. 
     *
     * 如果这是一个没有连接从节点服务器的主服务器，并且有一个活动的同步缓冲区，为了回收内存，
     * 我们可以在一段（配置的）时间后释放它。请注意，这不能用于从节点服务器：没有连接子
     * 从节点服务器的从节点服务器仍应将数据累积到缓冲区中，以便在故障转移后将PSYNC查询转
     * 换为主服务器时对其进行回复。
     * */
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog && server.masterhost == NULL)
    {
        time_t idle = server.unixtime - server.repl_no_slaves_since;

        if (idle > server.repl_backlog_time_limit) {
            /* When we free the backlog, we always use a new
             * replication ID and clear the ID2. This is needed
             * because when there is no backlog, the master_repl_offset
             * is not updated, but we would still retain our replication
             * ID, leading to the following problem:
             *
             * 1. We are a master instance.
             * 2. Our slave is promoted to master. It's repl-id-2 will
             *    be the same as our repl-id.
             * 3. We, yet as master, receive some updates, that will not
             *    increment the master_repl_offset.
             * 4. Later we are turned into a slave, connect to the new
             *    master that will accept our PSYNC request by second
             *    replication ID, but there will be data inconsistency
             *    because we received writes. 
             *
             * 当我们释放积压工作时，我们总是使用一个新的复制ID并清除ID2。这是必要的，因为
             * 当没有缓冲区时，master_repl_offset不会更新，但我们仍然会保留复制
             * ID，从而导致以下问题：
             *
             * 1。我们是一个典型的例子。
             *
             * 2.我们的从节点被提升为主节点。它的repl-id-2将与我们的repl-id相同。
             *
             * 3.作为主控，我们收到了一些更新，这些更新不会增加主控_repl_offset。
             *
             * 4.后来我们变成了一个从服务器，连接到新的主节点，该主节点将通过第二个复制ID接受我们的PSYNC请求，但由于我们
             *   收到了写入，因此会出现数据不一致。
             * */
            changeReplicationId();
            clearReplicationId2();
            freeReplicationBacklog();
            serverLog(LL_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected replicas.",
                (int) server.repl_backlog_time_limit);
        }
    }

    /* If AOF is disabled and we no longer have attached slaves, we can
     * free our Replication Script Cache as there is no need to propagate
     * EVALSHA at all. 
     *
     * 如果AOF被禁用，并且我们不再连接从节点服务器，我们可以释放复制脚本缓存，因为根本
     * 不需要传播EVALSHA。
     * */
    if (listLength(server.slaves) == 0 &&
        server.aof_state == AOF_OFF &&
        listLength(server.repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }

    /* Start a BGSAVE good for replication if we have slaves in
     * WAIT_BGSAVE_START state.
     *
     * In case of diskless replication, we make sure to wait the specified
     * number of seconds (according to configuration) so that other slaves
     * have the time to arrive before we start streaming. 
     *
     * 如果从节点处于WAIT_BGSAVE_Start状态，则启动一个适合复制的BGSAVE。
     * 在无盘复制的情况下，我们确保等待指定的秒数（根据配置），以便其他从节点服务器
     * 在我们开始流之前有时间到达。
     * */
    if (!hasActiveChildProcess()) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        int mincapa = -1;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                idle = server.unixtime - slave->lastinteraction;
                if (idle > max_idle) max_idle = idle;
                slaves_waiting++;
                mincapa = (mincapa == -1) ? slave->slave_capa :
                                            (mincapa & slave->slave_capa);
            }
        }

        if (slaves_waiting &&
            (!server.repl_diskless_sync ||
             max_idle > server.repl_diskless_sync_delay))
        {
            /* Start the BGSAVE. The called function may start a
             * BGSAVE with socket target or disk target depending on the
             * configuration and slaves capabilities. 
             *
             * 启动BGSAVE。被调用的函数可以根据配置和从节点功能启动带有套接字目标或磁盘目标
             * 的BGSAVE。
             * */
            startBgsaveForReplication(mincapa);
        }
    }

    /* Remove the RDB file used for replication if Redis is not running
     * with any persistence. 
     *
     * 如果Redis没有以任何持久性运行，请删除用于复制的RDB文件。
     * */
    removeRDBUsedToSyncReplicas();

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. 
     *
     * 刷新滞后的从节点数量<=最小从节点最大滞后。
     * */
    refreshGoodSlavesCount();
    replication_cron_loops++; /* Incremented with frequency 1 HZ. 
                               *
                               * 频率为1HZ时递增。
                               * */
}
