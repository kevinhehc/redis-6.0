/* Maxmemory directive handling (LRU eviction and other policies).
 * 最大内存指令处理（LRU 逐出和其他策略）。
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "atomicvar.h"

/* ----------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------*/

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across freeMemoryIfNeeded() calls.
 *
 * Entries inside the eviction pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * When an LFU policy is used instead, a reverse frequency indication is used
 * instead of the idle time, so that we still evict by larger value (larger
 * inverse frequency means to evict keys with the least frequent accesses).
 *
 * Empty entries have the key pointer set to NULL. 
 *
 * 为了提高LRU近似的质量，我们采用了一组键，这些键很适合在freeMemoryIfNeedd（）调用中驱逐。
 * 
 * 驱逐池中的条目按空闲时间排序，将更大的空闲时间放在右边（升序）。
 * 
 * 当使用LFU策略时，使用反向频率指示而不是空闲时间，因此我们仍然以更大的值驱逐（更大的反向频率意味着驱逐访问频率最低的键）。
 * 
 * 空条目的键指针设置为NULL。
 * */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) 
                                 *
                                 * 对象空闲时间（LFU的反向频率）
                                 * */
    
    sds key;                    /* Key name. 
                                 *
                                 * 键名称。
                                 * */
    
    sds cached;                 /* Cached SDS object for key name. 
                                 *
                                 * 已缓存键名称的SDS对象。
                                 * */
    
    int dbid;                   /* Key DB number. 
                                 *
                                 * 键数据库编号。
                                 * */
};

static struct evictionPoolEntry *EvictionPoolLRU;

/* ----------------------------------------------------------------------------
 * Implementation of eviction, aging and LRU 驱逐、化和LRU的实现
 * --------------------------------------------------------------------------*/

/* Return the LRU clock, based on the clock resolution. This is a time
 * in a reduced-bits format that can be used to set and check the
 * object->lru field of redisObject structures. 
 *
 * 根据时钟分辨率返回LRU时钟。这是一个缩减位格式的时间，可用于设置和检查redisObject结构的object->lru字段。
 * */
unsigned int getLRUClock(void) {
    return (mstime()/LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX;
}

/* This function is used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a system call. 
 *
 * 此功能用于获取当前LRU时钟。如果当前分辨率低于我们刷新LRU时钟的频率（在生产
 * 服务器中应该是这样），我们将返回预先计算的值，否则我们需要求助于系统调用。
 * */
unsigned int LRU_CLOCK(void) {
    unsigned int lruclock;
    if (1000/server.hz <= LRU_CLOCK_RESOLUTION) {
        lruclock = server.lruclock;
    } else {
        lruclock = getLRUClock();
    }
    return lruclock;
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. 
 *
 * 给定一个对象返回从未请求过该对象的最小毫秒数，使用近似的LRU算法。
 * */
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) *
                    LRU_CLOCK_RESOLUTION;
    }
}

/* freeMemoryIfNeeded() gets called when 'maxmemory' is set on the config
 * file to limit the max memory used by the server, before processing a
 * command.
 *
 * The goal of the function is to free enough memory to keep Redis under the
 * configured memory limit.
 *
 * The function starts calculating how many bytes should be freed to keep
 * Redis under the limit, and enters a loop selecting the best keys to
 * evict accordingly to the configured policy.
 *
 * If all the bytes needed to return back under the limit were freed the
 * function returns C_OK, otherwise C_ERR is returned, and the caller
 * should block the execution of commands that will result in more memory
 * used by the server.
 *
 * ------------------------------------------------------------------------
 *
 * LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by EVPOOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. 
 *
 * 在处理命令之前，当在配置文件上设置“maxmemory”以限制服务器使用的最大内
 * 存时，会调用freeMemoryIfNeedd（）。该函数的目标是释放足够的内存，使Redis保持在配置的内存限制之下。
 * 该函数开始计算应该释放多少字节以将Redis保持在限制之下，并进入一个循环，根据配置的策略选择要驱逐的最佳键。如果在限
 * 制下返回所需的所有字节都已释放，则函数将返回C_OK，否则将返回C_ERR，并且
 * 调用方应阻止命令的执行，这将导致服务器使用更多内存。
 * --------------------------------------------------------------------------
 * LRU近似算法Redis使用在恒定内存中运行的LRU算法的近似值。每次有一个键到期时，我们都会对N个键进行采样（其中
 * N个非常小，通常在5个左右），以填充一个最佳键池，从而收回M个键（池大小由EVPOOL_size定义）。
 * 如果采样的N个键比池中的一个当前键好，则将其添加
 * 到好键池中以过期（具有旧访问时间的键）。在填充池之后，我们在池中拥有的最佳密
 * 钥将过期。但是，请注意，删除键时，我们不会从池中删除键，因此池中可能包含不再
 * 存在的键。当我们试图收回一个键，而池中的所有条目都不存在时，我们会再次填充它
 * 。这一次，如果整个数据库中至少有一个键可以收回，我们将确保池中至少有个键可以
 * 被收回。
 * */

