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

#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"
#include "rio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <lua.h>
#include <signal.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

typedef long long mstime_t; /* millisecond time type. 
                             *
                             * 毫秒时间类型。
                             * */
typedef long long ustime_t; /* microsecond time type. 
                             *
                             * 微秒时间类型。
                             * */

#include "ae.h"      /* Event driven programming library 
                      *
                      * 事件驱动编程库
                      * */
#include "sds.h"     /* Dynamic safe strings 
                      *
                      * 动态安全字符串
                      * */
#include "dict.h"    /* Hash tables 
                      *
                      * 哈希表
                      * */
#include "adlist.h"  /* Linked lists 
                      *
                      * 链接列表
                      * */
#include "zmalloc.h" /* total memory usage aware version of malloc/free 
                      *
                      * malloc/free的总内存使用量感知版本
                      * */
#include "anet.h"    /* Networking the easy way 
                      *
                      * 以简单的方式建立网络
                      * */
#include "ziplist.h" /* Compact list data structure 
                      *
                      * 精简列表数据结构
                      * */
#include "intset.h"  /* Compact integer set structure 
                      *
                      * 紧凑整数集结构
                      * */
#include "version.h" /* Version macro 
                      *
                      * 版本宏
                      * */
#include "util.h"    /* Misc functions useful in many places 
                      *
                      * 在许多地方有用的杂项功能
                      * */
#include "latency.h" /* Latency monitor API 
                      *
                      * 延迟监视器API
                      * */
#include "sparkline.h" /* ASCII graphs API 
                        *
                        * ASCII图形API
                        * */
#include "quicklist.h"  /* Lists are encoded as linked lists of
                           N-elements flat arrays 
                         *
                         * 列表编码为N元素平面阵列的链接列表
                         * */
#include "rax.h"     /* Radix tree 
                      *
                      * 基数树
                      * */
#include "connection.h" /* Connection abstraction 
                         *
                         * 连接抽象
                         * */

#define REDISMODULE_CORE 1
#include "redismodule.h"    /* Redis modules API defines. 
                             *
                             * Redis模块API定义。
                             * */

/* Following includes allow test functions to be called from Redis main() 
 *
 * 以下包含允许从Redis main（）调用测试函数
 * */
#include "zipmap.h"
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"

/* min/max 
 *
 * 最小值/最大值
 * */
#undef min
#undef max
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

/* Error codes 
 *
 * 错误代码
 * */
// 执行状态代码
#define C_OK                    0
#define C_ERR                   -1

/* Static server configuration 
 *
 * 静态服务器配置
 * */
/* 服务器默认静态配置 */
// 每秒钟调用时间中断器的次数
#define CONFIG_DEFAULT_HZ        10             /* Time interrupt calls/sec. 
                                                 *
                                                 * 时间中断调用/秒。
                                                 * */
#define CONFIG_MIN_HZ            1
#define CONFIG_MAX_HZ            500
#define MAX_CLIENTS_PER_CLOCK_TICK 200          /* HZ is adapted based on that. 
                                                 *
                                                 * HZ在此基础上进行了调整。
                                                 * */
#define CONFIG_MAX_LINE    1024
#define CRON_DBS_PER_CALL 16
#define NET_MAX_WRITES_PER_EVENT (1024*64)
#define PROTO_SHARED_SELECT_CMDS 10
#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_BULKHDR_LEN 32
#define LOG_MAX_LEN    1024 /* Default maximum length of syslog messages.
                             *
                             * 系统日志消息的默认最大长度。
                             * */
#define AOF_REWRITE_ITEMS_PER_CMD 64
#define AOF_READ_DIFF_INTERVAL_BYTES (1024*10)
#define CONFIG_AUTHPASS_MAX_LEN 512
#define CONFIG_RUN_ID_SIZE 40
#define RDB_EOF_MARK_SIZE 40
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024*16)          /* 16k
                                                         * */
#define CONFIG_BGSAVE_RETRY_DELAY 5 /* Wait a few secs before trying again. 
                                     *
                                     * 请等待几秒钟，然后重试。
                                     * */
#define CONFIG_DEFAULT_PID_FILE "/var/run/redis.pid"
#define CONFIG_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define CONFIG_DEFAULT_UNIX_SOCKET_PERM 0
#define CONFIG_DEFAULT_LOGFILE ""
#define NET_IP_STR_LEN 46 /* INET6_ADDRSTRLEN is 46, but we need to be sure 
                           *
                           * INET6_ADDRSTRLEN是46，但我们需要确定
                           * */
#define NET_PEER_ID_LEN (NET_IP_STR_LEN+32) /* Must be enough for ip:port 
                                             *
                                             * ip:port必须足够
                                             * */
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32

#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Children process will exit with this status code to signal that the
 * process terminated without an error: this is useful in order to kill
 * a saving child (RDB or AOF one), without triggering in the parent the
 * write protection that is normally turned on on write errors.
 * Usually children that are terminated with SIGUSR1 will exit with this
 * special code. 
 *
 * 子进程将使用此状态代码退出，以表示进程已终止而没有出现错误：这对于杀死正在保存的
 * 子进程（RDB或AOF）非常有用，而不会在父进程中触发通常在写入错误时打开的写入
 * 保护。通常，以SIGUSR1终止的子级将使用此特殊代码退出。
 * */
#define SERVER_CHILD_NOERROR_RETVAL    255

/* Instantaneous metrics tracking. 
 *
 * 即时指标跟踪。
 * */
#define STATS_METRIC_SAMPLES 16     /* Number of samples per metric. 
                                     *
                                     * 每个度量的样本数。
                                     * */
#define STATS_METRIC_COMMAND 0      /* Number of commands executed. 
                                     *
                                     * 执行的命令数。
                                     * */
#define STATS_METRIC_NET_INPUT 1    /* Bytes read to network .
                                     *
                                     * 读取到网络的字节数。
                                     * */
#define STATS_METRIC_NET_OUTPUT 2   /* Bytes written to network. 
                                     *
                                     * 写入网络的字节数。
                                     * */
#define STATS_METRIC_COUNT 3

/* Protocol and I/O related defines 
 *
 * 协议和I/O相关定义
 * */
#define PROTO_MAX_QUERYBUF_LEN  (1024*1024*1024) /* 1GB max query buffer. 
                                                  *
                                                  * 最大1GB查询缓冲区。
                                                  * */
#define PROTO_IOBUF_LEN         (1024*16)  /* Generic I/O buffer size 
                                            *
                                            * 通用I/O缓冲区大小
                                            * */
#define PROTO_REPLY_CHUNK_BYTES (16*1024) /* 16k output buffer 
                                           *
                                           * 16k输出缓冲器
                                           * */
#define PROTO_INLINE_MAX_SIZE   (1024*64) /* Max size of inline reads 
                                           *
                                           * 内联读取的最大大小
                                           * */
#define PROTO_MBULK_BIG_ARG     (1024*32)
#define LONG_STR_SIZE      21          /* Bytes needed for long -> str + '\0' 
                                        *
                                        * long->str+'\0'所需的字节数
                                        * */
#define REDIS_AUTOSYNC_BYTES (1024*1024*32) /* fdatasync every 32MB 
                                             *
                                             * fdatasync每32MB
                                             * */

#define LIMIT_PENDING_QUERYBUF (4*1024*1024) /* 4mb
                                              * */

/* When configuring the server eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * a few more to stay safe. Since RESERVED_FDS defaults to 32, we add 96
 * in order to make sure of not over provisioning more than 128 fds. 
 *
 * 在配置服务器事件循环时，我们对其进行设置，以便我们可以处理的文件描述符总数为se
 * rver.maxclients+RRESERVED_FDS+几个以保持安全。由于
 * RESERVED_FDS默认为32，因此我们添加了96，以确保不会过度配置超过1
 * 28个FD。
 * */
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

/* OOM Score Adjustment classes. 
 *
 * OOM分数调整类。
 * */
#define CONFIG_OOM_MASTER 0
#define CONFIG_OOM_REPLICA 1
#define CONFIG_OOM_BGCHILD 2
#define CONFIG_OOM_COUNT 3

extern int configOOMScoreAdjValuesDefaults[CONFIG_OOM_COUNT];

/* Hash table parameters 
 *
 * 哈希表参数
 * */
#define HASHTABLE_MIN_FILL        10      /* Minimal hash table fill 10% 
                                           *
                                           * 最小哈希表填充10%
                                           * */

/* Command flags. Please check the command table defined in the server.c file
 * for more information about the meaning of every flag. 
 *
 * 命令标志。请查看server.c文件中定义的命令表，了解有关每个标志含义的详细信
 * 息。
 * */
#define CMD_WRITE (1ULL<<0)            /* "write" flag 
                                        *
                                        * “写入”标志
                                        * */
#define CMD_READONLY (1ULL<<1)         /* "read-only" flag 
                                        *
                                        * “只读”标志
                                        * */
#define CMD_DENYOOM (1ULL<<2)          /* "use-memory" flag 
                                        *
                                        * “使用内存”标志
                                        * */
#define CMD_MODULE (1ULL<<3)           /* Command exported by module. 
                                        *
                                        * 由模块导出的命令。
                                        * */
#define CMD_ADMIN (1ULL<<4)            /* "admin" flag 
                                        *
                                        * “admin”标志
                                        * */
#define CMD_PUBSUB (1ULL<<5)           /* "pub-sub" flag 
                                        *
                                        * “pub-sub” 标志
                                        * */
#define CMD_NOSCRIPT (1ULL<<6)         /* "no-script" flag 
                                        *
                                        * “无脚本”标志
                                        * */
#define CMD_RANDOM (1ULL<<7)           /* "random" flag 
                                        *
                                        * “随机”标志
                                        * */
#define CMD_SORT_FOR_SCRIPT (1ULL<<8)  /* "to-sort" flag 
                                        *
                                        * “要排序”标志
                                        * */
#define CMD_LOADING (1ULL<<9)          /* "ok-loading" flag 
                                        *
                                        * “加载正常”标志
                                        * */
#define CMD_STALE (1ULL<<10)           /* "ok-stale" flag 
                                        *
                                        * “ok stale”标志
                                        * */
#define CMD_SKIP_MONITOR (1ULL<<11)    /* "no-monitor" flag 
                                        *
                                        * “无监视器”标志
                                        * */
#define CMD_SKIP_SLOWLOG (1ULL<<12)    /* "no-slowlog" flag 
                                        *
                                        * “no slowlog”标志
                                        * */
#define CMD_ASKING (1ULL<<13)          /* "cluster-asking" flag 
                                        *
                                        * “集群询问”标志
                                        * */
#define CMD_FAST (1ULL<<14)            /* "fast" flag 
                                        *
                                        * “快速”标志
                                        * */
#define CMD_NO_AUTH (1ULL<<15)         /* "no-auth" flag 
                                        *
                                        * “无身份验证”标志
                                        * */

/* Command flags used by the module system. 
 *
 * 模块系统使用的命令标志。
 * */
#define CMD_MODULE_GETKEYS (1ULL<<16)  /* Use the modules getkeys interface. 
                                        *
                                        * 使用模块getkeys接口。
                                        * */
#define CMD_MODULE_NO_CLUSTER (1ULL<<17) /* Deny on Redis Cluster. 
                                          *
                                          * 在Redis群集上拒绝。
                                          * */

/* Command flags that describe ACLs categories. 
 *
 * 描述ACL类别的命令标志。
 * */
#define CMD_CATEGORY_KEYSPACE (1ULL<<18)
#define CMD_CATEGORY_READ (1ULL<<19)
#define CMD_CATEGORY_WRITE (1ULL<<20)
#define CMD_CATEGORY_SET (1ULL<<21)
#define CMD_CATEGORY_SORTEDSET (1ULL<<22)
#define CMD_CATEGORY_LIST (1ULL<<23)
#define CMD_CATEGORY_HASH (1ULL<<24)
#define CMD_CATEGORY_STRING (1ULL<<25)
#define CMD_CATEGORY_BITMAP (1ULL<<26)
#define CMD_CATEGORY_HYPERLOGLOG (1ULL<<27)
#define CMD_CATEGORY_GEO (1ULL<<28)
#define CMD_CATEGORY_STREAM (1ULL<<29)
#define CMD_CATEGORY_PUBSUB (1ULL<<30)
#define CMD_CATEGORY_ADMIN (1ULL<<31)
#define CMD_CATEGORY_FAST (1ULL<<32)
#define CMD_CATEGORY_SLOW (1ULL<<33)
#define CMD_CATEGORY_BLOCKING (1ULL<<34)
#define CMD_CATEGORY_DANGEROUS (1ULL<<35)
#define CMD_CATEGORY_CONNECTION (1ULL<<36)
#define CMD_CATEGORY_TRANSACTION (1ULL<<37)
#define CMD_CATEGORY_SCRIPTING (1ULL<<38)

/* AOF states 
 *
 * AOF状态
 * */
#define AOF_OFF 0             /* AOF is off 
                               *
                               * AOF关闭
                               * */
#define AOF_ON 1              /* AOF is on 
                               *
                               * AOF打开
                               * */
#define AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending 
                               *
                               * AOF等待重写以开始追加
                               * */

/* Client flags 
 *
 * 客户端标志
 * */
#define CLIENT_SLAVE (1<<0)   /* This client is a repliaca 
                               *
                               * 这个客户是一个repliaca
                               * */
#define CLIENT_MASTER (1<<1)  /* This client is a master 
                               *
                               * 这个客户是一个主节点
                               * */
#define CLIENT_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR 
                               *
                               * 此客户端是从节点监视器，请参阅monitor
                               * */
#define CLIENT_MULTI (1<<3)   /* This client is in a MULTI context 
                               *
                               * 此客户端位于MULTI上下文中
                               * */
#define CLIENT_BLOCKED (1<<4) /* The client is waiting in a blocking operation 
                               *
                               * 客户端正在等待阻止操作
                               * */
#define CLIENT_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail. 
                                 *
                                 * 监视的键已修改。EXEC将失败。
                                 * */
#define CLIENT_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply. 
                                         *
                                         * 写完完整回复后关闭。
                                         * */
#define CLIENT_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  server.unblocked_clients 
                                 *
                                 * 此客户端已被取消阻止并存储在服务器中。unblocked_clients
                                 * */
#define CLIENT_LUA (1<<8) /* This is a non connected client used by Lua 
                           *
                           * 这是Lua使用的非连接客户端
                           * */
#define CLIENT_ASKING (1<<9)     /* Client issued the ASKING command 
                                  *
                                  * 客户端发出ASKING命令
                                  * */
#define CLIENT_CLOSE_ASAP (1<<10)/* Close this client ASAP 
                                  *
                                  * 尽快关闭此客户端
                                  * */
#define CLIENT_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket 
                                    *
                                    * 通过Unix域套接字连接的客户端
                                    * */
#define CLIENT_DIRTY_EXEC (1<<12)  /* EXEC will fail for errors while queueing 
                                    *
                                    * 排队时EXEC将因错误而失败
                                    * */
#define CLIENT_MASTER_FORCE_REPLY (1<<13)  /* Queue replies even if is master 
                                            *
                                            * 即使是主答复，也要排队
                                            * */
#define CLIENT_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. 
                                    *
                                    * 强制传播当前命令的AOF。
                                    * */
#define CLIENT_FORCE_REPL (1<<15)  /* Force replication of current cmd. 
                                    *
                                    * 强制复制当前cmd。
                                    * */
#define CLIENT_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC. 
                                    *
                                    * 实例不理解PSYNC。
                                    * */
#define CLIENT_READONLY (1<<17)    /* Cluster client is in read-only state. 
                                    *
                                    * 群集客户端处于只读状态。
                                    * */
#define CLIENT_PUBSUB (1<<18)      /* Client is in Pub/Sub mode. 
                                    *
                                    * 客户端处于发布/子模式。
                                    * */
#define CLIENT_PREVENT_AOF_PROP (1<<19)  /* Don't propagate to AOF. 
                                          *
                                          * 不要传播到AOF。
                                          * */
#define CLIENT_PREVENT_REPL_PROP (1<<20)  /* Don't propagate to slaves. 
                                           *
                                           * 不要传播给从节点。
                                           * */
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP|CLIENT_PREVENT_REPL_PROP)
#define CLIENT_PENDING_WRITE (1<<21) /* Client has output to send but a write
                                        handler is yet not installed. 
                                      *
                                      * 客户端有要发送的输出，但尚未安装写入处理程序。
                                      * */
#define CLIENT_REPLY_OFF (1<<22)   /* Don't send replies to client. 
                                    *
                                    * 不要向客户发送回复。
                                    * */
#define CLIENT_REPLY_SKIP_NEXT (1<<23)  /* Set CLIENT_REPLY_SKIP for next cmd 
                                         *
                                         * 为下一个cmd设置CLIENT_REPLY_SKIP
                                         * */
#define CLIENT_REPLY_SKIP (1<<24)  /* Don't send just this reply. 
                                    *
                                    * 不要只发送此回复。
                                    * */
#define CLIENT_LUA_DEBUG (1<<25)  /* Run EVAL in debug mode. 
                                   *
                                   * 在调试模式下运行EVAL。
                                   * */
#define CLIENT_LUA_DEBUG_SYNC (1<<26)  /* EVAL debugging without fork() 
                                        *
                                        * 无分叉的EVAL调试（）
                                        * */
#define CLIENT_MODULE (1<<27) /* Non connected client used by some module. 
                               *
                               * 某些模块使用的未连接客户端。
                               * */
#define CLIENT_PROTECTED (1<<28) /* Client should not be freed for now. 
                                  *
                                  * 客户端现在不应该被释放。
                                  * */
#define CLIENT_PENDING_READ (1<<29) /* The client has pending reads and was put
                                       in the list of clients we can read
                                       from. 
                                     *
                                     * 客户端具有挂起的读取，并且已被放入我们可以从中读取的客户端列表中。
                                     * */
#define CLIENT_PENDING_COMMAND (1<<30) /* Used in threaded I/O to signal after
                                          we return single threaded that the
                                          client has already pending commands
                                          to be executed. 
                                        *
                                        * 用于线程I/O，在我们返回单线程后发出信号，表明客户端已经有待执行的挂起命令。
                                        * */
#define CLIENT_TRACKING (1ULL<<31) /* Client enabled keys tracking in order to
                                   perform client side caching. 
                                    *
                                    * 启用客户端的键跟踪，以便执行客户端缓存。
                                    * */
#define CLIENT_TRACKING_BROKEN_REDIR (1ULL<<32) /* Target client is invalid. 
                                                 *
                                                 * 目标客户端无效。
                                                 * */
#define CLIENT_TRACKING_BCAST (1ULL<<33) /* Tracking in BCAST mode. 
                                          *
                                          * BCAST模式下的跟踪。
                                          * */
#define CLIENT_TRACKING_OPTIN (1ULL<<34)  /* Tracking in opt-in mode. 
                                           *
                                           * 选择加入模式下的跟踪。
                                           * */
#define CLIENT_TRACKING_OPTOUT (1ULL<<35) /* Tracking in opt-out mode. 
                                           *
                                           * 选择退出模式下的跟踪。
                                           * */
#define CLIENT_TRACKING_CACHING (1ULL<<36) /* CACHING yes/no was given,
                                              depending on optin/optout mode. 
                                            *
                                            * 给出CACHING是/否，具体取决于optin/optout模式。
                                            * */
#define CLIENT_TRACKING_NOLOOP (1ULL<<37) /* Don't send invalidation messages
                                             about writes performed by myself.
                                           *
                                           * 不要发送关于我自己执行的写入的无效消息。
                                           * */
#define CLIENT_IN_TO_TABLE (1ULL<<38) /* This client is in the timeout table. 
                                       *
                                       * 此客户端在超时表中。
                                       * */
#define CLIENT_PROTOCOL_ERROR (1ULL<<39) /* Protocol error chatting with it. 
                                          *
                                          * 与它聊天时出现协议错误。
                                          * */
#define CLIENT_CLOSE_AFTER_COMMAND (1ULL<<40) /* Close after executing commands
                                               * and writing entire reply. 
                                               *
                                               * 执行命令并写入完整回复后关闭。
                                               * */
#define CLIENT_PUSHING (1ULL<<41) /* This client is pushing notifications. 
                                   *
                                   * 此客户端正在推送通知。
                                   * */

/* Client block type (btype field in client structure)
 * if CLIENT_BLOCKED flag is set. 
 *
 * 如果设置了Client_BLOCKED标志，则客户端块类型（客户端结构中的bty
 * pe字段）。
 * */
