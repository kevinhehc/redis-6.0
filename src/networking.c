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
#include "atomicvar.h"
#include "cluster.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <math.h>
#include <ctype.h>

static void setProtocolError(const char *errstr, client *c);
int postponeClientRead(client *c);
int ProcessingEventsWhileBlocked = 0; /* See processEventsWhileBlocked(). */

/* Return the size consumed from the allocator, for the specified SDS string,
 * including internal fragmentation. This function is used in order to compute
 * the client output buffer size.
 *
 * 返回指定 SDS 字符串的分配器消耗的大小，包括内部碎片。此函数用于计算客户端输出缓冲区大小。
 * */
size_t sdsZmallocSize(sds s) {
    void *sh = sdsAllocPtr(s);
    return zmalloc_size(sh);
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. This includes internal fragmentation.
 *
 * 返回 sds 字符串在对象>ptr 处为字符串对象使用的内存量。这包括内部碎片。
 * */
size_t getStringObjectSdsUsedMemory(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    switch(o->encoding) {
    case OBJ_ENCODING_RAW: return sdsZmallocSize(o->ptr);
    case OBJ_ENCODING_EMBSTR: return zmalloc_size(o)-sizeof(robj);
    default: return 0; /* Just integer encoding for now. 目前只是整数编码。*/
    }
}

/* Return the length of a string object.
 * This does NOT includes internal fragmentation or sds unused space.
 *
 * 返回字符串对象的长度。这不包括内部碎片或 sds 未使用的空间。
 * */
size_t getStringObjectLen(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    switch(o->encoding) {
    case OBJ_ENCODING_RAW: return sdslen(o->ptr);
    case OBJ_ENCODING_EMBSTR: return sdslen(o->ptr);
    default: return 0; /* Just integer encoding for now. */
    }
}

/* Client.reply list dup and free methods.
 *
 * 客户端回复列表 dup 和 free 方法。
 * */
void *dupClientReplyValue(void *o) {
    clientReplyBlock *old = o;
    clientReplyBlock *buf = zmalloc(sizeof(clientReplyBlock) + old->size);
    memcpy(buf, o, sizeof(clientReplyBlock) + old->size);
    return buf;
}

void freeClientReplyValue(void *o) {
    zfree(o);
}

int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

/* This function links the client to the global linked list of clients.
 * unlinkClient() does the opposite, among other things.
 *
 * 此函数将客户端链接到客户端的全局链表。unlinkClient（） 做了相反的事情。
 * */
void linkClient(client *c) {
    listAddNodeTail(server.clients,c);
    /* Note that we remember the linked list node where the client is stored,
     * this way removing the client in unlinkClient() will not require
     * a linear scan, but just a constant time operation.
     *
     * 请注意，我们记住了存储客户端的链表节点，这样在 unlinkClient（） 中删除客户端将不需要线性扫描，而只需要一个常量时间操作。
     * */
    c->client_list_node = listLast(server.clients);
    uint64_t id = htonu64(c->id);
    raxInsert(server.clients_index,(unsigned char*)&id,sizeof(id),c,NULL);
}

int authRequired(client *c) {
    /* Check if the user is authenticated. This check is skipped in case
     * the default user is flagged as "nopass" and is active.
     *
     * 检查用户是否已通过身份验证。如果默认用户被标记为“nopass”并且处于活动状态，则会跳过此检查。
     * */
    int auth_required = (!(DefaultUser->flags & USER_FLAG_NOPASS) ||
                          (DefaultUser->flags & USER_FLAG_DISABLED)) &&
                        !c->authenticated;
    return auth_required;
}

// 创建新的客户端实例
client *createClient(connection *conn) {
    client *c = zmalloc(sizeof(client));

    /* passing NULL as conn it is possible to create a non connected client.
     * This is useful since all the commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client.
     *
     * 将 NULL 作为连接传递，可以创建未连接的客户端。这很有用，因为所有命令都需要在客户端的上下文中执行。
     * 当命令在其他上下文中执行时（例如Lua脚本），我们需要一个未连接的客户端。
     * */
    if (conn) {
        connNonBlock(conn);
        connEnableTcpNoDelay(conn);
        if (server.tcpkeepalive)
            connKeepAlive(conn,server.tcpkeepalive);
        // 设置读处理函数
        connSetReadHandler(conn, readQueryFromClient);
        // 设置数据
        connSetPrivateData(conn, c);
    }

    // 数据库
    selectDb(c,0);
    uint64_t client_id = ++server.next_client_id;
    c->id = client_id;
    c->resp = 2;
    c->conn = conn;
    c->name = NULL;

    // 查询缓存相关的设置
    c->bufpos = 0;
    c->qb_pos = 0;
    c->querybuf = sdsempty();
    c->pending_querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->reqtype = 0;

    // 命令及参数
    c->argc = 0;
    c->argv = NULL;
    c->argv_len_sum = 0;
    c->cmd = c->lastcmd = NULL;
    c->user = DefaultUser;

    // 回复
    c->multibulklen = 0;
    c->bulklen = -1;
    c->sentlen = 0;
    // 状态
    c->flags = 0;
    // 用于 LRU 和 IDLE 计算
    c->ctime = c->lastinteraction = server.unixtime;
    /* If the default user does not require authentication, the user is
     * directly authenticated.
     *
     * 如果默认用户不需要身份验证，则直接对用户进行身份验证。
     * */
    // 身份验证
    c->authenticated = (c->user->flags & USER_FLAG_NOPASS) &&
                       !(c->user->flags & USER_FLAG_DISABLED);
    // REPL 状态
    c->replstate = REPL_STATE_NONE;
    c->repl_put_online_on_ack = 0;
    c->reploff = 0;
    c->read_reploff = 0;
    c->repl_ack_off = 0;
    c->repl_ack_time = 0;
    c->repl_last_partial_write = 0;
    // 附属监听端口
    c->slave_listening_port = 0;
    c->slave_ip[0] = '\0';
    c->slave_capa = SLAVE_CAPA_NONE;
    // 回复
    c->reply = listCreate();
    c->reply_bytes = 0;
    // 软缓存超限提醒
    c->obuf_soft_limit_reached_time = 0;

    // 回复处理函数
    listSetFreeMethod(c->reply,freeClientReplyValue);
    listSetDupMethod(c->reply,dupClientReplyValue);

    c->btype = BLOCKED_NONE;

    // 阻塞 POP 相关
    c->bpop.timeout = 0;
    c->bpop.keys = dictCreate(&objectKeyHeapPointerValueDictType,NULL);
    c->bpop.target = NULL;
    c->bpop.xread_group = NULL;
    c->bpop.xread_consumer = NULL;
    c->bpop.xread_group_noack = 0;
    c->bpop.numreplicas = 0;
    c->bpop.reploffset = 0;
    c->woff = 0;

    // 所有被监视的键
    c->watched_keys = listCreate();

    // pubsub
    c->pubsub_channels = dictCreate(&objectKeyPointerValueDictType,NULL);
    c->pubsub_patterns = listCreate();
    c->peerid = NULL;
    c->client_list_node = NULL;
    c->client_tracking_redirection = 0;
    c->client_tracking_prefixes = NULL;
    c->client_cron_last_memory_usage = 0;
    c->client_cron_last_memory_type = CLIENT_TYPE_NORMAL;
    c->auth_callback = NULL;
    c->auth_callback_privdata = NULL;
    c->auth_module = NULL;
    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    if (conn) linkClient(c);

    // 初始化事务状态
    initClientMultiState(c);
    return c;
}

/* This function puts the client in the queue of clients that should write
 * their output buffers to the socket. Note that it does not *yet* install
 * the write handler, to start clients are put in a queue of clients that need
 * to write, so we try to do that before returning in the event loop (see the
 * handleClientsWithPendingWrites() function).
 * If we fail and there is more data to write, compared to what the socket
 * buffers can hold, then we'll really install the handler.
 *
 * 此函数将客户端置于应将其输出缓冲区写入套接字的客户端队列中。
 * 请注意，它*尚未*安装写入处理程序，启动客户端被放入需要写入的客户端队列中，
 * 因此我们尝试在事件循环返回之前执行此操作（请参阅 handleClientsWithPendingWrites（） 函数）。
 * 如果我们失败并且有更多的数据要写入，与套接字缓冲区可以容纳的数据相比，那么我们将真正安装处理程序。
 * */
void clientInstallWriteHandler(client *c) {
    /* Schedule the client to write the output buffers to the socket only
     * if not already done and, for slaves, if the slave can actually receive
     * writes at this stage.
     *
     * 调度客户端仅在尚未完成时才将输出缓冲区写入套接字，对于从站，如果从站在此阶段实际可以接收写入。
     * */
    if (!(c->flags & CLIENT_PENDING_WRITE) &&
        (c->replstate == REPL_STATE_NONE ||
         (c->replstate == SLAVE_STATE_ONLINE && !c->repl_put_online_on_ack)))
    {
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once.
         *
         * 在这里，我们只是标记客户端并将其放入要写入套接字的客户端列表中，而不是安装写入处理程序。
         * 这样，在重新进入事件循环之前，我们可以尝试直接写入客户端套接字，避免系统调用。
         * 只有当我们无法一次编写整个回复时，我们才会真正安装写入处理程序。
         * */
        c->flags |= CLIENT_PENDING_WRITE;
        listAddNodeHead(server.clients_pending_write,c);
    }
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * 每次我们要向客户端传输新数据时都会调用此函数。行为如下：
 *
 * If the client should receive new data (normal clients will) the function
 * returns C_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * 如果客户端应该接收新数据（普通客户端会），该函数将返回C_OK，
 * 并确保在我们的事件循环中安装写入处理程序，以便在套接字可写时写入新数据。
 *
 * If the client should not receive new data, because it is a fake client
 * (used to load AOF in memory), a master or because the setup of the write
 * handler failed, the function returns C_ERR.
 *
 * 如果客户端不应该接收新数据，因为它是假客户端（用于在内存中加载 AOF）、主客户端或因为写处理程序的设置失败，则该函数返回C_ERR。
 *
 * The function may return C_OK without actually installing the write
 * event handler in the following cases:
 *
 * 在以下情况下，该函数可能会返回C_OK而无需实际安装 write 事件处理程序：
 *
 * 1) The event handler should already be installed since the output buffer
 *    already contains something.
 *    事件处理程序应该已经安装，因为输出缓冲区已经包含某些内容。
 * 2) The client is a slave but not yet online, so we want to just accumulate
 *    writes in the buffer but not actually sending them yet.
 *    客户端是服务器，但尚未联机，因此我们只想在缓冲区中累积写入，但实际上还没有发送它们。
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns C_ERR no
 * data should be appended to the output buffers.
 *
 * 通常在每次生成回复时调用，然后再向客户端输出缓冲区添加更多数据。如果函数返回C_ERR则不应将任何数据追加到输出缓冲区。
 *
 * */