/* Create a new eviction pool. 
 *
 * 创建一个新的驱逐池。
 * */
void evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep)*EVPOOL_SIZE);
    for (j = 0; j < EVPOOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
        ep[j].cached = sdsnewlen(NULL,EVPOOL_CACHED_SDS_SIZE);
        ep[j].dbid = 0;
    }
    EvictionPoolLRU = ep;
}

/* This is an helper function for freeMemoryIfNeeded(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time smaller than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. 
 *
 * 这是freeMemoryIfNeedd（）的一个助手函数，用于在每次要使键过期时用几个条目填充驱逐池。
 * 将添加空闲时间小于某个当前关键帧的关键帧。如果存在空闲条目，则始终添加键。
 * 
 * 我们按升序在适当的位置插入键，所以空闲时间较小的键在左边，空闲时间较高的键在右边。
 * */

void evictionPoolPopulate(int dbid, dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    dictEntry *samples[server.maxmemory_samples];

    count = dictGetSomeKeys(sampledict,samples,server.maxmemory_samples);
    for (j = 0; j < count; j++) {
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);

        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. 
         *
         * 如果我们从中采样的字典不是主字典（而是过期的字典），我们需要在键字典中再次查找
         * 键以获得值对象。
         * */
        if (server.maxmemory_policy != MAXMEMORY_VOLATILE_TTL) {
            if (sampledict != keydict) de = dictFind(keydict, key);
            o = dictGetVal(de);
        }

        /* Calculate the idle time according to the policy. This is called
         * idle just because the code initially handled LRU, but is in fact
         * just a score where an higher score means better candidate. 
         *
         * 根据策略计算空闲时间。这被称为空闲，只是因为代码最初处理LRU，但实际上只是一个
         * 分数，分数越高意味着更好的候选者。
         * */
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LRU) {
            idle = estimateObjectIdleTime(o);
        } else if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            /* When we use an LRU policy, we sort the keys by idle time
             * so that we expire keys starting from greater idle time.
             * However when the policy is an LFU one, we have a frequency
             * estimation, and we want to evict keys with lower frequency
             * first. So inside the pool we put objects using the inverted
             * frequency subtracting the actual frequency to the maximum
             * frequency of 255. 
             *
             * 当我们使用LRU策略时，我们按空闲时间对键进行排序，以便从更长的空闲时间开始使
             * 键过期。然而，当策略是LFU策略时，我们有一个频率估计，并且我们希望首先驱逐具
             * 有较低频率的键。因此，在池中，我们使用反向频率减去实际频率的最大频率255来放
             * 置对象。
             * */
            idle = 255-LFUDecrAndReturn(o);
        } else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            /* In this case the sooner the expire the better. 
             *
             * 在这种情况下，越早过期越好。
             * */
            idle = ULLONG_MAX - (long)dictGetVal(de);
        } else {
            serverPanic("Unknown eviction policy in evictionPoolPopulate()");
        }

        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. 
         *
         * 将元素插入池中。首先，找到空闲时间小于空闲时间的第一个空存储桶或第一个填充的存储桶。
         * */
        k = 0;
        while (k < EVPOOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;
        if (k == 0 && pool[EVPOOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. 
             *
             * 如果元素<我们拥有的最差元素，并且没有空的bucket，则无法插入。
             * */
            continue;
        } else if (k < EVPOOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. 
             *
             * 插入空位置。插入前无需进行任何设置。
             * */
        } else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  
             *
             * 插入在中间。现在k指向比要插入的元素大的第一个元素。
             * */
            if (pool[EVPOOL_SIZE-1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. 
                 *
                 * 右边有空位吗？在k处插入，将所有元素从k到端向右移动。
                 * */

                /* Save SDS before overwriting. 
                 *
                 * 在覆盖之前保存SDS。
                 * */
                sds cached = pool[EVPOOL_SIZE-1].cached;
                memmove(pool+k+1,pool+k,
                    sizeof(pool[0])*(EVPOOL_SIZE-k-1));
                pool[k].cached = cached;
            } else {
                /* No free space on right? Insert at k-1 
                 *
                 * 右边没有空闲空间？在k-1处插入
                 * */
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. 
                 *
                 * 将k（包含）左边的所有元素向左移动，因此我们丢弃空闲时间较小的元素。
                 * */
                sds cached = pool[0].cached; /* Save SDS before overwriting. 
                 *
                 * 在覆盖之前保存SDS。
                 * */
                if (pool[0].key != pool[0].cached) sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
                pool[k].cached = cached;
            }
        }

        /* Try to reuse the cached SDS string allocated in the pool entry,
         * because allocating and deallocating this object is costly
         * (according to the profiler, not my fantasy. Remember:
         * premature optimization bla bla bla. 
         *
         * 尝试重用池条目中分配的缓存SDS字符串，因为分配和释放此对象的成本很高（根据探查
         * 器的说法，不是我的幻想。记住：过早优化blablabla。
         * */
        int klen = sdslen(key);
        if (klen > EVPOOL_CACHED_SDS_SIZE) {
            pool[k].key = sdsdup(key);
        } else {
            memcpy(pool[k].cached,key,klen+1);
            sdssetlen(pool[k].cached,klen);
            pool[k].key = pool[k].cached;
        }
        pool[k].idle = idle;
        pool[k].dbid = dbid;
    }
}