#define BLOCKED_NONE 0    /* Not blocked, no CLIENT_BLOCKED flag set. 
                           *
                           * 未阻止，未设置CLIENT_blocked标志。
                           * */
#define BLOCKED_LIST 1    /* BLPOP & co. 
                           *
                           * BLPOP Banco。
                           * */
#define BLOCKED_WAIT 2    /* WAIT for synchronous replication. 
                           *
                           * 等待同步复制。
                           * */
#define BLOCKED_MODULE 3  /* Blocked by a loadable module. 
                           *
                           * 被可加载模块阻塞。
                           * */
#define BLOCKED_STREAM 4  /* XREAD. 
                           *
                           * 外部参照。
                           * */
#define BLOCKED_ZSET 5    /* BZPOP et al. 
                           *
                           * BZPOP等人。
                           * */
#define BLOCKED_NUM 6     /* Number of blocked states. 
                           *
                           * 阻止的状态数。
                           * */

/* Client request types 
 *
 * 客户端请求类型
 * */
#define PROTO_REQ_INLINE 1
#define PROTO_REQ_MULTIBULK 2

/* Client classes for client limits, currently used only for
 * the max-client-output-buffer limit implementation. 
 *
 * 客户端限制的客户端类，当前仅用于最大客户端输出缓冲区限制实现。
 * */
#define CLIENT_TYPE_NORMAL 0 /* Normal req-reply clients + MONITORs 
                              *
                              * 正常请求回复客户端+监视器
                              * */
#define CLIENT_TYPE_SLAVE 1  /* Slaves. 
                              *
                              * 从节点。
                              * */
#define CLIENT_TYPE_PUBSUB 2 /* Clients subscribed to PubSub channels. 
                              *
                              * 订阅PubSub频道的客户端。
                              * */
#define CLIENT_TYPE_MASTER 3 /* Master. 
                              *
                              * 主人
                              * */
#define CLIENT_TYPE_COUNT 4  /* Total number of client types. 
                              *
                              * 客户端类型的总数。
                              * */
#define CLIENT_TYPE_OBUF_COUNT 3 /* Number of clients to expose to output
                                    buffer configuration. Just the first
                                    three: normal, slave, pubsub. 
                                  *
                                  * 要向输出缓冲区配置公开的客户端数。只有前三个：普通，从节点，公共。
                                  * */

/* Slave replication state. Used in server.repl_state for slaves to remember
 * what to do next. 
 *
 * 从节点复制状态。在server.repl_state中用于从服务器，以便记住下一步
 * 要做什么。
 * */
#define REPL_STATE_NONE 0 /* No active replication 
                           *
                           * 没有活动复制
                           * */
#define REPL_STATE_CONNECT 1 /* Must connect to master 
                              *
                              * 必须连接到主机
                              * */
#define REPL_STATE_CONNECTING 2 /* Connecting to master 
                                 *
                                 * 正在连接到主机
                                 * */
/* --- Handshake states, must be ordered --- 
 *
 * ---握手状态，必须有序---
 * */
#define REPL_STATE_RECEIVE_PONG 3 /* Wait for PING reply 
                                   *
                                   * 等待PING回复
                                   * */
#define REPL_STATE_SEND_AUTH 4 /* Send AUTH to master 
                                *
                                * 将AUTH发送到master
                                * */
#define REPL_STATE_RECEIVE_AUTH 5 /* Wait for AUTH reply 
                                   *
                                   * 等待AUTH回复
                                   * */
#define REPL_STATE_SEND_PORT 6 /* Send REPLCONF listening-port 
                                *
                                * 发送REPLCONF侦听端口
                                * */
#define REPL_STATE_RECEIVE_PORT 7 /* Wait for REPLCONF reply 
                                   *
                                   * 等待REPLCONF回复
                                   * */
#define REPL_STATE_SEND_IP 8 /* Send REPLCONF ip-address 
                              *
                              * 发送REPLCONF ip地址
                              * */
#define REPL_STATE_RECEIVE_IP 9 /* Wait for REPLCONF reply 
                                   *
                                   * 等待REPLCONF回复
                                   * */
#define REPL_STATE_SEND_CAPA 10 /* Send REPLCONF capa 
                                 *
                                 * Send REPLCONF capa
                                 * */
#define REPL_STATE_RECEIVE_CAPA 11 /* Wait for REPLCONF reply 
                                   *
                                   * 等待REPLCONF回复
                                   * */
#define REPL_STATE_SEND_PSYNC 12 /* Send PSYNC 
                                  *
                                  * 发送PSYNC
                                  * */
#define REPL_STATE_RECEIVE_PSYNC 13 /* Wait for PSYNC reply 
                                     *
                                     * 等待PSYNC回复
                                     * */
/* --- End of handshake states --- 
 *
 * ---握手状态结束---
 * */
#define REPL_STATE_TRANSFER 14 /* Receiving .rdb from master 
                                *
                                * 从master接收.rdb
                                * */
#define REPL_STATE_CONNECTED 15 /* Connected to master 
                                 *
                                 * 已连接到主机
                                 * */

/* State of slaves from the POV of the master. Used in client->replstate.
 * In SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE states instead the server is waiting
 * to start the next background saving in order to send updates to it. 
 *
 * 从节点的状态从主人的POV。用于 client->replstate。在SEND_BULK
 * 和ONLINE状态下，从设备在其输出队列中接收新的更新。在WAIT_BGSAVE
 * 状态下，服务器正在等待开始下一次后台保存，以便向其发送更新。
 * */
#define SLAVE_STATE_WAIT_BGSAVE_START 6 /* We need to produce a new RDB file. 
                                         *
                                         * 我们需要生成一个新的RDB文件。
                                         * */
#define SLAVE_STATE_WAIT_BGSAVE_END 7 /* Waiting RDB file creation to finish. 
                                       *
                                       * 正在等待RDB文件创建完成。
                                       * */
#define SLAVE_STATE_SEND_BULK 8 /* Sending RDB file to slave. 
                                 *
                                 * 正在将RDB文件发送到slave。
                                 * */
#define SLAVE_STATE_ONLINE 9 /* RDB file transmitted, sending just updates. 
                              *
                              * RDB文件已传输，只发送更新。
                              * */

/* Slave capabilities. 
 *
 * 从节点功能。
 * */
#define SLAVE_CAPA_NONE 0
#define SLAVE_CAPA_EOF (1<<0)    /* Can parse the RDB EOF streaming format. 
                                  *
                                  * 可以解析RDB EOF流格式。
                                  * */
#define SLAVE_CAPA_PSYNC2 (1<<1) /* Supports PSYNC2 protocol. 
                                  *
                                  * 支持PSYNC2协议。
                                  * */

/* Synchronous read timeout - slave side 
 *
 * 同步读取超时-从端
 * */
#define CONFIG_REPL_SYNCIO_TIMEOUT 5

/* List related stuff 
 *
 * 列出相关内容
 * */
#define LIST_HEAD 0
#define LIST_TAIL 1
#define ZSET_MIN 0
#define ZSET_MAX 1

/* Sort operations 
 *
 * 排序操作
 * */
#define SORT_OP_GET 0

/* Log levels 
 *
 * 日志级别
 * */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_RAW (1<<10) /* Modifier to log without timestamp 
                        *
                        * 不带时间戳的日志修改器
                        * */

/* Supervision options 
 *
 * 监督选项
 * */
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3

/* Anti-warning macro... 
 *
 * 反警告宏。。。
 * */
#define UNUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^64 elements 
                               *
                               * 应该足以容纳2^64个元素
                               * */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 
                               *
                               * Skiplist P=1/4
                               * */

/* Append only defines 
 *
 * 仅追加定义
 * */
#define AOF_FSYNC_NO 0
#define AOF_FSYNC_ALWAYS 1
#define AOF_FSYNC_EVERYSEC 2

/* Replication diskless load defines 
 *
 * 复制无盘负载定义
 * */
#define REPL_DISKLESS_LOAD_DISABLED 0
#define REPL_DISKLESS_LOAD_WHEN_DB_EMPTY 1
#define REPL_DISKLESS_LOAD_SWAPDB 2

/* TLS Client Authentication 
 *
 * TLS客户端身份验证
 * */
#define TLS_CLIENT_AUTH_NO 0
#define TLS_CLIENT_AUTH_YES 1
#define TLS_CLIENT_AUTH_OPTIONAL 2

/* Sets operations codes 
 *
 * 设置操作代码
 * */
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/* oom-score-adj defines 
 *
 * oom分数adj定义
 * */
#define OOM_SCORE_ADJ_NO 0
#define OOM_SCORE_RELATIVE 1
#define OOM_SCORE_ADJ_ABSOLUTE 2

/* Redis maxmemory strategies. Instead of using just incremental number
 * for this defines, we use a set of flags so that testing for certain
 * properties common to multiple policies is faster. 
 *
 * Redis最大内存策略。我们使用了一组标志，而不是仅使用增量来进行定义，这样可以
 * 更快地测试多个策略通用的某些属性。
 * */
#define MAXMEMORY_FLAG_LRU (1<<0)
#define MAXMEMORY_FLAG_LFU (1<<1)
#define MAXMEMORY_FLAG_ALLKEYS (1<<2)
#define MAXMEMORY_FLAG_NO_SHARED_INTEGERS \
    (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU)

#define MAXMEMORY_VOLATILE_LRU ((0<<8)|MAXMEMORY_FLAG_LRU)
#define MAXMEMORY_VOLATILE_LFU ((1<<8)|MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_TTL (2<<8)
#define MAXMEMORY_VOLATILE_RANDOM (3<<8)
#define MAXMEMORY_ALLKEYS_LRU ((4<<8)|MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_LFU ((5<<8)|MAXMEMORY_FLAG_LFU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_RANDOM ((6<<8)|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_NO_EVICTION (7<<8)

/* Units 
 *
 * 单位
 * */
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

/* SHUTDOWN flags 
 *
 * SHUTDOWN标志
 * */
#define SHUTDOWN_NOFLAGS 0      /* No flags. 
                                 *
                                 * 没有标志。
                                 * */
#define SHUTDOWN_SAVE 1         /* Force SAVE on SHUTDOWN even if no save
                                   points are configured. 
                                 *
                                 * 即使没有配置保存点，也要在SHUTDOWN上强制保存。
                                 * */
#define SHUTDOWN_NOSAVE 2       /* Don't SAVE on SHUTDOWN. 
                                 *
                                 * 关机时不要保存。
                                 * */

/* Command call flags, see call() function 
 *
 * 命令调用标志，请参阅call（）函数
 * */
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1<<0)
#define CMD_CALL_STATS (1<<1)
#define CMD_CALL_PROPAGATE_AOF (1<<2)
#define CMD_CALL_PROPAGATE_REPL (1<<3)
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF|CMD_CALL_PROPAGATE_REPL)
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE)
#define CMD_CALL_NOWRAP (1<<4)  /* Don't wrap also propagate array into
                                   MULTI/EXEC: the caller will handle it.  
                                 *
                                 * 不要包装也要将数组传播到MULTI/EXEC中：调用者将处理它。
                                 * */

/* Command propagation flags, see propagate() function 
 *
 * 命令传播标志，请参阅propagate（）函数
 * */
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

/* RDB active child save type. 
 *
 * RDB活动子存储类型。
 * */
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1     /* RDB is written to disk. 
                                   *
                                   * RDB被写入磁盘。
                                   * */
#define RDB_CHILD_TYPE_SOCKET 2   /* RDB is written to slave socket. 
                                   *
                                   * RDB被写入从套接字。
                                   * */

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes. 
 *
 * 键空间更改通知类。出于配置目的，每个类都与一个字符相关联。
 * */
#define NOTIFY_KEYSPACE (1<<0)    /* K
                                   * */
#define NOTIFY_KEYEVENT (1<<1)    /* E
                                   * */
#define NOTIFY_GENERIC (1<<2)     /* g
                                   * */
#define NOTIFY_STRING (1<<3)      /* $
                                   * */
#define NOTIFY_LIST (1<<4)        /* l
                                   * */
#define NOTIFY_SET (1<<5)         /* s
                                   * */
#define NOTIFY_HASH (1<<6)        /* h
                                   * */
#define NOTIFY_ZSET (1<<7)        /* z
                                   * */
#define NOTIFY_EXPIRED (1<<8)     /* x
                                   * */
#define NOTIFY_EVICTED (1<<9)     /* e
                                   * */
#define NOTIFY_STREAM (1<<10)     /* t
                                   * */
#define NOTIFY_KEY_MISS (1<<11)   /* m (Note: This one is excluded from NOTIFY_ALL on purpose) 
                                   *
                                   * m（注意：此项是故意从NOTIFY_ALL中排除的）
                                   * */
#define NOTIFY_LOADED (1<<12)     /* module only key space notification, indicate a key loaded from rdb 
                                   *
                                   * 仅模块键空间通知，指示从rdb加载的键
                                   * */
#define NOTIFY_ALL (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM) /* A flag 
                                                                                                                                                              *
                                                                                                                                                              * 一面旗帜
                                                                                                                                                              * */

/* Get the first bind addr or NULL 
 *
 * 获取第一个绑定地址或NULL
 * */
#define NET_FIRST_BIND_ADDR (server.bindaddr_count ? server.bindaddr[0] : NULL)

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. 
 *
 * 使用以下宏，您可以在serverCron（）中运行指定周期（以毫秒为单位）的代码
 * 。实际分辨率取决于server.hz。
 * */
#define run_with_period(_ms_) if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))

/* We can print the stacktrace, so our assert is defined this way: 
 *
 * 我们可以打印stacktrace，因此我们的断言是这样定义的：
 * */
#define serverAssertWithInfo(_c,_o,_e) ((_e)?(void)0 : (_serverAssertWithInfo(_c,_o,#_e,__FILE__,__LINE__),_exit(1)))
#define serverAssert(_e) ((_e)?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define serverPanic(...) _serverPanic(__FILE__,__LINE__,__VA_ARGS__),_exit(1)

/*-----------------------------------------------------------------------------
 * Data types
 * 数据类型
 *----------------------------------------------------------------------------*/

/* A redis object, that is a type able to hold a string / list / set 
 *
 * redis对象，它是一种能够容纳字符串/列表/集合的类型
 * */

/* The actual Redis Object 
 *
 * 实际的Redis对象
 * */
#define OBJ_STRING 0    /* String object. 
                         *
                         * 字符串对象。
                         * */
#define OBJ_LIST 1      /* List object. 
                         *
                         * 列表对象。
                         * */
#define OBJ_SET 2       /* Set object. 
                         *
                         * 集合对象。
                         * */
#define OBJ_ZSET 3      /* Sorted set object. 
                         *
                         * 已排序的集合对象。
                         * */
#define OBJ_HASH 4      /* Hash object. 
                         *
                         * 哈希对象。
                         * */

/* The "module" object type is a special one that signals that the object
 * is one directly managed by a Redis module. In this case the value points
 * to a moduleValue struct, which contains the object value (which is only
 * handled by the module itself) and the RedisModuleType struct which lists
 * function pointers in order to serialize, deserialize, AOF-rewrite and
 * free the object.
 *
 * Inside the RDB file, module types are encoded as OBJ_MODULE followed
 * by a 64 bit module type ID, which has a 54 bits module-specific signature
 * in order to dispatch the loading to the right module, plus a 10 bits
 * encoding version. 
 *
 * “module”对象类型是一种特殊的对象类型，表示该对象是由Redis模块直接管
 * 理的对象。在这种情况下，值指向一个moduleValue结构，该结构包含对象值（仅由模块本身处理）
 * 和RedisModuleType结构，RedisModuteType结构列出函数指针，以便序列化、反序列化、AOF重写和释放对象。
 * 在RDB文件中，模块类型被编码为OBJ_module，后面跟着一个64位的模块类型ID，该ID具有54位的模块特定签名，
 * 以便将加载分配到正确的模块，再加上一个10位的编码版
 * 本。
 * */
#define OBJ_MODULE 5    /* Module object. 
                         *
                         * 模块对象。
                         * */
#define OBJ_STREAM 6    /* Stream object. 
                         *
                         * 流对象。
                         * */

/* Extract encver / signature from a module type ID. 
 *
 * 从模块类型ID中提取加密/签名。
 * */
#define REDISMODULE_TYPE_ENCVER_BITS 10
#define REDISMODULE_TYPE_ENCVER_MASK ((1<<REDISMODULE_TYPE_ENCVER_BITS)-1)
#define REDISMODULE_TYPE_ENCVER(id) (id & REDISMODULE_TYPE_ENCVER_MASK)
#define REDISMODULE_TYPE_SIGN(id) ((id & ~((uint64_t)REDISMODULE_TYPE_ENCVER_MASK)) >>REDISMODULE_TYPE_ENCVER_BITS)

/* Bit flags for moduleTypeAuxSaveFunc 
 *
 * moduleTypeAuxSaveFunc的位标志
 * */
#define REDISMODULE_AUX_BEFORE_RDB (1<<0)
#define REDISMODULE_AUX_AFTER_RDB (1<<1)

struct RedisModule;
struct RedisModuleIO;
struct RedisModuleDigest;
struct RedisModuleCtx;
struct redisObject;

/* Each module type implementation should export a set of methods in order
 * to serialize and deserialize the value in the RDB file, rewrite the AOF
 * log, create the digest for "DEBUG DIGEST", and free the value when a key
 * is deleted. 
 *
 * 每个模块类型的实现都应该导出一组方法，以便序列化和反序列化RDB文件中的值，重写
 * AOF日志，为“DEBUG digest”创建摘要，并在删除键时释放值。
 * */
typedef void *(*moduleTypeLoadFunc)(struct RedisModuleIO *io, int encver);
typedef void (*moduleTypeSaveFunc)(struct RedisModuleIO *io, void *value);
typedef int (*moduleTypeAuxLoadFunc)(struct RedisModuleIO *rdb, int encver, int when);
typedef void (*moduleTypeAuxSaveFunc)(struct RedisModuleIO *rdb, int when);
typedef void (*moduleTypeRewriteFunc)(struct RedisModuleIO *io, struct redisObject *key, void *value);
typedef void (*moduleTypeDigestFunc)(struct RedisModuleDigest *digest, void *value);
typedef size_t (*moduleTypeMemUsageFunc)(const void *value);
typedef void (*moduleTypeFreeFunc)(void *value);

/* A callback that is called when the client authentication changes. This
 * needs to be exposed since you can't cast a function pointer to (void *) 
 *
 * 客户端身份验证更改时调用的回调。这需要公开，因为您不能将函数指针强制转换为（void*）
 * */
typedef void (*RedisModuleUserChangedFunc) (uint64_t client_id, void *privdata);


/* The module type, which is referenced in each value of a given type, defines
 * the methods and links to the module exporting the type. 
 *
 * 在给定类型的每个值中引用的模块类型定义了导出该类型的模块的方法和链接。
 * */
typedef struct RedisModuleType {
    uint64_t id; /* Higher 54 bits of type ID + 10 lower bits of encoding ver. 
                  *
                  * ID类型的较高54位+编码ver的较低10位。
                  * */
    struct RedisModule *module;
    moduleTypeLoadFunc rdb_load;
    moduleTypeSaveFunc rdb_save;
    moduleTypeRewriteFunc aof_rewrite;
    moduleTypeMemUsageFunc mem_usage;
    moduleTypeDigestFunc digest;
    moduleTypeFreeFunc free;
    moduleTypeAuxLoadFunc aux_load;
    moduleTypeAuxSaveFunc aux_save;
    int aux_save_triggers;
    char name[10]; /* 9 bytes name + null term. Charset: A-Z a-z 0-9 _- 
                    *
                    * 9字节名称+空项。字符集：A-Z A-Z 0-9_-
                    * */
} moduleType;

/* In Redis objects 'robj' structures of type OBJ_MODULE, the value pointer
 * is set to the following structure, referencing the moduleType structure
 * in order to work with the value, and at the same time providing a raw
 * pointer to the value, as created by the module commands operating with
 * the module type.
 *
 * So for example in order to free such a value, it is possible to use
 * the following code:
 *
 *  if (robj->type == OBJ_MODULE) {
 *      moduleValue *mt = robj->ptr;
 *      mt->type->free(mt->value);
 *      zfree(mt); // We need to release this in-the-middle struct as well.
 *  }
 
 *
 * 在OBJ_MODULE类型的Redis对象“robj”结构中，值指针设置为以下结
 * 构，引用moduleType结构来处理该值，同时提供一个指向该值的原始指针，该指
 * 针由使用该模块类型的模块命令创建。
 *
 * 例如，为了释放这样的值，可以使用以下代码：
 *
 * if（robj->type=OBJ_MODULE）{
 *      moduleValue*mt=robj->ptr；
 *      mt->type->free（mt->value）；
 *      zfree（mt）；//我们也需要在中间结构中释放它。
 *  }
 * */
