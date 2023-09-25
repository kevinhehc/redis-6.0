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
#include <math.h> /* isnan(), isinf() 
                   *
                   * isnan（），isinf（）
                   * */

/*-----------------------------------------------------------------------------
 * String Commands
 * 字符串命令
 *----------------------------------------------------------------------------*/
/*
 * 检查长度 size 是否超过 Redis 的最大限制
 *
 * 复杂度：O(1)
 *
 * 返回值：
 *  REDIS_ERR   超过
 *  REDIS_OK    未超过
 */
static int checkStringLength(client *c, long long size, long long append) {
    if (c->flags & CLIENT_MASTER)
        return C_OK;
    /* 'uint64_t' cast is there just to prevent undefined behavior on overflow 
     *
     * “uint64_t”强制转换只是为了防止溢出时出现未定义的行为
     * */
    long long total = (uint64_t)size + append;
    /* Test configured max-bulk-len represending a limit of the biggest string object,
     * and also test for overflow. 
     *
     * 测试配置的表示最大字符串对象的限制的最大bulk-len，并测试溢出。
     * */
    if (total > server.proto_max_bulk_len || total < size || total < append) {
        addReplyError(c,"string exceeds maximum allowed size (proto-max-bulk-len)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see below).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. 
 *
 * setGenericCommand（）函数使用不同的选项和变体来实现SET操作。
 * 调用此函数是为了实现以下命令：SET、SETEX、PSETEX、SETNX
 *
 *  'flags'更改命令的行为（NX或XX，请参阅下文）。
 *
 *  'expire'表示以用户传递的Redis对象的形式设置的过期。根据指定的“单位”进行解释
 *
 *  ok_reply 和 “abort_respoy”是如果执行了操作，或者不是因为NX或XX标志，函数将向客户端回复的内容。
 *
 * 如果ok_reply为NULL，则使用“+ok”。
 * 如果abort_respoy为NULL，则使用“$-1”。
 * */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)          /* Set if key not exists. 
                                    *
                                    * 如果键不存在，则设置。
                                    * */
#define OBJ_SET_XX (1<<1)          /* Set if key exists. 
                                    *
                                    * 如果键存在，则进行设置。
                                    * */
#define OBJ_SET_EX (1<<2)          /* Set if time in seconds is given 
                                    *
                                    * 如果给定了以秒为单位的时间，则设置
                                    * */
#define OBJ_SET_PX (1<<3)          /* Set if time in ms in given 
                                    *
                                    * 如果给定时间（毫秒），则设置
                                    * */
#define OBJ_SET_KEEPTTL (1<<4)     /* Set and keep the ttl 
                                    *
                                    * 设置并保持ttl
                                    * */

/*
 * 通用 set 命令，用于 SET / SETEX 和 SETNX 等命令的底层实现
 *
 * 参数：
 *  c   客户端
 *  nx  如果不为 0 ，那么表示只有在 key 不存在时才进行 set 操作
 *  key
 *  val
 *  expire  过期时间
 *  unit    过期时间的单位，分为 UNIT_SECONDS 和 UNIT_MILLISECONDS
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; /* initialized to avoid any harmness warning 
                                 *
                                 * 已初始化以避免任何伤害警告
                                 * */

    // 如果带有 expire 参数，那么将它从 sds 转为 long long 类型
    if (expire) {
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK)
            return;
        if (milliseconds <= 0) {
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }
        // 决定过期时间是秒还是毫秒
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }

    // 如果给定了 nx 参数，并且 key 已经存在，那么直接向客户端返回
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.null[c->resp]);
        return;
    }
    // 设置 key-value 对
    genericSetKey(c,c->db,key,val,flags & OBJ_SET_KEEPTTL,1);
    server.dirty++;
    // 为 key 设置过期时间
    if (expire) setExpire(c,c->db,key,mstime()+milliseconds);
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC,
        "expire",key,c->db->id);
    // 向客户端返回回复
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [KEEPTTL] [EX <seconds>] [PX <milliseconds>] 
 * SET 命令的实现
 * */
