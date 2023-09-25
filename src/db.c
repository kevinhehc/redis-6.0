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
#include "cluster.h"
#include "atomicvar.h"

#include <signal.h>
#include <ctype.h>

/* Database backup. 
 *
 * 数据库备份。
 * */
struct dbBackup {
    redisDb *dbarray;
    rax *slots_to_keys;
    uint64_t slots_keys_count[CLUSTER_SLOTS];
};

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

int keyIsExpired(redisDb *db, robj *key);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. 
 *
 * 访问对象时更新LFU。首先，如果达到递减时间，则递减计数器。然后对数递增计数器，
 * 并更新访问时间。
 * */
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Low level key lookup API, not actually called directly from commands
 * implementations that should instead rely on lookupKeyRead(),
 * lookupKeyWrite() and lookupKeyReadWithFlags(). 
 *
 * 低级键查找API，实际上不是直接从命令实现调用的，
 * 而应该依赖于lookupKeyRead（）、lookupKeyWrite（）和lookupKeyReadWithFlags（）。
 * */
robj *lookupKey(redisDb *db, robj *key, int flags) {
    // 查找 key 对象
    dictEntry *de = dictFind(db->dict,key->ptr);

    // 存在
    if (de) {
        // 取出 key 对应的值对象
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. 
         *
         * 更新老化算法的访问时间。如果我们有一个正在保存的子进程，就不要这样做，因为这会引发
         * 一场写对写的疯狂复制。
         * */
        // 如果条件允许，那么更新 lru 时间
        if (!hasActiveChildProcess() && !(flags & LOOKUP_NOTOUCH)){
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }
        return val;
    } else {
        // 不存在
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 * 4. If keyspace notifications are enabled, a "keymiss" notification is fired.
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL if the key is logically expired
 * but still existing, in case this is a slave, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging
 * expiring our key via DELs in the replication link. 
 *
 * 查找用于读取操作的键，如果在指定的数据库中找不到该键，则返回NULL。作为调
 * 用此函数的副作用：
 * 1。如果键达到TTL，它就会过期。
 * 2.更新键上次访问时间。
 * 3.更新全局键命中/未命中统计数据（在INFO中报告）。
 * 4.如果启用了keyspace通知，则会触发“keymiss”通知。
 * 
 * 当我们在获得链接到键的对象后写入键时，不应使用此API，而应仅用于只读操作。
 * 标志更改此命令的行为：LOOKUP_NONE（或零）：不传递任何特殊标志。LOOKUP_NOTOUCH：不要更改键
 * 的最后访问时间。注意：如果键在逻辑上过期但仍然存在，则此函数也返回NULL，以
 * 防这是从项，因为此API仅用于读取操作。即使键过期是由主机驱动的，我们也可以正
 * 确地报告从机上的键过期，即使主机延迟通过复制链接中的DEL使我们的键过期。
 * */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;

    if (expireIfNeeded(db,key) == 1) {
        /* Key expired. If we are in the context of a master, expireIfNeeded()
         * returns 0 only when the key does not exist at all, so it's safe
         * to return NULL ASAP. 
         *
         * 键已过期。如果我们在master的上下文中，expireIfNeeded（）只有
         * 在键根本不存在时才返回0，因此尽快返回NULL是安全的。
         * */
        if (server.masterhost == NULL)
            goto keymiss;

        /* However if we are in the context of a slave, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessing expired values in a read-only fashion, that
         * will say the key as non existing.
         *
         * Notably this covers GETs when slaves are used to scale reads. 
         *
         * 然而，如果我们在从机的上下文中，expireIfNeeded（）不会真正尝试使键
         * 过期，它只返回有关键“逻辑”状态的信息：键过期取决于主机，以便对主机的数据集
         * 有一致的看法。
         * 
         * 然而，如果命令调用方不是master，并且作为额外的安全措施，调用
         * 的命令是只读命令，我们可以在这里安全地返回NULL，并为以只读方式访问过期值的客
         * 户端提供更一致的行为，也就是说键不存在。值得注意的是，这涵盖了当从设备用于缩放
         * 读取时的GET。
         * */
        if (server.current_client &&
            server.current_client != server.master &&
            server.current_client->cmd &&
            server.current_client->cmd->flags & CMD_READONLY)
        {
            goto keymiss;
        }
    }
    val = lookupKey(db,key,flags);
    if (val == NULL)
        goto keymiss;
    server.stat_keyspace_hits++;
    return val;

keymiss:
    if (!(flags & LOOKUP_NONOTIFY)) {
        server.stat_keyspace_misses++;
        notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
    }
    return NULL;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. 
 *
 * 类似于lookupKeyReadWithFlags（），但不使用任何标志，这是常
 * 见的情况。
 * */
// 为进行读操作而读取数据库
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. 
 *
 * 查找写操作的键，如果需要，如果达到TTL，则作为副作用，使键过期。如果键存在,
 * 则返回链接值对象；如果指定的DB中不存在键，则返回NULL。
 * */
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags) {
    // 检查 key 是否过期，如果是的话，将它删除
    expireIfNeeded(db,key);
    return lookupKey(db,key,flags);
}

/*
 * 为进行写操作而读取数据库
 *
 * 这个函数和 lookupKeyRead 的区别是
 * 这个函数不更新命中/不命中计数
 */
robj *lookupKeyWrite(redisDb *db, robj *key) {
    return lookupKeyWriteWithFlags(db, key, LOOKUP_NONE);
}

/*
 * 为执行读取操作而从数据库中取出给定 key 的值。
 * 如果 key 不存在，向客户端发送信息 reply 。
 */
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/*
 * 为执行写入操作而从数据库中取出给定 key 的值。
 * 如果 key 不存在，向客户端发送信息 reply 。
 */
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counte of the value if needed.
 *
 * 添加给定 key - value 对到数据库
 * 对 value 的引用计数处理由调用者决定
 *
 * The program is aborted if the key already exists.
 *
 * 添加只在 key 不存在的情况下进行
 */
void dbAdd(redisDb *db, robj *key, robj *val) {
    // 键（字符串）
    sds copy = sdsdup(key->ptr);
    // 保存 键-值 对
    int retval = dictAdd(db->dict, copy, val);

    serverAssertWithInfo(NULL,key,retval == DICT_OK);
    if (val->type == OBJ_LIST ||
        val->type == OBJ_ZSET ||
        val->type == OBJ_STREAM)
        signalKeyAsReady(db, key);
    if (server.cluster_enabled) slotToKeyAdd(key->ptr);
}

/* This is a special version of dbAdd() that is used only when loading
 * keys from the RDB file: the key is passed as an SDS string that is
 * retained by the function (and not freed by the caller).
 *
 * Moreover this function will not abort if the key is already busy, to
 * give more control to the caller, nor will signal the key as ready
 * since it is not useful in this context.
 *
 * The function returns 1 if the key was added to the database, taking
 * ownership of the SDS string, otherwise 0 is returned, and is up to the
 * caller to free the SDS string. 
 *
 * 这是dbAdd（）的一个特殊版本，仅在从RDB文件加载键时使用：键作为SDS
 * 字符串传递，由函数保留（而不是由调用方释放）。
 * 
 * 此外，如果键已经繁忙，则此函数不会中止，以向调用方提供更多控制，
 * 也不会向键发出就绪信号，因为它在这种情况下没有用处。如果键已添加到数据库中，
 * 则函数返回1，从而获得SDS字符串的所有权，否则返回0，由调用方释放SDS字符串。
 * */
int dbAddRDBLoad(redisDb *db, sds key, robj *val) {
    int retval = dictAdd(db->dict, key, val);
    if (retval != DICT_OK) return 0;
    if (server.cluster_enabled) slotToKeyAdd(key);
    return 1;
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * 使用新值 value 覆盖原本 key 的旧值
 * 对 value 的引用计数处理由调用者决定
 *
 * The program is aborted if the key was not already present.
 *
 * 添加只在 key 存在的情况下进行
 */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    // 取出节点
    dictEntry *de = dictFind(db->dict,key->ptr);

    serverAssertWithInfo(NULL,key,de != NULL);
    dictEntry auxentry = *de;
    robj *old = dictGetVal(de);
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        val->lru = old->lru;
    }
    dictSetVal(db->dict, de, val);

    if (server.lazyfree_lazy_server_del) {
        freeObjAsync(old);
        dictSetVal(db->dict, &auxentry, NULL);
    }

    dictFreeVal(db->dict, &auxentry);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 高阶 set 操作。
 * 可以给一个 key 设置 value ，不管 key 是否存在。
 *
 * 1) The ref count of the value object is incremented.
 *    value 对象的引用计数已增加
 * 2) clients WATCHing for the destination key notified.
 *    如果有 key 正在被 WATCH ，那么告知客户端这个 key 已被修改
 * 3) The expire time of the key is reset (the key is made persistent).
 *    key 的过期时间（如果有的话）会被重置，将 key 变为持久化的
 *
 * All the new keys in the database should be created via this interface.
 * The client 'c' argument may be set to NULL if the operation is performed
 * in a context where there is no clear client performing the operation. */
