/* The ziplist is a specially encoded dually linked list that is designed
 * to be very memory efficient. It stores both strings and integer values,
 * where integers are encoded as actual integers instead of a series of
 * characters. It allows push and pop operations on either side of the list
 * in O(1) time. However, because every operation requires a reallocation of
 * the memory used by the ziplist, the actual complexity is related to the
 * amount of memory used by the ziplist.
 *
 * Ziplist 是为内存占用而特别优化的双链表。
 *
 * 它可以保存字符串和整数，其中整数以整数类型而不是字符串来进行编码和保存。
 *
 * 对 ziplist 的两端进行 push 和 pop 的复杂度都为 O(1) ，
 * 不过，因为对 ziplist 的每次修改操作都需要进行内存重分配，
 * 因此，实际的时间复杂度与 ziplist 使用的内存大小有关。
 * ----------------------------------------------------------------------------
 *
 * ZIPLIST OVERALL LAYOUT
 * ======================
 *
 * The general layout of the ziplist is as follows:
 *
 * 以下是 ziplist 的内存结构：
 *
 * <zlbytes> <zltail> <zllen> <entry> <entry> ... <entry> <zlend>
 *
 * NOTE: all fields are stored in little endian, if not specified otherwise.
 *
 * <uint32_t zlbytes> is an unsigned integer to hold the number of bytes that
 * the ziplist occupies, including the four bytes of the zlbytes field itself.
 * This value needs to be stored to be able to resize the entire structure
 * without the need to traverse it first.
 *
 * <zlbytes> 是一个无符号整数(uint32_t)，用于记录整个 ziplist 所占用的字节数量。
 * 通过保存这个值，可以在不遍历整个 ziplist 的前提下，对整个 ziplist 进行内存重分配。
 *
 * <uint32_t zltail> is the offset to the last entry in the list. This allows
 * a pop operation on the far side of the list without the need for full
 * traversal.
 *
 * zltail> 是到列表中最后一个节点的偏移量(同样为 uint32_t)。
 * 有了这个偏移量，就可以在常数复杂度内对表尾进行操作，而不必遍历整个列表。
 *
 * <uint16_t zllen> is the number of entries. When there are more than
 * 2^16-2 entries, this value is set to 2^16-1 and we need to traverse the
 * entire list to know how many items it holds.
 *
 * <zllen> 是节点的数量，为 ``uint16_t`` 。
 * 当这个值大于 2**16-2 时，需要遍历整个列表，才能计算出列表的长度
 *
 * <uint8_t zlend> is a special entry representing the end of the ziplist.
 * Is encoded as a single byte equal to 255. No other normal entry starts
 * with a byte set to the value of 255.
 *
 * <zlend> 是一个单字节的特殊值，等于 255 ，它标识了列表的末端。
 *
 * ZIPLIST ENTRIES
 * ===============
 *
 * Every entry in the ziplist is prefixed by metadata that contains two pieces
 * of information. First, the length of the previous entry is stored to be
 * able to traverse the list from back to front. Second, the entry encoding is
 * provided. It represents the entry type, integer or string, and in the case
 * of strings it also represents the length of the string payload.
 * So a complete entry is stored like this:
 *
 * <prevlen> <encoding> <entry-data>
 *
 * Sometimes the encoding represents the entry itself, like for small integers
 * as we'll see later. In such a case the <entry-data> part is missing, and we
 * could have just:
 *
 * <prevlen> <encoding>
 *
 * The length of the previous entry, <prevlen>, is encoded in the following way:
 * If this length is smaller than 254 bytes, it will only consume a single
 * byte representing the length as an unsinged 8 bit integer. When the length
 * is greater than or equal to 254, it will consume 5 bytes. The first byte is
 * set to 254 (FE) to indicate a larger value is following. The remaining 4
 * bytes take the length of the previous entry as value.
 *
 * So practically an entry is encoded in the following way:
 *
 * <prevlen from 0 to 253> <encoding> <entry>
 *
 * Or alternatively if the previous entry length is greater than 253 bytes
 * the following encoding is used:
 *
 * 0xFE <4 bytes unsigned little endian prevlen> <encoding> <entry>
 *
 * The encoding field of the entry depends on the content of the
 * entry. When the entry is a string, the first 2 bits of the encoding first
 * byte will hold the type of encoding used to store the length of the string,
 * followed by the actual length of the string. When the entry is an integer
 * the first 2 bits are both set to 1. The following 2 bits are used to specify
 * what kind of integer will be stored after this header. An overview of the
 * different types and encodings is as follows. The first byte is always enough
 * to determine the kind of entry.
 *
 * |00pppppp| - 1 byte
 *      String value with length less than or equal to 63 bytes (6 bits).
 *      "pppppp" represents the unsigned 6 bit length.
 * |01pppppp|qqqqqqqq| - 2 bytes
 *      String value with length less than or equal to 16383 bytes (14 bits).
 *      IMPORTANT: The 14 bit number is stored in big endian.
 * |10000000|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
 *      String value with length greater than or equal to 16384 bytes.
 *      Only the 4 bytes following the first byte represents the length
 *      up to 2^32-1. The 6 lower bits of the first byte are not used and
 *      are set to zero.
 *      IMPORTANT: The 32 bit number is stored in big endian.
 * |11000000| - 3 bytes
 *      Integer encoded as int16_t (2 bytes).
 * |11010000| - 5 bytes
 *      Integer encoded as int32_t (4 bytes).
 * |11100000| - 9 bytes
 *      Integer encoded as int64_t (8 bytes).
 * |11110000| - 4 bytes
 *      Integer encoded as 24 bit signed (3 bytes).
 * |11111110| - 2 bytes
 *      Integer encoded as 8 bit signed (1 byte).
 * |1111xxxx| - (with xxxx between 0001 and 1101) immediate 4 bit integer.
 *      Unsigned integer from 0 to 12. The encoded value is actually from
 *      1 to 13 because 0000 and 1111 can not be used, so 1 should be
 *      subtracted from the encoded 4 bit value to obtain the right value.
 * |11111111| - End of ziplist special entry.
 *
 * Like for the ziplist header, all the integers are represented in little
 * endian byte order, even when this code is compiled in big endian systems.
 *
 * EXAMPLES OF ACTUAL ZIPLISTS
 * ===========================
 *
 * The following is a ziplist containing the two elements representing
 * the strings "2" and "5". It is composed of 15 bytes, that we visually
 * split into sections:
 *
 *  [0f 00 00 00] [0c 00 00 00] [02 00] [00 f3] [02 f6] [ff]
 *        |             |          |       |       |     |
 *     zlbytes        zltail    entries   "2"     "5"   end
 *
 * The first 4 bytes represent the number 15, that is the number of bytes
 * the whole ziplist is composed of. The second 4 bytes are the offset
 * at which the last ziplist entry is found, that is 12, in fact the
 * last entry, that is "5", is at offset 12 inside the ziplist.
 * The next 16 bit integer represents the number of elements inside the
 * ziplist, its value is 2 since there are just two elements inside.
 * Finally "00 f3" is the first entry representing the number 2. It is
 * composed of the previous entry length, which is zero because this is
 * our first entry, and the byte F3 which corresponds to the encoding
 * |1111xxxx| with xxxx between 0001 and 1101. We need to remove the "F"
 * higher order bits 1111, and subtract 1 from the "3", so the entry value
 * is "2". The next entry has a prevlen of 02, since the first entry is
 * composed of exactly two bytes. The entry itself, F6, is encoded exactly
 * like the first entry, and 6-1 = 5, so the value of the entry is 5.
 * Finally the special entry FF signals the end of the ziplist.
 *
 * Adding another element to the above string with the value "Hello World"
 * allows us to show how the ziplist encodes small strings. We'll just show
 * the hex dump of the entry itself. Imagine the bytes as following the
 * entry that stores "5" in the ziplist above:
 *
 * [02] [0b] [48 65 6c 6c 6f 20 57 6f 72 6c 64]
 *
 * The first byte, 02, is the length of the previous entry. The next
 * byte represents the encoding in the pattern |00pppppp| that means
 * that the entry is a string of length <pppppp>, so 0B means that
 * an 11 bytes string follows. From the third byte (48) to the last (64)
 * there are just the ASCII characters for "Hello World".
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2017, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"
#include "redisassert.h"

#define ZIP_END 255         /* Special "end of ziplist" entry. 
                             *
                             * 特殊的“拉链末尾”节点。
                             * */
#define ZIP_BIG_PREVLEN 254 /* ZIP_BIG_PREVLEN - 1 is the max number of bytes of
                               the previous entry, for the "prevlen" field prefixing
                               each entry, to be represented with just a single byte.
                               Otherwise it is represented as FE AA BB CC DD, where
                               AA BB CC DD are a 4 bytes unsigned integer
                               representing the previous entry len. 
                             *
                             * ZIP_BIG_PREVLEN-1是前一个节点的最大字节数，用于前缀为每个节点的
                             * “PREVLEN”字段，仅用一个字节表示。否则，它被表示为FE AA BB CC
                             *  DD，其中AA BB CC DD是一个4字节的无符号整数，表示前一个节点len
                             * 。
                             * */