void setCommand(client *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_SET_NO_FLAGS;

    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
            !(flags & OBJ_SET_XX))
        {
            flags |= OBJ_SET_NX;
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX))
        {
            flags |= OBJ_SET_XX;
        } else if (!strcasecmp(c->argv[j]->ptr,"KEEPTTL") &&
                   !(flags & OBJ_SET_EX) && !(flags & OBJ_SET_PX))
        {
            flags |= OBJ_SET_KEEPTTL;
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_KEEPTTL) &&
                   !(flags & OBJ_SET_PX) && next)
        {
            flags |= OBJ_SET_EX;
            unit = UNIT_SECONDS;
            expire = next;
            j++;
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_KEEPTTL) &&
                   !(flags & OBJ_SET_EX) && next)
        {
            flags |= OBJ_SET_PX;
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

/*
 * SETNX 命令的实现
 *
 * 复杂度：
 *  O(1)
 *
 * 返回值：void
 */
void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

/*
 * SETEX 命令的实现
 *
 * 复杂度：
 *  O(1)
 *
 * 返回值：void
 */
void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

/*
 * PSETEX 命令的实现
 *
 * 复杂度：
 *  O(1)
 *
 * 返回值：void
 */
void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

/*
 * 根据客户端指定的 key ，查找相应的值。
 * 各种 get 命令的底层实现。
 *
 * 复杂度：
 *  O(1)
 *
 * 返回值：
 *  REDIS_OK    查找完成（可能找到，也可能没找到）
 *  REDIS_ERR   找到，但 key 不是字符串类型
 */
int getGenericCommand(client *c) {
    robj *o;

    // 查找
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)
        return C_OK;

    // 返回
    if (o->type != OBJ_STRING) {
        addReply(c,shared.wrongtypeerr);
        return C_ERR;
    } else {
        addReplyBulk(c,o);
        return C_OK;
    }
}

/*
 * GET 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void getCommand(client *c) {
    getGenericCommand(c);
}

/*
 * GETSET 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void getsetCommand(client *c) {
    // 获取现有值，并添加到客户端回复 buffer 中
    if (getGenericCommand(c) == C_ERR) return;

    // 设置新值
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setKey(c,c->db,c->argv[1],c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);
    server.dirty++;
}

/*
 * SETRANGE 命令的实现
 *
 * 复杂度：O(N)
 *
 * 返回值：void
 */
