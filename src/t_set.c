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

/*-----------------------------------------------------------------------------
 * Set Commands
 * 设置 命令
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op);

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a regular
 * hash table. 
 *
 * Factory方法返回一个*canhold“value”的集合。当对象具有可编码
 * 的整数值时，将返回一个intset。否则就是一个普通的哈希表。
 * */
/*
 * 根据给定值 value ，决定是创建 intset 编码还是字典编码的集合
 *
 * T = O(1)
 */
robj *setTypeCreate(sds value) {
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK)
        return createIntsetObject();
    return createSetObject();
}

/* Add the specified value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. 
 *
 * 将指定的值添加到集合中。如果该值已经是集合的成员，则不执行任何操作并返回0，否则
 * 将添加新元素并返回1。
 * */
/*
 * 多态添加操作
 *
 * T = O(N)
 */
int setTypeAdd(robj *subject, sds value) {
    long long llval;
    // subject 为字典,O(1)
    if (subject->encoding == OBJ_ENCODING_HT) {
        dict *ht = subject->ptr;
        dictEntry *de = dictAddRaw(ht,value,NULL);
        if (de) {
            dictSetKey(ht,de,sdsdup(value));
            dictSetVal(ht,de,NULL);
            return 1;
        }
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        // subject 为intset , O(N)

        // value 可以表示为 long long 类型
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            uint8_t success = 0;
            // 添加值, O(N)
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
            // 添加成功
            if (success) {
                /* Convert to regular set when the intset contains
                 * too many entries. 
                 *
                 * 当intset包含太多条目时，转换为正则集。
                 * */
                size_t max_entries = server.set_max_intset_entries;
                /* limit to 1G entries due to intset internals. 
                 *
                 * 由于intset内部的原因，限制为1G条目。
                 * */
                if (max_entries >= 1<<30) max_entries = 1<<30;
                // 检查是否需要将 intset 转换为字典
                if (intsetLen(subject->ptr) > max_entries)
                    setTypeConvert(subject,OBJ_ENCODING_HT);
                return 1;
            }
        } else {
            // value 不能保存为 long long 类型，必须转换为字典
            /* Failed to get integer from object, convert to regular set. 
             *
             * 无法从对象中获取整数，请转换为常规集。
             * */

            setTypeConvert(subject,OBJ_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. 
             *
             * set*是一个intset，这个值不能进行整数编码，所以dictAdd应该总是有效的。
             * */
            // 添加值
            serverAssert(dictAdd(subject->ptr,sdsdup(value),NULL) == DICT_OK);
            return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/*
 * 多态删除操作
 *
 * T = O(N)
 */
int setTypeRemove(robj *setobj, sds value) {
    long long llval;
    // 字典编码, O(N)
    if (setobj->encoding == OBJ_ENCODING_HT) {
        if (dictDelete(setobj->ptr,value) == DICT_OK) {
            // 如果有需要，缩小字典, O(N)
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        // intset 编码

        // 如果 value 可以编码成 long long 类型，
        // 那么尝试在 intset 删除它
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            int success;
            // O(N)
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/*
 * 多态成员检查操作
 *
 * T = O(lg N)
 */
int setTypeIsMember(robj *subject, sds value) {
    long long llval;
    // 字典
    if (subject->encoding == OBJ_ENCODING_HT) {
        // O(1)
        return dictFind((dict*)subject->ptr,value) != NULL;
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        // intset

        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            // O(lg N)
            return intsetFind((intset*)subject->ptr,llval);
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/*
 * 创建一个多态迭代器
 *
 * T = O(1)
 */
setTypeIterator *setTypeInitIterator(robj *subject) {
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == OBJ_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        serverPanic("Unknown set encoding");
    }
    return si;
}

/*
 * 释放多态迭代器
 *
 * T = O(1)
 */
void setTypeReleaseIterator(setTypeIterator *si) {
    if (si->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position.
 *
 * Since set elements can be internally be stored as SDS strings or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointer
 * (sdsele) or (llele) accordingly.
 *
 * Note that both the sdsele and llele pointers should be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no longer elements -1 is returned. 
 *
 * 移动到集合中的下一个条目。返回当前位置的对象。
 *
 * 由于集合元素可以在内部存储为SDS
 * 字符串或简单的整数数组，因此setTypeNext返回正在迭代的集合对象的编码，
 * 并相应地填充相应的指针（sdsele）或（llele）。
 *
 * 请注意，sdsele和llele指针都应该被传递，并且不能为NULL，因为函数会试图用错误使用的值来填充
 * 未使用的字段。
 *
 * 当不再有元素时，返回-1。
 * */
/*
 * 取出迭代器指向的当前元素
 *
 * robj 参数保存字典编码的值， llele 参数保存 intset 保存的值
 *
 * 返回值指示到底哪种编码的值被取出了，返回 -1 表示集合为空
 *
 * T = O(1)
 */
int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele) {
    // 字典
    if (si->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *sdsele = dictGetKey(de);
        *llele = -123456789; /* Not needed. Defensive. 
                              *
                              * 不需要。防御的
                              * */
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
        *sdsele = NULL;      /* Not needed. Defensive.
                              *
                              * 不需要。防御的
                              * */
    } else {
        serverPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. 
 *
 * setTypeNext（）的非写时复制友好但易于使用的版本是setTypeNextObject（），
 * 返回新的SDS字符串。因此，如果你没有保留指向这个对象的指针，你应该对它调用sdsfree（）。
 * 这个函数是在COW不是问题的情况下进行写操作的方法。
 * */
/*
 * 获取迭代器的当前元素，并返回包含它的一个对象。
 * 如果调用者不使用这个对象的话，必须在使用完毕之后释放它。
 *
 * T = O(1)
 */
sds setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    sds sdsele;
    int encoding;

    encoding = setTypeNext(si,&sdsele,&intele);
    switch(encoding) {
        case -1:    return NULL;
        case OBJ_ENCODING_INTSET:
            return sdsfromlonglong(intele);
        case OBJ_ENCODING_HT:
            return sdsdup(sdsele);
        default:
            serverPanic("Unsupported encoding");
    }
    return NULL; /* just to suppress warnings 
                  *
                  * 只是为了抑制警告
                  * */
}

/* Return random element from a non empty set.
 * The returned element can be an int64_t value if the set is encoded
 * as an "intset" blob of integers, or an SDS string if the set
 * is a regular set.
 *
 * The caller provides both pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and is used by the caller to check if the
 * int64_t pointer or the redis object pointer was populated.
 *
 * Note that both the sdsele and llele pointers should be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused. 
 *
 * 从非空集返回随机元素。如果集合被编码为整数的“intset”blob，则返回的元
 * 素可以是int64_t值，如果集合是正则集，则可以是SDS字符串。
 *
 * 调用方提供了两个指针，以便用正确的对象填充。函数的返回值是对象的object->encoding字段，
 * 由调用方用于检查是否填充了int64_t指针或redis对象指针。
 *
 * 请注意，sdsele和llele指针都应该被传递，并且不能为NULL，因为函数会试图用
 * 错误使用的值来填充未使用的字段。
 * */
/*
 * 多态随机元素返回函数
 *
 * objele 保存字典编码的值， llele 保存 intset 编码的值
 *
 * 返回值指示到底那种编码的值被保存了。
 *
 * T = O(N)
 */
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele) {
    // 字典
    if (setobj->encoding == OBJ_ENCODING_HT) {
        // O(N)
        dictEntry *de = dictGetFairRandomKey(setobj->ptr);
        // O(1)
        *sdsele = dictGetKey(de);
        *llele = -123456789; /* Not needed. Defensive. 
                              *
                              * 不需要。防御的
                              * */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        // intset

        // O(1)
        *llele = intsetRandom(setobj->ptr);
        *sdsele = NULL; /* Not needed. Defensive. 
                              *
                              * 不需要。防御的
                              * */
    } else {
        serverPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

/*
 * 多态元素个数返回函数
 *
 * T = O(1)
 */
unsigned long setTypeSize(const robj *subject) {
    // 字典
    if (subject->encoding == OBJ_ENCODING_HT) {
        return dictSize((const dict*)subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {

        // intset
        return intsetLen((const intset*)subject->ptr);
    } else {
        serverPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. 
 *
 * 将集合转换为指定的编码。生成的dict（当转换为哈希表时）被预处理以保持原始集合
 * 中的元素数量。
 * */
void setTypeConvert(robj *setobj, int enc) {
    setTypeIterator *si;
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding == OBJ_ENCODING_INTSET);

    if (enc == OBJ_ENCODING_HT) {
        int64_t intele;
        dict *d = dictCreate(&setDictType,NULL);
        sds element;

        /* Presize the dict to avoid rehashing 
         *
         * 预设dict以避免重复
         * */
        dictExpand(d,intsetLen(setobj->ptr));

        /* To add the elements we extract integers and create redis objects 
         *
         * 为了添加元素，我们提取整数并创建redis对象
         * */
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si,&element,&intele) != -1) {
            element = sdsfromlonglong(intele);
            serverAssert(dictAdd(d,element,NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        setobj->encoding = OBJ_ENCODING_HT;
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        serverPanic("Unsupported set conversion");
    }
}

/*
 * T = O(N^2)
 */
void saddCommand(client *c) {
    robj *set;
    int j, added = 0;

    // 查找集合对象
    set = lookupKeyWrite(c->db,c->argv[1]);
    // 没找到，创建一个空集合, O(1)
    if (set == NULL) {
        set = setTypeCreate(c->argv[2]->ptr);
        dbAdd(c->db,c->argv[1],set);
    } else {
        // 找到，但不是集合，报错

        if (set->type != OBJ_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }

    // 将所有输入元素添加到集合
    // O(N^2)
    for (j = 2; j < c->argc; j++) {
        if (setTypeAdd(set,c->argv[j]->ptr)) added++;
    }

    // 如果至少有一个元素添加成功，那么通知 db ，key 被修改
    if (added) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[1],c->db->id);
    }

    // 返回添加元素的个数
    server.dirty += added;
    addReplyLongLong(c,added);
}


/*
 * T = O(N^2)
 */
void sremCommand(client *c) {
    robj *set;
    int j, deleted = 0, keyremoved = 0;

    // 查找集合，如果集合不存在或类型错误，直接返回
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    // 移除多个元素, O(N^2)
    for (j = 2; j < c->argc; j++) {
        if (setTypeRemove(set,c->argv[j]->ptr)) {
            deleted++;
            if (setTypeSize(set) == 0) {
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }

    // 如果至少有一个元素被移除，那么通知 db ，key 被修改
    if (deleted) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

/*
 * T = O(N)
 */
void smoveCommand(client *c) {
    robj *srcset, *dstset, *ele;
    // 源集合
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    // 目标集合
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    // 要被移动的元素
    ele = c->argv[3];

    /* If the source key does not exist return 0 
     *
     * 如果源键不存在，则返回0
     * */
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. 
     *
     * 如果源键的类型错误，或者目标键设置错误，则返回错误。
     * */
    if (checkType(c,srcset,OBJ_SET) ||
        (dstset && checkType(c,dstset,OBJ_SET))) return;

    /* If srcset and dstset are equal, SMOVE is a no-op 
     *
     * 如果srcset和dstset相等，则SMOVE为无操作
     * */
    // 源集合和目标集合相同，直接返回
    if (srcset == dstset) {
        addReply(c,setTypeIsMember(srcset,ele->ptr) ?
            shared.cone : shared.czero);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. 
     *
     * 如果无法从src集中删除元素，则返回0。
     * */
    // O(N)
    if (!setTypeRemove(srcset,ele->ptr)) {
        addReply(c,shared.czero);
        return;
    }
    notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);

    /* Remove the src set from the database when empty 
     *
     * 空时从数据库中删除src集
     * */
    if (setTypeSize(srcset) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Create the destination set when it doesn't exist 
     *
     * 当目标集不存在时创建它
     * */
    if (!dstset) {
        dstset = setTypeCreate(ele->ptr);
        dbAdd(c->db,c->argv[2],dstset);
    }

    // 通知 db ， key 被修改
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;

    /* An extra key has changed when ele was successfully added to dstset 
     *
     * 当ele成功添加到dstset时，一个额外的键发生了更改
     * */
    // 将目标元素添加到集合
    // O(N)
    if (setTypeAdd(dstset,ele->ptr)) {
        server.dirty++;
        signalModifiedKey(c,c->db,c->argv[2]);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[2],c->db->id);
    }
    addReply(c,shared.cone);
}

/*
 * T = O(lg N)
 */
void sismemberCommand(client *c) {
    robj *set;

    // 查找对象，检查类型
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    // 返回结果, O(lg N)
    if (setTypeIsMember(set,c->argv[2]->ptr))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

/*
 * T = O(1)
 */
void scardCommand(client *c) {
    robj *o;

    // 查找对象，检查类型
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_SET)) return;

    // 返回 size ,O(1)
    addReplyLongLong(c,setTypeSize(o));
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. 
 *
 * 处理“SPOP键＜count＞”变体。该命令的正常版本由spopCommand（）函数本身处理。
 * */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. 
 *
 * 与剩余规模相比，我们使用“创建新集合”策略的集合应该大多少倍？请稍后阅读实现中的
 * 内容以了解更多信息。
 * */
#define SPOP_MOVE_STRATEGY_MUL 5

void spopWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    robj *set;

    /* Get the count argument 
     *
     * 获取count参数
     * */
    if (getLongFromObjectOrReply(c,c->argv[2],&l,NULL) != C_OK) return;
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        addReply(c,shared.outofrangeerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set. Otherwise, return nil 
     *
     * 确保输入名称的键存在，并且它的类型确实是一个集合。否则，返回nil
     * */
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.emptyset[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;

    /* If count is zero, serve an empty set ASAP to avoid special
     * cases later. 
     *
     * 如果计数为零，请尽快提供空盘，以避免以后出现特殊情况。
     * */
    if (count == 0) {
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    size = setTypeSize(set);

    /* Generate an SPOP keyspace notification 
     *
     * 生成SPOP键空间通知
     * */
    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);
    server.dirty += count;

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set. 
     *
     * 情况1：请求的元素数量大于或等于集合内的元素数量：只需返回整个集合。
     * */
    if (count >= size) {
        /* We just return the entire set 
         *
         * 我们只归还整套
         * */
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,SET_OP_UNION);

        /* Delete the set as it is now empty 
         *
         * 删除集合，因为它现在为空
         * */
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);

        /* Propagate this command as a DEL operation 
         *
         * 将此命令作为DEL操作传播
         * */
        rewriteClientCommandVector(c,2,shared.del,c->argv[1]);
        signalModifiedKey(c,c->db,c->argv[1]);
        server.dirty++;
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SREM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. 
     *
     * 情况2和3需要将SPOP作为一组SREM命令进行复制。准备我们的复制论证向量。同
     * 时发送两个代码路径共用的数组长度。
     * */
    robj *propargv[3];
    propargv[0] = createStringObject("SREM",4);
    propargv[1] = c->argv[1];
    addReplySetLen(c,count);

    /* Common iteration vars. 
     *
     * 常见迭代变量。
     * */
    sds sdsele;
    robj *objele;
    int encoding;
    int64_t llele;
    unsigned long remaining = size-count; /* Elements left after SPOP. 
                                           *
                                           * SPOP之后留下的元素。
                                           * */

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set. Also we are sure that count < size.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. 
     *
     * 如果我们在这里，请求的元素数量小于集合内的元素数量。此外，我们确信count＜size。
     * 使用两种不同的策略。情况2：与集合大小相比，要返回的元素数量较小。我们可以提取随机元素并将它们返回到集合中。
     * */
    if (remaining*SPOP_MOVE_STRATEGY_MUL > count) {
        while(count--) {
            /* Emit and remove. 
             *
             * 发射并移除。
             * */
            encoding = setTypeRandomElement(set,&sdsele,&llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                addReplyBulkLongLong(c,llele);
                objele = createStringObjectFromLongLong(llele);
                set->ptr = intsetRemove(set->ptr,llele,NULL);
            } else {
                addReplyBulkCBuffer(c,sdsele,sdslen(sdsele));
                objele = createStringObject(sdsele,sdslen(sdsele));
                setTypeRemove(set,sdsele);
            }

            /* Replicate/AOF this command as an SREM operation 
             *
             * 将此命令复制/AOF作为SREM操作
             * */
            propargv[2] = objele;
            alsoPropagate(server.sremCommand,c->db->id,propargv,3,
                PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(objele);
        }
    } else {
    /* CASE 3: The number of elements to return is very big, approaching
     * the size of the set itself. After some time extracting random elements
     * from such a set becomes computationally expensive, so we use
     * a different strategy, we extract random elements that we don't
     * want to return (the elements that will remain part of the set),
     * creating a new set as we do this (that will be stored as the original
     * set). Then we return the elements left in the original set and
     * release it. 
     *
     * 情况3：返回的元素数量非常大，接近集合本身的大小。在一段时间后，从这样的集合中提
     * 取随机元素在计算上变得昂贵，所以我们使用不同的策略，提取我们不想返回的随机元素（
     * 仍将是集合的一部分的元素），在这样做的时候创建一个新的集合（将作为原始集合存储）。
     * 然后我们返回原始集合中剩下的元素并释放它。
     * */
        robj *newset = NULL;

        /* Create a new set with just the remaining elements. 
         *
         * 只使用剩下的元素创建一个新集合。
         * */
        while(remaining--) {
            encoding = setTypeRandomElement(set,&sdsele,&llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsdup(sdsele);
            }
            if (!newset) newset = setTypeCreate(sdsele);
            setTypeAdd(newset,sdsele);
            setTypeRemove(set,sdsele);
            sdsfree(sdsele);
        }

        /* Transfer the old set to the client. 
         *
         * 将旧集转移到客户端。
         * */
        setTypeIterator *si;
        si = setTypeInitIterator(set);
        while((encoding = setTypeNext(si,&sdsele,&llele)) != -1) {
            if (encoding == OBJ_ENCODING_INTSET) {
                addReplyBulkLongLong(c,llele);
                objele = createStringObjectFromLongLong(llele);
            } else {
                addReplyBulkCBuffer(c,sdsele,sdslen(sdsele));
                objele = createStringObject(sdsele,sdslen(sdsele));
            }

            /* Replicate/AOF this command as an SREM operation 
             *
             * 将此命令复制/AOF作为SREM操作
             * */
            propargv[2] = objele;
            alsoPropagate(server.sremCommand,c->db->id,propargv,3,
                PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(objele);
        }
        setTypeReleaseIterator(si);

        /* Assign the new set as the key value. 
         *
         * 将新集合指定为键值。
         * */
        dbOverwrite(c->db,c->argv[1],newset);
    }

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. 
     *
     * 即使我们增加了脏计数器，也不要传播命令本身。我们不想传播SPOP命令，因为我们使
     * 用alsoPropagate（）API将该命令传播为一组SREM操作。
     * */
    decrRefCount(propargv[0]);
    preventCommandPropagation(c);
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;
}

/*
 * T = O(N)
 */
void spopCommand(client *c) {
    robj *set, *ele, *aux;
    sds sdsele;
    int64_t llele;
    int encoding;

    if (c->argc == 3) {
        spopWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set 
     *
     * 确保输入名称的键存在，并且它的类型确实是一个集合
     * */
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
         == NULL || checkType(c,set,OBJ_SET)) return;

    /* Get a random element from the set 
     *
     * 从集合中获取随机元素
     * */
    encoding = setTypeRandomElement(set,&sdsele,&llele);

    /* Remove the element from the set 
     *
     * 从集合中移除元素
     * */
    // 根据返回值判断那个指针保存了元素
    if (encoding == OBJ_ENCODING_INTSET) {
        // 复制元素作为返回值
        ele = createStringObjectFromLongLong(llele);
        // 删除 intset 中的元素, O(N)
        set->ptr = intsetRemove(set->ptr,llele,NULL);
    } else {
        ele = createStringObject(sdsele,sdslen(sdsele));
        // 删除字典中原有的元素
        // O(1)
        setTypeRemove(set,ele->ptr);
    }

    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);

    /* Replicate/AOF this command as an SREM operation 
     *
     * 将此命令复制/AOF作为SREM操作
     * */
    aux = createStringObject("SREM",4);
    rewriteClientCommandVector(c,3,aux,c->argv[1],ele);
    decrRefCount(aux);

    /* Add the element to the reply 
     *
     * 将元素添加到回复中
     * */
    addReplyBulk(c,ele);
    decrRefCount(ele);

    /* Delete the set if it's empty 
     *
     * 如果集合为空，则删除该集合
     * */
    if (setTypeSize(set) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Set has been modified 
     *
     * 集合已修改
     * */
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. 
 *
 * 处理“SRANDMEMBER键＜count＞”变体。该命令的正常版本由srandmemberCommand（）函数本身处理。
 * */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. 
 *
 * 与我们不使用“删除元素”策略所要求的大小相比，该集应该大多少倍？请稍后阅读实现中
 * 的内容以了解更多信息。
 * */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3


/*
 * 带 count 参数的 SRANDMEMBER 命令的实现
 *
 * T = O(N^2)
 */
void srandmemberWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    int uniq = 1;
    robj *set;
    sds ele;
    int64_t llele;
    int encoding;

    dict *d;

    // 获取 count 参数
    if (getLongFromObjectOrReply(c,c->argv[2],&l,NULL) != C_OK) return;
    if (l<-LONG_MAX) {
        addReplyError(c, "value is out of range");
        return;
    }
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). 
         *
         * 负计数意味着：多次返回相同的元素（即每次提取后不要删除提取的元素）。
         * */
        count = -l;
        uniq = 0;
    }

    // 查找对象，检查类型, O(1)
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyset[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. 
     *
     * 如果计数为零，请尽快送达，以避免以后出现特殊情况。
     * */
    if (count == 0) {
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. 
     *
     * 情况1：计数为负数，因此提取方法只是：每次“返回N个随机元素”对整个集合进行采样
     * 。这种情况是琐碎的，并且可以在没有辅助数据结构的情况下提供服务。
     * */
    if (!uniq) {
        addReplySetLen(c,count);
        // O(N^2)
        while(count--) {
            // O(N)
            encoding = setTypeRandomElement(set,&ele,&llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c,ele,sdslen(ele));
            }
            if (c->flags & CLIENT_CLOSE_ASAP)
                break;
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. 
     *
     * 情况2：请求的元素数量大于集合内的元素数量：简单地返回整个集合。
     * */
    if (count >= size) {
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,SET_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. 
     *
     * 对于案例3和案例4，我们需要一个辅助字典。
     * */
    d = dictCreate(&objectKeyPointerValueDictType,NULL);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient. 
     *
     * 情况3：集合内的元素数不大于SRANDMEMBER_SUB_STRATEGY_M
     * UL乘以请求的元素数。在这种情况下，我们从头开始创建一个包含所有元素的集合，并减
     * 去随机元素以达到所需的元素数量。
     *
     * 之所以这样做，是因为如果请求的元素数量只比集合中
     * 的元素数量少一点，那么在CASE 3中使用的自然方法效率很低。
     * */
    // 如果 count * SRANDMEMBER_SUB_STRATEGY_MUL 比集合的大小(size)还大
    // 那么遍历集合，将集合的元素保存到另一个新集合里
    // 然后从新集合里随机删除元素，直到剩下 count 个元素为止，最后删除新集合
    // 这个比直接计算 count 个随机元素效率更高
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. 
         *
         * 将所有元素添加到临时字典中。
         * */
        si = setTypeInitIterator(set);
        while((encoding = setTypeNext(si,&ele,&llele)) != -1) {
            int retval = DICT_ERR;

            if (encoding == OBJ_ENCODING_INTSET) {
                retval = dictAdd(d,createStringObjectFromLongLong(llele),NULL);
            } else {
                retval = dictAdd(d,createStringObject(ele,sdslen(ele)),NULL);
            }
            serverAssert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        serverAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. 
         *
         * 删除随机元素以达到正确的计数。
         * */
        while(size > count) {
            dictEntry *de;

            de = dictGetRandomKey(d);
            dictDelete(d,dictGetKey(de));
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. 
     *
     * 情况4：与要求的元素数量相比，我们有一个很大的集合。在这种情况下，我们可以简单地
     * 从集合中获得随机元素，并将其添加到临时集合中，试图最终获得足够的唯一元素以达到指
     * 定的计数。
     * */
    // 集合的基数比 count 要小，这种情况下，随机返回 count 个元素即可
    else {
        unsigned long added = 0;
        robj *objele;

        // O(N^2)
        while(added < count) {

            // O(N)
            encoding = setTypeRandomElement(set,&ele,&llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                objele = createStringObjectFromLongLong(llele);
            } else {
                objele = createStringObject(ele,sdslen(ele));
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. 
             *
             * 尝试将该对象添加到词典中。如果它已经存在，则释放它，否则增加结果字典中的对象数量
             * */
            // O(1)
            if (dictAdd(d,objele,NULL) == DICT_OK)
                added++;
            else
                decrRefCount(objele);
        }
    }

    /* CASE 3 & 4: send the result to the user. 
     *
     * 情况3和4：将结果发送给用户。
     * */
    // O(N)
    {
        dictIterator *di;
        dictEntry *de;

        addReplySetLen(c,count);
        di = dictGetIterator(d);
        while((de = dictNext(di)) != NULL)
            addReplyBulk(c,dictGetKey(de));
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

/*
 * 返回集合中的单个随机元素
 *
 * T = O(N)
 */
void srandmemberCommand(client *c) {
    robj *set;
    sds ele;
    int64_t llele;
    int encoding;

    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    // 获取/创建对象，并检查它是否集合
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;

    // 获取随机元素
    // O(N)
    encoding = setTypeRandomElement(set,&ele,&llele);
    if (encoding == OBJ_ENCODING_INTSET) {
        addReplyBulkLongLong(c,llele);
    } else {
        addReplyBulkCBuffer(c,ele,sdslen(ele));
    }
}

/*
 * 对比两个集合的基数
 *
 * T = O(1)
 */
int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    if (setTypeSize(*(robj**)s1) > setTypeSize(*(robj**)s2)) return 1;
    if (setTypeSize(*(robj**)s1) < setTypeSize(*(robj**)s2)) return -1;
    return 0;
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. 
 *
 * 这是由SDIFF使用的，在这种情况下，我们可以接收应该作为空集处理的NULL。
 * */
/*
 * 对比两个集合的基数是否相同
 *
 * T = O(1)
 */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = *(robj**)s1, *o2 = *(robj**)s2;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

/*
 * T = O(N^2 lg N)
 */
void sinterGenericCommand(client *c, robj **setkeys,
                          unsigned long setnum, robj *dstkey) {
    // 分配指针数组，数量等于 setnum
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    // 集合迭代器
    setTypeIterator *si;
    robj *dstset = NULL;
    sds elesds;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding, empty = 0;

    // 将所有集合对象指针保存到 sets 数组
    // O(N)
    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
        if (!setobj) {
            /* A NULL is considered an empty set 
             *
             * NULL被认为是一个空集
             * */
            empty += 1;
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Set intersection with an empty set always results in an empty set.
     * Return ASAP if there is an empty set. 
     *
     * 集合与空集的交集总是导致空集。如果有空集，请尽快返回。
     * */
    if (empty > 0) {
        zfree(sets);
        if (dstkey) {
            if (dbDelete(c->db,dstkey)) {
                signalModifiedKey(c,c->db,dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
                server.dirty++;
            }
            addReply(c,shared.czero);
        } else {
            addReply(c,shared.emptyset[c->resp]);
        }
        return;
    }

    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance 
     *
     * 从最小到最大对集合进行排序，这将提高我们算法的性能
     * */
    // 按集合的基数从小到大排序集合
    // O(N^2)
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length 
     *
     * 我们应该输出的第一件事是元素的总数。。。由于这是一个多批量写入，但在这个阶段，我
     * 们不知道交集集的大小，所以我们使用了一个技巧，将一个空对象附加到输出列表中，并保
     * 存指针，以便以后用合适的长度修改它
     * */
    if (!dstkey) {
        replylen = addReplyDeferredLen(c);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside 
         *
         * 如果我们有一个目标键来存储结果集，那么创建一个内部有空集的键
         * */
        dstset = createIntsetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded 
     *
     * 迭代第一个（最小的）集合的所有元素，并将该元素与所有其他集合进行测试，如果至少有
     * 一个集合不包括该元素，则该元素将被丢弃
     * */
    // 取出 sets[0] 的元素，和其他元素进行交集操作，
    // 只要某个集合有一个不包含 sets[0] 的元素，
    // 那么这个元素就不被包含在交集结果集里面

    // 创建迭代器
    si = setTypeInitIterator(sets[0]);
    // 遍历 sets[0] , O(N^2 lg N)
    while((encoding = setTypeNext(si,&elesds,&intobj)) != -1) {
        // 和其他集合做交集操作
        // O(N lg N)
        for (j = 1; j < setnum; j++) {
            // 跳过相同的集合
            if (sets[j] == sets[0]) continue;
            // sets[0] 是 intset 时。。。
            if (encoding == OBJ_ENCODING_INTSET) {
                /* intset with intset is simple... and fast 
                 *
                 * 带intset的intset很简单。。。而且速度很快
                 * */
                // O(lg N)
                if (sets[j]->encoding == OBJ_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,intobj))
                {
                    break;
                /* in order to compare an integer with an object we
                 * have to use the generic function, creating an object
                 * for this 
                 *
                 * 为了将整数与对象进行比较，我们必须使用泛型函数，为此创建一个对象
                 * */
                } else if (sets[j]->encoding == OBJ_ENCODING_HT) {
                    // sets[0] 是字典时。。。


                    elesds = sdsfromlonglong(intobj);
                    if (!setTypeIsMember(sets[j],elesds)) {
                        sdsfree(elesds);
                        break;
                    }
                    sdsfree(elesds);
                }
            } else if (encoding == OBJ_ENCODING_HT) {
                if (!setTypeIsMember(sets[j],elesds)) {
                    break;
                }
            }
        }

        /* Only take action when all sets contain the member 
         *
         * 仅当所有集合都包含该成员时才执行操作
         * */
        if (j == setnum) {
            // 没有 dstkey ，直接返回给输出
            if (!dstkey) {
                if (encoding == OBJ_ENCODING_HT)
                    addReplyBulkCBuffer(c,elesds,sdslen(elesds));
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
            } else {
                // 有 dstkey ，添加到 dstkey
                if (encoding == OBJ_ENCODING_INTSET) {
                    elesds = sdsfromlonglong(intobj);
                    setTypeAdd(dstset,elesds);
                    sdsfree(elesds);
                } else {
                    setTypeAdd(dstset,elesds);
                }
            }
        }
    }
    // 释放迭代器
    setTypeReleaseIterator(si);

    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. 
         *
         * 如果交集不是空集，则将结果集存储到目标中。
         * */
        // 用结果集合代替原有的 dstkey
        int deleted = dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,"sinterstore",
                dstkey,c->db->id);
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
            if (deleted)
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
        signalModifiedKey(c,c->db,dstkey);
        server.dirty++;
    } else {
        setDeferredSetLen(c,replylen,cardinality);
    }
    zfree(sets);
}

/* SINTER key [key ...] 
 *
 * SINTER键[键…]
 * */
void sinterCommand(client *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

/* SINTERSTORE destination key [key ...] 
 *
 * SINTERSTORE目标键[键…]
 * */
void sinterstoreCommand(client *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}

#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/*
 * T = O(N^2)
 */
void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op) {
    // 集合指针数组
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    // 集合迭代器
    setTypeIterator *si;
    robj *dstset = NULL;
    sds ele;
    int j, cardinality = 0;
    int diff_algo = 1;

    // 收集所有集合对象指针
    // O(N)
    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. 
     *
     * 选择要使用的DIFF算法。算法1是O（N*M），其中N是元素第一集合的大小，M是
     * 集合的总数。算法2是O（N），其中N是所有集合中元素的总数。我们在这里计算当前输入的最佳匹配。
     * */
    // 根据集合的大小选择恰当的算法
    if (op == SET_OP_DIFF && sets[0]) {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            // 计算 N * M
            algo_one_work += setTypeSize(sets[0]);
            // 计算所有集合的总大小
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. 
         *
         * 如果存在共同的元素，则算法1具有更好的恒定时间并且执行更少的操作。给它一些优势。
         * */
        algo_one_work /= 2;
        // 选择算法
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. 
             *
             * 对于算法1，最好通过减小大小来对集合进行减法排序，这样我们更有可能尽快找到重复的
             * 元素。
             * */
            qsort(sets+1,setnum-1,sizeof(robj*),
                qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key
     *
     * 我们需要一个临时集对象来存储我们的并集。如果dstkey不为NULL（也就是说，
     * 我们在SUNIONSTORE操作中），那么这个set对象将是要设置到目标键中的
     * 结果对象
     * */
    // 用于保存 union 结果的对象
    // 如果 dstkey 不为空，将来这个值就会被保存为 dstkey
    dstset = createIntsetObject();

    // union 操作
    if (op == SET_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. 
         *
         * 并集是琐碎的，只需将每个集合的每个元素都添加到临时集合中即可。
         * */
        // 遍历所有集合的所有元素，将它们添加到 dstset 上去
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets 
                                     *
                                     * 不存在的键就像空集
                                     * */

            si = setTypeInitIterator(sets[j]);
            while((ele = setTypeNextObject(si)) != NULL) {
                // 已有的元素不会被计数
                if (setTypeAdd(dstset,ele)) cardinality++;
                sdsfree(ele);
            }
            setTypeReleaseIterator(si);
        }
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. 
         *
         * DIFF算法1：我们通过迭代第一个集合的所有元素来执行DIFF，并且只有在元素不
         * 存在于所有其他集合中时才将其添加到目标集合。这样，我们最多执行N*M次运算，其中
         * N是第一个集合的大小，M是集合的数量。
         * */
        // 遍历 sets[0] ，对于其中的每个元素 elem ，
        // 只有 elem 不存在与其他任何集合时，才将它添加到 dstset
        si = setTypeInitIterator(sets[0]);
        while((ele = setTypeNextObject(si)) != NULL) {
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; /* no key is an empty set. 
                                         *
                                         * 没有键是空集。
                                         * */
                if (sets[j] == sets[0]) break; /* same set! 
                                                *
                                                * 同一套！
                                                * */
                if (setTypeIsMember(sets[j],ele)) break;
            }
            if (j == setnum) {
                /* There is no other set with this element. Add it. 
                 *
                 * 没有其他集合包含此元素。添加它。
                 * */
                setTypeAdd(dstset,ele);
                cardinality++;
            }
            sdsfree(ele);
        }
        setTypeReleaseIterator(si);
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 2) {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. 
         *
         * DIFF算法2：将第一个集合的所有元素添加到辅助集合。然后从中移除所有下一个集合
         * 的所有元素。这是O（N），其中N是每个集合中所有元素的和。
         * */
        // 将 sets[0] 的所有元素保存到 dstset 对象中
        // 遍历其余的所有集合，如果被遍历集合和 dstset 有相同的元素，
        // 那么从 dstkey 中删除那个元素
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets 
                                     *
                                     * 不存在的键就像空集
                                     * */

            si = setTypeInitIterator(sets[j]);
            while((ele = setTypeNextObject(si)) != NULL) {
                if (j == 0) {
                    // 添加所有元素到 sets[0]
                    if (setTypeAdd(dstset,ele)) cardinality++;
                } else {
                    // 如果在其他集合碰见相同的元素，那么删除它
                    if (setTypeRemove(dstset,ele)) cardinality--;
                }
                sdsfree(ele);
            }
            setTypeReleaseIterator(si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. 
             *
             * 如果结果集为空，则退出，因为任何额外的元素删除都不会产生任何效果。
             * */
            if (cardinality == 0) break;
        }
    }

    /* Output the content of the resulting set, if not in STORE mode 
     *
     * 如果未处于STORE模式，则输出结果集的内容
     * */
    // 没有 dstkey ，直接输出结果
    if (!dstkey) {
        addReplySetLen(c,cardinality);
        si = setTypeInitIterator(dstset);
        while((ele = setTypeNextObject(si)) != NULL) {
            addReplyBulkCBuffer(c,ele,sdslen(ele));
            sdsfree(ele);
        }
        setTypeReleaseIterator(si);
        server.lazyfree_lazy_server_del ? freeObjAsync(dstset) :
                                          decrRefCount(dstset);
    } else {

        // 有 dstkey ，用 dstset 替换原来 dstkey 的对象
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside 
         *
         * 如果我们有一个目标键来存储结果集，那么创建这个键并将结果集放在里面
         * */
        int deleted = dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,
                op == SET_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->db->id);
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
            if (deleted)
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
        signalModifiedKey(c,c->db,dstkey);
        server.dirty++;
    }
    zfree(sets);
}

/* SUNION key [key ...] 
 *
 * SUNION键[键…]
 * */
void sunionCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_UNION);
}

/* SUNIONSTORE destination key [key ...] 
 *
 * SUNIONSTORE目标键[键…]
 * */
void sunionstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_UNION);
}

/* SDIFF key [key ...] 
 *
 * SDIFF键[键…]
 * */
void sdiffCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_DIFF);
}

/* SDIFFSTORE destination key [key ...] 
 *
 * SDIFSTORE目标键[键…]
 * */
void sdiffstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_DIFF);
}

void sscanCommand(client *c) {
    robj *set;
    unsigned long cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,set,OBJ_SET)) return;
    scanGenericCommand(c,set,cursor);
}
