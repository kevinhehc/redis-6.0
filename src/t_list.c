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

#define LIST_MAX_ITEM_SIZE ((1ull<<32)-1024)

/*-----------------------------------------------------------------------------
 * List API
 * 列表API
 *----------------------------------------------------------------------------*/

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. 
 *
 * 函数将元素推送到指定的列表对象“subject”，位于“where”指定的头或尾
 * 位置。调用方不需要增加“value”的refcount，因为如果需要，函数会处理
 * 它。
 * */
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        value = getDecodedObject(value);
        size_t len = sdslen(value->ptr);
        quicklistPush(subject->ptr, value->ptr, len, pos);
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, unsigned int sz) {
    return createStringObject((char*)data,sz);
}

/*
 * 多态 pop 对象
 *
 * T = O(1)
 */
robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;

    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

/*
 * 多态列表长度获取函数
 *
 * T = O(N)
 */
unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. 
 *
 * 在指定索引处初始化迭代器。
 * */
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. 
     *
     * LIST_HEAD表示从TAIL开始并将*移动到shead。LIST_TAIL表
     * 示从HEAD开始，向尾部移动*。
     * */
    int iter_direction =
        direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else {
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. 
 *
 * 清理迭代器。
 * */
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li->iter);
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. 
 *
 * 在提供的条目结构中存储指向当前条目的指针，并推进迭代器的位置。当当前条目实际上是
 * 条目时，返回1，否则返回0。
 * */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating 
     *
     * 迭代时防止转换
     * */
    serverAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistNext(li->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. 
 *
 * 在迭代器的当前位置返回entry或NULL。
 * */
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {
            value = createStringObject((char *)entry->entry.value,
                                       entry->entry.sz);
        } else {
            value = createStringObjectFromLongLong(entry->entry.longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

/*
 * 多态插入函数
 *
 * T = O(N^2)
 */
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        value = getDecodedObject(value);
        sds str = value->ptr;
        size_t len = sdslen(str);
        if (where == LIST_TAIL) {
            quicklistInsertAfter((quicklist *)entry->entry.quicklist,
                                 &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore((quicklist *)entry->entry.quicklist,
                                  &entry->entry, str, len);
        }
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. 
 *
 * 将给定对象与当前位置的条目进行比较。
 * */
int listTypeEqual(listTypeEntry *entry, robj *o) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
        return quicklistCompare(entry->entry.zi,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. 
 *
 * 删除指向的元素。
 * */
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelEntry(iter->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Create a quicklist from a single ziplist 
 *
 * 从单个快捷列表创建快速列表
 * */
void listTypeConvert(robj *subject, int enc) {
    serverAssertWithInfo(NULL,subject,subject->type==OBJ_LIST);
    serverAssertWithInfo(NULL,subject,subject->encoding==OBJ_ENCODING_ZIPLIST);

    if (enc == OBJ_ENCODING_QUICKLIST) {
        size_t zlen = server.list_max_ziplist_size;
        int depth = server.list_compress_depth;
        subject->ptr = quicklistCreateFromZiplist(zlen, depth, subject->ptr);
        subject->encoding = OBJ_ENCODING_QUICKLIST;
    } else {
        serverPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 * 列表命令
 *----------------------------------------------------------------------------*/
/*
 * [LR]PUSH 命令的实现
 *
 * T = O(N^3)
 */
void pushGenericCommand(client *c, int where) {
    int j, pushed = 0;

    for (j = 2; j < c->argc; j++) {
        if (sdslen(c->argv[j]->ptr) > LIST_MAX_ITEM_SIZE) {
            addReplyError(c, "Element too large");
            return;
        }
    }

    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);

    if (lobj && lobj->type != OBJ_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    for (j = 2; j < c->argc; j++) {
        if (!lobj) {
            lobj = createQuicklistObject();
            quicklistSetOptions(lobj->ptr, server.list_max_ziplist_size,
                                server.list_compress_depth);
            dbAdd(c->db,c->argv[1],lobj);
        }
        listTypePush(lobj,c->argv[j],where);
        pushed++;
    }
    addReplyLongLong(c, (lobj ? listTypeLength(lobj) : 0));
    if (pushed) {
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";

        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;
}

/*
 * LPUSH 命令的实现
 *
 * T = O(N^3)
 */
void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD);
}

/*
 * RPUSH 命令的实现
 *
 * T = O(N^3)
 */
void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL);
}

void pushxGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *subject;

    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    for (j = 2; j < c->argc; j++) {
        listTypePush(subject,c->argv[j],where);
        pushed++;
    }

    addReplyLongLong(c,listTypeLength(subject));

    if (pushed) {
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;
}

/*
 * LPUSHX 命令的实现
 *
 * T = O(N^2)
 */
void lpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_HEAD);
}

/*
 * RPUSHX 命令的实现
 *
 * T = O(N^2)
 */
void rpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_TAIL);
}

/*
 * LINSERT 命令的实现
 *
 * T = O(N^2)
 */
void linsertCommand(client *c) {
    int where;
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        where = LIST_TAIL;
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        where = LIST_HEAD;
    } else {
        addReply(c,shared.syntaxerr);
        return;
    }

    if (sdslen(c->argv[4]->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    /* Seek pivot from head to tail 
     *
     * 从头到尾查找枢轴
     * */
    iter = listTypeInitIterator(subject,0,LIST_TAIL);
    while (listTypeNext(iter,&entry)) {
        if (listTypeEqual(&entry,c->argv[3])) {
            listTypeInsert(&entry,c->argv[4],where);
            inserted = 1;
            break;
        }
    }
    listTypeReleaseIterator(iter);

    if (inserted) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"linsert",
                            c->argv[1],c->db->id);
        server.dirty++;
    } else {
        /* Notify client of a failed insert 
         *
         * 插入失败时通知客户端
         * */
        addReplyLongLong(c,-1);
        return;
    }

    addReplyLongLong(c,listTypeLength(subject));
}