/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) implementation.

 * We have 24 total bits of space in each object in order to implement
 * an LFU (Least Frequently Used) eviction policy, since we re-use the
 * LRU field for this purpose.
 *
 * We split the 24 bits into two fields:
 *
 *          16 bits      8 bits
 *     +----------------+--------+
 *     + Last decr time | LOG_C  |
 *     +----------------+--------+
 *
 * LOG_C is a logarithmic counter that provides an indication of the access
 * frequency. However this field must also be decremented otherwise what used
 * to be a frequently accessed key in the past, will remain ranked like that
 * forever, while we want the algorithm to adapt to access pattern changes.
 *
 * So the remaining 16 bits are used in order to store the "decrement time",
 * a reduced-precision Unix time (we take 16 bits of the time converted
 * in minutes since we don't care about wrapping around) where the LOG_C
 * counter is halved if it has an high value, or just decremented if it
 * has a low value.
 *
 * New keys don't start at zero, in order to have the ability to collect
 * some accesses before being trashed away, so they start at COUNTER_INIT_VAL.
 * The logarithmic increment performed on LOG_C takes care of COUNTER_INIT_VAL
 * when incrementing the key, so that keys starting at COUNTER_INIT_VAL
 * (or having a smaller value) have a very high chance of being incremented
 * on access.
 *
 * During decrement, the value of the logarithmic counter is halved if
 * its current value is greater than two times the COUNTER_INIT_VAL, otherwise
 * it is just decremented by one.
 * --------------------------------------------------------------------------*/

/* Return the current time in minutes, just taking the least significant
 * 16 bits. The returned time is suitable to be stored as LDT (last decrement
 * time) for the LFU implementation. 
 *
 * 以分钟为单位返回当前时间，只取最低有效的16位。返回的时间适合存储为LFU实现的
 * LDT（最后递减时间）。
 * */
unsigned long LFUGetTimeInMinutes(void) {
    return (server.unixtime/60) & 65535;
}

/* Given an object last access time, compute the minimum number of minutes
 * that elapsed since the last access. Handle overflow (ldt greater than
 * the current 16 bits minutes time) considering the time as wrapping
 * exactly once. 
 *
 * 给定对象上次访问时间，计算自上次访问以来经过的最小分钟数。处理溢出（ldt大于当
 * 前的16位分钟时间），将时间视为正好包装一次。
 * */
unsigned long LFUTimeElapsed(unsigned long ldt) {
    unsigned long now = LFUGetTimeInMinutes();
    if (now >= ldt) return now-ldt;
    return 65535-ldt+now;
}

/* Logarithmically increment a counter. The greater is the current counter value
 * the less likely is that it gets really implemented. Saturate it at 255. 
 *
 * 以对数方式递增计数器。当前计数器值越大，它真正实现的可能性就越小。在255饱和。
 * */
uint8_t LFULogIncr(uint8_t counter) {
    if (counter == 255) return 255;
    double r = (double)rand()/RAND_MAX;
    double baseval = counter - LFU_INIT_VAL;
    if (baseval < 0) baseval = 0;
    double p = 1.0/(baseval*server.lfu_log_factor+1);
    if (r < p) counter++;
    return counter;
}

/* If the object decrement time is reached decrement the LFU counter but
 * do not update LFU fields of the object, we update the access time
 * and counter in an explicit way when the object is really accessed.
 * And we will times halve the counter according to the times of
 * elapsed time than server.lfu_decay_time.
 * Return the object frequency counter.
 *
 * This function is used in order to scan the dataset for the best object
 * to fit: as we check for the candidate, we incrementally decrement the
 * counter of the scanned objects if needed. 
 *
 * 如果达到对象递减时间，则递减LFU计数器，但不更新对象的LFU字段，当真正访问对
 * 象时，我们以显式方式更新访问时间和计数器。我们会根据经过的时间将计数器减半，而不
 * 是server.lfu_decay_time。返回对象频率计数器。
 *
 * 该函数用于扫描数据集以寻找最适合的对象：当我们检查候选对象时，如果需要，我们会逐渐递减扫描对象的计数器。
 * */
unsigned long LFUDecrAndReturn(robj *o) {
    unsigned long ldt = o->lru >> 8;
    unsigned long counter = o->lru & 255;
    unsigned long num_periods = server.lfu_decay_time ? LFUTimeElapsed(ldt) / server.lfu_decay_time : 0;
    if (num_periods)
        counter = (num_periods > counter) ? 0 : counter - num_periods;
    return counter;
}

/* ----------------------------------------------------------------------------
 * The external API for eviction: freeMemoryIfNeeded() is called by the
 * server when there is data to add in order to make space if needed.
 *
 * 用于逐出的外部API：freeMemoryIfNeeded（）由服务器在需要时添加数据以腾出空间时调用
 * -------------------------------------------------------------------------- */

/* We don't want to count AOF buffers and slaves output buffers as
 * used memory: the eviction should use mostly data size. This function
 * returns the sum of AOF and slaves buffer. 
 *
 * 我们不想将AOF缓冲区和从输出缓冲区算作已用内存：驱逐应该主要使用数据大小。此函
 * 数返回AOF和从属缓冲区的总和。
 * */
size_t freeMemoryGetNotCountedMemory(void) {
    size_t overhead = 0;
    int slaves = listLength(server.slaves);

    if (slaves) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = listNodeValue(ln);
            overhead += getClientOutputBufferMemoryUsage(slave);
        }
    }
    if (server.aof_state != AOF_OFF) {
        overhead += sdsalloc(server.aof_buf)+aofRewriteBufferSize();
    }
    return overhead;
}

