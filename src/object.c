/* Redis Object implementation.
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
#include <math.h>
#include <ctype.h>

#ifdef __CYGWIN__
#define strtold(a,b) ((long double)strtod((a),(b)))
#endif

/* ===================== Creation and parsing of objects ====================
 *                       对象的创建和解析
 * */

/*
 * 根据给定类型和值，创建新对象
 */
robj *createObject(int type, void *ptr) {
    // 分配空间
    robj *o = zmalloc(sizeof(*o));
    // 初始化对象域
    o->type = type;
    // 默认编码
    o->encoding = OBJ_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;

    /* Set the LRU to the current lruclock (minutes resolution), or
     * alternatively the LFU counter. 
     *
     * 将 LRU 设置为当前 lruclock（分钟分辨率），或者设置为LFU计数器。
     * */
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();
    }
    return o;
}

/* Set a special refcount in the object to make it "shared":
 * incrRefCount and decrRefCount() will test for this special refcount
 * and will not touch the object. This way it is free to access shared
 * objects such as small integers from different threads without any
 * mutex.
 *
 * A common patter to create shared objects:
 *
 * robj *myobject = makeObjectShared(createObject(...));
 *
 
 *
 * 在对象中设置一个特殊的refcount以使其“共享”：incrRefCount和
 * decoRefCount（）将测试此特殊的refcount，并且不会接触对象。这
 * 样就可以自由地访问共享对象，例如来自不同线程的小整数，而不需要任何互斥。
 *
 * 创建共享对象的常用模式：
 * robj * myobject = makeObjectShared（createObject（…））；
 * */
robj *makeObjectShared(robj *o) {
    serverAssert(o->refcount == 1);
    o->refcount = OBJ_SHARED_REFCOUNT;
    return o;
}

/* Create a string object with encoding OBJ_ENCODING_RAW, that is a plain
 * string object where o->ptr points to a proper sds string. 
 *
 * 创建一个编码为OBJ_encoding_RAW的字符串对象，这是一个普通字符串对
 * 象，其中o->ptr指向一个正确的sds字符串。
 * */
/*
 * 根据给定字符数组，创建一个 String 对象
 */
robj *createRawStringObject(const char *ptr, size_t len) {
    return createObject(OBJ_STRING, sdsnewlen(ptr,len));
}

/* Create a string object with encoding OBJ_ENCODING_EMBSTR, that is
 * an object where the sds string is actually an unmodifiable string
 * allocated in the same chunk as the object itself. 
 *
 * 创建一个编码为OBJ_encoding_EMBSTR的字符串对象，该对象的sds
 * 字符串实际上是一个不可修改的字符串，分配在与对象本身相同的块中。
 * */
robj *createEmbeddedStringObject(const char *ptr, size_t len) {
    robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr8)+len+1);
    struct sdshdr8 *sh = (void*)(o+1);

    o->type = OBJ_STRING;
    o->encoding = OBJ_ENCODING_EMBSTR;
    o->ptr = sh+1;
    o->refcount = 1;
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();
    }

    sh->len = len;
    sh->alloc = len;
    sh->flags = SDS_TYPE_8;
    if (ptr == SDS_NOINIT)
        sh->buf[len] = '\0';
    else if (ptr) {
        memcpy(sh->buf,ptr,len);
        sh->buf[len] = '\0';
    } else {
        memset(sh->buf,0,len+1);
    }
    return o;
}

/* Create a string object with EMBSTR encoding if it is smaller than
 * OBJ_ENCODING_EMBSTR_SIZE_LIMIT, otherwise the RAW encoding is
 * used.
 *
 * The current limit of 44 is chosen so that the biggest string object
 * we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc. 
 *
 * 如果EMBSTR编码小于OBJ_encoding_EMBSTR_SIZE_LIMIT，则创建具有EMBSTR编码的字符串对象，否则使用RAW编码。
 * 选择44的当前限制是为了使我们分配为EMBSTR的最大字符串对象仍然适合 jemalloc [hhc] 的64字节区域。
 * */
#define OBJ_ENCODING_EMBSTR_SIZE_LIMIT 44
robj *createStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}

/* Create a string object from a long long value. When possible returns a
 * shared integer object, or at least an integer encoded one.
 *
 * If valueobj is non zero, the function avoids returning a shared
 * integer, because the object is going to be used as value in the Redis key
 * space (for instance when the INCR command is used), so we want LFU/LRU
 * values specific for each key. 
 *
 * 从长-长值创建字符串对象。如果可能，返回一个共享整数对象，或者至少返回一个整数编
 * 码对象。
 *
 * 如果valueobj不为零，函数将避免返回共享整数，因为该对象将用作Redis键空间中的值
 * （例如，当使用INCR命令时），因此我们希望LFU/LRU值特定于每个键。
 * */
// valueobj 指定是否使用共享的对象
robj *createStringObjectFromLongLongWithOptions(long long value, int valueobj) {
    robj *o;

    if (server.maxmemory == 0 ||
        !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS))
    {
        /* If the maxmemory policy permits, we can still return shared integers
         * even if valueobj is true. 
         *
         * 如果maxmemory策略允许，即使valueobj为true，我们仍然可以返回共享整数。
         * */
        valueobj = 0;
    }

    // 10000 一万以内的数据做共享
    if (value >= 0 && value < OBJ_SHARED_INTEGERS && valueobj == 0) {
        // 如果条件允许，使用共享对象[hhc]
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else {
        // 否则，创建新 String 对象
        if (value >= LONG_MIN && value <= LONG_MAX) {
            // long 类型的数字值以 long 类型保存
            o = createObject(OBJ_STRING, NULL);
            // 设置编码
            o->encoding = OBJ_ENCODING_INT;
            o->ptr = (void*)((long)value);
        } else {
            // long long 类型的数字值编码成字符串来保存
            o = createObject(OBJ_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}

/* Wrapper for createStringObjectFromLongLongWithOptions() always demanding
 * to create a shared object if possible. 
 *
 * createStringObjectFromLongLongWithOption
 * s（）的包装程序总是要求在可能的情况下创建共享对象。
 * */
/*
 * 根据给定数字值 value ，创建一个 String 对象
 */
robj *createStringObjectFromLongLong(long long value) {
    return createStringObjectFromLongLongWithOptions(value,0);
}

/* Wrapper for createStringObjectFromLongLongWithOptions() avoiding a shared
 * object when LFU/LRU info are needed, that is, when the object is used
 * as a value in the key space, and Redis is configured to evict based on
 * LFU/LRU. 
 *
 * createStringObjectFromLongLongWithOptions（）的Wrapper在需要LFU/LRU信息时，
 * 即当对象用作键空间中的值时，避免共享对象，Redis配置为基于LFU/LRU逐出。
 * */
robj *createStringObjectFromLongLongForValue(long long value) {
    return createStringObjectFromLongLongWithOptions(value,1);
}

/* Create a string object from a long double. If humanfriendly is non-zero
 * it does not use exponential format and trims trailing zeroes at the end,
 * however this results in loss of precision. Otherwise exp format is used
 * and the output of snprintf() is not modified.
 *
 * The 'humanfriendly' option is used for INCRBYFLOAT and HINCRBYFLOAT. 
 *
 * 从长双精度创建字符串对象。如果人性化为非零，则不使用指数格式，并在末尾修剪尾随的
 * 零，但这会导致精度损失。否则将使用exp格式，并且不会修改snprintf（）的
 * 输出。
 *
 * INCRBYFLOAT和HINCRBYFLOAT使用“人性化”选项。
 * */
/*
 * 根据给定 long double 值 value ，创建 String 对象
 */
robj *createStringObjectFromLongDouble(long double value, int humanfriendly) {
    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf,sizeof(buf),value,humanfriendly? LD_STR_HUMAN: LD_STR_AUTO);
    return createStringObject(buf,len);
}

/* Duplicate a string object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * This function also guarantees that duplicating a small integer object
 * (or a string object that contains a representation of a small integer)
 * will always result in a fresh object that is unshared (refcount == 1).
 *
 * The resulting object always has refcount set to 1. 
 *
 * 复制字符串对象，并确保返回的对象与原始对象具有相同的编码。
 *
 * 此函数还保证复制一个小整数对象（或包含小整数表示的字符串对象）将始终产生一个未共享的新对象（refcount==1）。
 *
 * 生成的对象总是将refcount设置为1。
 * */
/*
 * 复制一个 String 对象的副本
 */
robj *dupStringObject(const robj *o) {
    robj *d;

    serverAssert(o->type == OBJ_STRING);

    switch(o->encoding) {
    case OBJ_ENCODING_RAW:
        return createRawStringObject(o->ptr,sdslen(o->ptr));
    case OBJ_ENCODING_EMBSTR:
        return createEmbeddedStringObject(o->ptr,sdslen(o->ptr));
    case OBJ_ENCODING_INT:
        d = createObject(OBJ_STRING, NULL);
        d->encoding = OBJ_ENCODING_INT;
        d->ptr = o->ptr;
        return d;
    default:
        serverPanic("Wrong encoding.");
        break;
    }
}

/*
 * 创建一个 list 对象
 */
robj *createQuicklistObject(void) {
    quicklist *l = quicklistCreate();
    robj *o = createObject(OBJ_LIST,l);
    o->encoding = OBJ_ENCODING_QUICKLIST;
    return o;
}

/*
 * 创建一个 ziplist 对象
 */
robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(OBJ_LIST,zl);
    o->encoding = OBJ_ENCODING_ZIPLIST;
    return o;
}

/*
 * 创建一个 set 对象
 */
robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);
    robj *o = createObject(OBJ_SET,d);
    o->encoding = OBJ_ENCODING_HT;
    return o;
}