int prepareClientToWrite(client *c) {
    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all.
     *
     * 如果是 Lua 客户端，我们总是返回 ok 而不安装任何处理程序，因为根本没有套接字。
     * */
    if (c->flags & (CLIENT_LUA|CLIENT_MODULE)) return C_OK;

    /* If CLIENT_CLOSE_ASAP flag is set, we need not write anything.
     *
     * 如果设置了CLIENT_CLOSE_ASAP标志，我们不需要写任何东西。
     * */
    if (c->flags & CLIENT_CLOSE_ASAP) return C_ERR;

    /* CLIENT REPLY OFF / SKIP handling: don't send replies.
     * CLIENT_PUSHING handling: disables the reply silencing flags.
     *
     * 客户端回复关闭/跳过处理：不发送回复。CLIENT_PUSHING处理：禁用回复静音标志。
     * */
    if ((c->flags & (CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP)) &&
        !(c->flags & CLIENT_PUSHING)) return C_ERR;

    /* Masters don't receive replies, unless CLIENT_MASTER_FORCE_REPLY flag
     * is set.
     *
     * 大师不会收到回复，除非设置了CLIENT_MASTER_FORCE_REPLY标志。
     * */
    if ((c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_MASTER_FORCE_REPLY)) return C_ERR;

    if (!c->conn) return C_ERR; /* Fake client for AOF loading. */

    /* Schedule the client to write the output buffers to the socket, unless
     * it should already be setup to do so (it has already pending data).
     *
     * If CLIENT_PENDING_READ is set, we're in an IO thread and should
     * not install a write handler. Instead, it will be done by
     * handleClientsWithPendingReadsUsingThreads() upon return.
     *
     * 安排客户端将输出缓冲区写入套接字，除非它应该已经设置为这样做（它已经有挂起的数据）。
     * 如果设置了CLIENT_PENDING_READ，则我们处于 IO 线程中，不应安装写入处理程序。
     * 相反，它将在返回时由handleClientsWithPendingReadsUsingThreads（）完成。
     */
    if (!clientHasPendingReplies(c) && !(c->flags & CLIENT_PENDING_READ))
            // 插入到 server的 clients_pending_write 等待写队列
            clientInstallWriteHandler(c);

    /* Authorize the caller to queue in the output buffer of this client.
     *
     * 授权调用方在此客户端的输出缓冲区中排队。
     * */
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * 用于向输出缓冲区添加更多数据的低级函数。
 * -------------------------------------------------------------------------- */

/* Attempts to add the reply to the static buffer in the client struct.
 * Returns C_ERR if the buffer is full, or the reply list is not empty,
 * in which case the reply must be added to the reply list.
 *
 * 尝试将回复添加到客户端结构中的静态缓冲区。如果缓冲区已满或回复列表不为空，则返回C_ERR，在这种情况下，必须将回复添加到回复列表中。
 * */
/*
 * 仅当 c->reply 链表中没有节点时，
 * 将回复 s 追加到 c->buf 的后部。
 *
 * 添加成功返回 REDIS_OK ，添加失败返回 REDIS_ERR 。
 */
int _addReplyToBuffer(client *c, const char *s, size_t len) {
    // 可用的缓存字节数
    size_t available = sizeof(c->buf)-c->bufpos;

    // 客户端将被关闭，无须回复
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return C_OK;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    // 回复列表中已节点存在，不将内容添加到缓存
    if (listLength(c->reply) > 0) return C_ERR;

    /* Check that the buffer has enough space available for this string. */
    // 如果缓存的空间不足以保存回复，直接返回
    if (len > available) return C_ERR;

    // 追加到缓存中
    memcpy(c->buf+c->bufpos,s,len);
    c->bufpos+=len;
    return C_OK;
}

/* Adds the reply to the reply linked list.
 * 将回复添加到回复链表。
 * Note: some edits to this function need to be relayed to AddReplyFromClient.
 * 注意：对此函数的某些编辑需要中继到 AddReplyFromClient。
 * */
// 将回复对象所保存的值添加到回复列表 c->reply 末尾
void _addReplyProtoToList(client *c, const char *s, size_t len) {

    // 服务端已被关闭
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return;

    listNode *ln = listLast(c->reply);
    clientReplyBlock *tail = ln? listNodeValue(ln): NULL;

    /* Note that 'tail' may be NULL even if we have a tail node, because when
     * addReplyDeferredLen() is used, it sets a dummy node to NULL just
     * fo fill it later, when the size of the bulk length is set.
     *
     * 请注意，即使我们有一个尾节点，“tail”也可能是NULL，因为当使用addReplyDeferredLen（）时，
     * 它会将虚拟节点设置为NULL，只是稍后在设置批量长度的大小时填充它。
     * */

    /* Append to tail string when possible.
     *
     * 尽可能附加到尾部的字符。
     * */
    if (tail) {
        /* Copy the part we can fit into the tail, and leave the rest for a
         * new node
         *
         * 复制我们可以放入尾部的部分，并将其余部分留给新节点
         * */
        size_t avail = tail->size - tail->used;
        size_t copy = avail >= len? len: avail;
        memcpy(tail->buf + tail->used, s, copy);
        tail->used += copy;
        s += copy;
        len -= copy;
    }
    if (len) {
        /* Create a new node, make sure it is allocated to at
         * least PROTO_REPLY_CHUNK_BYTES
         *
         * 创建一个新节点，确保它至少分配给PROTO_REPLY_CHUNK_BYTES
         * */
        size_t size = len < PROTO_REPLY_CHUNK_BYTES? PROTO_REPLY_CHUNK_BYTES: len;
        tail = zmalloc(size + sizeof(clientReplyBlock));
        /* take over the allocation's internal fragmentation
         *
         * 接管分配的内部碎片
         * */
        tail->size = zmalloc_usable(tail) - sizeof(clientReplyBlock);
        tail->used = len;
        memcpy(tail->buf, s, len);
        listAddNodeTail(c->reply, tail);
        c->reply_bytes += tail->size;
    }

    // 如果突破了客户端的最大缓存限制，那么关闭客户端
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 *
 * 用于在客户端输出缓冲区上排队数据的更高级别函数。以下函数是命令实现将调用的函数。
 * -------------------------------------------------------------------------- */

/* Add the object 'obj' string representation to the client output buffer.
 *
 * 将对象“obj”字符串表示形式添加到客户端输出缓冲区。
 * */
void addReply(client *c, robj *obj) {
    if (prepareClientToWrite(c) != C_OK) return;

    if (sdsEncodedObject(obj)) {
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != C_OK)
            _addReplyProtoToList(c,obj->ptr,sdslen(obj->ptr));
    } else if (obj->encoding == OBJ_ENCODING_INT) {
        /* For integer encoded strings we just convert it into a string
         * using our optimized function, and attach the resulting string
         * to the output buffer.
         *
         * 对于整数编码的字符串，我们只需使用优化的函数将其转换为字符串，并将结果字符串附加到输出缓冲区。
         * */
        char buf[32];
        size_t len = ll2string(buf,sizeof(buf),(long)obj->ptr);
        if (_addReplyToBuffer(c,buf,len) != C_OK)
            _addReplyProtoToList(c,buf,len);
    } else {
        serverPanic("Wrong obj->encoding in addReply()");
    }
}

/* Add the SDS 's' string to the client output buffer, as a side effect
 * the SDS string is freed.
 *
 * 将 SDS 's' 字符串添加到客户端输出缓冲区，作为副作用，SDS 字符串将被释放。
 * */
// 将 sds 添加到回复列表末尾
void addReplySds(client *c, sds s) {
    if (prepareClientToWrite(c) != C_OK) {
        /* The caller expects the sds to be free'd.
         *
         * 调用方希望 sds 是空闲的。
         * */
        sdsfree(s);
        return;
    }
    if (_addReplyToBuffer(c,s,sdslen(s)) != C_OK)
        _addReplyProtoToList(c,s,sdslen(s));
    sdsfree(s);
}

/* This low level function just adds whatever protocol you send it to the
 * client buffer, trying the static buffer initially, and using the string
 * of objects if not possible.
 *
 * 这个低级函数只是添加你发送到客户端缓冲区的任何协议，最初尝试静态缓冲区，如果不可能，则使用对象字符串。
 *
 *
 * It is efficient because does not create an SDS object nor an Redis object
 * if not needed. The object will only be created by calling
 * _addReplyProtoToList() if we fail to extend the existing tail object
 * in the list of objects.
 *
 * 它非常高效，因为不需要时不会创建 SDS 对象或 Redis 对象。
 * 只有当我们无法扩展对象列表中的现有 tail 对象时，
 * 才会通过调用 _addReplyProtoToList（） 来创建对象。
 * */
void addReplyProto(client *c, const char *s, size_t len) {
    if (prepareClientToWrite(c) != C_OK) return;
    if (_addReplyToBuffer(c,s,len) != C_OK)
        _addReplyProtoToList(c,s,len);
}

/* Low level function called by the addReplyError...() functions.
 * It emits the protocol for a Redis error, in the form:
 *
 * -ERRORCODE Error Message<CR><LF>
 *
 * If the error code is already passed in the string 's', the error
 * code provided is used, otherwise the string "-ERR " for the generic
 * error code is automatically added.
 * Note that 's' must NOT end with \r\n. */
void addReplyErrorLength(client *c, const char *s, size_t len) {
    /* If the string already starts with "-..." then the error code
     * is provided by the caller. Otherwise we use "-ERR". */
    if (!len || s[0] != '-') addReplyProto(c,"-ERR ",5);
    addReplyProto(c,s,len);
    addReplyProto(c,"\r\n",2);
}

/* Do some actions after an error reply was sent (Log if needed, updates stats, etc.) */
void afterErrorReply(client *c, const char *s, size_t len) {
    /* Sometimes it could be normal that a slave replies to a master with
     * an error and this function gets called. Actually the error will never
     * be sent because addReply*() against master clients has no effect...
     * A notable example is:
     *
     *    EVAL 'redis.call("incr",KEYS[1]); redis.call("nonexisting")' 1 x
     *
     * Where the master must propagate the first change even if the second
     * will produce an error. However it is useful to log such events since
     * they are rare and may hint at errors in a script or a bug in Redis. */
    int ctype = getClientType(c);
    if (ctype == CLIENT_TYPE_MASTER || ctype == CLIENT_TYPE_SLAVE || c->id == CLIENT_ID_AOF) {
        char *to, *from;

        if (c->id == CLIENT_ID_AOF) {
            to = "AOF-loading-client";
            from = "server";
        } else if (ctype == CLIENT_TYPE_MASTER) {
            to = "master";
            from = "replica";
        } else {
            to = "replica";
            from = "master";
        }

        if (len > 4096) len = 4096;
        char *cmdname = c->lastcmd ? c->lastcmd->name : "<unknown>";
        serverLog(LL_WARNING,"== CRITICAL == This %s is sending an error "
                             "to its %s: '%.*s' after processing the command "
                             "'%s'", from, to, (int)len, s, cmdname);
        if (ctype == CLIENT_TYPE_MASTER && server.repl_backlog &&
            server.repl_backlog_histlen > 0)
        {
            showLatestBacklog();
        }
        server.stat_unexpected_error_replies++;
    }
}

/* The 'err' object is expected to start with -ERRORCODE and end with \r\n.
 * Unlike addReplyErrorSds and others alike which rely on addReplyErrorLength. */
void addReplyErrorObject(client *c, robj *err) {
    addReply(c, err);
    afterErrorReply(c, err->ptr, sdslen(err->ptr)-2); /* Ignore trailing \r\n */
}

/* See addReplyErrorLength for expectations from the input string. */
void addReplyError(client *c, const char *err) {
    addReplyErrorLength(c,err,strlen(err));
    afterErrorReply(c,err,strlen(err));
}

/* See addReplyErrorLength for expectations from the input string. */
void addReplyErrorSds(client *c, sds err) {
    addReplyErrorLength(c,err,sdslen(err));
    afterErrorReply(c,err,sdslen(err));
}

/* See addReplyErrorLength for expectations from the formatted string.
 * The formatted string is safe to contain \r and \n anywhere. */
void addReplyErrorFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Trim any newlines at the end (ones will be added by addReplyErrorLength) */
    s = sdstrim(s, "\r\n");
    /* Make sure there are no newlines in the middle of the string, otherwise
     * invalid protocol is emitted. */
    s = sdsmapchars(s, "\r\n", "  ",  2);
    addReplyErrorLength(c,s,sdslen(s));
    afterErrorReply(c,s,sdslen(s));
    sdsfree(s);
}

void addReplyStatusLength(client *c, const char *s, size_t len) {
    addReplyProto(c,"+",1);
    addReplyProto(c,s,len);
    addReplyProto(c,"\r\n",2);
}

void addReplyStatus(client *c, const char *status) {
    addReplyStatusLength(c,status,strlen(status));
}

void addReplyStatusFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(c,s,sdslen(s));
    sdsfree(s);
}

/* Sometimes we are forced to create a new reply node, and we can't append to
 * the previous one, when that happens, we wanna try to trim the unused space
 * at the end of the last reply node which we won't use anymore. */
void trimReplyUnusedTailSpace(client *c) {
    listNode *ln = listLast(c->reply);
    clientReplyBlock *tail = ln? listNodeValue(ln): NULL;

    /* Note that 'tail' may be NULL even if we have a tail node, becuase when
     * addReplyDeferredLen() is used */
    if (!tail) return;

    /* We only try to trim the space is relatively high (more than a 1/4 of the
     * allocation), otherwise there's a high chance realloc will NOP.
     * Also, to avoid large memmove which happens as part of realloc, we only do
     * that if the used part is small.  */
    if (tail->size - tail->used > tail->size / 4 &&
        tail->used < PROTO_REPLY_CHUNK_BYTES)
    {
        size_t old_size = tail->size;
        tail = zrealloc(tail, tail->used + sizeof(clientReplyBlock));
        /* take over the allocation's internal fragmentation (at least for
         * memory usage tracking) */
        tail->size = zmalloc_usable(tail) - sizeof(clientReplyBlock);
        c->reply_bytes = c->reply_bytes + tail->size - old_size;
        listNodeValue(ln) = tail;
    }
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
void *addReplyDeferredLen(client *c) {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredAggregateLen() will be called. */
    if (prepareClientToWrite(c) != C_OK) return NULL;
    trimReplyUnusedTailSpace(c);
    listAddNodeTail(c->reply,NULL); /* NULL is our placeholder. */
    return listLast(c->reply);
}

/* Populate the length object and try gluing it to the next chunk. */
void setDeferredAggregateLen(client *c, void *node, long length, char prefix) {
    serverAssert(length >= 0);
    listNode *ln = (listNode*)node;
    clientReplyBlock *next;
    char lenstr[128];
    size_t lenstr_len = sprintf(lenstr, "%c%ld\r\n", prefix, length);

    /* Abort when *node is NULL: when the client should not accept writes
     * we return NULL in addReplyDeferredLen() */
    if (node == NULL) return;
    serverAssert(!listNodeValue(ln));

    /* Normally we fill this dummy NULL node, added by addReplyDeferredLen(),
     * with a new buffer structure containing the protocol needed to specify
     * the length of the array following. However sometimes when there is
     * little memory to move, we may instead remove this NULL node, and prefix
     * our protocol in the node immediately after to it, in order to save a
     * write(2) syscall later. Conditions needed to do it:
     *
     * - The next node is non-NULL,
     * - It has enough room already allocated
     * - And not too large (avoid large memmove) */
    if (ln->next != NULL && (next = listNodeValue(ln->next)) &&
        next->size - next->used >= lenstr_len &&
        next->used < PROTO_REPLY_CHUNK_BYTES * 4) {
        memmove(next->buf + lenstr_len, next->buf, next->used);
        memcpy(next->buf, lenstr, lenstr_len);
        next->used += lenstr_len;
        listDelNode(c->reply,ln);
    } else {
        /* Create a new node */
        clientReplyBlock *buf = zmalloc(lenstr_len + sizeof(clientReplyBlock));
        /* Take over the allocation's internal fragmentation */
        buf->size = zmalloc_usable(buf) - sizeof(clientReplyBlock);
        buf->used = lenstr_len;
        memcpy(buf->buf, lenstr, lenstr_len);
        listNodeValue(ln) = buf;
        c->reply_bytes += buf->size;
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

void setDeferredArrayLen(client *c, void *node, long length) {
    setDeferredAggregateLen(c,node,length,'*');
}

void setDeferredMapLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2) length *= 2;
    setDeferredAggregateLen(c,node,length,prefix);
}

void setDeferredSetLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    setDeferredAggregateLen(c,node,length,prefix);
}

void setDeferredAttributeLen(client *c, void *node, long length) {
    serverAssert(c->resp >= 3);
    setDeferredAggregateLen(c,node,length,'|');
}

void setDeferredPushLen(client *c, void *node, long length) {
    serverAssert(c->resp >= 3);
    setDeferredAggregateLen(c,node,length,'>');
}

/* Add a double as a bulk reply */
void addReplyDouble(client *c, double d) {
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (c->resp == 2) {
            addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
        } else {
            addReplyProto(c, d > 0 ? ",inf\r\n" : ",-inf\r\n",
                              d > 0 ? 6 : 7);
        }
    } else {
        char dbuf[MAX_LONG_DOUBLE_CHARS+3],
             sbuf[MAX_LONG_DOUBLE_CHARS+32];
        int dlen, slen;
        if (c->resp == 2) {
            dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
            slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
            addReplyProto(c,sbuf,slen);
        } else {
            dlen = snprintf(dbuf,sizeof(dbuf),",%.17g\r\n",d);
            addReplyProto(c,dbuf,dlen);
        }
    }
}

void addReplyBigNum(client *c, const char* num, size_t len) {
    if (c->resp == 2) {
        addReplyBulkCBuffer(c, num, len);
    } else {
        addReplyProto(c,"(",1);
        addReplyProto(c,num,len);
        addReply(c,shared.crlf);
    }
}

/* Add a long double as a bulk reply, but uses a human readable formatting
 * of the double instead of exposing the crude behavior of doubles to the
 * dear user. */
void addReplyHumanLongDouble(client *c, long double d) {
    if (c->resp == 2) {
        robj *o = createStringObjectFromLongDouble(d,1);
        addReplyBulk(c,o);
        decrRefCount(o);
    } else {
        char buf[MAX_LONG_DOUBLE_CHARS];
        int len = ld2string(buf,sizeof(buf),d,LD_STR_HUMAN);
        addReplyProto(c,",",1);
        addReplyProto(c,buf,len);
        addReplyProto(c,"\r\n",2);
    }
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */
void addReplyLongLongWithPrefix(client *c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReply(c,shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReply(c,shared.bulkhdr[ll]);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyProto(c,buf,len+3);
}

void addReplyLongLong(client *c, long long ll) {
    if (ll == 0)
        addReply(c,shared.czero);
    else if (ll == 1)
        addReply(c,shared.cone);
    else
        addReplyLongLongWithPrefix(c,ll,':');
}

void addReplyAggregateLen(client *c, long length, int prefix) {
    if (prefix == '*' && length < OBJ_SHARED_BULKHDR_LEN)
        addReply(c,shared.mbulkhdr[length]);
    else
        addReplyLongLongWithPrefix(c,length,prefix);
}

void addReplyArrayLen(client *c, long length) {
    addReplyAggregateLen(c,length,'*');
}

void addReplyMapLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2) length *= 2;
    addReplyAggregateLen(c,length,prefix);
}

void addReplySetLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    addReplyAggregateLen(c,length,prefix);
}

void addReplyAttributeLen(client *c, long length) {
    serverAssert(c->resp >= 3);
    addReplyAggregateLen(c,length,'|');
}

void addReplyPushLen(client *c, long length) {
    serverAssert(c->resp >= 3);
    serverAssertWithInfo(c, NULL, c->flags & CLIENT_PUSHING);
    addReplyAggregateLen(c,length,'>');
}

void addReplyNull(client *c) {
    if (c->resp == 2) {
        addReplyProto(c,"$-1\r\n",5);
    } else {
        addReplyProto(c,"_\r\n",3);
    }
}

void addReplyBool(client *c, int b) {
    if (c->resp == 2) {
        addReply(c, b ? shared.cone : shared.czero);
    } else {
        addReplyProto(c, b ? "#t\r\n" : "#f\r\n",4);
    }
}

/* A null array is a concept that no longer exists in RESP3. However
 * RESP2 had it, so API-wise we have this call, that will emit the correct
 * RESP2 protocol, however for RESP3 the reply will always be just the
 * Null type "_\r\n". */
void addReplyNullArray(client *c) {
    if (c->resp == 2) {
        addReplyProto(c,"*-1\r\n",5);
    } else {
        addReplyProto(c,"_\r\n",3);
    }
}