typedef struct moduleValue {
    moduleType *type;
    void *value;
} moduleValue;

/* This is a wrapper for the 'rio' streams used inside rdb.c in Redis, so that
 * the user does not have to take the total count of the written bytes nor
 * to care about error conditions. 
 *
 * 这是Redis中rdb.c中使用的“rio”流的包装器，这样用户就不必计算写入的
 * 字节总数，也不必关心错误情况。
 * */
typedef struct RedisModuleIO {
    size_t bytes;       /* Bytes read / written so far. 
                         *
                         * 到目前为止读取/写入的字节数。
                         * */
    rio *rio;           /* Rio stream.
                         * */
    moduleType *type;   /* Module type doing the operation. 
                         *
                         * 执行操作的模块类型。
                         * */
    int error;          /* True if error condition happened. 
                         *
                         * 如果出现错误条件，则为True。
                         * */
    int ver;            /* Module serialization version: 1 (old),
                         * 2 (current version with opcodes annotation). 
                         *
                         * 模块序列化版本：1（旧版本），2（带操作码注释的当前版本）。
                         * */
    struct RedisModuleCtx *ctx; /* Optional context, see RM_GetContextFromIO()
                                 *
                                 * 可选上下文，请参阅RM_GetContextFromIO（）
                                 * */
    struct redisObject *key;    /* Optional name of key processed 
                                 *
                                 * 已处理键的可选名称
                                 * */
} RedisModuleIO;

/* Macro to initialize an IO context. Note that the 'ver' field is populated
 * inside rdb.c according to the version of the value to load. 
 *
 * 用于初始化IO上下文的宏。请注意，“ver”字段是根据要加载的值的版本在rdb.c中填充的。
 * */
#define moduleInitIOContext(iovar,mtype,rioptr,keyptr) do { \
    iovar.rio = rioptr; \
    iovar.type = mtype; \
    iovar.bytes = 0; \
    iovar.error = 0; \
    iovar.ver = 0; \
    iovar.key = keyptr; \
    iovar.ctx = NULL; \
} while(0);

/* This is a structure used to export DEBUG DIGEST capabilities to Redis
 * modules. We want to capture both the ordered and unordered elements of
 * a data structure, so that a digest can be created in a way that correctly
 * reflects the values. See the DEBUG DIGEST command implementation for more
 * background. 
 *
 * 这是一个用于将DEBUG DIGEST功能导出到Redis模块的结构。我们希望捕
 * 获数据结构的有序元素和无序元素，以便以正确反映值的方式创建摘要。有关更多背景信息，请参阅DEBUG DIGEST命令实现。
 * */
typedef struct RedisModuleDigest {
    unsigned char o[20];    /* Ordered elements. 
                             *
                             * 有序元素。
                             * */
    unsigned char x[20];    /* Xored elements. 
                             *
                             * Xored元素。
                             * */
} RedisModuleDigest;

/* Just start with a digest composed of all zero bytes. 
 *
 * 只需从一个由所有零字节组成的摘要开始。
 * */
#define moduleInitDigestContext(mdvar) do { \
    memset(mdvar.o,0,sizeof(mdvar.o)); \
    memset(mdvar.x,0,sizeof(mdvar.x)); \
} while(0);

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. 
 *
 * 对象编码。某些类型的对象（如Strings和Hashes）可以通过多种方式在内部
 * 表示。对象的“encoding”字段设置为此对象的其中一个字段。
 * */
#define OBJ_ENCODING_RAW 0     /* Raw representation 
                                *
                                * 原始表示
                                * */
#define OBJ_ENCODING_INT 1     /* Encoded as integer 
                                *
                                * 编码为整数
                                * */
#define OBJ_ENCODING_HT 2      /* Encoded as hash table 
                                *
                                * 编码为哈希表
                                * */
#define OBJ_ENCODING_ZIPMAP 3  /* Encoded as zipmap 
                                *
                                * 编码为zipmap
                                * */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. 
                                   *
                                   * 不再使用：旧列表编码。
                                   * */
#define OBJ_ENCODING_ZIPLIST 5 /* Encoded as ziplist 
                                *
                                * 编码为压缩列表
                                * */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset 
                                *
                                * 编码为intset
                                * */
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist 
                                  *
                                  * 编码为列表
                                  * */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding 
                                *
                                * 嵌入式sds字符串编码
                                * */
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of ziplists 
                                  *
                                  * 编码为压缩列表的链接列表
                                  * */
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks 
                                *
                                * 编码为列表包的基数树
                                * */

#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* Max value of obj->lru 
                                         *
                                         * obj->lru的最大值
                                         * */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms 
                                   *
                                   * LRU时钟分辨率（ms）
                                   * */

#define OBJ_SHARED_REFCOUNT INT_MAX     /* Global object never destroyed. 
                                         *
                                         * 全局对象从未被销毁。
                                         * */
#define OBJ_STATIC_REFCOUNT (INT_MAX-1) /* Object allocated in the stack. 
                                         *
                                         * 在堆栈中分配的对象。
                                         * */
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT
/*
 * Redis 对象
 */
typedef struct redisObject {
    // 类型
    unsigned type:4;
    // 编码方式
    unsigned encoding:4;
    // LRU 时间（相对于 server.lruclock）
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). 
                            *
                            * LRU时间（相对于全局LRU_clock）或LFU数据（最低有效8位频率和最高有
                            * 效16位访问时间）。
                            * */
    // 引用计数
    int refcount;
    // 指向对象的值
    void *ptr;
} robj;

/* The a string name for an object's type as listed above
 * Native types are checked against the OBJ_STRING, OBJ_LIST, OBJ_* defines,
 * and Module types have their registered name returned. 
 *
 * 上面列出的对象类型的字符串名称。根据OBJ_string、OBJ_LIST、OB
 * J_defines检查本机类型，并返回模块类型的注册名称。
 * */
char *getObjectTypeName(robj*);

/* Macro used to initialize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. 
 *
 * 用于初始化堆栈上分配的Redis对象的宏。请注意，这个宏是在结构定义附近获取的，
 * 以确保我们在结构更改时会更新它，从而避免像bug#85这样的错误以这种方式引入。
 * */
/*
 * 初始化一个从栈上分配的 Redis Object
 */
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = OBJ_STATIC_REFCOUNT; \
    _var.type = OBJ_STRING; \
    _var.encoding = OBJ_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0)

struct evictionPoolEntry; /* Defined in evict.c 
                           *
                           * 在逐出.c中定义
                           * */

/* This structure is used in order to represent the output buffer of a client,
 * which is actually a linked list of blocks like that, that is: client->reply. 
 *
 * 这个结构用于表示客户端的输出缓冲区，它实际上是一个类似块的链表，即：client
 * ->reply。
 * */
typedef struct clientReplyBlock {
    size_t size, used;
    char buf[];
} clientReplyBlock;

/* Redis database representation. There are multiple databases identified
 * by integers from 0 (the default database) up to the max configured
 * database. The database number is the 'id' field in the structure. 
 *
 * Redis数据库表示。有多个数据库由从0（默认数据库）到最大配置数据库的整数标识
 * 。数据库编号是结构中的“id”字段。
 * */
/*
 * 数据库结构
 */
typedef struct redisDb {
    // key space，包括键值对象
    dict *dict;                 /* The keyspace for this DB 
                                 *
                                 * 此数据库的键空间
                                 * */
    // 保存 key 的过期时间
    dict *expires;              /* Timeout of keys with a timeout set 
                                 *
                                 * 设置了超时的键超时
                                 * */
    // 正因为某个/某些 key 而被阻塞的客户端
    dict *blocking_keys;        /* Keys with clients waiting for data (BLPOP)
                                 *
                                 * 客户端正在等待数据的键（BLPOP）
                                 * */
    // 某个/某些接收到 PUSH 命令的阻塞 key
    dict *ready_keys;           /* Blocked keys that received a PUSH 
                                 *
                                 * 收到PUSH的被阻止键
                                 * */
    // 正在监视某个/某些 key 的所有客户端
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS 
                                 *
                                 * MULTI/EXEC CAS的WATCHED键
                                 * */
    // 数据库的号码
    int id;                     /* Database ID 
                                 *
                                 * 数据库ID
                                 * */
    long long avg_ttl;          /* Average TTL, just for stats 
                                 *
                                 * 平均TTL，仅用于统计
                                 * */
    unsigned long expires_cursor; /* Cursor of the active expire cycle. 
                                   *
                                   * 活动过期周期的光标。
                                   * */
    list *defrag_later;         /* List of key names to attempt to defrag one by one, gradually. 
                                 *
                                 * 要逐个尝试逐步进行碎片整理的键名称列表。
                                 * */
} redisDb;

/* Declare database backup that include redis main DBs and slots to keys map.
 * Definition is in db.c. We can't define it here since we define CLUSTER_SLOTS
 * in cluster.h. 
 *
 * 声明数据库备份，其中包括redis主数据库和slot-to-keys映射。定义在
 * db.c中。我们不能在这里定义它，因为我们在CLUSTER.h中定义了CLUST
 * ER_SLOTS。
 * */
typedef struct dbBackup dbBackup;

/* Client MULTI/EXEC state 
 *
 * 客户端MULTI/EXEC状态
 * */
/*
 * 事务命令结构
 */
typedef struct multiCmd {
    // 执行命令的所有 key 对象
    robj **argv;
    // 参数的数量
    int argc;
    // 被执行的命令
    struct redisCommand *cmd;
} multiCmd;

/*
 * 事务状态结构
 */
typedef struct multiState {
    // 数组，保存着所有在事务队列中的命令
    multiCmd *commands;     /* Array of MULTI commands 
                             *
                             * MULTI命令阵列
                             * */
    // 队列中命令的数量
    int count;              /* Total number of MULTI commands 
                             *
                             * MULTI命令总数
                             * */
    int cmd_flags;          /* The accumulated command flags OR-ed together.
                               So if at least a command has a given flag, it
                               will be set in this field. 
                             *
                             * 累加的命令标志“或”在一起。因此，如果至少一个命令有一个给定的标志，它将在该字段
                             * 中设置。
                             * */
    int cmd_inv_flags;      /* Same as cmd_flags, OR-ing the ~flags. so that it
                               is possible to know if all the commands have a
                               certain flag. 
                             *
                             * 与cmd_flags相同，对~标志进行OR运算。从而可以知道是否所有的命令都具有
                             * 特定的标志。
                             * */
    int minreplicas;        /* MINREPLICAS for synchronous replication 
                             *
                             * 用于同步复制的MINREPLICAS
                             * */
    time_t minreplicas_timeout; /* MINREPLICAS timeout as unixtime. 
                                 *
                                 * PLACES立即超时。
                                 * */
} multiState;

/* This structure holds the blocking operation state for a client.
 * The fields used depend on client->btype. 
 *
 * 此结构保存客户端的阻塞操作状态。使用的字段取决于客户端->btype。
 * */
/*
 * 记录客户端的阻塞状态
 */
typedef struct blockingState {
    /* Generic fields. 
     *
     * 通用字段。
     * */
    // 超时时间
    // 如果 UNIX 的当前时间大于等于这个值的话，
    // 那么取消对客户端的阻塞
    mstime_t timeout;       /* Blocking operation timeout. If UNIX current time
                             * is > timeout then the operation timed out. 
                             *
                             * 阻塞操作超时。如果UNIX当前时间>timeout，则操作超时。
                             * */

    /* BLOCKED_LIST, BLOCKED_ZSET and BLOCKED_STREAM 
     *
     * BLOCKED_LIST、BLOCKED_ZSET和BLOCKED_STREAM
     * */
    // 阻塞客户端的任意多个 key
    dict *keys;             /* The keys we are waiting to terminate a blocking
                             * operation such as BLPOP or XREAD. Or NULL. 
                             *
                             * 我们正在等待终止诸如BLPOP或XREAD之类的阻塞操作的键。或NULL。
                             * */
    // 在阻塞被取消时接受元素的 key
    // 只用于 BRPOPLPUSH 命令
    robj *target;           /* The key that should receive the element,
                             * for BRPOPLPUSH. 
                             *
                             * 应该接收元素的键，用于BRPOPLPUSH。
                             * */

    /* BLOCK_STREAM 
     *
     * 块流
     * */
    size_t xread_count;     /* XREAD COUNT option. 
                             *
                             * XREAD COUNT选项。
                             * */
    robj *xread_group;      /* XREADGROUP group name. 
                             *
                             * XREADGROUP组名称。
                             * */
    robj *xread_consumer;   /* XREADGROUP consumer name. 
                             *
                             * XREADGROUP使用者名称。
                             * */
    mstime_t xread_retry_time, xread_retry_ttl;
    int xread_group_noack;

    /* BLOCKED_WAIT 
     *
     * 阻止等待
     * */
    int numreplicas;        /* Number of replicas we are waiting for ACK. 
                             *
                             * 我们正在等待确认的副本数。
                             * */
    long long reploffset;   /* Replication offset to reach. 
                             *
                             * 要达到的复制偏移量。
                             * */

    /* BLOCKED_MODULE 
     *
     * BLOCKED_MODULE
     * */
    void *module_blocked_handle; /* RedisModuleBlockedClient structure.
                                    which is opaque for the Redis core, only
                                    handled in module.c. 
                                  *
                                  * RedisModuleBlockedClient结构。这对于Redis核心来说是
                                  * 不透明的，只在模块.c中处理。
                                  * */
} blockingState;

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we run this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. 
 *
 * 以下结构表示server.ready_keys列表中的一个节点，在该列表中，我们
 * 累积了所有客户端被阻止操作（如B[LR]POP）但在上次执行的命令上下文中接收到
 * 新数据的键。在执行完每个命令或脚本后，我们运行此列表以检查是否因此应该将数据提
 * 供给被阻止的客户端，从而取消阻止它们。请注意，server.ready_keys
 * 不会有重复项，因为在代表Redis数据库的每个结构中都有一个也称为ready_keys的字典，
 * 我们确保记住给定的键是否已经添加到server.ready_keys列表中。
 * */
/*
 * 记录了已就绪 key 和它对应的 db
 */
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* This structure represents a Redis user. This is useful for ACLs, the
 * user is associated to the connection after the connection is authenticated.
 * If there is no associated user, the connection uses the default user. 
 *
 * 此结构表示Redis用户。这对于ACL很有用，因为用户在连接经过身份验证后与连接
 * 关联。如果没有关联的用户，则连接使用默认用户。
 * */
#define USER_COMMAND_BITS_COUNT 1024    /* The total number of command bits
                                           in the user structure. The last valid
                                           command ID we can set in the user
                                           is USER_COMMAND_BITS_COUNT-1. 
                                         *
                                         * 用户结构中的命令位总数。我们可以在用户中设置的最后一个有效命令ID是user_c
                                         * ommand_BITS_COUNT-1。
                                         * */
#define USER_FLAG_ENABLED (1<<0)        /* The user is active. 
                                         *
                                         * 用户处于活动状态。
                                         * */
#define USER_FLAG_DISABLED (1<<1)       /* The user is disabled. 
                                         *
                                         * 用户已被禁用。
                                         * */
#define USER_FLAG_ALLKEYS (1<<2)        /* The user can mention any key. 
                                         *
                                         * 用户可以提及任何键。
                                         * */
#define USER_FLAG_ALLCOMMANDS (1<<3)    /* The user can run all commands. 
                                         *
                                         * 用户可以运行所有命令。
                                         * */
#define USER_FLAG_NOPASS      (1<<4)    /* The user requires no password, any
                                           provided password will work. For the
                                           default user, this also means that
                                           no AUTH is needed, and every
                                           connection is immediately
                                           authenticated. 
                                         *
                                         * 用户不需要密码，任何提供的密码都可以使用。对于默认用户，这也意味着不需要AUTH,
                                         * 并且每个连接都会立即进行身份验证。
                                         * */
typedef struct {
    sds name;       /* The username as an SDS string. 
                     *
                     * SDS字符串形式的用户名。
                     * */
    uint64_t flags; /* See USER_FLAG_* 
                     *
                     * 请参阅USER_FLAG_
                     * */

    /* The bit in allowed_commands is set if this user has the right to
     * execute this command. In commands having subcommands, if this bit is
     * set, then all the subcommands are also available.
     *
     * If the bit for a given command is NOT set and the command has
     * subcommands, Redis will also check allowed_subcommands in order to
     * understand if the command can be executed. 
     *
     * 如果该用户有权执行此命令，则会设置allowed_commands中的位。在具有
     * 子命令的命令中，如果设置了此位，则所有子命令也可用。如果给定命令的位未设置，并且
     * 该命令具有子命令，Redis还会检查allowed_subcommands，以了
     * 解该命令是否可以执行。
     * */
    uint64_t allowed_commands[USER_COMMAND_BITS_COUNT/64];

    /* This array points, for each command ID (corresponding to the command
     * bit set in allowed_commands), to an array of SDS strings, terminated by
     * a NULL pointer, with all the sub commands that can be executed for
     * this command. When no subcommands matching is used, the field is just
     * set to NULL to avoid allocating USER_COMMAND_BITS_COUNT pointers. 
     *
     * 对于每个命令ID（对应于allowed_commands中设置的命令位），该数组
     * 指向一个SDS字符串数组，该数组以NULL指针结束，其中包含可为此命令执行的所有
     * 子命令。如果不使用匹配的子命令，则只将字段设置为NULL，以避免分配USER_C
     * OMMAND_BITS_COUNT指针。
     * */
    sds **allowed_subcommands;
    list *passwords; /* A list of SDS valid passwords for this user. 
                      *
                      * 此用户的SDS有效密码列表。
                      * */
    list *patterns;  /* A list of allowed key patterns. If this field is NULL
                        the user cannot mention any key in a command, unless
                        the flag ALLKEYS is set in the user. 
                      *
                      * 允许的键模式列表。如果此字段为NULL，则用户不能在命令中提及任何键，除非在用
                      * 户中设置了ALLKEYS标志。
                      * */
} user;

/* With multiplexing we need to take per-client state.
 * Clients are taken in a linked list. 
 *
 * 对于多路复用，我们需要采用每个客户端的状态。客户端位于链接列表中。
 * */

#define CLIENT_ID_AOF (UINT64_MAX) /* Reserved ID for the AOF client. If you
                                      need more reserved IDs use UINT64_MAX-1,
                                      -2, ... and so forth. 
                                    *
                                    * AOF客户端的保留ID。如果您需要更多的保留ID，请使用UINT64_MAX-1
                                    * 、-2。。。等等
                                    * */

/*
 * 客户端结构
 *
 * 为每个连接到服务器的客户端保存维持一个该结构的映射，
 * 从而实现多路复用。
 */