/* Get the memory status from the point of view of the maxmemory directive:
 * if the memory used is under the maxmemory setting then C_OK is returned.
 * Otherwise, if we are over the memory limit, the function returns
 * C_ERR.
 *
 * The function may return additional info via reference, only if the
 * pointers to the respective arguments is not NULL. Certain fields are
 * populated only when C_ERR is returned:
 *
 *  'total'     total amount of bytes used.
 *              (Populated both for C_ERR and C_OK)
 *
 *  'logical'   the amount of memory used minus the slaves/AOF buffers.
 *              (Populated when C_ERR is returned)
 *
 *  'tofree'    the amount of memory that should be released
 *              in order to return back into the memory limits.
 *              (Populated when C_ERR is returned)
 *
 *  'level'     this usually ranges from 0 to 1, and reports the amount of
 *              memory currently used. May be > 1 if we are over the memory
 *              limit.
 *              (Populated both for C_ERR and C_OK)
 
 *
 * 从maxmemory指令的角度获取内存状态：如果使用的内存在maxmemory设
 * 置下，则返回C_OK。否则，如果超过内存限制，函数将返回C_ERR。
 *
 * 只有当指向相应参数的指针不为NULL时，函数才能通过引用返回附加信息。
 * 只有在返回C_ERR时才会填充某些字段：
 *
 * “total”  使用的字节总数（为C_ERR和C_OK填充）
 *
 * “logical” 所用内存量减去从属/AOF缓冲区。（返回C_ERR时填充）
 *
 * “tofree” 应释放的内存量，以便返回内存限制。（返回C_ERR时填充）
 *
 * “level” 通常范围为0到1，并报告当前使用的内存量。如果我们超过内存限制，则可能大于1。（为C_ERR和C_OK填充）
 * */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level) {
    size_t mem_reported, mem_used, mem_tofree;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. 
     *
     * 检查我们是否超过了内存使用限制。如果不是，则无需减去从属输出缓冲区。我们可以尽快
     * 回来。
     * */
    mem_reported = zmalloc_used_memory();
    if (total) *total = mem_reported;

    /* We may return ASAP if there is no need to compute the level. 
     *
     * 如果不需要计算液位，我们可能会尽快返回。
     * */
    int return_ok_asap = !server.maxmemory || mem_reported <= server.maxmemory;
    if (return_ok_asap && !level) return C_OK;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. 
     *
     * 从已用内存的计数中删除从属输出缓冲区和AOF缓冲区的大小。
     * */
    mem_used = mem_reported;
    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used-overhead : 0;

    /* Compute the ratio of memory usage. 
     *
     * 计算内存使用率。
     * */
    if (level) {
        if (!server.maxmemory) {
            *level = 0;
        } else {
            *level = (float)mem_used / (float)server.maxmemory;
        }
    }

    if (return_ok_asap) return C_OK;

    /* Check if we are still over the memory limit. 
     *
     * 检查我们是否仍超过内存限制。
     * */
    if (mem_used <= server.maxmemory) return C_OK;

    /* Compute how much memory we need to free. 
     *
     * 计算我们需要释放多少内存。
     * */
    mem_tofree = mem_used - server.maxmemory;

    if (logical) *logical = mem_used;
    if (tofree) *tofree = mem_tofree;

    return C_ERR;
}