/*
 * 创建一个 intset 对象
 */
robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(OBJ_SET,is);
    o->encoding = OBJ_ENCODING_INTSET;
    return o;
}

/*
 * 创建一个 hash 对象
 */
robj *createHashObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(OBJ_HASH, zl);
    o->encoding = OBJ_ENCODING_ZIPLIST;
    return o;
}

/*
 * 创建一个 zset 对象
 */
robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    // zset 使用 dict 和 skiplist 两个数据结构
    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();
    o = createObject(OBJ_ZSET,zs);
    o->encoding = OBJ_ENCODING_SKIPLIST;
    return o;
}

/*
 * 创建一个 ziplist 表示的 zset 对象
 */
robj *createZsetZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(OBJ_ZSET,zl);
    o->encoding = OBJ_ENCODING_ZIPLIST;
    return o;
}

robj *createStreamObject(void) {
    stream *s = streamNew();
    robj *o = createObject(OBJ_STREAM,s);
    o->encoding = OBJ_ENCODING_STREAM;
    return o;
}

robj *createModuleObject(moduleType *mt, void *value) {
    moduleValue *mv = zmalloc(sizeof(*mv));
    mv->type = mt;
    mv->value = value;
    return createObject(OBJ_MODULE,mv);
}

/*
 * 释放 string 对象
 */
void freeStringObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

/*
 * 释放 list 对象
 */
void freeListObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistRelease(o->ptr);
    } else {
        serverPanic("Unknown list encoding type");
    }
}

/*
 * 释放 set 对象
 */
void freeSetObject(robj *o) {
    switch (o->encoding) {
    // hash 表示
    case OBJ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    // intset 表示
    case OBJ_ENCODING_INTSET:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown set encoding type");
    }
}

/*
 *  释放 zset 对象
 */
void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
    // skiplist 表示
    case OBJ_ENCODING_SKIPLIST:
        zs = o->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
        break;
    // ziplist 表示
    case OBJ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown sorted set encoding");
    }
}

/*
 * 释放 hash 对象
 */
void freeHashObject(robj *o) {
    switch (o->encoding) {
    // hash 表示
    case OBJ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    // ziplist 表示
    case OBJ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown hash encoding type");
        break;
    }
}

void freeModuleObject(robj *o) {
    moduleValue *mv = o->ptr;
    mv->type->free(mv->value);
    zfree(mv);
}

void freeStreamObject(robj *o) {
    freeStream(o->ptr);
}

/*
 * 增加对象的引用计数
 */
void incrRefCount(robj *o) {
    if (o->refcount < OBJ_FIRST_SPECIAL_REFCOUNT) {
        o->refcount++;
    } else {
        if (o->refcount == OBJ_SHARED_REFCOUNT) {
            /* Nothing to do: this refcount is immutable. 
             *
             * 无所事事：这个refcount是不可变的。
             * */
        } else if (o->refcount == OBJ_STATIC_REFCOUNT) {
            serverPanic("You tried to retain an object allocated in the stack");
        }
    }
}

/*
 * 减少对象的引用计数
 *
 * 当对象的引用计数降为 0 时，释放这个对象
 */
void decrRefCount(robj *o) {
    if (o->refcount == 1) {
        switch(o->type) {
            // 如果引用数降为 0
            // 根据对象类型，调用相应的对象释放函数来释放对象的值
        case OBJ_STRING: freeStringObject(o); break;
        case OBJ_LIST: freeListObject(o); break;
        case OBJ_SET: freeSetObject(o); break;
        case OBJ_ZSET: freeZsetObject(o); break;
        case OBJ_HASH: freeHashObject(o); break;
        case OBJ_MODULE: freeModuleObject(o); break;
        case OBJ_STREAM: freeStreamObject(o); break;
        default: serverPanic("Unknown object type"); break;
        }
        // 释放对象本身
        zfree(o);
    } else {
        // 否则，只降低引用数
        if (o->refcount <= 0) serverPanic("decrRefCount against refcount <= 0");
        if (o->refcount != OBJ_SHARED_REFCOUNT) o->refcount--;
    }
}

/* This variant of decrRefCount() gets its argument as void, and is useful
 * as free method in data structures that expect a 'void free_object(void*)'
 * prototype for the free method. 
 *
 * decoRefCount（）的这个变体将其参数设置为void，并且在期望“void free_object（void*）”
 * 原型作为自由方法的数据结构中，它非常有用。
 * */
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

/* This function set the ref count to zero without freeing the object.
 * It is useful in order to pass a new object to functions incrementing
 * the ref count of the received object. Example:
 *
 *    functionThatWillIncrementRefCount(resetRefCount(CreateObject(...)));
 *
 * Otherwise you need to resort to the less elegant pattern:
 *
 *    *obj = createObject(...);
 *    functionThatWillIncrementRefCount(obj);
 *    decrRefCount(obj);
 
 *
 * 此函数在不释放对象的情况下将引用计数设置为零。将新对象传递给递增接收对象的ref
 * 计数的函数是有用的。示例：
 *
 *    functionThatWillIncrementRefCount（resetRefCount（CreateObject（…）））；
 *
 *   否则，您需要使用不那么优雅的模式：
 *
 *    *obj = createObject(...);
 *    functionThatWillIncrementRefCount(obj);
 *    decrRefCount(obj);
 * */