void setrangeCommand(client *c) {
    robj *o;
    long offset;
    // 用来替换旧内容的字符串
    sds value = c->argv[3]->ptr;

    // 将 offset 转换为 long 类型值
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;

    // 检查 offset 是否位于合法范围
    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

    // 查找给定 key
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Return 0 when setting nothing on a non-existing string 
         *
         * 对不存在的字符串不设置任何内容时返回0
         * */
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* Return when the resulting string exceeds allowed size 
         *
         * 当结果字符串超过允许的大小时返回
         * */
        // 当 value 的长度过大时，直接返回
        if (checkStringLength(c,offset,sdslen(value)) != C_OK)
            return;

        // 将 key 设置为空字符串对象
        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));
        dbAdd(c->db,c->argv[1],o);
    } else {
        // key 存在 ...

        size_t olen;

        /* Key exists, check type 
         *
         * 键存在，检查类型
         * */
        // 检查 key 是否字符串
        if (checkType(c,o,OBJ_STRING))
            return;

        /* Return existing string length when setting nothing 
         *
         * 不设置任何内容时返回现有字符串长度
         * */
        // 如果 value 为空字符串，那么直接返回原有字符串的长度
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* Return when the resulting string exceeds allowed size 
         *
         * 当结果字符串超过允许的大小时返回
         * */
        // 检查修改后的字符串长度会否超过最大限制
        if (checkStringLength(c,offset,sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. 
         *
         * 在共享或编码对象时创建副本。
         * */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    // 进行修改操作
    if (sdslen(value) > 0) {
        // 先用 0 字节填充整个范围
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
        // 复制内容到 key
        memcpy((char*)o->ptr+offset,value,sdslen(value));
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
        server.dirty++;
    }
    // 将修改后字符串的长度返回给客户端
    addReplyLongLong(c,sdslen(o->ptr));
}

/*
 * GETRANGE 命令的实现
 *
 * 复杂度：O(N)
 *
 * 返回值：void
 */
void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    // 获取 start 索引
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;

    // 获取 end 索引
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;

    // 如果 key 不存在，或者 key 不是字符串类型，那么直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    // 获取字符串，以及它的长度
    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* Convert negative indexes 
     *
     * 转换负索引
     * */
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }

    // 对负数索引进行转换
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. 
     *
     * 前提条件：end>=0&&end<strlen，所以唯一不能返回任何内容的条件是：start>end。
     * */
    if (start > end || strlen == 0) {
        addReply(c,shared.emptybulk);
    } else {
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/*
 * MGET 命令的实现
 *
 * 复杂度：O(N)
 *
 * 返回值：void
 */
void mgetCommand(client *c) {
    int j;

    // 执行多个读取
    addReplyArrayLen(c,c->argc-1);
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReplyNull(c);
        } else {
            if (o->type != OBJ_STRING) {
                addReplyNull(c);
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

/*
 * MSET / MSETNX 命令的底层实现
 *
 * 参数：
 *  nx  如果不为 0 ，那么只有在 key 不存在时才进行设置
 *
 * 复杂度：O(N)
 *
 * 返回值：void
 */
void msetGenericCommand(client *c, int nx) {
    int j;

    // 检查输入参数是否成对
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key already exists. 
     *
     * 处理NX标志。MSETNX语义是返回零，并且如果至少有一个键已经存在，则不设置任何内容。
     * */
    // 当 nx 选项打开时，检查给定的 key 是否已经存在
    // 只要任何一个 key 存在，那么就不进行修改，直接返回 0
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                addReply(c, shared.czero);
                return;
            }
        }
    }

    // 执行多个写入
    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c,c->db,c->argv[j],c->argv[j+1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

/*
 * MSET 命令的实现
 *
 * 复杂度：O(N)
 *
 * 返回值：void
 */
void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

/*
 * MSETNX 命令的实现
 *
 * 复杂度：O(N)
 *
 * 返回值：void
 */
void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}

/*
 * 对给定字符串保存的数值进行加法或者减法操作
 * incr / decr / incrby 和 decrby 等命令的底层实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;

    // 查找 key
    o = lookupKeyWrite(c->db,c->argv[1]);
    // 如果 key 非空且 key 类型错误，直接返回
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    // 如果值不能转换为数字，直接返回
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    // 检查和值是否会溢出
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    // 计算和值
    value += incr;

    // 根据 o 对象是否存在，选择覆写或者新建对象
    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((long)value);
    } else {
        new = createStringObjectFromLongLongForValue(value);
        if (o) {
            dbOverwrite(c->db,c->argv[1],new);
        } else {
            dbAdd(c->db,c->argv[1],new);
        }
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}

/*
 * INCR 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void incrCommand(client *c) {
    incrDecrCommand(c,1);
}

/*
 * DECR 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}

/*
 * incrby 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}

/*
 * decrby 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void decrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);
}

/*
 * INCRBYFLOAT 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new, *aux1, *aux2;

    // 获取 key 对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    // 如果对象存在且不为字符串类型，直接返回
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    // 如果对象 o 或者传入增量参数不能转换为浮点数，直接返回
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    // 计算和
    value += incr;
    // 溢出检查
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    // 将值保存到新对象，并覆写原有对象
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. 
     *
     * 始终将INCRBYFLOAT复制为具有最终值的SET命令，以确保浮点精度或格式的
     * 差异不会在副本中或AOF重新启动后产生差异。
     * */
    aux1 = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux1);
    decrRefCount(aux1);
    rewriteClientCommandArgument(c,2,new);
    aux2 = createStringObject("KEEPTTL",7);
    rewriteClientCommandArgument(c,3,aux2);
    decrRefCount(aux2);
}

/*
 * APPEND 命令的实现
 *
 * 复杂度：O(N)
 *
 * 返回值：void
 */