/* This function is periodically called to see if there is memory to free
 * according to the current "maxmemory" settings. In case we are over the
 * memory limit, the function will try to free some memory to return back
 * under the limit.
 *
 * The function returns C_OK if we are under the memory limit or if we
 * were over the limit, but the attempt to free memory was successful.
 * Otherwise if we are over the memory limit, but not enough memory
 * was freed to return back under the limit, the function returns C_ERR.
 *
 * 定期调用此函数，以查看是否有内存根据当前的“maxmemory”设置释放。
 * 如果我们超过内存限制，该函数将尝试释放一些内存以返回到限制以下。
 *
 * 如果我们低于内存限制或超过限制，但释放内存的尝试成功，该函数将返回C_OK。
 * 否则，如果我们超过了内存限制，但释放的内存不足，无法返回到限制以下，则该函数将返回C_ERR。
 *
 * */
int freeMemoryIfNeeded(void) {
    int keys_freed = 0;
    /* By default replicas should ignore maxmemory
     * and just be masters exact copies.
     *
     * 默认情况下，副本应忽略最大内存，而只是主副本的精确副本。
     * */
    if (server.masterhost && server.repl_slave_ignore_maxmemory) return C_OK;

    size_t mem_reported, mem_tofree, mem_freed;
    mstime_t latency, eviction_latency, lazyfree_latency;
    long long delta;
    int slaves = listLength(server.slaves);
    int result = C_ERR;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed.
     *
     * 当客户端暂停时，数据集应该是静态的，不仅来自客户端无法写入的 POV，还来自过期和未执行的键逐出的 POV。
     * */
    if (clientsArePaused()) return C_OK;
    if (getMaxmemoryState(&mem_reported,NULL,&mem_tofree,NULL) == C_OK)
        return C_OK;

    mem_freed = 0;

    latencyStartMonitor(latency);
    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION)
        goto cant_free; /* We need to free memory, but policy forbids. 
                         *
                         * 我们需要释放内存，但政策禁止。
                         * */

    while (mem_freed < mem_tofree) {
        int j, k, i;
        static unsigned int next_db = 0;
        sds bestkey = NULL;
        int bestdbid;
        redisDb *db;
        dict *dict;
        dictEntry *de;

        if (server.maxmemory_policy & (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU) ||
            server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL)
        {
            struct evictionPoolEntry *pool = EvictionPoolLRU;

            while(bestkey == NULL) {
                unsigned long total_keys = 0, keys;

                /* We don't want to make local-db choices when expiring keys,
                 * so to start populate the eviction pool sampling keys from
                 * every DB.
                 *
                 * 我们不希望在过期键时做出本地数据库选择，因此要开始从每个数据库填充逐出池采样键。
                 *
                 * */
                for (i = 0; i < server.dbnum; i++) {
                    db = server.db+i;
                    dict = (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) ?
                            db->dict : db->expires;
                    if ((keys = dictSize(dict)) != 0) {
                        evictionPoolPopulate(i, dict, db->dict, pool);
                        total_keys += keys;
                    }
                }
                if (!total_keys) break; /* No keys to evict. 没有数据可以淘汰*/

                /* Go backward from best to worst element to evict. 
                 *
                 * 从最好的元素到最坏的元素倒过来驱逐。
                 * */
                for (k = EVPOOL_SIZE-1; k >= 0; k--) {
                    if (pool[k].key == NULL) continue;
                    bestdbid = pool[k].dbid;

                    if (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) {
                        de = dictFind(server.db[pool[k].dbid].dict,
                            pool[k].key);
                    } else {
                        de = dictFind(server.db[pool[k].dbid].expires,
                            pool[k].key);
                    }

                    /* Remove the entry from the pool. 
                     *
                     * 从池中删除条目。
                     * */
                    if (pool[k].key != pool[k].cached)
                        sdsfree(pool[k].key);
                    pool[k].key = NULL;
                    pool[k].idle = 0;

                    /* If the key exists, is our pick. Otherwise it is
                     * a ghost and we need to try the next element.
                     *
                     *
                     * 如果键存在，是我们的选择。否则它是一个幽灵，我们需要尝试下一个元素。
                     * */
                    if (de) {
                        bestkey = dictGetKey(de);
                        break;
                    } else {
                        /* Ghost... Iterate again. 
                         *
                         *  幽灵  再次迭代。
                         * */
                    }
                }
            }
        }

        /* volatile-random and allkeys-random policy 
         *
         * volatile-random 和 allkeys-random 策略
         * */
        else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
                 server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
        {
            /* When evicting a random key, we try to evict a key for
             * each DB, so we use the static 'next_db' variable to
             * incrementally visit all DBs. 
             *
             * 当驱逐一个随机键时，我们试图为每个DB驱逐一个键，因此我们使用静态的“next_db”变量来增量访问所有DB。
             * */
            for (i = 0; i < server.dbnum; i++) {
                j = (++next_db) % server.dbnum;
                db = server.db+j;
                dict = (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ?
                        db->dict : db->expires;
                if (dictSize(dict) != 0) {
                    de = dictGetRandomKey(dict);
                    bestkey = dictGetKey(de);
                    bestdbid = j;
                    break;
                }
            }
        }

        /* Finally remove the selected key. 
         *
         * 最后移除选定的键。
         * */
        if (bestkey) {
            db = server.db+bestdbid;
            robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
            propagateExpire(db,keyobj,server.lazyfree_lazy_eviction);
            /* We compute the amount of memory freed by db*Delete() alone.
             * It is possible that actually the memory needed to propagate
             * the DEL in AOF and replication link is greater than the one
             * we are freeing removing the key, but we can't account for
             * that otherwise we would never exit the loop.
             *
             * Same for CSC invalidation messages generated by signalModifiedKey.
             *
             * AOF and Output buffer memory will be freed eventually so
             * we only care about memory used by the key space. 
             *
             * 我们单独计算db*Delete（）释放的内存量。实际上，在AOF和复制链接中传播
             * DEL所需的内存可能大于我们删除键时释放的内存，但我们无法解释这一点，否则我们
             * 永远不会退出循环。
             *
             * signalModifiedKey生成的CSC无效消息也是如此。
             *
             * AOF和Output缓冲区内存最终将被释放，所以我们只关心键空间使用的内存。
             * */
            delta = (long long) zmalloc_used_memory();
            latencyStartMonitor(eviction_latency);
            if (server.lazyfree_lazy_eviction)
                dbAsyncDelete(db,keyobj);
            else
                dbSyncDelete(db,keyobj);
            latencyEndMonitor(eviction_latency);
            latencyAddSampleIfNeeded("eviction-del",eviction_latency);
            delta -= (long long) zmalloc_used_memory();
            mem_freed += delta;
            server.stat_evictedkeys++;
            signalModifiedKey(NULL,db,keyobj);
            notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted",
                keyobj, db->id);
            decrRefCount(keyobj);
            keys_freed++;

            /* When the memory to free starts to be big enough, we may
             * start spending so much time here that is impossible to
             * deliver data to the slaves fast enough, so we force the
             * transmission here inside the loop. 
             *
             * 当要释放的内存开始足够大时，我们可能会开始在这里花费太多时间，以至于无法足够快地
             * 将数据传输到从属设备，所以我们强制在环路内进行传输。
             * */
            if (slaves) flushSlavesOutputBuffers();

            /* Normally our stop condition is the ability to release
             * a fixed, pre-computed amount of memory. However when we
             * are deleting objects in another thread, it's better to
             * check, from time to time, if we already reached our target
             * memory, since the "mem_freed" amount is computed only
             * across the dbAsyncDelete() call, while the thread can
             * release the memory all the time. 
             *
             * 通常，我们的停止条件是能够释放固定的、预先计算的内存量。然而，当我们在另一个线程
             * 中删除对象时，最好不时检查我们是否已经到达目标内存，因为“mem_freed”量
             * 仅通过dbAsyncDelete（）调用计算，而线程可以一直释放内存。
             * */
            if (server.lazyfree_lazy_eviction && !(keys_freed % 16)) {
                if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                    /* Let's satisfy our stop condition. 
                     *
                     * 让我们满足停止条件。
                     * */
                    mem_freed = mem_tofree;
                }
            }
        } else {
            goto cant_free; /* nothing to free... 
                             *
                             * 没有什么可以释放的。。。
                             * */
        }
    }
    result = C_OK;

