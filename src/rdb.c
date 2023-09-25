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
#include "lzf.h"    /* LZF compression library 
                     *
                     * LZF压缩库
                     * */
#include "zipmap.h"
#include "endianconv.h"
#include "stream.h"

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/param.h>

/* This macro is called when the internal RDB stracture is corrupt 
 *
 * 当内部RDB结构损坏时调用此宏
 * */
#define rdbExitReportCorruptRDB(...) rdbReportError(1, __LINE__,__VA_ARGS__)
/* This macro is called when RDB read failed (possibly a short read) 
 *
 * 当RDB读取失败（可能是短暂读取）时调用此宏
 * */
#define rdbReportReadError(...) rdbReportError(0, __LINE__,__VA_ARGS__)

char* rdbFileBeingLoaded = NULL; /* used for rdb checking on read error 
                                  *
                                  * 用于读取错误时的rdb检查
                                  * */
extern int rdbCheckMode;
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

#ifdef __GNUC__
void rdbReportError(int corruption_error, int linenum, char *reason, ...) __attribute__ ((format (printf, 3, 4)));
#endif
void rdbReportError(int corruption_error, int linenum, char *reason, ...) {
    va_list ap;
    char msg[1024];
    int len;

    len = snprintf(msg,sizeof(msg),
        "Internal error in RDB reading offset %llu, function at rdb.c:%d -> ",
        (unsigned long long)server.loading_loaded_bytes, linenum);
    va_start(ap,reason);
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    va_end(ap);

    if (!rdbCheckMode) {
        if (rdbFileBeingLoaded || corruption_error) {
            serverLog(LL_WARNING, "%s", msg);
            char *argv[2] = {"",rdbFileBeingLoaded};
            redis_check_rdb_main(2,argv,NULL);
        } else {
            serverLog(LL_WARNING, "%s. Failure loading rdb format from socket, assuming connection error, resuming operation.", msg);
            return;
        }
    } else {
        rdbCheckError("%s",msg);
    }
    serverLog(LL_WARNING, "Terminating server after rdb file reading failure.");
    exit(1);
}

/*
 * 写入 p 指针之后 len 长度的内容到 rio 。
 *
 * 成功返回写入长度，失败返回 0 。
 */
static ssize_t rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

/*
 * 将 type 写入到 rdb 中
 */
int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth. 
 *
 * 加载RDB格式的“类型”，即一个单字节无符号整数。此函数不仅用于加载对象类型，还
 * 用于加载特殊的“类型”，如文件结尾类型、EXPIRE类型等。
 * */
/*
 * 从 RDB 文件中读入 1 字节，这个字节标记了值对象的类型，EOF、过期时间等等。
 */
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

/* This is only used to load old databases stored with the RDB_OPCODE_EXPIRETIME
 * opcode. New versions of Redis store using the RDB_OPCODE_EXPIRETIME_MS
 * opcode. On error -1 is returned, however this could be a valid time, so
 * to check for loading errors the caller should call rioGetReadError() after
 * calling this function. 
 *
 * 这仅用于加载使用 RDB_OPODE_EXPIRETIME 操作码存储的旧数据库。
 * Redis的新版本使用RDB_OPODE_EXPIRETIME_MS操作码存储。返回错误-1，
 * 但是这可能是一个有效的时间，因此为了检查加载错误，调用方应该在调用此
 * 函数后调用rioGetReadError（）。
 * */
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    if (rioRead(rdb,&t32,4) == 0) return -1;
    return (time_t)t32;
}

int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    memrev64ifbe(&t64); /* Store in little endian. 
                         *
                         * 通过小端格式存储。
                         * */
    return rdbWriteRaw(rdb,&t64,8);
}

/* This function loads a time from the RDB file. It gets the version of the
 * RDB because, unfortunately, before Redis 5 (RDB version 9), the function
 * failed to convert data to/from little endian, so RDB files with keys having
 * expires could not be shared between big endian and little endian systems
 * (because the expire time will be totally wrong). The fix for this is just
 * to call memrev64ifbe(), however if we fix this for all the RDB versions,
 * this call will introduce an incompatibility for big endian systems:
 * after upgrading to Redis version 5 they will no longer be able to load their
 * own old RDB files. Because of that, we instead fix the function only for new
 * RDB versions, and load older RDB versions as we used to do in the past,
 * allowing big endian systems to load their own old RDB files.
 *
 * On I/O error the function returns LLONG_MAX, however if this is also a
 * valid stored value, the caller should use rioGetReadError() to check for
 * errors after calling this function. 
 *
 * 此函数从RDB文件加载时间。它得到了RDB的版本，因为不幸的是，在Redis 5
 * （RDB版本9）之前，该函数无法将数据转换为小端序，因此具有过期键的RDB文件
 * 无法在大端序和小端序系统之间共享（因为过期时间完全错误）。解决方法只是调用memrev64ifbe（），
 * 然而，如果我们为所有RDB版本修复此问题，则此调用将引入
 * big-endian系统的不兼容性：升级到Redis版本5后，它们将无法再加载自
 * 己的旧RDB文件。因此，我们只为新的RDB版本修复了该函数，并像过去那样加载旧的
 * RDB，允许big-endian系统加载自己的旧RDB文件。在I/O错误时，函数
 * 返回LLONG_MAX，但如果这也是一个有效的存储值，则调用方应在调用此函数后使
 * 用rioGetReadError（）检查错误。
 * */
long long rdbLoadMillisecondTime(rio *rdb, int rdbver) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return LLONG_MAX;
    if (rdbver >= 9) /* Check the top comment of this function. 
                      *
                      * 检查此函数的顶部注释。
                      * */
        memrev64ifbe(&t64); /* Convert in big endian if the system is BE. 
                             *
                             * 如果系统为BE，则转换为big-endian。
                             * */
    return (long long)t64;
}


/* Saves an encoded length.
 * 保存一个编码后的长度
 *
 * The first two bits in the first byte are used to hold the encoding type.
 * 第一字节的前两个位用于记录编码的类型
 *
 * See the REDIS_RDB_* definitions
 * for more information on the types of encoding.
 * 查看 rdb.h 中关于 REDIS_RDB_* 的定义来获取更多信息。
 */
int rdbSaveLen(rio *rdb, uint64_t len) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1<<6)) {
        /* Save a 6 bit len 
         *
         * 保存6位长度
         * */
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len 
         *
         * 保存14位长度
         * */
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len 
         *
         * 保存32位长度
         * */
        buf[0] = RDB_32BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        uint32_t len32 = htonl(len);
        if (rdbWriteRaw(rdb,&len32,4) == -1) return -1;
        nwritten = 1+4;
    } else {
        /* Save a 64 bit len 
         *
         * 保存64位长度
         * */
        buf[0] = RDB_64BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonu64(len);
        if (rdbWriteRaw(rdb,&len,8) == -1) return -1;
        nwritten = 1+8;
    }
    // 返回编码所使用的长度
    return nwritten;
}


/* Load an encoded length. If the loaded length is a normal length as stored
 * with rdbSaveLen(), the read length is set to '*lenptr'. If instead the
 * loaded length describes a special encoding that follows, then '*isencoded'
 * is set to 1 and the encoding format is stored at '*lenptr'.
 *
 * See the RDB_ENC_* definitions in rdb.h for more information on special
 * encodings.
 *
 * The function returns -1 on error, 0 on success. 
 *
 * 加载编码长度。如果加载的长度是用rdbSaveLen（）存储的正常长度，则读取长
 * 度设置为“*lenptr”。如果加载的长度描述了后面的特殊编码，则“*isencoded”设置为1，
 * 编码格式存储在“*lenptr”中。
 * 
 * 有关特殊编码的更多信息，请参阅RDB.h中的RDB_ENC_definitions。
 * 
 * 函数在出错时返回-1，在成功时返回0。
 * */
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return -1;
    type = (buf[0]&0xC0)>>6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. 
         *
         * 读取6位编码类型。
         * */
        if (isencoded) *isencoded = 1;
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. 
         *
         * 读取6位长度。
         * */
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. 
         *
         * 读取14位长度。
         * */
        if (rioRead(rdb,buf+1,1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. 
         *
         * 读取32位长度。
         * */
        uint32_t len;
        if (rioRead(rdb,&len,4) == 0) return -1;
        *lenptr = ntohl(len);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. 
         *
         * 读取64位长度。
         * */
        uint64_t len;
        if (rioRead(rdb,&len,8) == 0) return -1;
        *lenptr = ntohu64(len);
    } else {
        rdbExitReportCorruptRDB(
            "Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. 
                    *
                    * 从未联系过。
                    * */
    }
    return 0;
}

/* This is like rdbLoadLenByRef() but directly returns the value read
 * from the RDB stream, signaling an error by returning RDB_LENERR
 * (since it is a too large count to be applicable in any Redis data
 * structure). 
 *
 * 这类似于rdbLoadLenByRef（），但直接返回从RDB流读取的值，通过返
 * 回RDB_LENERR来发出错误信号（因为它的计数太大，不适用于任何Redis数
 * 据结构）。
 * */
uint64_t rdbLoadLen(rio *rdb, int *isencoded) {
    uint64_t len;

    if (rdbLoadLenByRef(rdb,isencoded,&len) == -1) return RDB_LENERR;
    return len;
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. 
 *
 * 当“value”参数符合编码类型支持的范围时，将其编码为整数。如果函数成功地对整
 * 数进行了编码，则表示形式将存储在“enc”指向的缓冲区指针中，并返回字符串长度。
 * 否则返回0。
 * */
/*
 * 尝试对 value 进行编码，成功返回编码所需的字节长度，否则返回 0 。
 */
int rdbEncodeInteger(long long value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * The returned value changes according to the flags, see
 * rdbGenericLoadStringObject() for more info. 
 *
 * 加载具有指定编码类型“enctype”的整数编码对象。返回的值会根据标志而变化，
 * 有关详细信息，请参阅rdbGenericLoadStringObject（）。
 * */
/*
 * 根据编码类型，将读取到的字符串转换为整数对象。
 */
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int encode = flags & RDB_LOAD_ENC;
    unsigned char enc[4];
    long long val;

    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = ((uint32_t)enc[0])|
            ((uint32_t)enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = ((uint32_t)enc[0])|
            ((uint32_t)enc[1]<<8)|
            ((uint32_t)enc[2]<<16)|
            ((uint32_t)enc[3]<<24);
        val = (int32_t)v;
    } else {
        rdbExitReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
        return NULL; /* Never reached. 
                    *
                    * 从未到达这里。
                    * */
    }
    if (plain || sds) {
        char buf[LONG_STR_SIZE], *p;
        int len = ll2string(buf,sizeof(buf),val);
        if (lenptr) *lenptr = len;
        p = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        memcpy(p,buf,len);
        return p;
    } else if (encode) {
        return createStringObjectFromLongLongForValue(val);
    } else {
        return createObject(OBJ_STRING,sdsfromlonglong(val));
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space 
 *
 * 形式为“2391”“-100”的字符串对象，不带任何空格，并且具有可容纳8、16
 * 或32位有符号值的值范围，可以将其编码为整数以节省空间
 * */
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number 
     *
     * 检查是否可以将此值编码为数字
     * */
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer 
     *
     * 如果转换回字符串的数字不相同，则不可能将字符串编码为整数
     * */
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    return rdbEncodeInteger(value,enc);
}

ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len,
                       size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk 
     *
     * 数据已压缩！让我们把它保存在磁盘上
     * */
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF;
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,compress_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,original_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) goto writeerr;
    nwritten += n;

    return nwritten;

writeerr:
    return -1;
}

/*
 * 读取并解压被 lzf 算法压缩的字符串，
 * 返回一个保存解压后的字符串内容的字符串对象
 */
ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it 
     *
     * 我们需要至少四个字节的压缩才能实现这一点
     * */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len);
    zfree(out);
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value
 * changes according to 'flags'. For more info check the
 * rdbGenericLoadStringObject() function. 
 *
 * 加载RDB格式的LZF压缩字符串。返回的值会根据“flags”而更改。有关详细信息，请查看rdbGenericLoadStringObject（）函数。
 * */