/*
 * LLEN 命令的实现
 *
 * T = O(N)
 */
void llenCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    addReplyLongLong(c,listTypeLength(o));
}

/*
 * LINDEX 命令的实现
 *
 * T = O(N)
 */
void lindexCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = NULL;

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistEntry entry;
        if (quicklistIndex(o->ptr, index, &entry)) {
            if (entry.value) {
                value = createStringObject((char*)entry.value,entry.sz);
            } else {
                value = createStringObjectFromLongLong(entry.longval);
            }
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReplyNull(c);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/*
 * LSET 命令的实现
 *
 * T = O(N^2)
 */
void lsetCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->argv[3];

    if (sdslen(value->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        int replaced = quicklistReplaceAtIndex(ql, index,
                                               value->ptr, sdslen(value->ptr));
        if (!replaced) {
            addReply(c,shared.outofrangeerr);
        } else {
            addReply(c,shared.ok);
            signalModifiedKey(c,c->db,c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/*
 * 从列表中弹出一个元素，如果弹出元素之后列表为空，那么删除它
 *
 * T = O(1)
 */
void popGenericCommand(client *c, int where) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;

    robj *value = listTypePop(o,where);
    if (value == NULL) {
        addReplyNull(c);
    } else {
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";

        addReplyBulk(c,value);
        decrRefCount(value);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
        if (listTypeLength(o) == 0) {
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                c->argv[1],c->db->id);
            dbDelete(c->db,c->argv[1]);
        }
        signalModifiedKey(c,c->db,c->argv[1]);
        server.dirty++;
    }
}

/*
 * LPOP 命令的实现
 *
 * T = O(1)
 */
void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

/*
 * RPOP 命令的实现
 *
 * T = O(1)
 */
void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

/*
 * T = O(N)
 */
void lrangeCommand(client *c) {
    robj *o;
    long start, end, llen, rangelen;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray)) == NULL
         || checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes 
     *
     * 转换负索引
     * */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. 
     *
     * 不变量：start>=0，因此当end＜0时，此测试将为true。当开始>结束或开始>=长度时，范围为空。
     * */
    if (start > end || start >= llen) {
        addReply(c,shared.emptyarray);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply 
     *
     * 以多批量回复的形式返回结果
     * */
    addReplyArrayLen(c,rangelen);
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        listTypeIterator *iter = listTypeInitIterator(o, start, LIST_TAIL);

        while(rangelen--) {
            listTypeEntry entry;
            listTypeNext(iter, &entry);
            quicklistEntry *qe = &entry.entry;
            if (qe->value) {
                addReplyBulkCBuffer(c,qe->value,qe->sz);
            } else {
                addReplyBulkLongLong(c,qe->longval);
            }
        }
        listTypeReleaseIterator(iter);
    } else {
        serverPanic("List encoding is not QUICKLIST!");
    }
}

/*
 * T = O(N^2)
 */
void ltrimCommand(client *c) {
    robj *o;
    long start, end, llen, ltrim, rtrim;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes 
     *
     * 转换负索引
     * */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. 
     *
     * 不变量：start>=0，因此当end＜0时，此测试将为true。当开始>结束或开始>=长度时，范围为空。
     * */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list 
         *
         * 空列表中的开始或开始>结束结果超出范围
         * */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim 
     *
     * 删除列表元素以执行修剪
     * */
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(o->ptr,0,ltrim);
        quicklistDelRange(o->ptr,-rtrim,rtrim);
    } else {
        serverPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

/* LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len]
 *
 * The "rank" is the position of the match, so if it is 1, the first match
 * is returned, if it is 2 the second match is returned and so forth.
 * It is 1 by default. If negative has the same meaning but the search is
 * performed starting from the end of the list.
 *
 * If COUNT is given, instead of returning the single element, a list of
 * all the matching elements up to "num-matches" are returned. COUNT can
 * be combiled with RANK in order to returning only the element starting
 * from the Nth. If COUNT is zero, all the matching elements are returned.
 *
 * MAXLEN tells the command to scan a max of len elements. If zero (the
 * default), all the elements in the list are scanned if needed.
 *
 * The returned elements indexes are always referring to what LINDEX
 * would return. So first element from head is 0, and so forth. 
 *
 * LPOS键元素[RANK RANK][COUNT num matches][MAXLEN len]
 *
 * “RANK”是匹配的位置，因此如果为1，则返回第一个匹配，如果为2，则返回第二个匹配，依此类推。默认情况下为1。
 * 如果否定具有相同的含义，但搜索是从列表的末尾开始执行的。
 *
 * 如果给定了COUNT，则不返回单个元素，而是返回所有匹配元素的列表，最多返回“num个匹配”。
 * COUNT可以与RANK组合，以便仅返回从第N个开始的元素。如果COUNT为零，则返回所有匹配的元素。
 *
 * MAXLEN告诉扫描最大len元素的命令。如果为零（默认值），则根据需要扫描列表中的所有元素。
 *
 * 返回的元素索引总是指LINDEX将返回的内容。所以从头开始的第一个元素是0，依此类推。
 * */
void lposCommand(client *c) {
    robj *o, *ele;
    ele = c->argv[2];
    int direction = LIST_TAIL;
    long rank = 1, count = -1, maxlen = 0; /* Count -1: option not given. 
                                            *
                                            * 计数-1：未给定选项。
                                            * */

    if (sdslen(ele->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

    /* Parse the optional arguments. 
     *
     * 分析可选参数。
     * */
    for (int j = 3; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1)-j;

        if (!strcasecmp(opt,"RANK") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &rank, NULL) != C_OK)
                return;
            if (rank == 0) {
                addReplyError(c,"RANK can't be zero: use 1 to start from "
                                "the first match, 2 from the second, ...");
                return;
            }
        } else if (!strcasecmp(opt,"COUNT") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &count, NULL) != C_OK)
                return;
            if (count < 0) {
                addReplyError(c,"COUNT can't be negative");
                return;
            }
        } else if (!strcasecmp(opt,"MAXLEN") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &maxlen, NULL) != C_OK)
                return;
            if (maxlen < 0) {
                addReplyError(c,"MAXLEN can't be negative");
                return;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* A negative rank means start from the tail. 
     *
     * 负排名意味着从尾部开始。
     * */
    if (rank < 0) {
        rank = -rank;
        direction = LIST_HEAD;
    }

    /* We return NULL or an empty array if there is no such key (or
     * if we find no matches, depending on the presence of the COUNT option. 
     *
     * 如果没有这样的键（或者没有找到匹配项，这取决于COUNT选项的存在），我们将返回NULL或空数组。
     * */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        if (count != -1)
            addReply(c,shared.emptyarray);
        else
            addReply(c,shared.null[c->resp]);
        return;
    }
    if (checkType(c,o,OBJ_LIST)) return;

    /* If we got the COUNT option, prepare to emit an array. 
     *
     * 如果我们得到了COUNT选项，准备发射一个数组。
     * */
    void *arraylenptr = NULL;
    if (count != -1) arraylenptr = addReplyDeferredLen(c);

    /* Seek the element. 
     *
     * 查找元素。
     * */
    listTypeIterator *li;
    li = listTypeInitIterator(o,direction == LIST_HEAD ? -1 : 0,direction);
    listTypeEntry entry;
    long llen = listTypeLength(o);
    long index = 0, matches = 0, matchindex = -1, arraylen = 0;
    while (listTypeNext(li,&entry) && (maxlen == 0 || index < maxlen)) {
        if (listTypeEqual(&entry,ele)) {
            matches++;
            matchindex = (direction == LIST_TAIL) ? index : llen - index - 1;
            if (matches >= rank) {
                if (arraylenptr) {
                    arraylen++;
                    addReplyLongLong(c,matchindex);
                    if (count && matches-rank+1 >= count) break;
                } else {
                    break;
                }
            }
        }
        index++;
        matchindex = -1; /* Remember if we exit the loop without a match. 
                          *
                          * 请记住，如果我们在没有匹配的情况下退出循环。
                          * */
    }
    listTypeReleaseIterator(li);

    /* Reply to the client. Note that arraylenptr is not NULL only if
     * the COUNT option was selected. 
     *
     * 回复客户。请注意，只有在选择了COUNT选项时，arraylenptr才不是NULL。
     * */
    if (arraylenptr != NULL) {
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else {
        if (matchindex != -1)
            addReplyLongLong(c,matchindex);
        else
            addReply(c,shared.null[c->resp]);
    }
}