void genericSetKey(client *c, redisDb *db, robj *key, robj *val, int keepttl, int signal) {
    // 根据 key 的存在情况，进行 key 的写入或覆盖操作
    if (lookupKeyWrite(db,key) == NULL) {
        dbAdd(db,key,val);
    } else {
        dbOverwrite(db,key,val);
    }

    // 增加值的引用计数
    incrRefCount(val);
    // 移除旧 key 原有的过期时间（如果有的话）
    if (!keepttl) removeExpire(db,key);
    // 告知所有正在 WATCH 这个键的客户端，键已经被修改
    if (signal) signalModifiedKey(c,db,key);
}

/* Common case for genericSetKey() where the TTL is not retained. 
 *
 * genericSetKey（）的常见情况，其中不保留TTL。
 * */
void setKey(client *c, redisDb *db, robj *key, robj *val) {
    genericSetKey(c,db,key,val,0,1);
}

/* Return true if the specified key exists in the specified database.
 * LRU/LFU info is not updated in any way. 
 *
 * 如果指定的键存在于指定的数据库中，则返回true。LRU/LFU信息不会以任何方式更新。
 * */
/*
 * 检查 key 是否存在于 DB
 *
 * 是的话返回 1 ，否则返回 0
 */
int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * 以 Redis Object 的形式随机返回数据库中的一个 key
 * 如果数据库为空，那么返回 NULL
 *
 * The function makes sure to return keys not already expired.
 *
 * 函数只返回未过期的 key
 */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
    int maxtries = 100;
    int allvolatile = dictSize(db->dict) == dictSize(db->expires);

    while(1) {
        sds key;
        robj *keyobj;

        // 从字典中返回随机值， O(N)
        de = dictGetFairRandomKey(db->dict);
        // 数据库为空
        if (de == NULL) return NULL;

        // 取出值对象
        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        // 检查 key 是否已过期
        if (dictFind(db->expires,key)) {
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                 * it could happen that all the keys are already logically
                 * expired in the slave, so the function cannot stop because
                 * expireIfNeeded() is false, nor it can stop because
                 * dictGetRandomKey() returns NULL (there are keys to return).
                 * To prevent the infinite loop we do some tries, but if there
                 * are the conditions for an infinite loop, eventually we
                 * return a key name that may be already expired. 
                 *
                 * 如果DB仅由具有过期集的键组成，则可能会发生所有键在逻辑上都已在从机中过期的情况，
                 * 因此该函数不能因为expireIfNeeded（）为false而停止，也不能因为dictGetRandomKey（）
                 * 返回NULL（有键要返回）而停止。为了防止无限循环，我们做了一些尝试，但如果存在无限循环的条件，
                 * 最终我们会返回一个可能已经过期的键名称。
                 * */
                return keyobj;
            }
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                // 这个 key 已过期，继续寻找下个 key
                continue; /* search for another key. This expired. 
                           *
                           * 搜索另一个键。已过期。
                           * */
            }
        }
        return keyobj;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB 
 *
 * 从数据库中删除键、值和相关的过期节点（如果有的话）
 * */
int dbSyncDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. 
     *
     * 从过期dict中删除节点不会释放键的sds，因为它与主字典共享。
     * */
    
    
    // 先删除过期时间
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);

    // 删除 key 和 value
    if (dictDelete(db->dict,key->ptr) == DICT_OK) {
        if (server.cluster_enabled) slotToKeyDel(key->ptr);
        return 1;
    } else {
        return 0;
    }
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. 
 *
 * 这是一个包装器，其行为取决于Redis懒惰免费配置。同步或异步删除键。
 * */
int dbDelete(redisDb *db, robj *key) {
    return server.lazyfree_lazy_server_del ? dbAsyncDelete(db,key) :
                                             dbSyncDelete(db,key);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 
 *
 * 准备存储在“key”中的字符串对象进行破坏性修改，以实现SETBIT或APPEND等命令。
 * 
 * 除非以下两个条件之一为真，否则对象通常可以修改：
 * 1）对象'o'是共享的refcount>1），我们不想影响其他用户。
 * 2） 对象编码不是“RAW”。
 * 
 * 如果函数在上述条件之一（或两者）中找到对象，则字符串对象的非共享/未编码副本将存储
 * 在指定的“db”中的“key”中。否则，将返回对象“o”本身。
 * 
 * 用法：对象“o”是调用者通过在“db”中查找“key”已经获得的，使用模式如下：
 * o=lookupKeyWrite（db，key）；
 * if（checkType（c，o，OBJ_STRING））返回；
 * o=dbUnshareStringValue（db，key，o）；
 * 
 * 此时，调用方已准备好修改对象，例如使用sdscat（）调用来附加一些数据或其他任何内容。
 * */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbOverwrite(db,key,o);
    }
    return o;
}

/* Remove all keys from the database(s) structure. The dbarray argument
 * may not be the server main DBs (could be a backup).
 *
 * The dbnum can be -1 if all the DBs should be emptied, or the specified
 * DB index if we want to empty only a single database.
 * The function returns the number of keys removed from the database(s). 
 *
 * 从数据库结构中删除所有键。dbarray参数可能不是服务器主数据库（可能是备份）。
 * 
 * 如果所有数据库都应该清空，那么dbnum可以是-1；如果我们只想清空一个数据库，
 * 那么它可以是指定的数据库索引。函数返回从数据库中删除的键数。
 * */
