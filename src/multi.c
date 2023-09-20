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

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
/*
 * 初始化客户端的事务状态
 *
 * T = O(1)
 */
void initClientMultiState(client *c) {
    c->mstate.commands = NULL;
    c->mstate.count = 0;
    c->mstate.cmd_flags = 0;
    c->mstate.cmd_inv_flags = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
/*
 * 释放所有在事务队列中的命令
 *
 * T = O(N^2)
 */
void freeClientMultiState(client *c) {
    int j;

    // 释放所有命令
    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;

        // 释放命令的所有参数
        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
/*
 * 将新命令添加到事务队列中
 *
 * T = O(N)
 */
void queueMultiCommand(client *c) {
    multiCmd *mc;
    int j;

    /* No sense to waste memory if the transaction is already aborted.
     * this is useful in case client sends these in a pipeline, or doesn't
     * bother to read previous responses and didn't notice the multi was already
     * aborted. */
    if (c->flags & CLIENT_DIRTY_EXEC)
        return;

    // 重分配空间，为新命令申请空间
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));


    // 指针指向新分配的空间
    // 并将命令内容保存进去
    mc = c->mstate.commands+c->mstate.count;
    // 保存要执行的命令
    mc->cmd = c->cmd;
    // 保存命令参数的数量
    mc->argc = c->argc;
    // 为参数分配空间
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    // 复制参数
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    // 为参数的引用计数增一
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);
    // 入队命令数量增一
    c->mstate.count++;
    c->mstate.cmd_flags |= c->cmd->flags;
    c->mstate.cmd_inv_flags |= ~c->cmd->flags;
}

/*
 * 放弃事务，清理并重置客户端的事务状态
 *
 * T = O(N^2)
 */
void discardTransaction(client *c) {
    // 释放参数空间
    freeClientMultiState(c);
    // 重置事务状态
    initClientMultiState(c);
    // 关闭相关的 flag
    c->flags &= ~(CLIENT_MULTI|CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC);
    // 取消所有 key 的监视, O(N^2)
    unwatchAllKeys(c);
}

/* Flag the transaction as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
/*
 * 如果在入队的过程中发生命令出错，
 * 那么让客户端变为“脏”，令下次事务执行失败
 *
 * T = O(1)
 */
void flagTransaction(client *c) {
    if (c->flags & CLIENT_MULTI)
        c->flags |= CLIENT_DIRTY_EXEC;
}

/*
 * MULTI 命令的实现
 *
 * 打开客户端的 FLAG ，让命令入队到事务队列里
 *
 * T = O(1)
 */
void multiCommand(client *c) {
    // MULTI 命令不能嵌套
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }
    // 打开事务的 FLAG
    // 从此之后，除 DISCARD 和 EXEC 等少数几个命令之外
    // 其他所有的命令都会被添加到事务队列里
    c->flags |= CLIENT_MULTI;
    addReply(c,shared.ok);
}

/*
 * DISCAD 命令的实现
 *
 * 放弃事务，并清理相关资源
 *
 * T = O(N)
 */