void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;

    // 查找 key 对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key 
         *
         * 创建键
         * */
        // 对象不存在 ...

        // 创建字符串对象，并将它添加到数据库
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        // 对象存在...

        // 如果不是字符串，直接返回
        /* Key exists, check type 
         *
         * 键存在，检查类型
         * */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* "append" is an argument, so always an sds 
         *
         * “append”是一个参数，所以总是一个sds
         * */
        // 检查拼接完成后，字符串的长度是否合法
        append = c->argv[2];
        if (checkStringLength(c,stringObjectLen(o),sdslen(append->ptr)) != C_OK)
            return;

        /* Append the value 
         *
         * 附加值
         * */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
    addReplyLongLong(c,totlen);
}

/*
 * STRLEN 命令的实现
 *
 * 复杂度：O(1)
 *
 * 返回值：void
 */
void strlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;
    addReplyLongLong(c,stringObjectLen(o));
}


/* STRALGO -- Implement complex algorithms on strings.
 *
 * STRALGO <algorithm> ... arguments ... 
 *
 * STRALGO——在字符串上实现复杂的算法。STRALGO＜算法＞。。。参数。。
 * 。
 * */
void stralgoLCS(client *c);     /* This implements the LCS algorithm. 
                                 *
                                 * 这实现了LCS算法。
                                 * */