/*
 * 将对象的引用数设置为 0 ，但不释放该对象
 */
robj *resetRefCount(robj *obj) {
    obj->refcount = 0;
    return obj;
}

/*
 * 检查给定对象 o 的类型是否为给定类型 type
 *
 * 类型不相同时返回 1 ，并向客户端报告类型错误。
 * 类型相同时返回 0 。
 */
int checkType(client *c, robj *o, int type) {
    if (o->type != type) {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

int isSdsRepresentableAsLongLong(sds s, long long *llval) {
    return string2ll(s,sdslen(s),llval) ? C_OK : C_ERR;
}

/*
 * 检查给定的 string 对象能否表示为 long long 类型值
 */
int isObjectRepresentableAsLongLong(robj *o, long long *llval) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    if (o->encoding == OBJ_ENCODING_INT) {
        if (llval) *llval = (long) o->ptr;
        return C_OK;
    } else {
        return isSdsRepresentableAsLongLong(o->ptr,llval);
    }
}

/* Optimize the SDS string inside the string object to require little space,
 * in case there is more than 10% of free space at the end of the SDS
 * string. This happens because SDS strings tend to overallocate to avoid
 * wasting too much time in allocations when appending to the string. 
 *
 * 如果SDS字符串末尾的可用空间超过10%，请优化字符串对象内的SDS字符串，使其
 * 只需要很少的空间。发生这种情况是因为SDS字符串在附加到字符串时往往会过度分配，
 * 以避免在分配中浪费太多时间。
 * */
void trimStringObjectIfNeeded(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW &&
        sdsavail(o->ptr) > sdslen(o->ptr)/10)
    {
        o->ptr = sdsRemoveFreeSpace(o->ptr);
    }
}

/* Try to encode a string object in order to save space 
 *
 * 尝试对字符串对象进行编码以节省空间
 * */
/*
 * 尝试将对象 o 编码成整数，并尝试将它加入到共享对象里面
 */
robj *tryObjectEncoding(robj *o) {
    long value;
    sds s = o->ptr;
    size_t len;

    /* Make sure this is a string object, the only type we encode
     * in this function. Other types use encoded memory efficient
     * representations but are handled by the commands implementing
     * the type. 
     *
     * 请确保这是一个字符串对象，是我们在此函数中编码的唯一类型。其他类型使用编码的高效
     * 内存表示，但由实现该类型的命令来处理。
     * */
    // 只尝试对 string 对象进行编码
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

    /* We try some specialized encoding only for objects that are
     * RAW or EMBSTR encoded, in other words objects that are still
     * in represented by an actually array of chars. 
     *
     * 我们只对RAW或EMBSTR编码的对象尝试一些专门的编码，换句话说，仍然由实际的
     * 字符数组表示的对象。
     * */
    if (!sdsEncodedObject(o)) return o;

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Redis and may end in places where
     * they are not handled. We handle them only as values in the keyspace. 
     *
     * 对共享对象进行编码是不安全的：共享对象可以在Redis的“对象空间”中的任何地方
     * 共享，并且可能会在没有处理它们的地方结束。我们只将它们作为键空间中的值来处理。
     * */
    // 不对共享对象进行编码
     if (o->refcount > 1) return o;

    /* Check if we can represent this string as a long integer.
     * Note that we are sure that a string larger than 20 chars is not
     * representable as a 32 nor 64 bit integer. 
     *
     * 检查我们是否可以将此字符串表示为长整数。请注意，我们确信大于20个字符的字符串不
     * 能表示为32或64位整数。
     * */
    len = sdslen(s);
    // 尝试将字符串值转换为 long 整数
    // 转换失败直接返回 o ，转换成功时继续执行
    if (len <= 20 && string2l(s,len,&value)) {
        /* This object is encodable as a long. Try to use a shared object.
         * Note that we avoid using shared integers when maxmemory is used
         * because every object needs to have a private LRU field for the LRU
         * algorithm to work well. 
         *
         * 此对象可编码为长。尝试使用共享对象。请注意，在使用maxmemory时，我们避免
         * 使用共享整数，因为每个对象都需要有一个专用LRU字段，LRU算法才能正常工作。
         * */
        // 执行到这一行时， value 保存的已经是一个 long 整数了
        if ((server.maxmemory == 0 ||
            !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) &&
            value >= 0 &&
            value < OBJ_SHARED_INTEGERS)
        {
            // 看看 value 是否属于可共享值的范围
            // 如果是的话就用共享对象代替这个对象 o
            decrRefCount(o);
            incrRefCount(shared.integers[value]);
            // 将共享对象返回
            return shared.integers[value];
        } else {
            if (o->encoding == OBJ_ENCODING_RAW) {
                sdsfree(o->ptr);
                // value 不属于共享范围，将它保存到对象 o 中
                // 更新编码方式
                o->encoding = OBJ_ENCODING_INT;
                // 释放旧值
                o->ptr = (void*) value;
                return o;
            } else if (o->encoding == OBJ_ENCODING_EMBSTR) {
                decrRefCount(o);
                return createStringObjectFromLongLongForValue(value);
            }
        }
    }

    /* If the string is small and is still RAW encoded,
     * try the EMBSTR encoding which is more efficient.
     * In this representation the object and the SDS string are allocated
     * in the same chunk of memory to save space and cache misses. 
     *
     * 如果字符串很小并且仍然是RAW编码的，请尝试EMBSTR编码，它更有效。在这种表
     * 示中，对象和SDS字符串被分配在相同的内存块中，以节省空间和缓存未命中。
     * */
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == OBJ_ENCODING_EMBSTR) return o;
        emb = createEmbeddedStringObject(s,sdslen(s));
        decrRefCount(o);
        return emb;
    }

    /* We can't encode the object...
     *
     * Do the last try, and at least optimize the SDS string inside
     * the string object to require little space, in case there
     * is more than 10% of free space at the end of the SDS string.
     *
     * We do that only for relatively large strings as this branch
     * is only entered if the length of the string is greater than
     * OBJ_ENCODING_EMBSTR_SIZE_LIMIT. 
     *
     * 我们无法对对象进行编码。。。如果SDS字符串的末尾有超过10%的可用空间，请进行
     * 最后一次尝试，至少优化字符串对象内的SDS字符串，使其只需要很少的空间。我们只对
     * 相对较大的字符串执行此操作，因为只有当字符串的长度大于OBJ_ENCODING_
     * EMBSTR_SIZE_LIMIT时才输入此分支。
     * */
    trimStringObjectIfNeeded(o);

    /* Return the original object. 
     *
     * 返回原始对象。
     * */
    return o;
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. 
 *
 * 获取编码对象的解码版本（作为新对象返回）。如果对象已经进行了原始编码，只需增加r
 * ef计数。
 * */
/*
 * 返回一个对象的未编码版本
 *
 * 如果输入对象是已编码的，那么返回的对象是输入对象的新对象副本
 * 如果输入对象是未编码的，那么为它的引用计数增一，然后返回它
 */
robj *getDecodedObject(robj *o) {
    robj *dec;

    // 返回未编码对象
    if (sdsEncodedObject(o)) {
        incrRefCount(o);
        return o;
    }

    // 返回已编码对象的未编码版本
    if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_INT) {
        char buf[32];

        // 将整数值转换回一个字符串对象
        ll2string(buf,32,(long)o->ptr);
        dec = createStringObject(buf,strlen(buf));
        return dec;
    } else {
        serverPanic("Unknown encoding type");
    }
}