cant_free:
    /* We are here if we are not able to reclaim memory. There is only one
     * last thing we can try: check if the lazyfree thread has jobs in queue
     * and wait... 
     *
     * 如果我们不能恢复记忆，我们就在这里。我们只能尝试最后一件事：检查lazyfree
     * 线程是否有作业在队列中并等待。。。
     * */
    if (result != C_OK) {
        latencyStartMonitor(lazyfree_latency);
        while(bioPendingJobsOfType(BIO_LAZY_FREE)) {
            if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                result = C_OK;
                break;
            }
            usleep(1000);
        }
        latencyEndMonitor(lazyfree_latency);
        latencyAddSampleIfNeeded("eviction-lazyfree",lazyfree_latency);
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("eviction-cycle",latency);
    return result;
}

/* This is a wrapper for freeMemoryIfNeeded() that only really calls the
 * function if right now there are the conditions to do so safely:
 *
 * - There must be no script in timeout condition.
 * - Nor we are loading data right now.
 *
 *
 * 这是freeMemoryIfNeedd（）的包装器，只有在目前有安全条件的情况下才能真正调用函数：
 *  - 在超时条件下不能有脚本。
 *  - 我们现在也没有加载数据。
 * */
int freeMemoryIfNeededAndSafe(void) {
    if (server.lua_timedout || server.loading) return C_OK;
    return freeMemoryIfNeeded();
}