void discardCommand(client *c) {
    // 只能在 MULTI 命令已启用的情况下使用
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    // 放弃事务, O(N)
    discardTransaction(c);
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implementation for more information. */
// 向所有附属节点和 AOF 文件发送 MULTI 命令
void execCommandPropagateMulti(client *c) {
    propagate(server.multiCommand,c->db->id,&shared.multi,1,
              PROPAGATE_AOF|PROPAGATE_REPL);
}

void execCommandPropagateExec(client *c) {
    propagate(server.execCommand,c->db->id,&shared.exec,1,
              PROPAGATE_AOF|PROPAGATE_REPL);
}

/* Aborts a transaction, with a specific error message.
 * The transaction is always aboarted with -EXECABORT so that the client knows
 * the server exited the multi state, but the actual reason for the abort is
 * included too.
 * Note: 'error' may or may not end with \r\n. see addReplyErrorFormat. */
void execCommandAbort(client *c, sds error) {
    discardTransaction(c);

    if (error[0] == '-') error++;
    addReplyErrorFormat(c, "-EXECABORT Transaction discarded because of: %s", error);

    /* Send EXEC to clients waiting data from MONITOR. We did send a MULTI
     * already, and didn't send any of the queued commands, now we'll just send
     * EXEC so it is clear that the transaction is over. */
    if (listLength(server.monitors) && !server.loading)
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

// EXEC 命令的实现
void execCommand(client *c) {
    int j;
    // 用于保存执行命令、命令的参数和参数数量的副本
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;
    int must_propagate = 0; /* Need to propagate MULTI/EXEC to AOF / slaves? */
    int was_master = server.masterhost == NULL;

    // 只能在 MULTI 已启用的情况下执行
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* EXEC with expired watched key is disallowed*/
    if (isWatchedKeyExpired(c)) {
        c->flags |= (CLIENT_DIRTY_CAS);
    }

    /* Check if we need to abort the EXEC because:
     * 以下情况发生时，取消事务
     *
     * 1) Some WATCHed key was touched.
     *    某些被监视的键已被修改（状态为 REDIS_DIRTY_CAS）
     *
     * 2) There was a previous error while queueing commands.
     *    有命令在入队时发生错误（状态为 REDIS_DIRTY_EXEC）
     *
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned.
     *
     * 第一种情况返回多个空白 NULL 对象，
     * 第二种情况返回一个 EXECABORT 错误。
     */
    if (c->flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC)) {
        // 根据状态，决定返回的错误的类型
        addReply(c, c->flags & CLIENT_DIRTY_EXEC ? shared.execaborterr :
                                                   shared.nullarray[c->resp]);
        discardTransaction(c);
        goto handle_monitor;
    }

    /* Exec all the queued commands */
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */
    // 将三个原始参数备份起来
    orig_argv = c->argv;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    addReplyArrayLen(c,c->mstate.count);
    // 执行所有入队的命令
    for (j = 0; j < c->mstate.count; j++) {
        // 因为 call 可能修改命令，而命令需要传送给其他同步节点
        // 所以这里将要执行的命令（及其参数）先备份起来
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;

        /* Propagate a MULTI request once we encounter the first command which
         * is not readonly nor an administrative one.
         * This way we'll deliver the MULTI/..../EXEC block as a whole and
         * both the AOF and the replication link will have the same consistency
         * and atomicity guarantees. */
        if (!must_propagate &&
            !server.loading &&
            !(c->cmd->flags & (CMD_READONLY|CMD_ADMIN)))
        {
            execCommandPropagateMulti(c);
            must_propagate = 1;
        }

        int acl_keypos;
        int acl_retval = ACLCheckCommandPerm(c,&acl_keypos);
        if (acl_retval != ACL_OK) {
            addACLLogEntry(c,acl_retval,acl_keypos,NULL);
            addReplyErrorFormat(c,
                "-NOPERM ACLs rules changed between the moment the "
                "transaction was accumulated and the EXEC call. "
                "This command is no longer allowed for the "
                "following reason: %s",
                (acl_retval == ACL_DENIED_CMD) ?
                "no permission to execute the command or subcommand" :
                "no permission to touch the specified keys");
        } else {
            // 执行命令
            call(c,server.loading ? CMD_CALL_NONE : CMD_CALL_FULL);
        }

        /* Commands may alter argc/argv, restore mstate. */
        // 还原原始的参数到队列里
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].cmd = c->cmd;
    }

    // 将三个原始参数备份起来
    c->argv = orig_argv;
    c->argc = orig_argc;
    c->cmd = orig_cmd;
    discardTransaction(c);

    /* Make sure the EXEC command will be propagated as well if MULTI
     * was already propagated. */
    if (must_propagate) {
        int is_master = server.masterhost == NULL;
        server.dirty++;
        /* If inside the MULTI/EXEC block this instance was suddenly
         * switched from master to slave (using the SLAVEOF command), the
         * initial MULTI was propagated into the replication backlog, but the
         * rest was not. We need to make sure to at least terminate the
         * backlog with the final EXEC. */
        if (server.repl_backlog && was_master && !is_master) {
            char *execcmd = "*1\r\n$4\r\nEXEC\r\n";
            feedReplicationBacklog(execcmd,strlen(execcmd));
        }
    }

handle_monitor:
    /* Send EXEC to clients waiting data from MONITOR. We do it here
     * since the natural order of commands execution is actually:
     * MUTLI, EXEC, ... commands inside transaction ...
     * Instead EXEC is flagged as CMD_SKIP_MONITOR in the command
     * table, and we do it here with correct ordering. */
    if (listLength(server.monitors) && !server.loading)
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * 实现为每个 DB 准备一个字典（哈希表），字典的键为该数据库被 WATCHED 的 key
 * 而字典的值是一个链表，保存了所有监视这个 key 的客户端
 * 一旦某个 key 被修改，程序会将整个链表的所有客户端都设置为被污染
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called.
 *
 * 此外，客户端还维持这一个保存所有 WATCH key 的链表，
 * 这样就可以在事务执行或者 UNWATCH 调用时，一次清除所有 WATCH key 。
 */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
/*
 * 被监视的 key 的资料
 */
typedef struct watchedKey {
    // 被监视的 key
    robj *key;
    // key 所在的数据库
    redisDb *db;
} watchedKey;

/* Watch for the specified key */
/*
 * 监视给定 key
 *
 * T = O(N)
 */
void watchForKey(client *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    // 检查该 key 是否已经被 WATCH
    // （出现在 WATCH 命令调用时一个 key 被输入多次的情况）
    // 如果是的话，直接返回
    // O(N)
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }
    /* This key is not already watched in this DB. Let's add it */
    // key 未被监视
    // 根据 key ，将客户端加入到 DB 的监视 key 字典中
    /* This key is not already watched in this DB. Let's add it */
    // O(1)
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients) {
        clients = listCreate();
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    listAddNodeTail(clients,c);
    /* Add the new key to the list of keys watched by this client */
    // 将 key 添加到客户端的监视列表中
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
/*
 * 取消所有该客户端监视的 key
 * 对事务状态的清除由调用者执行
 *
 * T = O(N^2)
 */
void unwatchAllKeys(client *c) {
    listIter li;
    listNode *ln;

    // 没有键被 watch ，直接返回
    if (listLength(c->watched_keys) == 0) return;
    // 从客户端以及 DB 中删除所有监视 key 和客户端的资料
    // O(N^2)
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        // 取出 watchedKey 结构
        wk = listNodeValue(ln);
        // 删除 db 中的客户端信息, O(1)
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        serverAssertWithInfo(c,NULL,clients != NULL);
        listDelNode(clients,listSearchKey(clients,c));
        /* Kill the entry at all if this was the only client */
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        // 将 key 从客户端的监视列表中删除, O(1)
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* iterates over the watched_keys list and
 * look for an expired key . */
int isWatchedKeyExpired(client *c) {
    listIter li;
    listNode *ln;
    watchedKey *wk;
    if (listLength(c->watched_keys) == 0) return 0;
    listRewind(c->watched_keys,&li);
    while ((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (keyIsExpired(wk->db, wk->key)) return 1;
    }

    return 0;
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
/*
 * “碰触”（touch）给定 key ，如果这个 key 正在被监视的话，
 * 让监视它的客户端在执行 EXEC 命令时失败。
 *
 * T = O(N)
 */
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;

    // 如果数据库中没有任何 key 被监视，那么直接返回
    if (dictSize(db->watched_keys) == 0) return;
    // 取出数据库中所有监视给定 key 的客户端
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;

    /* Mark all the clients watching this key as CLIENT_DIRTY_CAS */
    /* Check if we are already watching for this key */
    // 打开所有监视这个 key 的客户端的 REDIS_DIRTY_CAS 状态
    // O(N)
    listRewind(clients,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        c->flags |= CLIENT_DIRTY_CAS;
    }
}

/* Set CLIENT_DIRTY_CAS to all clients of DB when DB is dirty.
 * It may happen in the following situations:
 * FLUSHDB, FLUSHALL, SWAPDB
 *
 * replaced_with: for SWAPDB, the WATCH should be invalidated if
 * the key exists in either of them, and skipped only if it
 * doesn't exist in both. */
void touchAllWatchedKeysInDb(redisDb *emptied, redisDb *replaced_with) {
    listIter li;
    listNode *ln;
    dictEntry *de;

    if (dictSize(emptied->watched_keys) == 0) return;

    dictIterator *di = dictGetSafeIterator(emptied->watched_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        list *clients = dictGetVal(de);
        if (!clients) continue;
        listRewind(clients,&li);
        while((ln = listNext(&li))) {
            client *c = listNodeValue(ln);
            if (dictFind(emptied->dict, key->ptr)) {
                c->flags |= CLIENT_DIRTY_CAS;
            } else if (replaced_with && dictFind(replaced_with->dict, key->ptr)) {
                c->flags |= CLIENT_DIRTY_CAS;
            }
        }
    }
    dictReleaseIterator(di);
}

/*
 * 将所有输入键添加到监视列表当中
 *
 * T = O(N^2)
 */
void watchCommand(client *c) {
    int j;

    // 不能在事务中使用
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }
    // 监视所有 key ，O(N^2)
    for (j = 1; j < c->argc; j++)
        // O(N)
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}

/*
 * 取消对所有 key 的监视
 * 并关闭客户端的 REDIS_DIRTY_CAS 选项
 *
 * T = O(N^2)
 */
void unwatchCommand(client *c) {
    unwatchAllKeys(c);
    c->flags &= (~CLIENT_DIRTY_CAS);
    addReply(c,shared.ok);
}
