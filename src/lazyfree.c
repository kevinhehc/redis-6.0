#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "cluster.h"

static size_t lazyfree_objects = 0;
pthread_mutex_t lazyfree_objects_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Return the number of currently pending objects to free. 
 *
 * 返回当前要释放的挂起对象的数量。
 * */
size_t lazyfreeGetPendingObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfree_objects,aux);
    return aux;
}

/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is composed of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actually logical composed of multiple
 * elements.
 *
 * For lists the function returns the number of elements in the quicklist
 * representing the list. 
 *
 * 返回释放对象所需的工作量。返回值并不总是对象所包含的实际分配数，而是与之成比例的数字。
 * 
 * 对于字符串，函数总是返回1。
 * 
 * 对于由哈希表或其他数据结构表示的聚合对象，函数只返回对象所包含的元素数。
 * 由单个分配组成的对象总是被报告为具有单个项，即使它们实际上是由多个元素组成的逻辑对象。
 * 
 * 对于列表，函数返回表示列表的快速列表中的元素数。
 * */
size_t lazyfreeGetFreeEffort(robj *obj) {
    if (obj->type == OBJ_LIST) {
        quicklist *ql = obj->ptr;
        return ql->len;
    } else if (obj->type == OBJ_SET && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else if (obj->type == OBJ_ZSET && obj->encoding == OBJ_ENCODING_SKIPLIST){
        zset *zs = obj->ptr;
        return zs->zsl->length;
    } else if (obj->type == OBJ_HASH && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else if (obj->type == OBJ_STREAM) {
        size_t effort = 0;
        stream *s = obj->ptr;

        /* Make a best effort estimate to maintain constant runtime. Every macro
         * node in the Stream is one allocation. 
         *
         * 尽最大努力估计以保持恒定的运行时间。流中的每个宏节点都是一个分配。
         * */
        effort += s->rax->numnodes;

        /* Every consumer group is an allocation and so are the entries in its
         * PEL. We use size of the first group's PEL as an estimate for all
         * others. 
         *
         * 每个消费者组都是一个分配，其PEL中的节点也是如此。我们使用第一组PEL的大小作
         * 为对所有其他组的估计。
         * */
        if (s->cgroups && raxSize(s->cgroups)) {
            raxIterator ri;
            streamCG *cg;
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            /* There must be at least one group so the following should always
             * work. 
             *
             * 必须至少有一个组，以便以下各项始终有效。
             * */
            serverAssert(raxNext(&ri));
            cg = ri.data;
            effort += raxSize(s->cgroups)*(1+raxSize(cg->pel));
            raxStop(&ri);
        }
        return effort;
    } else {
        return 1; /* Everything else is a single allocation. 
                   *
                   * 其他一切都是单一的分配。
                   * */
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB.
 * If there are enough allocations to free the value object may be put into
 * a lazy free list instead of being freed synchronously. The lazy free list
 * will be reclaimed in a different bio.c thread. 
 *
 * 从数据库中删除键、值和相关的过期节点（如果有的话）。如果有足够的分配来释放值对象，
 * 则可以将其放入延迟释放列表，而不是同步释放。懒惰空闲列表将在另一个bio.c线程中回收。
 * */
#define LAZYFREE_THRESHOLD 64
int dbAsyncDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. 
     *
     * 从过期dict中删除节点不会释放键的sds，因为它与主字典共享。
     * */
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);

    /* If the value is composed of a few allocations, to free in a lazy way
     * is actually just slower... So under a certain limit we just free
     * the object synchronously. 
     *
     * 如果值由几个分配组成，那么以懒惰的方式释放实际上只是更慢。。。因此，在一定的限制
     * 下，我们只是同步地释放对象。
     * */
    dictEntry *de = dictUnlink(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetVal(de);
        size_t free_effort = lazyfreeGetFreeEffort(val);

        /* If releasing the object is too much work, do it in the background
         * by adding the object to the lazy free list.
         * Note that if the object is shared, to reclaim it now it is not
         * possible. This rarely happens, however sometimes the implementation
         * of parts of the Redis core may call incrRefCount() to protect
         * objects, and then call dbDelete(). In this case we'll fall
         * through and reach the dictFreeUnlinkedEntry() call, that will be
         * equivalent to just calling decrRefCount(). 
         *
         * 如果释放对象的工作量太大，可以在后台将对象添加到惰性空闲列表中。请注意，如果对象
         * 是共享的，那么现在就不可能回收它。这种情况很少发生，但有时Redis核心部分的实
         * 现可能会调用incrRefCount（）来保护对象，然后调用dbDelete（）。在这种情况下，
         * 我们将调用dictFreeUnlinkedEntry（），这相当于只调用decRefCount（）。
         * */
        if (free_effort > LAZYFREE_THRESHOLD && val->refcount == 1) {
            atomicIncr(lazyfree_objects,1);
            // 异步释放内存的处理
            bioCreateBackgroundJob(BIO_LAZY_FREE,val,NULL,NULL);
            dictSetVal(db->dict,de,NULL);
        }
    }

    /* Release the key-val pair, or just the key if we set the val
     * field to NULL in order to lazy free it later. 
     *
     * 释放键-val对，或者如果我们将val字段设置为NULL以稍后延迟释放它，则只释放键。
     * */
    if (de) {
        dictFreeUnlinkedEntry(db->dict,de);
        if (server.cluster_enabled) slotToKeyDel(key->ptr);
        return 1;
    } else {
        return 0;
    }
}

/* Free an object, if the object is huge enough, free it in async way. 
 *
 * 释放一个对象，如果对象足够大，则以异步方式释放它。
 * */
void freeObjAsync(robj *o) {
    size_t free_effort = lazyfreeGetFreeEffort(o);
    if (free_effort > LAZYFREE_THRESHOLD && o->refcount == 1) {
        atomicIncr(lazyfree_objects,1);
        // 异步释放内存的处理
        bioCreateBackgroundJob(BIO_LAZY_FREE,o,NULL,NULL);
    } else {
        decrRefCount(o);
    }
}

/* Empty a Redis DB asynchronously. What the function does actually is to
 * create a new empty set of hash tables and scheduling the old ones for
 * lazy freeing. 
 *
 * 异步清空Redis数据库。该函数实际所做的是创建一组新的空哈希表，并对旧的哈希表
 * 进行调度以进行懒惰释放。
 * */
void emptyDbAsync(redisDb *db) {
    dict *oldht1 = db->dict, *oldht2 = db->expires;
    db->dict = dictCreate(&dbDictType,NULL);
    db->expires = dictCreate(&keyptrDictType,NULL);
    atomicIncr(lazyfree_objects,dictSize(oldht1));
    // 异步释放内存的处理
    bioCreateBackgroundJob(BIO_LAZY_FREE,NULL,oldht1,oldht2);
}

/* Release the radix tree mapping Redis Cluster keys to slots asynchronously. 
 *
 * 释放将Redis Cluster键异步映射到插槽的基数树。
 * */
void freeSlotsToKeysMapAsync(rax *rt) {
    atomicIncr(lazyfree_objects,rt->numele);
    // 异步释放内存的处理
    bioCreateBackgroundJob(BIO_LAZY_FREE,NULL,NULL,rt);
}

/* Release objects from the lazyfree thread. It's just decrRefCount()
 * updating the count of objects to release. 
 *
 * 从lazyfree线程中释放对象。它只是decoRefCount（）更新要释放的对象的计数。
 * */
void lazyfreeFreeObjectFromBioThread(robj *o) {
    decrRefCount(o);
    atomicDecr(lazyfree_objects,1);
}

/* Release a database from the lazyfree thread. The 'db' pointer is the
 * database which was substituted with a fresh one in the main thread
 * when the database was logically deleted. 
 *
 * 从lazyfree线程中释放数据库。“db”指针是在逻辑删除数据库时在主线程中用新指针替换的数据库。
 * */
void lazyfreeFreeDatabaseFromBioThread(dict *ht1, dict *ht2) {
    size_t numkeys = dictSize(ht1);
    dictRelease(ht1);
    dictRelease(ht2);
    atomicDecr(lazyfree_objects,numkeys);
}

/* Release the radix tree mapping Redis Cluster keys to slots in the
 * lazyfree thread. 
 *
 * 释放将Redis Cluster键映射到lazyfree线程中插槽的基数树。
 * */
void lazyfreeFreeSlotsMapFromBioThread(rax *rt) {
    size_t len = rt->numele;
    raxFree(rt);
    atomicDecr(lazyfree_objects,len);
}