/* Different encoding/length possibilities 
 *
 * 不同的编码/长度可能性
 * */
#define ZIP_STR_MASK 0xc0
#define ZIP_INT_MASK 0x30
#define ZIP_STR_06B (0 << 6)
#define ZIP_STR_14B (1 << 6)
#define ZIP_STR_32B (2 << 6)
#define ZIP_INT_16B (0xc0 | 0<<4)
#define ZIP_INT_32B (0xc0 | 1<<4)
#define ZIP_INT_64B (0xc0 | 2<<4)
#define ZIP_INT_24B (0xc0 | 3<<4)
#define ZIP_INT_8B 0xfe

/* 4 bit integer immediate encoding |1111xxxx| with xxxx between
 * 0001 and 1101. */
#define ZIP_INT_IMM_MASK 0x0f   /* Mask to extract the 4 bits value. To add
                                   one is needed to reconstruct the value. 
                                 *
                                 * 掩码以提取4位值。需要添加一个来重建值。
                                 * */
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 
                                 *
                                 * 11110001
                                 * */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 
                                 *
                                 * 11111101
                                 * */

#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

/* Macro to determine if the entry is a string. String entries never start
 * with "11" as most significant bits of the first byte. 
 *
 * 宏，以确定节点是否为字符串。字符串节点从不以“11”作为第一个字节的最高有效位开
 * 始。
 * */
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK)

/* Utility macros.
 *
 * 实用程序宏。
 * */

/*
 * 用于取出 zl 各部分值的宏
 *
 * 所有宏复杂度都为 O(1)
 */

/* Return total bytes a ziplist is composed of. 
 *
 * 返回一个ziplist包含的总字节数。
 * */
// 取出列表以字节计算的列表长度(内存的 0 - 31 位，整数)
#define ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl)))

/* Return the offset of the last item inside the ziplist. 
 *
 * 返回ziplist中最后一个项目的偏移量。
 * */
// 取出列表的表尾偏移量(内存的 32 - 63 位，整数)
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))

/* Return the length of a ziplist, or UINT16_MAX if the length cannot be
 * determined without scanning the whole ziplist. 
 *
 * 返回一个ziplist的长度，如果不扫描整个ziplist就无法确定长度，则返回
 * UINT16_MAX。
 * */
// 取出列表的长度(内存的 64 - 79 位，整数)
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))

/* The size of a ziplist header: two 32 bit integers for the total
 * bytes count and last item offset. One 16 bit integer for the number
 * of items field. 
 *
 * ziplist标头的大小：两个32位整数表示总字节数和最后一项偏移量。一个16位
 * 整数表示项目数字段。
 * */
// 列表的 header 长度
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))

/* Size of the "end of ziplist" entry. Just one byte. 
 *
 * “压缩列表末尾”节点的大小。只有一个字节。
 * */
#define ZIPLIST_END_SIZE        (sizeof(uint8_t))

/* Return the pointer to the first entry of a ziplist. 
 *
 * 返回指向ziplist的第一个节点的指针。
 * */
// 返回列表的 header 之后的位置
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)

/* Return the pointer to the last entry of a ziplist, using the
 * last entry offset inside the ziplist header. 
 *
 * 使用ziplist标头内的最后一个节点偏移量，返回指向ziplist最后一个节点
 * 的指针。
 * */
// 返回列表最后一个元素之后的位置
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))

/* Return the pointer to the last byte of a ziplist, which is, the
 * end of ziplist FF entry. 
 *
 * 返回指向ziplist的最后一个字节的指针，即ziplist FF节点的末尾。
 * */
// 返回列表的结束符之前的位置
#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

/* Increment the number of items field in the ziplist header. Note that this
 * macro should never overflow the unsigned 16 bit integer, since entries are
 * always pushed one at a time. When UINT16_MAX is reached we want the count
 * to stay there to signal that a full scan is needed to get the number of
 * items inside the ziplist. 
 *
 * 增加ziplist标题中的项目数字段。请注意，此宏不应使无符号16位整数溢出，因
 * 为节点总是一次推送一个。当达到UINT16_MAX时，我们希望计数保持不变，以表
 * 示需要进行全面扫描才能获得ziplist中的项目数。
 * */
/*
 * 对 <zllen> 增一
 *
 * ZIPLIST_LENGTH(zl) 的最大值为 UINT16_MAX
 *
 * 复杂度：O(1)
 */
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) \
        ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr); \
}

/* Don't let ziplists grow over 1GB in any case, don't wanna risk overflow in
 * zlbytes
 *
 * 在任何情况下都不要让ziplists增长超过1GB，不要冒着zlbytes溢出的
 * 风险
 * */
#define ZIPLIST_MAX_SAFETY_SIZE (1<<30)
int ziplistSafeToAdd(unsigned char* zl, size_t add) {
    size_t len = zl? ziplistBlobLen(zl): 0;
    if (len + add > ZIPLIST_MAX_SAFETY_SIZE)
        return 0;
    return 1;
}


/* We use this function to receive information about a ziplist entry.
 * Note that this is not how the data is actually encoded, is just what we
 * get filled by a function in order to operate more easily. 
 *
 * 我们使用此函数来接收有关压缩列表节点的信息。请注意，这并不是数据的实际编码方式，
 * 只是我们通过函数填充的内容，以便更容易地操作。
 * */
/*
 * 节点结构
 */
typedef struct zlentry {
    // 保存前一节点的长度所需的长度
    unsigned int prevrawlensize; /* Bytes used to encode the previous entry len
                                  *
                                  * 用于对上一个节点len进行编码的字节
                                  * */
    // 前一节点的长度
    unsigned int prevrawlen;     /* Previous entry len. 
                                  *
                                  * 上一个节点len。
                                  * */
    // 保存节点的长度所需的长度
    unsigned int lensize;        /* Bytes used to encode this entry type/len.
                                    For example strings have a 1, 2 or 5 bytes
                                    header. Integers always use a single byte.
                                  *
                                  * 用于对此节点类型/len进行编码的字节。例如，字符串具有1、2或5字节的标头。整
                                  * 数总是使用单个字节。
                                  * */
    // 节点的长度
    unsigned int len;            /* Bytes used to represent the actual entry.
                                    For strings this is just the string length
                                    while for integers it is 1, 2, 3, 4, 8 or
                                    0 (for 4 bit immediate) depending on the
                                    number range. 
                                  *
                                  * 用于表示实际节点的字节。对于字符串，这只是字符串长度，而对于整数，它是1、2、3
                                  * 、4、8或0（对于4位立即数），具体取决于数字范围。
                                  * */
    // header 长度
    unsigned int headersize;     /* prevrawlensize + lensize. 
                                  *
                                  * prerawlensize+镜头。
                                  * */
    // 编码方式
    unsigned char encoding;      /* Set to ZIP_STR_* or ZIP_INT_* depending on
                                    the entry encoding. However for 4 bits
                                    immediate integers this can assume a range
                                    of values and must be range-checked. 
                                  *
                                  * 在节点编码上设置为ZIP_STR_或ZIP_INT_depending。然而，对
                                  * 于4位立即数整数，这可以假设一个值范围，并且必须进行范围检查。
                                  * */
    // 内容
    unsigned char *p;            /* Pointer to the very start of the entry, that
                                    is, this points to prev-entry-len field. 
                                  *
                                  * 指向节点最开始的指针，也就是说，它指向前一个节点len字段。
                                  * */
} zlentry;

#define ZIPLIST_ENTRY_ZERO(zle) { \
    (zle)->prevrawlensize = (zle)->prevrawlen = 0; \
    (zle)->lensize = (zle)->len = (zle)->headersize = 0; \
    (zle)->encoding = 0; \
    (zle)->p = NULL; \
}