long long emptyDbStructure(redisDb *dbarray, int dbnum, int async,
                           void(callback)(void*))
{
    long long removed = 0;
    int startdb, enddb;

    if (dbnum == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbnum;
    }

    for (int j = startdb; j <= enddb; j++) {
        removed += dictSize(dbarray[j].dict);
        if (async) {
            emptyDbAsync(&dbarray[j]);
        } else {
            dictEmpty(dbarray[j].dict,callback);
            dictEmpty(dbarray[j].expires,callback);
        }
        /* Because all keys of database are removed, reset average ttl. 
         *
         * 因为数据库的所有键都被删除了，所以重置平均ttl。
         * */
        dbarray[j].avg_ttl = 0;
        dbarray[j].expires_cursor = 0;
    }

    return removed;
}

/* Remove all keys from all the databases in a Redis server.
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP.
 *
 * On success the function returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. 
 *
 * 从Redis服务器中的所有数据库中删除所有键。如果给定回调，则会不时调用函数，
 * 以发出工作正在进行的信号。
 * 
 * 如果所有的数据库都应该刷新，那么dbnum可以是-1；
 * 如果我们只想刷新一个Redis数据库号，那么它可以是指定的数据库号。
 * 
 * 如果没有指定特殊标志，则标志为EMPTYDB_NO_Flags；如果我们希望在不同的线程中释
 * 放内存并使函数尽快返回，则标志是EMPTYDB_ASYNC。
 * 
 * 成功后，函数将返回从数据库中删除的键数。否则，在数据库编号超出范围的特定情况下返回-1，并且errno设置为EINVAL。
 * */
long long emptyDb(int dbnum, int flags, void(callback)(void*)) {
    int async = (flags & EMPTYDB_ASYNC);
    RedisModuleFlushInfoV1 fi = {REDISMODULE_FLUSHINFO_VERSION,!async,dbnum};
    long long removed = 0;

    if (dbnum < -1 || dbnum >= server.dbnum) {
        errno = EINVAL;
        return -1;
    }

    /* Fire the flushdb modules event. 
     *
     * 激发flushdb模块事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_START,
                          &fi);

    /* Make sure the WATCHed keys are affected by the FLUSH* commands.
     * Note that we need to call the function while the keys are still
     * there. 
     *
     * 确保WATCH键受到FLUSH命令的影响。注意，我们需要在键还在的时候调用函数。
     * */
    signalFlushedDb(dbnum);

    /* Empty redis database structure. 
     *
     * redis数据库结构为空。
     * */
    removed = emptyDbStructure(server.db, dbnum, async, callback);

    /* Flush slots to keys map if enable cluster, we can flush entire
     * slots to keys map whatever dbnum because only support one DB
     * in cluster mode. 
     *
     * 将插槽刷新到键映射如果启用集群，我们可以将整个插槽刷新到映射任何dbnum的键，
     * 因为在集群模式下只支持一个DB。
     * */
    if (server.cluster_enabled) slotToKeyFlush(async);

    if (dbnum == -1) flushSlaveKeysWithExpireList();

    /* Also fire the end event. Note that this event will fire almost
     * immediately after the start event if the flush is asynchronous. 
     *
     * 同时激发结束事件。请注意，如果刷新是异步的，则此事件几乎会在启动事件之后立即激发。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_END,
                          &fi);

    return removed;
}

/* Store a backup of the database for later use, and put an empty one
 * instead of it. 
 *
 * 存储数据库的备份以备将来使用，并放置一个空的备份来代替它。
 * */
dbBackup *backupDb(void) {
    dbBackup *backup = zmalloc(sizeof(dbBackup));

    /* Backup main DBs. 
     *
     * 备份主数据库。
     * */
    backup->dbarray = zmalloc(sizeof(redisDb)*server.dbnum);
    for (int i=0; i<server.dbnum; i++) {
        backup->dbarray[i] = server.db[i];
        server.db[i].dict = dictCreate(&dbDictType,NULL);
        server.db[i].expires = dictCreate(&keyptrDictType,NULL);
    }

    /* Backup cluster slots to keys map if enable cluster. 
     *
     * 如果启用群集，则备份群集插槽到键映射。
     * */
    if (server.cluster_enabled) {
        backup->slots_to_keys = server.cluster->slots_to_keys;
        memcpy(backup->slots_keys_count, server.cluster->slots_keys_count,
            sizeof(server.cluster->slots_keys_count));
        server.cluster->slots_to_keys = raxNew();
        memset(server.cluster->slots_keys_count, 0,
            sizeof(server.cluster->slots_keys_count));
    }

    return backup;
}

/* Discard a previously created backup, this can be slow (similar to FLUSHALL)
 * Arguments are similar to the ones of emptyDb, see EMPTYDB_ flags. 
 *
 * 放弃以前创建的备份，这可能很慢（类似于FLUSHALL）参数与emptyDb的参
 * 数相似，请参阅emptyDb_标志。
 * */
void discardDbBackup(dbBackup *buckup, int flags, void(callback)(void*)) {
    int async = (flags & EMPTYDB_ASYNC);

    /* Release main DBs backup . 
     *
     * 释放主数据库备份。
     * */
    emptyDbStructure(buckup->dbarray, -1, async, callback);
    for (int i=0; i<server.dbnum; i++) {
        dictRelease(buckup->dbarray[i].dict);
        dictRelease(buckup->dbarray[i].expires);
    }

    /* Release slots to keys map backup if enable cluster. 
     *
     * 如果启用集群，释放插槽以映射备份。
     * */
    if (server.cluster_enabled) freeSlotsToKeysMap(buckup->slots_to_keys, async);

    /* Release buckup. 
     *
     * 松开制动带。
     * */
    zfree(buckup->dbarray);
    zfree(buckup);
}

/* Restore the previously created backup (discarding what currently resides
 * in the db).
 * This function should be called after the current contents of the database
 * was emptied with a previous call to emptyDb (possibly using the async mode). 
 *
 * 恢复以前创建的备份（放弃当前驻留在数据库中的备份）。此函数应在数据库的当前内容通
 * 过上一次对emptyDb的调用清空后调用（可能使用异步模式）。
 * */
void restoreDbBackup(dbBackup *buckup) {
    /* Restore main DBs. 
     *
     * 恢复主数据库。
     * */
    for (int i=0; i<server.dbnum; i++) {
        serverAssert(dictSize(server.db[i].dict) == 0);
        serverAssert(dictSize(server.db[i].expires) == 0);
        dictRelease(server.db[i].dict);
        dictRelease(server.db[i].expires);
        server.db[i] = buckup->dbarray[i];
    }

    /* Restore slots to keys map backup if enable cluster. 
     *
     * 如果启用群集，则将插槽恢复到键映射备份。
     * */
    if (server.cluster_enabled) {
        serverAssert(server.cluster->slots_to_keys->numele == 0);
        raxFree(server.cluster->slots_to_keys);
        server.cluster->slots_to_keys = buckup->slots_to_keys;
        memcpy(server.cluster->slots_keys_count, buckup->slots_keys_count,
                sizeof(server.cluster->slots_keys_count));
    }

    /* Release buckup. 
     *
     * 松开制动带。
     * */
    zfree(buckup->dbarray);
    zfree(buckup);
}

// 选择数据库
int selectDb(client *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    c->db = &server.db[id];
    return C_OK;
}

long long dbTotalServerKeyCount() {
    long long total = 0;
    int j;
    for (j = 0; j < server.dbnum; j++) {
        total += dictSize(server.db[j].dict);
    }
    return total;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------
 *
 * 关键点空间更改的挂钩。每次修改数据库中的键时，都会调用函数signalModifiedKey（）。每次刷新数据库时，都会调用函数signalFlushDb（）
 * */

/* Note that the 'c' argument may be NULL if the key was modified out of
 * a context of a client. 
 *
 * 请注意，如果键是在客户端上下文之外修改的，则“c”参数可能为NULL。
 * */
// 通知所有监视 key 的客户端，key 已被修改。
// touchWatchedKey 定义在 multi.c
void signalModifiedKey(client *c, redisDb *db, robj *key) {
    touchWatchedKey(db,key);
    trackingInvalidateKey(c,key);
}

/*
 * FLUSHDB/FLUSHALL 命令调用之后的通知函数
 *
 * touchWatchedKeysOnFlush 定义在 multi.c
 */
void signalFlushedDb(int dbid) {
    int startdb, enddb;
    if (dbid == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbid;
    }

    for (int j = startdb; j <= enddb; j++) {
        touchAllWatchedKeysInDb(&server.db[j], NULL);
    }

    trackingInvalidateKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------
 * 
 * 在键空间上操作的类型不可知命令
 * */

/* Return the set of flags to use for the emptyDb() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * Currently the command just attempts to parse the "ASYNC" option. It
 * also checks if the command arity is wrong.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. 
 *
 * 返回一组标志，用于FLUSHALL和FLUSHDB命令的emptyDb（）调用。
 *
 * 目前，该命令只是试图解析“ASYNC”选项。它还检查命令arity是否错误。
 * 成功时返回C_OK，标志存储在*标志中，否则返回C_ERR，函数向客户端发送错误。
 * */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. 
     *
     * 分析可选的ASYNC选项。
     * */
    if (c->argc > 1) {
        if (c->argc > 2 || strcasecmp(c->argv[1]->ptr,"async")) {
            addReply(c,shared.syntaxerr);
            return C_ERR;
        }
        *flags = EMPTYDB_ASYNC;
    } else {
        *flags = EMPTYDB_NO_FLAGS;
    }
    return C_OK;
}

/* Flushes the whole server data set. 
 *
 * 刷新整个服务器数据集。
 * */
void flushAllDataAndResetRDB(int flags) {
    // 清空所有数据库
    server.dirty += emptyDb(-1,flags,NULL);
    // 如果正在执行数据库的保存工作，那么强制中断它
    if (server.rdb_child_pid != -1) killRDBChild();
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. 
         *
         * 通常rdbSave（）会重置dirty，但我们不希望在这里这样做，因为否则FLU
         * SHALL将不会被复制或放入AOF。
         * */
        int saved_dirty = server.dirty;
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(server.rdb_filename,rsiptr);
        server.dirty = saved_dirty;
    }
    server.dirty++;
#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchroneus. 
     *
     * 当没有流量时，jemalloc 5不会将页面发布回操作系统。对于大型数据库，flushdb会阻塞很长时间，
     * 所以多一点不会造成危害，这样flush和purge将是同步的。
     * */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHDB [ASYNC]
 *
 * Flushes the currently SELECTed Redis DB. 
 *
 * FLUSHDB[ASYNC]刷新当前选定的Redis数据库。
 * */
// 清空客户端当前所使用的数据库
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    server.dirty += emptyDb(c->db->id,flags,NULL);
    addReply(c,shared.ok);
#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchroneus. 
     *
     * 当没有流量时，jemalloc 5不会将页面发布回操作系统。对于大型数据库，flushdb会阻塞很长时间，所以多一点不会造成危害，
     * 这样flush和purge将是同步的。
     * */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. 
 *
 * FLUSHALL[ASYNC]刷新整个服务器数据集。
 * */
// 清空所有数据库
void flushallCommand(client *c) {
    int flags;
    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    flushAllDataAndResetRDB(flags);
    addReply(c,shared.ok);
}

/* This command implements DEL and LAZYDEL. 
 *
 * 此命令实现DEL和LAZYDEL。
 * */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j]);
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            signalModifiedKey(c,c->db,c->argv[j]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            numdel++;
        }
    }
    addReplyLongLong(c,numdel);
}