typedef struct client {
    uint64_t id;            /* Client incremental unique ID. 
                             *
                             * 客户端增量唯一ID。
                             * */
    connection *conn;
    int resp;               /* RESP protocol version. Can be 2 or 3. 
                             *
                             * RESP协议版本。可以是2或3。
                             * */
    // 指向当前目标数据库的指针
    redisDb *db;            /* Pointer to currently SELECTed DB. 
                             *
                             * 指向当前选定数据库的指针。
                             * */
    robj *name;             /* As set by CLIENT SETNAME. 
                             *
                             * 由CLIENT SETNAME设置。
                             * */
    // 查询缓存
    sds querybuf;           /* Buffer we use to accumulate client queries. 
                             *
                             * 我们用来累积客户端查询的缓冲区。
                             * */
    size_t qb_pos;          /* The position we have read in querybuf. 
                             *
                             * 我们在querybuf中读取的位置。
                             * */
    sds pending_querybuf;   /* If this client is flagged as master, this buffer
                               represents the yet not applied portion of the
                               replication stream that we are receiving from
                               the master. 
                             *
                             * 如果此客户端被标记为master，则此缓冲区表示我们从master接收的复制流中
                             * 尚未应用的部分。
                             * */
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size. 
                             *
                             * querybuf大小的最近峰值（100ms或更长）。
                             * */
    // 参数的个数
    int argc;               /* Num of arguments of current command. 
                             *
                             * 当前命令的参数数。
                             * */
    // 字符串表示的命令，以及命令的参数
    robj **argv;            /* Arguments of current command. 
                             *
                             * 当前命令的参数。
                             * */
    size_t argv_len_sum;    /* Sum of lengths of objects in argv list. 
                             *
                             * argv列表中对象的长度总和。
                             * */
    // 命令，以及上个命令
    struct redisCommand *cmd, *lastcmd;  /* Last command executed. 
                                          *
                                          * 最后执行的命令。
                                          * */
    user *user;             /* User associated with this connection. If the
                               user is set to NULL the connection can do
                               anything (admin). 
                             *
                             * 与此连接关联的用户。如果用户设置为NULL，则连接可以执行任何操作（admin）
                             * 。
                             * */
    // 回复类型
    int reqtype;            /* Request protocol type: PROTO_REQ_* 
                             *
                             * 请求协议类型：PROTO_REQ_
                             * */
    int multibulklen;       /* Number of multi bulk arguments left to read. 
                             *
                             * 剩余可读取的多批量参数数。
                             * */
    long bulklen;           /* Length of bulk argument in multi bulk request. 
                             *
                             * 多批量请求中批量参数的长度。
                             * */
    // 保存回复的链表
    list *reply;            /* List of reply objects to send to the client. 
                             *
                             * 要发送到客户端的答复对象的列表。
                             * */
    // 链表中保存的所有回复的总字节大小
    unsigned long long reply_bytes; /* Tot bytes of objects in reply list. 
                                     *
                                     * 回复列表中对象的总字节数。
                                     * */
    size_t sentlen;         /* Amount of bytes already sent in the current
                               buffer or object being sent. 
                             *
                             * 当前缓冲区或正在发送的对象中已发送的字节数。
                             * */
    time_t ctime;           /* Client creation time. 
                             *
                             * 客户端创建时间。
                             * */
    time_t lastinteraction; /* Time of the last interaction, used for timeout 
                             *
                             * 上次交互的时间，用于超时
                             * */
    time_t obuf_soft_limit_reached_time;
    uint64_t flags;         /* Client flags: CLIENT_* macros. 
                             *
                             * 客户端标志：Client_macros。
                             * */
    int authenticated;      /* Needed when the default user requires auth. 
                             *
                             * 当默认用户需要身份验证时需要。
                             * */
    // 客户端当前的同步状态
    int replstate;          /* Replication state if this is a slave. 
                             *
                             * 复制状态（如果这是从机）。
                             * */
    int repl_put_online_on_ack; /* Install slave write handler on first ACK. 
                                 *
                                 * 在第一个ACK上安装从节点写入处理程序。
                                 * 0 ：
                                 * 1 ：
                                 * */
    // 同步数据库的文件描述符
    int repldbfd;           /* Replication DB file descriptor. 
                             *
                             * 复制数据库文件描述符。
                             * */
    // 同步数据库文件的偏移量
    off_t repldboff;        /* Replication DB file offset. 
                             *
                             * 复制数据库文件偏移量。
                             * */
    // 同步数据库文件的大小
    off_t repldbsize;       /* Replication DB file size. 
                             *
                             * 复制数据库文件大小。
                             * */
    sds replpreamble;       /* Replication DB preamble. 
                             *
                             * 复制数据库前导码。
                             * */
    long long read_reploff; /* Read replication offset if this is a master. 
                             *
                             * 读取复制偏移量（如果这是主机）。
                             * */
    long long reploff;      /* Applied replication offset if this is a master. 
                             *
                             * 如果这是主机，则应用复制偏移量。
                             * */
    long long repl_ack_off; /* Replication ack offset, if this is a slave. 
                             *
                             * 复制ack偏移量，如果这是从机。
                             * */
    long long repl_ack_time;/* Replication ack time, if this is a slave. 
                             *
                             * 复制ack时间，如果这是从机。
                             * */
    long long repl_last_partial_write; /* The last time the server did a partial write from the RDB child pipe to this replica  
                                        *
                                        * 服务器上一次从RDB子管道对此复制副本进行部分写入时
                                        * */
    long long psync_initial_offset; /* FULLRESYNC reply offset other slaves
                                       copying this slave output buffer
                                       should use. 
                                     *
                                     * FULLRESYNC应答偏移量复制此从节点输出缓冲区的其他从节点应该使用。
                                     * */
    char replid[CONFIG_RUN_ID_SIZE+1]; /* Master replication ID (if master). 
                                        *
                                        * 主复制ID（如果为主）。
                                        * */
    int slave_listening_port; /* As configured with: REPLCONF listening-port 
                               *
                               * 配置为：REPLCONF侦听端口
                               * */
    char slave_ip[NET_IP_STR_LEN]; /* Optionally given by REPLCONF ip-address 
                                    *
                                    * REPLCONF ip地址可选
                                    * */
    int slave_capa;         /* Slave capabilities: SLAVE_CAPA_* bitwise OR. 
                             *
                             * 从节点功能：Slave_CAPA_bitwise OR。
                             * */
    // 事务实现
    multiState mstate;      /* MULTI/EXEC state 
                             *
                             * MULTI/EXEC状态
                             * */
    int btype;              /* Type of blocking op if CLIENT_BLOCKED. 
                             *
                             * 如果CLIENT_BLOCKED，阻塞操作的类型。
                             * */
    // 阻塞状态
    blockingState bpop;     /* blocking state 
                             *
                             * 阻塞状态
                             * */
    long long woff;         /* Last write global replication offset. 
                             *
                             * 上次写入全局复制偏移量。
                             * */
    // 被监视的 KEY
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS 
                             *
                             * 多重/执行CAS的键监视
                             * */
    // 订阅与发布
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) 
                             *
                             * 客户端感兴趣的频道（SUBSCRIBE）
                             * */
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) 
                             *
                             * 客户端感兴趣的模式（SUBSCRIBE）
                             * */
    sds peerid;             /* Cached peer ID. 
                             *
                             * 缓存的对等方ID。
                             * */
    listNode *client_list_node; /* list node in client list 
                                 *
                                 * 客户端列表中的列表节点
                                 * */
    RedisModuleUserChangedFunc auth_callback; /* Module callback to execute
                                               * when the authenticated user
                                               * changes. 
                                               *
                                               * 通过身份验证的用户更改时执行的模块回调。
                                               * */
    void *auth_callback_privdata; /* Private data that is passed when the auth
                                   * changed callback is executed. Opaque for
                                   * Redis Core. 
                                   *
                                   * 执行auth-changed回调时传递的私有数据。Redis Core的不透明。
                                   * */
    void *auth_module;      /* The module that owns the callback, which is used
                             * to disconnect the client if the module is
                             * unloaded for cleanup. Opaque for Redis Core.
                             *
                             * 拥有回调的模块，如果卸载模块进行清理，则用于断开客户端的连接。Redis Cor
                             * e的不透明。
                             * */

    /* If this client is in tracking mode and this field is non zero,
     * invalidation messages for keys fetched by this client will be send to
     * the specified client ID. 
     *
     * 如果此客户端处于跟踪模式，并且此字段为非零，则此客户端获取的键的无效消息将发送
     * 到指定的客户端ID。
     * */
    uint64_t client_tracking_redirection;
    rax *client_tracking_prefixes; /* A dictionary of prefixes we are already
                                      subscribed to in BCAST mode, in the
                                      context of client side caching. 
                                    *
                                    * 在客户端缓存的上下文中，我们已经在BCAST模式下订阅了前缀字典。
                                    * */
    /* In clientsCronTrackClientsMemUsage() we track the memory usage of
     * each client and add it to the sum of all the clients of a given type,
     * however we need to remember what was the old contribution of each
     * client, and in which categoty the client was, in order to remove it
     * before adding it the new value. 
     *
     * 在clientsCronTrackClientsMemUsage（）中，我们跟踪
     * 每个客户端的内存使用情况，并将其添加到给定类型的所有客户端的总和中，但我们需要记
     * 住每个客户端的旧贡献是什么，以及客户端属于哪个类别，以便在添加新值之前将其删除。
     * */
    uint64_t client_cron_last_memory_usage;
    int      client_cron_last_memory_type;
    /* Response buffer 
     *
     * 响应缓冲区
     * */
    // 回复缓存的当前缓存索引，「数据的位置的position」
    int bufpos;
    // 回复缓存，可以保存多个回复
    char buf[PROTO_REPLY_CHUNK_BYTES];
} client;

struct saveparam {
    time_t seconds;
    int changes;
};

struct moduleLoadQueueEntry {
    sds path;
    int argc;
    robj **argv;
};

/*
 * 共享对象
 */
struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *queued, *null[4], *nullarray[4], *emptymap[4], *emptyset[4],
    *emptyarray, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr,
    *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
    *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
    *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *unlink,
    *rpop, *lpop, *lpush, *rpoplpush, *zpopmin, *zpopmax, *emptyscan,
    *multi, *exec,
    *select[PROTO_SHARED_SELECT_CMDS],
    *integers[OBJ_SHARED_INTEGERS],
    *mbulkhdr[OBJ_SHARED_BULKHDR_LEN], /* "*<value>\r\n" 
                                        *
                                        * “*<value>\r”
                                        * */
    *bulkhdr[OBJ_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" 
                                        *
                                        * “$<value>\r”
                                        * */
    sds minstring, maxstring;
};

/* ZSETs use a specialized version of Skiplists 
 *
 * ZSET使用Skiplists的专用版本
 * */
/*
 * 跳跃表节点
 */
typedef struct zskiplistNode {
    // member 对象
    sds ele;
    // 分值
    double score;
    // 后退指针
    struct zskiplistNode *backward;
    // 层
    struct zskiplistLevel {
        // 前进指针
        struct zskiplistNode *forward;
        // 这个层跨越的节点数量
        unsigned long span;
    } level[];
} zskiplistNode;

/*
 * 跳跃表
 */
typedef struct zskiplist {
    // 头节点，尾节点
    struct zskiplistNode *header, *tail;
    // 节点数量
    unsigned long length;
    // 目前表内节点的最大层数
    int level;
} zskiplist;

/*
 * 有序集
 */
typedef struct zset {
    // 字典
    dict *dict;
    // 跳跃表
    zskiplist *zsl;
} zset;

typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes;
    unsigned long long soft_limit_bytes;
    time_t soft_limit_seconds;
} clientBufferLimitsConfig;

extern clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT];

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (PROPAGATE_*), and command pointer.
 *
 * Currently only used to additionally propagate more commands to AOF/Replication
 * after the propagation of the executed command. 
 *
 * redisOp结构定义了Redis操作，即具有参数向量、数据库ID、传播目标（P
 * ROPAGATE_*）和命令指针的命令实例。
 *
 * 当前仅用于在传播执行的命令之后将更多命令额外传播到AOF/Replication。
 * */
typedef struct redisOp {
    robj **argv;
    int argc, dbid, target;
    struct redisCommand *cmd;
} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in an easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 
 *
 * 定义Redis操作的数组。有一个API可以简单地添加到这个结构中。redisOp
 * ArrayInit（）；
 * redisOpArrayAppend（）；
 * redisOpArrayFree（）；
 * */
typedef struct redisOpArray {
    redisOp *ops;
    int numops;
} redisOpArray;

/* This structure is returned by the getMemoryOverheadData() function in
 * order to return memory overhead information. 
 *
 * 此结构由getMemoryOverheadData（）函数返回，以便返回内存开销信息。
 * */
struct redisMemOverhead {
    size_t peak_allocated;
    size_t total_allocated;
    size_t startup_allocated;
    size_t repl_backlog;
    size_t clients_slaves;
    size_t clients_normal;
    size_t aof_buffer;
    size_t lua_caches;
    size_t overhead_total;
    size_t dataset;
    size_t total_keys;
    size_t bytes_per_key;
    float dataset_perc;
    float peak_perc;
    float total_frag;
    ssize_t total_frag_bytes;
    float allocator_frag;
    ssize_t allocator_frag_bytes;
    float allocator_rss;
    ssize_t allocator_rss_bytes;
    float rss_extra;
    size_t rss_extra_bytes;
    size_t num_dbs;
    struct {
        size_t dbid;
        size_t overhead_ht_main;
        size_t overhead_ht_expires;
    } *db;
};

/* This structure can be optionally passed to RDB save/load functions in
 * order to implement additional functionalities, by storing and loading
 * metadata to the RDB file.
 *
 * Currently the only use is to select a DB at load time, useful in
 * replication in order to make sure that chained slaves (slaves of slaves)
 * select the correct DB and are able to accept the stream coming from the
 * top-level master. 
 *
 * 此结构可以选择性地传递给RDB保存/加载函数，以便通过将元数据存储并加载到RDB
 * 文件来实现附加功能。
 *
 * 目前，唯一的用途是在加载时选择一个DB，这在复制中很有用，以
 * 确保链接的从服务器（从服务器的从服务器）选择正确的DB，并能够接受来自顶级主服务
 * 器的流。
 * */
typedef struct rdbSaveInfo {
    /* Used saving and loading. 
     *
     * 用于保存和加载。
     * */
    int repl_stream_db;  /* DB to select in server.master client. 
                          *
                          * 要在server.master客户端中选择的数据库。
                          * */

    /* Used only loading. 
     *
     * 仅用于加载。
     * */
    int repl_id_is_set;  /* True if repl_id field is set. 
                          *
                          * 如果设置了repl_id字段，则为True。
                          * */
    char repl_id[CONFIG_RUN_ID_SIZE+1];     /* Replication ID. 
                                             *
                                             * 复制ID。
                                             * */
    long long repl_offset;                  /* Replication offset. 
                                             *
                                             * 复制偏移量。
                                             * */
} rdbSaveInfo;

#define RDB_SAVE_INFO_INIT {-1,0,"000000000000000000000000000000",-1}

struct malloc_stats {
    size_t zmalloc_used;
    size_t process_rss;
    size_t allocator_allocated;
    size_t allocator_active;
    size_t allocator_resident;
};

/*-----------------------------------------------------------------------------
 * TLS Context Configuration
 * TLS上下文配置
 *---------------------------------------------------------------------------- */

typedef struct redisTLSContextConfig {
    char *cert_file;
    char *key_file;
    char *dh_params_file;
    char *ca_cert_file;
    char *ca_cert_dir;
    char *protocols;
    char *ciphers;
    char *ciphersuites;
    int prefer_server_ciphers;
    int session_caching;
    int session_cache_size;
    int session_cache_timeout;
} redisTLSContextConfig;

/*-----------------------------------------------------------------------------
 * Global server state
 * 全局服务器状态
 *---------------------------------------------------------------------------*/

struct clusterState;

/* AIX defines hz to __hz, we don't use this define and in order to allow
 * Redis build on AIX we need to undef it. 
 *
 * AIX将hz定义为__hz，我们不使用这个定义，为了允许Redis在AIX上构建
 * ，我们需要取消它的定义。
 * */
#ifdef _AIX
#undef hz
#endif

#define CHILD_INFO_MAGIC 0xC17DDA7A12345678LL
#define CHILD_TYPE_NONE 0
#define CHILD_TYPE_RDB 1
#define CHILD_TYPE_AOF 2
#define CHILD_TYPE_LDB 3
#define CHILD_TYPE_MODULE 4

struct redisServer {
    /* General 
     *
     * 全体的
     * */
    pid_t pid;                  /* Main process pid. 
                                 *
                                 * 主进程pid。
                                 * */
    pthread_t main_thread_id;         /* Main thread id 
                                       *
                                       * 主线程id
                                       * */
    char *configfile;           /* Absolute config file path, or NULL 
                                 *
                                 * 绝对配置文件路径，或NULL
                                 * */
    char *executable;           /* Absolute executable file path. 
                                 *
                                 * 绝对可执行文件路径。
                                 * */
    char **exec_argv;           /* Executable argv vector (copy). 
                                 *
                                 * 可执行argv矢量（副本）。
                                 * */
    int dynamic_hz;             /* Change hz value depending on # of clients. 
                                 *
                                 * 根据客户端数量更改hz值。
                                 * */
    int config_hz;              /* Configured HZ value. May be different than
                                   the actual 'hz' field value if dynamic-hz
                                   is enabled. 
                                 *
                                 * 配置的HZ值。如果启用了动态hz，则可能与实际的“hz”字段值不同。
                                 * */
    mode_t umask;               /* The umask value of the process on startup 
                                 *
                                 * 启动时进程的umask值
                                 * */
    int hz;                     /* serverCron() calls frequency in hertz 
                                 *
                                 * serverCron（）调用频率（赫兹）
                                 * */
    int in_fork_child;          /* indication that this is a fork child 
                                 *
                                 * 表示这是一个分叉子
                                 * */
    // 数据库数组
    redisDb *db;
    // 命令表
    dict *commands;             /* Command table 
                                 *
                                 * 命令表
                                 * */
    dict *orig_commands;        /* Command table before command renaming. 
                                 *
                                 * 命令重命名前的命令表。
                                 * */
    // 事件状态结构
    aeEventLoop *el;
    _Atomic unsigned int lruclock; /* Clock for LRU eviction 
                                    *
                                    * LRU逐出时钟
                                    * */
    volatile sig_atomic_t shutdown_asap; /* SHUTDOWN needed ASAP 
                                          *
                                          * 需要尽快关闭
                                          * */
    int activerehashing;        /* Incremental rehash in serverCron() 
                                 *
                                 * serverCron（）中的增量重新散列
                                 * */
    int active_defrag_running;  /* Active defragmentation running (holds current scan aggressiveness) 
                                 *
                                 * 活动碎片整理正在运行（保持当前扫描攻击性）
                                 * */
    // PID 文件路径
    char *pidfile;              /* PID file path 
                                 *
                                 * PID文件路径
                                 * */
    // 宿主架构字长
    int arch_bits;              /* 32 or 64 depending on sizeof(long) 
                                 *
                                 * 32或64取决于尺寸（长）
                                 * */
    // CRON 函数调用的次数
    int cronloops;              /* Number of times the cron function run 
                                 *
                                 * cron函数运行的次数
                                 * */
    // 每次调用 exec 时都创建新 ID
    char runid[CONFIG_RUN_ID_SIZE+1];  /* ID always different at every exec. 
                                        *
                                        * 每个高管的ID总是不同的。
                                        * */
    // 如果服务器为 SENTINEL ，那么为真
    int sentinel_mode;          /* True if this instance is a Sentinel. 
                                 *
                                 * 如果此实例是Sentinel，则为True。
                                 * */
    size_t initial_memory_usage; /* Bytes used after initialization. 
                                  *
                                  * 初始化后使用的字节数。
                                  * */
    int always_show_logo;       /* Show logo even for non-stdout logging. 
                                 *
                                 * 即使对于非stdout日志记录也显示徽标。
                                 * */
    char *ignore_warnings;      /* Config: warnings that should be ignored. 
                                 *
                                 * 配置：应忽略的警告。
                                 * */
    /* Modules 
     *
     * 模块
     * */

    dict *moduleapi;            /* Exported core APIs dictionary for modules. 
                                 *
                                 * 已导出模块的核心API字典。
                                 * */
    dict *sharedapi;            /* Like moduleapi but containing the APIs that
                                   modules share with each other. 
                                 *
                                 * 类似于moduleapi，但包含模块之间共享的API。
                                 * */
    list *loadmodule_queue;     /* List of modules to load at startup. 
                                 *
                                 * 启动时要加载的模块列表。
                                 * */
    int module_blocked_pipe[2]; /* Pipe used to awake the event loop if a
                                   client blocked on a module command needs
                                   to be processed. 
                                 *
                                 * 如果需要处理模块命令上被阻止的客户端，则用于唤醒事件循环的管道。
                                 * */
    pid_t module_child_pid;     /* PID of module child 
                                 *
                                 * 模块子项PID
                                 * */


    /* Networking 
     *
     * 网络
     * */