/* Create the length prefix of a bulk reply, example: $2234 */
void addReplyBulkLen(client *c, robj *obj) {
    size_t len = stringObjectLen(obj);

    if (len < OBJ_SHARED_BULKHDR_LEN)
        addReply(c,shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(c,len,'$');
}

/* Add a Redis Object as a bulk reply */
void addReplyBulk(client *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/* Add a C buffer as bulk reply */
void addReplyBulkCBuffer(client *c, const void *p, size_t len) {
    addReplyLongLongWithPrefix(c,len,'$');
    addReplyProto(c,p,len);
    addReply(c,shared.crlf);
}

/* Add sds to reply (takes ownership of sds and frees it) */
void addReplyBulkSds(client *c, sds s)  {
    addReplyLongLongWithPrefix(c,sdslen(s),'$');
    addReplySds(c,s);
    addReply(c,shared.crlf);
}

/* Add a C null term string as bulk reply */
void addReplyBulkCString(client *c, const char *s) {
    if (s == NULL) {
        addReplyNull(c);
    } else {
        addReplyBulkCBuffer(c,s,strlen(s));
    }
}

/* Add a long long as a bulk reply */
void addReplyBulkLongLong(client *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(c,buf,len);
}

/* Reply with a verbatim type having the specified extension.
 *
 * The 'ext' is the "extension" of the file, actually just a three
 * character type that describes the format of the verbatim string.
 * For instance "txt" means it should be interpreted as a text only
 * file by the receiver, "md " as markdown, and so forth. Only the
 * three first characters of the extension are used, and if the
 * provided one is shorter than that, the remaining is filled with
 * spaces. */
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext) {
    if (c->resp == 2) {
        addReplyBulkCBuffer(c,s,len);
    } else {
        char buf[32];
        size_t preflen = snprintf(buf,sizeof(buf),"=%zu\r\nxxx:",len+4);
        char *p = buf+preflen-4;
        for (int i = 0; i < 3; i++) {
            if (*ext == '\0') {
                p[i] = ' ';
            } else {
                p[i] = *ext++;
            }
        }
        addReplyProto(c,buf,preflen);
        addReplyProto(c,s,len);
        addReplyProto(c,"\r\n",2);
    }
}

/* Add an array of C strings as status replies with a heading.
 * This function is typically invoked by from commands that support
 * subcommands in response to the 'help' subcommand. The help array
 * is terminated by NULL sentinel. */
void addReplyHelp(client *c, const char **help) {
    sds cmd = sdsnew((char*) c->argv[0]->ptr);
    void *blenp = addReplyDeferredLen(c);
    int blen = 0;

    sdstoupper(cmd);
    addReplyStatusFormat(c,
        "%s <subcommand> arg arg ... arg. Subcommands are:",cmd);
    sdsfree(cmd);

    while (help[blen]) addReplyStatus(c,help[blen++]);

    blen++;  /* Account for the header line(s). */
    setDeferredArrayLen(c,blenp,blen);
}

/* Add a suggestive error reply.
 * This function is typically invoked by from commands that support
 * subcommands in response to an unknown subcommand or argument error. */
void addReplySubcommandSyntaxError(client *c) {
    sds cmd = sdsnew((char*) c->argv[0]->ptr);
    sdstoupper(cmd);
    addReplyErrorFormat(c,
        "Unknown subcommand or wrong number of arguments for '%s'. Try %s HELP.",
        (char*)c->argv[1]->ptr,cmd);
    sdsfree(cmd);
}

/* Append 'src' client output buffers into 'dst' client output buffers. 
 * This function clears the output buffers of 'src' */
void AddReplyFromClient(client *dst, client *src) {
    /* If the source client contains a partial response due to client output
     * buffer limits, propagate that to the dest rather than copy a partial
     * reply. We don't wanna run the risk of copying partial response in case
     * for some reason the output limits don't reach the same decision (maybe
     * they changed) */
    if (src->flags & CLIENT_CLOSE_ASAP) {
        sds client = catClientInfoString(sdsempty(),dst);
        freeClientAsync(dst);
        serverLog(LL_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
        return;
    }

    /* First add the static buffer (either into the static buffer or reply list) */
    addReplyProto(dst,src->buf, src->bufpos);

    /* We need to check with prepareClientToWrite again (after addReplyProto)
     * since addReplyProto may have changed something (like CLIENT_CLOSE_ASAP) */
    if (prepareClientToWrite(dst) != C_OK)
        return;

    /* We're bypassing _addReplyProtoToList, so we need to add the pre/post
     * checks in it. */
    if (dst->flags & CLIENT_CLOSE_AFTER_REPLY) return;

    /* Concatenate the reply list into the dest */
    if (listLength(src->reply))
        listJoin(dst->reply,src->reply);
    dst->reply_bytes += src->reply_bytes;
    src->reply_bytes = 0;
    src->bufpos = 0;

    /* Check output buffer limits */
    asyncCloseClientOnOutputBufferLimitReached(dst);
}

/* Copy 'src' client output buffers into 'dst' client output buffers.
 * The function takes care of freeing the old output buffers of the
 * destination client. */
void copyClientOutputBuffer(client *dst, client *src) {
    listRelease(dst->reply);
    dst->sentlen = 0;
    dst->reply = listDup(src->reply);
    memcpy(dst->buf,src->buf,src->bufpos);
    dst->bufpos = src->bufpos;
    dst->reply_bytes = src->reply_bytes;
}

/* Return true if the specified client has pending reply buffers to write to
 * the socket. */
int clientHasPendingReplies(client *c) {
    return c->bufpos || listLength(c->reply);
}

void clientAcceptHandler(connection *conn) {
    client *c = connGetPrivateData(conn);

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,
                "Error accepting a client connection: %s",
                connGetLastError(conn));
        freeClientAsync(c);
        return;
    }

    /* If the server is running in protected mode (the default) and there
     * is no password set, nor a specific interface is bound, we don't accept
     * requests from non loopback interfaces. Instead we try to explain the
     * user what to do to fix it if needed.
     *
     * 如果服务器在保护模式（默认）下运行，并且没有设置密码，也没有绑定特定接口，则不接受来自非环回接口的请求。
     * 相反，我们尝试向用户解释如果需要，该怎么做才能修复它。
     * */
    if (server.protected_mode &&
        server.bindaddr_count == 0 &&
        DefaultUser->flags & USER_FLAG_NOPASS &&
        !(c->flags & CLIENT_UNIX_SOCKET))
    {
        char cip[NET_IP_STR_LEN+1] = { 0 };
        connPeerToString(conn, cip, sizeof(cip)-1, NULL);

        if (strcmp(cip,"127.0.0.1") && strcmp(cip,"::1")) {
            char *err =
                "-DENIED Redis is running in protected mode because protected "
                "mode is enabled, no bind address was specified, no "
                "authentication password is requested to clients. In this mode "
                "connections are only accepted from the loopback interface. "
                "If you want to connect from external computers to Redis you "
                "may adopt one of the following solutions: "
                "1) Just disable protected mode sending the command "
                "'CONFIG SET protected-mode no' from the loopback interface "
                "by connecting to Redis from the same host the server is "
                "running, however MAKE SURE Redis is not publicly accessible "
                "from internet if you do so. Use CONFIG REWRITE to make this "
                "change permanent. "
                "2) Alternatively you can just disable the protected mode by "
                "editing the Redis configuration file, and setting the protected "
                "mode option to 'no', and then restarting the server. "
                "3) If you started the server manually just for testing, restart "
                "it with the '--protected-mode no' option. "
                "4) Setup a bind address or an authentication password. "
                "NOTE: You only need to do one of the above things in order for "
                "the server to start accepting connections from the outside.\r\n";
            if (connWrite(c->conn,err,strlen(err)) == -1) {
                /* Nothing to do, Just to avoid the warning... */
            }
            server.stat_rejected_conn++;
            freeClientAsync(c);
            return;
        }
    }

    server.stat_numconnections++;
    moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE,
                          REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED,
                          c);
}

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(connection *conn, int flags, char *ip) {
    client *c;
    char conninfo[100];
    UNUSED(ip);

    if (connGetState(conn) != CONN_STATE_ACCEPTING) {
        serverLog(LL_VERBOSE,
            "Accepted client connection in error state: %s (conn: %s)",
            connGetLastError(conn),
            connGetInfo(conn, conninfo, sizeof(conninfo)));
        connClose(conn);
        return;
    }

    /* Limit the number of connections we take at the same time.
     *
     * Admission control will happen before a client is created and connAccept()
     * called, because we don't want to even start transport-level negotiation
     * if rejected.
     *
     * 限制我们同时采用的连接数量。
     * 准入控制将在创建客户端和调用 connAccept（） 之前发生，因为如果被拒绝，我们甚至不想开始传输级协商。
     * */
    if (listLength(server.clients) + getClusterConnectionsCount()
        >= server.maxclients)
    {
        char *err;
        if (server.cluster_enabled)
            err = "-ERR max number of clients + cluster "
                  "connections reached\r\n";
        else
            err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors.
         * Note that for TLS connections, no handshake was done yet so nothing
         * is written and the connection will just drop.
         *
         * 这是尽力而为的错误消息，不要检查写入错误。请注意，对于 TLS 连接，尚未进行握手，因此不会写入任何内容，连接将断开。
         * */

        if (connWrite(conn,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        connClose(conn);
        return;
    }

    /* Create connection and client */
    // 创建连接和客户端


    if ((c = createClient(conn)) == NULL) {
        serverLog(LL_WARNING,
            "Error registering fd event for the new client: %s (conn: %s)",
            connGetLastError(conn),
            connGetInfo(conn, conninfo, sizeof(conninfo)));
        connClose(conn); /* May be already closed, just ignore errors 可能已经关闭，只需忽略错误 */
        return;
    }

    /* Last chance to keep flags 保留标志位的最后机会*/
    c->flags |= flags;

    /* Initiate accept.
     *
     * 启动接受。
     *
     * Note that connAccept() is free to do two things here:
     * 1. Call clientAcceptHandler() immediately;
     * 2. Schedule a future call to clientAcceptHandler().
     *
     * 请注意，connAccept（） 可以在这里自由地做两件事：
     *  1. 立即调用 clientAcceptHandler（）;
     *  2. 安排将来对 clientAcceptHandler（） 的调用。
     *
     * Because of that, we must do nothing else afterwards.
     * 正因为如此，我们以后什么也做不了。
     */
    if (connAccept(conn, clientAcceptHandler) == C_ERR) {
        char conninfo[100];
        if (connGetState(conn) == CONN_STATE_ERROR)
            serverLog(LL_WARNING,
                    "Error accepting a client connection: %s (conn: %s)",
                    connGetLastError(conn), connGetInfo(conn, conninfo, sizeof(conninfo)));
        freeClient(connGetPrivateData(conn));
        return;
    }
}


void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    // MAX_ACCEPTS_PER_CALL = 1000
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        // 接收链接请求
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        //
        acceptCommonHandler(connCreateAcceptedSocket(cfd),0,cip);
    }
}

void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptCommonHandler(connCreateAcceptedTLS(cfd, server.tls_auth_clients),0,cip);
    }
}

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted connection to %s", server.unixsocket);
        acceptCommonHandler(connCreateAcceptedSocket(cfd),CLIENT_UNIX_SOCKET,NULL);
    }
}

static void freeClientArgv(client *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
    c->argv_len_sum = 0;
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
void disconnectSlaves(void) {
    listIter li;
    listNode *ln;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        freeClient((client*)ln->value);
    }
}

/* Remove the specified client from global lists where the client could
 * be referenced, not including the Pub/Sub channels.
 * This is used by freeClient() and replicationCacheMaster(). */
void unlinkClient(client *c) {
    listNode *ln;

    /* If this is marked as current client unset it. */
    if (server.current_client == c) server.current_client = NULL;

    /* Certain operations must be done only if the client has an active connection.
     * If the client was already unlinked or if it's a "fake client" the
     * conn is already set to NULL. */
    if (c->conn) {
        /* Remove from the list of active clients. */
        if (c->client_list_node) {
            uint64_t id = htonu64(c->id);
            raxRemove(server.clients_index,(unsigned char*)&id,sizeof(id),NULL);
            listDelNode(server.clients,c->client_list_node);
            c->client_list_node = NULL;
        }

        /* Check if this is a replica waiting for diskless replication (rdb pipe),
         * in which case it needs to be cleaned from that list */
        if (c->flags & CLIENT_SLAVE &&
            c->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
            server.rdb_pipe_conns)
        {
            int i;
            for (i=0; i < server.rdb_pipe_numconns; i++) {
                if (server.rdb_pipe_conns[i] == c->conn) {
                    rdbPipeWriteHandlerConnRemoved(c->conn);
                    server.rdb_pipe_conns[i] = NULL;
                    break;
                }
            }
        }
        connClose(c->conn);
        c->conn = NULL;
    }

    /* Remove from the list of pending writes if needed. */
    if (c->flags & CLIENT_PENDING_WRITE) {
        ln = listSearchKey(server.clients_pending_write,c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_pending_write,ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
    }

    /* Remove from the list of pending reads if needed. */
    if (c->flags & CLIENT_PENDING_READ) {
        ln = listSearchKey(server.clients_pending_read,c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_pending_read,ln);
        c->flags &= ~CLIENT_PENDING_READ;
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    if (c->flags & CLIENT_UNBLOCKED) {
        ln = listSearchKey(server.unblocked_clients,c);
        serverAssert(ln != NULL);
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~CLIENT_UNBLOCKED;
    }

    /* Clear the tracking status. */
    if (c->flags & CLIENT_TRACKING) disableTracking(c);
}

// 释放客户端
void freeClient(client *c) {
    listNode *ln;

    /* If a client is protected, yet we need to free it right now, make sure
     * to at least use asynchronous freeing. */
    if (c->flags & CLIENT_PROTECTED) {
        freeClientAsync(c);
        return;
    }

    /* For connected clients, call the disconnection event of modules hooks. */
    if (c->conn) {
        moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE,
                              REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED,
                              c);
    }

    /* Notify module system that this client auth status changed. */
    moduleNotifyUserChanged(c);

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. Note that we need to do this here, because later
     * we may call replicationCacheMaster() and the client should already
     * be removed from the list of clients to free. */
    if (c->flags & CLIENT_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close,c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_to_close,ln);
    }

    /* If it is our master that's being disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    if (server.master && c->flags & CLIENT_MASTER) {
        serverLog(LL_WARNING,"Connection with master lost.");
        if (!(c->flags & (CLIENT_PROTOCOL_ERROR|CLIENT_BLOCKED))) {
            c->flags &= ~(CLIENT_CLOSE_ASAP|CLIENT_CLOSE_AFTER_REPLY);
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave */
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        serverLog(LL_WARNING,"Connection with replica %s lost.",
            replicationGetSlaveName(c));
    }

    /* Free the query buffer */
    sdsfree(c->querybuf);
    sdsfree(c->pending_querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    if (c->flags & CLIENT_BLOCKED) unblockClient(c);
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);

    /* Free data structures. */
    listRelease(c->reply);
    freeClientArgv(c);

    /* Unlink the client: this will close the socket, remove the I/O
     * handlers, and remove references of the client from different
     * places where active clients may be referenced. */
    unlinkClient(c);

    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    if (c->flags & CLIENT_SLAVE) {
        if (c->replstate == SLAVE_STATE_SEND_BULK) {
            if (c->repldbfd != -1) close(c->repldbfd);
            if (c->replpreamble) sdsfree(c->replpreamble);
        }
        list *l = (c->flags & CLIENT_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        serverAssert(ln != NULL);
        listDelNode(l,ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        if (getClientType(c) == CLIENT_TYPE_SLAVE && listLength(server.slaves) == 0)
            server.repl_no_slaves_since = server.unixtime;
        refreshGoodSlavesCount();
        /* Fire the replica change modules event. */
        if (c->replstate == SLAVE_STATE_ONLINE)
            moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                                  REDISMODULE_SUBEVENT_REPLICA_CHANGE_OFFLINE,
                                  NULL);
    }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    if (c->flags & CLIENT_MASTER) replicationHandleMasterDisconnection();

   /* Remove the contribution that this client gave to our
     * incrementally computed memory usage. */
    server.stat_clients_type_memory[c->client_cron_last_memory_type] -=
        c->client_cron_last_memory_usage;

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->name) decrRefCount(c->name);
    zfree(c->argv);
    c->argv_len_sum = 0;
    freeClientMultiState(c);
    sdsfree(c->peerid);
    zfree(c);
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
void freeClientAsync(client *c) {
    /* We need to handle concurrent access to the server.clients_to_close list
     * only in the freeClientAsync() function, since it's the only function that
     * may access the list while Redis uses I/O threads. All the other accesses
     * are in the context of the main thread while the other threads are
     * idle. */
    if (c->flags & CLIENT_CLOSE_ASAP || c->flags & CLIENT_LUA) return;
    c->flags |= CLIENT_CLOSE_ASAP;
    if (server.io_threads_num == 1) {
        /* no need to bother with locking if there's just one thread (the main thread) */
        listAddNodeTail(server.clients_to_close,c);
        return;
    }
    static pthread_mutex_t async_free_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&async_free_queue_mutex);
    listAddNodeTail(server.clients_to_close,c);
    pthread_mutex_unlock(&async_free_queue_mutex);
}