// 从数据库中删除所有给定 key
void delCommand(client *c) {
    delGenericCommand(c,server.lazyfree_lazy_user_del);
}

void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. 
 *
 * EXISTS键1键2。。。key_N。返回值是现有键的数量。
 * */
// 检查给定 key 是否存在
void existsCommand(client *c) {
    long long count = 0;
    int j;

    for (j = 1; j < c->argc; j++) {
        if (lookupKeyReadWithFlags(c->db,c->argv[j],LOOKUP_NOTOUCH)) count++;
    }
    addReplyLongLong(c,count);
}

// 切换数据库
void selectCommand(client *c) {
    long id;

    // id 号必须是整数
    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != C_OK)
        return;

    // 不允许在集群模式下似乎用 SELECT
    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }

    // 切换数据库
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        addReply(c,shared.ok);
    }
}

/*
 * RANDOMKEY 命令的实现
 *
 * 随机从数据库中返回一个键
 */
void randomkeyCommand(client *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReplyNull(c);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

/*
 * KEYS 命令的实现
 *
 * 查找和给定模式匹配的 key
 */
void keysCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addReplyDeferredLen(c);

    // 指向当前数据库的 key space
    di = dictGetSafeIterator(c->db->dict);
    // key 的匹配模式
    allkeys = (pattern[0] == '*' && plen == 1);
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;

        // 检查当前迭代到的 key 是否匹配，如果是的话，将它返回
        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            // 只返回不过期的 key
            if (!keyIsExpired(c->db,keyobj)) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredArrayLen(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. 
 *
 * scanGenericCommand使用此回调将字典迭代器返回的元素收集到列表中
 * 。
 * */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];
    robj *o = pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = dictGetKey(de);
        sds sdsval = dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. 
 *
 * 尝试分析存储在对象“o”中的SCAN光标：如果光标有效，则将其作为无符号整数存储
 * 到*cursor中，并返回C_OK。否则返回C_ERR并向客户端发送错误。
 * */
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. 
     *
     * 使用strtoul（）是因为我们需要一个*unsignedlong，所以getL
     * ongLongFromObject（）不会覆盖整个光标空间。
     * */
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash, Set or Zset object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. 
 *
 * 该命令执行SCAN、HSCAN和SSCAN命令。如果传递了对象“o”，则它必须是
 * Hash、Set或Zset对象，否则，如果“o”为NULL，则命令将对与当前数据
 * 库关联的字典进行操作。
 *
 * 当“o”不为NULL时，函数假定客户端参数向量中的第一个参数是键，因此在迭代之前跳过它以解析选项。
 *
 * 在Hash对象的情况下，函数返回Hash上每个元素的字段和值。
 * */
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    sds typename = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. 
     *
     * 对象必须为NULL（以迭代键名称），或者对象的类型必须为Set、Sorted Set或Hash。
     * */
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. 
     *
     * 将i设置为第一个选项参数。上一个是光标。
     * */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. 
                              *
                              * 如果需要，跳过关键参数。
                              * */

    /* Step 1: Parse options. 
     *
     * 步骤1:分析选项。
     * */
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. 
             *
             * 如果模式恰好是“*”，那么它总是匹配的，所以这相当于禁用它。
             * */
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "type") && o == NULL && j >= 2) {
            /* SCAN for a particular type only applies to the db dict 
             *
             * 特定类型的SCAN仅适用于数据库dict
             * */
            typename = c->argv[i+1]->ptr;
            i+= 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. 
     *
     * 第2步：迭代集合。请注意，如果对象是用ziplist、intset或任何其他非哈
     * 希表的表示进行编码的，那么我们确信它也由少量元素组成。因此，为了避免进入状态，我
     * 们只需在一次调用中返回对象内部的所有内容，将光标设置为零以表示迭代结束。
     * */

    /* Handle the case of a hash table. 
     *
     * 处理哈希表的情况。
     * */
    ht = NULL;
    if (o == NULL) {
        ht = c->db->dict;
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. 
                     *
                     * 我们返回此类型的键/值。
                     * */
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. 
                     *
                     * 我们返回此类型的键/值。
                     * */
    }

    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. 
         *
         * 我们将最大迭代次数设置为指定COUNT的十倍，因此，如果哈希表处于病态状态（非常
         * 稀疏），我们避免以不返回或返回很少元素为代价来阻塞太多时间。
         * */
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. 
         *
         * 我们向回调传递两个指针：它将向其中添加新元素的列表，以及包含字典的对象，以便可以
         * 以依赖于类型的方式获取更多数据。
         * */
        privdata[0] = keys;
        privdata[1] = o;
        do {
            cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count);
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. 
     *
     * 步骤3：滤芯。
     * */
    node = listFirst(keys);
    while (node) {
        robj *kobj = listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. 
         *
         * 如果元素与模式不匹配，则对其进行筛选。
         * */
        if (!filter && use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter an element if it isn't the type we want. 
         *
         * 如果元素不是我们想要的类型，请对其进行筛选。
         * */
        if (!filter && o == NULL && typename){
            robj* typecheck = lookupKeyReadWithFlags(c->db, kobj, LOOKUP_NOTOUCH);
            char* type = getObjectTypeName(typecheck);
            if (strcasecmp((char*) typename, type)) filter = 1;
        }

        /* Filter element if it is an expired key. 
         *
         * 如果是过期键，则筛选元素。
         * */
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associated value if needed. 
         *
         * 如果需要，请删除元素及其关联值。
         * */
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. 
         *
         * 如果这是一个散列或排序集，我们有一个键值元素的平面列表，所以如果这个元素被过滤了，
         * 请删除该值，或者如果它没有被过滤，则跳过它：我们只匹配键。
         * */
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
            if (filter) {
                kobj = listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. 
     *
     * 步骤4：回复客户。
     * */
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyArrayLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);
    listRelease(keys);
}

/* The SCAN command completely relies on scanGenericCommand. 
 *
 * SCAN命令完全依赖于scanGenericCommand。
 * */
void scanCommand(client *c) {
    unsigned long cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

/*
 * DBSIZE 命令的实现
 *
 * 返回数据库键值对数量
 */
void dbsizeCommand(client *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}

/*
 * LASTSAVE 命令的实现
 *
 * 返回数据库的最后保存时间
 */
void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

char* getObjectTypeName(robj *o) {
    char* type;
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        case OBJ_STREAM: type = "stream"; break;
        case OBJ_MODULE: {
            moduleValue *mv = o->ptr;
            type = mv->type->name;
        }; break;
        default: type = "unknown"; break;
        }
    }
    return type;
}

/*
 * TYPE 命令的实现
 *
 * 返回 key 对象类型的字符串形式
 */
void typeCommand(client *c) {
    robj *o;
    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    addReplyStatus(c, getObjectTypeName(o));
}

/*
 * 关闭服务器
 */
void shutdownCommand(client *c) {
    int flags = 0;

    // 选择关闭的模式
    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    // 关闭
    if (prepareForShutdown(flags) == C_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

// 对 key 进行改名
void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. 
     *
     * 当source和dest键相同时，如果该键存在，则不执行任何操作，但我们仍然会在
     * 未存在的键上返回错误。
     * */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;

    // 取出源 key
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

    incrRefCount(o);
    expire = getExpire(c->db,c->argv[1]);
    // 取出目标 key
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        // 如果目标 key 存在，且 nx FLAG 打开，那么设置失败，直接返回
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. 
         *
         * 覆盖：在创建具有相同名称的新键之前，先删除旧键。
         * */
        // 否则，将目标 key 删除
        dbDelete(c->db,c->argv[2]);
    }
    // 将源对象以目标 key 的名字添加到数据库
    dbAdd(c->db,c->argv[2],o);
    // 如果源 key 有超时时间，那么设置新 key 的超时时间
    if (expire != -1) setExpire(c,c->db,c->argv[2],expire);
    // 删除旧的源 key
    dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

// 将 key 从一个数据库移动到另一个数据库
void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    long long dbid, expire;

    // 不允许在集群情况下使用
    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers 
     *
     * 获取源和目标数据库指针
     * */
    // 记录源数据库
    src = c->db;
    srcid = c->db->id;

    // 通过切换数据库来测试目标数据库是否存在
    if (getLongLongFromObject(c->argv[2],&dbid) == C_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,dbid) == C_ERR)
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
    // 记录目标数据库
    dst = c->db;
    // 切换回源数据库
    selectDb(c,srcid); /* Back to the source DB 
                        *
                        * 返回到源数据库
                        * */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. 
     *
     * 如果用户使用与源数据库相同的数据库作为目标进行移动，则可能是一个错误。
     * */
    // 源数据库和目标数据库相同，直接返回
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference 
     *
     * 检查元素是否存在并获取引用
     * */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB 
     *
     * 如果键已存在于目标数据库中，则返回零
     * */
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    // 将 key 添加到目标数据库
    dbAdd(dst,c->argv[1],o);
    if (expire != -1) setExpire(c,dst,c->argv[1],expire);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB 
     *
     * 好啊键已移动，删除源数据库中的 key
     * */
    dbDelete(src,c->argv[1]);
    signalModifiedKey(c,src,c->argv[1]);
    signalModifiedKey(c,dst,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_from",c->argv[1],src->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_to",c->argv[1],dst->id);

    server.dirty++;
    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment
 * where the function is used for more info. 
 *
 * dbSwapDatabases（）的Helper函数：扫描具有一个或多个被阻止客
 * 户端的键列表，查找B[LR]POP或其他阻止命令，并在键类型正确的情况下发出
 * 就绪信号。有关详细信息，请参阅使用该函数的注释。
 * */
void scanDatabaseForReadyLists(redisDb *db) {
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        if (value && (value->type == OBJ_LIST ||
                      value->type == OBJ_STREAM ||
                      value->type == OBJ_ZSET))
            signalKeyAsReady(db, key);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. 
 *
 * 在运行时交换两个数据库，这样所有客户端即使已经连接，也能神奇地看到新数据库。请注
 * 意，客户端结构c->db指向一个给定的db，因此我们需要更聪明地交换底层引用的结
 * 构，否则我们需要修复对Redis db结构的所有引用。如果至少有一个DB ID超
 * 出范围，则返回C_ERR，否则返回C_OK。
 * */
int dbSwapDatabases(long id1, long id2) {
    if (id1 < 0 || id1 >= server.dbnum ||
        id2 < 0 || id2 >= server.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    redisDb aux = server.db[id1];
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. 
     *
     * 交换哈希表。请注意，我们不交换blocking_keys、ready_keys和
     * watched_keys，因为我们希望客户端保持在原来的DB中。
     * */
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;
    db1->expires_cursor = db2->expires_cursor;

    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;
    db2->expires_cursor = aux.expires_cursor;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed.
     *
     * Also the swapdb should make transaction fail if there is any
     * client watching keys 
     *
     * 现在，我们需要处理列表上被阻止的客户端：作为交换两个DB的效果，如果X在交换后恰
     * 好存在于新版本的DB中，那么在给定DB中等待列表X的客户端现在可能实际上被取消阻
     * 止。
     *
     * 然而，通常情况下，我们只在创建列表时在dbAdd（）中出于效率原因进行此检查.
     *
     *
     * 因此，在这里，我们需要重新扫描列表中被阻止的客户端列表，并在需要时将信号列表视
     * 为就绪。此外，如果有任何客户端监视键，swapdb应该使事务失败
     * */
    scanDatabaseForReadyLists(db1);
    touchAllWatchedKeysInDb(db1, db2);
    scanDatabaseForReadyLists(db2);
    touchAllWatchedKeysInDb(db2, db1);
    return C_OK;
}

/* SWAPDB db1 db2 
 *
 * SWAPDB db1 db2
 * */
void swapdbCommand(client *c) {
    long id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. 
     *
     * 在集群模式下是不允许的：我们只有DB 0。
     * */
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. 
     *
     * 获取两个DB索引。
     * */
    if (getLongFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getLongFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... 
     *
     * 交换
     * */
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        RedisModuleSwapDbInfo si = {REDISMODULE_SWAPDBINFO_VERSION,id1,id2};
        moduleFireServerEvent(REDISMODULE_EVENT_SWAPDB,0,&si);
        server.dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

// 移除 key 的过期时间
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. 
     *
     * 只有当主dict中有相应的节点时，才能删除expire。否则，键将永远不会被释
     * 放。
     * */
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. 
 *
 * 将过期设置为指定的键。如果在用户调用命令的上下文中设置了expire，则“c”
 * 是客户端，否则“c”将设置为NULL。“when”参数是以毫秒为单位的绝对unix时间，在此时间之后，键将不再被视为有效。
 */
// 为 key 设置过期时间
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict 
     *
     * 在过期dict中重用来自主dict的sds
     * */
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    de = dictAddOrFind(db->expires,dictGetKey(kde));
    dictSetSignedIntegerVal(de,when);

    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) 
 *
 * 返回指定键的过期时间，如果没有与该键关联的过期时间（即键是非易失性的），则
 * 返回-1
 * */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP 
     *
     * 没有过期？尽快返回
     * */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). 
     *
     * 该节点是在过期dict中找到的，这意味着它也应该存在于主dict（安全检查）中。
     * */
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    // 取出字典值中保存的整数值
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. 
 *
 * 将过期时间传播到从属文件和AOF文件中。当主键到期时，该键的DEL操作将发送
 * 到所有从键和AOF文件（如果启用）。通过这种方式，键到期集中在一个地方，并且
 * 由于AOF和主->从链路都保证了操作顺序，即使我们允许对到期键进行写操作，一切
 * 都将是一致的。
 * */
/*
 * 向附属节点和 AOF 文件传播过期命令
 *
 * 当一个键在主节点中过期时，传播一个 DEL 命令到所有附属节点和 AOF 文件
 *
 * 通过将删除过期键的工作集中在主节点中，可以维持数据库的一致性。
 */
void propagateExpire(redisDb *db, robj *key, int lazy) {
    robj *argv[2];

    // DEL 命名
    argv[0] = lazy ? shared.unlink : shared.del;
    // 目标键
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    if (server.aof_state != AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
    replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. 
 *
 * 检查键是否过期。
 * */
int keyIsExpired(redisDb *db, robj *key) {
    mstime_t when = getExpire(db,key);
    mstime_t now;

    if (when < 0) return 0; /* No expire for this key 
                             *
                             * 此键没有过期
                             * */

    /* Don't expire anything while loading. It will be done later. 
     *
     * 加载时不要使任何内容过期。稍后再做。
     * */
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. 
     *
     * 如果我们在Lua脚本的上下文中，我们假设时间被阻塞到Lua脚本开始的时间。通过这
     * 种方式，键只能在第一次访问时过期，而不能在脚本执行过程中过期，从而使对从属/AOF的传播保持一致。
     * 有关更多信息，请参阅Github上的第1525期。
     * */
    if (server.lua_caller) {
        now = server.lua_time_start;
    }
    /* If we are in the middle of a command execution, we still want to use
     * a reference time that does not change: in that case we just use the
     * cached time, that we update before each call in the call() function.
     * This way we avoid that commands such as RPOPLPUSH or similar, that
     * may re-open the same key multiple times, can invalidate an already
     * open object in a next call, if the next call will see the key expired,
     * while the first did not. 
     *
     * 如果我们正在执行命令，我们仍然希望使用一个不变的引用时间：在这种情况下，我们只使
     * 用缓存的时间，我们在call（）函数中的每个调用之前都会更新它。通过这种方式，我
     * 们可以避免像RPOPLPUSH或类似的命令，这些命令可能会多次重新打开同一个键
     * ，如果下一个调用将看到键过期，而第一个调用没有过期，则会在下一次调用中使已经打
     * 开的对象无效。
     * */
    else if (server.fixed_time_expire > 0) {
        now = server.mstime;
    }
    /* For the other cases, we want to use the most fresh time we have. 
     *
     * 对于其他情况，我们希望利用最新鲜的时间。
     * */
    else {
        now = mstime();
    }

    /* The key expired if the current (virtual or real) time is greater
     * than the expire time of the key. 
     *
     * 如果当前（虚拟或真实）时间大于键的过期时间，则键过期。
     * */
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. 
 *
 * 当我们要对给定的键执行某些操作时，会调用此函数，但即使该键仍存在于数据库中，
 * 它也可能在逻辑上已经过期。调用此函数的主要方式是通过lookupKey*（）函数
 * 族。
 *
 * 函数的行为取决于实例的复制角色，因为从实例不会使键过期，它们会等待来自主实
 * 例的DEL来处理一致性问题。然而，即使从机也会尝试为函数提供一致的返回值，这样在
 * 从机端执行的读取命令将能够表现得像键过期一样，即使键仍然存在（因为主机尚未传
 * 播DEL）。
 *
 * 在master中，作为找到过期键的副作用，该键将从数据库中逐出。
 * 这也可能触发在AOF/复制流中传播DEL/UNLINK命令。
 *
 * 如果键仍然有效，则 函数的返回值为0，否则，如果键过期，则函数返回1。
 * */
/*
 * 如果 key 已经过期，那么将它删除，否则，不做动作。
 *
 * key 没有过期时间、服务器正在载入或 key 未过期时，返回 0
 * key 已过期，那么返回正数值
 */
int expireIfNeeded(redisDb *db, robj *key) {
    // key 没有过期时间，直接返回
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a slave, instead of
     * evicting the expired key from the database, we return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. 
     *
     * 如果我们在从机的上下文中运行，而不是从数据库中驱逐过期的键，我们会返回ASAP
     * ：从机键的过期由主机控制，主机会向我们发送过期键的合成DEL操作。尽管如此，
     * 我们还是会尝试向调用者返回正确的信息，即，如果我们认为键应该仍然有效，则返回0
     * ；如果我们认为此时键已过期，则返回1。
     * */
    // 如果服务器作为附属节点运行，那么直接返回
    // 因为附属节点的过期是由主节点通过发送 DEL 命令来删除的
    // 不必自主删除
    if (server.masterhost != NULL) return 1;

    /* Delete the key 
     *
     * 删除键
     * */
    // 传播过期命令
    server.stat_expiredkeys++;
    // 传播过期命令
    propagateExpire(db,key,server.lazyfree_lazy_expire);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",key,db->id);
    // 从数据库中删除 key
    int retval = server.lazyfree_lazy_expire ? dbAsyncDelete(db,key) :
                                               dbSyncDelete(db,key);
    if (retval) signalModifiedKey(NULL,db,key);
    return retval;
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * API 从命令中获取关键参数
 * ---------------------------------------------------------------------------*/

/* Prepare the getKeysResult struct to hold numkeys, either by using the
 * pre-allocated keysbuf or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 
 *
 * 通过使用预先分配的keysbuf或在堆上分配一个新数组，准备getKeysResult结构以保存numkeys。
 * 在开始填充结果之前，必须至少调用一次此函数，并且
 * 可以重复调用此函数以扩大结果数组。
 * */
int *getKeysPrepareResult(getKeysResult *result, int numkeys) {
    /* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the pre-allocated stack
     * buffer here. 
     *
     * GETKEYS_RESULT_INIT将键初始化为NULL，将其指向此处预先分配
     * 的堆栈缓冲区。
     * */
    if (!result->keys) {
        serverAssert(!result->numkeys);
        result->keys = result->keysbuf;
    }

    /* Resize if necessary 
     *
     * 必要时调整大小
     * */
    if (numkeys > result->size) {
        if (result->keys != result->keysbuf) {
            /* We're not using a static buffer, just (re)alloc 
             *
             * 我们没有使用静态缓冲区，只是（重新）分配
             * */
            result->keys = zrealloc(result->keys, numkeys * sizeof(int));
        } else {
            /* We are using a static buffer, copy its contents 
             *
             * 我们正在使用静态缓冲区，复制其内容
             * */
            result->keys = zmalloc(numkeys * sizeof(int));
            if (result->numkeys)
                memcpy(result->keys, result->keysbuf, result->numkeys * sizeof(int));
        }
        result->size = numkeys;
    }

    return result->keys;
}

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). 
 *
 * 基本情况是使用命令表中给定的键位置（firstkey、lastkey、step）
 * 。
 * */
int getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    if (cmd->firstkey == 0) {
        result->numkeys = 0;
        return 0;
    }

    last = cmd->lastkey;
    if (last < 0) last = argc+last;

    int count = ((last - cmd->firstkey)+1);
    keys = getKeysPrepareResult(result, count);

    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        if (j >= argc) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. 
             *
             * 模块命令和参数数量不固定的标准命令（负arity参数）没有调度时间arity检查，
             * 因此我们需要处理用户在此处传递无效参数数量的情况。在这种情况下，我们不返回任何
             * 键，并期望命令实现报告arity或语法错误。
             * */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                getKeysFreeResult(result);
                result->numkeys = 0;
                return 0;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i++] = j;
    }
    result->numkeys = i;
    return i;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. 
 *
 * 返回所有参数，这些参数是通过argc/argv传递的命令中的键。该命令返回数组中
 * 所有键参数的位置，因此实际返回值是一个堆分配的整数数组。数组的长度通过引用返回到
 * *numkeys中。'根据argv[0]中的命令名称，cmd必须指向rediCo
 * mmand表中的相应节点。如果不需要特定于命令的辅助函数，则此函数使用命令表，否
 * 则它将调用特定于命令函数。
 * */
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,result);
    } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,result);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,result);
    }
}