    int port;                   /* TCP listening port 
                                 *
                                 * TCP侦听端口
                                 * */
    int tls_port;               /* TLS listening port 
                                 *
                                 * TLS侦听端口
                                 * */
    int tcp_backlog;            /* TCP listen() backlog 
                                 *
                                 * TCP列表（）囤积
                                 * */
    // 绑定地址
    char *bindaddr[CONFIG_BINDADDR_MAX]; /* Addresses we should bind to 
                                          *
                                          * 我们应该绑定的地址
                                          * */
    int bindaddr_count;         /* Number of addresses in server.bindaddr[] 
                                 *
                                 * 服务器中的地址数。bindaddr[]
                                 * */
    // 套接字路径
    char *unixsocket;           /* UNIX socket path 
                                 *
                                 * UNIX套接字路径
                                 * */
    // 套接字权限
    mode_t unixsocketperm;      /* UNIX socket permission 
                                 *
                                 * UNIX套接字权限
                                 * */
    int ipfd[CONFIG_BINDADDR_MAX]; /* TCP socket file descriptors 
                                    *
                                    * TCP套接字文件描述符
                                    * */
    int ipfd_count;             /* Used slots in ipfd[] 
                                 *
                                 * ipfd[]中已使用的插槽
                                 * */
    int tlsfd[CONFIG_BINDADDR_MAX]; /* TLS socket file descriptors 
                                     *
                                     * TLS套接字文件描述符
                                     * */
    int tlsfd_count;            /* Used slots in tlsfd[] 
                                 *
                                 * tlsfd[]中已使用的插槽
                                 * */
    int sofd;                   /* Unix socket file descriptor 
                                 *
                                 * Unix套接字文件描述符
                                 * */
    int cfd[CONFIG_BINDADDR_MAX];/* Cluster bus listening socket 
                                  *
                                  * 集群总线监听插座
                                  * */
    int cfd_count;              /* Used slots in cfd[] 
                                 *
                                 * cfd中使用的插槽[]
                                 * */

    /* 客户端 */
    // 所有当前活动的客户端
    list *clients;              /* List of active clients 
                                 *
                                 * 活动客户端列表
                                 * */
    // 所有等待关闭的客户端
    list *clients_to_close;     /* Clients to close asynchronously 
                                 *
                                 * 要异步关闭的客户端
                                 * */
    list *clients_pending_write; /* There is to write or install handler. 
                                  *
                                  * 存在要写入或安装的处理程序。
                                  * */
    list *clients_pending_read;  /* Client has pending read socket buffers. 
                                  *
                                  * 客户端具有挂起的读取套接字缓冲区。
                                  * */
    // 所有附属节点和 MONITOR
    list *slaves, *monitors;    /* List of slaves and MONITORs 
                                 *
                                 * 从节点设备和监视器列表
                                 * */
    // 当前客户端，只在创建崩溃报告时使用
    client *current_client;     /* Current client executing the command. 
                                 *
                                 * 正在执行命令的当前客户端。
                                 * */

    rax *clients_timeout_table; /* Radix tree for blocked clients timeouts. 
                                 *
                                 * 阻止的客户端超时的基数树。
                                 * */
    long fixed_time_expire;     /* If > 0, expire keys against server.mstime. 
                                 *
                                 * 如果>0，则根据server.mstime使键过期。
                                 * */
    rax *clients_index;         /* Active clients dictionary by client ID. 
                                 *
                                 * 活动客户端字典（按客户端ID）。
                                 * */
    int clients_paused;         /* True if clients are currently paused 
                                 *
                                 * 如果客户端当前已暂停，则为True
                                 * */
    mstime_t clients_pause_end_time; /* Time when we undo clients_paused 
                                      *
                                      * 撤消客户端的时间_暂停
                                      * */
    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c 
                                  *
                                  * anet.c的错误缓冲区
                                  * */
    dict *migrate_cached_sockets;/* MIGRATE cached sockets 
                                  *
                                  * MIGRATE缓存套接字
                                  * */
    _Atomic uint64_t next_client_id; /* Next client unique ID. Incremental. 
                                      *
                                      * 下一个客户端唯一ID。增量。
                                      * */
    int protected_mode;         /* Don't accept external connections. 
                                 *
                                 * 不接受外部连接。
                                 * */
    int gopher_enabled;         /* If true the server will reply to gopher
                                   queries. Will still serve RESP2 queries. 
                                 *
                                 * 如果为true，服务器将回复gopher查询。仍将提供RESP2查询。
                                 * */
    int io_threads_num;         /* Number of IO threads to use. 
                                 *
                                 * 要使用的IO线程数。
                                 * */
    int io_threads_do_reads;    /* Read and parse from IO threads? 
                                 *
                                 * 从IO线程读取并解析？
                                 * */
    int io_threads_active;      /* Is IO threads currently active? 
                                 *
                                 * IO线程当前是否处于活动状态？
                                 * */
    long long events_processed_while_blocked; /* processEventsWhileBlocked() 
                                               *
                                               * processEventsWhileBlocked（）
                                               * */

