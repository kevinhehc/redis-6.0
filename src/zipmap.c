/* String -> String Map data structure optimized for size.
 *
 * 为节约空间而实现的 String -> String Map 结构
 *
 * This file implements a data structure mapping strings to other strings
 * implementing an O(n) lookup data structure designed to be very memory
 * efficient.
 *
 * 本文件实现了一个从 String 到 String 的映射结构，
 * 它的查找复杂度为 O(n) ，并且非常节约内存
 *
 * The Redis Hash type uses this data structure for hashes composed of a small
 * number of elements, to switch to a hash table once a given number of
 * elements is reached.
 *
 * Redis Hash 类型使用这个数据结构对数量不多的元素进行储存，
 * 一旦元素的数量超过某个给定值，它就会自动转换成哈希表
 *
 * Given that many times Redis Hashes are used to represent objects composed
 * of few fields, this is a very big win in terms of used memory.
 *
 * 因为很多时候，一个 Hash 都只保存少数几个 key-value 对，
 * 所以使用 zipmap 比起直接使用真正的哈希表要节约不少内存。
 *
 * --------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* Memory layout of a zipmap, for the map "foo" => "bar", "hello" => "world":
 *
 * 对于映射 "foo" => "bar", "hello" => "world" ， zipmap 有以下内存结构：
 *
 * <zmlen><len>"foo"<len><free>"bar"<len>"hello"<len><free>"world"<ZIPMAP_END>
 *
 * <zmlen> is 1 byte length that holds the current size of the zipmap.
 * When the zipmap length is greater than or equal to 254, this value
 * is not used and the zipmap needs to be traversed to find out the length.
 *
 * <zmlen> 的长度为 1 字节，它保存了 zipmap 的当前大小（size）。
 * 只有 zipmap 的长度 < 254 时，这个值才被使用。
 * 当 zipmap 的长度 >= 254 ，需要遍历整个 zipmap 才能知道它的大小。
 *
 * <len> is the length of the following string (key or value).
 * <len> lengths are encoded in a single value or in a 5 bytes value.
 * If the first byte value (as an unsigned 8 bit value) is between 0 and
 * 253, it's a single-byte length. If it is 254 then a four bytes unsigned
 * integer follows (in the host byte ordering). A value of 255 is used to
 * signal the end of the hash. The special value 254 is used to mark
 * empty space that can be used to add new key/value pairs.
 *
 * <len> 表示跟在它后面的字符串(键或值)的长度。
 *
 * <len> 可以用 1 字节或者 5 字节来编码：
 *
 *   * 如果 <len> 的第一字节(无符号 8 bit)是介于 0 至 252 之间的值，
 *     那么这个字节就是字符串的长度。
 *
 *   * 如果第一字节的值为 253 ，那么这个字节之后的 4 字节无符号整数
 *     (大/小端由所宿主节点器决定)就是字符串的长度。
 *
 *   * 值 254 用于标识未被使用的、可以添加新 key-value 对的空间。
 *
 *   * 值 255 用于表示数据结构的末尾。
 *
 * <free> is the number of free unused bytes after the string, resulting
 * from modification of values associated to a key. For instance if "foo"
 * is set to "bar", and later "foo" will be set to "hi", it will have a
 * free byte to use if the value will enlarge again later, or even in
 * order to add a key/value pair if it fits.
 *
 * <free> 是字符串之后，未被使用的字节数量。
 *
 * 这个值用于记录那些因为值被修改，而被节约下来的空间。
 * 举个例子：
 * zimap 里原本有一个 "foo" -> "bar" 的映射，
 * 但是后来它被修改为 "foo" -> "hi" ，
 * 现在，在字符串 "hi" 之后就有一个字节的未使用空间。
 *
 * 似乎情况，未使用的空间可以用于将来再次对值做修改
 * （比如，再次将 "foo" 的值修改为 "yoo" ，等等）
 * 如果未使用空间足够大，那么在它里面添加一个新的 key-value 对也是可能的。
 *
 * <free> is always an unsigned 8 bit number, because if after an
 * update operation there are more than a few free bytes, the zipmap will be
 * reallocated to make sure it is as small as possible.
 *
 * <free> 总是一个无符号 8 位数字。
 * 因为在执行更新操作之后，如果剩余字节数大于等于 ZIPMAP_VALUE_MAX_FREE ，
 * 那么 zipmap 就会进行重分配，并对自身空间进行紧缩，
 * 因此， <free> 的值不会很大，8 位的长度对于保存 <free> 来说已经足够。
 *
 * The most compact representation of the above two elements hash is actually:
 *
 * "foo" -> "bar" 和 "hello" -> "world" 最紧凑的表示如下：
 *
 * "\x02\x03foo\x03\x00bar\x05hello\x05\x00world\xff"
 *
 * Note that because keys and values are prefixed length "objects",
 * the lookup will take O(N) where N is the number of elements
 * in the zipmap and *not* the number of bytes needed to represent the zipmap.
 * This lowers the constant times considerably.
 *
 * 注意，因为 key 和 value 都是带有长度的对象，
 * 因此 zipmap 的查找操作的复杂度为 O(N) ，
 * 其中 N 是元素的数量，而不是 zipmap 的字节数量（前者的常数更小一些）。
 */