/* Extract the encoding from the byte pointed by 'ptr' and set it into
 * 'encoding' field of the zlentry structure. 
 *
 * 从“ptr”指向的字节中提取编码，并将其设置为zlentry结构的“encoding”字段。
 * */
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {  \
    (encoding) = (ptr[0]); \
    if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while(0)

/* Return bytes needed to store integer encoded by 'encoding'. 
 *
 * 返回存储由“encoding”编码的整数所需的字节。
 * */
unsigned int zipIntSize(unsigned char encoding) {
    switch(encoding) {
    case ZIP_INT_8B:  return 1;
    case ZIP_INT_16B: return 2;
    case ZIP_INT_24B: return 3;
    case ZIP_INT_32B: return 4;
    case ZIP_INT_64B: return 8;
    }
    if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
        return 0; /* 4 bit immediate 
                   *
                   * 4位立即数
                   * */
    panic("Invalid integer encoding 0x%02X", encoding);
    return 0;
}

/* Write the encoding header of the entry in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. Arguments:
 *
 * 'encoding' is the encoding we are using for the entry. It could be
 * ZIP_INT_* or ZIP_STR_* or between ZIP_INT_IMM_MIN and ZIP_INT_IMM_MAX
 * for single-byte small immediate integers.
 *
 * 'rawlen' is only used for ZIP_STR_* encodings and is the length of the
 * string that this entry represents.
 *
 * The function returns the number of bytes used by the encoding/length
 * header stored in 'p'. 
 *
 * 在“p”中写入节点的编码标头。如果p为NULL，它只返回编码这样一个长度所需的字
 * 节数。参数：“encoding”是我们用于节点的编码。对于单字节小立即数，它可以
 * 是ZIP_INT_or ZIP_STR_或介于ZIP_INT_IMM_MIN和ZIP_INT-IMM_MAX之间rawlen’仅用于ZIP_STR_encodings，
 * 是此节点所代表的字符串的长度。该函数返回存储在“p”中的编码/长度标头所使用的字节数。
 * */
unsigned int zipStoreEntryEncoding(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
    unsigned char len = 1, buf[5];

    if (ZIP_IS_STR(encoding)) {
        /* Although encoding is given it may not be set for strings,
         * so we determine it here using the raw length. 
         *
         * 虽然给出了编码，但可能不会为字符串设置，所以我们在这里使用原始长度来确定它。
         * */
        if (rawlen <= 0x3f) {
            if (!p) return len;
            buf[0] = ZIP_STR_06B | rawlen;
        } else if (rawlen <= 0x3fff) {
            len += 1;
            if (!p) return len;
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
            buf[1] = rawlen & 0xff;
        } else {
            len += 4;
            if (!p) return len;
            buf[0] = ZIP_STR_32B;
            buf[1] = (rawlen >> 24) & 0xff;
            buf[2] = (rawlen >> 16) & 0xff;
            buf[3] = (rawlen >> 8) & 0xff;
            buf[4] = rawlen & 0xff;
        }
    } else {
        /* Implies integer encoding, so length is always 1. 
         *
         * 表示整数编码，因此长度始终为1。
         * */
        if (!p) return len;
        buf[0] = encoding;
    }

    /* Store this length at p. 
     *
     * 将此长度存储在p。
     * */
    memcpy(p,buf,len);
    return len;
}

/* Decode the entry encoding type and data length (string length for strings,
 * number of bytes used for the integer for integer entries) encoded in 'ptr'.
 * The 'encoding' variable will hold the entry encoding, the 'lensize'
 * variable will hold the number of bytes required to encode the entry
 * length, and the 'len' variable will hold the entry length. 
 *
 * 解码“ptr”中编码的节点编码类型和数据长度（字符串的字符串长度，整数节点的整数
 * 所用的字节数）。“encoding”变量将保留节点编码，“lensize”变量将
 * 保存对节点长度进行编码所需的字节数，“len”变量将保持节点长度。
 * */
/*
 * 从 ptr 指针中取出节点的编码、保存节点长度所需的长度、以及节点的长度
 *
 * 复杂度：O(1)
 *
 * 返回值：
 *  int 编码节点所需的长度
 */
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                    \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                                     \
    if ((encoding) < ZIP_STR_MASK) {                                           \
        if ((encoding) == ZIP_STR_06B) {                                       \
            (lensize) = 1;                                                     \
            (len) = (ptr)[0] & 0x3f;                                           \
        } else if ((encoding) == ZIP_STR_14B) {                                \
            (lensize) = 2;                                                     \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                       \
        } else if ((encoding) == ZIP_STR_32B) {                                \
            (lensize) = 5;                                                     \
            (len) = ((uint32_t)(ptr)[1] << 24) |                               \
                    ((uint32_t)(ptr)[2] << 16) |                               \
                    ((uint32_t)(ptr)[3] <<  8) |                               \
                    ((uint32_t)(ptr)[4]);                                      \
        } else {                                                               \
            panic("Invalid string encoding 0x%02X", (encoding));               \
        }                                                                      \
    } else {                                                                   \
        (lensize) = 1;                                                         \
        (len) = zipIntSize(encoding);                                          \
    }                                                                          \
} while(0)

/* Encode the length of the previous entry and write it to "p". This only
 * uses the larger encoding (required in __ziplistCascadeUpdate). 
 *
 * 对上一个节点的长度进行编码，并将其写入“p”。这只使用较大的编码（__ziplistCascadeUpdate中需要）。
 * */
int zipStorePrevEntryLengthLarge(unsigned char *p, unsigned int len) {
    if (p != NULL) {
        p[0] = ZIP_BIG_PREVLEN;
        memcpy(p+1,&len,sizeof(len));
        memrev32ifbe(p+1);
    }
    return 1+sizeof(len);
}

/* Encode the length of the previous entry and write it to "p". Return the
 * number of bytes needed to encode this length if "p" is NULL. 
 *
 * 对上一个节点的长度进行编码，并将其写入“p”。如果“p”为NULL，则返回对此长度进行编码所需的字节数。
 * */
unsigned int zipStorePrevEntryLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return (len < ZIP_BIG_PREVLEN) ? 1 : sizeof(len)+1;
    } else {
        if (len < ZIP_BIG_PREVLEN) {
            p[0] = len;
            return 1;
        } else {
            return zipStorePrevEntryLengthLarge(p,len);
        }
    }
}

/* Return the number of bytes used to encode the length of the previous
 * entry. The length is returned by setting the var 'prevlensize'. 
 *
 * 返回用于对上一个节点的长度进行编码的字节数。通过设置 var “prevlensize”返回长度。
 * */
/*
 * 从指针 ptr 中取出保存前一个节点的长度所需的字节数
 *
 * 复杂度：O(1)
 *
 * 返回值：
 *  unsigned int
 */
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                          \
    if ((ptr)[0] < ZIP_BIG_PREVLEN) {                                          \
        (prevlensize) = 1;                                                     \
    } else {                                                                   \
        (prevlensize) = 5;                                                     \
    }                                                                          \
} while(0)

/* Return the length of the previous element, and the number of bytes that
 * are used in order to encode the previous element length.
 * 'ptr' must point to the prevlen prefix of an entry (that encodes the
 * length of the previous entry in order to navigate the elements backward).
 * The length of the previous entry is stored in 'prevlen', the number of
 * bytes needed to encode the previous entry length are stored in
 * 'prevlensize'. 
 *
 * 返回上一个元素的长度，以及用于对上一个元件长度进行编码的字节数。'ptr'必须指
 * 向节点的prevlen前缀（对上一个节点的长度进行编码，以便向后导航元素）。前一
 * 节点的长度存储在“prevlen”中，对前一条长度进行编码所需的字节数存储在“prevlensize”中。
 * */
/*
* 从指针 ptr 中取出前一个节点的长度
*
* 复杂度：O(1)
*
* 返回值：
*   unsigned int
*/
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                     \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
    if ((prevlensize) == 1) {                                                  \
        (prevlen) = (ptr)[0];                                                  \
    } else if ((prevlensize) == 5) {                                           \
        assert(sizeof((prevlen)) == 4);                                        \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);                             \
        memrev32ifbe(&prevlen);                                                \
    }                                                                          \
} while(0)

/* Given a pointer 'p' to the prevlen info that prefixes an entry, this
 * function returns the difference in number of bytes needed to encode
 * the prevlen if the previous entry changes of size.
 *
 * So if A is the number of bytes used right now to encode the 'prevlen'
 * field.
 *
 * And B is the number of bytes that are needed in order to encode the
 * 'prevlen' if the previous element will be updated to one of size 'len'.
 *
 * Then the function returns B - A
 *
 * So the function returns a positive number if more space is needed,
 * a negative number if less space is needed, or zero if the same space
 * is needed. 
 *
 * 给定指向前缀为节点的prevlen信息的指针“p”，如果前一个节点的大小发生变化，
 * 此函数将返回对prevlen进行编码所需的字节数差异。
 *
 * 因此，如果A是目前用于编码“prevlen”字段的字节数。B是如果前一个元素将更新为大小为“len”的元
 * 素，则对“prevlen”进行编码所需的字节数。然后函数返回B-A，所以如果需要
 * 更多的空间，函数返回一个正数，如果需要更少的空间，则返回一个负数，或者如果需要相同的空间，返回零。
 * */
/*
 * 返回编码 len 所需的长度减去编码 p 的前一个节点的大小所需的长度之差
 *
 * 复杂度：O(1)
 */
int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);
    return zipStorePrevEntryLength(NULL, len) - prevlensize;
}

/* Return the total number of bytes used by the entry pointed to by 'p'. 
 *
 * 返回由“p”指向的节点所使用的字节总数。
 * */
unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int prevlensize, encoding, lensize, len;
    // 保存前驱节点长度的空间长度
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);
    // 保存本节点的空间长度
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
    return prevlensize + lensize + len;
}

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'. 
 *
 * 检查“entry”指向的字符串是否可以编码为整数。将整数值存储在“v”中，并将其
 * 编码存储在“encoding”中。
 * */
int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
    long long value;

    if (entrylen >= 32 || entrylen == 0) return 0;
    // 尝试转换为整数
    if (string2ll((char*)entry,entrylen,&value)) {
        /* Great, the string can be encoded. Check what's the smallest
         * of our encoding types that can hold this value. 
         *
         * 太好了，字符串可以被编码。检查我们的编码类型中可以保持此值的最小编码类型。
         * */
        // 选择整数编码
        if (value >= 0 && value <= 12) {
            *encoding = ZIP_INT_IMM_MIN+value;
        } else if (value >= INT8_MIN && value <= INT8_MAX) {
            *encoding = ZIP_INT_8B;
        } else if (value >= INT16_MIN && value <= INT16_MAX) {
            *encoding = ZIP_INT_16B;
        } else if (value >= INT24_MIN && value <= INT24_MAX) {
            *encoding = ZIP_INT_24B;
        } else if (value >= INT32_MIN && value <= INT32_MAX) {
            *encoding = ZIP_INT_32B;
        } else {
            *encoding = ZIP_INT_64B;
        }
        *v = value;
        return 1;
    }
    return 0;
}