    /* RDB / AOF loading information 
     *
     * RDB/AOF加载信息
     * */
    volatile sig_atomic_t loading; /* We are loading data from disk if true 
                                    *
                                    * 如果为true，我们将从磁盘加载数据
                                    * */
    off_t loading_total_bytes;
    off_t loading_loaded_bytes;
    time_t loading_start_time;
    off_t loading_process_events_interval_bytes;
    /* Fast pointers to often looked up command 
     *
     * 指向经常查找的命令的快速指针
     * */
    struct redisCommand *delCommand, *multiCommand, *lpushCommand,
                        *lpopCommand, *rpopCommand, *zpopminCommand,
                        *zpopmaxCommand, *sremCommand, *execCommand,
                        *expireCommand, *pexpireCommand, *xclaimCommand,
                        *xgroupCommand, *rpoplpushCommand;
    /* Fields used only for stats 
     *
     * 仅用于统计的字段
     * */
    time_t stat_starttime;          /* Server start time 
                                     *
                                     * 服务器启动时间
                                     * */
    long long stat_numcommands;     /* Number of processed commands 
                                     *
                                     * 已处理的命令数
                                     * */
    long long stat_numconnections;  /* Number of connections received 
                                     *
                                     * 接收的连接数
                                     * */
    long long stat_expiredkeys;     /* Number of expired keys 
                                     *
                                     * 过期键数
                                     * */
    double stat_expired_stale_perc; /* Percentage of keys probably expired 
                                     *
                                     * 可能过期的键百分比
                                     * */
    long long stat_expired_time_cap_reached_count; /* Early expire cylce stops.
                                                    *
                                                    * 提前到期的循环停止。
                                                    * */
    long long stat_expire_cycle_time_used; /* Cumulative microseconds used. 
                                            *
                                            * 使用的累计微秒数。
                                            * */
    long long stat_evictedkeys;     /* Number of evicted keys (maxmemory) 
                                     *
                                     * 收回的键数（最大内存）
                                     * */
    long long stat_keyspace_hits;   /* Number of successful lookups of keys 
                                     *
                                     * 成功查找键的次数
                                     * */
    long long stat_keyspace_misses; /* Number of failed lookups of keys 
                                     *
                                     * 键查找失败的次数
                                     * */
    long long stat_active_defrag_hits;      /* number of allocations moved 
                                             *
                                             * 移动的分配数
                                             * */
    long long stat_active_defrag_misses;    /* number of allocations scanned but not moved 
                                             *
                                             * 已扫描但未移动的分配数
                                             * */
    long long stat_active_defrag_key_hits;  /* number of keys with moved allocations 
                                             *
                                             * 分配已移动的键数
                                             * */
    long long stat_active_defrag_key_misses;/* number of keys scanned and not moved 
                                             *
                                             * 已扫描但未移动的键数
                                             * */
    long long stat_active_defrag_scanned;   /* number of dictEntries scanned 
                                             *
                                             * 扫描的dictEntries数
                                             * */
    size_t stat_peak_memory;        /* Max used memory record 
                                     *
                                     * 最大已用内存记录
                                     * */
    long long stat_fork_time;       /* Time needed to perform latest fork() 
                                     *
                                     * 执行最新fork（）所需的时间
                                     * */
    // fork 进程，每 GB 内存拷贝的内存，主要拷贝的是页目录项和页表项 【可以参考 linux 线性地址的理解】
    double stat_fork_rate;          /* Fork rate in GB/sec. 
                                     *
                                     * 分叉速率（以GB/秒为单位）。
                                     * */
    long long stat_rejected_conn;   /* Clients rejected because of maxclients 
                                     *
                                     * 由于maxclient而拒绝的客户端
                                     * */
    long long stat_sync_full;       /* Number of full resyncs with slaves. 
                                     *
                                     * 具有从节点服务器的完全重新同步数。
                                     * */
    long long stat_sync_partial_ok; /* Number of accepted PSYNC requests. 
                                     *
                                     * 已接受的PSYNC请求数。
                                     * */
    long long stat_sync_partial_err;/* Number of unaccepted PSYNC requests. 
                                     *
                                     * 未接受的PSYNC请求数。
                                     * */
    // 保存慢查询日志的链表
    list *slowlog;                  /* SLOWLOG list of commands 
                                     *
                                     * SLOWLOG命令列表
                                     * */
    // 慢查询日志的当前 id 值
    long long slowlog_entry_id;     /* SLOWLOG current entry ID 
                                     *
                                     * SLOWLOG当前节点ID
                                     * */
    // 慢查询时间限制
    long long slowlog_log_slower_than; /* SLOWLOG time limit (to get logged) 
                                        *
                                        * SLOWLOG时间限制（记录）
                                        * */
    // 慢查询日志的最大节点数量
    unsigned long slowlog_max_len;     /* SLOWLOG max number of items logged 
                                        *
                                        * SLOWLOG记录的最大项目数
                                        * */
    struct malloc_stats cron_malloc_stats; /* sampled in serverCron(). 
                                            *
                                            * serverCron（）中的示例。
                                            * */
    _Atomic long long stat_net_input_bytes; /* Bytes read from network. 
                                             *
                                             * 从网络读取的字节数。
                                             * */
    _Atomic long long stat_net_output_bytes; /* Bytes written to network. 
                                     *
                                     * 写入网络的字节数。
                                     * */
    size_t stat_rdb_cow_bytes;      /* Copy on write bytes during RDB saving. 
                                     *
                                     * RDB保存期间写入时复制字节。
                                     * */
    size_t stat_aof_cow_bytes;      /* Copy on write bytes during AOF rewrite. 
                                     *
                                     * 在AOF重写期间写入时复制字节。
                                     * */
    size_t stat_module_cow_bytes;   /* Copy on write bytes during module fork. 
                                     *
                                     * 在模块分叉期间写入字节时进行复制。
                                     * */
    uint64_t stat_clients_type_memory[CLIENT_TYPE_COUNT];/* Mem usage by type 
                                                          *
                                                          * Mem使用情况（按类型）
                                                          * */
    long long stat_unexpected_error_replies; /* Number of unexpected (aof-loading, replica to master, etc.) error replies 
                                              *
                                              * 意外错误回复数（aof加载、复制到主机等）
                                              * */
    long long stat_io_reads_processed; /* Number of read events processed by IO / Main threads 
                                        *
                                        * IO/主线程处理的读取事件数
                                        * */
    long long stat_io_writes_processed; /* Number of write events processed by IO / Main threads 
                                         *
                                         * IO/主线程处理的写入事件数
                                         * */
    _Atomic long long stat_total_reads_processed; /* Total number of read events processed 
                                                   *
                                                   * 已处理的读取事件总数
                                                   * */
    _Atomic long long stat_total_writes_processed; /* Total number of write events processed 
                                                    *
                                                    * 已处理的写入事件总数
                                                    * */
    /* The following two are used to track instantaneous metrics, like
     * number of operations per second, network traffic. 
     *
     * 以下两个用于跟踪瞬时指标，如每秒操作次数、网络流量。
     * */
    struct {
        long long last_sample_time; /* Timestamp of last sample in ms 
                                     *
                                     * 最后一个样本的时间戳（毫秒）
                                     * */
        long long last_sample_count;/* Count in last sample 
                                     *
                                     * 上次采样计数
                                     * */
        long long samples[STATS_METRIC_SAMPLES];
        int idx;
    } inst_metric[STATS_METRIC_COUNT];
    /* Configuration 
     *
     * 配置
     * */
    int verbosity;                  /* Loglevel in redis.conf 
                                     *
                                     * redis.conf中的日志级别
                                     * */
    int maxidletime;                /* Client timeout in seconds 
                                     *
                                     * 客户端超时（秒）
                                     * */
    int tcpkeepalive;               /* Set SO_KEEPALIVE if non-zero. 
                                     *
                                     * 如果非零，则设置SO_KEEPALIVE。
                                     * */
    int active_expire_enabled;      /* Can be disabled for testing purposes. 
                                     *
                                     * 可以出于测试目的禁用。
                                     * */
    int active_expire_effort;       /* From 1 (default) to 10, active effort. 
                                     *
                                     * 从1（默认值）到10，积极努力。
                                     * */
    int active_defrag_enabled;
    int jemalloc_bg_thread;         /* Enable jemalloc background thread 
                                     *
                                     * 启用jemalloc后台线程
                                     * */
    size_t active_defrag_ignore_bytes; /* minimum amount of fragmentation waste to start active defrag 
                                        *
                                        * 启动活动碎片整理所需的最小碎片浪费量
                                        * */
    int active_defrag_threshold_lower; /* minimum percentage of fragmentation to start active defrag 
                                        *
                                        * 启动活动碎片整理的最小碎片百分比
                                        * */
    int active_defrag_threshold_upper; /* maximum percentage of fragmentation at which we use maximum effort 
                                        *
                                        * 我们使用最大努力的最大碎片百分比
                                        * */
    int active_defrag_cycle_min;       /* minimal effort for defrag in CPU percentage 
                                        *
                                        * CPU百分比中碎片整理的最小工作量
                                        * */
    int active_defrag_cycle_max;       /* maximal effort for defrag in CPU percentage 
                                        *
                                        * CPU百分比中碎片整理的最大工作量
                                        * */
    unsigned long active_defrag_max_scan_fields; /* maximum number of fields of set/hash/zset/list to process from within the main dict scan 
                                                  *
                                                  * 要在主dict扫描中处理的set/hash/zset/list的最大字段数
                                                  * */
    _Atomic size_t client_max_querybuf_len; /* Limit for client query buffer length 
                                             *
                                             * 客户端查询缓冲区长度限制
                                             * */
    int dbnum;                      /* Total number of configured DBs 
                                     *
                                     * 配置的数据库总数
                                     * */
    int supervised;                 /* 1 if supervised, 0 otherwise. 
                                     *
                                     * 如果有监督，则为1，否则为0。
                                     * */
    int supervised_mode;            /* See SUPERVISED_* 
                                     *
                                     * 请参阅 SUPERVISED_
                                     * */
    int daemonize;                  /* True if running as a daemon 
                                     *
                                     * 如果作为守护程序运行，则为True
                                     * */
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT];


    /* AOF persistence 
     *
     * AOF持久性
     * */
    int aof_enabled;                /* AOF configuration 
                                     *
                                     * AOF配置
                                     * */
    int aof_state;                  /* AOF_(ON|OFF|WAIT_REWRITE) */
    int aof_fsync;                  /* Kind of fsync() policy 
                                     *
                                     * fsync（）策略的类型
                                     * */
    char *aof_filename;             /* Name of the AOF file 
                                     *
                                     * AOF文件的名称
                                     * */
    int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. 
                                     *
                                     * 如果正在进行重写，请不要进行fsync。
                                     * */
    int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... 
                                     *
                                     * 如果%增长大于M，则重写AOF。。。
                                     * */
    off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. 
                                     *
                                     * AOF文件是至少N个字节。
                                     * */
    off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. 
                                     *
                                     * 最新启动或重写时的AOF大小。
                                     * */
    off_t aof_current_size;         /* AOF current size. 
                                     *
                                     * AOF当前大小。
                                     * */
    off_t aof_fsync_offset;         /* AOF offset which is already synced to disk. 
                                     *
                                     * 已同步到磁盘的AOF偏移量。
                                     * */
    int aof_flush_sleep;            /* Micros to sleep before flush. (used by tests) 
                                     *
                                     * 在刷盘前睡眠一下。（用于测试）
                                     * */
    int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. 
                                     *
                                     * BGSAVE终止后进行重写。
                                     * */
    pid_t aof_child_pid;            /* PID if rewriting process 
                                     *
                                     * PID如果重写过程
                                     * */
    list *aof_rewrite_buf_blocks;   /* Hold changes during an AOF rewrite. 
                                     *
                                     * 在AOF重写期间保留更改。
                                     * */
    sds aof_buf;      /* AOF buffer, written before entering the event loop 
                       *
                       * AOF缓冲区，在进入事件循环之前写入
                       * */
    int aof_fd;       /* File descriptor of currently selected AOF file 
                       *
                       * 当前所选AOF文件的文件描述符
                       * */
    int aof_selected_db; /* Currently selected DB in AOF 
                          *
                          * AOF中当前选择的DB
                          * */
    time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush 
                                       *
                                       * 延迟AOF刷新的UNIX时间
                                       * */
    time_t aof_last_fsync;            /* UNIX time of last fsync() 
                                       *
                                       * 最后一次fsync（）的UNIX时间
                                       * */
    time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. 
                                     *
                                     * 上次AOF重写运行所用的时间。
                                     * */
    time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. 
                                     *
                                     * 当前AOF重写开始时间。
                                     * */
    int aof_lastbgrewrite_status;   /* C_OK or C_ERR 
                                     *
                                     * C_OK或C_ERR
                                     * */
    unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter 
                                       *
                                       * 延迟AOF fsync（）计数器
                                       * */
    int aof_rewrite_incremental_fsync;/* fsync incrementally while aof rewriting? 
                                       *
                                       * 在aof重写时递增fsync？
                                       * */
    int rdb_save_incremental_fsync;   /* fsync incrementally while rdb saving? 
                                       *
                                       * 在rdb保存时递增fsync？
                                       * */
    int aof_last_write_status;      /* C_OK or C_ERR 
                                     *
                                     * C_OK或C_ERR
                                     * */
    int aof_last_write_errno;       /* Valid if aof_last_write_status is ERR 
                                     *
                                     * 如果aof_last_write_status为ERR，则有效
                                     * */
    int aof_load_truncated;         /* Don't stop on unexpected AOF EOF. 
                                     *
                                     * 不要在意外的AOF EOF上停下来。
                                     * */
    int aof_use_rdb_preamble;       /* Use RDB preamble on AOF rewrites. 
                                     *
                                     * 在AOF重写时使用RDB前导码。
                                     * */
    /* AOF pipes used to communicate between parent and child during rewrite. 
     *
     * 在重写期间，用于在父级和子级之间进行通信的AOF管道。
     * */
    int aof_pipe_write_data_to_child;
    int aof_pipe_read_data_from_parent;
    int aof_pipe_write_ack_to_parent;
    int aof_pipe_read_ack_from_child;
    int aof_pipe_write_ack_to_child;
    int aof_pipe_read_ack_from_parent;
    int aof_stop_sending_diff;     /* If true stop sending accumulated diffs
                                      to child process. 
                                    *
                                    * 如果为true，则停止向子进程发送累积的diff。
                                    * */
    sds aof_child_diff;             /* AOF diff accumulator child side. 
                                     *
                                     * AOF差分累加器子进程侧。
                                     * */
    /* RDB persistence 
     *
     * RDB持久性
     * */
    long long dirty;                /* Changes to DB from the last save 
                                     *
                                     * 上次保存后对数据库的更改
                                     * */
    long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE 
                                     *
                                     * 用于在失败的BGSAVE上还原脏
                                     * */
    pid_t rdb_child_pid;            /* PID of RDB saving child 
                                     *
                                     * RDB保存子进程的PID
                                     * */
    struct saveparam *saveparams;   /* Save points array for RDB 
                                     *
                                     * RDB的保存点数组
                                     * */
    int saveparamslen;              /* Number of saving points 
                                     *
                                     * 保存点数
                                     * */
    char *rdb_filename;             /* Name of RDB file 
                                     *
                                     * RDB文件的名称
                                     * */
    int rdb_compression;            /* Use compression in RDB? 
                                     *
                                     * 在RDB中使用压缩？
                                     * */
    int rdb_checksum;               /* Use RDB checksum? 
                                     *
                                     * 是否使用RDB校验和？
                                     * */
    int rdb_del_sync_files;         /* Remove RDB files used only for SYNC if
                                       the instance does not use persistence. 
                                     *
                                     * 如果实例不使用持久性，则删除仅用于SYNC的RDB文件。
                                     * */
    time_t lastsave;                /* Unix time of last successful save 
                                     *
                                     * 上次成功保存的Unix时间
                                     * */
    time_t lastbgsave_try;          /* Unix time of last attempted bgsave 
                                     *
                                     * 上次尝试bgsave的Unix时间
                                     * */
    time_t rdb_save_time_last;      /* Time used by last RDB save run. 
                                     *
                                     * 上次RDB存储运行所用的时间。
                                     * */
    time_t rdb_save_time_start;     /* Current RDB save start time. 
                                     *
                                     * 当前RDB保存开始时间。
                                     * */
    int rdb_bgsave_scheduled;       /* BGSAVE when possible if true. 
                                     *
                                     * 如果为true，则在可能的情况下执行BGSAVE。
                                     * */
    int rdb_child_type;             /* Type of save by active child. 
                                     *
                                     * 按活动子项保存的类型。
                                     * */
    int lastbgsave_status;          /* C_OK or C_ERR 
                                     *
                                     * C_OK或C_ERR
                                     * */
    int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE 
                                     *
                                     * 如果不能BGSAVE，则不允许写入
                                     * */
    int rdb_pipe_read;              /* RDB pipe used to transfer the rdb data 
                                     *
                                     * 用于传输RDB数据的RDB管道
                                     * */
                                    /* to the parent process in diskless repl. 
                                     *
                                     * 到无盘repl中的父进程。
                                     * */
    int rdb_child_exit_pipe;        /* Used by the diskless parent allow child exit. 
                                     *
                                     * 由无盘父进程使用，允许子进程退出。
                                     * */
    connection **rdb_pipe_conns;    /* Connections which are currently the 
                                     *
                                     * 当前为
                                     * */
    int rdb_pipe_numconns;          /* target of diskless rdb fork child. 
                                     *
                                     * 无盘rdb fork-child的目标。
                                     * */
    int rdb_pipe_numconns_writing;  /* Number of rdb conns with pending writes. 
                                     *
                                     * 具有挂起写入的rdb连接数。
                                     * */
    char *rdb_pipe_buff;            /* In diskless replication, this buffer holds data 
                                     *
                                     * 在无盘复制中，此缓冲区保存数据
                                     * */
    int rdb_pipe_bufflen;           /* that was read from the the rdb pipe. 
                                     *
                                     * 这是从rdb管道中读取的。
                                     * 在无盘复制中，缓冲区保存数据的长度
                                     * */
    int rdb_key_save_delay;         /* Delay in microseconds between keys while
                                     * writing the RDB. (for testings) 
                                     *
                                     * 写入RDB时键之间的延迟（以微秒为单位）。（用于测试）
                                     * */
    int key_load_delay;             /* Delay in microseconds between keys while
                                     * loading aof or rdb. (for testings) 
                                     *
                                     * 加载aof或rdb时键之间的延迟（以微秒为单位）。（用于测试）
                                     * */
    /* Pipe and data structures for child -> parent info sharing. 
     *
     * 用于子级->父级信息共享的管道和数据结构。
     * */
    int child_info_pipe[2];         /* Pipe used to write the child_info_data. 
                                     *
                                     * 用于写入child_info_data的管道。
                                     * */
    struct {
        int process_type;           /* AOF or RDB child? 
                                     *
                                     * AOF还是RDB进程？
                                     * */
        size_t cow_size;            /* Copy on write size. 
                                     *
                                     * 写入时复制大小。
                                     * */
        unsigned long long magic;   /* Magic value to make sure data is valid. 
                                     *
                                     * 确保数据有效的魔术值。
                                     * */
    } child_info_data;
    /* Propagation of commands in AOF / replication 
     *
     * AOF/复制中的命令传播
     * */


    redisOpArray also_propagate;    /* Additional command to propagate. 
                                     *
                                     * 要传播的附加命令。
                                     * */
    /* Logging 
     *
     * 日志记录
     * */
    char *logfile;                  /* Path of log file 
                                     *
                                     * 日志文件的路径
                                     * */
    int syslog_enabled;             /* Is syslog enabled? 
                                     *
                                     * 系统日志是否已启用？
                                     * */
    char *syslog_ident;             /* Syslog ident 
                                     *
                                     * 标识系统日志
                                     * */
    int syslog_facility;            /* Syslog facility 
                                     *
                                     * Syslog设施
                                     * */
    /* Replication (master) 
     *
     * 复制（主）
     * */
    char replid[CONFIG_RUN_ID_SIZE+1];  /* My current replication ID. 
                                         *
                                         * 我当前的复制ID
                                         * 其实就是主从复制，主节点的节点Id
                                         * */
    char replid2[CONFIG_RUN_ID_SIZE+1]; /* replid inherited from master
                                         *
                                         * 从master继承的replid
                                         * 我从节点升级为主节点的时候，这个值就是曾经作为从节点的对应的主节点的Id
                                         * */
    long long master_repl_offset;   /* My current replication offset 
                                     *
                                     * 我当前的复制偏移量
                                     * */
    long long second_replid_offset; /* Accept offsets up to this for replid2. 
                                     *
                                     * 接受replid2的最大偏移量。
                                     * 其实就是变更时「slave 变成 master 」的时候，作为 slave 已经同步了的偏移量
                                     * */
    int slaveseldb;                 /* Last SELECTed DB in replication output 
                                     *
                                     * 复制输出中上次选择的数据库
                                     * */
    int repl_ping_slave_period;     /* Master pings the slave every N seconds 
                                     *
                                     * 主设备每N秒ping一次从设备
                                     * */
    char *repl_backlog;             /* Replication backlog for partial syncs 
                                     *
                                     * 部分同步的复制缓冲区
                                     * */
    long long repl_backlog_size;    /* Backlog circular buffer size 
                                     *
                                     * 积压工作循环缓冲区大小
                                     * */
    long long repl_backlog_histlen; /* Backlog actual data length 
                                     *
                                     * 积压实际数据长度
                                     * 其实就是真正需要发送的同步数据长度 = 总数据 - 已经发送的数据 
                                     * */
    long long repl_backlog_idx;     /* Backlog circular buffer current offset,
                                       that is the next byte will'll write to.
                                     *
                                     * Backlog循环缓冲区当前偏移量，也就是下一个字节将写入的值。其实就是缓冲区待写入数据的开始位置
                                     * */
    long long repl_backlog_off;     /* Replication "master offset" of first
                                       byte in the replication backlog buffer.
                                     *
                                     * 复制囤积缓冲区中第一个字节的复制“主偏移量”。其实就是待发送的数据的开始位置
                                     * */
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. 
                                     *
                                     * 积压工作释放后没有从节点的时间。
                                     * */
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0. 
                                     *
                                     * 从那时起我们就没有从节点了。仅当server.slaves len为0时有效。
                                     * */
    int repl_min_slaves_to_write;   /* Min number of slaves to write. 
                                     *
                                     * 要写入的最小从节点数量。
                                     * */
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. 
                                     *
                                     * ＜count＞从机写入的最大滞后时间。
                                     * */
    int repl_good_slaves_count;     /* Number of slaves with lag <= max_lag. 
                                     *
                                     * 滞后<=max_lag的从机数量。
                                     * */
    int repl_diskless_sync;         /* Master send RDB to slaves sockets directly. 
                                     *
                                     * 主节点将 RDB 直接通过 socket 发送到从节点。
                                     * */
    int repl_diskless_load;         /* Slave parse RDB directly from the socket.
                                     * see REPL_DISKLESS_LOAD_* enum 
                                     *
                                     * 从服务器直接从套接字解析RDB。请参阅  REPL_DISKLESS_LOAD_* enum
                                     * */
    int repl_diskless_sync_delay;   /* Delay to start a diskless repl BGSAVE. 
                                     *
                                     * 延迟启动无盘repl BGSAVE。
                                     * */
    /* Replication (slave) 
     *
     * 复制（从节点）
     * */
    char *masteruser;               /* AUTH with this user and masterauth with master 
                                     *
                                     * 使用此用户进行AUTH，使用master进行master身份验证
                                     * */
    char *masterauth;               /* AUTH with this password with master 
                                     *
                                     * 使用主密码进行AUTH
                                     * */
    char *masterhost;               /* Hostname of master 
                                     *
                                     * 主节点主机名
                                     * 如果值为空才可以是主节点
                                     * */
    int masterport;                 /* Port of master 
                                     *
                                     * 主机端口
                                     * */
    int repl_timeout;               /* Timeout after N seconds of master idle 
                                     *
                                     * 主机空闲N秒后超时，默认值60
                                     * */
    client *master;     /* Client that is master for this slave 
                         *
                         * 此从节点服务器的主客户端
                         * */
    client *cached_master; /* Cached master to be reused for PSYNC. 
                            *
                            * 缓存的主机将被重新用于PSYNC。
                            * */
    int repl_syncio_timeout; /* Timeout for synchronous I/O calls 
                              *
                              * 同步I/O调用超时
                              * */
    int repl_state;          /* Replication status if the instance is a slave 
                              *
                              * 如果实例是从节点实例，则复制状态
                              * */
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. 
                               *
                               * 同步期间要从master读取的RDB的大小。
                               * */
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. 
                               *
                               * 同步期间从master读取的RDB数量。
                               * */
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. 
                                         *
                                         * 上次fsync ed时的偏移量。
                                         * */
    connection *repl_transfer_s;     /* Slave -> Master SYNC connection 
                                      *
                                      * 从->主SYNC连接
                                      * */
    int repl_transfer_fd;    /* Slave -> Master SYNC temp file descriptor 
                              *
                              * 从->主SYNC临时文件描述符
                              * */
    char *repl_transfer_tmpfile; /* Slave-> master SYNC temp file name 
                                  *
                                  * 从->主SYNC临时文件名
                                  * */
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout 
                                  *
                                  * 最新读取的Unix时间，用于超时
                                  * */
    int repl_serve_stale_data; /* Serve stale data when link is down? 
                                *
                                * 链接断开时提供过时数据？
                                * */
    int repl_slave_ro;          /* Slave is read only? 
                                 *
                                 * Slave是只读的吗？
                                 * */
    int repl_slave_ignore_maxmemory;    /* If true slaves do not evict. 
                                         *
                                         * 如果真正的从节点不驱逐。
                                         * */
    time_t repl_down_since; /* Unix time at which link with master went down 
                             *
                             * 与master链接断开的Unix时间
                             * */
    int repl_disable_tcp_nodelay;   /* Disable TCP_NODELAY after SYNC? 
                                     *
                                     * SYNC后禁用TCP_NODELAY？
                                     * */
    int slave_priority;             /* Reported in INFO and used by Sentinel. 
                                     *
                                     * 在INFO中报告并由Sentinel使用。
                                     * */
    int slave_announce_port;        /* Give the master this listening port. 
                                     *
                                     * 给主机这个监听端口。
                                     * */
    char *slave_announce_ip;        /* Give the master this ip address. 
                                     *
                                     * 将此ip地址提供给主机。
                                     * */
    /* The following two fields is where we store master PSYNC replid/offset
     * while the PSYNC is in progress. At the end we'll copy the fields into
     * the server->master client structure. 
     *
     * 以下两个字段是在PSYNC进行时存储主PSYNC replid/offset的地方。最后，我们将把字段复制到服务器->主客户端结构中。
     * */
    char master_replid[CONFIG_RUN_ID_SIZE+1];  /* Master PSYNC runid. 
                                                *
                                                * 主PSYNC runid。
                                                * */
    long long master_initial_offset;           /* Master PSYNC offset. 
                                                *
                                                * 主PSYNC偏移。
                                                * */
    int repl_slave_lazy_flush;          /* Lazy FLUSHALL before loading DB? 
                                         *
                                         * 加载DB之前懒惰的FLUSHALL？
                                         * */
    /* Replication script cache. 
     *
     * 复制脚本缓存。
     * */
    dict *repl_scriptcache_dict;        /* SHA1 all slaves are aware of. 
                                         *
                                         * SHA1所有从节点都知道。
                                         * */
    list *repl_scriptcache_fifo;        /* First in, first out LRU eviction. 
                                         *
                                         * 先进先出LRU驱逐。
                                         * */
    unsigned int repl_scriptcache_size; /* Max number of elements. 
                                         *
                                         * 元素的最大数量。
                                         * */
    /* Synchronous replication. 
     *
     * 同步复制。
     * */
    list *clients_waiting_acks;         /* Clients waiting in WAIT command. 
                                         *
                                         * 等待WAIT命令的客户端。
                                         * */
    int get_ack_from_slaves;            /* If true we send REPLCONF GETACK. 
                                         *
                                         * 如果为真，我们将发送REPLCONF GETACK。
                                         * */
    /* Limits 
     *
     * 限制
     * */
    unsigned int maxclients;            /* Max number of simultaneous clients 
                                         *
                                         * 最大同时客户端数
                                         * */
    unsigned long long maxmemory;   /* Max number of memory bytes to use 
                                     *
                                     * 要使用的最大内存字节数
                                     * */
    int maxmemory_policy;           /* Policy for key eviction 
                                     *
                                     * 键驱逐策略
                                     * */
    int maxmemory_samples;          /* Precision of random sampling 
                                     *
                                     * 随机抽样精度
                                     * */
    int lfu_log_factor;             /* LFU logarithmic counter factor. 
                                     *
                                     * LFU对数计数器因子。
                                     * */
    int lfu_decay_time;             /* LFU counter decay factor. 
                                     *
                                     * LFU计数器衰减因子。
                                     * */
    long long proto_max_bulk_len;   /* Protocol bulk length maximum size. 
                                     *
                                     * 协议批量长度最大大小。
                                     * */
    int oom_score_adj_base;         /* Base oom_score_adj value, as observed on startup 
                                     *
                                     * 启动时观察到的基本oom_score_adj值
                                     * */
    int oom_score_adj_values[CONFIG_OOM_COUNT];   /* Linux oom_score_adj configuration 
                                                   *
                                                   * Linux oom_score_adj配置
                                                   * */
    int oom_score_adj;                            /* If true, oom_score_adj is managed 
                                                   *
                                                   * 如果为true，则管理oom_score_adj
                                                   * */
    /* Blocked clients 
     *
     * 被阻止的客户端
     * */
    unsigned int blocked_clients;   /* # of clients executing a blocking cmd.
                                     *
                                     * #的客户端执行阻止cmd。
                                     * */
    unsigned int blocked_clients_by_type[BLOCKED_NUM];
    // 要在下一次事件 lopp 前取消阻塞的所有客户端
    list *unblocked_clients; /* list of clients to unblock before next loop 
                              *
                              * 在下一个循环之前要取消阻止的客户端列表
                              * */
    list *ready_keys;        /* List of readyList structures for BLPOP & co 
                              *
                              * readyList BLPOP&co的结构
                              * */
    /* Client side caching. 
     *
     * 客户端缓存。
     * */
    unsigned int tracking_clients;  /* # of clients with tracking enabled.
                                     *
                                     * #已启用跟踪的客户端数。
                                     * */
    size_t tracking_table_max_keys; /* Max number of keys in tracking table. 
                                     *
                                     * 跟踪表中的最大键数。
                                     * */
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() 
     *
     * 排序参数-qsort_r（）仅在BSD下可用，因此我们必须将此状态设置为全局状态
     * ，以便将其传递给sortCompare（）
     * */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    /* Zip structure config, see redis.conf for more information  
     *
     * Zip结构配置，有关详细信息，请参阅redis.conf
     * */
    size_t hash_max_ziplist_entries;
    size_t hash_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    size_t hll_sparse_max_bytes;
    size_t stream_node_max_bytes;
    long long stream_node_max_entries;
    /* List parameters 
     *
     * 列出参数
     * */
    int list_max_ziplist_size;
    int list_compress_depth;
    /* time cache 
     *
     * 时间缓存
     * */
    _Atomic time_t unixtime;    /* Unix time sampled every cron cycle. 
                                 *
                                 * Unix时间在每个cron周期采样。
                                 * */
    time_t timezone;            /* Cached timezone. As set by tzset(). 
                                 *
                                 * 缓存的时区。由tzset（）设置。
                                 * */
    int daylight_active;        /* Currently in daylight saving time. 
                                 *
                                 * 目前处于夏令时。
                                 * */
    mstime_t mstime;            /* 'unixtime' in milliseconds. 
                                 *
                                 * “unixtime”（以毫秒为单位）。
                                 * */
    ustime_t ustime;            /* 'unixtime' in microseconds. 
                                 *
                                 * “unixtime”（以微秒为单位）。
                                 * */
    /* Pubsub 
     *
     * Pubsub
     * */
    // 频道
    dict *pubsub_channels;  /* Map channels to list of subscribed clients 
                             *
                             * 将频道映射到订阅的客户端列表
                             * */
    // 模式
    list *pubsub_patterns;  /* A list of pubsub_patterns 
                             *
                             * pubsub_patterns列表
                             * */
    dict *pubsub_patterns_dict;  /* A dict of pubsub_patterns 
                                  *
                                  * pubsub_patterns的dict
                                  * */
    int notify_keyspace_events; /* Events to propagate via Pub/Sub. This is an
                                   xor of NOTIFY_... flags. 
                                 *
                                 * 通过Pub/Sub传播的事件。这是NOTIFY_的xor。。。旗帜。
                                 * */
    /* Cluster 
     *
     * 簇
     * */
    int cluster_enabled;      /* Is cluster enabled? 
                               *
                               * 群集是否已启用？
                               * */
    mstime_t cluster_node_timeout; /* Cluster node timeout. 
                                    *
                                    * 群集节点超时。
                                    * */
    char *cluster_configfile; /* Cluster auto-generated config file name. 
                               *
                               * 群集自动生成的配置文件名。
                               * */
    struct clusterState *cluster;  /* State of the cluster 
                                    *
                                    * 集群的状态
                                    * */
    int cluster_migration_barrier; /* Cluster replicas migration barrier. 
                                    *
                                    * 群集复制副本迁移障碍。
                                    * */
    int cluster_slave_validity_factor; /* Slave max data age for failover. 
                                        *
                                        * 从机故障切换的最大数据使用期限。
                                        * */
    int cluster_require_full_coverage; /* If true, put the cluster down if
                                          there is at least an uncovered slot.
                                        *
                                        * 如果为true，则如果至少有一个未覆盖的插槽，则放下集群。
                                        * */
    int cluster_slave_no_failover;  /* Prevent slave from starting a failover
                                       if the master is in failure state. 
                                     *
                                     * 如果主设备处于故障状态，请阻止从节点设备启动故障转移。
                                     * */
    char *cluster_announce_ip;  /* IP address to announce on cluster bus. 
                                 *
                                 * 要在群集总线上公布的IP地址。
                                 * */
    int cluster_announce_port;     /* base port to announce on cluster bus. 
                                    *
                                    * 要在集群总线上宣布的基本端口。
                                    * */
    int cluster_announce_bus_port; /* bus port to announce on cluster bus. 
                                    *
                                    * 要在集群总线上宣布的总线端口。
                                    * */
    int cluster_module_flags;      /* Set of flags that Redis modules are able
                                      to set in order to suppress certain
                                      native Redis Cluster features. Check the
                                      REDISMODULE_CLUSTER_FLAG_*. 
                                    *
                                    * Redis模块可以设置的一组标志，以抑制某些本机Redis群集功能。检查REDI
                                    * SMODULE_CLUSTER_FLAG_*。
                                    * */
    int cluster_allow_reads_when_down; /* Are reads allowed when the cluster
                                        is down? 
                                        *
                                        * 集群关闭时是否允许读取？
                                        * */
    int cluster_config_file_lock_fd;   /* cluster config fd, will be flock 
                                        *
                                        * 集群配置fd，将是flock
                                        * */
    /* Scripting 
     *
     * 编写脚本
     * */
    lua_State *lua; /* The Lua interpreter. We use just one for all clients 
                     *
                     * Lua翻译。我们只为所有客户使用一个
                     * */
    client *lua_client;   /* The "fake client" to query Redis from Lua 
                           *
                           * 从Lua查询Redis的“假客户端”
                           * */
    client *lua_caller;   /* The client running EVAL right now, or NULL 
                           *
                           * 当前正在运行EVAL的客户端，或为NULL
                           * */
    char* lua_cur_script; /* SHA1 of the script currently running, or NULL 
                           *
                           * 当前运行的脚本的SHA1，或NULL
                           * */
    dict *lua_scripts;         /* A dictionary of SHA1 -> Lua scripts 
                                *
                                * SHA1->Lua脚本词典
                                * */
    unsigned long long lua_scripts_mem;  /* Cached scripts' memory + oh 
                                          *
                                          * 缓存脚本的内存+哦
                                          * */
    mstime_t lua_time_limit;  /* Script timeout in milliseconds 
                               *
                               * 脚本超时（毫秒）
                               * */
    mstime_t lua_time_start;  /* Start time of script, milliseconds time 
                               *
                               * 脚本的开始时间，毫秒时间
                               * */
    int lua_write_dirty;  /* True if a write command was called during the
                             execution of the current script. 
                           *
                           * 如果在执行当前脚本期间调用了写入命令，则为True。
                           * */
    int lua_random_dirty; /* True if a random command was called during the
                             execution of the current script. 
                           *
                           * 如果在执行当前脚本期间调用了随机命令，则为True。
                           * */
    int lua_replicate_commands; /* True if we are doing single commands repl. 
                                 *
                                 * 如果我们正在执行单个命令repl，则为True。
                                 * */
    int lua_multi_emitted;/* True if we already propagated MULTI. 
                           *
                           * 如果我们已经传播了MULTI，则为True。
                           * */
    int lua_repl;         /* Script replication flags for redis.set_repl(). 
                           *
                           * 为redis.set_repl（）编写复制标志的脚本。
                           * */
    int lua_timedout;     /* True if we reached the time limit for script
                             execution. 
                           *
                           * 如果我们达到了脚本执行的时间限制，则为True。
                           * */
    int lua_kill;         /* Kill the script if true. 
                           *
                           * 如果是真的，请删除脚本。
                           * */
    int lua_always_replicate_commands; /* Default replication type. 
                                        *
                                        * 默认复制类型。
                                        * */
    int lua_oom;          /* OOM detected when script start? 
                           *
                           * 脚本启动时检测到OOM？
                           * */
    /* Lazy free 
     *
     * 懒惰自由
     * */
    int lazyfree_lazy_eviction;
    int lazyfree_lazy_expire;
    int lazyfree_lazy_server_del;
    int lazyfree_lazy_user_del;
    /* Latency monitor 
     *
     * 延迟监视器
     * */
    long long latency_monitor_threshold;
    dict *latency_events;
    /* ACLs 
     *
     * ACL
     * */
    char *acl_filename;     /* ACL Users file. NULL if not configured. 
                             *
                             * ACL用户文件。如果未配置，则为NULL。
                             * */
    unsigned long acllog_max_len; /* Maximum length of the ACL LOG list. 
                                   *
                                   * ACL日志列表的最大长度。
                                   * */
    sds requirepass;        /* Remember the cleartext password set with the
                               old "requirepass" directive for backward
                               compatibility with Redis <= 5. 
                             *
                             * 请记住使用旧的“requirepass”指令设置的明文密码，以便与Redis<=
                             * 5向后兼容。
                             * */
    /* Assert & bug reporting 
     *
     * 断言错误报告（&B）
     * */
    const char *assert_failed;
    const char *assert_file;
    int assert_line;
    int bug_report_start; /* True if bug report header was already logged. 
                           *
                           * 如果已记录错误报告标头，则为True。
                           * */
    int watchdog_period;  /* Software watchdog period in ms. 0 = off 
                           *
                           * 软件看门狗周期（毫秒）0=关闭
                           * */
    /* System hardware info 
     *
     * 系统硬件信息
     * */
    size_t system_memory_size;  /* Total memory in system as reported by OS 
                                 *
                                 * OS报告的系统内存总量
                                 * */
    /* TLS Configuration 
     *
     * TLS配置
     * */
    int tls_cluster;
    int tls_replication;
    int tls_auth_clients;
    redisTLSContextConfig tls_ctx_config;
    /* cpu affinity 
     *
     * cpu亲和性
     * */
    char *server_cpulist; /* cpu affinity list of redis server main/io thread. 
                           *
                           * redis服务器主/io线程的cpu关联列表。
                           * */
    char *bio_cpulist; /* cpu affinity list of bio thread. 
                        *
                        * bio线程的cpu关联列表。
                        * */
    char *aof_rewrite_cpulist; /* cpu affinity list of aof rewrite process. 
                                *
                                * aof重写进程的cpu关联列表。
                                * */
    char *bgsave_cpulist; /* cpu affinity list of bgsave process. 
                           *
                           * bgsave进程的cpu关联列表。
                           * */
};