/* Free the clietns marked as CLOSE_ASAP, return the number of clients
 * freed. */
int freeClientsInAsyncFreeQueue(void) {
    int freed = 0;
    listIter li;
    listNode *ln;

    listRewind(server.clients_to_close,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_PROTECTED) continue;

        c->flags &= ~CLIENT_CLOSE_ASAP;
        freeClient(c);
        listDelNode(server.clients_to_close,ln);
        freed++;
    }
    return freed;
}

/* Return a client by ID, or NULL if the client ID is not in the set
 * of registered clients. Note that "fake clients", created with -1 as FD,
 * are not registered clients. */
client *lookupClientByID(uint64_t id) {
    id = htonu64(id);
    client *c = raxFind(server.clients_index,(unsigned char*)&id,sizeof(id));
    return (c == raxNotFound) ? NULL : c;
}

/* Write data in output buffers to client. Return C_OK if the client
 * is still valid after the call, C_ERR if it was freed because of some
 * error.  If handler_installed is set, it will attempt to clear the
 * write event.
 *
 * This function is called by threads, but always with handler_installed
 * set to 0. So when handler_installed is set to 0 the function must be
 * thread safe. */
int writeToClient(client *c, int handler_installed) {
    /* Update total number of writes on server */
    server.stat_total_writes_processed++;

    ssize_t nwritten = 0, totwritten = 0;
    size_t objlen;
    clientReplyBlock *o;

    while(clientHasPendingReplies(c)) {
        if (c->bufpos > 0) {
            nwritten = connWrite(c->conn,c->buf+c->sentlen,c->bufpos-c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            if ((int)c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else {
            o = listNodeValue(listFirst(c->reply));
            objlen = o->used;

            if (objlen == 0) {
                c->reply_bytes -= o->size;
                listDelNode(c->reply,listFirst(c->reply));
                continue;
            }

            nwritten = connWrite(c->conn, o->buf + c->sentlen, objlen - c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            if (c->sentlen == objlen) {
                c->reply_bytes -= o->size;
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                /* If there are no longer objects in the list, we expect
                 * the count of reply bytes to be exactly zero. */
                if (listLength(c->reply) == 0)
                    serverAssert(c->reply_bytes == 0);
            }
        }
        /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver.
         *
         * Moreover, we also send as much as possible if the client is
         * a slave or a monitor (otherwise, on high-speed traffic, the
         * replication/output buffer will grow indefinitely) */
        if (totwritten > NET_MAX_WRITES_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory) &&
            !(c->flags & CLIENT_SLAVE)) break;
    }
    server.stat_net_output_bytes += totwritten;
    if (nwritten == -1) {
        if (connGetState(c->conn) == CONN_STATE_CONNECTED) {
            nwritten = 0;
        } else {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", connGetLastError(c->conn));
            freeClientAsync(c);
            return C_ERR;
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & CLIENT_MASTER)) c->lastinteraction = server.unixtime;
    }
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;
        /* Note that writeToClient() is called in a threaded way, but
         * adDeleteFileEvent() is not thread safe: however writeToClient()
         * is always called with handler_installed set to 0 from threads
         * so we are fine.
         *
         * 请注意，writeToClient（） 是以线程方式调用的，
         * 但 adDeleteFileEvent（） 不是线程安全的：
         * 但是 writeToClient（） 总是在线程中handler_installed设置为 0 的情况下调用，所以我们很好。
         * */
        if (handler_installed) connSetWriteHandler(c->conn, NULL);

        /* Close connection after entire reply has been sent. */
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClientAsync(c);
            return C_ERR;
        }
    }
    return C_OK;
}

/* Write event handler. Just send data to the client. */
// 将所有回复发送到客户端
void sendReplyToClient(connection *conn) {
    client *c = connGetPrivateData(conn);
    writeToClient(c,1);
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth.
 *
 * 此函数在进入事件循环之前被调用，希望我们可以将回复写入客户端输出缓冲区，而无需使用 syscall 来安装可写事件处理程序、调用它等等。
 * */
int handleClientsWithPendingWrites(void) {
    listIter li;
    listNode *ln;
    int processed = listLength(server.clients_pending_write);

    listRewind(server.clients_pending_write,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
        listDelNode(server.clients_pending_write,ln);

        /* If a client is protected, don't do anything,
         * that may trigger write error or recreate handler. */
        if (c->flags & CLIENT_PROTECTED) continue;

        /* Don't write to clients that are going to be closed anyway. */
        if (c->flags & CLIENT_CLOSE_ASAP) continue;

        /* Try to write buffers to the client socket. */
        if (writeToClient(c,0) == C_ERR) continue;

        /* If after the synchronous writes above we still have data to
         * output to the client, we need to install the writable handler. */
        if (clientHasPendingReplies(c)) {
            int ae_barrier = 0;
            /* For the fsync=always policy, we want that a given FD is never
             * served for reading and writing in the same event loop iteration,
             * so that in the middle of receiving the query, and serving it
             * to the client, we'll call beforeSleep() that will do the
             * actual fsync of AOF to disk. the write barrier ensures that. */
            if (server.aof_state == AOF_ON &&
                server.aof_fsync == AOF_FSYNC_ALWAYS)
            {
                ae_barrier = 1;
            }
            if (connSetWriteHandlerWithBarrier(c->conn, sendReplyToClient, ae_barrier) == C_ERR) {
                freeClientAsync(c);
            }
        }
    }
    return processed;
}

/* resetClient prepare the client to process the next command */
// 重置客户端的参数信息，等待下次命令
void resetClient(client *c) {
    redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;

    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    if (!(c->flags & CLIENT_MULTI) && prevcmd != askingCommand)
        c->flags &= ~CLIENT_ASKING;

    /* We do the same for the CACHING command as well. It also affects
     * the next command or transaction executed, in a way very similar
     * to ASKING. */
    if (!(c->flags & CLIENT_MULTI) && prevcmd != clientCommand)
        c->flags &= ~CLIENT_TRACKING_CACHING;

    /* Remove the CLIENT_REPLY_SKIP flag if any so that the reply
     * to the next command will be sent, but set the flag if the command
     * we just processed was "CLIENT REPLY SKIP". */
    c->flags &= ~CLIENT_REPLY_SKIP;
    if (c->flags & CLIENT_REPLY_SKIP_NEXT) {
        c->flags |= CLIENT_REPLY_SKIP;
        c->flags &= ~CLIENT_REPLY_SKIP_NEXT;
    }
}

/* This function is used when we want to re-enter the event loop but there
 * is the risk that the client we are dealing with will be freed in some
 * way. This happens for instance in:
 *
 * * DEBUG RELOAD and similar.
 * * When a Lua script is in -BUSY state.
 *
 * So the function will protect the client by doing two things:
 *
 * 1) It removes the file events. This way it is not possible that an
 *    error is signaled on the socket, freeing the client.
 * 2) Moreover it makes sure that if the client is freed in a different code
 *    path, it is not really released, but only marked for later release. */
void protectClient(client *c) {
    c->flags |= CLIENT_PROTECTED;
    if (c->conn) {
        connSetReadHandler(c->conn,NULL);
        connSetWriteHandler(c->conn,NULL);
    }
}

/* This will undo the client protection done by protectClient() */
void unprotectClient(client *c) {
    if (c->flags & CLIENT_PROTECTED) {
        c->flags &= ~CLIENT_PROTECTED;
        if (c->conn) {
            connSetReadHandler(c->conn,readQueryFromClient);
            if (clientHasPendingReplies(c)) clientInstallWriteHandler(c);
        }
    }
}

/* Like processMultibulkBuffer(), but for the inline protocol instead of RESP,
 * this function consumes the client query buffer and creates a command ready
 * to be executed inside the client structure. Returns C_OK if the command
 * is ready to be executed, or C_ERR if there is still protocol to read to
 * have a well formed command. The function also returns C_ERR when there is
 * a protocol error: in such a case the client structure is setup to reply
 * with the error and close the connection. */
int processInlineBuffer(client *c) {
    char *newline;
    int argc, j, linefeed_chars = 1;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf+c->qb_pos,'\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
            addReplyError(c,"Protocol error: too big inline request");
            setProtocolError("too big inline request",c);
        }
        return C_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != c->querybuf+c->qb_pos && *(newline-1) == '\r')
        newline--, linefeed_chars++;

    /* Split the input buffer up to the \r\n */
    querylen = newline-(c->querybuf+c->qb_pos);
    aux = sdsnewlen(c->querybuf+c->qb_pos,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError("unbalanced quotes in inline request",c);
        return C_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && getClientType(c) == CLIENT_TYPE_SLAVE)
        c->repl_ack_time = server.unixtime;

    /* Masters should never send us inline protocol to run actual
     * commands. If this happens, it is likely due to a bug in Redis where
     * we got some desynchronization in the protocol, for example
     * beause of a PSYNC gone bad.
     *
     * However the is an exception: masters may send us just a newline
     * to keep the connection active. */
    if (querylen != 0 && c->flags & CLIENT_MASTER) {
        sdsfreesplitres(argv,argc);
        serverLog(LL_WARNING,"WARNING: Receiving inline protocol from master, master stream corruption? Closing the master connection and discarding the cached master.");
        setProtocolError("Master using the inline protocol. Desync?",c);
        return C_ERR;
    }

    /* Move querybuffer position to the next query in the buffer. */
    c->qb_pos += querylen+linefeed_chars;

    /* Setup argv array on client structure */
    if (argc) {
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*argc);
        c->argv_len_sum = 0;
    }

    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        c->argv[c->argc] = createObject(OBJ_STRING,argv[j]);
        c->argc++;
        c->argv_len_sum += sdslen(argv[j]);
    }
    zfree(argv);
    return C_OK;
}

/* Helper function. Record protocol erro details in server log,
 * and set the client as CLIENT_CLOSE_AFTER_REPLY and
 * CLIENT_PROTOCOL_ERROR. */
#define PROTO_DUMP_LEN 128
static void setProtocolError(const char *errstr, client *c) {
    if (server.verbosity <= LL_VERBOSE || c->flags & CLIENT_MASTER) {
        sds client = catClientInfoString(sdsempty(),c);

        /* Sample some protocol to given an idea about what was inside. */
        char buf[256];
        if (sdslen(c->querybuf)-c->qb_pos < PROTO_DUMP_LEN) {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%s'", c->querybuf+c->qb_pos);
        } else {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%.*s' (... more %zu bytes ...) '%.*s'", PROTO_DUMP_LEN/2, c->querybuf+c->qb_pos, sdslen(c->querybuf)-c->qb_pos-PROTO_DUMP_LEN, PROTO_DUMP_LEN/2, c->querybuf+sdslen(c->querybuf)-PROTO_DUMP_LEN/2);
        }

        /* Remove non printable chars. */
        char *p = buf;
        while (*p != '\0') {
            if (!isprint(*p)) *p = '.';
            p++;
        }

        /* Log all the client and protocol info. */
        int loglevel = (c->flags & CLIENT_MASTER) ? LL_WARNING :
                                                    LL_VERBOSE;
        serverLog(loglevel,
            "Protocol error (%s) from client: %s. %s", errstr, client, buf);
        sdsfree(client);
    }
    c->flags |= (CLIENT_CLOSE_AFTER_REPLY|CLIENT_PROTOCOL_ERROR);
}

/* Process the query buffer for client 'c', setting up the client argument
 * vector for command execution. Returns C_OK if after running the function
 * the client has a well-formed ready to be processed command, otherwise
 * C_ERR if there is still to read more buffer to get the full command.
 * The function also returns C_ERR when there is a protocol error: in such a
 * case the client structure is setup to reply with the error and close
 * the connection.
 *
 * This function is called if processInputBuffer() detects that the next
 * command is in RESP format, so the first byte in the command is found
 * to be '*'. Otherwise for inline commands processInlineBuffer() is called. */
int processMultibulkBuffer(client *c) {
    char *newline = NULL;
    int ok;
    long long ll;

    if (c->multibulklen == 0) {
        /* The client should have been reset */
        serverAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf+c->qb_pos,'\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                addReplyError(c,"Protocol error: too big mbulk count string");
                setProtocolError("too big mbulk count string",c);
            }
            return C_ERR;
        }

        /* Buffer should also contain \n */
        if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(sdslen(c->querybuf)-c->qb_pos-2))
            return C_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        serverAssertWithInfo(c,NULL,c->querybuf[c->qb_pos] == '*');
        ok = string2ll(c->querybuf+1+c->qb_pos,newline-(c->querybuf+1+c->qb_pos),&ll);
        if (!ok || ll > 1024*1024) {
            addReplyError(c,"Protocol error: invalid multibulk length");
            setProtocolError("invalid mbulk count",c);
            return C_ERR;
        } else if (ll > 10 && authRequired(c)) {
            addReplyError(c, "Protocol error: unauthenticated multibulk length");
            setProtocolError("unauth mbulk count", c);
            return C_ERR;
        }

        c->qb_pos = (newline-c->querybuf)+2;

        if (ll <= 0) return C_OK;

        c->multibulklen = ll;

        /* Setup argv array on client structure */
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
        c->argv_len_sum = 0;
    }

    serverAssertWithInfo(c,NULL,c->multibulklen > 0);
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf+c->qb_pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                    addReplyError(c,
                        "Protocol error: too big bulk count string");
                    setProtocolError("too big bulk count string",c);
                    return C_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(sdslen(c->querybuf)-c->qb_pos-2))
                break;

            if (c->querybuf[c->qb_pos] != '$') {
                addReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[c->qb_pos]);
                setProtocolError("expected $ but got something else",c);
                return C_ERR;
            }

            ok = string2ll(c->querybuf+c->qb_pos+1,newline-(c->querybuf+c->qb_pos+1),&ll);
            if (!ok || ll < 0 ||
                (!(c->flags & CLIENT_MASTER) && ll > server.proto_max_bulk_len)) {
                addReplyError(c,"Protocol error: invalid bulk length");
                setProtocolError("invalid bulk length",c);
                return C_ERR;
            } else if (ll > 16384 && authRequired(c)) {
                addReplyError(c, "Protocol error: unauthenticated bulk length");
                setProtocolError("unauth bulk length", c);
                return C_ERR;
            }

            c->qb_pos = newline-c->querybuf+2;
            if (ll >= PROTO_MBULK_BIG_ARG) {
                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data.
                 *
                 * But only when the data we have not parsed is less than
                 * or equal to ll+2. If the data length is greater than
                 * ll+2, trimming querybuf is just a waste of time, because
                 * at this time the querybuf contains not only our bulk. */
                if (sdslen(c->querybuf)-c->qb_pos <= (size_t)ll+2) {
                    sdsrange(c->querybuf,c->qb_pos,-1);
                    c->qb_pos = 0;
                    /* Hint the sds library about the amount of bytes this string is
                     * going to contain. */
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2-sdslen(c->querybuf));
                }
            }
            c->bulklen = ll;
        }

        /* Read bulk argument */
        if (sdslen(c->querybuf)-c->qb_pos < (size_t)(c->bulklen+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (c->qb_pos == 0 &&
                c->bulklen >= PROTO_MBULK_BIG_ARG &&
                sdslen(c->querybuf) == (size_t)(c->bulklen+2))
            {
                c->argv[c->argc++] = createObject(OBJ_STRING,c->querybuf);
                c->argv_len_sum += c->bulklen;
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsnewlen(SDS_NOINIT,c->bulklen+2);
                sdsclear(c->querybuf);
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+c->qb_pos,c->bulklen);
                c->argv_len_sum += c->bulklen;
                c->qb_pos += c->bulklen+2;
            }
            c->bulklen = -1;
            c->multibulklen--;
        }
    }

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) return C_OK;

    /* Still not ready to process the command */
    return C_ERR;
}

/* Perform necessary tasks after a command was executed:
 *
 * 1. The client is reset unless there are reasons to avoid doing it.
 * 2. In the case of master clients, the replication offset is updated.
 * 3. Propagate commands we got from our master to replicas down the line. */