/* Store integer 'value' at 'p', encoded as 'encoding' 
 *
 * 将整数“value”存储在“p”，编码为“encoding”
 * */
void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;
    // 8 bit 整数
    if (encoding == ZIP_INT_8B) {
        ((int8_t*)p)[0] = (int8_t)value;
    } else if (encoding == ZIP_INT_16B) {
        // 16 bit 整数

        i16 = value;
        memcpy(p,&i16,sizeof(i16));
        memrev16ifbe(p);
    } else if (encoding == ZIP_INT_24B) {
        // 24 bit 整数

        i32 = ((uint64_t)value)<<8;
        memrev32ifbe(&i32);
        memcpy(p,((uint8_t*)&i32)+1,sizeof(i32)-sizeof(uint8_t));
    } else if (encoding == ZIP_INT_32B) {
        // 32 bit 整数

        i32 = value;
        memcpy(p,&i32,sizeof(i32));
        memrev32ifbe(p);
    } else if (encoding == ZIP_INT_64B) {
        // 64 bit 整数

        i64 = value;
        memcpy(p,&i64,sizeof(i64));
        memrev64ifbe(p);
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        // 值和编码保存在同一个 byte
        /* Nothing to do, the value is stored in the encoding itself. 
         *
         * 无需执行任何操作，值存储在编码本身中。
         * */
    } else {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p' 
 *
 * 从“p”读取编码为“encoding”的整数
 * */
int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;
    // 8 bit
    if (encoding == ZIP_INT_8B) {
        ret = ((int8_t*)p)[0];
    } else if (encoding == ZIP_INT_16B) {
        // 16 bit

        memcpy(&i16,p,sizeof(i16));
        memrev16ifbe(&i16);
        ret = i16;
    } else if (encoding == ZIP_INT_32B) {
        // 32 bit

        memcpy(&i32,p,sizeof(i32));
        memrev32ifbe(&i32);
        ret = i32;
    } else if (encoding == ZIP_INT_24B) {
        // 24 bit

        i32 = 0;
        memcpy(((uint8_t*)&i32)+1,p,sizeof(i32)-sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32>>8;
    } else if (encoding == ZIP_INT_64B) {
        // 64 bit

        memcpy(&i64,p,sizeof(i64));
        memrev64ifbe(&i64);
        ret = i64;
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        // 值和编码保存在同一个 byte
        ret = (encoding & ZIP_INT_IMM_MASK)-1;
    } else {
        assert(NULL);
    }
    return ret;
}

/* Return a struct with all information about an entry. 
 *
 * 返回一个结构，其中包含有关某个项的所有信息。
 * 从指针 p 中提取出节点的各个属性，并将属性保存到 zlentry 结构，然后返回
 * */
void zipEntry(unsigned char *p, zlentry *e) {

    // 取出前一个节点的长度
    ZIP_DECODE_PREVLEN(p, e->prevrawlensize, e->prevrawlen);
    // 取出当前节点的编码、保存节点的长度所需的长度、以及节点的长度
    ZIP_DECODE_LENGTH(p + e->prevrawlensize, e->encoding, e->lensize, e->len);
    // 记录 header 的长度
    e->headersize = e->prevrawlensize + e->lensize;
    // 记录指针 p
    e->p = p;
}

/* Create a new empty ziplist. 
 *
 * 创建一个新的空压缩列表。
 * */
unsigned char *ziplistNew(void) {
    // 分配 2 个 32 bit，一个 16 bit，以及一个 8 bit
    // 分别用于 <zlbytes><zltail><zllen> 和 <zlend>
    unsigned int bytes = ZIPLIST_HEADER_SIZE+ZIPLIST_END_SIZE;
    unsigned char *zl = zmalloc(bytes);
    // 设置长度
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
    // 设置表尾偏移量
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
    // 设置列表项数量
    ZIPLIST_LENGTH(zl) = 0;
    // 设置表尾标识
    zl[bytes-1] = ZIP_END;
    return zl;
}

/* Resize the ziplist. 
 *
 * 调整拉链头的大小。
 * */
/*
 * 对 zl 进行空间重非配，并更新相关属性
 *
 * 复杂度：O(N)
 *
 * 返回值：更新后的 ziplist
 */
unsigned char *ziplistResize(unsigned char *zl, size_t len) {
    assert(len < UINT32_MAX);
    // 重分配
    zl = zrealloc(zl,len);
    // 更新长度
    ZIPLIST_BYTES(zl) = intrev32ifbe(len);
    // 设置表尾
    zl[len-1] = ZIP_END;
    return zl;
}

/* When an entry is inserted, we need to set the prevlen field of the next
 * entry to equal the length of the inserted entry. It can occur that this
 * length cannot be encoded in 1 byte and the next entry needs to be grow
 * a bit larger to hold the 5-byte encoded prevlen. This can be done for free,
 * because this only happens when an entry is already being inserted (which
 * causes a realloc and memmove). However, encoding the prevlen may require
 * that this entry is grown as well. This effect may cascade throughout
 * the ziplist when there are consecutive entries with a size close to
 * ZIP_BIG_PREVLEN, so we need to check that the prevlen can be encoded in
 * every consecutive entry.
 *
 * Note that this effect can also happen in reverse, where the bytes required
 * to encode the prevlen field can shrink. This effect is deliberately ignored,
 * because it can cause a "flapping" effect where a chain prevlen fields is
 * first grown and then shrunk again after consecutive inserts. Rather, the
 * field is allowed to stay larger than necessary, because a large prevlen
 * field implies the ziplist is holding large entries anyway.
 *
 * The pointer "p" points to the first entry that does NOT need to be
 * updated, i.e. consecutive fields MAY need an update. 
 *
 * 当插入一个节点时，我们需要将下一个节点的prevlen字段设置为等于插入节点的长
 * 度。可能发生的情况是，此长度不能用1字节编码，并且下一个节点需要增长一点以容纳5
 * 字节编码的prevlen。这可以免费完成，因为只有当一个节点已经插入时才会发生这
 * 种情况（这会导致realloc和memmove）。然而，对prevlen进行编码
 * 可能也需要增加此节点。当存在大小接近ZIP_BIG_PREVLEN的连续节点时，
 * 这种效果可能会在整个ziplist中级联，因此我们需要检查PREVLEN是否可以
 * 在每个连续节点中编码。
 *
 * 请注意，这种效果也可能反过来发生，对prevlen字段进行
 * 编码所需的字节可能会缩小。这种效应被故意忽略，因为它会导致“拍打”效应，即先生长
 * 一个链prevlen字段，然后在连续插入后再次收缩。相反，允许该字段保持大于所需
 * 的大小，因为较大的prevlen字段意味着ziplist无论如何都包含较大的节点。
 *
 * 指针“p”指向不需要更新的第一个节点，即连续字段可能需要更新。
 * */
/*
 * 当将一个新节点添加到某个节点之前的时候，
 * 如果原节点的 prevlen 不足以保存新节点的长度，
 * 那么就需要对原节点的空间进行扩展（从 1 字节扩展到 5 字节）。
 *
 * 但是，当对原节点进行扩展之后，原节点的下一个节点的 prevlen 可能出现空间不足，
 * 这种情况在多个连续节点的长度都接近 ZIP_BIGLEN 时可能发生。
 *
 * 这个函数就用于处理这种连续扩展动作。
 *
 * 因为节点的长度变小而引起的连续缩小也是可能出现的，
 * 不过，为了避免扩展-缩小-扩展-缩小这样的情况反复出现（flapping，抖动），
 * 我们不处理这种情况，而是任由 prevlen 比所需的长度更长
 *
 * 复杂度：O(N^2)
 *
 * 返回值：更新后的 ziplist
 */
unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;
    size_t offset, noffset, extra;
    unsigned char *np;
    zlentry cur, next;

    // 一直更新直到表尾
    while (p[0] != ZIP_END) {
        // 当前节点
        zipEntry(p, &cur);
        // 当前节点的长度
        rawlen = cur.headersize + cur.len;
        // 编码当前节点的长度所需的空间大小
        rawlensize = zipStorePrevEntryLength(NULL,rawlen);

        /* Abort if there is no next entry. 
         *
         * 如果没有下一个节点，则中止。
         * */
        // 已经到达表尾，退出
        if (p[rawlen] == ZIP_END) break;
        // 取出下一节点
        zipEntry(p+rawlen, &next);

        /* Abort when "prevlen" has not changed. 
         *
         * 当“prevlen”未更改时中止。
         * */
        // 如果下一的 prevlen 等于当前节点的 rawlen
        // 那么说明编码大小无需改变，退出
        if (next.prevrawlen == rawlen) break;

        // 下一节点的长度编码空间不足，进行扩展
        if (next.prevrawlensize < rawlensize) {
            /* The "prevlen" field of "next" needs more bytes to hold
             * the raw length of "cur". 
             *
             * “next”的“prevlen”字段需要更多的字节来容纳“cur”的原始长度。
             * */
            offset = p-zl;
            // 需要多添加的长度
            extra = rawlensize-next.prevrawlensize;
            // 重分配，复杂度为 O(N)
            zl = ziplistResize(zl,curlen+extra);
            p = zl+offset;

            /* Current pointer and offset for next element. 
             *
             * 下一个元素的当前指针和偏移量。
             * */
            np = p+rawlen;
            noffset = np-zl;

            /* Update tail offset when next element is not the tail element. 
             *
             * 当下一个元素不是尾部元素时更新尾部偏移。
             * */
            if ((zl+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np) {
                ZIPLIST_TAIL_OFFSET(zl) =
                    intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+extra);
            }

            /* Move the tail to the back. 
             *
             * 把尾巴移到后面。
             * */
            // 为获得空间而进行数据移动，复杂度万恶 O(N)
            memmove(np+rawlensize,
                np+next.prevrawlensize,
                curlen-noffset-next.prevrawlensize-1);
            zipStorePrevEntryLength(np,rawlen);

            /* Advance the cursor 
             *
             * 移动光标
             * */
            p += rawlen;
            curlen += extra;
        } else {
            // 下一节点的长度编码空间有多余，不进行收缩
            // 只是将被编码的长度写入空间
            if (next.prevrawlensize > rawlensize) {
                /* This would result in shrinking, which we want to avoid.
                 * So, set "rawlen" in the available bytes. 
                 *
                 * 这将导致收缩，我们希望避免这种情况。因此，在可用字节中设置“rawlen”。
                 * */
                zipStorePrevEntryLengthLarge(p+rawlen,rawlen);
            } else {
                zipStorePrevEntryLength(p+rawlen,rawlen);
            }

            /* Stop here, as the raw length of "next" has not changed. 
             *
             * 到此为止，因为“next”的原始长度没有改变。
             * */
            break;
        }
    }
    return zl;
}