/* Compare two string objects via strcmp() or strcoll() depending on flags.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: when REDIS_COMPARE_BINARY is used a binary-safe comparison
 * is used. 
 *
 * 根据标志，通过strcmp（）或strcoll（）比较两个字符串对象。请注意，对
 * 象可能是整数编码的。在这种情况下，我们使用ll2string（）来获取堆栈上数字
 * 的字符串表示并比较字符串，这比调用getDecodedObject（）快得多。重
 * 要提示：当使用REDIS_COMPARE_BINARY时，将使用二进制安全比较。
 * */

#define REDIS_COMPARE_BINARY (1<<0)
#define REDIS_COMPARE_COLL (1<<1)

int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
    serverAssertWithInfo(NULL,a,a->type == OBJ_STRING && b->type == OBJ_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;

    if (a == b) return 0;
    if (sdsEncodedObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);
    } else {
        alen = ll2string(bufa,sizeof(bufa),(long) a->ptr);
        astr = bufa;
    }
    if (sdsEncodedObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);
    } else {
        blen = ll2string(bufb,sizeof(bufb),(long) b->ptr);
        bstr = bufb;
    }
    if (flags & REDIS_COMPARE_COLL) {
        return strcoll(astr,bstr);
    } else {
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr,bstr,minlen);
        if (cmp == 0) return alen-blen;
        return cmp;
    }
}

/* Wrapper for compareStringObjectsWithFlags() using binary comparison. 
 *
 * 使用二进制比较的compareStringObjectsWithFlags（）的包装器。
 * */
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_BINARY);
}

/* Wrapper for compareStringObjectsWithFlags() using collation. 
 *
 * 使用排序规则的compareStringObjectsWithFlags（）的包装器。
 * */
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_COLL);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. 
 *
 * 如果从字符串比较的角度来看两个对象相同，则相等的字符串对象返回1，否则返回0。请
 * 注意，此函数比检查（compareStringObject（a，b）==0）更快，因为它可以执行更多的优化。
 * */
int equalStringObjects(robj *a, robj *b) {
    if (a->encoding == OBJ_ENCODING_INT &&
        b->encoding == OBJ_ENCODING_INT){
        /* If both strings are integer encoded just check if the stored
         * long is the same. 
         *
         * 如果两个字符串都是整数编码的，只需检查存储的长度是否相同。
         * */
        return a->ptr == b->ptr;
    } else {
        return compareStringObjects(a,b) == 0;
    }
}

size_t stringObjectLen(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    if (sdsEncodedObject(o)) {
        return sdslen(o->ptr);
    } else {
        return sdigits10((long)o->ptr);
    }
}