void commandProcessed(client *c) {
    long long prev_offset = c->reploff;
    if (c->flags & CLIENT_MASTER && !(c->flags & CLIENT_MULTI)) {
        /* Update the applied replication offset of our master. */
        c->reploff = c->read_reploff - sdslen(c->querybuf) + c->qb_pos;
    }

    /* Don't reset the client structure for clients blocked in a
     * module blocking command, so that the reply callback will
     * still be able to access the client argv and argc field.
     * The client will be reset in unblockClientFromModule(). */
    if (!(c->flags & CLIENT_BLOCKED) ||
        c->btype != BLOCKED_MODULE)
    {
        resetClient(c);
    }

    /* If the client is a master we need to compute the difference
     * between the applied offset before and after processing the buffer,
     * to understand how much of the replication stream was actually
     * applied to the master state: this quantity, and its corresponding
     * part of the replication stream, will be propagated to the
     * sub-replicas and to the replication backlog. */
    if (c->flags & CLIENT_MASTER) {
        long long applied = c->reploff - prev_offset;
        if (applied) {
            replicationFeedSlavesFromMasterStream(server.slaves,
                    c->pending_querybuf, applied);
            sdsrange(c->pending_querybuf,applied,-1);
        }
    }
}

/* This function calls processCommand(), but also performs a few sub tasks
 * for the client that are useful in that context:
 *
 * 1. It sets the current client to the client 'c'.
 * 2. calls commandProcessed() if the command was handled.
 *
 * The function returns C_ERR in case the client was freed as a side effect
 * of processing the command, otherwise C_OK is returned.
 *
 * 此函数调用 processCommand（），但也为客户端执行一些在该上下文中有用的子任务：
 * 1. 它将当前客户端设置为客户端 'c'。
 * 2. 如果命令已处理，则调用命令已处理（）。
 * 该函数返回C_ERR以防客户端作为处理命令的副作用被释放，否则返回C_OK。
 *
 * */

int processCommandAndResetClient(client *c) {
    int deadclient = 0;
    server.current_client = c;
    if (processCommand(c) == C_OK) {
        commandProcessed(c);
    }
    if (server.current_client == NULL) deadclient = 1;
    server.current_client = NULL;
    /* freeMemoryIfNeeded may flush slave output buffers. This may
     * result into a slave, that may be the active client, to be
     * freed. */
    return deadclient ? C_ERR : C_OK;
}

/* This function will execute any fully parsed commands pending on
 * the client. Returns C_ERR if the client is no longer valid after executing
 * the command, and C_OK for all other cases. */
int processPendingCommandsAndResetClient(client *c) {
    if (c->flags & CLIENT_PENDING_COMMAND) {
        c->flags &= ~CLIENT_PENDING_COMMAND;
        if (processCommandAndResetClient(c) == C_ERR) {
            return C_ERR;
        }
    }
    return C_OK;
}

/* This function is called every time, in the client structure 'c', there is
 * more query buffer to process, because we read more data from the socket
 * or because a client was blocked and later reactivated, so there could be
 * pending query buffer, already representing a full command, to process.
 *
 * 每次调用此函数，在客户端结构“c”中，有更多的查询缓冲区需要处理，
 * 因为我们从套接字读取更多数据，或者因为客户端被阻止并稍后重新激活，
 * 因此可能存在挂起的查询缓冲区，已经表示完整的命令，要处理
 * */
void processInputBuffer(client *c) {
    /* Keep processing while there is something in the input buffer */
    // 当输入缓冲区中有内容时继续处理
    while(c->qb_pos < sdslen(c->querybuf)) {
        /* Return if clients are paused. */
        // 如果客户端已暂停，则返回
        if (!(c->flags & CLIENT_SLAVE) && 
            !(c->flags & CLIENT_PENDING_READ) && 
            clientsArePaused()) break;

        /* Immediately abort if the client is in the middle of something. */
        // 如果客户端正在处理某些事情，请立即中止。
        if (c->flags & CLIENT_BLOCKED) break;

        /* Don't process more buffers from clients that have already pending
         * commands to execute in c->argv. */
        // 不要处理来自已挂起的命令要在 c->argv 中执行的客户端的更多缓冲区。
        if (c->flags & CLIENT_PENDING_COMMAND) break;

        /* Don't process input from the master while there is a busy script
         * condition on the slave. We want just to accumulate the replication
         * stream (instead of replying -BUSY like we do with other clients) and
         * later resume the processing.
         *
         * 当从站上出现繁忙的脚本条件时，不要处理来自主站的输入。
         * 我们只想累积复制流（而不是像其他客户端那样回复 -BUSY ），然后恢复处理。
         *
         * */

        if (server.lua_timedout && c->flags & CLIENT_MASTER) break;

        /* CLIENT_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands).
         *
         * The same applies for clients we want to terminate ASAP.
         *
         * CLIENT_CLOSE_AFTER_REPLY 在将回复写入客户端后关闭连接。
         * 确保在设置此标志后不要让回复增长（即不要处理更多命令）。
         * 这同样适用于我们希望尽快终止的客户端。
         *
         * */
        if (c->flags & (CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP)) break;

        /* Determine request type when unknown. */
        // 确定未知的请求类型。
        if (!c->reqtype) {
            if (c->querybuf[c->qb_pos] == '*') {
                c->reqtype = PROTO_REQ_MULTIBULK;
            } else {
                c->reqtype = PROTO_REQ_INLINE;
            }
        }

        if (c->reqtype == PROTO_REQ_INLINE) {
            if (processInlineBuffer(c) != C_OK) break;
            /* If the Gopher mode and we got zero or one argument, process
             * the request in Gopher mode. To avoid data race, Redis won't
             * support Gopher if enable io threads to read queries.
             *
             * 如果 Gopher 模式和我们得到零个或一个参数，则在 Gopher 模式下处理请求。
             * 为了避免数据竞争，如果启用io线程读取查询，Redis将不支持Gopher。
             *
             * */
            // 前提是配置了 io 线程进行读写
            if (server.gopher_enabled && !server.io_threads_do_reads &&
                ((c->argc == 1 && ((char*)(c->argv[0]->ptr))[0] == '/') ||
                  c->argc == 0))
            {
                processGopherRequest(c);
                resetClient(c);
                c->flags |= CLIENT_CLOSE_AFTER_REPLY;
                break;
            }
        } else if (c->reqtype == PROTO_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != C_OK) break;
        } else {
            serverPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            resetClient(c);
        } else {
            /* If we are in the context of an I/O thread, we can't really
             * execute the command here. All we can do is to flag the client
             * as one that needs to process the command.
             *
             * 如果我们在 I/O 线程的上下文中，则无法在此处真正执行命令。我们所能做的就是将客户端标记为需要处理命令的客户端
             * */

            if (c->flags & CLIENT_PENDING_READ) {
                c->flags |= CLIENT_PENDING_COMMAND;
                break;
            }

            /* We are finally ready to execute the command. */
            // 我们终于准备好执行命令了。
            if (processCommandAndResetClient(c) == C_ERR) {
                /* If the client is no longer valid, we avoid exiting this
                 * loop and trimming the client buffer later. So we return
                 * ASAP in that case.
                 *
                 * 如果客户端不再有效，我们避免退出此循环并在以后修剪客户端缓冲区。因此，在这种情况下，我们会尽快返回。
                 * */
                return;
            }
        }
    }

    /* Trim to pos */
    // 修剪到位置
    if (c->qb_pos) {
        sdsrange(c->querybuf,c->qb_pos,-1);
        c->qb_pos = 0;
    }
}

// 2. 从客户端读
void readQueryFromClient(connection *conn) {
    client *c = connGetPrivateData(conn);
    int nread, readlen;
    size_t qblen;

    /* Check if we want to read from the client later when exiting from
     * the event loop. This is the case if threaded I/O is enabled. */
    // 是否推迟读，如果是的话 ，会把client句柄添加到 server.clients_pending_read 队列
    if (postponeClientRead(c)) return;

    /* Update total number of reads on server */
    // 更新服务器上的读取总数
    server.stat_total_reads_processed++;

    readlen = PROTO_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument.
     *
     * 如果这是一个多批量请求，并且我们正在处理足够大的批量回复，请尝试最大化查询缓冲区包含表示对象的 SDS 字符串的可能性，
     * 即使冒着需要更多 read（2） 调用的风险。这样，函数processMultiBulkBuffer（）可以避免复制缓冲区来创建表示参数的Redis对象。
     * */
    if (c->reqtype == PROTO_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= PROTO_MBULK_BIG_ARG)
    {
        ssize_t remaining = (size_t)(c->bulklen+2)-sdslen(c->querybuf);

        /* Note that the 'remaining' variable may be zero in some edge case,
         * for example once we resume a blocked client after CLIENT PAUSE.
         *
         * 请注意，在某些边缘情况下，“剩余”变量可能为零，例如，一旦我们在客户端暂停后恢复被阻止的客户端。
         * */
        if (remaining > 0 && remaining < readlen) readlen = remaining;
    }

    // 分配空间
    qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    // 读入到 buf
    nread = connRead(c->conn, c->querybuf+qblen, readlen);
    // 处理读错误值和 EOF （客户端已关闭）
    if (nread == -1) {
        if (connGetState(conn) == CONN_STATE_CONNECTED) {
            return;
        } else {
            serverLog(LL_VERBOSE, "Reading from client: %s",connGetLastError(c->conn));
            freeClientAsync(c);
            return;
        }
    } else if (nread == 0) {
        serverLog(LL_VERBOSE, "Client closed connection");
        freeClientAsync(c);
        return;
    } else if (c->flags & CLIENT_MASTER) {
        /* Append the query buffer to the pending (not applied) buffer
         * of the master. We'll use this buffer later in order to have a
         * copy of the string applied by the last command executed.
         *
         * 将查询缓冲区追加到主节点的挂起（未应用）缓冲区。我们稍后将使用此缓冲区，以便执行最后一个命令所应用的字符串副本。
         * */
        c->pending_querybuf = sdscatlen(c->pending_querybuf,
                                        c->querybuf+qblen,nread);
    }

    sdsIncrLen(c->querybuf,nread);
    c->lastinteraction = server.unixtime;
    if (c->flags & CLIENT_MASTER) c->read_reploff += nread;
    server.stat_net_input_bytes += nread;
    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        serverLog(LL_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClientAsync(c);
        return;
    }

    /* There is more data in the client input buffer, continue parsing it
     * in case to check if there is a full command to execute.
     *
     * 客户端输入缓冲区中有更多数据，请继续解析它，以防检查是否有完整的命令要执行。
     * */
     processInputBuffer(c);
}

void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer) {
    client *c;
    listNode *ln;
    listIter li;
    unsigned long lol = 0, bib = 0;

    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        c = listNodeValue(ln);

        if (listLength(c->reply) > lol) lol = listLength(c->reply);
        if (sdslen(c->querybuf) > bib) bib = sdslen(c->querybuf);
    }
    *longest_output_list = lol;
    *biggest_input_buffer = bib;
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of NET_PEER_ID_LEN bytes, including
 * the null term.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