#include <stdio.h>
#include <string.h>
#include "zmalloc.h"
#include "endianconv.h"

// 一个字节所能保存的 zipmap 元素数量不能超过这个值
#define ZIPMAP_BIGLEN 254
// 标识 zipmap 的结束
#define ZIPMAP_END 255

/* The following defines the max value for the <free> field described in the
 * comments above, that is, the max number of trailing bytes in a value. 
 *
 * 下面定义了上面注释中描述的＜free＞字段的最大值，即一个值中尾随字节的最大数量
 *
 * */
// 允许更新之后，值留空的字节数量
#define ZIPMAP_VALUE_MAX_FREE 4

/* The following macro returns the number of bytes needed to encode the length
 * for the integer value _l, that is, 1 byte for lengths < ZIPMAP_BIGLEN and
 * 5 bytes for all the other lengths. 
 *
 * 以下宏返回对整数值_l的长度进行编码所需的字节数，即，长度<ZIPMAP_BIG
 * LEN为1个字节，所有其他长度为5个字节。
 * */
// 返回编码给定 <len> 所需的字节数
#define ZIPMAP_LEN_BYTES(_l) (((_l) < ZIPMAP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

/* Create a new empty zipmap. 
 *
 * 创建一个新的空zipmap。
 * */
unsigned char *zipmapNew(void) {
    unsigned char *zm = zmalloc(2);

    zm[0] = 0; /* Length 
                *
                * 长
                * */
    zm[1] = ZIPMAP_END;
    return zm;
}

/* Decode the encoded length pointed by 'p' 
 *
 * 解码“p”指向的编码长度
 * */
static unsigned int zipmapDecodeLength(unsigned char *p) {
    unsigned int len = *p;

    // <len> 保存在p ，直接返回
    if (len < ZIPMAP_BIGLEN) return len;
    // <len> 保存在p 之后的 4 个字节中
    memcpy(&len,p+1,sizeof(unsigned int));
    memrev32ifbe(&len);
    return len;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. 
 *
 * 将长度“l”编码为“p”。如果p为NULL，它只返回编码这样一个长度所需的字节数
 * 。
 * */
static unsigned int zipmapEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return ZIPMAP_LEN_BYTES(len);
    } else {
        if (len < ZIPMAP_BIGLEN) {
            p[0] = len;
            return 1;
        } else {
            p[0] = ZIPMAP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            memrev32ifbe(p+1);
            return 1+sizeof(len);
        }
    }
}