/* Free the result of getKeysFromCommand. 
 *
 * 释放getKeysFromCommand的结果。
 * */
void getKeysFreeResult(getKeysResult *result) {
    if (result && result->keys != result->keysbuf)
        zfree(result->keys);
}

/* Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> 
 *
 * 从以下命令中提取键的Helper函数：ZUNIONSTORE＜destkey＞
 * ＜num keys＞＜key＞＜key<key><options>ZINTERS
 * TORE<destkey><num keys><key><key><按键><op
 * tions>
 * */
int zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. 
     *
     * 卫生检查。如果命令将以语法错误进行回复，则不要返回任何键。
     * */
    if (num < 1 || num > (argc-3)) {
        result->numkeys = 0;
        return 0;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect 
     *
     * z中的关键点{union,inter}store来自两个位置：argv[1]=存
     * 储键，argv[3]…n]=要相交的键
     * */
    /* Total keys = {union,inter} keys + storage key 
     *
     * 键总数=｛联合，内部｝键+存储键
     * */
    keys = getKeysPrepareResult(result, num+1);
    result->numkeys = num+1;

    /* Add all key positions for argv[3...n] to keys[] 
     *
     * 将argv[3…n]的所有键位置添加到键[]
     * */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). 
     *
     * 最后添加argv[1]键位置（存储键目标）。
     * */
    keys[num] = 1;

    return result->numkeys;
}

/* Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] 
 *
 * 从以下命令提取键的Helper函数：EVAL<script><num-keys
 * ><key><key><key>[更多内容]EVALSHA<script><nu
 * m keys><key><key><key>[更多内容]
 * */
int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. 
     *
     * 卫生检查。如果命令将以语法错误进行回复，则不要返回任何键。
     * */
    if (num <= 0 || num > (argc-3)) {
        result->numkeys = 0;
        return 0;
    }

    keys = getKeysPrepareResult(result, num);
    result->numkeys = num;

    /* Add all key positions for argv[3...n] to keys[] 
     *
     * 将argv[3…n]的所有键位置添加到键[]
     * */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return result->numkeys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. 
 *
 * 从SORT命令中提取键的Helper函数。SORT＜排序键＞。。。STORE<
 * 存储键>。。。SORT的第一个参数始终是一个键，但是后面是类似SQL风格的选项
 * 列表。在这里，我们只解析最小值，以便正确识别“STORE”选项中的键。
 * */
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);

    num = 0;
    keys = getKeysPrepareResult(result, 2); /* Alloc 2 places for the worst case. 
                                             *
                                             * 在最坏的情况下分配2个名额。
                                             * */
    keys[num++] = 1; /* <sort-key> is always present. 
                      *
                      * ＜sort key＞始终存在。
                      * */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. 
     *
     * 搜索STORE选项。默认情况下，我们认为选项没有参数，所以如果我们发现一个未知的
     * 选项名称，我们会扫描下一个。然而，也有带有1或2个参数的选项，因此我们在这里提供
     * 了一个列表，以便跳过正确数量的参数。
     * */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. 
                   *
                   * 元素结束。
                   * */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. 
                 *
                 * 注意：我们在这里不增加“num”，并继续循环以确保在提供多个选项的情况下处理*最
                 * 后一个“STORE”选项。这与SORT的行为相同。
                 * */
                found_store = 1;
                keys[num] = i+1; /* <store-key> 
                                  *
                                  * <存储键>
                                  * */
                break;
            }
        }
    }
    result->numkeys = num + found_store;
    return result->numkeys;
}