void genClientPeerId(client *client, char *peerid,
                            size_t peerid_len) {
    if (client->flags & CLIENT_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",server.unixsocket);
    } else {
        /* TCP client. */
        connFormatPeer(client->conn,peerid,peerid_len);
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
char *getClientPeerId(client *c) {
    char peerid[NET_PEER_ID_LEN];

    if (c->peerid == NULL) {
        genClientPeerId(c,peerid,sizeof(peerid));
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

/* Concatenate a string representing the state of a client in a human
 * readable format, into the sds string 's'. */
sds catClientInfoString(sds s, client *client) {
    char flags[16], events[3], conninfo[CONN_INFO_LEN], *p;

    p = flags;
    if (client->flags & CLIENT_SLAVE) {
        if (client->flags & CLIENT_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & CLIENT_MASTER) *p++ = 'M';
    if (client->flags & CLIENT_PUBSUB) *p++ = 'P';
    if (client->flags & CLIENT_MULTI) *p++ = 'x';
    if (client->flags & CLIENT_BLOCKED) *p++ = 'b';
    if (client->flags & CLIENT_TRACKING) *p++ = 't';
    if (client->flags & CLIENT_TRACKING_BROKEN_REDIR) *p++ = 'R';
    if (client->flags & CLIENT_DIRTY_CAS) *p++ = 'd';
    if (client->flags & CLIENT_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & CLIENT_UNBLOCKED) *p++ = 'u';
    if (client->flags & CLIENT_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & CLIENT_UNIX_SOCKET) *p++ = 'U';
    if (client->flags & CLIENT_READONLY) *p++ = 'r';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    p = events;
    if (client->conn) {
        if (connHasReadHandler(client->conn)) *p++ = 'r';
        if (connHasWriteHandler(client->conn)) *p++ = 'w';
    }
    *p = '\0';

    /* Compute the total memory consumed by this client. */
    size_t obufmem = getClientOutputBufferMemoryUsage(client);
    size_t total_mem = obufmem;
    total_mem += zmalloc_size(client); /* includes client->buf */
    total_mem += sdsZmallocSize(client->querybuf);
    /* For efficiency (less work keeping track of the argv memory), it doesn't include the used memory
     * i.e. unused sds space and internal fragmentation, just the string length. but this is enough to
     * spot problematic clients. */
    total_mem += client->argv_len_sum;
    if (client->argv)
        total_mem += zmalloc_size(client->argv);

    return sdscatfmt(s,
        "id=%U addr=%s %s name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U argv-mem=%U obl=%U oll=%U omem=%U tot-mem=%U events=%s cmd=%s user=%s",
        (unsigned long long) client->id,
        getClientPeerId(client),
        connGetInfo(client->conn, conninfo, sizeof(conninfo)),
        client->name ? (char*)client->name->ptr : "",
        (long long)(server.unixtime - client->ctime),
        (long long)(server.unixtime - client->lastinteraction),
        flags,
        client->db->id,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (client->flags & CLIENT_MULTI) ? client->mstate.count : -1,
        (unsigned long long) sdslen(client->querybuf),
        (unsigned long long) sdsavail(client->querybuf),
        (unsigned long long) client->argv_len_sum,
        (unsigned long long) client->bufpos,
        (unsigned long long) listLength(client->reply),
        (unsigned long long) obufmem, /* should not include client->buf since we want to see 0 for static clients. */
        (unsigned long long) total_mem,
        events,
        client->lastcmd ? client->lastcmd->name : "NULL",
        client->user ? client->user->name : "(superuser)");
}

// 收集并返回所有客户端的状态信息
sds getAllClientsInfoString(int type) {
    listNode *ln;
    listIter li;
    client *client;
    sds o = sdsnewlen(SDS_NOINIT,200*listLength(server.clients));
    sdsclear(o);
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        if (type != -1 && getClientType(client) != type) continue;
        o = catClientInfoString(o,client);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

/* This function implements CLIENT SETNAME, including replying to the
 * user with an error if the charset is wrong (in that case C_ERR is
 * returned). If the function succeeeded C_OK is returned, and it's up
 * to the caller to send a reply if needed.
 *
 * Setting an empty string as name has the effect of unsetting the
 * currently set name: the client will remain unnamed.
 *
 * This function is also used to implement the HELLO SETNAME option. */
int clientSetNameOrReply(client *c, robj *name) {
    int len = sdslen(name->ptr);
    char *p = name->ptr;

    /* Setting the client name to an empty string actually removes
     * the current name. */
    if (len == 0) {
        if (c->name) decrRefCount(c->name);
        c->name = NULL;
        return C_OK;
    }

    /* Otherwise check if the charset is ok. We need to do this otherwise
     * CLIENT LIST format will break. You should always be able to
     * split by space to get the different fields. */
    for (int j = 0; j < len; j++) {
        if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
            addReplyError(c,
                "Client names cannot contain spaces, "
                "newlines or special characters.");
            return C_ERR;
        }
    }
    if (c->name) decrRefCount(c->name);
    c->name = name;
    incrRefCount(name);
    return C_OK;
}

/*
 * CLIENT 命令的实现
 *
 * 列出客户端的信息，或者对客户端进行操作。
 */
void clientCommand(client *c) {
    listNode *ln;
    listIter li;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ID                     -- Return the ID of the current connection.",
"GETNAME                -- Return the name of the current connection.",
"KILL <ip:port>         -- Kill connection made from <ip:port>.",
"KILL <option> <value> [option value ...] -- Kill connections. Options are:",
"     ADDR <ip:port>                      -- Kill connection made from <ip:port>",
"     TYPE (normal|master|replica|pubsub) -- Kill connections by type.",
"     USER <username>   -- Kill connections authenticated with such user.",
"     SKIPME (yes|no)   -- Skip killing current connection (default: yes).",
"LIST [options ...]     -- Return information about client connections. Options:",
"     TYPE (normal|master|replica|pubsub) -- Return clients of specified type.",
"PAUSE <timeout>        -- Suspend all Redis clients for <timout> milliseconds.",
"REPLY (on|off|skip)    -- Control the replies sent to the current connection.",
"SETNAME <name>         -- Assign the name <name> to the current connection.",
"UNBLOCK <clientid> [TIMEOUT|ERROR] -- Unblock the specified blocked client.",
"TRACKING (on|off) [REDIRECT <id>] [BCAST] [PREFIX first] [PREFIX second] [OPTIN] [OPTOUT]... -- Enable client keys tracking for client side caching.",
"CACHING  (yes|no)      -- Enable/Disable tracking of the keys for next command in OPTIN/OPTOUT mode.",
"GETREDIR               -- Return the client ID we are redirecting to when tracking is enabled.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"id") && c->argc == 2) {
        /* CLIENT ID */
        addReplyLongLong(c,c->id);
    } else if (!strcasecmp(c->argv[1]->ptr,"list")) {
        /* CLIENT LIST */
        int type = -1;
        if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr,"type")) {
            type = getClientTypeByName(c->argv[3]->ptr);
            if (type == -1) {
                addReplyErrorFormat(c,"Unknown client type '%s'",
                    (char*) c->argv[3]->ptr);
                return;
             }
        } else if (c->argc != 2) {
            addReply(c,shared.syntaxerr);
            return;
        }
        sds o = getAllClientsInfoString(type);
        addReplyVerbatim(c,o,sdslen(o),"txt");
        sdsfree(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"reply") && c->argc == 3) {
        /* CLIENT REPLY ON|OFF|SKIP */
        if (!strcasecmp(c->argv[2]->ptr,"on")) {
            c->flags &= ~(CLIENT_REPLY_SKIP|CLIENT_REPLY_OFF);
            addReply(c,shared.ok);
        } else if (!strcasecmp(c->argv[2]->ptr,"off")) {
            c->flags |= CLIENT_REPLY_OFF;
        } else if (!strcasecmp(c->argv[2]->ptr,"skip")) {
            if (!(c->flags & CLIENT_REPLY_OFF))
                c->flags |= CLIENT_REPLY_SKIP_NEXT;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        user *user = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        if (c->argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = c->argv[2]->ptr;
            skipme = 0; /* With the old form, you can kill yourself. 使用旧形式，您可以自杀。 */
        } else if (c->argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. 新样式语法：解析选项*/
            while(i < c->argc) {
                int moreargs = c->argc > i+1;

                if (!strcasecmp(c->argv[i]->ptr,"id") && moreargs) {
                    long long tmp;

                    if (getLongLongFromObjectOrReply(c,c->argv[i+1],&tmp,NULL)
                        != C_OK) return;
                    id = tmp;
                } else if (!strcasecmp(c->argv[i]->ptr,"type") && moreargs) {
                    type = getClientTypeByName(c->argv[i+1]->ptr);
                    if (type == -1) {
                        addReplyErrorFormat(c,"Unknown client type '%s'",
                            (char*) c->argv[i+1]->ptr);
                        return;
                    }
                } else if (!strcasecmp(c->argv[i]->ptr,"addr") && moreargs) {
                    addr = c->argv[i+1]->ptr;
                } else if (!strcasecmp(c->argv[i]->ptr,"user") && moreargs) {
                    user = ACLGetUserByName(c->argv[i+1]->ptr,
                                            sdslen(c->argv[i+1]->ptr));
                    if (user == NULL) {
                        addReplyErrorFormat(c,"No such user '%s'",
                            (char*) c->argv[i+1]->ptr);
                        return;
                    }
                } else if (!strcasecmp(c->argv[i]->ptr,"skipme") && moreargs) {
                    if (!strcasecmp(c->argv[i+1]->ptr,"yes")) {
                        skipme = 1;
                    } else if (!strcasecmp(c->argv[i+1]->ptr,"no")) {
                        skipme = 0;
                    } else {
                        addReply(c,shared.syntaxerr);
                        return;
                    }
                } else {
                    addReply(c,shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }

        /* Iterate clients killing all the matching clients.
         * 迭代客户端杀死所有匹配的客户端。
         * */
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *client = listNodeValue(ln);
            if (addr && strcmp(getClientPeerId(client),addr) != 0) continue;
            if (type != -1 && getClientType(client) != type) continue;
            if (id != 0 && client->id != id) continue;
            if (user && client->user != user) continue;
            if (c == client && skipme) continue;

            /* Kill it. */
            if (c == client) {
                close_this_client = 1;
            } else {
                freeClient(client);
            }
            killed++;
        }

        /* Reply according to old/new format.
         *
         * 根据旧/新格式回复。
         * */
        if (c->argc == 3) {
            if (killed == 0)
                addReplyError(c,"No such client");
            else
                addReply(c,shared.ok);
        } else {
            addReplyLongLong(c,killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers.
         *
         * 如果必须关闭此客户端，请仅在我们将回复排队到其输出缓冲区后将其标记为CLOSE_AFTER_REPLY。
         * */
        if (close_this_client) c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    } else if (!strcasecmp(c->argv[1]->ptr,"unblock") && (c->argc == 3 ||
                                                          c->argc == 4))
    {
        /* CLIENT UNBLOCK <id> [timeout|error] */
        long long id;
        int unblock_error = 0;

        if (c->argc == 4) {
            if (!strcasecmp(c->argv[3]->ptr,"timeout")) {
                unblock_error = 0;
            } else if (!strcasecmp(c->argv[3]->ptr,"error")) {
                unblock_error = 1;
            } else {
                addReplyError(c,
                    "CLIENT UNBLOCK reason should be TIMEOUT or ERROR");
                return;
            }
        }
        if (getLongLongFromObjectOrReply(c,c->argv[2],&id,NULL)
            != C_OK) return;
        struct client *target = lookupClientByID(id);
        if (target && target->flags & CLIENT_BLOCKED) {
            if (unblock_error)
                addReplyError(target,
                    "-UNBLOCKED client unblocked via CLIENT UNBLOCK");
            else
                replyToBlockedClientTimedOut(target);
            unblockClient(target);
            addReply(c,shared.cone);
        } else {
            addReply(c,shared.czero);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"setname") && c->argc == 3) {
        /* CLIENT SETNAME */
        if (clientSetNameOrReply(c,c->argv[2]) == C_OK)
            addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"getname") && c->argc == 2) {
        /* CLIENT GETNAME */
        if (c->name)
            addReplyBulk(c,c->name);
        else
            addReplyNull(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"pause") && c->argc == 3) {
        /* CLIENT PAUSE */
        long long duration;

        if (getTimeoutFromObjectOrReply(c,c->argv[2],&duration,
                UNIT_MILLISECONDS) != C_OK) return;
        pauseClients(duration);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"tracking") && c->argc >= 3) {
        /* CLIENT TRACKING (on|off) [REDIRECT <id>] [BCAST] [PREFIX first]
         *                          [PREFIX second] [OPTIN] [OPTOUT] ... */
        long long redir = 0;
        uint64_t options = 0;
        robj **prefix = NULL;
        size_t numprefix = 0;

        /* Parse the options. */
        for (int j = 3; j < c->argc; j++) {
            int moreargs = (c->argc-1) - j;

            if (!strcasecmp(c->argv[j]->ptr,"redirect") && moreargs) {
                j++;
                if (redir != 0) {
                    addReplyError(c,"A client can only redirect to a single "
                                    "other client");
                    zfree(prefix);
                    return;
                }

                if (getLongLongFromObjectOrReply(c,c->argv[j],&redir,NULL) !=
                    C_OK)
                {
                    zfree(prefix);
                    return;
                }
                /* We will require the client with the specified ID to exist
                 * right now, even if it is possible that it gets disconnected
                 * later. Still a valid sanity check.
                 *
                 * 我们将要求具有指定 ID 的客户端立即存在，即使它以后可能会断开连接。仍然是一个有效的健全性检查。
                 * */
                if (lookupClientByID(redir) == NULL) {
                    addReplyError(c,"The client ID you want redirect to "
                                    "does not exist");
                    zfree(prefix);
                    return;
                }
            } else if (!strcasecmp(c->argv[j]->ptr,"bcast")) {
                options |= CLIENT_TRACKING_BCAST;
            } else if (!strcasecmp(c->argv[j]->ptr,"optin")) {
                options |= CLIENT_TRACKING_OPTIN;
            } else if (!strcasecmp(c->argv[j]->ptr,"optout")) {
                options |= CLIENT_TRACKING_OPTOUT;
            } else if (!strcasecmp(c->argv[j]->ptr,"noloop")) {
                options |= CLIENT_TRACKING_NOLOOP;
            } else if (!strcasecmp(c->argv[j]->ptr,"prefix") && moreargs) {
                j++;
                prefix = zrealloc(prefix,sizeof(robj*)*(numprefix+1));
                prefix[numprefix++] = c->argv[j];
            } else {
                zfree(prefix);
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Options are ok: enable or disable the tracking for this client.
         *
         * 选项正常：启用或禁用此客户端的跟踪。
         * */
        if (!strcasecmp(c->argv[2]->ptr,"on")) {
            /* Before enabling tracking, make sure options are compatible
             * among each other and with the current state of the client.
             *
             * 在启用跟踪之前，请确保选项彼此兼容以及与客户端的当前状态兼容。
             * */
            if (!(options & CLIENT_TRACKING_BCAST) && numprefix) {
                addReplyError(c,
                    "PREFIX option requires BCAST mode to be enabled");
                zfree(prefix);
                return;
            }

            if (c->flags & CLIENT_TRACKING) {
                int oldbcast = !!(c->flags & CLIENT_TRACKING_BCAST);
                int newbcast = !!(options & CLIENT_TRACKING_BCAST);
                if (oldbcast != newbcast) {
                    addReplyError(c,
                    "You can't switch BCAST mode on/off before disabling "
                    "tracking for this client, and then re-enabling it with "
                    "a different mode.");
                    zfree(prefix);
                    return;
                }
            }

            if (options & CLIENT_TRACKING_BCAST &&
                options & (CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT))
            {
                addReplyError(c,
                "OPTIN and OPTOUT are not compatible with BCAST");
                zfree(prefix);
                return;
            }

            if (options & CLIENT_TRACKING_OPTIN && options & CLIENT_TRACKING_OPTOUT)
            {
                addReplyError(c,
                "You can't specify both OPTIN mode and OPTOUT mode");
                zfree(prefix);
                return;
            }

            if ((options & CLIENT_TRACKING_OPTIN && c->flags & CLIENT_TRACKING_OPTOUT) ||
                (options & CLIENT_TRACKING_OPTOUT && c->flags & CLIENT_TRACKING_OPTIN))
            {
                addReplyError(c,
                "You can't switch OPTIN/OPTOUT mode before disabling "
                "tracking for this client, and then re-enabling it with "
                "a different mode.");
                zfree(prefix);
                return;
            }

            enableTracking(c,redir,options,prefix,numprefix);
        } else if (!strcasecmp(c->argv[2]->ptr,"off")) {
            disableTracking(c);
        } else {
            zfree(prefix);
            addReply(c,shared.syntaxerr);
            return;
        }
        zfree(prefix);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"caching") && c->argc >= 3) {
        if (!(c->flags & CLIENT_TRACKING)) {
            addReplyError(c,"CLIENT CACHING can be called only when the "
                            "client is in tracking mode with OPTIN or "
                            "OPTOUT mode enabled");
            return;
        }

        char *opt = c->argv[2]->ptr;
        if (!strcasecmp(opt,"yes")) {
            if (c->flags & CLIENT_TRACKING_OPTIN) {
                c->flags |= CLIENT_TRACKING_CACHING;
            } else {
                addReplyError(c,"CLIENT CACHING YES is only valid when tracking is enabled in OPTIN mode.");
                return;
            }
        } else if (!strcasecmp(opt,"no")) {
            if (c->flags & CLIENT_TRACKING_OPTOUT) {
                c->flags |= CLIENT_TRACKING_CACHING;
            } else {
                addReplyError(c,"CLIENT CACHING NO is only valid when tracking is enabled in OPTOUT mode.");
                return;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }

        /* Common reply for when we succeeded.
         * 当我们成功时的共同回复。
         * */
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"getredir") && c->argc == 2) {
        /* CLIENT GETREDIR */
        if (c->flags & CLIENT_TRACKING) {
            addReplyLongLong(c,c->client_tracking_redirection);
        } else {
            addReplyLongLong(c,-1);
        }
    } else {
        addReplyErrorFormat(c, "Unknown subcommand or wrong number of arguments for '%s'. Try CLIENT HELP", (char*)c->argv[1]->ptr);
    }
}

/* HELLO <protocol-version> [AUTH <user> <password>] [SETNAME <name>] */
void helloCommand(client *c) {
    long long ver;

    if (getLongLongFromObject(c->argv[1],&ver) != C_OK ||
        ver < 2 || ver > 3)
    {
        addReplyError(c,"-NOPROTO unsupported protocol version");
        return;
    }

    for (int j = 2; j < c->argc; j++) {
        int moreargs = (c->argc-1) - j;
        const char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt,"AUTH") && moreargs >= 2) {
            if (ACLAuthenticateUser(c, c->argv[j+1], c->argv[j+2]) == C_ERR) {
                addReplyError(c,"-WRONGPASS invalid username-password pair");
                return;
            }
            j += 2;
        } else if (!strcasecmp(opt,"SETNAME") && moreargs) {
            if (clientSetNameOrReply(c, c->argv[j+1]) == C_ERR) return;
            j++;
        } else {
            addReplyErrorFormat(c,"Syntax error in HELLO option '%s'",opt);
            return;
        }
    }

    /* At this point we need to be authenticated to continue.
     *
     * 此时，我们需要进行身份验证才能继续。
     * */
    if (!c->authenticated) {
        addReplyError(c,"-NOAUTH HELLO must be called with the client already "
                        "authenticated, otherwise the HELLO AUTH <user> <pass> "
                        "option can be used to authenticate the client and "
                        "select the RESP protocol version at the same time");
        return;
    }

    /* Let's switch to the specified RESP mode.
     * 让我们切换到指定的 RESP 模式。
     * */
    c->resp = ver;
    addReplyMapLen(c,6 + !server.sentinel_mode);

    addReplyBulkCString(c,"server");
    addReplyBulkCString(c,"redis");

    addReplyBulkCString(c,"version");
    addReplyBulkCString(c,REDIS_VERSION);

    addReplyBulkCString(c,"proto");
    addReplyLongLong(c,ver);

    addReplyBulkCString(c,"id");
    addReplyLongLong(c,c->id);

    addReplyBulkCString(c,"mode");
    if (server.sentinel_mode) addReplyBulkCString(c,"sentinel");
    else if (server.cluster_enabled) addReplyBulkCString(c,"cluster");
    else addReplyBulkCString(c,"standalone");

    if (!server.sentinel_mode) {
        addReplyBulkCString(c,"role");
        addReplyBulkCString(c,server.masterhost ? "replica" : "master");
    }

    addReplyBulkCString(c,"modules");
    addReplyLoadedModules(c);
}

/* This callback is bound to POST and "Host:" command names. Those are not
 * really commands, but are used in security attacks in order to talk to
 * Redis instances via HTTP, with a technique called "cross protocol scripting"
 * which exploits the fact that services like Redis will discard invalid
 * HTTP headers and will process what follows.
 *
 * As a protection against this attack, Redis will terminate the connection
 * when a POST or "Host:" header is seen, and will log the event from
 * time to time (to avoid creating a DOS as a result of too many logs).
 *
 * 此回调绑定到 POST 和“主机：”命令名称。
 * 这些并不是真正的命令，而是用于安全攻击，以便通过HTTP与Redis实例通信，使用一种称为“跨协议脚本”的技术，
 * 该技术利用Redis等服务将丢弃无效的HTTP标头并处理以下内容的事实。为了防止这种攻击，
 * Redis 将在看到 POST 或“Host：”标头时终止连接，并将不时记录事件（以避免由于日志过多而创建 DOS）。
 * */
void securityWarningCommand(client *c) {
    static time_t logged_time;
    time_t now = time(NULL);

    if (labs(now-logged_time) > 60) {
        serverLog(LL_WARNING,"Possible SECURITY ATTACK detected. It looks like somebody is sending POST or Host: commands to Redis. This is likely due to an attacker attempting to use Cross Protocol Scripting to compromise your Redis instance. Connection aborted.");
        logged_time = now;
    }
    freeClientAsync(c);
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented.
 *
 * 重写客户端的命令向量。所有新对象的引用计数都会递增。释放旧的命令向量，并减少旧的对象引用计数。
 * */
void rewriteClientCommandVector(client *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector 新的参数向量*/

    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented.
     *
     * 我们在最后释放原始向量中的对象，因此我们可以确定，如果在新向量中重用相同的对象，则 refcount 在递减之前会递增。
     * */
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    zfree(c->argv);
    /* Replace argv and argc with our new versions.
     *
     * 将 argv 和 argc 替换为我们的新版本。
     * */
    c->argv = argv;
    c->argc = argc;
    c->argv_len_sum = 0;
    for (j = 0; j < c->argc; j++)
        if (c->argv[j])
            c->argv_len_sum += getStringObjectLen(c->argv[j]);
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    serverAssertWithInfo(c,NULL,c->cmd != NULL);
    va_end(ap);
}

/* Completely replace the client command vector with the provided one.
 * 将客户端命令向量完全替换为提供的命令向量。
 * */
void replaceClientCommandVector(client *c, int argc, robj **argv) {
    int j;
    freeClientArgv(c);
    zfree(c->argv);
    c->argv = argv;
    c->argc = argc;
    c->argv_len_sum = 0;
    for (j = 0; j < c->argc; j++)
        if (c->argv[j])
            c->argv_len_sum += getStringObjectLen(c->argv[j]);
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    serverAssertWithInfo(c,NULL,c->cmd != NULL);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented.
 *
 * 重写命令向量中的单个项目。新的 val 引用计数递增，旧值递减。
 *
 * It is possible to specify an argument over the current size of the
 * argument vector: in this case the array of objects gets reallocated
 * and c->argc set to the max value. However it's up to the caller to
 *
 * 可以在参数向量的当前大小上指定参数：在这种情况下，
 * 对象数组被重新分配，c->argc设置为最大值。但是，这取决于调用方
 *
 * 1. Make sure there are no "holes" and all the arguments are set.
 *    确保没有“漏洞”并且设置了所有参数。
 * 2. If the original argument vector was longer than the one we
 *    want to end with, it's up to the caller to set c->argc and
 *    free the no longer used objects on c->argv.
 *    如果原始参数向量比我们想要结束的参数向量长，
 *    则由调用方设置 c->argc 并释放 c->argv 上不再使用的对象。
 *
 *    */
void rewriteClientCommandArgument(client *c, int i, robj *newval) {
    robj *oldval;

    if (i >= c->argc) {
        c->argv = zrealloc(c->argv,sizeof(robj*)*(i+1));
        c->argc = i+1;
        c->argv[i] = NULL;
    }
    oldval = c->argv[i];
    if (oldval) c->argv_len_sum -= getStringObjectLen(oldval);
    if (newval) c->argv_len_sum += getStringObjectLen(newval);
    c->argv[i] = newval;
    incrRefCount(newval);
    if (oldval) decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd.
     *
     * 如果这是命令名称，请确保修复 c->cmd。
     * */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
        serverAssertWithInfo(c,NULL,c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is
 * using to store the reply still not read by the client.
 *
 * 此函数返回 Redis 用于存储客户端仍未读取的回复的字节数。
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits.
 *
 * 注意：此函数非常快，因此可以根据调用者的意愿多次调用。此函数当前的主要用途是强制实施客户端输出长度限制。
 * */
unsigned long getClientOutputBufferMemoryUsage(client *c) {
    unsigned long list_item_size = sizeof(listNode) + sizeof(clientReplyBlock);
    return c->reply_bytes + (list_item_size*listLength(c->reply));
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * The function will return one of the following:
 * CLIENT_TYPE_NORMAL -> Normal client
 * CLIENT_TYPE_SLAVE  -> Slave
 * CLIENT_TYPE_PUBSUB -> Client subscribed to Pub/Sub channels
 * CLIENT_TYPE_MASTER -> The client representing our replication master.
 *
 * 获取客户端的类，用于对不同类的客户端强制实施限制。
 * 该函数将返回以下内容之一：
 * CLIENT_TYPE_NORMAL -> 普通客户端
 * CLIENT_TYPE_SLAVE -> 从属
 * CLIENT_TYPE_PUBSUB -> 订阅发布/订阅频道的客户端
 * CLIENT_TYPE_MASTER -> 代表复制主客户端的客户端。
 */
int getClientType(client *c) {
    if (c->flags & CLIENT_MASTER) return CLIENT_TYPE_MASTER;
    /* Even though MONITOR clients are marked as replicas, we
     * want the expose them as normal clients.
     *
     * 即使 MONITOR 客户端被标记为副本，我们也希望将它们公开为普通客户端。
     * */
    if ((c->flags & CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR))
        return CLIENT_TYPE_SLAVE;
    if (c->flags & CLIENT_PUBSUB) return CLIENT_TYPE_PUBSUB;
    return CLIENT_TYPE_NORMAL;
}

int getClientTypeByName(char *name) {
    if (!strcasecmp(name,"normal")) return CLIENT_TYPE_NORMAL;
    else if (!strcasecmp(name,"slave")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"replica")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return CLIENT_TYPE_PUBSUB;
    else if (!strcasecmp(name,"master")) return CLIENT_TYPE_MASTER;
    else return -1;
}

char *getClientTypeName(int class) {
    switch(class) {
    case CLIENT_TYPE_NORMAL: return "normal";
    case CLIENT_TYPE_SLAVE:  return "slave";
    case CLIENT_TYPE_PUBSUB: return "pubsub";
    case CLIENT_TYPE_MASTER: return "master";
    default:                       return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * 该函数检查客户端是否达到输出缓冲区软限制或硬限制，并更新检查软限制所需的状态作为副作用。
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned.
 *  返回值：如果客户端达到软限制或硬限制，则不为零。否则返回零。
 *               */
int checkClientOutputBufferLimits(client *c) {
    int soft = 0, hard = 0, class;
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    class = getClientType(c);
    /* For the purpose of output buffer limiting, masters are handled
     * like normal clients.
     *
     * 出于输出缓冲区限制的目的，主节点的处理方式与普通客户端相同。
     * */
    if (class == CLIENT_TYPE_MASTER) class = CLIENT_TYPE_NORMAL;

    if (server.client_obuf_limits[class].hard_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].hard_limit_bytes)
        hard = 1;
    if (server.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds.
     *
     * 我们需要检查是否在指定的秒数内连续达到软限制。
     * */
    if (soft) {
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = server.unixtime;
            soft = 0; /* First time we see the soft limit reached 我们第一次看到达到软限制 */
        } else {
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <=
                server.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached.

                             客户端仍未达到被视为已达到软限制的最大秒数。
                             */
            }
        }
    } else {
        c->obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client CLIENT_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers.
 *
 * 如果达到输出缓冲区大小的软限制或硬限制，则异步关闭客户端。
 * 调用方可以检查客户端是否将关闭，检查是否设置了客户端CLIENT_CLOSE_ASAP标志。
 * 注意：我们需要异步关闭客户端，因为此函数是从无法安全释放客户端的上下文中调用的，
 * 即从在客户端输出缓冲区内推送数据的较低级别的函数调用。
 * */
// 如果客户端的缓存限制被突破了，那么关闭这个客户端
void asyncCloseClientOnOutputBufferLimitReached(client *c) {
    if (!c->conn) return; /* It is unsafe to free fake clients. */
    serverAssert(c->reply_bytes < SIZE_MAX-(1024*64));
    if (c->reply_bytes == 0 || c->flags & CLIENT_CLOSE_ASAP) return;
    if (checkClientOutputBufferLimits(c)) {
        sds client = catClientInfoString(sdsempty(),c);

        freeClientAsync(c);
        serverLog(LL_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
    }
}

/* Helper function used by freeMemoryIfNeeded() in order to flush slaves
 * output buffers without returning control to the event loop.
 * This is also called by SHUTDOWN for a best-effort attempt to send
 * slaves the latest writes.
 *
 * freeMemoryIfNeed（） 使用的帮助程序函数，用于刷新从站输出缓冲区而不将控制权返回给事件循环。
 * SHUTDOWN 也调用了它，以尽力尝试向从属服务器发送最新的写入。
 *
 * */
void flushSlavesOutputBuffers(void) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = listNodeValue(ln);
        int can_receive_writes = connHasWriteHandler(slave->conn) ||
                                 (slave->flags & CLIENT_PENDING_WRITE);

        /* We don't want to send the pending data to the replica in a few
         * cases:
         *
         * 1. For some reason there is neither the write handler installed
         *    nor the client is flagged as to have pending writes: for some
         *    reason this replica may not be set to receive data. This is
         *    just for the sake of defensive programming.
         *
         * 2. The put_online_on_ack flag is true. To know why we don't want
         *    to send data to the replica in this case, please grep for the
         *    flag for this flag.
         *
         * 3. Obviously if the slave is not ONLINE.
         *
         * 我们不想在少数情况下将待处理的数据发送到副本。例：
         *
         * 1.由于某种原因，既没有安装写处理程序，也没有将客户端标记为具有挂起的写入：
         *   由于某种原因，此副本可能未设置为接收数据。这只是为了防御性编程。
         *
         * 2.put_online_on_ack标志为真。要知道为什么在这种情况下我们不想将数据发送到副本，
         *   请 grep 作为此标志的标志。
         *
         * 3.显然，如果奴隶不在线。
         */
        if (slave->replstate == SLAVE_STATE_ONLINE &&
            can_receive_writes &&
            !slave->repl_put_online_on_ack &&
            clientHasPendingReplies(slave))
        {
            writeToClient(slave,0);
        }
    }
}

/* Pause clients up to the specified unixtime (in ms). While clients
 * are paused no command is processed from clients, so the data set can't
 * change during that time.
 *
 * However while this function pauses normal and Pub/Sub clients, slaves are
 * still served, so this function can be used on server upgrades where it is
 * required that slaves process the latest bytes from the replication stream
 * before being turned to masters.
 *
 * This function is also internally used by Redis Cluster for the manual
 * failover procedure implemented by CLUSTER FAILOVER.
 *
 * The function always succeed, even if there is already a pause in progress.
 * In such a case, the pause is extended if the duration is more than the
 * time left for the previous duration. However if the duration is smaller
 * than the time left for the previous pause, no change is made to the
 * left duration.
 *
 *
 * 将客户端暂停至指定的 unixtime（以毫秒为单位）。
 * 暂停客户端时，不会处理来自客户端的命令，因此在此期间无法更改数据集。
 * 但是，当此功能暂停普通客户端和 Pub/Sub 客户端时，仍会为从属服务器提供服务，
 * 因此此功能可用于服务器升级，其中要求从属服务器在转到主服务器之前处理复制流中的最新字节。
 * Redis 集群内部也使用此函数执行由集群故障转移实现的手动故障转移过程。
 * 该函数始终成功，即使已经有暂停正在进行中也是如此。
 * 在这种情况下，如果持续时间超过上一个持续时间的剩余时间，则暂停时间会延长。
 * 但是，如果持续时间小于上一次暂停的剩余时间，则不会对左侧持续时间进行任何更改。
 * */
void pauseClients(mstime_t end) {
    if (!server.clients_paused || end > server.clients_pause_end_time)
        server.clients_pause_end_time = end;
    server.clients_paused = 1;
}

/* Return non-zero if clients are currently paused. As a side effect the
 * function checks if the pause time was reached and clear it.
 *
 * 如果客户端当前已暂停，则返回非零值。作为副作用，该函数检查是否已达到暂停时间并清除它。
 * */
int clientsArePaused(void) {
    if (server.clients_paused &&
        server.clients_pause_end_time < server.mstime)
    {
        listNode *ln;
        listIter li;
        client *c;

        server.clients_paused = 0;

        /* Put all the clients in the unblocked clients queue in order to
         * force the re-processing of the input buffer if any.
         *
         * 将所有客户端放入未阻止的客户端队列中，以便强制重新处理输入缓冲区（如果有）。
         * */
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            c = listNodeValue(ln);

            /* Don't touch slaves and blocked clients.
             * The latter pending requests will be processed when unblocked.
             *
             * 不要触摸奴隶和被阻止的客户端。后一个待处理的请求将在解锁时进行处理。
             * */
            if (c->flags & (CLIENT_SLAVE|CLIENT_BLOCKED)) continue;
            queueClientForReprocessing(c);
        }
    }
    return server.clients_paused;
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * 此函数由 Redis 调用，以便不时处理一些事件，同时阻止进入一些不可中断的操作。
 * 这允许在启动时加载数据集时或在与主节点完全重新同步后以 -LOAD 错误回复客户端，依此类推。
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop 4 times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * 它调用事件循环以处理一些事件。具体来说，只要我们收到某个事件已处理的确认，我们就会尝试调用事件循环 4 次，
 * 以便继续执行为客户端提供服务所需的接受、读取、写入、关闭序列。
 *
 * The function returns the total number of events processed.
 *
 * 该函数返回处理的事件总数。
 * */
void processEventsWhileBlocked(void) {
    int iterations = 4; /* See the function top-comment.  请参阅函数顶部注释。 */

    /* Note: when we are processing events while blocked (for instance during
     * busy Lua scripts), we set a global flag. When such flag is set, we
     * avoid handling the read part of clients using threaded I/O.
     * See https://github.com/antirez/redis/issues/6988 for more info.
     *
     *
     * 注意：当我们在阻塞的情况下处理事件时（例如在繁忙的Lua脚本期间），我们设置了一个全局标志。
     * 设置此类标志时，我们避免使用线程 I/O 处理客户端的读取部分。
     * 有关详细信息，请参阅 https://github.com/antirez/redis/issues/6988。
     * */
    ProcessingEventsWhileBlocked = 1;
    while (iterations--) {
        long long startval = server.events_processed_while_blocked;
        long long ae_events = aeProcessEvents(server.el,
            AE_FILE_EVENTS|AE_DONT_WAIT|
            AE_CALL_BEFORE_SLEEP|AE_CALL_AFTER_SLEEP);
        /* Note that server.events_processed_while_blocked will also get
         * incremeted by callbacks called by the event loop handlers.
         *
         * 请注意，事件循环处理程序调用的回调也会增加server.events_processed_while_blocked。
         * */
        server.events_processed_while_blocked += ae_events;
        long long events = server.events_processed_while_blocked - startval;
        if (!events) break;
    }
    ProcessingEventsWhileBlocked = 0;
}

/* ==========================================================================
 * Threaded I/O
 * ========================================================================== */

int tio_debug = 0;

#define IO_THREADS_MAX_NUM 128
#define IO_THREADS_OP_READ 0
#define IO_THREADS_OP_WRITE 1

pthread_t io_threads[IO_THREADS_MAX_NUM];
pthread_mutex_t io_threads_mutex[IO_THREADS_MAX_NUM];
_Atomic unsigned long io_threads_pending[IO_THREADS_MAX_NUM];
int io_threads_op;      /* IO_THREADS_OP_WRITE or IO_THREADS_OP_READ. */

/* This is the list of clients each thread will serve when threaded I/O is
 * used. We spawn io_threads_num-1 threads, since one is the main thread
 * itself.
 *
 *
 * 这是使用线程 I/O 时每个线程将服务的客户端列表。我们生成 io_threads_num-1 个线程，因为其中一个是主线程本身。
 * */
list *io_threads_list[IO_THREADS_MAX_NUM];

// 1. IO 线程的要入口
void *IOThreadMain(void *myid) {
    /* The ID is the thread number (from 0 to server.iothreads_num-1), and is
     * used by the thread to just manipulate a single sub-array of clients.
     *
     * ID 是线程编号（从 0 到 server.iothreads_num-1），线程使用它来操作客户端的单个子数组。
     * */
    long id = (unsigned long)myid;
    char thdname[16];

    snprintf(thdname, sizeof(thdname), "io_thd_%ld", id);
    redis_set_thread_title(thdname);
    redisSetCpuAffinity(server.server_cpulist);
    makeThreadKillable();

    while(1) {
        /* Wait for start 等待启动*/
        for (int j = 0; j < 1000000; j++) {
            if (io_threads_pending[id] != 0) break;
        }

        /* Give the main thread a chance to stop this thread.
         *
         * 给主线程一个停止此线程的机会。
         * */
        if (io_threads_pending[id] == 0) {
            pthread_mutex_lock(&io_threads_mutex[id]);
            pthread_mutex_unlock(&io_threads_mutex[id]);
            continue;
        }

        serverAssert(io_threads_pending[id] != 0);

        if (tio_debug) printf("[%ld] %d to handle\n", id, (int)listLength(io_threads_list[id]));

        /* Process: note that the main thread will never touch our list
         * before we drop the pending count to 0.
         *
         * 进程：请注意，在我们把挂起的计数降低到 0 之前，主线程永远不会触及我们的列表。
         * */
        listIter li;
        listNode *ln;
        listRewind(io_threads_list[id],&li);
        while((ln = listNext(&li))) {
            client *c = listNodeValue(ln);
            // 判断 io_threads_op 是读还是写
            if (io_threads_op == IO_THREADS_OP_WRITE) {
                writeToClient(c,0);
            } else if (io_threads_op == IO_THREADS_OP_READ) {
                readQueryFromClient(c->conn);
            } else {
                serverPanic("io_threads_op value is unknown");
            }
        }
        listEmpty(io_threads_list[id]);
        io_threads_pending[id] = 0;

        if (tio_debug) printf("[%ld] Done\n", id);
    }
}

/* Initialize the data structures needed for threaded I/O.
 *
 * 初始化线程 I/O 所需的数据结构。
 * */
void initThreadedIO(void) {
    server.io_threads_active = 0; /* We start with threads not active. 我们从不活动的线程开始。*/

    /* Don't spawn any thread if the user selected a single thread:
     * we'll handle I/O directly from the main thread.
     *
     * 如果用户选择了单个线程，则不要生成任何线程：我们将直接从主线程处理 I/O。
     * */
    if (server.io_threads_num == 1) return;

    if (server.io_threads_num > IO_THREADS_MAX_NUM) {
        serverLog(LL_WARNING,"Fatal: too many I/O threads configured. "
                             "The maximum number is %d.", IO_THREADS_MAX_NUM);
        exit(1);
    }

    /* Spawn and initialize the I/O threads.
     *
     * 生成并初始化 I/O 线程。
     * */
    for (int i = 0; i < server.io_threads_num; i++) {
        /* Things we do for all the threads including the main thread.
         *
         * 我们为所有线程（包括主线程）执行的操作。
         * */
        io_threads_list[i] = listCreate();
        if (i == 0) continue; /* Thread 0 is the main thread. */

        /* Things we do only for the additional threads.
         *
         * 我们只为其他线程做的事情。
         * */
        pthread_t tid;
        pthread_mutex_init(&io_threads_mutex[i],NULL);
        io_threads_pending[i] = 0;
        pthread_mutex_lock(&io_threads_mutex[i]); /* Thread will be stopped. */
        if (pthread_create(&tid,NULL,IOThreadMain,(void*)(long)i) != 0) {
            serverLog(LL_WARNING,"Fatal: Can't initialize IO thread.");
            exit(1);
        }
        io_threads[i] = tid;
    }
}

// Kill 掉io线程
void killIOThreads(void) {
    int err, j;
    for (j = 0; j < server.io_threads_num; j++) {
        if (io_threads[j] == pthread_self()) continue;
        if (io_threads[j] && pthread_cancel(io_threads[j]) == 0) {
            if ((err = pthread_join(io_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "IO thread(tid:%lu) can not be joined: %s",
                        (unsigned long)io_threads[j], strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "IO thread(tid:%lu) terminated",(unsigned long)io_threads[j]);
            }
        }
    }
}

// 开启 io 线程
void startThreadedIO(void) {
    if (tio_debug) { printf("S"); fflush(stdout); }
    if (tio_debug) printf("--- STARTING THREADED IO ---\n");
    serverAssert(server.io_threads_active == 0);
    for (int j = 1; j < server.io_threads_num; j++)
        pthread_mutex_unlock(&io_threads_mutex[j]);
    server.io_threads_active = 1;
}

// 停止 io 线程
void stopThreadedIO(void) {
    /* We may have still clients with pending reads when this function
     * is called: handle them before stopping the threads.
     *
     * 调用此函数时，我们可能仍有客户端具有挂起的读取：在停止线程之前处理它们。
     * */
    handleClientsWithPendingReadsUsingThreads();
    if (tio_debug) { printf("E"); fflush(stdout); }
    if (tio_debug) printf("--- STOPPING THREADED IO [R%d] [W%d] ---\n",
        (int) listLength(server.clients_pending_read),
        (int) listLength(server.clients_pending_write));
    serverAssert(server.io_threads_active == 1);
    for (int j = 1; j < server.io_threads_num; j++)
        pthread_mutex_lock(&io_threads_mutex[j]);
    server.io_threads_active = 0;
}

/* This function checks if there are not enough pending clients to justify
 * taking the I/O threads active: in that case I/O threads are stopped if
 * currently active. We track the pending writes as a measure of clients
 * we need to handle in parallel, however the I/O threading is disabled
 * globally for reads as well if we have too little pending clients.
 *
 * The function returns 0 if the I/O threading should be used because there
 * are enough active threads, otherwise 1 is returned and the I/O threads
 * could be possibly stopped (if already active) as a side effect.
 *
 * 此函数检查是否没有足够的挂起客户端来证明使 I/O 线程处于活动状态是合理的：
 * 在这种情况下，如果当前处于活动状态，则 I/O 线程将停止。
 * 我们跟踪挂起的写入作为我们需要并行处理的客户端的度量，
 * 但是如果我们的挂起客户端太少，则全局禁用 I/O 线程以进行读取。
 * 如果由于有足够的活动线程而应使用 I/O 线程，则该函数返回 0，否则返回 1，
 * 并且 I/O 线程可能会停止（如果已处于活动状态）作为副作用。
 *
 * */
// 如果需要的话就停止 io 线程
int stopThreadedIOIfNeeded(void) {
    int pending = listLength(server.clients_pending_write);

    /* Return ASAP if IO threads are disabled (single threaded mode).
     *
     * 如果禁用 IO 线程（单线程模式），请尽快返回。
     * */
    if (server.io_threads_num == 1) return 1;

    if (pending < (server.io_threads_num*2)) {
        if (server.io_threads_active) stopThreadedIO();
        return 1;
    } else {
        return 0;
    }
}

// 使用 io 线程处理等待写的客户端
int handleClientsWithPendingWritesUsingThreads(void) {
    int processed = listLength(server.clients_pending_write);
    if (processed == 0) return 0; /* Return ASAP if there are no clients. */

    /* If I/O threads are disabled or we have few clients to serve, don't
     * use I/O threads, but thejboring synchronous code.
     *
     * 如果禁用了 I/O 线程，或者我们要服务的客户端很少，请不要使用 I/O 线程，而是使用无聊的同步代码。
     * */
    // 判断是否有必要开启IO多线程
    if (server.io_threads_num == 1 || stopThreadedIOIfNeeded()) {
        return handleClientsWithPendingWrites();
    }

    /* Start threads if needed. */
    // 开启io多线程
    if (!server.io_threads_active) startThreadedIO();

    if (tio_debug) printf("%d TOTAL WRITE pending clients\n", processed);

    /* Distribute the clients across N different lists.
     *
     * 将客户端分布到 N 个不同的列表。
     * */
    listIter li;
    listNode *ln;
    // 创建一个迭代器li，用于遍历任务队列clients_pending_write
    listRewind(server.clients_pending_write,&li);
    // 默认是0，先分配给主线程去做（生产者也可能是消费者），如果设置成1，则先让io线程1去做
    int item_id = 0;
    // io_threads_list[0] 主线程
    // io_threads_list[1] io线程
    // io_threads_list[2] io线程
    // io_threads_list[3] io线程
    // io_threads_list[4] io线程
    while((ln = listNext(&li))) {
        // 取出一个任务
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;

        /* Remove clients from the list of pending writes since
         * they are going to be closed ASAP. */
        // 表示该客户端的输出缓冲区超过了服务器允许范围,将在下一次循环进行一个关闭,也不返回任何信息给客户端，删除待读客户端
        if (c->flags & CLIENT_CLOSE_ASAP) {
            listDelNode(server.clients_pending_write, ln);
            continue;
        }

        // 负载均衡：将任务队列中的任务 添加 到不同的线程消费队列中去，每个线程就可以从当前线程的消费队列中取任务就行了
        // 这样做的好处是，避免加锁。当前是在主线程中，进行分配任务
        // 通过取余操作，将任务均分给不同io线程
        int target_id = item_id % server.io_threads_num;
        listAddNodeTail(io_threads_list[target_id],c);
        item_id++;
    }

    /* Give the start condition to the waiting threads, by setting the
     * start condition atomic var.
     *
     * 通过设置启动条件原子变量，将启动条件提供给等待线程。
     * */
    io_threads_op = IO_THREADS_OP_WRITE;
    for (int j = 1; j < server.io_threads_num; j++) {
        int count = listLength(io_threads_list[j]);
        // 设置io线程启动条件，启动io线程
        io_threads_pending[j] = count;
    }

    /* Also use the main thread to process a slice of clients. */
    // 让主线程去处理一部分任务（io_threads_list[0]）
    listRewind(io_threads_list[0],&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        writeToClient(c,0);
    }
    listEmpty(io_threads_list[0]);

    /* Wait for all the other threads to end their work. */
    // 剩下的任务io_threads_list[1]，io_threads_list[2].....给io线程去做，等待io线程完成任务
    while(1) {
        unsigned long pending = 0;
        for (int j = 1; j < server.io_threads_num; j++)
            // 等待io线程结束，并返回处理的数量
            pending += io_threads_pending[j];
        if (pending == 0) break;
    }
    if (tio_debug) printf("I/O WRITE All threads finshed\n");

    /* Run the list of clients again to install the write handler where
     * needed.
     *
     * 再次运行客户端列表以根据需要安装写入处理程序。
     * */
    listRewind(server.clients_pending_write,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        /* Install the write handler if there are pending writes in some
         * of the clients.
         *
         * 如果某些客户端中存在挂起的写入，请安装写入处理程序。
         * */
        if (clientHasPendingReplies(c) &&
                connSetWriteHandler(c->conn, sendReplyToClient) == AE_ERR)
        {
            freeClientAsync(c);
        }
    }
    listEmpty(server.clients_pending_write);

    /* Update processed count on server
     *
     * 更新服务器上的已处理计数
     * */
    server.stat_io_writes_processed += processed;

    return processed;
}

/* Return 1 if we want to handle the client read later using threaded I/O.
 * This is called by the readable handler of the event loop.
 * As a side effect of calling this function the client is put in the
 * pending read clients and flagged as such.
 *
 * 如果我们想稍后使用线程 I/O 处理客户端读取，则返回 1。
 * 这是由事件循环的可读处理程序调用的 作为调用此函数的副作用，客户端被放入挂起的读取客户端中并标记为这样。
 *
 * */

// 推迟客户端读
int postponeClientRead(client *c) {
    if (server.io_threads_active &&
        server.io_threads_do_reads &&
        !clientsArePaused() &&
        !ProcessingEventsWhileBlocked &&
        !(c->flags & (CLIENT_MASTER|CLIENT_SLAVE|CLIENT_PENDING_READ)))
    {
        c->flags |= CLIENT_PENDING_READ;
        listAddNodeHead(server.clients_pending_read,c);
        return 1;
    } else {
        return 0;
    }
}

/* When threaded I/O is also enabled for the reading + parsing side, the
 * readable handler will just put normal clients into a queue of clients to
 * process (instead of serving them synchronously). This function runs
 * the queue using the I/O threads, and process them in order to accumulate
 * the reads in the buffers, and also parse the first command available
 * rendering it in the client structures.
 *
 * 当读取 + 解析端也启用了线程 I/O 时，可读处理程序只会将普通客户端放入客户端队列中进行处理（而不是同步为它们提供服务）。
 * 此函数使用 I/O 线程运行队列，并处理它们以累积缓冲区中的读取，并解析在客户端结构中呈现它的第一个可用命令。
 * */

// 使用 io 线程处理等待读的客户端
int handleClientsWithPendingReadsUsingThreads(void) {
    // 前提是 io 线程激活了而且 io 线程 配置了读写
    if (!server.io_threads_active || !server.io_threads_do_reads) return 0;
    int processed = listLength(server.clients_pending_read);
    if (processed == 0) return 0;

    if (tio_debug) printf("%d TOTAL READ pending clients\n", processed);

    /* Distribute the clients across N different lists.
     *
     * 将客户端分布到 N 个不同的列表。
     * */
    listIter li;
    listNode *ln;
    listRewind(server.clients_pending_read,&li);
    int item_id = 0;
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        int target_id = item_id % server.io_threads_num;
        listAddNodeTail(io_threads_list[target_id],c);
        item_id++;
    }

    /* Give the start condition to the waiting threads, by setting the
     * start condition atomic var.
     *
     * 通过设置启动条件原子变量，将启动条件提供给等待线程。
     * */
    io_threads_op = IO_THREADS_OP_READ;
    for (int j = 1; j < server.io_threads_num; j++) {
        int count = listLength(io_threads_list[j]);
        io_threads_pending[j] = count;
    }

    /* Also use the main thread to process a slice of clients.
     * 还可以使用主线程处理客户端的一部分。
     * */
    listRewind(io_threads_list[0],&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        readQueryFromClient(c->conn);
    }
    listEmpty(io_threads_list[0]);

    /* Wait for all the other threads to end their work.
     * 等待所有其他线程结束其工作。
     * */
    // 这里其实会阻塞等待
    while(1) {
        unsigned long pending = 0;
        for (int j = 1; j < server.io_threads_num; j++)
            pending += io_threads_pending[j];
        if (pending == 0) break;
    }
    if (tio_debug) printf("I/O READ All threads finshed\n");

    /* Run the list of clients again to process the new buffers.
     *
     * 再次运行客户端列表以处理新缓冲区。
     * */
    while(listLength(server.clients_pending_read)) {
        ln = listFirst(server.clients_pending_read);
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_READ;
        listDelNode(server.clients_pending_read,ln);
        /* Clients can become paused while executing the queued commands,
         * so we need to check in between each command. If a pause was
         * executed, we still remove the command and it will get picked up
         * later when clients are unpaused and we re-queue all clients.
         *
         * 客户端在执行排队的命令时可能会暂停，因此我们需要在每个命令之间进行签入。
         * 如果执行了暂停，我们仍会删除该命令，稍后当客户端取消暂停并重新排队所有客户端时，它将被拾取。
         * */
        if (clientsArePaused()) continue;

        if (processPendingCommandsAndResetClient(c) == C_ERR) {
            /* If the client is no longer valid, we avoid
             * processing the client later. So we just go
             * to the next.
             *
             * 如果客户端不再有效，我们将避免稍后处理客户端。所以我们就去下一个。
             * */
            continue;
        }
        processInputBuffer(c);

        /* We may have pending replies if a thread readQueryFromClient() produced
         * replies and did not install a write handler (it can't).
         *
         * 如果线程 readQueryFromClient（） 生成回复并且没有安装写入处理程序（它不能），我们可能会有挂起的回复。
         */
        if (!(c->flags & CLIENT_PENDING_WRITE) && clientHasPendingReplies(c))
            clientInstallWriteHandler(c);
    }

    /* Update processed count on server
     *
     * 更新服务器上的已处理计数
     * */
    server.stat_io_reads_processed += processed;

    return processed;
}