/* Search for a matching key, returning a pointer to the entry inside the
 * zipmap. Returns NULL if the key is not found.
 *
 * If NULL is returned, and totlen is not NULL, it is set to the entire
 * size of the zipmap, so that the calling function will be able to
 * reallocate the original zipmap to make room for more entries. 
 *
 * 查找给定 key ，找到返回指向 zipmap 中某个实体的一个指针，否则返回 NULL。
 *
 * 如果查找失败，且 totlen 不为 NULL ，
 * 那么将 totlen 设置为 zipmap 实体的大小(size)，
 * 因此，调用函数可以通过对原 zipmap 进行重分配，从
 * 而为 zipmap 的实体分配更多空间。
 */
static unsigned char *zipmapLookupRaw(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned int *totlen) {
    unsigned char *p = zm+1,  // 掠过 <zmlen> ，指向第一个实体
                  *k = NULL;
    unsigned int l,llen;

    // 遍历 zipmap
    while(*p != ZIPMAP_END) {
        unsigned char free;

        /* Match or skip the key 
         *
         * 匹配或跳过键
         * */
        // 获取实体的 <len> 长度
        l = zipmapDecodeLength(p);
        // 计算编码 <len> 所需的长度
        llen = zipmapEncodeLength(NULL,l);
        // key 不为空
        // 还没找到过给定 key
        // 实体的 <len> 和 klen 相同
        // key 匹配
        if (key != NULL && k == NULL && l == klen && !memcmp(p+llen,key,l)) {
            /* Only return when the user doesn't care
             * for the total length of the zipmap. 
             *
             * 只有当用户不关心zipmap的总长度时才返回。
             * */
            // 匹配成功
            if (totlen != NULL) {
                k = p;
            } else {
                return p;
            }
        }
        // 跳过 key
        p += llen+l;
        /* Skip the value as well 
         *
         * 也跳过该值
         * */
        // 跳过 value
        // value 的长度
        l = zipmapDecodeLength(p);
        // 跳过 value 的 <len> 的长度
        p += zipmapEncodeLength(NULL,l);
        // 获取 <free> 的值
        free = p[0];
        // 跳过 value ， <free> 字节，以及 <free> 的长度
        p += l+1+free; /* +1 to skip the free byte 
                        *
                        * +1跳过空闲字节
                        * */
    }
    if (totlen != NULL) *totlen = (unsigned int)(p-zm)+1;
    return k;
}

/*
 * 返回保存 key-value 对所需长度
 *
 * 一个 key-value 对有以下结构 <len>key<len><free>value
 * 其中两个 <len> 都为 1 字节或者 5 字节
 * 而 <free> 为一字节
 */
static unsigned long zipmapRequiredLength(unsigned int klen, unsigned int vlen) {
    unsigned int l;

    // 最少需要 3 字节加上 key 和 value 的长度
    l = klen+vlen+3;
    // 如果有需要的话，为 <len> 加上额外的空间
    if (klen >= ZIPMAP_BIGLEN) l += 4;
    if (vlen >= ZIPMAP_BIGLEN) l += 4;
    return l;
}

/* Return the total amount used by a key (encoded length + payload) 
 *
 * 返回键使用的总量（编码长度+有效载荷）
 * */
static unsigned int zipmapRawKeyLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    return zipmapEncodeLength(NULL,l) + l;
}

/* Return the total amount used by a value
 * (encoded length + single byte free count + payload) 
 *
 * 返回一个值使用的总量（编码长度+单字节空闲计数+有效负载）
 * */
static unsigned int zipmapRawValueLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    unsigned int used;

    used = zipmapEncodeLength(NULL,l);
    used += p[used] + 1 + l;
    return used;
}

/* If 'p' points to a key, this function returns the total amount of
 * bytes used to store this entry (entry = key + associated value + trailing
 * free space if any). 
 *
 * 如果“p”指向一个键，此函数将返回用于存储此项的总字节数（项=键+关联值+尾部可用空间（如果有））。
 * */
/*
 * 如果 p 指向一个 key ，那么返回整个储存这个实体所需的字节数
 * (实体 = key + 关联 value + (可能有的)空余空间)
 */
static unsigned int zipmapRawEntryLength(unsigned char *p) {
    unsigned int l = zipmapRawKeyLength(p);
    return l + zipmapRawValueLength(p+l);
}