void stralgoCommand(client *c) {
    /* Select the algorithm. 
     *
     * 选择算法。
     * */
    if (!strcasecmp(c->argv[1]->ptr,"lcs")) {
        stralgoLCS(c);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

/* STRALGO <algo> [IDX] [MINMATCHLEN <len>] [WITHMATCHLEN]
 *     STRINGS <string> <string> | KEYS <keya> <keyb>
 */
void stralgoLCS(client *c) {
    uint32_t i, j;
    long long minmatchlen = 0;
    sds a = NULL, b = NULL;
    int getlen = 0, getidx = 0, withmatchlen = 0;
    robj *obja = NULL, *objb = NULL;

    for (j = 2; j < (uint32_t)c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1) - j;

        if (!strcasecmp(opt,"IDX")) {
            getidx = 1;
        } else if (!strcasecmp(opt,"LEN")) {
            getlen = 1;
        } else if (!strcasecmp(opt,"WITHMATCHLEN")) {
            withmatchlen = 1;
        } else if (!strcasecmp(opt,"MINMATCHLEN") && moreargs) {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&minmatchlen,NULL)
                != C_OK) goto cleanup;
            if (minmatchlen < 0) minmatchlen = 0;
            j++;
        } else if (!strcasecmp(opt,"STRINGS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            a = c->argv[j+1]->ptr;
            b = c->argv[j+2]->ptr;
            j += 2;
        } else if (!strcasecmp(opt,"KEYS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            obja = lookupKeyRead(c->db,c->argv[j+1]);
            objb = lookupKeyRead(c->db,c->argv[j+2]);
            if ((obja && obja->type != OBJ_STRING) ||
                (objb && objb->type != OBJ_STRING))
            {
                addReplyError(c,
                    "The specified keys must contain string values");
                /* Don't cleanup the objects, we need to do that
                 * only after callign getDecodedObject(). 
                 *
                 * 不要清理对象，我们只需要在调用getDecodedObject（）之后才能进行清理。
                 * */
                obja = NULL;
                objb = NULL;
                goto cleanup;
            }
            obja = obja ? getDecodedObject(obja) : createStringObject("",0);
            objb = objb ? getDecodedObject(objb) : createStringObject("",0);
            a = obja->ptr;
            b = objb->ptr;
            j += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Complain if the user passed ambiguous parameters. 
     *
     * 如果用户传递了不明确的参数，则提示。
     * */
    if (a == NULL) {
        addReplyError(c,"Please specify two strings: "
                        "STRINGS or KEYS options are mandatory");
        goto cleanup;
    } else if (getlen && getidx) {
        addReplyError(c,
            "If you want both the length and indexes, please "
            "just use IDX.");
        goto cleanup;
    }

    /* Detect string truncation or later overflows. 
     *
     * 检测字符串截断或更高版本的溢出。
     * */
    if (sdslen(a) >= UINT32_MAX-1 || sdslen(b) >= UINT32_MAX-1) {
        addReplyError(c, "String too long for LCS");
        goto cleanup;
    }

    /* Compute the LCS using the vanilla dynamic programming technique of
     * building a table of LCS(x,y) substrings. 
     *
     * 使用构建LCS（x，y）子字符串表的普通动态编程技术来计算LCS。
     * */
    uint32_t alen = sdslen(a);
    uint32_t blen = sdslen(b);

    /* Setup an uint32_t array to store at LCS[i,j] the length of the
     * LCS A0..i-1, B0..j-1. Note that we have a linear array here, so
     * we index it as LCS[j+(blen+1)*j] 
     *
     * 设置uint32_t数组以在LCS[i，j]处存储LCS A0..i-1、B0..j-1的长度。
     * 注意，我们这里有一个线性数组，所以我们将其索引为LCS[j+（blen+1）*j]
     * */
    #define LCS(A,B) lcs[(B)+((A)*(blen+1))]

    /* Try to allocate the LCS table, and abort on overflow or insufficient memory. 
     *
     * 尝试分配LCS表，并在内存溢出或不足时中止。
     * */
    unsigned long long lcssize = (unsigned long long)(alen+1)*(blen+1); /* Can't overflow due to the size limits above. 
                                                                         *
                                                                         * 由于以上大小限制，无法溢出。
                                                                         * */
    unsigned long long lcsalloc = lcssize * sizeof(uint32_t);
    uint32_t *lcs = NULL;
    if (lcsalloc < SIZE_MAX && lcsalloc / lcssize == sizeof(uint32_t))
        lcs = zmalloc(lcsalloc);
    if (!lcs) {
        addReplyError(c, "Insufficient memory");
        goto cleanup;
    }

    /* Start building the LCS table. 
     *
     * 开始构建LCS表。
     * */
    for (uint32_t i = 0; i <= alen; i++) {
        for (uint32_t j = 0; j <= blen; j++) {
            if (i == 0 || j == 0) {
                /* If one substring has length of zero, the
                 * LCS length is zero. 
                 *
                 * 如果一个子字符串的长度为零，则LCS长度为零。
                 * */
                LCS(i,j) = 0;
            } else if (a[i-1] == b[j-1]) {
                /* The len LCS (and the LCS itself) of two
                 * sequences with the same final character, is the
                 * LCS of the two sequences without the last char
                 * plus that last char. 
                 *
                 * 具有相同最终字符的两个序列的len LCS（和LCS本身），是没有最后一个字符的
                 * 这两个序列加上最后一个字母的LCS。
                 * */
                LCS(i,j) = LCS(i-1,j-1)+1;
            } else {
                /* If the last character is different, take the longest
                 * between the LCS of the first string and the second
                 * minus the last char, and the reverse. 
                 *
                 * 如果最后一个字符不同，则取第一个字符串和第二个字符串的LCS之间的最长值减去最后
                 * 一个char，反之亦然。
                 * */
                uint32_t lcs1 = LCS(i-1,j);
                uint32_t lcs2 = LCS(i,j-1);
                LCS(i,j) = lcs1 > lcs2 ? lcs1 : lcs2;
            }
        }
    }

    /* Store the actual LCS string in "result" if needed. We create
     * it backward, but the length is already known, we store it into idx. 
     *
     * 如果需要，将实际LCS字符串存储在“result”中。我们向后创建它，但长度是已
     * 知的，我们将其存储到idx中。
     * */
    uint32_t idx = LCS(alen,blen);
    sds result = NULL;        /* Resulting LCS string. 
                               *
                               * 生成的LCS字符串。
                               * */
    void *arraylenptr = NULL; /* Deffered length of the array for IDX. 
                               *
                               * 为IDX定义的数组长度。
                               * */
    uint32_t arange_start = alen, /* alen signals that values are not set. 
                                   *
                                   * alen表示未设置值。
                                   * */
             arange_end = 0,
             brange_start = 0,
             brange_end = 0;

    /* Do we need to compute the actual LCS string? Allocate it in that case. 
     *
     * 我们需要计算实际的LCS字符串吗？在这种情况下进行分配。
     * */
    int computelcs = getidx || !getlen;
    if (computelcs) result = sdsnewlen(SDS_NOINIT,idx);

    /* Start with a deferred array if we have to emit the ranges. 
     *
     * 如果必须发射范围，则从延迟数组开始。
     * */
    uint32_t arraylen = 0;  /* Number of ranges emitted in the array. 
                             *
                             * 阵列中发射的范围数。
                             * */
    if (getidx) {
        addReplyMapLen(c,2);
        addReplyBulkCString(c,"matches");
        arraylenptr = addReplyDeferredLen(c);
    }

    i = alen, j = blen;
    while (computelcs && i > 0 && j > 0) {
        int emit_range = 0;
        if (a[i-1] == b[j-1]) {
            /* If there is a match, store the character and reduce
             * the indexes to look for a new match. 
             *
             * 如果存在匹配，则存储字符并减少索引以查找新的匹配。
             * */
            result[idx-1] = a[i-1];

            /* Track the current range. 
             *
             * 跟踪当前范围。
             * */
            if (arange_start == alen) {
                arange_start = i-1;
                arange_end = i-1;
                brange_start = j-1;
                brange_end = j-1;
            } else {
                /* Let's see if we can extend the range backward since
                 * it is contiguous. 
                 *
                 * 让我们看看我们是否可以向后扩展范围，因为它是连续的。
                 * */
                if (arange_start == i && brange_start == j) {
                    arange_start--;
                    brange_start--;
                } else {
                    emit_range = 1;
                }
            }
            /* Emit the range if we matched with the first byte of
             * one of the two strings. We'll exit the loop ASAP. 
             *
             * 如果我们与两个字符串之一的第一个字节匹配，则发出范围。我们会尽快退出循环。
             * */
            if (arange_start == 0 || brange_start == 0) emit_range = 1;
            idx--; i--; j--;
        } else {
            /* Otherwise reduce i and j depending on the largest
             * LCS between, to understand what direction we need to go. 
             *
             * 否则，根据两者之间最大的LCS来减少i和j，以了解我们需要朝哪个方向前进。
             * */
            uint32_t lcs1 = LCS(i-1,j);
            uint32_t lcs2 = LCS(i,j-1);
            if (lcs1 > lcs2)
                i--;
            else
                j--;
            if (arange_start != alen) emit_range = 1;
        }

        /* Emit the current range if needed. 
         *
         * 如果需要，发射当前范围。
         * */
        uint32_t match_len = arange_end - arange_start + 1;
        if (emit_range) {
            if (minmatchlen == 0 || match_len >= minmatchlen) {
                if (arraylenptr) {
                    addReplyArrayLen(c,2+withmatchlen);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,arange_start);
                    addReplyLongLong(c,arange_end);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,brange_start);
                    addReplyLongLong(c,brange_end);
                    if (withmatchlen) addReplyLongLong(c,match_len);
                    arraylen++;
                }
            }
            arange_start = alen; /* Restart at the next match. 
                                  *
                                  * 在下一场比赛中重新开始。
                                  * */
        }
    }

    /* Signal modified key, increment dirty, ... 
     *
     * 通知修改键，增量脏记录。。。
     * */

    /* Reply depending on the given options. 
     *
     * 根据给定选项进行回复。
     * */
    if (arraylenptr) {
        addReplyBulkCString(c,"len");
        addReplyLongLong(c,LCS(alen,blen));
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else if (getlen) {
        addReplyLongLong(c,LCS(alen,blen));
    } else {
        addReplyBulkSds(c,result);
        result = NULL;
    }

    /* Cleanup. 
     *
     * 清理。
     * */
    sdsfree(result);
    zfree(lcs);

cleanup:
    if (obja) decrRefCount(obja);
    if (objb) decrRefCount(objb);
    return;
}