int getDoubleFromObject(const robj *o, double *target) {
    double value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (!string2d(o->ptr, sdslen(o->ptr), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

int getLongDoubleFromObject(robj *o, long double *target) {
    long double value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (!string2ld(o->ptr, sdslen(o->ptr), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg) {
    long double value;
    if (getLongDoubleFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

/*
 * 从对象 o 中提取 long long 值，并使用指针保存所提取的值
 */
int getLongLongFromObject(robj /* 提取对象 */ *o, long long *target/* 保存值的指针 */) {
    long long value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        // 根据不同编码，取出值
        if (sdsEncodedObject(o)) {
            if (string2ll(o->ptr,sdslen(o->ptr),&value) == 0) return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return C_OK;
}

/*
 * 将给定对象 o 的 long long 值取出，并通过指针 target 进行保存
 * 如果提取失败，添加错误信息 msg 到客户端 c
 */
int getLongLongFromObjectOrReply(client *c, robj *o, /* 要提取 long long 值的对象 */
                                 long long *target,  // 保存值的指针
                                 const char *msg) {  // 错误信息
    long long value;
    if (getLongLongFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not an integer or out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
    long long value;

    if (getLongLongFromObjectOrReply(c, o, &value, msg) != C_OK) return C_ERR;
    if (value < LONG_MIN || value > LONG_MAX) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

/*
 * 返回编码的字符串形式
 */
char *strEncoding(int encoding) {
    switch(encoding) {
    case OBJ_ENCODING_RAW: return "raw";
    case OBJ_ENCODING_INT: return "int";
    case OBJ_ENCODING_HT: return "hashtable";
    case OBJ_ENCODING_QUICKLIST: return "quicklist";
    case OBJ_ENCODING_ZIPLIST: return "ziplist";
    case OBJ_ENCODING_INTSET: return "intset";
    case OBJ_ENCODING_SKIPLIST: return "skiplist";
    case OBJ_ENCODING_EMBSTR: return "embstr";
    case OBJ_ENCODING_STREAM: return "stream";
    default: return "unknown";
    }
}

/* =========================== Memory introspection ========================= 
 *                             内存内省
 * */


/* This is an helper function with the goal of estimating the memory
 * size of a radix tree that is used to store Stream IDs.
 *
 * Note: to guess the size of the radix tree is not trivial, so we
 * approximate it considering 16 bytes of data overhead for each
 * key (the ID), and then adding the number of bare nodes, plus some
 * overhead due by the data and child pointers. This secret recipe
 * was obtained by checking the average radix tree created by real
 * workloads, and then adjusting the constants to get numbers that
 * more or less match the real memory usage.
 *
 * Actually the number of nodes and keys may be different depending
 * on the insertion speed and thus the ability of the radix tree
 * to compress prefixes. 
 *
 * 这是一个辅助函数，目的是估计用于存储流ID的基数树的内存大小。注意：猜测基数树的
 * 大小并非易事，因此我们考虑每个键（ID）16字节的数据开销，然后添加裸节点的数量，
 * 再加上数据和子指针引起的一些开销，对其进行近似。这个秘密配方是通过检查实际工作
 * 负载创建的平均基数树，然后调整常数以获得或多或少与实际内存使用情况匹配的数字来获
 * 得的。实际上，节点和键的数量可能不同，这取决于插入速度，从而取决于基数树压缩前
 * 缀的能力。
 * */
size_t streamRadixTreeMemoryUsage(rax *rax) {
    size_t size;
    size = rax->numele * sizeof(streamID);
    size += rax->numnodes * sizeof(raxNode);
    /* Add a fixed overhead due to the aux data pointer, children, ... 
     *
     * 由于辅助数据指针、子进程原因，添加了固定的开销。。。
     * */
    size += rax->numnodes * sizeof(long)*30;
    return size;
}

/* Returns the size in bytes consumed by the key's value in RAM.
 * Note that the returned value is just an approximation, especially in the
 * case of aggregated data types where only "sample_size" elements
 * are checked and averaged to estimate the total size. 
 *
 * 返回RAM中键的值所消耗的字节大小。请注意，返回的值只是一个近似值，尤其是在聚合
 * 数据类型的情况下，只有“sample_size”元素被检查并取平均值来估计总大小。
 * */
#define OBJ_COMPUTE_SIZE_DEF_SAMPLES 5 /* Default sample size. 
                                        *
                                        * 默认样本大小。
                                        * */
size_t objectComputeSize(robj *o, size_t sample_size) {
    sds ele, ele2;
    dict *d;
    dictIterator *di;
    struct dictEntry *de;
    size_t asize = 0, elesize = 0, samples = 0;

    if (o->type == OBJ_STRING) {
        if(o->encoding == OBJ_ENCODING_INT) {
            asize = sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_RAW) {
            asize = sdsZmallocSize(o->ptr)+sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_EMBSTR) {
            asize = sdslen(o->ptr)+2+sizeof(*o);
        } else {
            serverPanic("Unknown string encoding");
        }
    } else if (o->type == OBJ_LIST) {
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;
            asize = sizeof(*o)+sizeof(quicklist);
            do {
                elesize += sizeof(quicklistNode)+ziplistBlobLen(node->zl);
                samples++;
            } while ((node = node->next) && samples < sample_size);
            asize += (double)elesize/samples*ql->len;
        } else if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+ziplistBlobLen(o->ptr);
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        if (o->encoding == OBJ_ENCODING_HT) {
            d = o->ptr;
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = dictGetKey(de);
                elesize += sizeof(struct dictEntry) + sdsZmallocSize(ele);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            intset *is = o->ptr;
            asize = sizeof(*o)+sizeof(*is)+(size_t)is->encoding*is->length;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen(o->ptr));
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            d = ((zset*)o->ptr)->dict;
            zskiplist *zsl = ((zset*)o->ptr)->zsl;
            zskiplistNode *znode = zsl->header->level[0].forward;
            asize = sizeof(*o)+sizeof(zset)+sizeof(zskiplist)+sizeof(dict)+
                    (sizeof(struct dictEntry*)*dictSlots(d))+
                    zmalloc_size(zsl->header);
            while(znode != NULL && samples < sample_size) {
                elesize += sdsZmallocSize(znode->ele);
                elesize += sizeof(struct dictEntry) + zmalloc_size(znode);
                samples++;
                znode = znode->level[0].forward;
            }
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen(o->ptr));
        } else if (o->encoding == OBJ_ENCODING_HT) {
            d = o->ptr;
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = dictGetKey(de);
                ele2 = dictGetVal(de);
                elesize += sdsZmallocSize(ele) + sdsZmallocSize(ele2);
                elesize += sizeof(struct dictEntry);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        stream *s = o->ptr;
        asize = sizeof(*o);
        asize += streamRadixTreeMemoryUsage(s->rax);

        /* Now we have to add the listpacks. The last listpack is often non
         * complete, so we estimate the size of the first N listpacks, and
         * use the average to compute the size of the first N-1 listpacks, and
         * finally add the real size of the last node. 
         *
         * 现在我们必须添加列表包。最后一个列表包通常是不完整的，因此我们估计前N个列表包的
         * 大小，并使用平均值计算前N-1个列表包，最后添加最后一个节点的实际大小。
         * */
        raxIterator ri;
        raxStart(&ri,s->rax);
        raxSeek(&ri,"^",NULL,0);
        size_t lpsize = 0, samples = 0;
        while(samples < sample_size && raxNext(&ri)) {
            unsigned char *lp = ri.data;
            lpsize += lpBytes(lp);
            samples++;
        }
        if (s->rax->numele <= samples) {
            asize += lpsize;
        } else {
            if (samples) lpsize /= samples; /* Compute the average. 
                                             *
                                             * 计算平均值。
                                             * */
            asize += lpsize * (s->rax->numele-1);
            /* No need to check if seek succeeded, we enter this branch only
             * if there are a few elements in the radix tree. 
             *
             * 无需检查查找是否成功，只有当基数树中有几个元素时，我们才进入该分支。
             * */
            raxSeek(&ri,"$",NULL,0);
            raxNext(&ri);
            asize += lpBytes(ri.data);
        }
        raxStop(&ri);

        /* Consumer groups also have a non trivial memory overhead if there
         * are many consumers and many groups, let's count at least the
         * overhead of the pending entries in the groups and consumers
         * PELs. 
         *
         * 使用者组也有一个非平凡的内存开销。如果有许多使用者和许多组，让我们至少计算一下组
         * 和使用者PEL中挂起的节点的开销。
         * */
        if (s->cgroups) {
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;
                asize += sizeof(*cg);
                asize += streamRadixTreeMemoryUsage(cg->pel);
                asize += sizeof(streamNACK)*raxSize(cg->pel);

                /* For each consumer we also need to add the basic data
                 * structures and the PEL memory usage. 
                 *
                 * 对于每个消费者，我们还需要添加基本数据结构和PEL内存使用情况。
                 * */
                raxIterator cri;
                raxStart(&cri,cg->consumers);
                raxSeek(&cri,"^",NULL,0);
                while(raxNext(&cri)) {
                    streamConsumer *consumer = cri.data;
                    asize += sizeof(*consumer);
                    asize += sdslen(consumer->name);
                    asize += streamRadixTreeMemoryUsage(consumer->pel);
                    /* Don't count NACKs again, they are shared with the
                     * consumer group PEL. 
                     *
                     * 不要再计算NACK，它们与消费者团体PEL共享。
                     * */
                }
                raxStop(&cri);
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;
        if (mt->mem_usage != NULL) {
            asize = mt->mem_usage(mv->value);
        } else {
            asize = 0;
        }
    } else {
        serverPanic("Unknown object type");
    }
    return asize;
}

/* Release data obtained with getMemoryOverheadData(). 
 *
 * 释放使用getMemoryOverheadData（）获得的数据。
 * */
void freeMemoryOverheadData(struct redisMemOverhead *mh) {
    zfree(mh->db);
    zfree(mh);
}

/* Return a struct redisMemOverhead filled with memory overhead
 * information used for the MEMORY OVERHEAD and INFO command. The returned
 * structure pointer should be freed calling freeMemoryOverheadData(). 
 *
 * 返回一个结构体redisMemOverhead，该结构体填充了用于memoryoverhead和INFO命令的内存开销信息。
 * 应通过调用freeMemoryOverheadData（）释放返回的结构指针。
 * */
struct redisMemOverhead *getMemoryOverheadData(void) {
    int j;
    size_t mem_total = 0;
    size_t mem = 0;
    size_t zmalloc_used = zmalloc_used_memory();
    struct redisMemOverhead *mh = zcalloc(sizeof(*mh));

    mh->total_allocated = zmalloc_used;
    mh->startup_allocated = server.initial_memory_usage;
    mh->peak_allocated = server.stat_peak_memory;
    mh->total_frag =
        (float)server.cron_malloc_stats.process_rss / server.cron_malloc_stats.zmalloc_used;
    mh->total_frag_bytes =
        server.cron_malloc_stats.process_rss - server.cron_malloc_stats.zmalloc_used;
    mh->allocator_frag =
        (float)server.cron_malloc_stats.allocator_active / server.cron_malloc_stats.allocator_allocated;
    mh->allocator_frag_bytes =
        server.cron_malloc_stats.allocator_active - server.cron_malloc_stats.allocator_allocated;
    mh->allocator_rss =
        (float)server.cron_malloc_stats.allocator_resident / server.cron_malloc_stats.allocator_active;
    mh->allocator_rss_bytes =
        server.cron_malloc_stats.allocator_resident - server.cron_malloc_stats.allocator_active;
    mh->rss_extra =
        (float)server.cron_malloc_stats.process_rss / server.cron_malloc_stats.allocator_resident;
    mh->rss_extra_bytes =
        server.cron_malloc_stats.process_rss - server.cron_malloc_stats.allocator_resident;

    mem_total += server.initial_memory_usage;

    mem = 0;
    if (server.repl_backlog)
        mem += zmalloc_size(server.repl_backlog);
    mh->repl_backlog = mem;
    mem_total += mem;

    /* Computing the memory used by the clients would be O(N) if done
     * here online. We use our values computed incrementally by
     * clientsCronTrackClientsMemUsage(). 
     *
     * 如果在这里在线计算，客户端使用的内存将为O（N）。我们使用由clientsCronTrackClientsMemUsage（）递增计算的值。
     * */
    mh->clients_slaves = server.stat_clients_type_memory[CLIENT_TYPE_SLAVE];
    mh->clients_normal = server.stat_clients_type_memory[CLIENT_TYPE_MASTER]+
                         server.stat_clients_type_memory[CLIENT_TYPE_PUBSUB]+
                         server.stat_clients_type_memory[CLIENT_TYPE_NORMAL];
    mem_total += mh->clients_slaves;
    mem_total += mh->clients_normal;

    mem = 0;
    if (server.aof_state != AOF_OFF) {
        mem += sdsZmallocSize(server.aof_buf);
        mem += aofRewriteBufferSize();
    }
    mh->aof_buffer = mem;
    mem_total+=mem;

    mem = server.lua_scripts_mem;
    mem += dictSize(server.lua_scripts) * sizeof(dictEntry) +
        dictSlots(server.lua_scripts) * sizeof(dictEntry*);
    mem += dictSize(server.repl_scriptcache_dict) * sizeof(dictEntry) +
        dictSlots(server.repl_scriptcache_dict) * sizeof(dictEntry*);
    if (listLength(server.repl_scriptcache_fifo) > 0) {
        mem += listLength(server.repl_scriptcache_fifo) * (sizeof(listNode) + 
            sdsZmallocSize(listNodeValue(listFirst(server.repl_scriptcache_fifo))));
    }
    mh->lua_caches = mem;
    mem_total+=mem;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        long long keyscount = dictSize(db->dict);
        if (keyscount==0) continue;

        mh->total_keys += keyscount;
        mh->db = zrealloc(mh->db,sizeof(mh->db[0])*(mh->num_dbs+1));
        mh->db[mh->num_dbs].dbid = j;

        mem = dictSize(db->dict) * sizeof(dictEntry) +
              dictSlots(db->dict) * sizeof(dictEntry*) +
              dictSize(db->dict) * sizeof(robj);
        mh->db[mh->num_dbs].overhead_ht_main = mem;
        mem_total+=mem;

        mem = dictSize(db->expires) * sizeof(dictEntry) +
              dictSlots(db->expires) * sizeof(dictEntry*);
        mh->db[mh->num_dbs].overhead_ht_expires = mem;
        mem_total+=mem;

        mh->num_dbs++;
    }

    mh->overhead_total = mem_total;
    mh->dataset = zmalloc_used - mem_total;
    mh->peak_perc = (float)zmalloc_used*100/mh->peak_allocated;

    /* Metrics computed after subtracting the startup memory from
     * the total memory. 
     *
     * 从总内存中减去启动内存后计算的度量。
     * */
    size_t net_usage = 1;
    if (zmalloc_used > mh->startup_allocated)
        net_usage = zmalloc_used - mh->startup_allocated;
    mh->dataset_perc = (float)mh->dataset*100/net_usage;
    mh->bytes_per_key = mh->total_keys ? (net_usage / mh->total_keys) : 0;

    return mh;
}

/* Helper for "MEMORY allocator-stats", used as a callback for the jemalloc
 * stats output. 
 *
 * “MEMORY分配器统计数据”的帮助程序，用作jemalloc统计数据输出的回调
 * 。
 * */
void inputCatSds(void *result, const char *str) {
    /* result is actually a (sds *), so re-cast it here 
     *
     * 结果实际上是一个（sds*），所以在这里重新播放
     * */
    sds *info = (sds *)result;
    *info = sdscat(*info, str);
}

/* This implements MEMORY DOCTOR. An human readable analysis of the Redis
 * memory condition. 
 *
 * 这实现了MEMORY DOCTOR。Redis内存状况的可读分析。
 * */
sds getMemoryDoctorReport(void) {
    int empty = 0;          /* Instance is empty or almost empty. 
                             *
                             * 实例为空或几乎为空。
                             * */
    int big_peak = 0;       /* Memory peak is much larger than used mem. 
                             *
                             * 内存峰值比使用过的内存大得多。
                             * */
    int high_frag = 0;      /* High fragmentation. 
                             *
                             * 高度碎片化。
                             * */
    int high_alloc_frag = 0;/* High allocator fragmentation. 
                             *
                             * 分配器碎片高。
                             * */
    int high_proc_rss = 0;  /* High process rss overhead. 
                             *
                             * 高处理rss开销。
                             * */
    int high_alloc_rss = 0; /* High rss overhead. 
                             *
                             * rss开销高。
                             * */
    int big_slave_buf = 0;  /* Slave buffers are too big. 
                             *
                             * 从属缓冲区太大。
                             * */
    int big_client_buf = 0; /* Client buffers are too big. 
                             *
                             * 客户端缓冲区太大。
                             * */
    int many_scripts = 0;   /* Script cache has too many scripts. 
                             *
                             * 脚本缓存中的脚本太多。
                             * */
    int num_reports = 0;
    struct redisMemOverhead *mh = getMemoryOverheadData();

    if (mh->total_allocated < (1024*1024*5)) {
        empty = 1;
        num_reports++;
    } else {
        /* Peak is > 150% of current used memory? 
         *
         * 峰值是否大于当前已用内存的150%？
         * */
        if (((float)mh->peak_allocated / mh->total_allocated) > 1.5) {
            big_peak = 1;
            num_reports++;
        }

        /* Fragmentation is higher than 1.4 and 10MB ?
         *
         * 碎片化高于1.4和10MB？
         * */
        if (mh->total_frag > 1.4 && mh->total_frag_bytes > 10<<20) {
            high_frag = 1;
            num_reports++;
        }

        /* External fragmentation is higher than 1.1 and 10MB? 
         *
         * 外部碎片高于1.1和10MB？
         * */
        if (mh->allocator_frag > 1.1 && mh->allocator_frag_bytes > 10<<20) {
            high_alloc_frag = 1;
            num_reports++;
        }

        /* Allocator rss is higher than 1.1 and 10MB ? 
         *
         * 分配器rss高于1.1和10MB？
         * */
        if (mh->allocator_rss > 1.1 && mh->allocator_rss_bytes > 10<<20) {
            high_alloc_rss = 1;
            num_reports++;
        }

        /* Non-Allocator rss is higher than 1.1 and 10MB ? 
         *
         * 非分配器rss高于1.1和10MB？
         * */
        if (mh->rss_extra > 1.1 && mh->rss_extra_bytes > 10<<20) {
            high_proc_rss = 1;
            num_reports++;
        }

        /* Clients using more than 200k each average? 
         *
         * 平均每个使用量超过20万的客户？
         * */
        long numslaves = listLength(server.slaves);
        long numclients = listLength(server.clients)-numslaves;
        if (mh->clients_normal / numclients > (1024*200)) {
            big_client_buf = 1;
            num_reports++;
        }

        /* Slaves using more than 10 MB each? 
         *
         * 每个从属服务器使用超过10 MB？
         * */
        if (numslaves > 0 && mh->clients_slaves / numslaves > (1024*1024*10)) {
            big_slave_buf = 1;
            num_reports++;
        }

        /* Too many scripts are cached? 
         *
         * 缓存了太多脚本？
         * */
        if (dictSize(server.lua_scripts) > 1000) {
            many_scripts = 1;
            num_reports++;
        }
    }

    sds s;
    if (num_reports == 0) {
        s = sdsnew(
        "Hi Sam, I can't find any memory issue in your instance. "
        "I can only account for what occurs on this base.\n");
    } else if (empty == 1) {
        s = sdsnew(
        "Hi Sam, this instance is empty or is using very little memory, "
        "my issues detector can't be used in these conditions. "
        "Please, leave for your mission on Earth and fill it with some data. "
        "The new Sam and I will be back to our programming as soon as I "
        "finished rebooting.\n");
    } else {
        s = sdsnew("Sam, I detected a few issues in this Redis instance memory implants:\n\n");
        if (big_peak) {
            s = sdscat(s," * Peak memory: In the past this instance used more than 150% the memory that is currently using. The allocator is normally not able to release memory after a peak, so you can expect to see a big fragmentation ratio, however this is actually harmless and is only due to the memory peak, and if the Redis instance Resident Set Size (RSS) is currently bigger than expected, the memory will be used as soon as you fill the Redis instance with more data. If the memory peak was only occasional and you want to try to reclaim memory, please try the MEMORY PURGE command, otherwise the only other option is to shutdown and restart the instance.\n\n");
        }
        if (high_frag) {
            s = sdscatprintf(s," * High total RSS: This instance has a memory fragmentation and RSS overhead greater than 1.4 (this means that the Resident Set Size of the Redis process is much larger than the sum of the logical allocations Redis performed). This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. If the problem is a large peak memory, then there is no issue. Otherwise, make sure you are using the Jemalloc allocator and not the default libc malloc. Note: The currently used allocator is \"%s\".\n\n", ZMALLOC_LIB);
        }
        if (high_alloc_frag) {
            s = sdscatprintf(s," * High allocator fragmentation: This instance has an allocator external fragmentation greater than 1.1. This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. You can try enabling 'activedefrag' config option.\n\n");
        }
        if (high_alloc_rss) {
            s = sdscatprintf(s," * High allocator RSS overhead: This instance has an RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the allocator is much larger than the sum what the allocator actually holds). This problem is usually due to a large peak memory (check if there is a peak memory entry above in the report), you can try the MEMORY PURGE command to reclaim it.\n\n");
        }
        if (high_proc_rss) {
            s = sdscatprintf(s," * High process RSS overhead: This instance has non-allocator RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the Redis process is much larger than the RSS the allocator holds). This problem may be due to Lua scripts or Modules.\n\n");
        }
        if (big_slave_buf) {
            s = sdscat(s," * Big replica buffers: The replica output buffers in this instance are greater than 10MB for each replica (on average). This likely means that there is some replica instance that is struggling receiving data, either because it is too slow or because of networking issues. As a result, data piles on the master output buffers. Please try to identify what replica is not receiving data correctly and why. You can use the INFO output in order to check the replicas delays and the CLIENT LIST command to check the output buffers of each replica.\n\n");
        }
        if (big_client_buf) {
            s = sdscat(s," * Big client buffers: The clients output buffers in this instance are greater than 200K per client (on average). This may result from different causes, like Pub/Sub clients subscribed to channels bot not receiving data fast enough, so that data piles on the Redis instance output buffer, or clients sending commands with large replies or very large sequences of commands in the same pipeline. Please use the CLIENT LIST command in order to investigate the issue if it causes problems in your instance, or to understand better why certain clients are using a big amount of memory.\n\n");
        }
        if (many_scripts) {
            s = sdscat(s," * Many scripts: There seem to be many cached scripts in this instance (more than 1000). This may be because scripts are generated and `EVAL`ed, instead of being parameterized (with KEYS and ARGV), `SCRIPT LOAD`ed and `EVALSHA`ed. Unless `SCRIPT FLUSH` is called periodically, the scripts' caches may end up consuming most of your memory.\n\n");
        }
        s = sdscat(s,"I'm here to keep you safe, Sam. I want to help you.\n");
    }
    freeMemoryOverheadData(mh);
    return s;
}

/* Set the object LRU/LFU depending on server.maxmemory_policy.
 * The lfu_freq arg is only relevant if policy is MAXMEMORY_FLAG_LFU.
 * The lru_idle and lru_clock args are only relevant if policy
 * is MAXMEMORY_FLAG_LRU.
 * Either or both of them may be <0, in that case, nothing is set. 
 *
 * 根据server.maxmemory_policy设置对象LRU/LFU。只有当
 * 策略为maxmemory_FLAG_LFU时，lfufreq参数才相关。只有当策
 * 略为MAXMEMORY_FLAG_lru时，lru_idle和lru_lock参数才相关。
 *
 * 它们中的一个或两个可能都＜0，在这种情况下，不设置任何内容。
 * */
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier) {
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        if (lfu_freq >= 0) {
            serverAssert(lfu_freq <= 255);
            val->lru = (LFUGetTimeInMinutes()<<8) | lfu_freq;
            return 1;
        }
    } else if (lru_idle >= 0) {
        /* Provided LRU idle time is in seconds. Scale
         * according to the LRU clock resolution this Redis
         * instance was compiled with (normally 1000 ms, so the
         * below statement will expand to lru_idle*1000/1000. 
         *
         * 假设LRU空闲时间以秒为单位。根据这个Redis实例编译时使用的LRU时钟分辨率
         * 进行缩放（通常为1000ms，因此下面的语句将扩展到LRU_idle*1000/
         * 1000。
         * */
        lru_idle = lru_idle*lru_multiplier/LRU_CLOCK_RESOLUTION;
        long lru_abs = lru_clock - lru_idle; /* Absolute access time. 
                                              *
                                              * 绝对访问时间。
                                              * */
        /* If the LRU field underflows (since LRU it is a wrapping
         * clock), the best we can do is to provide a large enough LRU
         * that is half-way in the circlular LRU clock we use: this way
         * the computed idle time for this object will stay high for quite
         * some time. 
         *
         * 如果LRU字段下溢（因为LRU是一个包装时钟），我们能做的最好的事情就是提供一个
         * 足够大的LRU，它是我们使用的环形LRU时钟的一半：这样，该对象的计算空闲时间将
         * 在相当长的一段时间内保持较高。
         * */
        if (lru_abs < 0)
            lru_abs = (lru_clock+(LRU_CLOCK_MAX/2)) % LRU_CLOCK_MAX;
        val->lru = lru_abs;
        return 1;
    }
    return 0;
}

/* ======================= The OBJECT and MEMORY commands =================== 
 *                         OBJECT和MEMORY命令
 * */

/* This is a helper function for the OBJECT command. We need to lookup keys
 * without any modification of LRU or other parameters. 
 *
 * 这是OBJECT命令的辅助函数。我们需要在不修改LRU或其他参数的情况下查找键
 * 。
 * */
robj *objectCommandLookup(client *c, robj *key) {
    return lookupKeyReadWithFlags(c->db,key,LOOKUP_NOTOUCH|LOOKUP_NONOTIFY);
}

robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply) {
    robj *o = objectCommandLookup(c,key);

    if (!o) addReply(c, reply);
    return o;
}

/* Object command allows to inspect the internals of a Redis Object.
 * Usage: OBJECT <refcount|encoding|idletime|freq> <key> */
void objectCommand(client *c) {
    robj *o;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ENCODING <key> -- Return the kind of internal representation used in order to store the value associated with a key.",
"FREQ <key> -- Return the access frequency index of the key. The returned integer is proportional to the logarithm of the recent access frequency of the key.",
"IDLETIME <key> -- Return the idle time of the key, that is the approximated number of seconds elapsed since the last access to the key.",
"REFCOUNT <key> -- Return the number of references of the value associated with the specified key.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        addReplyLongLong(c,o->refcount);
    } else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        addReplyBulkCString(c,strEncoding(o->encoding));
    } else if (!strcasecmp(c->argv[1]->ptr,"idletime") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            addReplyError(c,"An LFU maxmemory policy is selected, idle time not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        addReplyLongLong(c,estimateObjectIdleTime(o)/1000);
    } else if (!strcasecmp(c->argv[1]->ptr,"freq") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        if (!(server.maxmemory_policy & MAXMEMORY_FLAG_LFU)) {
            addReplyError(c,"An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        /* LFUDecrAndReturn should be called
         * in case of the key has not been accessed for a long time,
         * because we update the access time only
         * when the key is read or overwritten. 
         *
         * 如果键长时间未被访问，则应调用LFUDecrAndReturn，因为我们只有在
         * 读取或覆盖键时才会更新访问时间。
         * */
        addReplyLongLong(c,LFUDecrAndReturn(o));
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* The memory command will eventually be a complete interface for the
 * memory introspection capabilities of Redis.
 *
 * Usage: MEMORY usage <key> 
 *
 * 内存命令最终将成为Redis内存自省功能的完整接口。
 * 
 * MEMORY usage <key> 
 * */
void memoryCommand(client *c) {
    if (!strcasecmp(c->argv[1]->ptr,"help") && c->argc == 2) {
        const char *help[] = {
"DOCTOR - Return memory problems reports.",
"MALLOC-STATS -- Return internal statistics report from the memory allocator.",
"PURGE -- Attempt to purge dirty pages for reclamation by the allocator.",
"STATS -- Return information about the memory usage of the server.",
"USAGE <key> [SAMPLES <count>] -- Return memory in bytes used by <key> and its value. Nested values are sampled up to <count> times (default: 5).",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"usage") && c->argc >= 3) {
        dictEntry *de;
        long long samples = OBJ_COMPUTE_SIZE_DEF_SAMPLES;
        for (int j = 3; j < c->argc; j++) {
            if (!strcasecmp(c->argv[j]->ptr,"samples") &&
                j+1 < c->argc)
            {
                if (getLongLongFromObjectOrReply(c,c->argv[j+1],&samples,NULL)
                     == C_ERR) return;
                if (samples < 0) {
                    addReply(c,shared.syntaxerr);
                    return;
                }
                if (samples == 0) samples = LLONG_MAX;;
                j++; /* skip option argument. 
                      *
                      * 跳过选项参数。
                      * */
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
        if ((de = dictFind(c->db->dict,c->argv[2]->ptr)) == NULL) {
            addReplyNull(c);
            return;
        }
        size_t usage = objectComputeSize(dictGetVal(de),samples);
        usage += sdsZmallocSize(dictGetKey(de));
        usage += sizeof(dictEntry);
        addReplyLongLong(c,usage);
    } else if (!strcasecmp(c->argv[1]->ptr,"stats") && c->argc == 2) {
        struct redisMemOverhead *mh = getMemoryOverheadData();

        addReplyMapLen(c,25+mh->num_dbs);

        addReplyBulkCString(c,"peak.allocated");
        addReplyLongLong(c,mh->peak_allocated);

        addReplyBulkCString(c,"total.allocated");
        addReplyLongLong(c,mh->total_allocated);

        addReplyBulkCString(c,"startup.allocated");
        addReplyLongLong(c,mh->startup_allocated);

        addReplyBulkCString(c,"replication.backlog");
        addReplyLongLong(c,mh->repl_backlog);

        addReplyBulkCString(c,"clients.slaves");
        addReplyLongLong(c,mh->clients_slaves);

        addReplyBulkCString(c,"clients.normal");
        addReplyLongLong(c,mh->clients_normal);

        addReplyBulkCString(c,"aof.buffer");
        addReplyLongLong(c,mh->aof_buffer);

        addReplyBulkCString(c,"lua.caches");
        addReplyLongLong(c,mh->lua_caches);

        for (size_t j = 0; j < mh->num_dbs; j++) {
            char dbname[32];
            snprintf(dbname,sizeof(dbname),"db.%zd",mh->db[j].dbid);
            addReplyBulkCString(c,dbname);
            addReplyMapLen(c,2);

            addReplyBulkCString(c,"overhead.hashtable.main");
            addReplyLongLong(c,mh->db[j].overhead_ht_main);

            addReplyBulkCString(c,"overhead.hashtable.expires");
            addReplyLongLong(c,mh->db[j].overhead_ht_expires);
        }

        addReplyBulkCString(c,"overhead.total");
        addReplyLongLong(c,mh->overhead_total);

        addReplyBulkCString(c,"keys.count");
        addReplyLongLong(c,mh->total_keys);

        addReplyBulkCString(c,"keys.bytes-per-key");
        addReplyLongLong(c,mh->bytes_per_key);

        addReplyBulkCString(c,"dataset.bytes");
        addReplyLongLong(c,mh->dataset);

        addReplyBulkCString(c,"dataset.percentage");
        addReplyDouble(c,mh->dataset_perc);

        addReplyBulkCString(c,"peak.percentage");
        addReplyDouble(c,mh->peak_perc);

        addReplyBulkCString(c,"allocator.allocated");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_allocated);

        addReplyBulkCString(c,"allocator.active");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_active);

        addReplyBulkCString(c,"allocator.resident");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_resident);

        addReplyBulkCString(c,"allocator-fragmentation.ratio");
        addReplyDouble(c,mh->allocator_frag);

        addReplyBulkCString(c,"allocator-fragmentation.bytes");
        addReplyLongLong(c,mh->allocator_frag_bytes);

        addReplyBulkCString(c,"allocator-rss.ratio");
        addReplyDouble(c,mh->allocator_rss);

        addReplyBulkCString(c,"allocator-rss.bytes");
        addReplyLongLong(c,mh->allocator_rss_bytes);

        addReplyBulkCString(c,"rss-overhead.ratio");
        addReplyDouble(c,mh->rss_extra);

        addReplyBulkCString(c,"rss-overhead.bytes");
        addReplyLongLong(c,mh->rss_extra_bytes);

        addReplyBulkCString(c,"fragmentation"); /* this is the total RSS overhead, including fragmentation 
                                                 *
                                                 * 这是RSS的总开销，包括碎片
                                                 * */
        addReplyDouble(c,mh->total_frag); /* it is kept here for backwards compatibility 
                                           *
                                           * 它保留在这里是为了向后兼容
                                           * */

        addReplyBulkCString(c,"fragmentation.bytes");
        addReplyLongLong(c,mh->total_frag_bytes);

        freeMemoryOverheadData(mh);
    } else if (!strcasecmp(c->argv[1]->ptr,"malloc-stats") && c->argc == 2) {
#if defined(USE_JEMALLOC)
        sds info = sdsempty();
        je_malloc_stats_print(inputCatSds, &info, NULL);
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(info);
#else
        addReplyBulkCString(c,"Stats not supported for the current allocator");
#endif
    } else if (!strcasecmp(c->argv[1]->ptr,"doctor") && c->argc == 2) {
        sds report = getMemoryDoctorReport();
        addReplyVerbatim(c,report,sdslen(report),"txt");
        sdsfree(report);
    } else if (!strcasecmp(c->argv[1]->ptr,"purge") && c->argc == 2) {
        if (jemalloc_purge() == 0)
            addReply(c, shared.ok);
        else
            addReplyError(c, "Error purging dirty pages");
    } else {
        addReplyErrorFormat(c, "Unknown subcommand or wrong number of arguments for '%s'. Try MEMORY HELP", (char*)c->argv[1]->ptr);
    }
}