/*
 * 调整 zipmap 的大小为 len 字节
 */
static inline unsigned char *zipmapResize(unsigned char *zm, unsigned int len) {
    zm = zrealloc(zm, len);
    zm[len-1] = ZIPMAP_END;
    return zm;
}

/* Set key to value, creating the key if it does not already exist.
 * If 'update' is not NULL, *update is set to 1 if the key was
 * already preset, otherwise to 0. 
 *
 * 将键设置为值，如果该键不存在，则创建该键。如果“update”不为NULL，则如果键已预设，则*update设置为1，否则设置为0。
 * */
/*
 * 将 key 映射到 value ，如果 key 不存在就创建一个新的。
 *
 * 如果 key 原本已经存在，并且 update 不为 NULL ，
 * 那么将 *update 设为 1 ，否则设置为 0 。
 */
unsigned char *zipmapSet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char *val, unsigned int vlen, int *update) {
    unsigned int zmlen, offset;
    unsigned int freelen,
                 reqlen = zipmapRequiredLength(klen,vlen); // 保存实体所需的空间大小
    unsigned int empty, vempty;
    unsigned char *p;

    freelen = reqlen;
    if (update) *update = 0;
    // 按 key 查找映射
    p = zipmapLookupRaw(zm,key,klen,&zmlen);
    if (p == NULL) {
        /* Key not found: enlarge 
         *
         * 找不到键：放大
         * */
        // key 不存在，扩展 zipmap
        zm = zipmapResize(zm, zmlen+reqlen);
        p = zm+zmlen-1; // -1 回退到 ZIPMAP_END 上
        // 更新 zipmap 的长度
        zmlen = zmlen+reqlen;

        /* Increase zipmap length (this is an insert) 
         *
         * 增加拉链长度（这是一个插入）
         * */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]++;
    } else {
        /* Key found. Is there enough space for the new value? 
         *
         * 找到键。新价值有足够的空间吗？
         * */
        /* Compute the total length: 
         *
         * 计算总长度：
         * */
        if (update) *update = 1;
        // 获取实体的空间
        freelen = zipmapRawEntryLength(p);
        if (freelen < reqlen) {
            /* Store the offset of this key within the current zipmap, so
             * it can be resized. Then, move the tail backwards so this
             * pair fits at the current position. 
             *
             * 将此键的偏移量存储在当前zipmap中，以便调整其大小。然后，将尾巴向后移动，使这对适合当前位置。
             * */
            // 所需空间比实体现有空间大，扩大 zipmap 空间
            offset = p-zm;
            zm = zipmapResize(zm, zmlen-freelen+reqlen);
            p = zm+offset;

            /* The +1 in the number of bytes to be moved is caused by the
             * end-of-zipmap byte. Note: the *original* zmlen is used. 
             *
             * 要移动的字节数中的+1是由zipmap字节的末尾引起的。注：使用*originalzmlen。
             * */
            // 后移数据
            // 之前：
            // <many-bytes><p><remain-bytes>
            // 之后：
            // <many-bytes>< ... p ... ><remain-bytes>
            memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
            // 更新长度变量
            zmlen = zmlen-freelen+reqlen;
            freelen = reqlen;
        }
    }

    /* We now have a suitable block where the key/value entry can
     * be written. If there is too much free space, move the tail
     * of the zipmap a few bytes to the front and shrink the zipmap,
     * as we want zipmaps to be very space efficient. 
     *
     * 我们现在有了一个合适的块，可以在其中写入键/值节点。如果有太多的可用空间，
     * 请将zipmap的尾部向前移动几个字节，然后缩小zipmap，因为我们希望zipmap
     * 非常节省空间。
     * */
    // 计算更新 value 之后的空余空间长度，如果有需要就对 zipmap 进行紧缩
    empty = freelen-reqlen;
    if (empty >= ZIPMAP_VALUE_MAX_FREE) {
        /* First, move the tail <empty> bytes to the front, then resize
         * the zipmap to be <empty> bytes smaller. 
         *
         * 首先，将尾部的＜empty＞字节移到前面，然后将zipmap的大小调整为较小的＜empt＞字节。
         * */
        offset = p-zm;
        memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
        zmlen -= empty;
        zm = zipmapResize(zm, zmlen);
        p = zm+offset;
        vempty = 0;
    } else {
        vempty = empty;
    }

    /* Just write the key + value and we are done. 
     *
     * 只要写出键+值，我们就完成了。
     * */
    /* Key: 
     *
     * 键：
     * */
    p += zipmapEncodeLength(p,klen);
    memcpy(p,key,klen);
    p += klen;
    /* Value: 
     *
     * 价值：
     * */
    p += zipmapEncodeLength(p,vlen);
    *p++ = vempty;
    memcpy(p,val,vlen);
    return zm;
}