int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, first, *keys;
    UNUSED(cmd);

    /* Assume the obvious form. 
     *
     * 采用明显的形式。
     * */
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. 
     *
     * 但是使用KEYS选项检查扩展的。
     * */
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;
                num = argc-first;
                break;
            }
        }
    }

    keys = getKeysPrepareResult(result, num);
    for (i = 0; i < num; i++) keys[i] = first+i;
    result->numkeys = num;
    return num;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ... */
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command 
     *
     * 检查命令中是否存在存储的键
     * */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         
         *
         * 对于用户同时指定“store”和“storedist”选项的情况，指定的第二个键
         * 将覆盖第一个键。此行为与georadiusCommand方法中的行为保持相同。
         * */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     
     *
     * 命令中的键来自两个位置：argv[1]=键，argv[5]…n]=存储的键
     * （如果存在）
     * */
    keys = getKeysPrepareResult(result, num);

    /* Add all key positions to keys[] 
     *
     * 将所有关键位置添加到关键点[]
     * */
    keys[0] = 1;
    if(num > 1) {
         keys[1] = stored_key;
    }
    result->numkeys = num;
    return num;
}

/* LCS ... [KEYS <key1> <key2>] ... 
 *
 * LCS。。。[KEYS＜key1>＜key2>]。。。
 * */
int lcsGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i;
    int *keys = getKeysPrepareResult(result, 2);
    UNUSED(cmd);

    /* We need to parse the options of the command in order to check for the
     * "KEYS" argument before the "STRINGS" argument. 
     *
     * 我们需要分析命令的选项，以便在“STRING”参数之前检查“KEYS”参数。
     * */
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        int moreargs = (argc-1) - i;

        if (!strcasecmp(arg, "strings")) {
            break;
        } else if (!strcasecmp(arg, "keys") && moreargs >= 2) {
            keys[0] = i+1;
            keys[1] = i+2;
            result->numkeys = 2;
            return result->numkeys;
        }
    }
    result->numkeys = 0;
    return result->numkeys;
}

/* Helper function to extract keys from memory command.
 * MEMORY USAGE <key> 
 *
 * 从内存命令中提取键的Helper函数。记忆使用<key>
 * */
int memoryGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);

    getKeysPrepareResult(result, 1);
    if (argc >= 3 && !strcasecmp(argv[1]->ptr,"usage")) {
        result->keys[0] = 2;
        result->numkeys = 1;
        return result->numkeys;
    }
    result->numkeys = 0;
    return 0;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N 
 *
 * XREAD[BLOCK<毫秒>][COUNT<计数>][GROUP<组名><tt
 * l>]STREAMS key_1 key_2。。。键_N ID_1 ID_2。
 * 。。ID_N
 * */
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num = 0, *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the
     * name of the stream key. 
     *
     * 我们需要解析命令的选项，以便查找第一个“STREAMS”字符串，它实际上就是选项
     * 。这是必要的，因为“STREAMS”也可以是使用者组的名称，甚至是流键的名称。
     * */
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. 
                  *
                  * 跳过选项参数。
                  * */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. 
                  *
                  * 跳过选项参数。
                  * */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. 
                  *
                  * 跳过选项参数。
                  * */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. 
             *
             * 没事可做。
             * */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. 
                    *
                    * 语法错误。
                    * */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. 
                    *
                    * 语法错误。
                    * */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        result->numkeys = 0;
        return 0;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key. 
               *
               * 我们有一半的键，因为有参数，因为还有ID，每个键一个。
               * */

    keys = getKeysPrepareResult(result, num);
    for (i = streams_pos+1; i < argc-num; i++) keys[i-streams_pos-1] = i;
    result->numkeys = num;
    return num;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot. 
 *
 * 插槽到键API。Redis Cluster使用它来快速获取属于指定哈希槽的键。
 * 当我们需要了解是否有给定哈希槽的键时，这在重新哈希集群和其他情况下非常有用。
 * */
void slotToKeyUpdateKey(sds key, int add) {
    size_t keylen = sdslen(key);
    unsigned int hashslot = keyHashSlot(key,keylen);
    unsigned char buf[64];
    unsigned char *indexed = buf;

    server.cluster->slots_keys_count[hashslot] += add ? 1 : -1;
    if (keylen+2 > 64) indexed = zmalloc(keylen+2);
    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    memcpy(indexed+2,key,keylen);
    if (add) {
        raxInsert(server.cluster->slots_to_keys,indexed,keylen+2,NULL,NULL);
    } else {
        raxRemove(server.cluster->slots_to_keys,indexed,keylen+2,NULL);
    }
    if (indexed != buf) zfree(indexed);
}

void slotToKeyAdd(sds key) {
    slotToKeyUpdateKey(key,1);
}

void slotToKeyDel(sds key) {
    slotToKeyUpdateKey(key,0);
}

/* Release the radix tree mapping Redis Cluster keys to slots. If 'async'
 * is true, we release it asynchronously. 
 *
 * 释放将Redis Cluster键映射到插槽的基数树。如果“async”为true，我们将异步释放它。
 * */
void freeSlotsToKeysMap(rax *rt, int async) {
    if (async) {
        freeSlotsToKeysMapAsync(rt);
    } else {
        raxFree(rt);
    }
}

/* Empty the slots-keys map of Redis CLuster by creating a new empty one and
 * freeing the old one. 
 *
 * 通过创建一个新的空插槽并释放旧插槽，清空Redis CLuster的插槽键映射。
 * */
void slotToKeyFlush(int async) {
    rax *old = server.cluster->slots_to_keys;

    server.cluster->slots_to_keys = raxNew();
    memset(server.cluster->slots_keys_count,0,
           sizeof(server.cluster->slots_keys_count));
    freeSlotsToKeysMap(old, async);
}

/* Pupulate the specified array of objects with keys in the specified slot.
 * New objects are returned to represent keys, it's up to the caller to
 * decrement the reference count to release the keys names. 
 *
 * 用指定插槽中的键对指定的对象数组进行瞳孔放大。返回新对象来表示键，这取决于调用者
 * 减少引用计数以释放键名称。
 * */
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    raxSeek(&iter,">=",indexed,2);
    while(count-- && raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        keys[j++] = createStringObject((char*)iter.key+2,iter.key_len-2);
    }
    raxStop(&iter);
    return j;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. 
 *
 * 删除指定哈希槽中的所有键。将返回删除的项目数。
 * */
unsigned int delKeysInSlot(unsigned int hashslot) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    while(server.cluster->slots_keys_count[hashslot]) {
        raxSeek(&iter,">=",indexed,2);
        raxNext(&iter);

        robj *key = createStringObject((char*)iter.key+2,iter.key_len-2);
        dbDelete(&server.db[0],key);
        decrRefCount(key);
        j++;
    }
    raxStop(&iter);
    return j;
}

unsigned int countKeysInSlot(unsigned int hashslot) {
    return server.cluster->slots_keys_count[hashslot];
}