/*
 * T = O(N^3)
 */
void lremCommand(client *c) {
    robj *subject, *obj;
    obj = c->argv[3];
    long toremove;
    long removed = 0;

    if (sdslen(obj->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != C_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,OBJ_LIST)) return;

    listTypeIterator *li;
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,LIST_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,LIST_TAIL);
    }

    listTypeEntry entry;
    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(li, &entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    if (removed) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"lrem",c->argv[1],c->db->id);
    }

    if (listTypeLength(subject) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    addReplyLongLong(c,removed);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 
 *
 * 这是这个命令的语义
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 * 这个想法是能够以可靠的方式从列表中获取元素，因为
 * 该元素不仅被返回，而且还被推送到另一个列表中。这个命令最初是由Ezra Zygm
 * untowicz提出的。
 * */

/*
 * 将 value 添加到 dstkey 列表里
 * 如果 dstkey 为空，那么创建一个新列表，然后执行添加动作
 *
 * T = O(N^2)
 */
void rpoplpushHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist 
     *
     * 如果键不存在，则创建列表
     * */
    if (!dstobj) {
        dstobj = createQuicklistObject();
        quicklistSetOptions(dstobj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        dbAdd(c->db,dstkey,dstobj);
    }
    signalModifiedKey(c,c->db,dstkey);
    listTypePush(dstobj,value,LIST_HEAD);
    notifyKeyspaceEvent(NOTIFY_LIST,"lpush",dstkey,c->db->id);
    /* Always send the pushed value to the client. 
     *
     * 始终将推送的值发送给客户端。
     * */
    addReplyBulk(c,value);
}

/*
 * T = O(N^2)
 */
void rpoplpushCommand(client *c) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,sobj,OBJ_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. 
         *
         * 这可能只有在加载非常旧的RDB文件之后才会发生。最新版本的Redis删除空列表的键。
         * */
        addReplyNull(c);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        if (dobj && checkType(c,dobj,OBJ_LIST)) return;
        value = listTypePop(sobj,LIST_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). 
         *
         * 我们保存了触摸过的键，并对其进行了保护，因为rpoplpushHandlePu
         * sh可能会更改客户端命令参数向量（目前不会）。
         * */
        incrRefCount(touchedkey);
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented 
         *
         * listTypePop返回一个refcount递增的对象
         * */
        decrRefCount(value);

        /* Delete the source list when it is empty 
         *
         * 删除空的源列表
         * */
        notifyKeyspaceEvent(NOTIFY_LIST,"rpop",touchedkey,c->db->id);
        if (listTypeLength(sobj) == 0) {
            dbDelete(c->db,touchedkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        signalModifiedKey(c,c->db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
        if (c->cmd->proc == brpoplpushCommand) {
            rewriteClientCommandVector(c,3,shared.rpoplpush,c->argv[1],c->argv[2]);
        }
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 * 阻止POP操作
 *----------------------------------------------------------------------------*/

/* This is a helper function for handleClientsBlockedOnKeys(). Its work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped from the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns C_OK if we are able to serve the client, otherwise
 * C_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. 
 *
 * 这是handleClientsBlockedOnKeys（）的辅助函数。它的工作
 * 是为在指定的“db”上下文中的“key”上被阻止的特定客户端（接收器）提供服务，
 * 执行以下操作：
 *
 * 1）为客户端提供“value”元素。
 * 2） 如果dstkey不为NULL（我们提供的是BRPOPLPUSH），还将“value”元素推送到目标列表（命令的LPUSH一侧）。
 * 3） 将生成的BRPOP、BLPOP和其他LPUSH（如果有的话）传播到AOF和复制通道中。
 *
 * 参数“where”是LIST_TAIL或LIST_HEAD，指示“value”元素是从头部（BLPOP）还是尾部（BRPOP）弹出的，以便我们可以正确传播命令。
 *
 * 如果我们能够为客户端提供服务，则函数返回C_OK，否则返回C_ERR以向调用方发出信号，表明由于客户端没有得到服务，
 * 列表POP操作应该撤消：这种情况只发生在BRPOPLPUSH，因为它的类型错误，所以无法将值推送到目标键。
 * */
int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. 
         *
         * 传播[LR]POP操作。
         * */
        argv[0] = (where == LIST_HEAD) ? shared.lpop :
                                          shared.rpop;
        argv[1] = key;
        propagate((where == LIST_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        addReplyArrayLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);

        /* Notify event. 
         *
         * 通知事件。
         * */
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
        notifyKeyspaceEvent(NOTIFY_LIST,event,key,receiver->db->id);
    } else {
        /* BRPOPLPUSH */
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the RPOPLPUSH operation. 
             *
             * 传播RPOPLPUSH操作。
             * */
            argv[0] = shared.rpoplpush;
            argv[1] = key;
            argv[2] = dstkey;
            propagate(server.rpoplpushCommand,
                db->id,argv,3,
                PROPAGATE_AOF|
                PROPAGATE_REPL);

            /* Notify event ("lpush" was notified by rpoplpushHandlePush). 
             *
             * Notify事件（rpoplpushHandlePush通知了“lpush”）。
             * */
            notifyKeyspaceEvent(NOTIFY_LIST,"rpop",key,receiver->db->id);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. 
             *
             * 由于目标类型错误，BRPOPLPUSH失败。
             * */
            return C_ERR;
        }
    }
    return C_OK;
}

/* Blocking RPOP/LPOP 
 *
 * 阻止RPOP/LOP
 * */
/*
 * BLPOP/BRPOP 的底层实现
 */
void blockingPopGenericCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    // 获取 timeout 参数
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;

    // 遍历所有 key
    // 如果找到第一个不为空的列表对象，那么对它进行 POP ，然后返回
    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != OBJ_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                // key 为非空列表？
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a normal [LR]POP. 
                     *
                     * 非空列表，这就像一个正常的[LR]POP。
                     * */
                    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
                    // 非空列表，执行普通的 [LR]POP
                    robj *value = listTypePop(o,where);
                    serverAssert(value != NULL);

                    addReplyArrayLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
                    decrRefCount(value);
                    notifyKeyspaceEvent(NOTIFY_LIST,event,
                                        c->argv[j],c->db->id);
                    // 删除空列表
                    if (listTypeLength(o) == 0) {
                        dbDelete(c->db,c->argv[j]);
                        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                            c->argv[j],c->db->id);
                    }
                    signalModifiedKey(c,c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. 
                     *
                     * 将其复制为[LR]POP，而不是B[LR]POP。
                     * */
                    rewriteClientCommandVector(c,2,
                        (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). 
     *
     * 如果我们在MULTI/EXEC中，并且列表为空，那么我们唯一能做的就是将其视为超时（即使超时为0）。
     * */
    // 如果命令正被事务包含，那么只能返回等待超时
    // （阻塞不能用在事务里，因为这回造成事务一直等待下去）
    if (c->flags & CLIENT_MULTI) {
        addReplyNullArray(c);
        return;
    }

    /* If the keys do not exist we must block 
     *
     * 如果键不存在，我们必须阻止
     * */
    // 所有给定 key 都为空，进行 block
    blockForKeys(c,BLOCKED_LIST,c->argv + 1,c->argc - 2,timeout,NULL,NULL);
}

void blpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_HEAD);
}

void brpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_TAIL);
}

void brpoplpushCommand(client *c) {
    mstime_t timeout;

    // 获取 timeout 参数
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;

    // 查找 key 对象
    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    // 不存在？
    if (key == NULL) {
        if (c->flags & CLIENT_MULTI) {
            /* Blocking against an empty list in a multi state
             * returns immediately. 
             *
             * 在多状态下，对空列表的阻塞会立即返回。
             * */
            addReplyNull(c);
        } else {
            /* The list is empty and the client blocks. 
             *
             * 该列表为空，客户端将被阻止。
             * */
            // 直接等待元素 push 到 key
            blockForKeys(c,BLOCKED_LIST,c->argv + 1,1,timeout,c->argv[2],NULL);
        }
    } else {
        if (key->type != OBJ_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. 
             *
             * 该列表存在并且具有元素，因此执行常规rpoplpushCommand。
             * */
            serverAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