void *rdbLoadLzfStringObject(rio *rdb, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    uint64_t len, clen;
    unsigned char *c = NULL;
    char *val = NULL;

    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;

    /* Allocate our target according to the uncompressed size. 
     *
     * 根据未压缩的大小分配我们的目标。
     * */
    if (plain) {
        val = zmalloc(len);
    } else {
        val = sdsnewlen(SDS_NOINIT,len);
    }
    if (lenptr) *lenptr = len;

    /* Load the compressed representation and uncompress it to target. 
     *
     * 加载压缩的表示并将其解压缩到目标。
     * */
    if (rioRead(rdb,c,clen) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) {
        rdbExitReportCorruptRDB("Invalid LZF compressed string");
    }
    zfree(c);

    if (plain || sds) {
        return val;
    } else {
        return createObject(OBJ_STRING,val);
    }
err:
    zfree(c);
    if (plain)
        zfree(val);
    else
        sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form 
 *
 * 将字符串对象另存为磁盘上的[len][data]。如果对象是整数值的字符串表示，
 * 我们会尝试将其保存为特殊形式
 * */
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding 
     *
     * 尝试整数编码
     * */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it 
     *
     * 尝试LZF压缩-在20字节以下，它甚至无法压缩aaaaaaaaaa，所以跳过它
     * */
    if (server.rdb_compression && len > 20) {
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way 
         *
         * 返回值0表示数据无法压缩，按旧方式保存
         * */
    }

    /* Store verbatim 
     *
     * 逐字存储
     * */
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. 
 *
 * 将 long long  值另存为编码字符串或字符串。
 * */
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
    // 尝试进行编码
    int enclen = rdbEncodeInteger(value,buf);
    if (enclen > 0) {
        // 编码成功
        return rdbWriteRaw(rdb,buf,enclen);
    } else {
        /* Encode as string 
         *
         * 编码为字符串
         * */
        // 编码失败，将整数保存为字符串
        enclen = ll2string((char*)buf,32,value);
        serverAssert(enclen < 32);
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;
        nwritten += n;
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveRawString() gets a Redis object instead. 
 *
 * 像rdbSaveRawString（）一样，获取Redis对象。
 * */
/*
 * 将字符串 obj 保存到 rdb ，在此之前，尝试对 obj 进行编码
 */
ssize_t rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. 
     *
     * 如果对象已经是整数编码的，请避免对对象进行解码，然后再次对其进行编码。
     * */
    if (obj->encoding == OBJ_ENCODING_INT) {
        // 整数在尝试编码之后写入
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);
    } else {
        // 如果是字符串，直接写入 rdb
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 *
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to
 *               encode it in a special way to be more memory
 *               efficient. When this flag is passed the function
 *               no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc()
 *                 instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 *
 * On I/O error NULL is returned.
 
 *
 * 根据标志从RDB文件加载字符串对象：
 * 
 * RDB_Load_NONE（无标志）：加载未编码的RDB对象。
 * RDB_LOAD_ENC：如果返回的类型是Redis对象，请尝试用一种特殊的方式对其进行编码，以提高内存效率。
 *               当传递此标志时，函数不再保证obj->ptr是SDS字符串。
 * RDB_LOAD_PLAIN：返回一个用zmalloc（）分配的普通字符串，而不是一个有sds的Redis对象。I/O错误返回NULL。
 * */
/*
 * 根据编码 encode ，从 rdb 文件中读取字符串，并返回字符串对象
 */
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr) {
    int encode = flags & RDB_LOAD_ENC;
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int isencoded;
    unsigned long long len;

    // 获取字符串的长度
    len = rdbLoadLen(rdb,&isencoded);
    if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            // 字节串是整数，创建整数对象并返回
            return rdbLoadIntegerObject(rdb,len,flags,lenptr);
        case RDB_ENC_LZF:
            // 字节串是被 lzf 算法压缩的字符串
            return rdbLoadLzfStringObject(rdb,flags,lenptr);
        default:
            rdbExitReportCorruptRDB("Unknown RDB string encoding type %llu",len);
            return NULL;
        }
    }

    if (len == RDB_LENERR) return NULL;
    if (plain || sds) {
        void *buf = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        if (lenptr) *lenptr = len;
        if (len && rioRead(rdb,buf,len) == 0) {
            if (plain)
                zfree(buf);
            else
                sdsfree(buf);
            return NULL;
        }
        return buf;
    } else {
        robj *o = encode ? createStringObject(SDS_NOINIT,len) :
                           createRawStringObject(SDS_NOINIT,len);
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
        return o;
    }
}

robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
}

robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC,NULL);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 
 *
 * 保存一个双值。双精度保存为字符串，前缀为指定表示长度的无符号8位整数。此8位整数
 * 具有特殊值，以便指定以下条件：
 * 253:不是数字
 * 254:+inf 
 * 255:-inf
 * */
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. 
         *
         * 检查浮子是否在安全范围内，以便铸造成长形。我们假设这里的long-long是64
         * 位。此外，我们假设在double精度＜52位的情况下没有实现。
         * 
         * 在这种假设下，我们测试了一个双精度是否在一个区间内，在这个区间内，长-长转换是安全的。
         * 然后使用两个铸件，我们确保小数部分为零。如果所有这些都是真的，我们使用整数打印功能，
         * 这要快得多。
         * */
        double min = -4503599627370495; /* (2^52)-1 
                                         *
                                         * (2^52)-1
                                         * */
        double max = 4503599627370496; /* -(2^52) 
                                        *
                                        * -(2^52)
                                        * */
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf)-1,(long long)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue() 
 *
 * 有关双重序列化的信息，请检查rdbSaveDoubleValue（）
 * */
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    if (rioRead(rdb,&len,1) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return 0;
    }
}

/* Saves a double for RDB 8 or greater, where IE754 binary64 format is assumed.
 * We just make sure the integer is always stored in little endian, otherwise
 * the value is copied verbatim from memory to disk.
 *
 * Return -1 on error, the size of the serialized value on success. 
 *
 * 为RDB 8或更高版本保存一个双精度，其中假定为IE754二进制64格式。我们只
 * 需确保整数始终存储在little-endian中，否则该值将从内存逐字复制到磁盘。错误时返回-1，成功时返回序列化值的大小。
 * */
int rdbSaveBinaryDoubleValue(rio *rdb, double val) {
    memrev64ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Loads a double from RDB 8 or greater. See rdbSaveBinaryDoubleValue() for
 * more info. On error -1 is returned, otherwise 0. 
 *
 * 从RDB 8或更高版本加载一个double。有关详细信息，请参阅rdbSaveBinaryDoubleValue（）。出现错误时返回-1，否则返回0。
 * */
int rdbLoadBinaryDoubleValue(rio *rdb, double *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev64ifbe(val);
    return 0;
}

/* Like rdbSaveBinaryDoubleValue() but single precision. 
 *
 * 类似rdbSaveBinaryDoubleValue（），但为单精度。
 * */
int rdbSaveBinaryFloatValue(rio *rdb, float val) {
    memrev32ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Like rdbLoadBinaryDoubleValue() but single precision. 
 *
 * 类似rdbLoadBinaryDoubleValue（），但为单精度。
 * */
int rdbLoadBinaryFloatValue(rio *rdb, float *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev32ifbe(val);
    return 0;
}

/* Save the object type of object "o". 
 *
 * 保存对象“o”的对象类型。
 * */
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    // 字符串
    case OBJ_STRING:
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    // 列表
    case OBJ_LIST:
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return rdbSaveType(rdb,RDB_TYPE_LIST_QUICKLIST);
        else
            serverPanic("Unknown list encoding");
    // 集合
    case OBJ_SET:
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else
            serverPanic("Unknown set encoding");
    // 有序集
    case OBJ_ZSET:
        // ziplist
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_ZIPLIST);
        // 跳跃表
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_2);
        else
            serverPanic("Unknown sorted set encoding");
    // 哈希
    case OBJ_HASH:
        // ziplist
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_HT)
            // 字典
            return rdbSaveType(rdb,RDB_TYPE_HASH);
        else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return rdbSaveType(rdb,RDB_TYPE_STREAM_LISTPACKS);
    case OBJ_MODULE:
        return rdbSaveType(rdb,RDB_TYPE_MODULE_2);
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning 
                *
                * 避免警告
                * */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type. 
 *
 * 使用rdbLoadType（）以RDB格式加载TYPE，但如果该类型不是特定的有效对象类型，则返回-1。
 * */