/* Remove the specified key. If 'deleted' is not NULL the pointed integer is
 * set to 0 if the key was not found, to 1 if it was found and deleted. 
 *
 * 删除指定的键。如果“deleted”不为NULL，则如果未找到键，则将指向整数
 * 设置为0，如果找到并删除了键，则设置为1。
 * */
/*
 * 移除指定的 key ，并返回修改后的 zipmap 。
 *
 * 如果 deleted 不为 NULL ，那么：
 *   - key 没找到所以删除失败，设置为 0 。
 *   - key 找到，并且删除成功，设置为 1 。
 */
unsigned char *zipmapDel(unsigned char *zm, unsigned char *key, unsigned int klen, int *deleted) {
    unsigned int zmlen, freelen;
    unsigned char *p = zipmapLookupRaw(zm,key,klen,&zmlen);
    if (p) {
        // 计算要释放的空间长度
        freelen = zipmapRawEntryLength(p);
        // 前移数据
        // 之前：
        // <many-bytes>< ... p ... ><remain-bytes>
        // 之后:
        // <many-bytes><p><remain-bytes>
        memmove(p, p+freelen, zmlen-((p-zm)+freelen+1));
        // 紧缩空间
        zm = zipmapResize(zm, zmlen-freelen);

        /* Decrease zipmap length 
         *
         * 更新 zipmap 长度
         * */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]--;

        if (deleted) *deleted = 1;
    } else {
        if (deleted) *deleted = 0;
    }
    return zm;
}

/* Call before iterating through elements via zipmapNext() 
 *
 * 在通过zipmapNext（）迭代元素之前调用，根据给定的 zipmap ，生成迭代对象
 * */
unsigned char *zipmapRewind(unsigned char *zm) {
    return zm+1;
}

/* This function is used to iterate through all the zipmap elements.
 * In the first call the first argument is the pointer to the zipmap + 1.
 * In the next calls what zipmapNext returns is used as first argument.
 * Example:
 *
 * unsigned char *i = zipmapRewind(my_zipmap);
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 
 *
 * 这个函数用于遍历所有的zipmap元素。在第一个调用中，第一个参数是指向zipm
 * ap+1的指针。在接下来的调用中，zipmapNext返回的内容将用作第一个参数
 *
 *
 * 函数的 zm 参数是一个带状态的迭代对象，由 zipmapRewind() 创建。
 *
 * 用例：
 *
 * unsigned char *i = zipmapRewind(my_zipmap);
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 */
unsigned char *zipmapNext(unsigned char *zm, unsigned char **key, unsigned int *klen, unsigned char **value, unsigned int *vlen) {
    if (zm[0] == ZIPMAP_END) return NULL;
    if (key) {
        *key = zm;
        *klen = zipmapDecodeLength(zm);
        *key += ZIPMAP_LEN_BYTES(*klen);
    }
    zm += zipmapRawKeyLength(zm);
    if (value) {
        *value = zm+1;
        *vlen = zipmapDecodeLength(zm);
        *value += ZIPMAP_LEN_BYTES(*vlen);
    }
    zm += zipmapRawValueLength(zm);
    return zm;
}