/*
 * 订阅模式
 */
typedef struct pubsubPattern {
    // 订阅模式的客户端
    client *client;
    // 订阅的模式
    robj *pattern;
} pubsubPattern;

#define MAX_KEYS_BUFFER 256

/* A result structure for the various getkeys function calls. It lists the
 * keys as indices to the provided argv.
 
 *
 * 各种getkeys函数调用的结果结构。它将键列为所提供argv的索引。
 * */
typedef struct {
    int keysbuf[MAX_KEYS_BUFFER];       /* Pre-allocated buffer, to save heap allocations 
                                         *
                                         * 预先分配的缓冲区，用于保存堆分配
                                         * */
    int *keys;                          /* Key indices array, points to keysbuf or heap 
                                         *
                                         * 键索引数组，指向keybuf或堆
                                         * */
    int numkeys;                        /* Number of key indices return 
                                         *
                                         * 返回的关键索引数
                                         * */
    int size;                           /* Available array size 
                                         *
                                         * 可用阵列大小
                                         * */
} getKeysResult;
#define GETKEYS_RESULT_INIT { {0}, NULL, 0, MAX_KEYS_BUFFER }

typedef void redisCommandProc(client *c);
typedef int redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
struct redisCommand {
    // 命令的名字
    char *name;
    // 命令的实现函数
    redisCommandProc *proc;
    // 命令所需的参数数量
    int arity;
    // 字符形式表示的 FLAG 值
    char *sflags;   /* Flags as string representation, one char per flag. 
                     *
                     * 标记为字符串表示，每个标记一个字符。
                     * */
    // 实际的 FLAG 值，由 sflags 计算得出
    uint64_t flags; /* The actual flags, obtained from the 'sflags' field. 
                     *
                     * 实际标志，从“sflags”字段获得。
                     * */
    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect. 
     *
     * 使用函数来确定命令行中的键参数。用于Redis群集重定向。
     * */
    // 可选，在以下三个参数不足以决定命令的 key 参数时使用
    redisGetKeysProc *getkeys_proc;
    /* What keys should be loaded in background when calling this command? 
     *
     * 调用此命令时，应该在后台加载哪些键？
     * */
    // 第一个 key 的位置
    int firstkey; /* The first argument that's a key (0 = no keys) 
                   *
                   * 第一个参数是键（0=没有键）
                   * */
    // 第二个 key 的位置
    int lastkey;  /* The last argument that's a key 
                   *
                   * 最后一个参数是键
                   * */
    // 两个 key 之间的空隔
    int keystep;  /* The step between first and last key 
                   *
                   * 第一个键和最后一个键之间的步骤
                   * */
    // 这个命令被执行所耗费的总毫秒数
    long long microseconds,
    // 这个命令被调用的总次数
    calls;
    int id;     /* Command ID. This is a progressive ID starting from 0 that
                   is assigned at runtime, and is used in order to check
                   ACLs. A connection is able to execute a given command if
                   the user associated to the connection has this command
                   bit set in the bitmap of allowed commands. 
                 *
                 * 命令ID。这是一个从0开始的渐进式ID，在运行时分配，用于检查ACL。如果与连接
                 * 相关联的用户在允许的命令的位图中设置了该命令位，则连接能够执行给定的命令。
                 * */
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

typedef struct _redisSortObject {
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;

/* Structure to hold list iteration abstraction. 
 *
 * 用于保存列表迭代抽象的结构。
 * */
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction; /* Iteration direction 
                              *
                              * 迭代方向
                              * */
    quicklistIter *iter;
} listTypeIterator;

/* Structure for an entry while iterating over a list. 
 *
 * 在列表上迭代时的节点的结构。
 * */
typedef struct {
    listTypeIterator *li;
    quicklistEntry entry; /* Entry in quicklist 
                           *
                           * 快速列表中的节点
                           * */
} listTypeEntry;

/* Structure to hold set iteration abstraction. 
 *
 * 用于保存集合迭代抽象的结构。
 * */
typedef struct {
    robj *subject;
    int encoding;
    int ii; /* intset iterator 
             *
             * 迭代程序
             * */
    dictIterator *di;
} setTypeIterator;

/* Structure to hold hash iteration abstraction. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. 
 *
 * 用于保存哈希迭代抽象的结构。请注意，散列上的迭代同时涉及字段和值。因为可能不是两
 * 者都需要，所以将指针存储在迭代器中，以避免为字段/值分配不必要的内存。
 * */
/*
 * 哈希类型对象的迭代器
 */
typedef struct {
    robj *subject;
    int encoding;

    // 用于遍历 ziplist
    unsigned char *fptr, *vptr;

    // 用于遍历字典
    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

#include "stream.h"  /* Stream data type header file. 
                      *
                      * 流数据类型头文件。
                      * */

#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2

/*-----------------------------------------------------------------------------
 * Extern declarations
 * 外部声明
 *----------------------------------------------------------------------------*/

extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType objectKeyPointerValueDictType;
extern dictType objectKeyHeapPointerValueDictType;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType clusterNodesBlackListDictType;
extern dictType dbDictType;
extern dictType shaScriptObjectDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType replScriptCacheDictType;
extern dictType keyptrDictType;
extern dictType modulesDictType;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 * 函数原型
 *----------------------------------------------------------------------------*/

/* Modules 
 *
 * 模块
 * */
void moduleInitModulesSystem(void);
int moduleLoad(const char *path, void **argv, int argc);
void moduleLoadFromQueue(void);
int moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
moduleType *moduleTypeLookupModuleByID(uint64_t id);
void moduleTypeNameByID(char *name, uint64_t moduleid);
void moduleFreeContext(struct RedisModuleCtx *ctx);
void unblockClientFromModule(client *c);
void moduleHandleBlockedClients(void);
void moduleBlockedClientTimedOut(client *c);
void moduleBlockedClientPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask);
size_t moduleCount(void);
void moduleAcquireGIL(void);
int moduleTryAcquireGIL(void);
void moduleReleaseGIL(void);
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);
void moduleCallCommandFilters(client *c);
void ModuleForkDoneHandler(int exitcode, int bysignal);
int TerminateModuleForkChild(int child_pid, int wait);
ssize_t rdbSaveModulesAux(rio *rdb, int when);
int moduleAllDatatypesHandleErrors();
sds modulesCollectInfo(sds info, const char *section, int for_crash_report, int sections);
void moduleFireServerEvent(uint64_t eid, int subid, void *data);
void processModuleLoadingProgressEvent(int is_aof);
int moduleTryServeClientBlockedOnKey(client *c, robj *key);
void moduleUnblockClient(client *c);
int moduleClientIsBlockedOnKeys(client *c);
void moduleNotifyUserChanged(client *c);

/* Utils 
 *
 * 工具
 * */
long long ustime(void);
long long mstime(void);
void getRandomHexChars(char *p, size_t len);
void getRandomBytes(unsigned char *p, size_t len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
long long redisPopcount(void *s, long count);
void redisSetProcTitle(char *title);
int redisCommunicateSystemd(const char *sd_notify_msg);
void redisSetCpuAffinity(const char *cpulist);

/* networking.c -- Networking and Client related operations 
 *
 * networking.c——网络和客户端相关操作
 * */
client *createClient(connection *conn);
void closeTimedoutClients(void);
void freeClient(client *c);
void freeClientAsync(client *c);
void resetClient(client *c);
void sendReplyToClient(connection *conn);
void *addReplyDeferredLen(client *c);
void setDeferredArrayLen(client *c, void *node, long length);
void setDeferredMapLen(client *c, void *node, long length);
void setDeferredSetLen(client *c, void *node, long length);
void setDeferredAttributeLen(client *c, void *node, long length);
void setDeferredPushLen(client *c, void *node, long length);
void processInputBuffer(client *c);
void processGopherRequest(client *c);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(connection *conn);
void addReplyNull(client *c);
void addReplyNullArray(client *c);
void addReplyBool(client *c, int b);
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext);
void addReplyProto(client *c, const char *s, size_t len);
void AddReplyFromClient(client *c, client *src);
void addReplyBulk(client *c, robj *obj);
void addReplyBulkCString(client *c, const char *s);
void addReplyBulkCBuffer(client *c, const void *p, size_t len);
void addReplyBulkLongLong(client *c, long long ll);
void addReply(client *c, robj *obj);
void addReplySds(client *c, sds s);
void addReplyBulkSds(client *c, sds s);
void addReplyErrorObject(client *c, robj *err);
void addReplyErrorSds(client *c, sds err);
void addReplyError(client *c, const char *err);
void addReplyStatus(client *c, const char *status);
void addReplyDouble(client *c, double d);
void addReplyBigNum(client *c, const char* num, size_t len);
void addReplyHumanLongDouble(client *c, long double d);
void addReplyLongLong(client *c, long long ll);
void addReplyArrayLen(client *c, long length);
void addReplyMapLen(client *c, long length);
void addReplySetLen(client *c, long length);
void addReplyAttributeLen(client *c, long length);
void addReplyPushLen(client *c, long length);
void addReplyHelp(client *c, const char **help);
void addReplySubcommandSyntaxError(client *c);
void addReplyLoadedModules(client *c);
void copyClientOutputBuffer(client *dst, client *src);
size_t sdsZmallocSize(sds s);
size_t getStringObjectSdsUsedMemory(robj *o);
void freeClientReplyValue(void *o);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
char *getClientPeerId(client *client);
sds catClientInfoString(sds s, client *client);
sds getAllClientsInfoString(int type);
void rewriteClientCommandVector(client *c, int argc, ...);
void rewriteClientCommandArgument(client *c, int i, robj *newval);
void replaceClientCommandVector(client *c, int argc, robj **argv);
unsigned long getClientOutputBufferMemoryUsage(client *c);
int freeClientsInAsyncFreeQueue(void);
void asyncCloseClientOnOutputBufferLimitReached(client *c);
int getClientType(client *c);
int getClientTypeByName(char *name);
char *getClientTypeName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
int listenToPort(int port, int *fds, int *count);
void pauseClients(mstime_t duration);
int clientsArePaused(void);
void processEventsWhileBlocked(void);
int handleClientsWithPendingWrites(void);
int handleClientsWithPendingWritesUsingThreads(void);
int handleClientsWithPendingReadsUsingThreads(void);
int stopThreadedIOIfNeeded(void);
int clientHasPendingReplies(client *c);
void unlinkClient(client *c);
int writeToClient(client *c, int handler_installed);
void linkClient(client *c);
void protectClient(client *c);
void unprotectClient(client *c);
void initThreadedIO(void);
client *lookupClientByID(uint64_t id);
int authRequired(client *c);

#ifdef __GNUC__
void addReplyErrorFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif

/* Client side caching (tracking mode) 
 *
 * 客户端缓存（跟踪模式）
 * */
void enableTracking(client *c, uint64_t redirect_to, uint64_t options, robj **prefix, size_t numprefix);
void disableTracking(client *c);
void trackingRememberKeys(client *c);
void trackingInvalidateKey(client *c, robj *keyobj);
void trackingInvalidateKeysOnFlush(int dbid);
void trackingLimitUsedSlots(void);
uint64_t trackingGetTotalItems(void);
uint64_t trackingGetTotalKeys(void);
uint64_t trackingGetTotalPrefixes(void);
void trackingBroadcastInvalidationMessages(void);

/* List data type 
 *
 * 列表数据类型
 * */
void listTypeTryConversion(robj *subject, robj *value);
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(const robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
void unblockClientWaitingData(client *c);
void popGenericCommand(client *c, int where);

/* MULTI/EXEC/WATCH... 
 *
 * MULTI/EXEC/WATCH。。。
 * */
void unwatchAllKeys(client *c);
void initClientMultiState(client *c);
void freeClientMultiState(client *c);
void queueMultiCommand(client *c);
void touchWatchedKey(redisDb *db, robj *key);
int isWatchedKeyExpired(client *c);
void touchAllWatchedKeysInDb(redisDb *emptied, redisDb *replaced_with);
void discardTransaction(client *c);
void flagTransaction(client *c);
void execCommandAbort(client *c, sds error);
void execCommandPropagateMulti(client *c);
void execCommandPropagateExec(client *c);

/* Redis object implementation 
 *
 * Redis对象实现
 * */
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
void incrRefCount(robj *o);
robj *makeObjectShared(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *createEmbeddedStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(sds s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
robj *createStreamObject(void);
robj *createModuleObject(moduleType *mt, void *value);
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int checkType(client *c, robj *o, int type);
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg);
int getDoubleFromObject(const robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);
void trimStringObjectIfNeeded(robj *o);
#define sdsEncodedObject(objptr) (objptr->encoding == OBJ_ENCODING_RAW || objptr->encoding == OBJ_ENCODING_EMBSTR)

/* Synchronous I/O with timeout 
 *
 * 带超时的同步I/O
 * */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication 
 *
 * 复制
 * */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen);
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr, int type);
void replicationCron(void);
void replicationHandleMasterDisconnection(void);
void replicationCacheMaster(client *c);
void resizeReplicationBacklog(long long newsize);
void replicationSetMaster(char *ip, int port);
void replicationUnsetMaster(void);
void refreshGoodSlavesCount(void);
void replicationScriptCacheInit(void);
void replicationScriptCacheFlush(void);
void replicationScriptCacheAdd(sds sha1);
int replicationScriptCacheExists(sds sha1);
void processClientsWaitingReplicas(void);
void unblockClientWaitingReplicas(client *c);
int replicationCountAcksByOffset(long long offset);
void replicationSendNewlineToMaster(void);
long long replicationGetSlaveOffset(void);
char *replicationGetSlaveName(client *c);
long long getPsyncInitialOffset(void);
int replicationSetupSlaveForFullResync(client *slave, long long offset);
void changeReplicationId(void);
void clearReplicationId2(void);
void chopReplicationBacklog(void);
void replicationCacheMasterUsingMyself(void);
void feedReplicationBacklog(void *ptr, size_t len);
void showLatestBacklog(void);
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void rdbPipeWriteHandlerConnRemoved(struct connection *conn);

/* Generic persistence functions 
 *
 * 通用持久性函数
 * */
void startLoadingFile(FILE* fp, char* filename, int rdbflags);
void startLoading(size_t size, int rdbflags);
void loadingProgress(off_t pos);
void stopLoading(int success);
void startSaving(int rdbflags);
void stopSaving(int success);
int allPersistenceDisabled(void);

#define DISK_ERROR_TYPE_AOF 1       /* Don't accept writes: AOF errors. 
                                     *
                                     * 不接受写入：AOF错误。
                                     * */
#define DISK_ERROR_TYPE_RDB 2       /* Don't accept writes: RDB errors. 
                                     *
                                     * 不接受写入：RDB错误。
                                     * */
#define DISK_ERROR_TYPE_NONE 0      /* No problems, we can accept writes. 
                                     *
                                     * 没有问题，我们可以接受写作。
                                     * */
int writeCommandsDeniedByDiskError(void);

/* RDB persistence 
 *
 * RDB持久性
 * */
#include "rdb.h"
void killRDBChild(void);
int bg_unlink(const char *filename);

/* AOF persistence 
 *
 * AOF持久性
 * */
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char *filename);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
void aofRewriteBufferReset(void);
unsigned long aofRewriteBufferSize(void);
ssize_t aofReadDiffFromParent(void);
void killAppendOnlyChild(void);
void restartAOFAfterSYNC();

/* Child info 
 *
 * 子进程信息
 * */
void openChildInfoPipe(void);
void closeChildInfoPipe(void);
void sendChildInfo(int process_type);
void receiveChildInfo(void);

/* Fork helpers 
 *
 * fork 助手
 * */
int redisFork(int type);
int hasActiveChildProcess();
void sendChildCOWInfo(int ptype, char *pname);

/* acl.c -- Authentication related prototypes. 
 *
 * acl.c——与身份验证相关的原型。
 * */
extern rax *Users;
extern user *DefaultUser;
void ACLInit(void);
/* Return values for ACLCheckUserCredentials(). 
 *
 * 返回ACLCheckUserCredentials（）的值。
 * */
#define ACL_OK 0
#define ACL_DENIED_CMD 1
#define ACL_DENIED_KEY 2
#define ACL_DENIED_AUTH 3 /* Only used for ACL LOG entries. 
                           *
                           * 仅用于ACL LOG节点。
                           * */
int ACLCheckUserCredentials(robj *username, robj *password);
int ACLAuthenticateUser(client *c, robj *username, robj *password);
unsigned long ACLGetCommandID(const char *cmdname);
user *ACLGetUserByName(const char *name, size_t namelen);
int ACLCheckCommandPerm(client *c, int *keyidxptr);
int ACLSetUser(user *u, const char *op, ssize_t oplen);
sds ACLDefaultUserFirstPassword(void);
uint64_t ACLGetCommandCategoryFlagByName(const char *name);
int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err);
char *ACLSetUserStringError(void);
int ACLLoadConfiguredUsers(void);
sds ACLDescribeUser(user *u);
void ACLLoadUsersAtStartup(void);
void addReplyCommandCategories(client *c, struct redisCommand *cmd);
user *ACLCreateUnlinkedUser();
void ACLFreeUserAndKillClients(user *u);
void addACLLogEntry(client *c, int reason, int keypos, sds username);