int rdbLoadObjectType(rio *rdb) {
    int type;
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

/* This helper function serializes a consumer group Pending Entries List (PEL)
 * into the RDB file. The 'nacks' argument tells the function if also persist
 * the informations about the not acknowledged message, or if to persist
 * just the IDs: this is useful because for the global consumer group PEL
 * we serialized the NACKs as well, but when serializing the local consumer
 * PELs we just add the ID, that will be resolved inside the global PEL to
 * put a reference to the same structure. 
 *
 * 这个助手函数将使用者组PendingEntriesList（PEL）序列化到RDB文件中。
 * “nacks”参数告诉函数是否还保留有关未确认消息的信息，或者是否只保
 * 留ID：这很有用，因为对于全局使用者组PEL，我们也序列化了NACK，但在序列化
 * 本地使用者PEL时，我们只添加ID，该ID将在全局PEL中解析，以引用相同的结构
 * 。
 * */
ssize_t rdbSaveStreamPEL(rio *rdb, rax *pel, int nacks) {
    ssize_t n, nwritten = 0;

    /* Number of entries in the PEL. 
     *
     * PEL中的节点数。
     * */
    if ((n = rdbSaveLen(rdb,raxSize(pel))) == -1) return -1;
    nwritten += n;

    /* Save each entry. 
     *
     * 保存每个节点。
     * */
    raxIterator ri;
    raxStart(&ri,pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        /* We store IDs in raw form as 128 big big endian numbers, like
         * they are inside the radix tree key. 
         *
         * 我们将ID以原始形式存储为128个大端数字，就像它们在基数树键中一样。
         * */
        if ((n = rdbWriteRaw(rdb,ri.key,sizeof(streamID))) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        if (nacks) {
            streamNACK *nack = ri.data;
            if ((n = rdbSaveMillisecondTime(rdb,nack->delivery_time)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveLen(rdb,nack->delivery_count)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            /* We don't save the consumer name: we'll save the pending IDs
             * for each consumer in the consumer PEL, and resolve the consumer
             * at loading time. 
             *
             * 我们不保存使用者名称：我们将在使用者PEL中保存每个使用者的挂起ID，并在加载时
             * 解析该使用者。
             * */
        }
    }
    raxStop(&ri);
    return nwritten;
}

/* Serialize the consumers of a stream consumer group into the RDB. Helper
 * function for the stream data type serialization. What we do here is to
 * persist the consumer metadata, and it's PEL, for each consumer. 
 *
 * 将流使用者组的使用者序列化到RDB中。流数据类型序列化的帮助程序函数。我们在这里
 * 所做的是为每个消费者持久化消费者元数据，它是PEL。
 * */
size_t rdbSaveStreamConsumers(rio *rdb, streamCG *cg) {
    ssize_t n, nwritten = 0;

    /* Number of consumers in this consumer group. 
     *
     * 此消费者组中的消费者数量。
     * */
    if ((n = rdbSaveLen(rdb,raxSize(cg->consumers))) == -1) return -1;
    nwritten += n;

    /* Save each consumer. 
     *
     * 保存每个消费者。
     * */
    raxIterator ri;
    raxStart(&ri,cg->consumers);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamConsumer *consumer = ri.data;

        /* Consumer name. 
         *
         * 消费者名称。
         * */
        if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Last seen time. 
         *
         * 上次露面时间。
         * */
        if ((n = rdbSaveMillisecondTime(rdb,consumer->seen_time)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Consumer PEL, without the ACKs (see last parameter of the function
         * passed with value of 0), at loading time we'll lookup the ID
         * in the consumer group global PEL and will put a reference in the
         * consumer local PEL. 
         *
         * 使用者PEL，如果没有ACK（请参阅传递的函数的最后一个参数，值为0），在加载时
         * ，我们将在使用者组全局PEL中查找ID，并在使用者本地PEL中放置引用。
         * */
        if ((n = rdbSaveStreamPEL(rdb,consumer->pel,0)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;
    }
    raxStop(&ri);
    return nwritten;
}

/* Save a Redis object.
 * Returns -1 on error, number of bytes written on success. 
 *
 * 保存Redis对象。错误时返回-1，成功时返回写入的字节数。
 * */
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key) {
    ssize_t n = 0, nwritten = 0;

    if (o->type == OBJ_STRING) {
        /* Save a string value 
         *
         * 保存字符串值
         * */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    } else if (o->type == OBJ_LIST) {
        /* Save a list value 
         *
         * 保存列表值
         * */
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;

            if ((n = rdbSaveLen(rdb,ql->len)) == -1) return -1;
            nwritten += n;

            while(node) {
                if (quicklistNodeIsCompressed(node)) {
                    void *data;
                    size_t compress_len = quicklistGetLzf(node, &data);
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) return -1;
                    nwritten += n;
                } else {
                    if ((n = rdbSaveRawString(rdb,node->zl,node->sz)) == -1) return -1;
                    nwritten += n;
                }
                node = node->next;
            }
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        /* Save a set value 
         *
         * 保存 OBJ_SET 类型数据的值 
         * */
        if (o->encoding == OBJ_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                sds ele = dictGetKey(de);
                if ((n = rdbSaveRawString(rdb,(unsigned char*)ele,sdslen(ele)))
                    == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value 
         *
         * 保存已 OBJ_ZSET 的值
         * */
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            zskiplist *zsl = zs->zsl;

            if ((n = rdbSaveLen(rdb,zsl->length)) == -1) return -1;
            nwritten += n;

            /* We save the skiplist elements from the greatest to the smallest
             * (that's trivial since the elements are already ordered in the
             * skiplist): this improves the load process, since the next loaded
             * element will always be the smaller, so adding to the skiplist
             * will always immediately stop at the head, making the insertion
             * O(1) instead of O(log(N)). 
             *
             * 我们将skiplist元素从最大值保存到最小值（这很琐碎，因为元素已经在skiplist中排序）：
             * 这改进了加载过程，因为下一个加载的元素总是较小的，所以添加到skiplist总是会在开头立即停止，使插入O（1）而不是O（log（N））。
             * */
            zskiplistNode *zn = zsl->tail;
            while (zn != NULL) {
                if ((n = rdbSaveRawString(rdb,
                    (unsigned char*)zn->ele,sdslen(zn->ele))) == -1)
                {
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                    return -1;
                nwritten += n;
                zn = zn->backward;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value 
         *
         * 保存 OBJ_HASH 值
         * */
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;

        } else if (o->encoding == OBJ_ENCODING_HT) {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                sds field = dictGetKey(de);
                sds value = dictGetVal(de);

                if ((n = rdbSaveRawString(rdb,(unsigned char*)field,
                        sdslen(field))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveRawString(rdb,(unsigned char*)value,
                        sdslen(value))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        /* Store how many listpacks we have inside the radix tree. 
         *
         * 存储基数树中的列表包数量。
         * */
        stream *s = o->ptr;
        rax *rax = s->rax;
        if ((n = rdbSaveLen(rdb,raxSize(rax))) == -1) return -1;
        nwritten += n;

        /* Serialize all the listpacks inside the radix tree as they are,
         * when loading back, we'll use the first entry of each listpack
         * to insert it back into the radix tree. 
         *
         * 按原样序列化基数树中的所有列表包，在加载时，我们将使用每个列表包的第一个节点将其插入基数树中。
         * */
        raxIterator ri;
        raxStart(&ri,rax);
        raxSeek(&ri,"^",NULL,0);
        while (raxNext(&ri)) {
            unsigned char *lp = ri.data;
            size_t lp_bytes = lpBytes(lp);
            if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lp_bytes)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
        }
        raxStop(&ri);

        /* Save the number of elements inside the stream. We cannot obtain
         * this easily later, since our macro nodes should be checked for
         * number of items: not a great CPU / space tradeoff. 
         *
         * 保存流中的元素数。我们以后不能轻易地获得这一点，因为我们的宏节点应该检查项目的数量：这不是一个很大的CPU/空间折衷。
         * */
        if ((n = rdbSaveLen(rdb,s->length)) == -1) return -1;
        nwritten += n;
        /* Save the last entry ID. 
         *
         * 保存最后一个节点ID。
         * */
        if ((n = rdbSaveLen(rdb,s->last_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->last_id.seq)) == -1) return -1;
        nwritten += n;

        /* The consumer groups and their clients are part of the stream
         * type, so serialize every consumer group. 
         *
         * 使用者组及其客户端是流类型的一部分，因此序列化每个使用者组。
         * */

        /* Save the number of groups. 
         *
         * 保存组数。
         * */
        size_t num_cgroups = s->cgroups ? raxSize(s->cgroups) : 0;
        if ((n = rdbSaveLen(rdb,num_cgroups)) == -1) return -1;
        nwritten += n;

        if (num_cgroups) {
            /* Serialize each consumer group. 
             *
             * 序列化每个使用者组。
             * */
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;

                /* Save the group name. 
                 *
                 * 保存组名称。
                 * */
                if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Last ID. 
                 *
                 * 最后一个ID。
                 * */
                if ((n = rdbSaveLen(rdb,cg->last_id.ms)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveLen(rdb,cg->last_id.seq)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the global PEL. 
                 *
                 * 保存全局PEL。
                 * */
                if ((n = rdbSaveStreamPEL(rdb,cg->pel,1)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the consumers of this group. 
                 *
                 * 保存此组的使用者。
                 * */
                if ((n = rdbSaveStreamConsumers(rdb,cg)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        /* Save a module-specific value. 
         *
         * 保存特定于模块的值。
         * */
        RedisModuleIO io;
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;

        /* Write the "module" identifier as prefix, so that we'll be able
         * to call the right module during loading. 
         *
         * 将“模块”标识符写为前缀，这样我们就可以在加载过程中调用正确的模块。
         * */
        int retval = rdbSaveLen(rdb,mt->id);
        if (retval == -1) return -1;
        io.bytes += retval;

        /* Then write the module-specific representation + EOF marker. 
         *
         * 然后编写模块特定表示+EOF标记。
         * */
        moduleInitIOContext(io,mt,rdb,key);
        mt->rdb_save(&io,mv->value);
        retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
        if (retval == -1)
            io.error = 1;
        else
            io.bytes += retval;

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }
        return io.error ? -1 : (ssize_t)io.bytes;
    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. 
 *
 * 如果使用rdbSaveObject（）函数保存，则返回对象在磁盘上的长度。目前，
 * 我们使用一个技巧来获得这个长度，只需对代码进行很少的更改。将来我们可以改用更快的
 * 解决方案。
 * */
size_t rdbSavedObjectLen(robj *o, robj *key) {
    ssize_t len = rdbSaveObject(NULL,o,key);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * 保存键值对，值的类型，以及它的过期时间（如果有的话）。
 *
 * On error -1 is returned.
 * 出错返回 -1 。
 *
 * On success if the key was actaully saved 1 is returned, otherwise 0
 * is returned (the key was already expired).
 *
 * 如果 key 已经过期，放弃保存，返回 0 。
 * 如果 key 保存成功，返回 1 。
 */
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime) {
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* Save the expire time 
     *
     * 保存过期时间
     * */
    if (expiretime != -1) {
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save the LRU info. 
     *
     * 保存LRU信息。
     * */
    if (savelru) {
        uint64_t idletime = estimateObjectIdleTime(val);
        idletime /= 1000; /* Using seconds is enough and requires less space.
                           *
                           * 使用秒就足够了，而且需要更少的空间。
                           * */
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. 
     *
     * 保存LFU信息。
     * */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(val);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. 
         *
         * 我们可以用两个字节来编码：操作码和8位计数器，因为频率是对数的，范围为0-255
         * 。请注意，我们不存储减半时间，因为加载时将其重置一次不会对频率产生太大影响。
         * */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value 
     *
     * 保存类型、键、值
     * */
    
    // 保存值类型
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    // 保存 key
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    // 保存 value
    if (rdbSaveObject(rdb,val,key) == -1) return -1;

    /* Delay return if required (for testing) 
     *
     * 如果需要，延迟返回（用于测试）
     * */
    if (server.rdb_key_save_delay)
        usleep(server.rdb_key_save_delay);

    return 1;
}

/* Save an AUX field. 
 *
 * 保存AUX字段。
 * */
ssize_t rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    ssize_t ret, len = 0;
    if ((ret = rdbSaveType(rdb,RDB_OPCODE_AUX)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,key,keylen)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,val,vallen)) == -1) return -1;
    len += ret;
    return len;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained
 * with strlen(). 
 *
 * rdbSaveAuxField（）的包装器，当可以使用strlen（）获得key
 * /val长度时使用。
 * */
ssize_t rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). 
 *
 * strlen（键）+integer类型（最长为长-长范围）的包装器。
 * */
ssize_t rdbSaveAuxFieldStrInt(rio *rdb, char *key, long long val) {
    char buf[LONG_STR_SIZE];
    int vlen = ll2string(buf,sizeof(buf),val);
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. 
 *
 * 保存一些默认的AUX字段，其中包含有关生成的RDB的信息。
 * */
int rdbSaveInfoAuxFields(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;
    int aof_preamble = (rdbflags & RDBFLAGS_AOF_PREAMBLE) != 0;

    /* Add a few fields about the state when the RDB was created. 
     *
     * 添加一些关于创建RDB时的状态的字段。
     * */
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) return -1;

    /* Handle saving options that generate aux fields. 
     *
     * 处理生成辅助字段的保存选项。
     * */
    if (rsi) {
        if (rdbSaveAuxFieldStrInt(rdb,"repl-stream-db",rsi->repl_stream_db)
            == -1) return -1;
        if (rdbSaveAuxFieldStrStr(rdb,"repl-id",server.replid)
            == -1) return -1;
        if (rdbSaveAuxFieldStrInt(rdb,"repl-offset",server.master_repl_offset)
            == -1) return -1;
    }
    if (rdbSaveAuxFieldStrInt(rdb,"aof-preamble",aof_preamble) == -1) return -1;
    return 1;
}

ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt) {
    /* Save a module-specific aux value. 
     *
     * 保存模块特定的辅助值。
     * */
    RedisModuleIO io;
    int retval = rdbSaveType(rdb, RDB_OPCODE_MODULE_AUX);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* Write the "module" identifier as prefix, so that we'll be able
     * to call the right module during loading. 
     *
     * 将“模块”标识符写为前缀，这样我们就可以在加载过程中调用正确的模块。
     * */
    retval = rdbSaveLen(rdb,mt->id);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* write the 'when' so that we can provide it on loading. add a UINT opcode
     * for backwards compatibility, everything after the MT needs to be prefixed
     * by an opcode. 
     *
     * 编写“when”，以便我们可以在加载时提供它。添加一个UINT操作码以实现向后兼
     * 容性，MT之后的所有内容都需要以操作码为前缀。
     * */
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_UINT);
    if (retval == -1) return -1;
    io.bytes += retval;
    retval = rdbSaveLen(rdb,when);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* Then write the module-specific representation + EOF marker. 
         *
         * 然后编写模块特定表示+EOF标记。
         * */
    moduleInitIOContext(io,mt,rdb,NULL);
    mt->aux_save(&io,when);
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
    if (retval == -1)
        io.error = 1;
    else
        io.bytes += retval;

    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    if (io.error)
        return -1;
    return io.bytes;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. 
 *
 * 生成RDB格式的数据库转储，将其发送到指定的Redis I/O通道。成功时返回C_OK，否则返回C_ERR，
 * 部分输出或全部输出可能会因I/O错误而丢失。
 * 
 * 当函数返回C_ERR时，如果“error”不为NULL，则“error”所指的整数将设置
 * 为I/O错误之后的errno值。
 * */
int rdbSaveRio(rio *rdb, int *error, int rdbflags, rdbSaveInfo *rsi) {
    dictIterator *di = NULL;
    dictEntry *de;
    char magic[10];
    int j;
    uint64_t cksum;
    size_t processed = 0;

    // 如果有需要的话，设置校验和计算函数
    if (server.rdb_checksum)
        rdb->update_cksum = rioGenericUpdateChecksum;
    // 以 "REDIS <VERSION>" 格式写入文件头，以及 RDB 的版本
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    if (rdbSaveInfoAuxFields(rdb,rdbflags,rsi) == -1) goto werr;
    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_BEFORE_RDB) == -1) goto werr;

    // 遍历所有数据库，保存它们的数据
    for (j = 0; j < server.dbnum; j++) {
        // 指向数据库
        redisDb *db = server.db+j;
        // 指向数据库 key space
        dict *d = db->dict;
        // 数据库为空， pass ，处理下个数据库
        if (dictSize(d) == 0) continue;
        // 创建迭代器
        di = dictGetSafeIterator(d);

        /* Write the SELECT DB opcode 
         *
         * 编写SELECT DB操作码
         * */
        // 记录正在使用的数据库的号码
        if (rdbSaveType(rdb,RDB_OPCODE_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(rdb,j) == -1) goto werr;

        /* Write the RESIZE DB opcode. 
         *
         * 编写RESIZE DB操作码。
         * */
        uint64_t db_size, expires_size;
        db_size = dictSize(db->dict);
        expires_size = dictSize(db->expires);
        if (rdbSaveType(rdb,RDB_OPCODE_RESIZEDB) == -1) goto werr;
        if (rdbSaveLen(rdb,db_size) == -1) goto werr;
        if (rdbSaveLen(rdb,expires_size) == -1) goto werr;

        /* Iterate this DB writing every entry 
         *
         * 迭代此DB写入每个节点
         * */
        // 将数据库中的所有节点保存到 RDB 文件
        while((de = dictNext(di)) != NULL) {
            // 取出键
            sds keystr = dictGetKey(de);
            // 取出值
            robj key, *o = dictGetVal(de);
            long long expire;

            initStaticStringObject(key,keystr);
            // 取出过期时间
            expire = getExpire(db,&key);
            if (rdbSaveKeyValuePair(rdb,&key,o,expire) == -1) goto werr;

            /* When this RDB is produced as part of an AOF rewrite, move
             * accumulated diff from parent to child while rewriting in
             * order to have a smaller final write. 
             *
             * 当这个RDB作为AOF重写的一部分生成时，在重写时将累积的diff从父进程移动到子进程，以便获得较小的最终写入。
             * */
            if (rdbflags & RDBFLAGS_AOF_PREAMBLE &&
                rdb->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES)
            {
                processed = rdb->processed_bytes;
                aofReadDiffFromParent();
            }
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. 
                    *
                    * 这样我们就不会在出现错误时再次发布它。
                    * */
    }

    /* If we are storing the replication information on disk, persist
     * the script cache as well: on successful PSYNC after a restart, we need
     * to be able to process any EVALSHA inside the replication backlog the
     * master will send us. 
     *
     * 如果我们将复制信息存储在磁盘上，也要保留脚本缓存：在重新启动后成功的PSYNC上，
     * 我们需要能够处理主机将发送给我们的复制囤积中的任何EVALSHA。
     * */
    if (rsi && dictSize(server.lua_scripts)) {
        di = dictGetIterator(server.lua_scripts);
        while((de = dictNext(di)) != NULL) {
            robj *body = dictGetVal(de);
            if (rdbSaveAuxField(rdb,"lua",3,body->ptr,sdslen(body->ptr)) == -1)
                goto werr;
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. 
                    *
                    * 这样我们就不会在出现错误时再次发布它。
                    * */
    }

    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_AFTER_RDB) == -1) goto werr;

    /* EOF opcode 
     *
     * EOF操作码
     * */
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. 
     *
     * CRC64校验和。如果校验和计算被禁用，它将为零，在这种情况下，加载代码将跳过检查。
     * */
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return C_OK;

werr:
    if (error) *error = errno;
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. 
 *
 * 这只是rdbSaveRio（）的一个包装器，它为生成的RDB转储添加了一个前缀和
 * 一个后缀。前缀是：
 * 
 * $EOF:<40 bytes unguessable hex string>\r\n
 * 
 * 而后缀是我们在前缀中宣布的40字节十六进制字符串。通过这种方式，接收有效载荷的进程可以了解有
 * 效载荷何时结束，而无需对内容进行任何处理。
 * */
int rdbSaveRioWithEOFMark(rio *rdb, int *error, rdbSaveInfo *rsi) {
    char eofmark[RDB_EOF_MARK_SIZE];

    startSaving(RDBFLAGS_REPLICATION);
    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) *error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    if (rdbSaveRio(rdb,error,RDBFLAGS_NONE,rsi) == C_ERR) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    stopSaving(1);
    return C_OK;

werr: /* Write error. 
       *
       * 写入错误。
       * */
    /* Set 'error' only if not already set by rdbSaveRio() call. 
     *
     * 仅当rdbSaveRio（）调用尚未设置时，才设置“error”。
     * */
    if (error && *error == 0) *error = errno;
    stopSaving(0);
    return C_ERR;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. 
 *
 * 将数据库保存在磁盘上。错误时返回C_ERR，成功时返回C_OK。
 * */
int rdbSave(char *filename, rdbSaveInfo *rsi) {
    char tmpfile[256];
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. 
                           *
                           * 错误消息的当前工作目录路径。
                           * */
    FILE *fp = NULL;
    rio rdb;
    int error = 0;

    // 以 "temp-<pid>.rdb" 格式创建临时文件名
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Failed opening the RDB file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        return C_ERR;
    }

    // 初始化 rio 文件
    rioInitWithFile(&rdb,fp);
    startSaving(RDBFLAGS_NONE);

    if (server.rdb_save_incremental_fsync)
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES);

    if (rdbSaveRio(&rdb,&error,RDBFLAGS_NONE,rsi) == C_ERR) {
        errno = error;
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers 
     *
     * 确保数据不会保留在操作系统的输出缓冲区中
     * */
    if (fflush(fp)) goto werr;
    if (fsync(fileno(fp))) goto werr;
    if (fclose(fp)) { fp = NULL; goto werr; }
    fp = NULL;
    
    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. 
     *
     * 使用RENAME确保只有在生成的DB文件正常的情况下才原子地更改DB文件。
     * */
    if (rename(tmpfile,filename) == -1) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }

    serverLog(LL_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    server.lastbgsave_status = C_OK;
    stopSaving(1);
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    if (fp) fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}

/*
 * 使用子进程保存数据库数据，不阻塞主进程
 */
int rdbSaveBackground(char *filename, rdbSaveInfo *rsi) {
    pid_t childpid;

    if (hasActiveChildProcess()) return C_ERR;

    // 修改服务器状态
    server.dirty_before_bgsave = server.dirty;
    server.lastbgsave_try = time(NULL);
    openChildInfoPipe();

    // 创建子进程
    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        int retval;

        /* Child 
         *
         * 子进程
         * */
        redisSetProcTitle("redis-rdb-bgsave");
        redisSetCpuAffinity(server.bgsave_cpulist);
        retval = rdbSave(filename,rsi);
        if (retval == C_OK) {
            sendChildCOWInfo(CHILD_TYPE_RDB, "RDB");
        }
        // 退出子进程
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent 
         *
         * 父进程
         * */
        if (childpid == -1) {
            closeChildInfoPipe();
            server.lastbgsave_status = C_ERR;
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,"Background saving started by pid %d",childpid);
        // 记录保存开始的时间
        server.rdb_save_time_start = time(NULL);
        // 记录子进程的 id
        server.rdb_child_pid = childpid;
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;
        // 在执行时关闭对数据库的 rehash
        // 避免 copy-on-write
        updateDictResizePolicy();
        return C_OK;
    }
    return C_OK; /* unreached 
                  *
                  * 未到达这里
                  * */
}

/* Note that we may call this function in signal handle 'sigShutdownHandler',
 * so we need guarantee all functions we call are async-signal-safe.
 * If  we call this function from signal handle, we won't call bg_unlik that
 * is not async-signal-safe. 
 *
 * 请注意，我们可能会在信号句柄“sigShutdownHandler”中调用此函数
 * ，因此我们需要确保我们调用的所有函数都是异步信号安全的。如果我们从信号句柄调用这
 * 个函数，我们就不会调用不异步信号安全的bg_unlik。
 * */
void rdbRemoveTempFile(pid_t childpid, int from_signal) {
    char tmpfile[256];
    char pid[32];

    /* Generate temp rdb file name using aync-signal safe functions. 
     *
     * 使用aync信号安全函数生成临时rdb文件名。
     * */
    int pid_len = ll2string(pid, sizeof(pid), childpid);
    strcpy(tmpfile, "temp-");
    strncpy(tmpfile+5, pid, pid_len);
    strcpy(tmpfile+5+pid_len, ".rdb");

    if (from_signal) {
        /* bg_unlink is not async-signal-safe, but in this case we don't really
         * need to close the fd, it'll be released when the process exists. 
         *
         * bg_unlink不是异步信号安全的，但在这种情况下，我们真的不需要关闭fd，它
         * 会在进程存在时释放。
         * */
        int fd = open(tmpfile, O_RDONLY|O_NONBLOCK);
        UNUSED(fd);
        unlink(tmpfile);
    } else {
        bg_unlink(tmpfile);
    }
}

/* This function is called by rdbLoadObject() when the code is in RDB-check
 * mode and we find a module value of type 2 that can be parsed without
 * the need of the actual module. The value is parsed for errors, finally
 * a dummy redis object is returned just to conform to the API. 
 *
 * 当代码处于RDB检查模式时，rdbLoadObject（）会调用此函数，并且我们
 * 会发现类型为2的模块值，该值可以在不需要实际模块的情况下进行解析。对该值进行错误
 * 分析，最后返回一个伪reds对象以符合API。
 * */
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename) {
    uint64_t opcode;
    while((opcode = rdbLoadLen(rdb,NULL)) != RDB_MODULE_OPCODE_EOF) {
        if (opcode == RDB_MODULE_OPCODE_SINT ||
            opcode == RDB_MODULE_OPCODE_UINT)
        {
            uint64_t len;
            if (rdbLoadLenByRef(rdb,NULL,&len) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading integer from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            robj *o = rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
            if (o == NULL) {
                rdbExitReportCorruptRDB(
                    "Error reading string from module %s value", modulename);
            }
            decrRefCount(o);
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            float val;
            if (rdbLoadBinaryFloatValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading float from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            double val;
            if (rdbLoadBinaryDoubleValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading double from module %s value", modulename);
            }
        }
    }
    return createStringObject("module-dummy-value",18);
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. 
 *
 * 从指定的文件中加载指定类型的Redis对象。成功后，将返回一个新分配的对象，否则为NULL。
 * */
robj *rdbLoadObject(int rdbtype, rio *rdb, sds key) {
    robj *o = NULL, *ele, *dec;
    uint64_t len;
    unsigned int i;

    // 读取并返回字符串对象
    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value 
         *
         * 读取字符串值
         * */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (rdbtype == RDB_TYPE_LIST) {
        /* Read list value 
         *
         * 读取列表值
         * */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        /* Load every single element of the list 
         *
         * 加载列表中的每个元素
         * */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            dec = getDecodedObject(ele);
            size_t len = sdslen(dec->ptr);
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read Set value 
         *
         * 读取并返回集合对象
         * */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. 
         *
         * 当节点太多时，请使用正则集。
         * */
        // 根据元素的数量决定集合的编码
        size_t max_entries = server.set_max_intset_entries;
        if (max_entries >= 1<<30) max_entries = 1<<30;
        if (len > max_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing 
             *
             * 为了避免重复，尽快将dict扩展到合适的大小会更快
             * */
            if (len > DICT_HT_INITIAL_SIZE)
                dictExpand(o->ptr,len);
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the set 
         *
         * 加载集合中的每个元素
         * */
        // 载入所有集合元素
        for (i = 0; i < len; i++) {
            long long llval;
            sds sdsele;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }

            if (o->encoding == OBJ_ENCODING_INTSET) {
                /* Fetch integer value from element. 
                 *
                 * 从元素中获取整数值。
                 * */
                if (isSdsRepresentableAsLongLong(sdsele,&llval) == C_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                } else {
                    setTypeConvert(o,OBJ_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set. 
             *
             * 当集合刚刚转换为常规哈希表编码的集合时，也会调用此函数。
             * */
            if (o->encoding == OBJ_ENCODING_HT) {
                dictAdd((dict*)o->ptr,sdsele,NULL);
            } else {
                sdsfree(sdsele);
            }
        }
        
    } else if (rdbtype == RDB_TYPE_ZSET_2 || rdbtype == RDB_TYPE_ZSET) {
        /* Read list/set value. 
         *
         * 读取列表/设置值。
         * */
        uint64_t zsetlen;
        size_t maxelelen = 0, totelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        o = createZsetObject();
        zs = o->ptr;

        if (zsetlen > DICT_HT_INITIAL_SIZE)
            dictExpand(zs->dict,zsetlen);

        /* Load every single element of the sorted set. 
         *
         * 加载排序集的每个元素。
         * */
        while(zsetlen--) {
            sds sdsele;
            double score;
            zskiplistNode *znode;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }

            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            } else {
                if (rdbLoadDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            }

            /* Don't care about integer-encoded strings. 
             *
             * 不关心整数编码的字符串。
             * */
            if (sdslen(sdsele) > maxelelen) maxelelen = sdslen(sdsele);
            totelelen += sdslen(sdsele);

            znode = zslInsert(zs->zsl,score,sdsele);
            dictAdd(zs->dict,sdsele,&znode->score);
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. 
         *
         * 加载后转换*，因为已排序的集合不是按顺序存储的。
         * */
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value &&
            ziplistSafeToAdd(NULL, totelelen))
        {
            zsetConvert(o,OBJ_ENCODING_ZIPLIST);
        }

        // 载入哈希
    } else if (rdbtype == RDB_TYPE_HASH) {
        uint64_t len;
        int ret;
        sds field, value;

        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;

        o = createHashObject();

        /* Too many entries? Use a hash table. 
         *
         * 节点太多？使用哈希表。
         * */
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);

        /* Load every field and value into the ziplist 
         *
         * 将每个字段和值加载到压缩列表中
         * */
        while (o->encoding == OBJ_ENCODING_ZIPLIST && len > 0) {
            len--;
            /* Load raw strings 
             *
             * 加载原始字符串
             * */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                sdsfree(field);
                decrRefCount(o);
                return NULL;
            }

            /* Convert to hash table if size threshold is exceeded 
             *
             * 如果超过大小阈值，则转换为哈希表
             * */
            if (sdslen(field) > server.hash_max_ziplist_value ||
                sdslen(value) > server.hash_max_ziplist_value ||
                !ziplistSafeToAdd(o->ptr, sdslen(field)+sdslen(value)))
            {
                hashTypeConvert(o, OBJ_ENCODING_HT);
                ret = dictAdd((dict*)o->ptr, field, value);
                if (ret == DICT_ERR) {
                    rdbExitReportCorruptRDB("Duplicate hash fields detected");
                }
                break;
            }

            /* Add pair to ziplist 
             *
             * 将双鞋添加到拉链列表
             * */
            o->ptr = ziplistPush(o->ptr, (unsigned char*)field,
                    sdslen(field), ZIPLIST_TAIL);
            o->ptr = ziplistPush(o->ptr, (unsigned char*)value,
                    sdslen(value), ZIPLIST_TAIL);

            sdsfree(field);
            sdsfree(value);
        }

        if (o->encoding == OBJ_ENCODING_HT && len > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,len);

        /* Load remaining fields and values into the hash table 
         *
         * 将剩余字段和值加载到哈希表中
         * */
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            len--;
            /* Load encoded strings 
             *
             * 加载编码字符串
             * */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                sdsfree(field);
                decrRefCount(o);
                return NULL;
            }

            /* Add pair to hash table 
             *
             * 将对添加到哈希表
             * */
            ret = dictAdd((dict*)o->ptr, field, value);
            if (ret == DICT_ERR) {
                rdbExitReportCorruptRDB("Duplicate keys detected");
            }
        }

        /* All pairs should be read by now 
         *
         * 现在应该读取所有配对
         * */
        serverAssert(len == 0);
    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST) {
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        while (len--) {
            unsigned char *zl =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (zl == NULL) {
                decrRefCount(o);
                return NULL;
            }
            quicklistAppendZiplist(o->ptr, zl);
        }
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST)
    {
        unsigned char *encoded =
            rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
        if (encoded == NULL) return NULL;
        o = createObject(OBJ_STRING,encoded); /* Obj type fixed below. 
                                               *
                                               * 对象类型固定在下面。
                                               * */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. 
         *
         * 修复对象编码，如果根据当前配置，编码数据类型中有太多元素，请确保将编码数据类型转
         * 换为基类型。注意，我们只检查长度，而不是最大元素大小，因为这是O（N）扫描。最终
         * 一切都会改变。
         * */
        // 根据 rdbtype ，设置对象的类型和编码
        // 并检查是否有需要进行编码转换
        switch(rdbtype) {
            // 2.6 之后已经废弃
            case RDB_TYPE_HASH_ZIPMAP:
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. 
                 *
                 * 转换为ziplist编码的哈希。当Redis 2.4创建的加载转储被弃用时，必须
                 * 弃用此选项。
                 * */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        if (!ziplistSafeToAdd(zl, (size_t)flen + vlen)) {
                            rdbExitReportCorruptRDB("Hash zipmap too big (%u)", flen);
                        }

                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries ||
                        maxlen > server.hash_max_ziplist_value)
                    {
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                }
                break;
            case RDB_TYPE_LIST_ZIPLIST:
                o->type = OBJ_LIST;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                listTypeConvert(o,OBJ_ENCODING_QUICKLIST);
                break;
            case RDB_TYPE_SET_INTSET:
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_ZSET_ZIPLIST:
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)
                    zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                break;
            case RDB_TYPE_HASH_ZIPLIST:
                o->type = OBJ_HASH;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)
                    hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            default:
                /* totally unreachable 
                 *
                 * 完全无法到达
                 * */
                rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }
    } else if (rdbtype == RDB_TYPE_STREAM_LISTPACKS) {
        o = createStreamObject();
        stream *s = o->ptr;
        uint64_t listpacks = rdbLoadLen(rdb,NULL);
        if (listpacks == RDB_LENERR) {
            rdbReportReadError("Stream listpacks len loading failed.");
            decrRefCount(o);
            return NULL;
        }

        while(listpacks--) {
            /* Get the master ID, the one we'll use as key of the radix tree
             * node: the entries inside the listpack itself are delta-encoded
             * relatively to this ID. 
             *
             * 获取主ID，我们将使用它作为基数树节点的键：列表包本身中的节点相对于该ID进行delta编码。
             * */
            sds nodekey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (nodekey == NULL) {
                rdbReportReadError("Stream master ID loading failed: invalid encoding or I/O error.");
                decrRefCount(o);
                return NULL;
            }
            if (sdslen(nodekey) != sizeof(streamID)) {
                rdbExitReportCorruptRDB("Stream node key entry is not the "
                                        "size of a stream ID");
            }

            /* Load the listpack. 
             *
             * 加载 listpack。
             * */
            unsigned char *lp =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (lp == NULL) {
                rdbReportReadError("Stream listpacks loading failed.");
                sdsfree(nodekey);
                decrRefCount(o);
                return NULL;
            }
            unsigned char *first = lpFirst(lp);
            if (first == NULL) {
                /* Serialized listpacks should never be empty, since on
                 * deletion we should remove the radix tree key if the
                 * resulting listpack is empty. 
                 *
                 * 序列化的列表包永远不应该为空，因为在删除时，如果生成的列表包为空，我们应该删除基
                 * 数树键。
                 * */
                rdbExitReportCorruptRDB("Empty listpack inside stream");
            }

            /* Insert the key in the radix tree. 
             *
             * 在基数树中插入键。
             * */
            int retval = raxInsert(s->rax,
                (unsigned char*)nodekey,sizeof(streamID),lp,NULL);
            sdsfree(nodekey);
            if (!retval)
                rdbExitReportCorruptRDB("Listpack re-added with existing key");
        }
        /* Load total number of items inside the stream. 
         *
         * 加载流中的项目总数。
         * */
        s->length = rdbLoadLen(rdb,NULL);

        /* Load the last entry ID. 
         *
         * 加载最后一个节点ID。
         * */
        s->last_id.ms = rdbLoadLen(rdb,NULL);
        s->last_id.seq = rdbLoadLen(rdb,NULL);

        if (rioGetReadError(rdb)) {
            rdbReportReadError("Stream object metadata loading failed.");
            decrRefCount(o);
            return NULL;
        }

        /* Consumer groups loading 
         *
         * 消费者组加载
         * */
        uint64_t cgroups_count = rdbLoadLen(rdb,NULL);
        if (cgroups_count == RDB_LENERR) {
            rdbReportReadError("Stream cgroup count loading failed.");
            decrRefCount(o);
            return NULL;
        }
        while(cgroups_count--) {
            /* Get the consumer group name and ID. We can then create the
             * consumer group ASAP and populate its structure as
             * we read more data. 
             *
             * 获取消费者组名称和ID。然后，我们可以尽快创建消费者组，并在读取更多数据时填充其
             * 结构。
             * */
            streamID cg_id;
            sds cgname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (cgname == NULL) {
                rdbReportReadError(
                    "Error reading the consumer group name from Stream");
                decrRefCount(o);
                return NULL;
            }

            cg_id.ms = rdbLoadLen(rdb,NULL);
            cg_id.seq = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) {
                rdbReportReadError("Stream cgroup ID loading failed.");
                sdsfree(cgname);
                decrRefCount(o);
                return NULL;
            }

            streamCG *cgroup = streamCreateCG(s,cgname,sdslen(cgname),&cg_id);
            if (cgroup == NULL)
                rdbExitReportCorruptRDB("Duplicated consumer group name %s",
                                         cgname);
            sdsfree(cgname);

            /* Load the global PEL for this consumer group, however we'll
             * not yet populate the NACK structures with the message
             * owner, since consumers for this group and their messages will
             * be read as a next step. So for now leave them not resolved
             * and later populate it. 
             *
             * 加载该使用者组的全局PEL，但是我们还不会用消息所有者填充NACK结构，因为下一
             * 步将读取该组的使用者及其消息。因此，暂时不解决这些问题，以后再填充。
             * */
            uint64_t pel_size = rdbLoadLen(rdb,NULL);
            if (pel_size == RDB_LENERR) {
                rdbReportReadError("Stream PEL size loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                    rdbReportReadError("Stream PEL ID loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                streamNACK *nack = streamCreateNACK(NULL);
                nack->delivery_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                nack->delivery_count = rdbLoadLen(rdb,NULL);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream PEL NACK loading failed.");
                    decrRefCount(o);
                    streamFreeNACK(nack);
                    return NULL;
                }
                if (!raxInsert(cgroup->pel,rawid,sizeof(rawid),nack,NULL))
                    rdbExitReportCorruptRDB("Duplicated gobal PEL entry "
                                            "loading stream consumer group");
            }

            /* Now that we loaded our global PEL, we need to load the
             * consumers and their local PELs. 
             *
             * 现在我们加载了全球PEL，我们需要加载消费者及其本地PEL。
             * */
            uint64_t consumers_num = rdbLoadLen(rdb,NULL);
            if (consumers_num == RDB_LENERR) {
                rdbReportReadError("Stream consumers num loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(consumers_num--) {
                sds cname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
                if (cname == NULL) {
                    rdbReportReadError(
                        "Error reading the consumer name from Stream group.");
                    decrRefCount(o);
                    return NULL;
                }
                streamConsumer *consumer =
                    streamLookupConsumer(cgroup,cname,SLC_NONE);
                sdsfree(cname);
                consumer->seen_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream short read reading seen time.");
                    decrRefCount(o);
                    return NULL;
                }

                /* Load the PEL about entries owned by this specific
                 * consumer. 
                 *
                 * 加载关于此特定使用者拥有的节点的PEL。
                 * */
                pel_size = rdbLoadLen(rdb,NULL);
                if (pel_size == RDB_LENERR) {
                    rdbReportReadError(
                        "Stream consumer PEL num loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                while(pel_size--) {
                    unsigned char rawid[sizeof(streamID)];
                    if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                        rdbReportReadError(
                            "Stream short read reading PEL streamID.");
                        decrRefCount(o);
                        return NULL;
                    }
                    streamNACK *nack = raxFind(cgroup->pel,rawid,sizeof(rawid));
                    if (nack == raxNotFound)
                        rdbExitReportCorruptRDB("Consumer entry not found in "
                                                "group global PEL");

                    /* Set the NACK consumer, that was left to NULL when
                     * loading the global PEL. Then set the same shared
                     * NACK structure also in the consumer-specific PEL. 
                     *
                     * 设置NACK使用者，在加载全局PEL时将其保留为NULL。然后在消费者特定PEL
                     * 中也设置相同的共享NACK结构。
                     * */
                    nack->consumer = consumer;
                    if (!raxInsert(consumer->pel,rawid,sizeof(rawid),nack,NULL))
                        rdbExitReportCorruptRDB("Duplicated consumer PEL entry "
                                                " loading a stream consumer "
                                                "group");
                }
            }
        }
    } else if (rdbtype == RDB_TYPE_MODULE || rdbtype == RDB_TYPE_MODULE_2) {
        uint64_t moduleid = rdbLoadLen(rdb,NULL);
        if (rioGetReadError(rdb)) {
            rdbReportReadError("Short read module id");
            return NULL;
        }
        moduleType *mt = moduleTypeLookupModuleByID(moduleid);
        char name[10];

        if (rdbCheckMode && rdbtype == RDB_TYPE_MODULE_2) {
            moduleTypeNameByID(name,moduleid);
            return rdbLoadCheckModuleValue(rdb,name);
        }

        if (mt == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data I can't load: no matching module '%s'", name);
            exit(1);
        }
        RedisModuleIO io;
        robj keyobj;
        initStaticStringObject(keyobj,key);
        moduleInitIOContext(io,mt,rdb,&keyobj);
        io.ver = (rdbtype == RDB_TYPE_MODULE) ? 1 : 2;
        /* Call the rdb_load method of the module providing the 10 bit
         * encoding version in the lower 10 bits of the module ID. 
         *
         * 调用模块ID的低10位中提供10位编码版本的模块的rdb_load方法。
         * */
        void *ptr = mt->rdb_load(&io,moduleid&1023);
        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        /* Module v2 serialization has an EOF mark at the end. 
         *
         * 模块v2序列化的末尾有一个EOF标记。
         * */
        if (io.ver == 2) {
            uint64_t eof = rdbLoadLen(rdb,NULL);
            if (eof == RDB_LENERR) {
                o = createModuleObject(mt,ptr); /* creating just in order to easily destroy 
                                                 *
                                                 * 创造只是为了轻易摧毁
                                                 * */
                decrRefCount(o);
                return NULL;
            }
            if (eof != RDB_MODULE_OPCODE_EOF) {
                serverLog(LL_WARNING,"The RDB file contains module data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                exit(1);
            }
        }

        if (ptr == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
            exit(1);
        }
        o = createModuleObject(mt,ptr);
    } else {
        rdbReportReadError("Unknown RDB encoding type %d",rdbtype);
        return NULL;
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. 
 *
 * 标记我们正在全局状态下加载，并设置提供加载统计信息所需的字段。
 * */
/*
 * 更新文件状态
 */
void startLoading(size_t size, int rdbflags) {
    /* Load the DB 
     *
     * 加载数据库
     * */
    server.loading = 1;
    server.loading_start_time = time(NULL);
    server.loading_loaded_bytes = 0;
    server.loading_total_bytes = size;

    /* Fire the loading modules start event. 
     *
     * 激发加载模块启动事件。
     * */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_LOADING_AOF_START;
    else if(rdbflags & RDBFLAGS_REPLICATION)
        subevent = REDISMODULE_SUBEVENT_LOADING_REPL_START;
    else
        subevent = REDISMODULE_SUBEVENT_LOADING_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,subevent,NULL);
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats.
 * 'filename' is optional and used for rdb-check on error 
 *
 * 标记我们正在全局状态下加载，并设置提供加载统计信息所需的字段。'filename'是可选的，用于错误时的rdb检查
 * */
void startLoadingFile(FILE *fp, char* filename, int rdbflags) {
    struct stat sb;
    if (fstat(fileno(fp), &sb) == -1)
        sb.st_size = 0;
    rdbFileBeingLoaded = filename;
    startLoading(sb.st_size, rdbflags);
}

/* Refresh the loading progress info 
 *
 * 刷新加载进度信息
 * */
/*
 * 更新最大分配内存数
 */
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished 
 *
 * 加载完成
 * */
void stopLoading(int success) {
    server.loading = 0;
    rdbFileBeingLoaded = NULL;

    /* Fire the loading modules end event. 
     *
     * 激发加载模块结束事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,
                          success?
                            REDISMODULE_SUBEVENT_LOADING_ENDED:
                            REDISMODULE_SUBEVENT_LOADING_FAILED,
                          NULL);
}

void startSaving(int rdbflags) {
    /* Fire the persistence modules end event. 
     *
     * 激发持久性模块结束事件。
     * */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START;
    else if (getpid()!=server.pid)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START;
    else
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,subevent,NULL);
}

void stopSaving(int success) {
    /* Fire the persistence modules end event. 
     *
     * 激发持久性模块结束事件。
     * */
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,
                          success?
                            REDISMODULE_SUBEVENT_PERSISTENCE_ENDED:
                            REDISMODULE_SUBEVENT_PERSISTENCE_FAILED,
                          NULL);
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  
 *
 * 跟踪加载进度，以便不时为客户端提供服务，并在需要时计算rdb校验和
 * */
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        /* The DB can take some non trivial amount of time to load. Update
         * our cached time since it is used to create and update the last
         * interaction time with clients and for other important things. 
         *
         * DB可能需要花费一些不平凡的时间来加载。更新我们的缓存时间，因为它用于创建和更新
         * 与客户端的最后一次交互时间以及其他重要事项。
         * */
        updateCachedTime(0);
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
            replicationSendNewlineToMaster();
        loadingProgress(r->processed_bytes);
        processEventsWhileBlocked();
        processModuleLoadingProgressEvent(0);
    }
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned and 'errno' is set accordingly. 
 *
 * 从rio流“RDB”加载RDB文件。成功时返回C_OK，否则返回C_ERR并相应
 * 设置“errno”。
 * */
int rdbLoadRio(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    uint64_t dbid;
    int type, rdbver;
    redisDb *db = server.db+0;
    char buf[1024];

    rdb->update_cksum = rdbLoadProgressCallback;
    rdb->max_processing_chunk = server.loading_process_events_interval_bytes;
    if (rioRead(rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return C_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return C_ERR;
    }

    /* Key-specific attributes, set by opcodes before the key type. 
     *
     * 键特定的属性，由操作码在键类型之前设置。
     * */
    long long lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
    long long lru_clock = LRU_CLOCK();

    while(1) {
        sds key;
        robj *val;

        /* Read type. 
         *
         * 读取类型。
         * */
        if ((type = rdbLoadType(rdb)) == -1) goto eoferr;

        /* Handle special types. 
         *
         * 处理特殊类型。
         * */
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. 
             *
             * EXPIRETIME：加载与要加载的下一个键关联的过期。请注意，在加载expi
             * re之后，我们需要加载实际类型，然后继续。
             * */
            expiretime = rdbLoadTime(rdb);
            expiretime *= 1000;
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. 
                       *
                       * 读取下一个操作码。
                       * */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. 
             *
             * EXPIRETIME_MS：RDB v3引入的毫秒精度过期时间。像EXPIRET
             * IME，但没有更精确。
             * */
            expiretime = rdbLoadMillisecondTime(rdb,rdbver);
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. 
                       *
                       * 读取下一个操作码。
                       * */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. 
             *
             * FREQ:LFU频率。
             * */
            uint8_t byte;
            if (rioRead(rdb,&byte,1) == 0) goto eoferr;
            lfu_freq = byte;
            continue; /* Read next opcode. 
                       *
                       * 读取下一个操作码。
                       * */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. 
             *
             * IDLE：LRU空闲时间。
             * */
            uint64_t qword;
            if ((qword = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            lru_idle = qword;
            continue; /* Read next opcode. 
                       *
                       * 读取下一个操作码。
                       * */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. 
             *
             * EOF：文件结束，退出主循环。
             * */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. 
             *
             * SELECTDB：选择指定的数据库。
             * */
            if ((dbid = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            continue; /* Read next opcode. 
                       *
                       * 读取下一个操作码。
                       * */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. 
             *
             * RESIZEDB:提示当前所选数据库中键的大小，以避免无用的重新哈希。
             * */
            uint64_t db_size, expires_size;
            if ((db_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            dictExpand(db->dict,db_size);
            dictExpand(db->expires,expires_size);
            continue; /* Read next opcode. 
                       *
                       * 读取下一个操作码。
                       * */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are required to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. 
             *
             * AUX：通用字符串字段。用于向向后兼容的RDB添加状态。RDB加载的实现需要跳过
             * 他们不理解的AUX字段。AUX字段由两个字符串组成：键和值。
             * */
            robj *auxkey, *auxval;
            if ((auxkey = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(rdb)) == NULL) goto eoferr;

            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered
                 * information fields and are logged at startup with a log
                 * level of NOTICE. 
                 *
                 * 所有名称以“%”开头的字段都被视为信息字段，并且在启动时以NOTICE的日志级别
                 * 进行记录。
                 * */
                serverLog(LL_NOTICE,"RDB '%s': %s",
                    (char*)auxkey->ptr,
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-stream-db")) {
                if (rsi) rsi->repl_stream_db = atoi(auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-id")) {
                if (rsi && sdslen(auxval->ptr) == CONFIG_RUN_ID_SIZE) {
                    memcpy(rsi->repl_id,auxval->ptr,CONFIG_RUN_ID_SIZE+1);
                    rsi->repl_id_is_set = 1;
                }
            } else if (!strcasecmp(auxkey->ptr,"repl-offset")) {
                if (rsi) rsi->repl_offset = strtoll(auxval->ptr,NULL,10);
            } else if (!strcasecmp(auxkey->ptr,"lua")) {
                /* Load the script back in memory. 
                 *
                 * 将脚本加载回内存。
                 * */
                if (luaCreateFunction(NULL,server.lua,auxval) == NULL) {
                    rdbExitReportCorruptRDB(
                        "Can't load Lua script from RDB file! "
                        "BODY: %s", (char*)auxval->ptr);
                }
            } else if (!strcasecmp(auxkey->ptr,"redis-ver")) {
                serverLog(LL_NOTICE,"Loading RDB produced by version %s",
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"ctime")) {
                time_t age = time(NULL)-strtol(auxval->ptr,NULL,10);
                if (age < 0) age = 0;
                serverLog(LL_NOTICE,"RDB age %ld seconds",
                    (unsigned long) age);
            } else if (!strcasecmp(auxkey->ptr,"used-mem")) {
                long long usedmem = strtoll(auxval->ptr,NULL,10);
                serverLog(LL_NOTICE,"RDB memory usage when created %.2f Mb",
                    (double) usedmem / (1024*1024));
            } else if (!strcasecmp(auxkey->ptr,"aof-preamble")) {
                long long haspreamble = strtoll(auxval->ptr,NULL,10);
                if (haspreamble) serverLog(LL_NOTICE,"RDB has an AOF tail");
            } else if (!strcasecmp(auxkey->ptr,"redis-bits")) {
                /* Just ignored. 
                 *
                 * 只是忽略了。
                 * */
            } else {
                /* We ignore fields we don't understand, as by AUX field
                 * contract. 
                 *
                 * 我们忽略了我们不理解的字段，就像AUX字段合同一样。
                 * */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'",
                    (char*)auxkey->ptr);
            }

            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. 
                       *
                       * 再次读取类型。
                       * */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* Load module data that is not related to the Redis key space.
             * Such data can be potentially be stored both before and after the
             * RDB keys-values section. 
             *
             * 加载与Redis键空间无关的模块数据。这样的数据可能存储在RDB键值部分之前和
             * 之后。
             * */
            uint64_t moduleid = rdbLoadLen(rdb,NULL);
            int when_opcode = rdbLoadLen(rdb,NULL);
            int when = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) goto eoferr;
            if (when_opcode != RDB_MODULE_OPCODE_UINT) {
                rdbReportReadError("bad when_opcode");
                goto eoferr;
            }
            moduleType *mt = moduleTypeLookupModuleByID(moduleid);
            char name[10];
            moduleTypeNameByID(name,moduleid);

            if (!rdbCheckMode && mt == NULL) {
                /* Unknown module. 
                 *
                 * 未知模块。
                 * */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load: no matching module '%s'", name);
                exit(1);
            } else if (!rdbCheckMode && mt != NULL) {
                if (!mt->aux_load) {
                    /* Module doesn't support AUX. 
                     *
                     * 模块不支持AUX。
                     * */
                    serverLog(LL_WARNING,"The RDB file contains module AUX data, but the module '%s' doesn't seem to support it.", name);
                    exit(1);
                }

                RedisModuleIO io;
                moduleInitIOContext(io,mt,rdb,NULL);
                io.ver = 2;
                /* Call the rdb_load method of the module providing the 10 bit
                 * encoding version in the lower 10 bits of the module ID. 
                 *
                 * 调用模块ID的低10位中提供10位编码版本的模块的rdb_load方法。
                 * */
                if (mt->aux_load(&io,moduleid&1023, when) != REDISMODULE_OK || io.error) {
                    moduleTypeNameByID(name,moduleid);
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
                    goto eoferr;
                }
                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }
                uint64_t eof = rdbLoadLen(rdb,NULL);
                if (eof != RDB_MODULE_OPCODE_EOF) {
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                    goto eoferr;
                }
                continue;
            } else {
                /* RDB check mode. 
                 *
                 * RDB检查模式。
                 * */
                robj *aux = rdbLoadCheckModuleValue(rdb,name);
                decrRefCount(aux);
                continue; /* Read next opcode. 
                       *
                       * 读取下一个操作码。
                       * */
            }
        }

        /* Read key 
         *
         * 读取键
         * */
        if ((key = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL)
            goto eoferr;
        /* Read value 
         *
         * 读取值
         * */
        if ((val = rdbLoadObject(type,rdb,key)) == NULL) {
            sdsfree(key);
            goto eoferr;
        }

        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave.
         * Similarly if the RDB is the preamble of an AOF file, we want to
         * load all the keys as they are, since the log of operations later
         * assume to work in an exact keyspace state. 
         *
         * 检查键是否已过期。当从磁盘加载RDB文件时，无论是在启动时，还是从主机接收到RDB时，
         * 都会使用此函数。在后一种情况下，主机负责键过期。如果我们在这里使键过期，那么主设备
         * 拍摄的快照可能不会反映在从属设备上。类似地，如果RDB是AOF文件的前导，我们希望按原样加载所有键，
         * 因为操作日志稍后会假设在精确的键空间状态下工作。
         * */
        if (iAmMaster() &&
            !(rdbflags&RDBFLAGS_AOF_PREAMBLE) &&
            expiretime != -1 && expiretime < now)
        {
            sdsfree(key);
            decrRefCount(val);
        } else {
            robj keyobj;
            initStaticStringObject(keyobj,key);

            /* Add the new object in the hash table 
             *
             * 在哈希表中添加新对象
             * */
            int added = dbAddRDBLoad(db,key,val);
            if (!added) {
                if (rdbflags & RDBFLAGS_ALLOW_DUP) {
                    /* This flag is useful for DEBUG RELOAD special modes.
                     * When it's set we allow new keys to replace the current
                     * keys with the same name. 
                     *
                     * 此标志对于DEBUG RELOAD特殊模式非常有用。当它被设置时，我们允许新的密
                     * 钥用相同的名称替换当前的键。
                     * */
                    dbSyncDelete(db,&keyobj);
                    dbAddRDBLoad(db,key,val);
                } else {
                    serverLog(LL_WARNING,
                        "RDB has duplicated key '%s' in DB %d",key,db->id);
                    serverPanic("Duplicated key found in RDB file");
                }
            }

            /* Set the expire time if needed 
             *
             * 如果需要，设置过期时间
             * */
            if (expiretime != -1) {
                setExpire(NULL,db,&keyobj,expiretime);
            }

            /* Set usage information (for eviction). 
             *
             * 设置使用信息（用于驱逐）。
             * */
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock,1000);

            /* call key space notification on key loaded for modules only 
             *
             * 仅为模块加载的键上的调用键空间通知
             * */
            moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", &keyobj, db->id);
        }

        /* Loading the database more slowly is useful in order to test
         * certain edge cases. 
         *
         * 较慢地加载数据库对于测试某些边缘情况非常有用。
         * */
        if (server.key_load_delay) usleep(server.key_load_delay);

        /* Reset the state that is key-specified and is populated by
         * opcodes before the key, so that we start from scratch again. 
         *
         * 重置键指定的状态，该状态由键之前的操作码填充，这样我们就可以从头开始了。
         * */
        expiretime = -1;
        lfu_freq = -1;
        lru_idle = -1;
    }
    /* Verify the checksum if RDB version is >= 5 
     *
     * 如果RDB版本>=5，请验证校验和
     * */
    if (rdbver >= 5) {
        uint64_t cksum, expected = rdb->cksum;

        if (rioRead(rdb,&cksum,8) == 0) goto eoferr;
        if (server.rdb_checksum) {
            memrev64ifbe(&cksum);
            if (cksum == 0) {
                serverLog(LL_WARNING,"RDB file was saved with checksum disabled: no check performed.");
            } else if (cksum != expected) {
                serverLog(LL_WARNING,"Wrong RDB checksum expected: (%llx) but "
                    "got (%llx). Aborting now.",
                        (unsigned long long)expected,
                        (unsigned long long)cksum);
                rdbExitReportCorruptRDB("RDB CRC error");
            }
        }
    }
    return C_OK;

    /* Unexpected end of file is handled here calling rdbReportReadError():
     * this will in turn either abort Redis in most cases, or if we are loading
     * the RDB file from a socket during initial SYNC (diskless replica mode),
     * we'll report the error to the caller, so that we can retry. 
     *
     * 这里通过调用rdbReportReadError（）来处理意外的文件结尾：在大多
     * 数情况下，这将反过来中止Redis，或者如果我们在初始SYNC（无盘复制模式）期
     * 间从套接字加载RDB文件，我们将向调用方报告错误，以便重试。
     * */
eoferr:
    serverLog(LL_WARNING,
        "Short read or OOM loading DB. Unrecoverable error, aborting now.");
    rdbReportReadError("Unexpected EOF reading RDB file");
    return C_ERR;
}

/* Like rdbLoadRio() but takes a filename instead of a rio stream. The
 * filename is open for reading and a rio stream object created in order
 * to do the actual loading. Moreover the ETA displayed in the INFO
 * output is initialized and finalized.
 *
 * If you pass an 'rsi' structure initialied with RDB_SAVE_OPTION_INIT, the
 * loading code will fiil the information fields in the structure. 
 *
 * 与rdbLoadRio（）类似，但使用文件名而不是rio流。打开文件名进行读取，
 * 并创建一个rio流对象以进行实际加载。此外，INFO输出中显示的ETA被初始化并
 * 最终确定。如果传递用RDB_SAVE_OPTION_INIT初始化的“rsi”结构，则加载代码将填充结构中的信息字段。
 * */
/*
 * 读取 rdb 文件，并将其中的对象保存到内存中
 */
int rdbLoad(char *filename, rdbSaveInfo *rsi, int rdbflags) {
    FILE *fp;
    rio rdb;
    int retval;

    // 打开文件
    if ((fp = fopen(filename,"r")) == NULL) return C_ERR;
    startLoadingFile(fp, filename,rdbflags);
    // 初始化 rdb 文件
    rioInitWithFile(&rdb,fp);
    // 检查 rdb 文件头（“REDIS”字符串，以及版本号）
    retval = rdbLoadRio(&rdb,rdbflags,rsi);
    fclose(fp);
    stopLoading(retval==C_OK);
    return retval;
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. 
 *
 * 后台保存子项（BGSAVE）终止了其工作。处理好这个。此功能涵盖实际BGSAVE的情况。
 * */
static void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = C_OK;
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        server.lastbgsave_status = C_ERR;
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.rdb_child_pid, 0);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. 
         *
         * SIGUSR1被列入白名单，因此我们有一种在不触发错误条件的情况下终结子进程的方法
         * 。
         * */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Slaves socket transfers for
 * diskless replication. 
 *
 * 后台保存进程（BGSAVE）终止了其工作。处理好这个。此函数涵盖RDB->从节点套接字传输用于无盘复制的情况。
 * */
/*
 * 根据 BGSAVE 子进程的返回值，对服务器状态进行更新
 */
static void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    // 保存成功
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");

        // 保存失败
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");

        // 子进程被终结
    } else {
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }

    // 更新服务器状态
    if (server.rdb_child_exit_pipe!=-1)
        close(server.rdb_child_exit_pipe);
    aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
    close(server.rdb_pipe_read);
    server.rdb_child_exit_pipe = -1;
    server.rdb_pipe_read = -1;
    zfree(server.rdb_pipe_conns);
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    zfree(server.rdb_pipe_buff);
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
}

/* When a background RDB saving/transfer terminates, call the right handler. 
 *
 * 当后台RDB保存/传输终止时，调用正确的处理程序。
 * */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    int type = server.rdb_child_type;
    switch(server.rdb_child_type) {
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }

    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) 
     *
     * 可能有从机在等待BGSAVE以便得到服务（SYNC的第一阶段是dump.rdb的
     * 批量传输）
     * */
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, type);
}

/* Kill the RDB saving child using SIGUSR1 (so that the parent will know
 * the child did not exit for an error, but because we wanted), and performs
 * the cleanup needed. 
 *
 * 使用SIGUSR1杀死保存RDB的进程（这样父级就会知道子进程不是因为错误而退出，
 * 而是因为我们想要），并执行所需的清理。
 * */
void killRDBChild(void) {
    kill(server.rdb_child_pid,SIGUSR1);
    rdbRemoveTempFile(server.rdb_child_pid, 0);
    closeChildInfoPipe();
    updateDictResizePolicy();
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. 
 *
 * 生成一个子RDB，将RDB写入当前处于SLAVE_STATE_WAIT_BGSAVE_START状态的从机的套接字。
 * */
int rdbSaveToSlavesSockets(rdbSaveInfo *rsi) {
    listNode *ln;
    listIter li;
    pid_t childpid;
    int pipefds[2], rdb_pipe_write, safe_to_exit_pipe;

    if (hasActiveChildProcess()) return C_ERR;

    /* Even if the previous fork child exited, don't start a new one until we
     * drained the pipe. 
     *
     * 即使上一个 fork 子进程退出，也不要启动新的，直到我们排空管道。
     * */
    if (server.rdb_pipe_conns) return C_ERR;

    /* Before to fork, create a pipe that is used to transfer the rdb bytes to
     * the parent, we can't let it write directly to the sockets, since in case
     * of TLS we must let the parent handle a continuous TLS state when the
     * child terminates and parent takes over. 
     *
     * 在分叉之前，创建一个用于将rdb字节传输到父级的管道，我们不能让它直接写入套接字
     * ，因为在TLS的情况下，当子级终止并由父级接管时，我们必须让父级处理连续的TLS
     * 状态。
     * */
    if (pipe(pipefds) == -1) return C_ERR;
    server.rdb_pipe_read = pipefds[0]; /* read end 
                                        *
                                        * 读取结束
                                        * */
    rdb_pipe_write = pipefds[1]; /* write end 
                                  *
                                  * 写入结束
                                  * */
    anetNonBlock(NULL, server.rdb_pipe_read);

    /* create another pipe that is used by the parent to signal to the child
     * that it can exit. 
     *
     * 创建另一个管道，父进程使用该管道向子进程发出可以退出的信号。
     * */
    if (pipe(pipefds) == -1) {
        close(rdb_pipe_write);
        close(server.rdb_pipe_read);
        return C_ERR;
    }
    safe_to_exit_pipe = pipefds[0]; /* read end 
                                        *
                                        * 读取结束
                                        * */
    server.rdb_child_exit_pipe = pipefds[1]; /* write end 
                                              *
                                              * 写入结束
                                              * */

    /* Collect the connections of the replicas we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. 
     *
     * 收集我们要将RDB传输到的副本的连接，这些连接为i WAIT_BGSAVE_START状态。
     * */
    server.rdb_pipe_conns = zmalloc(sizeof(connection *)*listLength(server.slaves));
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            server.rdb_pipe_conns[server.rdb_pipe_numconns++] = slave->conn;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
        }
    }

    /* Create the child process. 
     *
     * 创建子进程。
     * */
    openChildInfoPipe();
    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        /* Child 
         *
         * 子进程
         * */
        int retval, dummy;
        rio rdb;

        rioInitWithFd(&rdb,rdb_pipe_write);

        redisSetProcTitle("redis-rdb-to-slaves");
        redisSetCpuAffinity(server.bgsave_cpulist);

        retval = rdbSaveRioWithEOFMark(&rdb,NULL,rsi);
        if (retval == C_OK && rioFlush(&rdb) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            sendChildCOWInfo(CHILD_TYPE_RDB, "RDB");
        }

        rioFreeFd(&rdb);
        /* wake up the reader, tell it we're done. 
         *
         * 唤醒读者，告诉他们我们完了。
         * */
        close(rdb_pipe_write);
        close(server.rdb_child_exit_pipe); /* close write end so that we can detect the close on the parent. 
                                            *
                                            * 关闭写结束，这样我们就可以检测到父对象上的关闭。
                                            * */
        /* hold exit until the parent tells us it's safe. we're not expecting
         * to read anything, just get the error when the pipe is closed. 
         *
         * 等到父进程告诉我们安全后再离开。我们不希望读取任何内容，只要在管道关闭时得到错误即可。
         * */
        dummy = read(safe_to_exit_pipe, pipefds, 1);
        UNUSED(dummy);
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent 
         *
         * 父进程
         * */
        close(safe_to_exit_pipe);
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END 
             *
             * 撤消状态更改。调用方将对处于BGSAVE_START状态的所有从机执行清理，但早
             * 期对replicationSetupSlaveForFullResync（）的调用将其转换为BGSAVE_END
             * */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
                    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
                }
            }
            close(rdb_pipe_write);
            close(server.rdb_pipe_read);
            zfree(server.rdb_pipe_conns);
            server.rdb_pipe_conns = NULL;
            server.rdb_pipe_numconns = 0;
            server.rdb_pipe_numconns_writing = 0;
            closeChildInfoPipe();
        } else {
            serverLog(LL_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            updateDictResizePolicy();
            close(rdb_pipe_write); /* close write in parent so that it can detect the close on the child. 
                                    *
                                    * 关闭写入父项，以便它可以检测子项上的关闭。
                                    * */
            if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
                serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
            }
        }
        return (childpid == -1) ? C_ERR : C_OK;
    }
    return C_OK; /* Unreached. 
                  *
                  * 未读取。
                  * */
}

/*
 * 阻塞式数据保存
 */
void saveCommand(client *c) {
    // 后台保存工作已在进行中，返回
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    // 保存所有数据库数据到指定的文件
    if (rdbSave(server.rdb_filename,rsiptr) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] 
 *
 * BGSAVE[时间表]
 * */
/*
 * 非阻塞式数据保存
 */
void bgsaveCommand(client *c) {
    int schedule = 0;

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite
     * is in progress. Instead of returning an error a BGSAVE gets scheduled. 
     *
     * SCHEDULE选项在AOF重写过程中更改BGSAVE的行为。BGSAVE不会返回错误，而是被安排。
     * */
    if (c->argc > 1) {
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
            schedule = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);

    // 后台保存工作正在进行，返回
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");

        // AOF 重写工作正在进行，返回
    } else if (hasActiveChildProcess()) {
        if (schedule) {
            server.rdb_bgsave_scheduled = 1;
            addReplyStatus(c,"Background saving scheduled");
        } else {
            addReplyError(c,
            "Another child process is active (AOF?): can't BGSAVE right now. "
            "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
            "possible.");
        }

        // 开始后台写入
    } else if (rdbSaveBackground(server.rdb_filename,rsiptr) == C_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}

/* Populate the rdbSaveInfo structure used to persist the replication
 * information inside the RDB file. Currently the structure explicitly
 * contains just the currently selected DB from the master stream, however
 * if the rdbSave*() family functions receive a NULL rsi structure also
 * the Replication ID/offset is not saved. The function popultes 'rsi'
 * that is normally stack-allocated in the caller, returns the populated
 * pointer if the instance has a valid master client, otherwise NULL
 * is returned, and the RDB saving will not persist any replication related
 * information. 
 *
 * 填充rdbSaveInfo结构，该结构用于将复制信息持久化到RDB文件中。目前，
 * 该结构只显式地包含当前从主流中选择的DB，但是，如果rdbSave*（）系列函数
 * 接收到NULL rsi结构，则复制ID/偏移量也不会保存。函数populates “rsi”，
 * 它通常是在调用程序中堆栈分配的，如果实例具有有效的主客户端，则返回填
 * 充的指针，否则返回NULL，并且RDB保存将不会保存任何与复制相关的信息。
 * */
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi) {
    rdbSaveInfo rsi_init = RDB_SAVE_INFO_INIT;
    *rsi = rsi_init;

    /* If the instance is a master, we can populate the replication info
     * only when repl_backlog is not NULL. If the repl_backlog is NULL,
     * it means that the instance isn't in any replication chains. In this
     * scenario the replication info is useless, because when a slave
     * connects to us, the NULL repl_backlog will trigger a full
     * synchronization, at the same time we will use a new replid and clear
     * replid2. 
     *
     * 如果实例是主实例，则只有当repl_backlog不为NULL时，我们才能填充复
     * 制信息。如果repl_backlog为NULL，则表示该实例不在任何复制链中。在
     * 这种情况下，复制信息是无用的，因为当从设备连接到我们时，NULL repl_backlog
     * 将触发完全同步，同时我们将使用新的replid并清除replid2。
     * */
    if (!server.masterhost && server.repl_backlog) {
        /* Note that when server.slaveseldb is -1, it means that this master
         * didn't apply any write commands after a full synchronization.
         * So we can let repl_stream_db be 0, this allows a restarted slave
         * to reload replication ID/offset, it's safe because the next write
         * command must generate a SELECT statement. 
         *
         * 请注意，当server.slavesldb为-1时，这意味着该主机在完全同步后没
         * 有应用任何写命令。因此，我们可以让repl_stream_db为0，这允许重新启
         * 动的从属服务器重新加载复制ID/偏移量，这是安全的，因为下一个写命令必须生成SELECT语句。
         * */
        rsi->repl_stream_db = server.slaveseldb == -1 ? 0 : server.slaveseldb;
        return rsi;
    }

    /* If the instance is a slave we need a connected master
     * in order to fetch the currently selected DB. 
     *
     * 如果实例是从实例，我们需要一个连接的主实例来获取当前选择的DB。
     * */
    if (server.master) {
        rsi->repl_stream_db = server.master->db->id;
        return rsi;
    }

    /* If we have a cached master we can use it in order to populate the
     * replication selected DB info inside the RDB file: the slave can
     * increment the master_repl_offset only from data arriving from the
     * master, so if we are disconnected the offset in the cached master
     * is valid. 
     *
     * 如果我们有一个缓存的master，我们可以使用它来填充RDB文件中复制选择的DB
     * 信息：slave只能根据来自master的数据来增加master_repl_offset，所以如果我们断开连接，缓存master中的偏移量是有效的。
     * */
    if (server.cached_master) {
        rsi->repl_stream_db = server.cached_master->db->id;
        return rsi;
    }
    return NULL;
}