/* Search a key and retrieve the pointer and len of the associated value.
 * If the key is found the function returns 1, otherwise 0. 
 *
 * 搜索一个键并检索相关值的指针和长度。如果找到键，函数将返回1，否则返回0。
 * */
int zipmapGet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char **value, unsigned int *vlen) {
    unsigned char *p;

    if ((p = zipmapLookupRaw(zm,key,klen,NULL)) == NULL) return 0;
    p += zipmapRawKeyLength(p);
    *vlen = zipmapDecodeLength(p);
    *value = p + ZIPMAP_LEN_BYTES(*vlen) + 1;
    return 1;
}

/* Return 1 if the key exists, otherwise 0 is returned. 
 *
 * 如果键存在，则返回1，否则返回0。
 * */ 
int zipmapExists(unsigned char *zm, unsigned char *key, unsigned int klen) {
    return zipmapLookupRaw(zm,key,klen,NULL) != NULL;
}

/* Return the number of entries inside a zipmap 
 *
 * 返回 zipmap 里，实体的数量
 * */
unsigned int zipmapLen(unsigned char *zm) {
    unsigned int len = 0;
    if (zm[0] < ZIPMAP_BIGLEN) {
        len = zm[0];
    } else {
        unsigned char *p = zipmapRewind(zm);
        while((p = zipmapNext(p,NULL,NULL,NULL,NULL)) != NULL) len++;

        /* Re-store length if small enough 
         *
         * 如果足够小，则重新存储长度
         * */
        if (len < ZIPMAP_BIGLEN) zm[0] = len;
    }
    return len;
}

/* Return the raw size in bytes of a zipmap, so that we can serialize
 * the zipmap on disk (or everywhere is needed) just writing the returned
 * amount of bytes of the C array starting at the zipmap pointer. 
 *
 * 返回zipmap的原始大小（以字节为单位），这样我们就可以在磁盘上序列化zipmap（或者在需要的任何地方），
 * 只需写入从zipmap指针开始的C数组的返回字节数
 * 。
 * */
/*
 * 返回整个 zipmap 的长度
 */
size_t zipmapBlobLen(unsigned char *zm) {
    unsigned int totlen;
    zipmapLookupRaw(zm,NULL,0,&totlen);
    return totlen;
}

#ifdef REDIS_TEST
static void zipmapRepr(unsigned char *p) {
    unsigned int l;

    printf("{status %u}",*p++);
    while(1) {
        if (p[0] == ZIPMAP_END) {
            printf("{end}");
            break;
        } else {
            unsigned char e;

            l = zipmapDecodeLength(p);
            printf("{key %u}",l);
            p += zipmapEncodeLength(NULL,l);
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l;

            l = zipmapDecodeLength(p);
            printf("{value %u}",l);
            p += zipmapEncodeLength(NULL,l);
            e = *p++;
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l+e;
            if (e) {
                printf("[");
                while(e--) printf(".");
                printf("]");
            }
        }
    }
    printf("\n");
}

#define UNUSED(x) (void)(x)
int zipmapTest(int argc, char *argv[]) {
    unsigned char *zm;

    UNUSED(argc);
    UNUSED(argv);

    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);

    printf("\nLook up large key:\n");
    {
        unsigned char buf[512];
        unsigned char *value;
        unsigned int vlen, i;
        for (i = 0; i < 512; i++) buf[i] = 'a';

        zm = zipmapSet(zm,buf,512,(unsigned char*) "long",4,NULL);
        if (zipmapGet(zm,buf,512,&value,&vlen)) {
            printf("  <long key> is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }

    printf("\nPerform a direct lookup:\n");
    {
        unsigned char *value;
        unsigned int vlen;

        if (zipmapGet(zm,(unsigned char*) "foo",3,&value,&vlen)) {
            printf("  foo is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }
    printf("\nIterate through elements:\n");
    {
        unsigned char *i = zipmapRewind(zm);
        unsigned char *key, *value;
        unsigned int klen, vlen;

        while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
            printf("  %d:%.*s => %d:%.*s\n", klen, klen, key, vlen, vlen, value);
        }
    }
    return 0;
}
#endif