/* Delete "num" entries, starting at "p". Returns pointer to the ziplist. 
 *
 * 删除从“p”开始的“num”节点。返回指向压缩列表的指针。
 * */
/*
 * 从指针 p 开始，删除 num 个节点
 *
 * 复杂度：O(N^2)
 *
 * 返回值：删除元素后的 ziplist
 */
unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    // 首个节点
    zipEntry(p, &first);
    // 累积起所有删除目标（节点）的编码长度
    // 并移动指针 p
    // 复杂度 O(N)
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p);
        deleted++;
    }

    // 被删除的节点的 byte 总和
    totlen = p-first.p; /* Bytes taken by the element(s) to delete. 
                         *
                         * 要删除的元素占用的字节数。
                         * */
    if (totlen > 0) {
        if (p[0] != ZIP_END) {
            /* Storing `prevrawlen` in this entry may increase or decrease the
             * number of bytes required compare to the current `prevrawlen`.
             * There always is room to store this, because it was previously
             * stored by an entry that is now being deleted. 
             *
             * 与当前的“prevrawlen”相比，在该节点中存储“prevrrawlen”可能会增加或减少所需的字节数。
             * 总有存储空间，因为它以前是由一个正在删除的节点存储的。
             * */
            // 更新最后一个被删除的节点之后的一个节点，
            // 将它的 prevlan 值设置为 first.prevrawlen ，
            // 也即是被删除的第一个节点的前一个节点的长度
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);

            /* Note that there is always space when p jumps backward: if
             * the new previous entry is large, one of the deleted elements
             * had a 5 bytes prevlen header, so there is for sure at least
             * 5 bytes free and we need just 4. 
             *
             * 请注意，当p向后跳时总是有空间的：如果上一个新节点很大，那么其中一个删除的元素有
             * 一个5字节的prevlen头，所以肯定至少有5个字节可用，我们只需要4个。
             * */
            p -= nextdiff;
            zipStorePrevEntryLength(p,first.prevrawlen);

            /* Update offset for tail 
             *
             * 更新尾部偏移
             * */
            // 更新 ziplist 到表尾的偏移量
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);

            /* When the tail contains more than one entry, we need to take
             * "nextdiff" in account as well. Otherwise, a change in the
             * size of prevlen doesn't have an effect on the *tail* offset. 
             *
             * 当尾部包含多个节点时，我们也需要考虑“nextdiff”。否则，prevlen大
             * 小的变化不会对*tailoffset产生影响。
             * */
            // 跟新 ziplist 的偏移量，如果有需要的话，算上 nextdiff
            zipEntry(p, &tail);
            if (p[tail.headersize+tail.len] != ZIP_END) {
                ZIPLIST_TAIL_OFFSET(zl) =
                   intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
            }

            /* Move tail to the front of the ziplist 
             *
             * 将尾部移到拉链的前面
             * */
            // 前移内存中的数据，覆盖原本的被删除数据
            // 复杂度 O(N)
            memmove(first.p,p,
                intrev32ifbe(ZIPLIST_BYTES(zl))-(p-zl)-1);
        } else {
            /* The entire tail was deleted. No need to move memory. 
             *
             * 整个尾部都被删除了。无需移动内存。
             * */
            // 被删除的是尾节点，无须内存移动，直接更新偏移值就可以了
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe((first.p-zl)-first.prevrawlen);
        }

        /* Resize and update length 
         *
         * 调整和更新长度
         * */
        // 调整大小，并更新 ziplist 的长度
        // 复杂度 O(N)
        offset = first.p-zl;
        zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl))-totlen+nextdiff);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
        p = zl+offset;

        /* When nextdiff != 0, the raw length of the next entry has changed, so
         * we need to cascade the update throughout the ziplist 
         *
         * 当nextdiff！=0，下一个节点的原始长度已经更改，因此我们需要在整个ziplist中级联更新
         * */
        // 层级更新
        if (nextdiff != 0)
            // 复杂度 O(N^2)
            zl = __ziplistCascadeUpdate(zl,p);
    }
    return zl;
}

/* Insert item at "p". 
 *
 * 在“p”处插入项目。
 * */
/*
 * 添加保存给定元素 s 的新节点到地址 p
 *
 * 复杂度：O(N^2)
 *
 * 返回值：删除元素后的 ziplist
 */
unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen;
    unsigned int prevlensize, prevlen = 0;
    size_t offset;
    int nextdiff = 0;
    unsigned char encoding = 0;
    long long value = 123456789; /* initialized to avoid warning. Using a value
                                    that is easy to see if for some reason
                                    we use it uninitialized. 
                                  *
                                  * 已初始化以避免警告。如果出于某种原因，我们在未初始化的情况下使用一个易于查看的值。
                                  * */
    zlentry tail;

    /* Find out prevlen for the entry that is inserted. 
     *
     * 找出插入节点的前缀。
     * */
    // 如果 p 之后不是没有节点（不是插入到末端）
    // 那么取出节点相关资料，以及 prevlen
    if (p[0] != ZIP_END) {
        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
    } else {
        // 获取列表最后一个节点（表尾）的地址
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
        // 如果地址之后不是末端（也即是，列表至少有一个节点）
        if (ptail[0] != ZIP_END) {
            // 保存 ptail 指向的节点的空间长度
            prevlen = zipRawEntryLength(ptail);
        }
    }

    /* See if the entry can be encoded 
     *
     * 查看节点是否可以编码
     * */
    // 查看能否将新值保存为整数
    // 如果可以的话返回 1 ，
    // 并将新值保存到 value ，编码形式保存到 encoding
    if (zipTryEncoding(s,slen,&value,&encoding)) {
        /* 'encoding' is set to the appropriate integer encoding 
         *
         * “encoding”设置为适当的整数编码
         * */
        reqlen = zipIntSize(encoding);
    } else {
        /* 'encoding' is untouched, however zipStoreEntryEncoding will use the
         * string length to figure out how to encode it. 
         *
         * “encoding”未受影响，但是zipStoreEntryEncoding将使用字符串长度来确定如何对其进行编码。
         * */
        reqlen = slen;
    }
    /* We need space for both the length of the previous entry and
     * the length of the payload. 
     *
     * 我们需要为前一个节点的长度和有效载荷的长度留出空间。
     * */
    // 计算编码 prevlen 所需的长度
    reqlen += zipStorePrevEntryLength(NULL,prevlen);
    // 计算编码 slen 所需的长度
    reqlen += zipStoreEntryEncoding(NULL,encoding,slen);

    /* When the insert position is not equal to the tail, we need to
     * make sure that the next entry can hold this entry's length in
     * its prevlen field. 
     *
     * 当插入位置不等于尾部时，我们需要确保下一个节点可以在其prevlen字段中保持该
     * 节点的长度。
     * */
    int forcelarge = 0;
    // 如果添加的位置不是表尾，那么必须确定后继节点的 prevlen 空间
    // 足以保存新节点的编码长度
    // zipPrevLenByteDiff 的返回值有三种可能：
    // 1）新旧两个节点的编码长度相等，返回 0
    // 2）新节点编码长度 > 旧节点编码长度，返回 5 - 1 = 4
    // 3）旧节点编码长度 > 新编码节点长度，返回 1 - 5 = -4
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0;
    if (nextdiff == -4 && reqlen < 4) {
        nextdiff = 0;
        forcelarge = 1;
    }

    /* Store offset because a realloc may change the address of zl. 
     *
     * 存储偏移量，因为realloc可能会更改zl的地址。
     * */
    // 保存偏移量，因为重分配空间有可能改变 zl 的内存地址
    offset = p-zl;
    // 重分配空间，并更新长度属性和表尾 新空间长度 = 现有长度 + 新节点所需长度 + 编码新节点长度所需的长度差 O(N)
    zl = ziplistResize(zl,curlen+reqlen+nextdiff);
    // 更新 p 的指针
    p = zl+offset;

    /* Apply memory move when necessary and update tail offset. 
     *
     * 必要时应用内存移动并更新尾部偏移。
     * */
    // 如果新节点不是添加到列表末端，那么它后面就有其他节点 因此，我们需要移动这部分节点
    if (p[0] != ZIP_END) {
        /* Subtract one because of the ZIP_END bytes 
         *
         * 由于ZIP_END字节而减去一
         * */
        // 向右移动移原有数据，为新节点让出空间
        // O(N)
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff);

        /* Encode this entry's raw length in the next entry. 
         *
         * 在下一个节点中对该节点的原始长度进行编码。
         * */
        if (forcelarge)
            zipStorePrevEntryLengthLarge(p+reqlen,reqlen);
        else
            // 将本节点的长度编码至下一节点
            zipStorePrevEntryLength(p+reqlen,reqlen);

        /* Update offset for tail 
         *
         * 更新尾部偏移
         * */
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);

        /* When the tail contains more than one entry, we need to take
         * "nextdiff" in account as well. Otherwise, a change in the
         * size of prevlen doesn't have an effect on the *tail* offset. 
         *
         * 当尾部包含多个节点时，我们也需要考虑“nextdiff”。否则，prevlen大
         * 小的变化不会对*tailoffset产生影响。
         * */
        // 有需要的话，将 nextdiff 也加上到 zltail 上
        zipEntry(p+reqlen, &tail);
        if (p[reqlen+tail.headersize+tail.len] != ZIP_END) {
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
        }
    } else {
        /* This element will be the new tail. 
         *
         * 这个元素将成为新的尾巴。
         * */
        // 更新 ziplist 的 zltail 属性，现在新添加节点为表尾节点
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p-zl);
    }

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade the update throughout the ziplist 
     *
     * 当nextdiff！=0，下一个节点的原始长度已经更改，因此我们需要在整个ziplist中级联更新
     * */
    if (nextdiff != 0) {
        offset = p-zl;
        zl = __ziplistCascadeUpdate(zl,p+reqlen);
        p = zl+offset;
    }

    /* Write the entry 
     *
     * 编写节点
     * */
    // 写入数据到节点

    // 编码上一节点的长度，并向后移动指针
    p += zipStorePrevEntryLength(p,prevlen);
    // 编码本节点的长度和类型，并向后移动指针
    p += zipStoreEntryEncoding(p,encoding,slen);
    // 写入内容到节点
    if (ZIP_IS_STR(encoding)) {
        memcpy(p,s,slen);
    } else {
        zipSaveInteger(p,value,encoding);
    }
    // 更新节点数量
    ZIPLIST_INCR_LENGTH(zl,1);
    return zl;
}

/* Merge ziplists 'first' and 'second' by appending 'second' to 'first'.
 *
 * NOTE: The larger ziplist is reallocated to contain the new merged ziplist.
 * Either 'first' or 'second' can be used for the result.  The parameter not
 * used will be free'd and set to NULL.
 *
 * After calling this function, the input parameters are no longer valid since
 * they are changed and free'd in-place.
 *
 * The result ziplist is the contents of 'first' followed by 'second'.
 *
 * On failure: returns NULL if the merge is impossible.
 * On success: returns the merged ziplist (which is expanded version of either
 * 'first' or 'second', also frees the other unused input ziplist, and sets the
 * input ziplist argument equal to newly reallocated ziplist return value. 
 *
 * 通过在“first”后面附加“second”来合并压缩列表“first和”second“。
 *
 * 注意：较大的ziplist会被重新分配，以包含新的合并ziplist。
 * 结果可以使用“first”或“second”。未使用的参数将是空闲的，并设置为NULL。
 *
 *
 * 调用此函数后，输入参数将不再有效，因为它们已更改并在适当位置释放。结果ziplist是“first”后面跟“second”的内容。
 *
 * 失败时：如果无法合并，则返回NULL。
 * 成功时：返回合并的ziplist（它是“first”或“second”的扩展版本，还释放其他未使用的输入ziplist，
 *        并将输入ziplit参数设置为新重新分配的ziplit返回值。
 * */
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second) {
    /* If any params are null, we can't merge, so NULL. 
     *
     * 如果任何params为null，我们就不能合并，所以为null。
     * */
    if (first == NULL || *first == NULL || second == NULL || *second == NULL)
        return NULL;

    /* Can't merge same list into itself. 
     *
     * 无法将同一列表合并到自身中。
     * */
    if (*first == *second)
        return NULL;

    size_t first_bytes = intrev32ifbe(ZIPLIST_BYTES(*first));
    size_t first_len = intrev16ifbe(ZIPLIST_LENGTH(*first));

    size_t second_bytes = intrev32ifbe(ZIPLIST_BYTES(*second));
    size_t second_len = intrev16ifbe(ZIPLIST_LENGTH(*second));

    int append;
    unsigned char *source, *target;
    size_t target_bytes, source_bytes;
    /* Pick the largest ziplist so we can resize easily in-place.
     * We must also track if we are now appending or prepending to
     * the target ziplist. 
     *
     * 选择最大的压缩列表，这样我们就可以在适当的位置轻松调整大小。我们还必须跟踪我们现
     * 在是否正在添加或准备添加到目标ziplist。
     * */
    if (first_len >= second_len) {
        /* retain first, append second to first. 
         *
         * 保留第一个，将第二个附加到第一个。
         * */
        target = *first;
        target_bytes = first_bytes;
        source = *second;
        source_bytes = second_bytes;
        append = 1;
    } else {
        /* else, retain second, prepend first to second. 
         *
         * 否则，保留第二名，准备从第一名到第二名。
         * */
        target = *second;
        target_bytes = second_bytes;
        source = *first;
        source_bytes = first_bytes;
        append = 0;
    }

    /* Calculate final bytes (subtract one pair of metadata) 
     *
     * 计算最后的字节（减去一对元数据）
     * */
    size_t zlbytes = first_bytes + second_bytes -
                     ZIPLIST_HEADER_SIZE - ZIPLIST_END_SIZE;
    size_t zllength = first_len + second_len;

    /* Combined zl length should be limited within UINT16_MAX 
     *
     * 组合zl长度应限制在UINT16_MAX内
     * */
    zllength = zllength < UINT16_MAX ? zllength : UINT16_MAX;

    /* larger values can't be stored into ZIPLIST_BYTES 
     *
     * 较大的值不能存储到 ZIPLIST_BYTES 中
     * */
    assert(zlbytes < UINT32_MAX);

    /* Save offset positions before we start ripping memory apart. 
     *
     * 在我们开始拆分内存之前保存偏移位置。
     * */
    size_t first_offset = intrev32ifbe(ZIPLIST_TAIL_OFFSET(*first));
    size_t second_offset = intrev32ifbe(ZIPLIST_TAIL_OFFSET(*second));

    /* Extend target to new zlbytes then append or prepend source. 
     *
     * 将目标扩展到新的 zlbytes，然后附加或预附加源。
     * */
    target = zrealloc(target, zlbytes);
    if (append) {
        /* append == appending to target 
         *
         * append==附加到目标
         * */

        /* Copy source after target (copying over original [END]):
         *   [TARGET - END, SOURCE - HEADER] 
         *
         * 在目标之后复制源（在原始[END]上复制）：[target-END，source-HEADER]
         * */
        memcpy(target + target_bytes - ZIPLIST_END_SIZE,
               source + ZIPLIST_HEADER_SIZE,
               source_bytes - ZIPLIST_HEADER_SIZE);
    } else {
        /* !append == prepending to target 
         *
         * ！append==准备到目标
         * */

        /* Move target *contents* exactly size of (source - [END]),
         * then copy source into vacated space (source - [END]):
         *   [SOURCE - END, TARGET - HEADER] 
         *
         * 按（source-[END]）的大小移动target*内容，然后将源复制到空出的
         * 空间（source-/END]）：[source-END，target-HEADER]
         * */
        memmove(target + source_bytes - ZIPLIST_END_SIZE,
                target + ZIPLIST_HEADER_SIZE,
                target_bytes - ZIPLIST_HEADER_SIZE);
        memcpy(target, source, source_bytes - ZIPLIST_END_SIZE);
    }

    /* Update header metadata. 
     *
     * 更新标头元数据。
     * */
    ZIPLIST_BYTES(target) = intrev32ifbe(zlbytes);
    ZIPLIST_LENGTH(target) = intrev16ifbe(zllength);
    /* New tail offset is:
     *   + N bytes of first ziplist
     *   - 1 byte for [END] of first ziplist
     *   + M bytes for the offset of the original tail of the second ziplist
     *   - J bytes for HEADER because second_offset keeps no header. 
     *
     * 新的尾部偏移量是：第一个ziplist的+N个字节-第一个ziblist的[END]的1个字节+第二个 ziplist 的原始尾部的偏移量的M个字节-HEADER的
     * J个字节，因为second_offset不保留任何头。
     * */
    ZIPLIST_TAIL_OFFSET(target) = intrev32ifbe(
                                   (first_bytes - ZIPLIST_END_SIZE) +
                                   (second_offset - ZIPLIST_HEADER_SIZE));

    /* __ziplistCascadeUpdate just fixes the prev length values until it finds a
     * correct prev length value (then it assumes the rest of the list is okay).
     * We tell CascadeUpdate to start at the first ziplist's tail element to fix
     * the merge seam. 
     *
     * __ziplistCascadeUpdate只修复prev长度值，直到找到正确的
     * prev长度（然后它假设列表的其余部分是可以的）。我们告诉CascadeUpdate从第一个ziplist的尾部元素开始修复合并接缝。
     * */
    target = __ziplistCascadeUpdate(target, target+first_offset);

    /* Now free and NULL out what we didn't realloc 
     *
     * 现在释放并清空我们没有实现的内容
     * */
    if (append) {
        zfree(*second);
        *second = NULL;
        *first = target;
    } else {
        zfree(*first);
        *first = NULL;
        *second = target;
    }
    return target;
}