/* Sorted sets data type 
 *
 * 排序集数据类型
 * */

/* Input flags. 
 *
 * 输入标志。
 * */
#define ZADD_NONE 0
#define ZADD_INCR (1<<0)    /* Increment the score instead of setting it. 
                             *
                             * 增加分数而不是设置分数。
                             * */
#define ZADD_NX (1<<1)      /* Don't touch elements not already existing. 
                             *
                             * 不要触摸尚未存在的元素。
                             * */
#define ZADD_XX (1<<2)      /* Only touch elements already existing. 
                             *
                             * 仅触摸已存在的元素。
                             * */

/* Output flags. 
 *
 * 输出标志。
 * */
#define ZADD_NOP (1<<3)     /* Operation not performed because of conditionals.
                             *
                             * 由于条件的原因，未执行操作。
                             * */
#define ZADD_NAN (1<<4)     /* Only touch elements already existing. 
                             *
                             * 仅触摸已存在的元素。
                             * */
#define ZADD_ADDED (1<<5)   /* The element was new and was added. 
                             *
                             * 元素是新的，已添加。
                             * */
#define ZADD_UPDATED (1<<6) /* The element already existed, score updated. 
                             *
                             * 元素已存在，分数已更新。
                             * */

/* Flags only used by the ZADD command but not by zsetAdd() API: 
 *
 * 标志仅由ZADD命令使用，但不由zsetAdd（）API使用：
 * */
#define ZADD_CH (1<<16)      /* Return num of elements added or updated. 
                              *
                              * 返回添加或更新的元素数。
                              * */

/* Struct to hold an inclusive/exclusive range spec by score comparison. 
 *
 * 通过分数比较来保存包含/排除范围规范的结构。
 * */
/*
 * 用于保存范围值的结构
 */
typedef struct {
    double min, max;
    // min 和 max 是否包括在内？
    int minex, maxex; /* are min or max exclusive? 
                       *
                       * 最小值还是最大值是独占的？
                       * */
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. 
 *
 * 通过字典比较保存包含/排除范围规范的结构。
 * */
typedef struct {
    sds min, max;     /* May be set to shared.(minstring|maxstring) */
    int minex, maxex; /* are min or max exclusive? 
                       *
                       * 最小值还是最大值是独占的？
                       * */
} zlexrangespec;

zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);
int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);
unsigned long zsetLength(const robj *zobj);
void zsetConvert(robj *zobj, int encoding);
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen);
int zsetScore(robj *zobj, sds member, double *score);
unsigned long zslGetRank(zskiplist *zsl, double score, sds o);
int zsetAdd(robj *zobj, double score, sds ele, int *flags, double *newscore);
long zsetRank(robj *zobj, sds ele, int reverse);
int zsetDel(robj *zobj, sds ele);
void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, robj *countarg);
sds ziplistGetObject(unsigned char *sptr);
int zslValueGteMin(double value, zrangespec *spec);
int zslValueLteMax(double value, zrangespec *spec);
void zslFreeLexRange(zlexrangespec *spec);
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range);
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range);
int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);
int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);

/* Core functions 
 *
 * 核心功能
 * */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level);
size_t freeMemoryGetNotCountedMemory();
int freeMemoryIfNeeded(void);
int freeMemoryIfNeededAndSafe(void);
int processCommand(client *c);
int processPendingCommandsAndResetClient(client *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(const char *s);
struct redisCommand *lookupCommandOrOriginal(sds name);
void call(client *c, int flags);
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags);
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int target);
void redisOpArrayInit(redisOpArray *oa);
void redisOpArrayFree(redisOpArray *oa);
void forceCommandPropagation(client *c, int flags);
void preventCommandPropagation(client *c);
void preventCommandAOF(client *c);
void preventCommandReplication(client *c);
int prepareForShutdown(int flags);
#ifdef __GNUC__
void serverLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void serverLog(int level, const char *fmt, ...);
#endif
void serverLogRaw(int level, const char *msg);
void serverLogFromHandler(int level, const char *msg);
void usage(void);
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void populateCommandTable(void);
void resetCommandTableStats(void);
void adjustOpenFilesLimit(void);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(int update_daylight_info);
void resetServerStats(void);
void activeDefragCycle(void);
unsigned int getLRUClock(void);
unsigned int LRU_CLOCK(void);
const char *evictPolicyToString(void);
struct redisMemOverhead *getMemoryOverheadData(void);
void freeMemoryOverheadData(struct redisMemOverhead *mh);
void checkChildrenDone(void);
int setOOMScoreAdj(int process_class);

#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1<<0)     /* Do proper shutdown. 
                                              *
                                              * 正确关闭。
                                              * */
#define RESTART_SERVER_CONFIG_REWRITE (1<<1) /* CONFIG REWRITE before restart.
                                              *
                                              * 重新启动前配置重写。
                                              * */
int restartServer(int flags, mstime_t delay);

/* Set data type 
 *
 * 设置数据类型
 * */
robj *setTypeCreate(sds value);
int setTypeAdd(robj *subject, sds value);
int setTypeRemove(robj *subject, sds value);
int setTypeIsMember(robj *subject, sds value);
setTypeIterator *setTypeInitIterator(robj *subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele);
sds setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);
unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);
unsigned long setTypeSize(const robj *subject);
void setTypeConvert(robj *subject, int enc);

/* Hash data type 
 *
 * 哈希数据类型
 * */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0

void hashTypeConvert(robj *o, int enc);
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
int hashTypeExists(robj *o, sds key);
int hashTypeDelete(robj *o, sds key);
unsigned long hashTypeLength(const robj *o);
hashTypeIterator *hashTypeInitIterator(robj *subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll);
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what);
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll);
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(client *c, robj *key);
robj *hashTypeGetValueObject(robj *o, sds field);
int hashTypeSet(robj *o, sds field, sds value, int flags);

/* Pub / Sub 
 *
 * 发布/订阅
 * */
int pubsubUnsubscribeAllChannels(client *c, int notify);
int pubsubUnsubscribeAllPatterns(client *c, int notify);
void freePubsubPattern(void *p);
int listMatchPubsubPattern(void *a, void *b);
int pubsubPublishMessage(robj *channel, robj *message);
void addReplyPubsubMessage(client *c, robj *channel, robj *msg);

/* Keyspace events notification 
 *
 * 键空间事件通知
 * */
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
int keyspaceEventsStringToFlags(char *classes);
sds keyspaceEventsFlagsToString(int flags);

/* Configuration 
 *
 * 配置
 * */
void loadServerConfig(char *filename, char *options);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams(void);
struct rewriteConfigState; /* Forward declaration to export API. 
                            *
                            * 转发声明以导出API。
                            * */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force);
int rewriteConfig(char *path, int force_all);
void initConfigValues();

/* db.c -- Keyspace access API 
 *
 * db.c--键空间访问API
 * */
int removeExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key, int lazy);
int keyIsExpired(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key);
long long getExpire(redisDb *db, robj *key);
void setExpire(client *c, redisDb *db, robj *key, long long when);
int checkAlreadyExpired(long long when);
robj *lookupKey(redisDb *db, robj *key, int flags);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags);
robj *objectCommandLookup(client *c, robj *key);
robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply);
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier);
#define LOOKUP_NONE 0
#define LOOKUP_NOTOUCH (1<<0)
#define LOOKUP_NONOTIFY (1<<1)
void dbAdd(redisDb *db, robj *key, robj *val);
int dbAddRDBLoad(redisDb *db, sds key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void genericSetKey(client *c, redisDb *db, robj *key, robj *val, int keepttl, int signal);
void setKey(client *c, redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbSyncDelete(redisDb *db, robj *key);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);

#define EMPTYDB_NO_FLAGS 0      /* No flags. 
                                 *
                                 * 没有标志。
                                 * */
#define EMPTYDB_ASYNC (1<<0)    /* Reclaim memory in another thread. 
                                 *
                                 * 回收另一个线程中的内存。
                                 * */
long long emptyDb(int dbnum, int flags, void(callback)(void*));
long long emptyDbStructure(redisDb *dbarray, int dbnum, int async, void(callback)(void*));
void flushAllDataAndResetRDB(int flags);
long long dbTotalServerKeyCount();
dbBackup *backupDb(void);
void restoreDbBackup(dbBackup *buckup);
void discardDbBackup(dbBackup *buckup, int flags, void(callback)(void*));


int selectDb(client *c, int id);
void signalModifiedKey(client *c, redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(client *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);
void slotToKeyAdd(sds key);
void slotToKeyDel(sds key);
int dbAsyncDelete(redisDb *db, robj *key);
void emptyDbAsync(redisDb *db);
void slotToKeyFlush(int async);
size_t lazyfreeGetPendingObjectsCount(void);
void freeObjAsync(robj *obj);
void freeSlotsToKeysMapAsync(rax *rt);
void freeSlotsToKeysMap(rax *rt, int async);


/* API to get key arguments from commands 
 *
 * API从命令中获取关键参数
 * */
int *getKeysPrepareResult(getKeysResult *result, int numkeys);
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
void getKeysFreeResult(getKeysResult *result);
int zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result);
int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int memoryGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int lcsGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

/* Cluster 
 *
 * 集群
 * */
void clusterInit(void);
unsigned short crc16(const char *buf, int len);
unsigned int keyHashSlot(char *key, int keylen);
void clusterCron(void);
void clusterPropagatePublish(robj *channel, robj *message);
void migrateCloseTimedoutSockets(void);
void clusterBeforeSleep(void);
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, unsigned char *payload, uint32_t len);

/* Sentinel 
 *
 * 哨兵
 * */
void initSentinelConfig(void);
void initSentinel(void);
void sentinelTimer(void);
char *sentinelHandleConfiguration(char **argv, int argc);
void sentinelIsRunning(void);

/* redis-check-rdb & aof 
 *
 * redis检查rdb&aof
 * */
int redis_check_rdb(char *rdbfilename, FILE *fp);
int redis_check_rdb_main(int argc, char **argv, FILE *fp);
int redis_check_aof_main(int argc, char **argv);

/* Scripting 
 *
 * 编写脚本
 * */
void scriptingInit(int setup);
int ldbRemoveChild(pid_t pid);
void ldbKillForkedSessions(void);
int ldbPendingChildren(void);
sds luaCreateFunction(client *c, lua_State *lua, robj *body);

/* Blocked clients 
 *
 * 被阻止的客户端
 * */
void processUnblockedClients(void);
void blockClient(client *c, int btype);
void unblockClient(client *c);
void queueClientForReprocessing(client *c);
void replyToBlockedClientTimedOut(client *c);
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit);
void disconnectAllBlockedClients(void);
void handleClientsBlockedOnKeys(void);
void signalKeyAsReady(redisDb *db, robj *key);
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout, robj *target, streamID *ids);

/* timeout.c -- Blocked clients timeout and connections timeout. 
 *
 * timeout.c——阻塞的客户端超时和连接超时。
 * */
void addClientToTimeoutTable(client *c);
void removeClientFromTimeoutTable(client *c);
void handleBlockedClientsTimeout(void);
int clientsCronHandleTimeout(client *c, mstime_t now_ms);

/* expire.c -- Handling of expired keys 
 *
 * expire.c--过期键的处理
 * */
void activeExpireCycle(int type);
void expireSlaveKeys(void);
void rememberSlaveKeyWithExpire(redisDb *db, robj *key);
void flushSlaveKeysWithExpireList(void);
size_t getSlaveKeyWithExpireCount(void);

/* evict.c -- maxmemory handling and LRU eviction. 
 *
 * revest.c——最大内存处理和LRU逐出。
 * */
void evictionPoolAlloc(void);
#define LFU_INIT_VAL 5
unsigned long LFUGetTimeInMinutes(void);
uint8_t LFULogIncr(uint8_t value);
unsigned long LFUDecrAndReturn(robj *o);

/* Keys hashing / comparison functions for dict.c hash tables. 
 *
 * dict.c哈希表的键哈希/比较函数。
 * */
uint64_t dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
void dictSdsDestructor(void *privdata, void *val);

/* Git SHA1 */
char *redisGitSHA1(void);
char *redisGitDirty(void);
uint64_t redisBuildId(void);
char *redisBuildIdString(void);

/* Commands prototypes 
 *
 * 命令原型
 * */
void authCommand(client *c);
void pingCommand(client *c);
void echoCommand(client *c);
void commandCommand(client *c);
void setCommand(client *c);
void setnxCommand(client *c);
void setexCommand(client *c);
void psetexCommand(client *c);
void getCommand(client *c);
void delCommand(client *c);
void unlinkCommand(client *c);
void existsCommand(client *c);
void setbitCommand(client *c);
void getbitCommand(client *c);
void bitfieldCommand(client *c);
void bitfieldroCommand(client *c);
void setrangeCommand(client *c);
void getrangeCommand(client *c);
void incrCommand(client *c);
void decrCommand(client *c);
void incrbyCommand(client *c);
void decrbyCommand(client *c);
void incrbyfloatCommand(client *c);
void selectCommand(client *c);
void swapdbCommand(client *c);
void randomkeyCommand(client *c);
void keysCommand(client *c);
void scanCommand(client *c);
void dbsizeCommand(client *c);
void lastsaveCommand(client *c);
void saveCommand(client *c);
void bgsaveCommand(client *c);
void bgrewriteaofCommand(client *c);
void shutdownCommand(client *c);
void moveCommand(client *c);
void renameCommand(client *c);
void renamenxCommand(client *c);
void lpushCommand(client *c);
void rpushCommand(client *c);
void lpushxCommand(client *c);
void rpushxCommand(client *c);
void linsertCommand(client *c);
void lpopCommand(client *c);
void rpopCommand(client *c);
void llenCommand(client *c);
void lindexCommand(client *c);
void lrangeCommand(client *c);
void ltrimCommand(client *c);
void typeCommand(client *c);
void lsetCommand(client *c);
void saddCommand(client *c);
void sremCommand(client *c);
void smoveCommand(client *c);
void sismemberCommand(client *c);
void scardCommand(client *c);
void spopCommand(client *c);
void srandmemberCommand(client *c);
void sinterCommand(client *c);
void sinterstoreCommand(client *c);
void sunionCommand(client *c);
void sunionstoreCommand(client *c);
void sdiffCommand(client *c);
void sdiffstoreCommand(client *c);
void sscanCommand(client *c);
void syncCommand(client *c);
void flushdbCommand(client *c);
void flushallCommand(client *c);
void sortCommand(client *c);
void lremCommand(client *c);
void lposCommand(client *c);
void rpoplpushCommand(client *c);
void infoCommand(client *c);
void mgetCommand(client *c);
void monitorCommand(client *c);
void expireCommand(client *c);
void expireatCommand(client *c);
void pexpireCommand(client *c);
void pexpireatCommand(client *c);
void getsetCommand(client *c);
void ttlCommand(client *c);
void touchCommand(client *c);
void pttlCommand(client *c);
void persistCommand(client *c);
void replicaofCommand(client *c);
void roleCommand(client *c);
void debugCommand(client *c);
void msetCommand(client *c);
void msetnxCommand(client *c);
void zaddCommand(client *c);
void zincrbyCommand(client *c);
void zrangeCommand(client *c);
void zrangebyscoreCommand(client *c);
void zrevrangebyscoreCommand(client *c);
void zrangebylexCommand(client *c);
void zrevrangebylexCommand(client *c);
void zcountCommand(client *c);
void zlexcountCommand(client *c);
void zrevrangeCommand(client *c);
void zcardCommand(client *c);
void zremCommand(client *c);
void zscoreCommand(client *c);
void zremrangebyscoreCommand(client *c);
void zremrangebylexCommand(client *c);
void zpopminCommand(client *c);
void zpopmaxCommand(client *c);
void bzpopminCommand(client *c);
void bzpopmaxCommand(client *c);
void multiCommand(client *c);
void execCommand(client *c);
void discardCommand(client *c);
void blpopCommand(client *c);
void brpopCommand(client *c);
void brpoplpushCommand(client *c);
void appendCommand(client *c);
void strlenCommand(client *c);
void zrankCommand(client *c);
void zrevrankCommand(client *c);
void hsetCommand(client *c);
void hsetnxCommand(client *c);
void hgetCommand(client *c);
void hmsetCommand(client *c);
void hmgetCommand(client *c);
void hdelCommand(client *c);
void hlenCommand(client *c);
void hstrlenCommand(client *c);
void zremrangebyrankCommand(client *c);
void zunionstoreCommand(client *c);
void zinterstoreCommand(client *c);
void zscanCommand(client *c);
void hkeysCommand(client *c);
void hvalsCommand(client *c);
void hgetallCommand(client *c);
void hexistsCommand(client *c);
void hscanCommand(client *c);
void configCommand(client *c);
void hincrbyCommand(client *c);
void hincrbyfloatCommand(client *c);
void subscribeCommand(client *c);
void unsubscribeCommand(client *c);
void psubscribeCommand(client *c);
void punsubscribeCommand(client *c);
void publishCommand(client *c);
void pubsubCommand(client *c);
void watchCommand(client *c);
void unwatchCommand(client *c);
void clusterCommand(client *c);
void restoreCommand(client *c);
void migrateCommand(client *c);
void askingCommand(client *c);
void readonlyCommand(client *c);
void readwriteCommand(client *c);
void dumpCommand(client *c);
void objectCommand(client *c);
void memoryCommand(client *c);
void clientCommand(client *c);
void helloCommand(client *c);
void evalCommand(client *c);
void evalShaCommand(client *c);
void scriptCommand(client *c);
void timeCommand(client *c);
void bitopCommand(client *c);
void bitcountCommand(client *c);
void bitposCommand(client *c);
void replconfCommand(client *c);
void waitCommand(client *c);
void geoencodeCommand(client *c);
void geodecodeCommand(client *c);
void georadiusbymemberCommand(client *c);
void georadiusbymemberroCommand(client *c);
void georadiusCommand(client *c);
void georadiusroCommand(client *c);
void geoaddCommand(client *c);
void geohashCommand(client *c);
void geoposCommand(client *c);
void geodistCommand(client *c);
void pfselftestCommand(client *c);
void pfaddCommand(client *c);
void pfcountCommand(client *c);
void pfmergeCommand(client *c);
void pfdebugCommand(client *c);
void latencyCommand(client *c);
void moduleCommand(client *c);
void securityWarningCommand(client *c);
void xaddCommand(client *c);
void xrangeCommand(client *c);
void xrevrangeCommand(client *c);
void xlenCommand(client *c);
void xreadCommand(client *c);
void xgroupCommand(client *c);
void xsetidCommand(client *c);
void xackCommand(client *c);
void xpendingCommand(client *c);
void xclaimCommand(client *c);
void xinfoCommand(client *c);
void xdelCommand(client *c);
void xtrimCommand(client *c);
void lolwutCommand(client *c);
void aclCommand(client *c);
void stralgoCommand(client *c);

#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif

/* Debugging stuff 
 *
 * 调试内容
 * */
void _serverAssertWithInfo(const client *c, const robj *o, const char *estr, const char *file, int line);
void _serverAssert(const char *estr, const char *file, int line);
#ifdef __GNUC__
void _serverPanic(const char *file, int line, const char *msg, ...)
    __attribute__ ((format (printf, 3, 4)));
#else
void _serverPanic(const char *file, int line, const char *msg, ...);
#endif
void bugReportStart(void);
void serverLogObjectDebugInfo(const robj *o);
void sigsegvHandler(int sig, siginfo_t *info, void *secret);
sds genRedisInfoString(const char *section);
sds genModulesInfoString(sds info);
void enableWatchdog(int period);
void disableWatchdog(void);
void watchdogScheduleSignal(int period);
void serverLogHexDump(int level, char *descr, void *value, size_t len);
int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);
void mixDigest(unsigned char *digest, void *ptr, size_t len);
void xorDigest(unsigned char *digest, void *ptr, size_t len);
int populateCommandTableParseFlags(struct redisCommand *c, char *strflags);
void killIOThreads(void);
void killThreads(void);
void makeThreadKillable(void);

/* TLS stuff 
 *
 * TLS内容
 * */
void tlsInit(void);
int tlsConfigure(redisTLSContextConfig *ctx_config);

#define redisDebug(fmt, ...) \
    printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() \
    printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

int iAmMaster(void);

#endif