/*
 * 将新元素插入为列表的表头节点或者表尾节点
 *
 * 复杂度：O(N^2)
 *
 * 返回值：添加操作完成后的 ziplist
 */
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
    unsigned char *p;
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    return __ziplistInsert(zl,p,s,slen);
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. 
 *
 * 返回一个偏移量，用于使用ziplistNext进行迭代。当给定的索引为负数时，将
 * 从后向前遍历列表。当列表在提供的索引处不包含元素时，将返回NULL。
 * */
unsigned char *ziplistIndex(unsigned char *zl, int index) {
    unsigned char *p;
    unsigned int prevlensize, prevlen = 0;
    // 向前遍历
    if (index < 0) {
        index = (-index)-1;
        p = ZIPLIST_ENTRY_TAIL(zl);
        // 如果 ziplist 不为空。。。
        if (p[0] != ZIP_END) {
            ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
            while (prevlen > 0 && index--) {
                p -= prevlen;
                ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
            }
        }
    } else {
        // 向后遍历
        p = ZIPLIST_ENTRY_HEAD(zl);
        // 根据 entry.prevrawlen 向后进行迭代
        while (p[0] != ZIP_END && index--) {
            p += zipRawEntryLength(p);
        }
    }
    return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/* Return pointer to next entry in ziplist.
 *
 * zl is the pointer to the ziplist
 * p is the pointer to the current element
 *
 * The element after 'p' is returned, otherwise NULL if we are at the end. 
 *
 * 返回指向压缩列表中下一个节点的指针。
 *
 * zl 是指向ziplist的指针
 * p 是指向当前元素的指针
 *
 * 返回'p'之后的元素，否则如果我们在末尾，则返回NULL。
 * */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
    ((void) zl);

    /* "p" could be equal to ZIP_END, caused by ziplistDelete,
     * and we should return NULL. Otherwise, we should return NULL
     * when the *next* element is ZIP_END (there is no next entry). 
     *
     * “p”可能等于ziplistDelete引起的ZIP_END，我们应该返回NULL。
     * 否则，当*nextelement为ZIP_END（没有下一个节点）时，我们应该返回NULL。
     * */
    if (p[0] == ZIP_END) {
        return NULL;
    }

    // 指向下一节点，O(1)
    p += zipRawEntryLength(p);
    if (p[0] == ZIP_END) {
        return NULL;
    }

    return p;
}

/* Return pointer to previous entry in ziplist. 
 *
 * 返回指向ziplist中上一个节点的指针。
 * */
/*
 * 返回 p 的前一个节点
 *
 * 复杂度：O(1)
 */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
    unsigned int prevlensize, prevlen = 0;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. 
     *
     * 从ZIP_END向后循环应该返回尾部。当“p”等于列表的第一个元素时，我们已经处
     * 于最前面，应该返回NULL。
     * */
    // 这是表尾
    if (p[0] == ZIP_END) {
        p = ZIPLIST_ENTRY_TAIL(zl);
        return (p[0] == ZIP_END) ? NULL : p;
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
        // 到达表头，停止
        return NULL;
    } else {
        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
        assert(prevlen > 0);
        return p-prevlen;
    }
}

/* Get entry pointed to by 'p' and store in either '*sstr' or 'sval' depending
 * on the encoding of the entry. '*sstr' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the ziplist, 1 otherwise. 
 *
 * 获取“p”指向的节点，并根据节点的编码存储在“*sstr”或“sval”中*sstr总是设置为NULL，以便能够查明是设置了字符串指针还是整数值。
 * 如果'p'指向 ziplist的末尾，则返回0，否则返回1。
 * */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    // 表尾
    if (p == NULL || p[0] == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    // 获取节点
    zipEntry(p, &entry);
    // 字符串
    if (ZIP_IS_STR(entry.encoding)) {
        if (sstr) {
            *slen = entry.len;
            *sstr = p+entry.headersize;
        }
    } else {
        // 数字值
        if (sval) {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);
        }
    }
    return 1;
}

/* Insert an entry at "p". 
 *
 * 在“p”处插入一个节点。
 * */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    return __ziplistInsert(zl,p,s,slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. 
 *
 * 从压缩列表中删除*p指向的单个节点。同时更新*p，以便能够在删除节点的同时对ziplist进行迭代。
 * */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    size_t offset = *p-zl;
    zl = __ziplistDelete(zl,*p,1);

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. 
     *
     * 将指向当前元素的指针存储在p中，因为ziplistDelete将执行realloc操作，
     * 这可能会导致不同的“zl”指针。当删除方向是从后到前时，我们可能会删除最
     * 后一个节点，并以“p”指向ZIP_end结束，因此请检查此项。
     * */
    *p = zl+offset;
    return zl;
}

/* Delete a range of entries from the ziplist. 
 *
 * 从压缩列表中删除一系列节点。
 * */
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num) {
    unsigned char *p = ziplistIndex(zl,index);
    return (p == NULL) ? zl : __ziplistDelete(zl,p,num);
}

/* Compare entry pointer to by 'p' with 'sstr' of length 'slen'. 
 *
 * 将“p”指向的节点指针与长度为“slen”的“sstr”进行比较。
 * */

/* Return 1 if equal. 
 *
 * 如果相等，则返回1。
 * */
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;
    // p 是表尾
    if (p[0] == ZIP_END) return 0;

    // 获取节点属性
    zipEntry(p, &entry);
    // 对比字符串
    if (ZIP_IS_STR(entry.encoding)) {
        /* Raw compare 
         *
         * 原始比较
         * */
        if (entry.len == slen) {
            return memcmp(p+entry.headersize,sstr,slen) == 0;
        } else {
            return 0;
        }
    } else {
        // 对比整数

        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. 
         *
         * 尝试比较编码值。不要比较编码，因为不同的实现可能对整数进行不同的编码。
         * */
        if (zipTryEncoding(sstr,slen,&sval,&sencoding)) {
          zval = zipLoadInteger(p+entry.headersize,entry.encoding);
          return zval == sval;
        }
    }
    return 0;
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. 
 *
 *  根据给定的 vstr 和 vlen ，查找指向与指定节点相等的节点的指针。在每次比较之间跳过“跳过”节点。找不到字段时
 * 返回NULL。
 * */
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;

    // 遍历整个列表
    while (p[0] != ZIP_END) {
        unsigned int prevlensize, encoding, lensize, len;
        unsigned char *q;

        // 编码前一个节点的长度所需的空间
        ZIP_DECODE_PREVLENSIZE(p, prevlensize);
        // 当前节点的长度
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
        // 保存下一个节点的地址
        q = p + prevlensize + lensize;

        if (skipcnt == 0) {
            /* Compare current entry with specified entry 
             *
             * 将当前节点与指定节点进行比较
             * */
            if (ZIP_IS_STR(encoding)) {
                if (len == vlen && memcmp(q, vstr, vlen) == 0) {
                    return p;
                }
            } else {
                // 对比整数


                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. 
                 *
                 * 找出搜索到的字段是否可以编码。请注意，我们只做第一次，一旦完成，vencoding将设置为非零，vll将设置为整数值。
                 * */
                // 对传入值进行 decode
                if (vencoding == 0) {
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
                        /* If the entry can't be encoded we set it to
                         * UCHAR_MAX so that we don't retry again the next
                         * time. 
                         *
                         * 如果无法对节点进行编码，我们将其设置为UCHAR_MAX，以便下次不再重试。
                         * */
                        vencoding = UCHAR_MAX;
                    }
                    /* Must be non-zero by now 
                     *
                     * 现在必须为非零
                     * */
                    assert(vencoding);
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. 
                 *
                 * 将当前节点与指定节点进行比较，只有在vencoding的情况下才能进行此操作！= UCHAR_MAX，
                 * 因为如果字段不可能进行编码，它就不能是有效的整数。
                 * */
                if (vencoding != UCHAR_MAX) {
                    long long ll = zipLoadInteger(q, encoding);
                    // 对比
                    if (ll == vll) {
                        return p;
                    }
                }
            }

            /* Reset skip count 
             *
             * 重置跳过计数
             * */
            skipcnt = skip;
        } else {
            /* Skip entry 
             *
             * 跳过节点
             * */
            skipcnt--;
        }

        /* Move to next entry 
         *
         * 移动到下一个节点
         * */
        p = q + len;
    }

    return NULL;
}

/* Return length of ziplist. 
 *
 * 返回拉链长度。
 * */
/*
 * 返回 ziplist 的长度
 *
 * 复杂度：O(N)
 */
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    // 节点的数量 < UINT16_MAX
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {
        // 长度保存在一个 uint16 整数中
        len = intrev16ifbe(ZIPLIST_LENGTH(zl));
    } else {
        // 节点的数量 >= UINT16_MAX

        // 遍历整个 ziplist ，计算长度
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END) {
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough 
         *
         * 如果足够小，则重新存储长度
         * */
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
    }
    return len;
}

/* Return ziplist blob size in bytes. 
 *
 * 返回ziplist blob大小（以字节为单位）。
 * */
/*
 * 返回整个 ziplist 的空间大小
 *
 * 复杂度：O(1)
 */
size_t ziplistBlobLen(unsigned char *zl) {
    return intrev32ifbe(ZIPLIST_BYTES(zl));
}

void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    int index = 0;
    zlentry entry;

    printf(
        "{total bytes %d} "
        "{num entries %u}\n"
        "{tail offset %u}\n",
        intrev32ifbe(ZIPLIST_BYTES(zl)),
        intrev16ifbe(ZIPLIST_LENGTH(zl)),
        intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END) {
        zipEntry(p, &entry);
        printf(
            "{\n"
                "\taddr 0x%08lx,\n"
                "\tindex %2d,\n"
                "\toffset %5ld,\n"
                "\thdr+entry len: %5u,\n"
                "\thdr len%2u,\n"
                "\tprevrawlen: %5u,\n"
                "\tprevrawlensize: %2u,\n"
                "\tpayload %5u\n",
            (long unsigned)p,
            index,
            (unsigned long) (p-zl),
            entry.headersize+entry.len,
            entry.headersize,
            entry.prevrawlen,
            entry.prevrawlensize,
            entry.len);
        printf("\tbytes: ");
        for (unsigned int i = 0; i < entry.headersize+entry.len; i++) {
            printf("%02x|",p[i]);
        }
        printf("\n");
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding)) {
            printf("\t[str]");
            if (entry.len > 40) {
                if (fwrite(p,40,1,stdout) == 0) perror("fwrite");
                printf("...");
            } else {
                if (entry.len &&
                    fwrite(p,entry.len,1,stdout) == 0) perror("fwrite");
            }
        } else {
            printf("\t[int]%lld", (long long) zipLoadInteger(p,entry.encoding));
        }
        printf("\n}\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}

#ifdef REDIS_TEST
#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

static unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

static unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

static void stress(int pos, int num, int maxsize, int dnum) {
    int i,j,k;
    unsigned char *zl;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos 
         *
         * 从pos推送+弹出次数
         * */
        start = usec();
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,pos);
            zl = ziplistDeleteRange(zl,0,1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
            i,intrev32ifbe(ZIPLIST_BYTES(zl)),num,posstr[pos],usec()-start);
        zfree(zl);
    }
}

static unsigned char *pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl,where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p,&vstr,&vlen,&vlong)) {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr) {
            if (vlen && fwrite(vstr,vlen,1,stdout) == 0) perror("fwrite");
        }
        else {
            printf("%lld", vlong);
        }

        printf("\n");
        return ziplistDelete(zl,&p);
    } else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

static int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
    break;
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

static void verify(unsigned char *zl, zlentry *e) {
    int len = ziplistLen(zl);
    zlentry _e;

    ZIPLIST_ENTRY_ZERO(&_e);

    for (int i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        zipEntry(ziplistIndex(zl, i), &e[i]);

        memset(&_e, 0, sizeof(zlentry));
        zipEntry(ziplistIndex(zl, -len+i), &_e);

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

int ziplistTest(int argc, char **argv) {
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;

    /* If an argument is given, use it as the random seed. 
     *
     * 如果给定了一个参数，请将其用作随机种子。
     * */
    if (argc == 2)
        srand(atoi(argv[1]));

    zl = createIntList();
    ziplistRepr(zl);

    zfree(zl);

    zl = createList();
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_HEAD);
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    zl = pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    zfree(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
        zfree(zl);
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl,&p);
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
        zfree(zl);
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        while (ziplistGet(p,&entry,&elen,&value)) {
            if (entry && strncmp("foo",(char*)entry,elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl,&p);
            } else {
                printf("Entry: ");
                if (entry) {
                    if (elen && fwrite(entry,elen,1,stdout) == 0)
                        perror("fwrite");
                } else {
                    printf("%lld",value);
                }
                p = ziplistNext(zl,p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
        zfree(zl);
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257] = {0}, v2[257] = {0};
        memset(v1,'x',256);
        memset(v2,'y',256);
        zl = ziplistNew();
        zl = ziplistPush(zl,(unsigned char*)v1,strlen(v1),ZIPLIST_TAIL);
        zl = ziplistPush(zl,(unsigned char*)v2,strlen(v2),ZIPLIST_TAIL);

        /* Pop values again and compare their value. 
         *
         * 再次弹出值并比较它们的值。
         * */
        p = ziplistIndex(zl,0);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v1,(char*)entry,elen) == 0);
        p = ziplistIndex(zl,1);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v2,(char*)entry,elen) == 0);
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257] = {{0}};
        zlentry e[3] = {{.prevrawlensize = 0, .prevrawlen = 0, .lensize = 0,
                         .len = 0, .headersize = 0, .encoding = 0, .p = NULL}};
        size_t i;

        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][  1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            zl = ziplistPush(zl, (unsigned char *) v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 
         *
         * 删除节点1将增加节点2的“prewrawlensize”
         * */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Create long list and check indices:\n");
    {
        zl = ziplistNew();
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf,"%d",i);
            zl = ziplistPush(zl,(unsigned char*)buf,len,ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++) {
            p = ziplistIndex(zl,i);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(i == value);

            p = ziplistIndex(zl,-i-1);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(999-i == value);
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Merge test:\n");
    {
        /* create list gives us: [hello, foo, quux, 1024] 
         *
         * createlist给了我们：[hello，foo，quux，1024]
         * */
        zl = createList();
        unsigned char *zl2 = createList();

        unsigned char *zl3 = ziplistNew();
        unsigned char *zl4 = ziplistNew();

        if (ziplistMerge(&zl4, &zl4)) {
            printf("ERROR: Allowed merging of one ziplist into itself.\n");
            return 1;
        }

        /* Merge two empty ziplists, get empty result back. 
         *
         * 合并两个空的ziplist，得到空的结果。
         * */
        zl4 = ziplistMerge(&zl3, &zl4);
        ziplistRepr(zl4);
        if (ziplistLen(zl4)) {
            printf("ERROR: Merging two empty ziplists created entries.\n");
            return 1;
        }
        zfree(zl4);

        zl2 = ziplistMerge(&zl, &zl2);
        /* merge gives us: [hello, foo, quux, 1024, hello, foo, quux, 1024] 
         *
         * merge给了我们：[hello, foo, quux, 1024, hello, foo, quux, 1024]
         * */
        ziplistRepr(zl2);

        if (ziplistLen(zl2) != 8) {
            printf("ERROR: Merged length not 8, but: %u\n", ziplistLen(zl2));
            return 1;
        }

        p = ziplistIndex(zl2,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl2,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }

        p = ziplistIndex(zl2,4);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl2,7);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
        zfree(zl);
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist 
         *
         * 将临时变量从ziplist中取出
         * */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        for (i = 0; i < 20000; i++) {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref,(void (*)(void*))sdsfree);
            len = rand() % 256;

            /* Create lists 
             *
             * 创建列表
             * */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to ziplist 
                 *
                 * 添加到压缩列表
                 * */
                zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

                /* Add to reference list 
                 *
                 * 添加到参考列表
                 * */
                if (where == ZIPLIST_HEAD) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == ZIPLIST_TAIL) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. 
                 *
                 * 获取元素的简单方法，但类似于从Tcl测试套件执行的stresser。
                 * */
                p = ziplistIndex(zl,j);
                refnode = listIndex(ref,j);

                assert(ziplistGet(p,&sstr,&slen,&sval));
                if (sstr == NULL) {
                    buflen = sprintf(buf,"%lld",sval);
                } else {
                    buflen = slen;
                    memcpy(buf,sstr,buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf,listNodeValue(refnode),buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with variable ziplist size:\n");
    {
        stress(ZIPLIST_HEAD,100000,16384,256);
        stress(ZIPLIST_TAIL,100000,16384,256);
    }

    return 0;
}
#endif
