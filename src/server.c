/*
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
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"
#include "atomicvar.h"
#include "mt19937-64.h"

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

/* Our shared "common" objects 
 *
 * 我们共享的“共同”对象*/
struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. 
 *
 * 实际用作常量的全局变量。以下双值用于磁盘上的双值序列化，并在运行时进行初始化，以
 * 避免奇怪的编译器优化。*/

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars 
 *
 * 全球VARS*/
// 服务器状态
struct redisServer server; /* Server global state 
                            *
                            * 服务器全局状态*/

/* Our command table.
 * 我们的命令表
 *
 * Every entry is composed of the following fields:
 * 每个条目都由以下字段组成
 *
 * name:        A string representing the command name.
 *              命令的名称
 *
 * function:    Pointer to the C function implementing the command.
 *              命令的实现函数
 *
 * arity:       Number of arguments, it is possible to use -N to say >= N
 *              参数的数量，可以用 -N 表示 >= N
 *
 * sflags:      Command flags as string. See below for a table of flags.
 *              字符串形式的 FLAG ，用来计算下面的 flags 属性
 *
 * flags:       Flags as bitmask. Computed by Redis using the 'sflags' field.
 *              位掩码形式的 FLAG ，由 sflags 字符串计算得出
 *
 * get_keys_proc: An optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 *                 一个可选的函数，用于从命令中取出 key 参数。
 *                只在以下三个参数不足以表示 key 参数时使用。
 *
 * first_key_index: First argument that is a key
 *                   第一个是 key 的参数
 *
 * last_key_index: Last argument that is a key
 *                 最后一个是 key 的参数
 *
 * key_step:    Step to get all the keys from first to last argument.
 *              For instance in MSET the step is two since arguments
 *              are key,val,key,val,...
 *              从 first 参数和 last 参数间，获取所有 key 的步数（step）
 *              比如说， MSET 命令的格式为 MSET key value [key value ...]
 *               它的 step 就为 2
 *
 * microseconds: Microseconds of total execution time for this command.
 *               执行这个命令耗费的总微秒数
 *
 * calls:       Total number of calls of this command.
 *              命令被执行的总次数
 *
 * id:          Command bit identifier for ACLs or other goals.
 *              ACL 或其他目标的命令位标识符
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 * 标志、微秒和调用字段由 Redis 计算，应始终设置为零。
 *
 * Command flags are expressed using space separated strings, that are turned
 * into actual flags by the populateCommandTable() function.
 * 命令标志使用空格分隔的字符串表示，这些字符串由 populateCommandTable（） 函数转换为实际标志。
 *
 * 命令的 FLAG 由 SFLAG 域设置，之后 populateCommandTable() 从 SFLAG 中计算出
 * 真正的 FLAG 。
 *
 * This is the meaning of the flags:
 * 这是标志的含义：
 *
 * write:       Write command (may modify the key space).
 *              写入命令，可能会修改 key space
 *
 * read-only:   All the non special commands just reading from keys without
 *              changing the content, or returning other information like
 *              the TIME command. Special commands such administrative commands
 *              or transaction related commands (multi, exec, discard, ...)
 *              are not flagged as read-only commands, since they affect the
 *              server or the connection in other ways.
 *              所有非特殊命令只是从键读取而不更改内容，或返回其他信息，如 TIME 命令。
 *              特殊命令（如管理命令或与事务相关的命令（multi、exec、discard等）不会标记为只读命令，
 *              因为它们以其他方式影响服务器或连接。
 *
 *              读命令，不修改 key space
 *
 * use-memory:  May increase memory usage once called. Don't allow if out
 *              of memory.
 *              可能会占用大量内存的命令，调用时对内存占用进行检查
 *
 * admin:       Administrative command, like SAVE or SHUTDOWN.
 *              管理员使用的命令
 *
 * pub-sub:     Pub/Sub related command.
 *              发送/订阅相关的命令
 *
 * no-script:   Command not allowed in scripts.
 *              脚本中不允许使用命令。
 *
 * random:      Random command. Command is not deterministic, that is, the same
 *              command with the same arguments, with the same key space, may
 *              have different results. For instance SPOP and RANDOMKEY are
 *              two random commands.
 *              随机命令。命令不是确定性的，也就是说，具有相同参数、相同键空间的相同命令可能会有不同的结果。
 *              例如，SPOP和RANDOMKEY是两个随机命令。
 *
 *              随机命令，对于同样数据集的同一个命令调用，得出的结果可能是不相同的。
 *
 * to-sort:     Sort command output array if called from script, so that the
 *              output is deterministic. When this flag is used (not always
 *              possible), then the "random" flag is not needed.
 *              如果从脚本调用，则对命令输出数组进行排序，以便输出具有确定性。
 *              当使用此标志（并非总是可能）时，则不需要“随机”标志。
 *
 *              如果命令在脚本中执行，那么对输出进行排序，从而让输出变得确定起来。
 *
 * ok-loading:  Allow the command while loading the database.
 *              允许在载入数据库时执行的命令
 *
 * ok-stale:    Allow the command while a slave has stale data but is not
 *              allowed to serve this data. Normally no command is accepted
 *              in this condition but just a few.
 *              当从属服务器具有过时数据时允许该命令，但不允许提供此数据。
 *              通常在这种情况下不接受任何命令，只接受少数命令。
 *
 * no-monitor:  Do not automatically propagate the command on MONITOR.
 *              不要在监视器上自动传播该命令。
 *
 * no-slowlog:  Do not automatically propagate the command to the slowlog.
 *               不要自动将命令传播到慢日志。
 *
 * cluster-asking: Perform an implicit ASKING for this command, so the
 *              command will be accepted in cluster mode if the slot is marked
 *              as 'importing'.
 *              对此命令执行隐式 ASK，因此如果插槽标记为“正在导入”，则该命令将在群集模式下被接受
 *
 * fast:        Fast command: O(1) or O(log(N)) command that should never
 *              delay its execution as long as the kernel scheduler is giving
 *              us time. Note that commands that may trigger a DEL as a side
 *              effect (like SET) are not fast commands.
 *              快速命令：O（1） 或 O（log（N）） 命令，只要内核调度程序给我们时间，它就永远不会延迟它的执行。
 *              请注意，可能触发 DEL 作为副作用的命令（如 SET）不是快速命令。
 *
 * The following additional flags are only used in order to put commands
 * in a specific ACL category. Commands can have multiple ACL categories.
 *
 * 以下附加标志仅用于将命令放入特定 ACL 类别。命令可以有多个 ACL 类别。
 *
 * @keyspace, @read, @write, @set, @sortedset, @list, @hash, @string, @bitmap,
 * @hyperloglog, @stream, @admin, @fast, @slow, @pubsub, @blocking, @dangerous,
 * @connection, @transaction, @scripting, @geo.
 *
 * Note that:
 *
 * 1) The read-only flag implies the @read ACL category.
 * 2) The write flag implies the @write ACL category.
 * 3) The fast flag implies the @fast ACL category.
 * 4) The admin flag implies the @admin and @dangerous ACL category.
 * 5) The pub-sub flag implies the @pubsub ACL category.
 * 6) The lack of fast flag implies the @slow ACL category.
 * 7) The non obvious "keyspace" category includes the commands
 *    that interact with keys without having anything to do with
 *    specific data structures, such as: DEL, RENAME, MOVE, SELECT,
 *    TYPE, EXPIRE*, PEXPIRE*, TTL, PTTL, ...
 *
 * 1） 只读标志表示 ACL 类别@read。
 * 2） 写入标志表示 ACL 类别@write。
 * 3） 快速标志表示 ACL 类别@fast。
 * 4） 管理员标志表示 @admin 和 @dangerous ACL 类别。
 * 5） 发布-订阅标志表示 @pubsub ACL 类别。
 * 6）缺少快速标志意味着@slow ACL类别。
 * 7）不明显的“键空间”类别包括命令 与键交互，与 特定的数据结构，如：DEL、重命名、移动、选择、 类型， 过期*， 过期*，TTL PTTL，...
 */

struct redisCommand redisCommandTable[] = {
    {"module",moduleCommand,-2,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"get",getCommand,2,
     "read-only fast @string",
     0,NULL,1,1,1,0,0,0},

    /* Note that we can't flag set as fast, since it may perform an
     * implicit DEL of a large key. 
     *
     * 请注意，我们不能这么快地标记set，因为它可能会执行大密钥的隐式DEL。*/
    {"set",setCommand,-3,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"setnx",setnxCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"setex",setexCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"psetex",psetexCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"append",appendCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"strlen",strlenCommand,2,
     "read-only fast @string",
     0,NULL,1,1,1,0,0,0},

    {"del",delCommand,-2,
     "write @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"unlink",unlinkCommand,-2,
     "write fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"exists",existsCommand,-2,
     "read-only fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"setbit",setbitCommand,4,
     "write use-memory @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"getbit",getbitCommand,3,
     "read-only fast @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitfield",bitfieldCommand,-2,
     "write use-memory @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitfield_ro",bitfieldroCommand,-2,
     "read-only fast @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"setrange",setrangeCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"getrange",getrangeCommand,4,
     "read-only @string",
     0,NULL,1,1,1,0,0,0},

    {"substr",getrangeCommand,4,
     "read-only @string",
     0,NULL,1,1,1,0,0,0},

    {"incr",incrCommand,2,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"decr",decrCommand,2,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"mget",mgetCommand,-2,
     "read-only fast @string",
     0,NULL,1,-1,1,0,0,0},

    {"rpush",rpushCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpush",lpushCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"rpushx",rpushxCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpushx",lpushxCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"linsert",linsertCommand,5,
     "write use-memory @list",
     0,NULL,1,1,1,0,0,0},

    {"rpop",rpopCommand,2,
     "write fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpop",lpopCommand,2,
     "write fast @list",
     0,NULL,1,1,1,0,0,0},

    {"brpop",brpopCommand,-3,
     "write no-script @list @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"brpoplpush",brpoplpushCommand,4,
     "write use-memory no-script @list @blocking",
     0,NULL,1,2,1,0,0,0},

    {"blpop",blpopCommand,-3,
     "write no-script @list @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"llen",llenCommand,2,
     "read-only fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lindex",lindexCommand,3,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"lset",lsetCommand,4,
     "write use-memory @list",
     0,NULL,1,1,1,0,0,0},

    {"lrange",lrangeCommand,4,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"ltrim",ltrimCommand,4,
     "write @list",
     0,NULL,1,1,1,0,0,0},

    {"lpos",lposCommand,-3,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"lrem",lremCommand,4,
     "write @list",
     0,NULL,1,1,1,0,0,0},

    {"rpoplpush",rpoplpushCommand,3,
     "write use-memory @list",
     0,NULL,1,2,1,0,0,0},

    {"sadd",saddCommand,-3,
     "write use-memory fast @set",
     0,NULL,1,1,1,0,0,0},

    {"srem",sremCommand,-3,
     "write fast @set",
     0,NULL,1,1,1,0,0,0},

    {"smove",smoveCommand,4,
     "write fast @set",
     0,NULL,1,2,1,0,0,0},

    {"sismember",sismemberCommand,3,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"scard",scardCommand,2,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"spop",spopCommand,-2,
     "write random fast @set",
     0,NULL,1,1,1,0,0,0},

    {"srandmember",srandmemberCommand,-2,
     "read-only random @set",
     0,NULL,1,1,1,0,0,0},

    {"sinter",sinterCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sinterstore",sinterstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"sunion",sunionCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sunionstore",sunionstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"sdiff",sdiffCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sdiffstore",sdiffstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"smembers",sinterCommand,2,
     "read-only to-sort @set",
     0,NULL,1,1,1,0,0,0},

    {"sscan",sscanCommand,-3,
     "read-only random @set",
     0,NULL,1,1,1,0,0,0},

    {"zadd",zaddCommand,-4,
     "write use-memory fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zincrby",zincrbyCommand,4,
     "write use-memory fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrem",zremCommand,-3,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebyscore",zremrangebyscoreCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebyrank",zremrangebyrankCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebylex",zremrangebylexCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zunionstore",zunionstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterGetKeys,1,1,1,0,0,0},

    {"zinterstore",zinterstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterGetKeys,1,1,1,0,0,0},

    {"zrange",zrangeCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrangebyscore",zrangebyscoreCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrangebyscore",zrevrangebyscoreCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrangebylex",zrangebylexCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrangebylex",zrevrangebylexCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zcount",zcountCommand,4,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zlexcount",zlexcountCommand,4,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrange",zrevrangeCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zcard",zcardCommand,2,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zscore",zscoreCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrank",zrankCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrank",zrevrankCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zscan",zscanCommand,-3,
     "read-only random @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zpopmin",zpopminCommand,-2,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zpopmax",zpopmaxCommand,-2,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"bzpopmin",bzpopminCommand,-3,
     "write no-script fast @sortedset @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"bzpopmax",bzpopmaxCommand,-3,
     "write no-script fast @sortedset @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"hset",hsetCommand,-4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hsetnx",hsetnxCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hget",hgetCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hmset",hsetCommand,-4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hmget",hmgetCommand,-3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hincrby",hincrbyCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hincrbyfloat",hincrbyfloatCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hdel",hdelCommand,-3,
     "write fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hlen",hlenCommand,2,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hstrlen",hstrlenCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hkeys",hkeysCommand,2,
     "read-only to-sort @hash",
     0,NULL,1,1,1,0,0,0},

    {"hvals",hvalsCommand,2,
     "read-only to-sort @hash",
     0,NULL,1,1,1,0,0,0},

    {"hgetall",hgetallCommand,2,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"hexists",hexistsCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hscan",hscanCommand,-3,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"incrby",incrbyCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"decrby",decrbyCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"incrbyfloat",incrbyfloatCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"getset",getsetCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"mset",msetCommand,-3,
     "write use-memory @string",
     0,NULL,1,-1,2,0,0,0},

    {"msetnx",msetnxCommand,-3,
     "write use-memory @string",
     0,NULL,1,-1,2,0,0,0},

    {"randomkey",randomkeyCommand,1,
     "read-only random @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"select",selectCommand,2,
     "ok-loading fast ok-stale @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"swapdb",swapdbCommand,3,
     "write fast @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"move",moveCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    /* Like for SET, we can't mark rename as a fast command because
     * overwriting the target key may result in an implicit slow DEL. 
     *
     * 与SET一样，我们不能将rename标记为快速命令，因为覆盖目标键可能会导致隐含
     * 的慢速DEL。*/
    {"rename",renameCommand,3,
     "write @keyspace",
     0,NULL,1,2,1,0,0,0},

    {"renamenx",renamenxCommand,3,
     "write fast @keyspace",
     0,NULL,1,2,1,0,0,0},

    {"expire",expireCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"expireat",expireatCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"pexpire",pexpireCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"pexpireat",pexpireatCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"keys",keysCommand,2,
     "read-only to-sort @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"scan",scanCommand,-2,
     "read-only random @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"dbsize",dbsizeCommand,1,
     "read-only fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"auth",authCommand,-2,
     "no-auth no-script ok-loading ok-stale fast no-monitor no-slowlog @connection",
     0,NULL,0,0,0,0,0,0},

    /* We don't allow PING during loading since in Redis PING is used as
     * failure detection, and a loading server is considered to be
     * not available. 
     *
     * 我们不允许在加载期间使用PING，因为在Redis中，PING被用作故障检测，并
     * 且加载服务器被认为不可用。*/
    {"ping",pingCommand,-1,
     "ok-stale fast @connection",
     0,NULL,0,0,0,0,0,0},

    {"echo",echoCommand,2,
     "read-only fast @connection",
     0,NULL,0,0,0,0,0,0},

    {"save",saveCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"bgsave",bgsaveCommand,-1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"bgrewriteaof",bgrewriteaofCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"shutdown",shutdownCommand,-1,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"lastsave",lastsaveCommand,1,
     "read-only random fast ok-loading ok-stale @admin @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"type",typeCommand,2,
     "read-only fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"multi",multiCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"exec",execCommand,1,
     "no-script no-monitor no-slowlog ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"discard",discardCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"sync",syncCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"psync",syncCommand,3,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"replconf",replconfCommand,-1,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"flushdb",flushdbCommand,-1,
     "write @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"flushall",flushallCommand,-1,
     "write @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"sort",sortCommand,-2,
     "write use-memory @list @set @sortedset @dangerous",
     0,sortGetKeys,1,1,1,0,0,0},

    {"info",infoCommand,-1,
     "ok-loading ok-stale random @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"monitor",monitorCommand,1,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"ttl",ttlCommand,2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"touch",touchCommand,-2,
     "read-only fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"pttl",pttlCommand,2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"persist",persistCommand,2,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"slaveof",replicaofCommand,3,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"replicaof",replicaofCommand,3,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"role",roleCommand,1,
     "ok-loading ok-stale no-script fast read-only @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"debug",debugCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"config",configCommand,-2,
     "admin ok-loading ok-stale no-script",
     0,NULL,0,0,0,0,0,0},

    {"subscribe",subscribeCommand,-2,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"unsubscribe",unsubscribeCommand,-1,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"psubscribe",psubscribeCommand,-2,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"punsubscribe",punsubscribeCommand,-1,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"publish",publishCommand,3,
     "pub-sub ok-loading ok-stale fast",
     0,NULL,0,0,0,0,0,0},

    {"pubsub",pubsubCommand,-2,
     "pub-sub ok-loading ok-stale random",
     0,NULL,0,0,0,0,0,0},

    {"watch",watchCommand,-2,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,1,-1,1,0,0,0},

    {"unwatch",unwatchCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"cluster",clusterCommand,-2,
     "admin ok-stale random",
     0,NULL,0,0,0,0,0,0},

    {"restore",restoreCommand,-4,
     "write use-memory @keyspace @dangerous",
     0,NULL,1,1,1,0,0,0},

    {"restore-asking",restoreCommand,-4,
    "write use-memory cluster-asking @keyspace @dangerous",
    0,NULL,1,1,1,0,0,0},

    {"migrate",migrateCommand,-6,
     "write random @keyspace @dangerous",
     0,migrateGetKeys,0,0,0,0,0,0},

    {"asking",askingCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"readonly",readonlyCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"readwrite",readwriteCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"dump",dumpCommand,2,
     "read-only random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"object",objectCommand,-2,
     "read-only random @keyspace",
     0,NULL,2,2,1,0,0,0},

    {"memory",memoryCommand,-2,
     "random read-only",
     0,memoryGetKeys,0,0,0,0,0,0},

    {"client",clientCommand,-2,
     "admin no-script random ok-loading ok-stale @connection",
     0,NULL,0,0,0,0,0,0},

    {"hello",helloCommand,-2,
     "no-auth no-script fast no-monitor ok-loading ok-stale no-slowlog @connection",
     0,NULL,0,0,0,0,0,0},

    /* EVAL can modify the dataset, however it is not flagged as a write
     * command since we do the check while running commands from Lua. 
     *
     * EVAL可以修改数据集，但它没有被标记为写命令，因为我们在运行Lua的命令时进行
     * 检查。*/
    {"eval",evalCommand,-3,
     "no-script @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"evalsha",evalShaCommand,-3,
     "no-script @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"slowlog",slowlogCommand,-2,
     "admin random ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"script",scriptCommand,-2,
     "no-script @scripting",
     0,NULL,0,0,0,0,0,0},

    {"time",timeCommand,1,
     "read-only random fast ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"bitop",bitopCommand,-4,
     "write use-memory @bitmap",
     0,NULL,2,-1,1,0,0,0},

    {"bitcount",bitcountCommand,-2,
     "read-only @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitpos",bitposCommand,-3,
     "read-only @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"wait",waitCommand,3,
     "no-script @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"command",commandCommand,-1,
     "ok-loading ok-stale random @connection",
     0,NULL,0,0,0,0,0,0},

    {"geoadd",geoaddCommand,-5,
     "write use-memory @geo",
     0,NULL,1,1,1,0,0,0},

    /* GEORADIUS has store options that may write. 
     *
     * GEORADIUS具有可以写入的存储选项。*/
    {"georadius",georadiusCommand,-6,
     "write use-memory @geo",
     0,georadiusGetKeys,1,1,1,0,0,0},

    {"georadius_ro",georadiusroCommand,-6,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"georadiusbymember",georadiusbymemberCommand,-5,
     "write use-memory @geo",
     0,georadiusGetKeys,1,1,1,0,0,0},

    {"georadiusbymember_ro",georadiusbymemberroCommand,-5,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geohash",geohashCommand,-2,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geopos",geoposCommand,-2,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geodist",geodistCommand,-4,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"pfselftest",pfselftestCommand,1,
     "admin @hyperloglog",
      0,NULL,0,0,0,0,0,0},

    {"pfadd",pfaddCommand,-2,
     "write use-memory fast @hyperloglog",
     0,NULL,1,1,1,0,0,0},

    /* Technically speaking PFCOUNT may change the key since it changes the
     * final bytes in the HyperLogLog representation. However in this case
     * we claim that the representation, even if accessible, is an internal
     * affair, and the command is semantically read only. 
     *
     * 从技术上讲，PFCOUNT可能会更改 key，因为它会更改HyperLogLog表示
     * 中的最后字节。然而，在这种情况下，我们声称表示，即使可访问，也是内部事务，并且命
     * 令在语义上是只读的。*/
    {"pfcount",pfcountCommand,-2,
     "read-only @hyperloglog",
     0,NULL,1,-1,1,0,0,0},

    {"pfmerge",pfmergeCommand,-2,
     "write use-memory @hyperloglog",
     0,NULL,1,-1,1,0,0,0},

    {"pfdebug",pfdebugCommand,-3,
     "admin write",
     0,NULL,0,0,0,0,0,0},

    {"xadd",xaddCommand,-5,
     "write use-memory fast random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xrange",xrangeCommand,-4,
     "read-only @stream",
     0,NULL,1,1,1,0,0,0},

    {"xrevrange",xrevrangeCommand,-4,
     "read-only @stream",
     0,NULL,1,1,1,0,0,0},

    {"xlen",xlenCommand,2,
     "read-only fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xread",xreadCommand,-4,
     "read-only @stream @blocking",
     0,xreadGetKeys,0,0,0,0,0,0},

    {"xreadgroup",xreadCommand,-7,
     "write @stream @blocking",
     0,xreadGetKeys,0,0,0,0,0,0},

    {"xgroup",xgroupCommand,-2,
     "write use-memory @stream",
     0,NULL,2,2,1,0,0,0},

    {"xsetid",xsetidCommand,3,
     "write use-memory fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xack",xackCommand,-4,
     "write fast random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xpending",xpendingCommand,-3,
     "read-only random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xclaim",xclaimCommand,-6,
     "write random fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xinfo",xinfoCommand,-2,
     "read-only random @stream",
     0,NULL,2,2,1,0,0,0},

    {"xdel",xdelCommand,-3,
     "write fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xtrim",xtrimCommand,-2,
     "write random @stream",
     0,NULL,1,1,1,0,0,0},

    {"post",securityWarningCommand,-1,
     "ok-loading ok-stale read-only",
     0,NULL,0,0,0,0,0,0},

    {"host:",securityWarningCommand,-1,
     "ok-loading ok-stale read-only",
     0,NULL,0,0,0,0,0,0},

    {"latency",latencyCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"lolwut",lolwutCommand,-1,
     "read-only fast",
     0,NULL,0,0,0,0,0,0},

    {"acl",aclCommand,-2,
     "admin no-script no-slowlog ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"stralgo",stralgoCommand,-2,
     "read-only @string",
     0,lcsGetKeys,0,0,0,0,0,0}
};

/*============================ Utility functions ============================ */

/* We use a private localtime implementation which is fork-safe. The logging
 * function of Redis may be called from other threads. 
 *
 * 我们使用一个fork 安全的私有localtime实现。Redis的日志记录
 * 功能可以从其他线程调用。*/
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

/* Low level logging. To use only for very big messages, otherwise
 * serverLog() is to prefer. 
 *
 * 低级别日志记录。只用于非常大的消息，否则最好使用serverLog（）。*/
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = server.logfile[0] == '\0';

    level &= 0xff; /* clear flags 
                    *
                    * 清除标志*/
    if (level < server.verbosity) return;

    fp = log_to_stdout ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        int off;
        struct timeval tv;
        int role_char;
        pid_t pid = getpid();

        gettimeofday(&tv,NULL);
        struct tm tm;
        nolocks_localtime(&tm,tv.tv_sec,server.timezone,server.daylight_active);
        off = strftime(buf,sizeof(buf),"%d %b %Y %H:%M:%S.",&tm);
        snprintf(buf+off,sizeof(buf)-off,"%03d",(int)tv.tv_usec/1000);
        if (server.sentinel_mode) {
            role_char = 'X'; /* Sentinel. 
                              *
                              * 哨兵*/
        } else if (pid != server.pid) {
            role_char = 'C'; /* RDB / AOF writing child. 
                              *
                              * RDB/AOF正在编写子级。*/
        } else {
            role_char = (server.masterhost ? 'S':'M'); /* Slave or Master. 
                                                        *
                                                        * 奴隶或主人。*/
        }
        fprintf(fp,"%d:%c %s %c %s\n",
            (int)getpid(),role_char, buf,c[level],msg);
    }
    fflush(fp);

    if (!log_to_stdout) fclose(fp);
    if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. 
 *
 * 类似于serverLogRaw（），但具有类似printf的支持。这是代码中使用
 * 的函数。原始版本仅用于在崩溃时转储INFO输出。*/
void serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    if ((level&0xff) < server.verbosity) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level,msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). 
 *
 * 在没有类似printf功能的情况下，以从信号处理程序安全调用的方式记录固定消息。
 * 
 * 实际上，从Redis的角度来看，我们只将其用于不致命的信号。serverLog（）
 * 提供了无论如何都会杀死服务器的信号，以及我们需要类似printf功能的地方。*/ 
void serverLogFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = server.logfile[0] == '\0';
    char buf[64];

    if ((level&0xff) < server.verbosity || (log_to_stdout && server.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO :
                         open(server.logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    if (fd == -1) return;
    ll2string(buf,sizeof(buf),getpid());
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,":signal-handler (",17) == -1) goto err;
    ll2string(buf,sizeof(buf),time(NULL));
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,") ",2) == -1) goto err;
    if (write(fd,msg,strlen(msg)) == -1) goto err;
    if (write(fd,"\n",1) == -1) goto err;
err:
    if (!log_to_stdout) close(fd);
}

/* Return the UNIX time in microseconds 
 *
 * 以微秒为单位返回UNIX时间*/
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds 
 *
 * 返回以毫秒为单位的UNIX时间*/
mstime_t mstime(void) {
    return ustime()/1000;
}

/* After an RDB dump or AOF rewrite we exit from children using _exit() instead of
 * exit(), because the latter may interact with the same file objects used by
 * the parent process. However if we are testing the coverage normal exit() is
 * used in order to obtain the right coverage information. 
 *
 * 在RDB转储或AOF重写之后，我们使用_exit（）而不是exit（）从子进程退出，
 * 因为后者可能与父进程使用的相同文件对象交互。然而，如果我们正在测试覆盖率，
 * 则使用normal exit（）来获得正确的覆盖率信息。*/ 
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== Hash table type implementation  ==================== */

/* This is a hash table type that uses the SDS dynamic strings library as
 * keys and redis objects as values (objects can hold SDS strings,
 * lists, sets). 
 *
 * 这是一种哈希表类型，使用SDS动态字符串库作为键，使用redis对象作为值（对象
 * 可以包含SDS字符串、列表、集合）。*/

void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}

void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}

int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. 
 *
 * 一个不区分大小写的版本，用于命令查找表和其他需要进行区分大小写非二进制安全比较的
 * 地方。*/
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

void dictObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Lazy freeing will set value to NULL. 
                              *
                              * 延迟释放会将值设置为NULL。*/
    decrRefCount(val);
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
}

uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT &&
        o2->encoding == OBJ_ENCODING_INT)
            return o1->ptr == o2->ptr;

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. 
     *
     * 由于OBJ_STATIC_REFCOUNT，我们避免在没有充分理由的情况下调用g
     * etDecodedObject（），因为它会增加对象的REFCOUNT（），这是
     * 无效的。因此，我们检查以确保dictFind（）也适用于静态对象。*/
    if (o1->refcount != OBJ_STATIC_REFCOUNT) o1 = getDecodedObject(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
    if (o1->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o2);
    return cmp;
}

uint64_t dictEncObjHash(const void *key) {
    robj *o = (robj*) key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == OBJ_ENCODING_INT) {
            char buf[32];
            int len;

            len = ll2string(buf,32,(long)o->ptr);
            return dictGenHashFunction((unsigned char*)buf, len);
        } else {
            uint64_t hash;

            o = getDecodedObject(o);
            hash = dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
            decrRefCount(o);
            return hash;
        }
    }
}

/* Generic hash table type where keys are Redis Objects, Values
 * dummy pointers. 
 *
 * 泛型哈希表类型，其中键为Redis对象，值为伪指针。*/
dictType objectKeyPointerValueDictType = {
    dictEncObjHash,            /* hash function  散列函数*/
    NULL,                      /* key dup  键复制*/
    NULL,                      /* val dup 值复制*/
    dictEncObjKeyCompare,      /* key compare  键比较*/
    dictObjectDestructor,      /* key destructor  键析构函数*/
    NULL                       /* val destructor 值析构函数*/
};

/* Like objectKeyPointerValueDictType(), but values can be destroyed, if
 * not NULL, calling zfree(). 
 *
 * 类似于objectKeyPointerValueDictType（），但如果值不
 * 是NULL，则可以通过调用zfree（）来销毁值。*/
dictType objectKeyHeapPointerValueDictType = {
    dictEncObjHash,            /* hash function  散列函数*/
    NULL,                     /* key dup  键复制*/
    NULL,                      /* val dup 值复制*/
    dictEncObjKeyCompare,      /* key compare  键比较*/
    dictObjectDestructor,      /* key destructor  键析构函数*/
    dictVanillaFree            /* val destructor 值析构函数*/
};

/* Set dictionary type. Keys are SDS strings, values are not used. 
 *
 * 设置字典类型。键是SDS字符串，不使用值。*/
dictType setDictType = {
    dictSdsHash,              /* hash function  散列函数*/
    NULL,                       /* key dup  键复制*/
    NULL,                      /* val dup 值复制*/
    dictSdsKeyCompare,        /* key compare  键比较*/
    dictSdsDestructor,        /* key destructor  键析构函数*/
    NULL                       /* val destructor 值析构函数*/
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) 
 *
 * 排序集哈希（注意：除了哈希表之外，还使用了skiplist）*/
dictType zsetDictType = {
    dictSdsHash,              /* hash function  散列函数*/
    NULL,                       /* key dup  键复制*/
    NULL,                      /* val dup 值复制*/
    dictSdsKeyCompare,        /* key compare  键比较*/
    NULL,                      /* Note: SDS string shared & freed by skiplist 
                                *
                                * 注意：由skiplist共享和释放的SDS字符串*/
    NULL                       /* val destructor 值析构函数*/
};

/* Db->dict, keys are sds strings, vals are Redis objects. 
 *
 * Db->dict，keys是sds字符串，vals是Redis对象。*/
dictType dbDictType = {
    dictSdsHash,               /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCompare,         /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    dictObjectDestructor   /* val destructor 值析构函数*/
};

/* server.lua_scripts sha (as sds string) -> scripts (as robj) cache. 
 *
 * server.lua_scripts-sha（作为sds-string）->脚本
 * （作为robj）缓存。*/
dictType shaScriptObjectDictType = {
    dictSdsCaseHash,           /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCaseCompare,     /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    dictObjectDestructor        /* val destructor 值析构函数*/
};

/* Db->expires 
 *
 * Db->过期*/
dictType keyptrDictType = {
    dictSdsHash,               /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCompare,         /* key compare  键比较*/
    NULL,                      /* key destructor  键析构函数*/
    NULL                        /* val destructor 值析构函数*/
};

/* Command table. sds string -> command struct pointer. 
 *
 * 命令表。sds-string->命令结构指针。*/
dictType commandTableDictType = {
    dictSdsCaseHash,           /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCaseCompare,     /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    NULL                        /* val destructor 值析构函数*/
};

/* Hash type hash table (note that small hashes are represented with ziplists) 
 *
 * 哈希类型哈希表（注意，小哈希用ziplists表示）*/
dictType hashDictType = {
    dictSdsHash,               /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCompare,         /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    dictSdsDestructor           /* val destructor 值析构函数*/
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. 
 *
 * 键列表哈希表类型将未编码的redis对象作为键，将列表作为值。它用于阻塞操作（B
 * LPOP），并将交换的密钥映射到等待加载此密钥的客户端列表。*/
dictType keylistDictType = {
    dictObjHash,               /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictObjKeyCompare,         /* key compare  键比较*/
    dictObjectDestructor,      /* key destructor  键析构函数*/
    dictListDestructor          /* val destructor 值析构函数*/
};

/* Cluster nodes hash table, mapping nodes addresses 1.2.3.4:6379 to
 * clusterNode structures. 
 *
 * 集群节点哈希表，将节点地址1.2.3.4:6379映射到clusterNode结
 * 构。*/
dictType clusterNodesDictType = {
    dictSdsHash,               /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCompare,         /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    NULL                        /* val destructor 值析构函数*/
};

/* Cluster re-addition blacklist. This maps node IDs to the time
 * we can re-add this node. The goal is to avoid readding a removed
 * node for some time. 
 *
 * 集群重新添加黑名单。这会将节点ID映射到我们可以重新添加此节点的时间。目标是避免
 * 在一段时间内重新读取已删除的节点。*/
dictType clusterNodesBlackListDictType = {
    dictSdsCaseHash,           /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCaseCompare,     /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    NULL                        /* val destructor 值析构函数*/
};

/* Modules system dictionary type. Keys are module name,
 * values are pointer to RedisModule struct. 
 *
 * 模块系统字典类型。键是模块名称，值是指向RedisModule结构的指针。*/
dictType modulesDictType = {
    dictSdsCaseHash,           /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCaseCompare,     /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    NULL                        /* val destructor 值析构函数*/
};

/* Migrate cache dict type. 
 *
 * 迁移缓存dict类型。*/
dictType migrateCacheDictType = {
    dictSdsHash,               /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCompare,         /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    NULL                        /* val destructor 值析构函数*/
};

/* Replication cached script dict (server.repl_scriptcache_dict).
 * Keys are sds SHA1 strings, while values are not used at all in the current
 * implementation. 
 *
 * 复制缓存的脚本dict（server.repl_scriptcache_dict
 * ）。键是sds-SHA1字符串，而在当前实现中根本不使用值。*/
dictType replScriptCacheDictType = {
    dictSdsCaseHash,           /* hash function  散列函数*/
    NULL,                        /* key dup  键复制*/
    NULL,                       /* val dup 值复制*/
    dictSdsKeyCaseCompare,     /* key compare  键比较*/
    dictSdsDestructor,         /* key destructor  键析构函数*/
    NULL                        /* val destructor 值析构函数*/
};

/*
 * 检查字典的使用率是否低于系统允许的最小比率
 *
 * 是的话返回 1 ，否则返回 0 。
 */
int htNeedsResize(dict *dict) {
    long long size, used;

    // 哈希表大小
    size = dictSlots(dict);
    // 哈希表已用节点数量
    used = dictSize(dict);
    // 当哈希表的大小大于 DICT_HT_INITIAL_SIZE
    // 并且字典的填充率低于 REDIS_HT_MINFILL 时
    // 返回 1
    return (size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < HASHTABLE_MIN_FILL));
}

/* If the percentage of used slots in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory 
 *
 * 如果HT中已使用插槽的百分比达到HASHTABLE_MIN_FILL，则调整哈希
 * 表的大小以节省内存*/
/*
 * 对服务器中的所有数据库键空间字典、以及过期时间字典进行检查，
 * 看是否需要对这些字典进行收缩。
 *
 * 如果字典的使用空间比率低于 REDIS_HT_MINFILL
 * 那么将字典的大小缩小，让 USED/BUCKETS 的比率 <= 1
 */
void tryResizeHashTables(int dbid) {
    // 缩小键空间字典
    if (htNeedsResize(server.db[dbid].dict))
        dictResize(server.db[dbid].dict);
    // 缩小过期时间字典
    if (htNeedsResize(server.db[dbid].expires))
        dictResize(server.db[dbid].expires);
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns 1 if some rehashing was performed, otherwise 0
 * is returned. 
 *
 * 我们的哈希表实现在写入/读取哈希表时递增地执行重新哈希。尽管如此，如果服务器空闲
 * ，哈希表将在很长一段时间内使用两个表。因此，我们尝试在每次调用该函数时使用1毫秒
 * 的CPU时间来执行一些重新散列。
 *
 * 如果执行了一些重新散列，则函数返回1，否则返回0
 * 。*/
/*
 * 在 Redis Cron 中调用，对数据库中第一个遇到的、可以进行 rehash 的哈希表
 * 进行 1 毫秒的渐进式 rehash
 */
int incrementallyRehash(int dbid) {
    /* Keys dictionary 
     *
     * 密钥字典*/
    if (dictIsRehashing(server.db[dbid].dict)) {
        dictRehashMilliseconds(server.db[dbid].dict,1);
        return 1; /* already used our millisecond for this loop... 
                   *
                   * 已经为这个循环使用了毫秒。。。*/
    }
    /* Expires 
     *
     * 过期*/
    if (dictIsRehashing(server.db[dbid].expires)) {
        dictRehashMilliseconds(server.db[dbid].expires,1);
        return 1; /* already used our millisecond for this loop... 
                   *
                   * 已经为这个循环使用了毫秒。。。*/
    }
    return 0;
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize or rehash the tables accordingly to the fact we have an
 * active fork child running. 
 *
 * 一旦某种后台进程终止，就会调用此函数，因为我们希望避免在有子进程时调整哈希表的大
 * 小，以便在写入时很好地进行复制（否则，当调整大小时，会复制大量内存页）。这个函数
 * 的目标是更新dict.c的能力，根据我们有一个活动的fork-child正在运行
 * 的事实来调整或重新格式化表。*/

// 在执行保存时关闭对数据库的 rehash
// 避免 copy-on-write 问题
void updateDictResizePolicy(void) {
    if (server.in_fork_child != CHILD_TYPE_NONE)
        dictSetResizeEnabled(DICT_RESIZE_FORBID);
    else if (hasActiveChildProcess())
        dictSetResizeEnabled(DICT_RESIZE_AVOID);
    else
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
}

/* Return true if there are no active children processes doing RDB saving,
 * AOF rewriting, or some side process spawned by a loaded module. 
 *
 * 如果没有活动的子进程进行RDB保存、AOF重写或由加载的模块派生的某些辅助进程，
 * 则返回true。*/
int hasActiveChildProcess() {
    return server.rdb_child_pid != -1 ||
           server.aof_child_pid != -1 ||
           server.module_child_pid != -1;
}

/* Return true if this instance has persistence completely turned off:
 * both RDB and AOF are disabled. 
 *
 * 如果此实例完全关闭了持久性，则返回true：RDB和AOF都被禁用。*/
int allPersistenceDisabled(void) {
    return server.saveparamslen == 0 && server.aof_state == AOF_OFF;
}

/* ======================= Cron: called every 100 ms ======================== */

/* Add a sample to the operations per second array of samples. 
 *
 * 将一个样本添加到每秒操作的样本数组中。*/
void trackInstantaneousMetric(int metric, long long current_reading) {
    long long t = mstime() - server.inst_metric[metric].last_sample_time;
    long long ops = current_reading -
                    server.inst_metric[metric].last_sample_count;
    long long ops_sec;

    ops_sec = t > 0 ? (ops*1000/t) : 0;

    server.inst_metric[metric].samples[server.inst_metric[metric].idx] =
        ops_sec;
    server.inst_metric[metric].idx++;
    server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    server.inst_metric[metric].last_sample_time = mstime();
    server.inst_metric[metric].last_sample_count = current_reading;
}

/* Return the mean of all the samples. 
 *
 * 返回所有样本的平均值。*/
long long getInstantaneousMetric(int metric) {
    int j;
    long long sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++)
        sum += server.inst_metric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * The function always returns 0 as it never terminates the client. 
 *
 * 客户端查询缓冲区是一个sds.c字符串，它可以以大量未使用的可用空间结束，如果需
 * 要，此函数会回收空间。函数始终返回0，因为它从未终止客户端。*/
// 收缩查询缓存的空间
int clientsCronResizeQueryBuffer(client *c) {
    size_t querybuf_size = sdsAllocSize(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction;

    /* There are two conditions to resize the query buffer:
     * 1) Query buffer is > BIG_ARG and too big for latest peak.
     * 2) Query buffer is > BIG_ARG and client is idle. 
     *
     * 有两个条件可以调整查询缓冲区的大小：
     *
     * 1）查询缓冲区大于BIG_ARG，并且对于最近的峰值来说太大。
     * 2） 查询缓冲区>BIG_ARG，客户端空闲。*/
    if (querybuf_size > PROTO_MBULK_BIG_ARG &&
         ((querybuf_size/(c->querybuf_peak+1)) > 2 ||
          idletime > 2))
    {
        /* Only resize the query buffer if it is actually wasting
         * at least a few kbytes. 
         *
         * 只有当查询缓冲区实际浪费了至少几千字节时，才调整其大小。*/
        if (sdsavail(c->querybuf) > 1024*4) {
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
        }
    }
    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. 
     *
     * 再次重置峰值以捕获下一个周期中的峰值内存使用情况。*/
    c->querybuf_peak = 0;

    /* Clients representing masters also use a "pending query buffer" that
     * is the yet not applied part of the stream we are reading. Such buffer
     * also needs resizing from time to time, otherwise after a very large
     * transfer (a huge value or a big MIGRATE operation) it will keep using
     * a lot of memory. 
     *
     * 代表主机的客户端还使用“挂起的查询缓冲区”，这是我们正在读取的流中尚未应用的部分
     * 。这样的缓冲区也需要不时地调整大小，否则在一个非常大的传输（一个巨大的值或一个大
     * 的MIGRATE操作）之后，它将继续使用大量内存。*/
    if (c->flags & CLIENT_MASTER) {
        /* There are two conditions to resize the pending query buffer:
         * 1) Pending Query buffer is > LIMIT_PENDING_QUERYBUF.
         * 2) Used length is smaller than pending_querybuf_size/2 
         *
         * 有两个条件可以调整挂起查询缓冲区的大小：
         * 1）挂起查询的缓冲区大于LIMIT_PENDING_QUERYBUF。
         * 2） 使用的长度小于pending_querybuf_size/2
         * */
        size_t pending_querybuf_size = sdsAllocSize(c->pending_querybuf);
        if(pending_querybuf_size > LIMIT_PENDING_QUERYBUF &&
           sdslen(c->pending_querybuf) < (pending_querybuf_size/2))
        {
            c->pending_querybuf = sdsRemoveFreeSpace(c->pending_querybuf);
        }
    }
    return 0;
}

/* This function is used in order to track clients using the biggest amount
 * of memory in the latest few seconds. This way we can provide such information
 * in the INFO output (clients section), without having to do an O(N) scan for
 * all the clients.
 *
 * This is how it works. We have an array of CLIENTS_PEAK_MEM_USAGE_SLOTS slots
 * where we track, for each, the biggest client output and input buffers we
 * saw in that slot. Every slot correspond to one of the latest seconds, since
 * the array is indexed by doing UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS.
 *
 * When we want to know what was recently the peak memory usage, we just scan
 * such few slots searching for the maximum value. 
 *
 * 此功能用于跟踪最近几秒钟内使用最大内存量的客户端。通过这种方式，我们可以在INFO
 * 输出（客户端部分）中提供这样的信息，而不必对所有客户端进行O（N）扫描。
 *
 * 这就是它的工作原理。我们有一个CLIENTS_PEAK_MEM_USAGE_SLOTS
 * 插槽阵列，我们在其中跟踪每个插槽中看到的最大客户端输出和输入缓冲区。每个槽都对应
 * 于最近的一秒，因为数组是通过执行UNIXTIME%CLIENTS_PEAK_MEM_USAGE_SLOTS
 * 进行索引的。当我们想知道最近的内存使用峰值是多少时，我
 * 们只需扫描这么少的插槽来搜索最大值。
 * */
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS];
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS];

int clientsCronTrackExpansiveClients(client *c) {
    size_t in_usage = sdsZmallocSize(c->querybuf) + c->argv_len_sum;
    size_t out_usage = getClientOutputBufferMemoryUsage(c);
    int i = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    int zeroidx = (i+1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;

    /* Always zero the next sample, so that when we switch to that second, we'll
     * only register samples that are greater in that second without considering
     * the history of such slot.
     *
     * Note: our index may jump to any random position if serverCron() is not
     * called for some reason with the normal frequency, for instance because
     * some slow command is called taking multiple seconds to execute. In that
     * case our array may end containing data which is potentially older
     * than CLIENTS_PEAK_MEM_USAGE_SLOTS seconds: however this is not a problem
     * since here we want just to track if "recently" there were very expansive
     * clients from the POV of memory usage. 
     *
     * 总是将下一个样本归零，这样当我们切换到该秒时，我们将只注册该秒中较大的样本，而不
     * 考虑该插槽的历史记录。
     *
     * 注意：如果由于某种原因没有以正常频率调用serverCron（），
     * 我们的索引可能会跳到任何随机位置，例如因为调用某个慢命令需要几秒钟才能执
     * 行。在这种情况下，我们的数组可能最终包含的数据可能早于CLIENTS_PEAK_
     * MEM_USAGE_SLOTS秒：但是这不是问题，因为在这里我们只想跟踪“最近”
     * 是否有来自内存使用POV的非常广泛的客户端。*/
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0;

    /* Track the biggest values observed so far in this slot. 
     *
     * 跟踪此时段中迄今为止观察到的最大值。*/
    if (in_usage > ClientsPeakMemInput[i]) ClientsPeakMemInput[i] = in_usage;
    if (out_usage > ClientsPeakMemOutput[i]) ClientsPeakMemOutput[i] = out_usage;

    return 0; /* This function never terminates the client. 
               *
               * 此函数从不终止客户端。*/
}

/* Iterating all the clients in getMemoryOverheadData() is too slow and
 * in turn would make the INFO command too slow. So we perform this
 * computation incrementally and track the (not instantaneous but updated
 * to the second) total memory used by clients using clinetsCron() in
 * a more incremental way (depending on server.hz). 
 *
 * 在getMemoryOverheadData（）中迭代所有客户端的速度太慢，反过
 * 来会使INFO命令太慢。因此，我们递增地执行此计算，并以更递增的方式（取决于server.hz）
 * 跟踪客户端使用clinetsCron（）使用的总内存（不是即时的而是更新到第二个）。
 *
 * */
int clientsCronTrackClientsMemUsage(client *c) {
    size_t mem = 0;
    int type = getClientType(c);
    mem += getClientOutputBufferMemoryUsage(c);
    mem += sdsZmallocSize(c->querybuf);
    mem += zmalloc_size(c);
    mem += c->argv_len_sum;
    if (c->argv) mem += zmalloc_size(c->argv);
    /* Now that we have the memory used by the client, remove the old
     * value from the old category, and add it back. 
     *
     * 现在我们有了客户端使用的内存，从旧类别中删除旧值，然后将其添加回来。
     * */
    server.stat_clients_type_memory[c->client_cron_last_memory_type] -=
        c->client_cron_last_memory_usage;
    server.stat_clients_type_memory[type] += mem;
    /* Remember what we added and where, to remove it next time. 
     *
     * 记住我们添加的内容和位置，以便下次删除。*/
    c->client_cron_last_memory_usage = mem;
    c->client_cron_last_memory_type = type;
    return 0;
}

/* Return the max samples in the memory usage of clients tracked by
 * the function clientsCronTrackExpansiveClients(). 
 *
 * 返回函数clientsCronTrackExpansiveClients（）跟踪
 * 的客户端内存使用情况的最大样本数。*/
void getExpansiveClientsInfo(size_t *in_usage, size_t *out_usage) {
    size_t i = 0, o = 0;
    for (int j = 0; j < CLIENTS_PEAK_MEM_USAGE_SLOTS; j++) {
        if (ClientsPeakMemInput[j] > i) i = ClientsPeakMemInput[j];
        if (ClientsPeakMemOutput[j] > o) o = ClientsPeakMemOutput[j];
    }
    *in_usage = i;
    *out_usage = o;
}

/* This function is called by serverCron() and is used in order to perform
 * operations on clients that are important to perform constantly. For instance
 * we use this function in order to disconnect clients after a timeout, including
 * clients blocked in some blocking command with a non-zero timeout.
 *
 * The function makes some effort to process all the clients every second, even
 * if this cannot be strictly guaranteed, since serverCron() may be called with
 * an actual frequency lower than server.hz in case of latency events like slow
 * commands.
 *
 * It is very important for this function, and the functions it calls, to be
 * very fast: sometimes Redis has tens of hundreds of connected clients, and the
 * default server.hz value is 10, so sometimes here we need to process thousands
 * of clients per second, turning this function into a source of latency.
 
 *
 * 此函数由serverCron（）调用，用于在客户端上执行对持续执行非常重要的操作
 * 。例如，我们使用此函数是为了在超时后断开客户端的连接，包括在某个超时为非零的阻塞
 * 命令中被阻塞的客户端。
 *
 * 该函数会努力每秒处理所有客户端，即使不能严格保证这一点，因
 * 为在出现诸如慢命令之类的延迟事件时，调用serverCron（）的实际频率可能低于server.hz。
 *
 * 对于这个函数及其调用的函数来说，速度非常快是非常重要的：有
 * 时Redis有数以万计的连接客户端，而默认的server.hz值是10，所以有时
 * 我们需要每秒处理数千个客户端，从而将这个函数变成延迟源。*/
#define CLIENTS_CRON_MIN_ITERATIONS 5
/*
 * 客户端常规任务
 *
 * 检查连接是否超时，以及清理多余的查询缓存
 */
void clientsCron(void) {
    /* Try to process at least numclients/server.hz of clients
     * per call. Since normally (if there are no big latency events) this
     * function is called server.hz times per second, in the average case we
     * process all the clients in 1 second. 
     *
     * 尝试每次调用至少处理numclients/server.hz个客户端。由于正常情
     * 况下（如果没有大延迟事件），该函数每秒被调用server.hz次，因此在平均情况
     * 下，我们在1秒内处理所有客户端。*/
    int numclients = listLength(server.clients);
    // 遍历次数=客户端数量/服务器设定的频率
    int iterations = numclients/server.hz;
    mstime_t now = mstime();

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. 
     *
     * 即使我们需要处理少于clients_CRON_MIN_ITERATIONS的客户
     * ，以满足我们每秒处理每个客户一次的合同，我们也要处理至少几个客户。*/
    // 最少执行 5 次遍历
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;

    while(listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. 
         *
         * 旋转列表，取当前头，进行处理。这样，如果客户端必须从列表中删除，它就是第一个元素
         * ，我们就不会进行O（N）计算。*/
        // 将处理的客户端调到表头，
        // 这样在要删除客户端时，复杂度就是 O(1) 而不是 O(N) 了
        listRotateTailToHead(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. 
         *
         * 以下函数对客户端执行不同的服务检查。协议是，如果客户端被终止，它们将返回非零。
         * */

        // 检查客户端是否超时，如果是的话，删除它的连接
        // 如果客户端正因 BLPOP/BRPOP/BLPOPRPUSH 阻塞，那么检查阻塞是否超时，
        // 是的话就退出阻塞状态
        if (clientsCronHandleTimeout(c,now)) continue;
        // 释放客户端查询缓存多余的空间
        if (clientsCronResizeQueryBuffer(c)) continue;
        if (clientsCronTrackExpansiveClients(c)) continue;
        if (clientsCronTrackClientsMemUsage(c)) continue;
    }
}

/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. 
 *
 * 此函数处理Redis数据库中需要增量执行的“后台”操作，如活动密钥过期、调整大小
 * 、重新哈希。*/
void databasesCron(void) {
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. 
     *
     * 通过随机采样使键过期。从节点不需要，因为节点会为我们合成DEL。*/
    if (server.active_expire_enabled) {
        // 如果服务器是主节点的话，进行过期键删除
        // 如果服务器是附属节点的话，那么等待主节点发来的 DEL 命令
        if (iAmMaster()) {
            activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
        } else {
            expireSlaveKeys();
        }
    }

    /* Defrag keys gradually. 
     *
     * 逐渐对密钥进行碎片整理。*/
    activeDefragCycle();

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. 
     *
     * 如果需要，请执行哈希表重新哈希，但前提是磁盘上没有其他进程保存DB。否则，重新哈
     * 希是不好的，因为它会导致大量的内存页写入复制。*/
    if (!hasActiveChildProcess()) {
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. 
         *
         * 我们使用全局计数器，因此如果我们在给定的DB停止计算，我们将能够在下一个cron
         * 循环迭代中从连续的开始。*/
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* Don't test more DBs than we have. 
         *
         * 不要测试比我们更多的数据库。*/
        if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

        /* Resize 
         *
         * 调整大小*/
        for (j = 0; j < dbs_per_call; j++) {
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        /* Rehash*/
        if (server.activerehashing) {
            for (j = 0; j < dbs_per_call; j++) {
                int work_done = incrementallyRehash(rehash_db);
                if (work_done) {
                    /* If the function did some work, stop here, we'll do
                     * more at the next cron loop. 
                     *
                     * 如果函数做了一些工作，那么就到此为止，我们将在下一个cron循环中做更多的工作。*/
                    break;
                } else {
                    /* If this db didn't need rehash, we'll try the next one. 
                     *
                     * 如果这个数据库不需要重新散列，我们将尝试下一个。*/
                    rehash_db++;
                    rehash_db %= server.dbnum;
                }
            }
        }
    }
}

/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL).
 *
 * This function should be fast because it is called at every command execution
 * in call(), so it is possible to decide if to update the daylight saving
 * info or not using the 'update_daylight_info' argument. Normally we update
 * such info only when calling this function from serverCron() but not when
 * calling it from call(). 
 *
 * 我们取全局状态下unix时间的缓存值，因为在虚拟内存和老化的情况下，每次访问对象
 * 时都要将当前时间存储在对象中，而不需要准确性。访问全局var比调用时间（NULL）快得多。
 *
 * 此函数应该很快，因为它在call（）中的每次命令执行时都会被调用，因此
 * 可以使用“update_daylight_info”参数来决定是否更新夏令时信息。
 * 通常，我们只在从serverCron（）调用此函数时更新此类信息，而在从call（）调用时不更新。
 * */
void updateCachedTime(int update_daylight_info) {
    server.ustime = ustime();
    server.mstime = server.ustime / 1000;
    server.unixtime = server.mstime / 1000;

    /* To get information about daylight saving time, we need to call
     * localtime_r and cache the result. However calling localtime_r in this
     * context is safe since we will never fork() while here, in the main
     * thread. The logging function will call a thread safe version of
     * localtime that has no locks. 
     *
     * 要获得有关夏令时的信息，我们需要调用localtime_r并缓存结果。然而，在此
     * 上下文中调用localtime_r是安全的，因为在这里，在主线程中，我们永远不会
     * 分叉（）。日志记录函数将调用一个没有锁的线程安全版本的localtime。
     * */
    if (update_daylight_info) {
        struct tm tm;
        time_t ut = server.unixtime;
        localtime_r(&ut,&tm);
        server.daylight_active = tm.tm_isdst;
    }
}

void checkChildrenDone(void) {
    int statloc;
    pid_t pid;

    if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
        int exitcode = WEXITSTATUS(statloc);
        int bysignal = 0;

        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

        /* sigKillChildHandler catches the signal and calls exit(), but we
         * must make sure not to flag lastbgsave_status, etc incorrectly.
         * We could directly terminate the child process via SIGUSR1
         * without handling it, but in this case Valgrind will log an
         * annoying error. 
         *
         * sigKillChildHandler捕获信号并调用exit（），但我们必须确保
         * 不要错误地标记lastbgsave_status等。我们可以通过SIGUSR1直
         * 接终止子进程而不进行处理，但在这种情况下，Valgrind将记录一个令人讨厌的错 误。
         * */
        if (exitcode == SERVER_CHILD_NOERROR_RETVAL) {
            bysignal = SIGUSR1;
            exitcode = 1;
        }

        if (pid == -1) {
            serverLog(LL_WARNING,"wait3() returned an error: %s. "
                "rdb_child_pid = %d, aof_child_pid = %d, module_child_pid = %d",
                strerror(errno),
                (int) server.rdb_child_pid,
                (int) server.aof_child_pid,
                (int) server.module_child_pid);
        } else if (pid == server.rdb_child_pid) {
            backgroundSaveDoneHandler(exitcode,bysignal);
            if (!bysignal && exitcode == 0) receiveChildInfo();
        } else if (pid == server.aof_child_pid) {
            backgroundRewriteDoneHandler(exitcode,bysignal);
            if (!bysignal && exitcode == 0) receiveChildInfo();
        } else if (pid == server.module_child_pid) {
            ModuleForkDoneHandler(exitcode,bysignal);
            if (!bysignal && exitcode == 0) receiveChildInfo();
        } else {
            if (!ldbRemoveChild(pid)) {
                serverLog(LL_WARNING,
                    "Warning, detected child with unmatched pid: %ld",
                    (long)pid);
            }
        }
        // 如果 BGSAVE 和 BGREWRITEAOF 都已经完成，那么重新开始 REHASH
        updateDictResizePolicy();
        closeChildInfoPipe();
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * 时间中断器，调用间隔为 REDIS_HZ 。
 *
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * 以下是需要异步地完成的工作：
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 *   主动回收过期的键
 *
 * - Software watchdog.
 *   软件看门狗
 *
 * - Update some statistic.
 *   更新统计信息
 *
 * - Incremental rehashing of the DBs hash tables.
 *   对数据库进行渐进式 REHASH
 *
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 *   触发 BGSAVE 、 AOF 重写，并处理随之而来的子进程中介
 * - Clients timeout of different kinds.
 *   各种类型的客户端超时
 *
 * - Replication reconnection.
 *   重连复制节点
 *
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 *
 * 因为在这个函数中直接调用的函数都会以 REDIS_HZ 频率调用，
 * 为了调整部分函数执行的频率，使用了 run_with_period(ms) { ... }
 * 来修改代码的执行频率
 *
 */

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Software watchdog: deliver the SIGALRM that will reach the signal
     * handler if we don't return here fast enough. 
     *
     * 软件看门狗：如果我们没有足够快地返回到这里，则传递将到达信号处理程序的SIGALRM。*/
    if (server.watchdog_period) watchdogScheduleSignal(server.watchdog_period);

    /* Update the time cache. 
     *
     * 更新时间缓存。*/
    updateCachedTime(1);

    // hz = 1/T，T是周期。（其中hz是指赫兹，T是指以秒为单位）

    // CONFIG_MIN_HZ = 1       最小是1 = 1秒运行1次
    // CONFIG_MAX_HZ = 500     最大是500 = 1秒运行500次
    // CONFIG_DEFAULT_HZ = 10  默认10次


    // 先用配置的hz
    server.hz = server.config_hz;
    /* Adapt the server.hz value to the number of configured clients. If we have
     * many clients, we want to call serverCron() with an higher frequency. 
     *
     * 根据配置的客户端数量调整server.hz值。如果我们有很多客户端，我们希望调用
     * serverCron（）的频率更高。
     * */

    // 如果配置是自适应，动态变化的
    if (server.dynamic_hz) {
        while (listLength(server.clients) / server.hz >
               // 默认是 MAX_CLIENTS_PER_CLOCK_TICK = 200
               MAX_CLIENTS_PER_CLOCK_TICK)
        {
            // 如果客户端的数量越多，执行频率就越快
            server.hz *= 2;
            if (server.hz > CONFIG_MAX_HZ) {
                server.hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }


    // run_with_period 的解析：

    // 宏定义，等价于： if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))
    /*
     * 1、【_ms_ <= 1000/server.hz】
     *          如果定义的时间比周期时间短，那每次都执行
     *
     * 2、【!(server.cronloops%((_ms_)/(1000/server.hz)))】
     *          2.1、 period =「 1000/server.hz 」：定义了周期时间长
     *          2.2、 count =「 ((_ms_)/ period ) 」 ： 需要运行多少个周期
     *          2.3、 times = 「 server.cronloops 」 服务器运行了几个周期
     *          2.4、「 times % count 」取模，如果等于0，就是刚好运行了那么多次
     *          2.5、「！0」取反为真，就是如果取模等于0，刚好跑了那么多次
     *
     * 3、忽略 (_ms_ <= 1000/server.hz) 前提：
     *    假如 hz = 10,   那么 run_with_period(1000)，因为 serverCron 1毫秒触发一次，那么   10毫秒运行1次
     *    假如 hz = 100,  那么 run_with_period(1000)，因为 serverCron 1毫秒触发一次，那么  100毫秒运行1次
     *    假如 hz = 1000, 那么 run_with_period(1000)，因为 serverCron 1毫秒触发一次，那么 1000毫秒运行1次
     *    结论：hz越大，serverCron 执行的次数越少
     *
     *    假如 hz = 10,   那么 run_with_period(10)，  因为 serverCron 1毫秒触发一次，那么  0.1毫秒运行1次
     *    假如 hz = 10,   那么 run_with_period(100)， 因为 serverCron 1毫秒触发一次，那么    1毫秒运行1次
     *    假如 hz = 10,   那么 run_with_period(1000)，因为 serverCron 1毫秒触发一次，那么   10毫秒运行1次
     *    结论：hz 为 10，那 run_with_period(100) 就是 1毫秒 跑一次的概念了。
     * */


    // 如何保证 定义的 hz 就是真正的运行频率，跟CPU速度无关
    // 因为 serverCron 创建的时候，1ms触发，代码出处如下「server.c」：
    //  if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR)

    // 后面会根据 timeProc 的返回值是否循环定时，serverCron 就是 timeProc指针指向的函数,返回值就是延迟的时间，代码出处如下「ae.c」:
    // retval = te->timeProc(eventLoop, id, te->clientData);

    run_with_period(100) {
        trackInstantaneousMetric(STATS_METRIC_COMMAND,server.stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT,
                server.stat_net_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT,
                server.stat_net_output_bytes);
    }

    /* We have just LRU_BITS bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock.
     *
     * Note that even if the counter wraps it's not a big problem,
     * everything will still work but some object will appear younger
     * to Redis. However for this to happen a given object should never be
     * touched for all the time needed to the counter to wrap, which is
     * not likely.
     *
     * Note that you can change the resolution altering the
     * LRU_CLOCK_RESOLUTION define. 
     *
     * 对于LRU信息，每个对象只有LRU_BITS位。因此，我们使用（最终包装）LRU时钟。
     *
     * 请注意，即使计数器包装并不是什么大问题，一切都会正常工作，但有些对象对Re
     * dis来说会显得更年轻。然而，如果发生这种情况，在计数器进行包装所需的所有时间内，
     * 永远不应该触摸给定的对象，这是不可能的。
     *
     * 请注意，您可以更改 LRU_CLOCK_RESOLUTION 定义的分辨率。*/
    server.lruclock = getLRUClock();

    /* Record the max memory used since the server was started. 
     *
     * 记录自服务器启动以来使用的最大内存。
     * */
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) {
        /* Sample the RSS and other metrics here since this is a relatively slow call.
         * We must sample the zmalloc_used at the same time we take the rss, otherwise
         * the frag ratio calculate may be off (ratio of two samples at different times) 
         *
         * 在这里对RSS和其他指标进行采样，因为这是一个相对较慢的调用。我们必须在获取rss的同时
         * 对所使用的zmalloc_used进行采样，否则碎片比率计算可能会出错（不同时间两个样本的比率）
         * */
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        /* Sampling the allcator info can be slow too.
         * The fragmentation ratio it'll show is potentically more accurate
         * it excludes other RSS pages such as: shared libraries, LUA and other non-zmalloc
         * allocations, and allocator reserved pages that can be pursed (all not actual frag) 
         *
         * 对allcator信息进行采样也可能很慢。它显示的碎片比率可能更准确——它排除了
         * 其他RSS页面，如：共享库、LUA和其他非zmalloc分配，以及可以处理的分配
         * 器保留页面（所有页面都不是实际的碎片）*/
        zmalloc_get_allocator_info(&server.cron_malloc_stats.allocator_allocated,
                                   &server.cron_malloc_stats.allocator_active,
                                   &server.cron_malloc_stats.allocator_resident);
        /* in case the allocator isn't providing these stats, fake them so that
         * fragmention info still shows some (inaccurate metrics) 
         *
         * 如果分配器没有提供这些统计数据，那么就伪造它们，这样fragadote信息仍然会
         * 显示一些（不准确的指标）*/
        if (!server.cron_malloc_stats.allocator_resident) {
            /* LUA memory isn't part of zmalloc_used, but it is part of the process RSS,
             * so we must desuct it in order to be able to calculate correct
             * "allocator fragmentation" ratio 
             *
             * LUA内存不是zmalloc_used的一部分，但它是进程RSS的一部分。因此，
             * 我们必须减少它，以便能够计算正确的“分配器碎片”比率*/
            size_t lua_memory = lua_gc(server.lua,LUA_GCCOUNT,0)*1024LL;
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss - lua_memory;
        }
        if (!server.cron_malloc_stats.allocator_active)
            server.cron_malloc_stats.allocator_active = server.cron_malloc_stats.allocator_resident;
        if (!server.cron_malloc_stats.allocator_allocated)
            server.cron_malloc_stats.allocator_allocated = server.cron_malloc_stats.zmalloc_used;
    }

    /* We received a SIGTERM, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. 
     *
     * 我们收到了一个SIGTERM，以安全的方式在这里关闭，因为在信号处理程序中这样做
     * 是不好的。*/
    if (server.shutdown_asap) {
        // 保存数据库，清理服务器，并退出
        if (prepareForShutdown(SHUTDOWN_NOFLAGS) == C_OK) exit(0);
        serverLog(LL_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
        server.shutdown_asap = 0;
    }

    /* Show some info about non-empty databases 
     *
     * 显示有关非空数据库的一些信息
     * */
    run_with_period(5000) {
        for (j = 0; j < server.dbnum; j++) {
            long long size, used, vkeys;

            size = dictSlots(server.db[j].dict);
            used = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (used || vkeys) {
                serverLog(LL_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
                /* dictPrintStats(server.dict); */
            }
        }
    }

    /* Show information about connected clients 
     *
     * 显示有关已连接客户端的信息
     * */
    if (!server.sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_DEBUG,
                "%lu clients connected (%lu replicas), %zu bytes in use",
                listLength(server.clients)-listLength(server.slaves),
                listLength(server.slaves),
                zmalloc_used_memory());
        }
    }

    /* We need to do a few operations on clients asynchronously. 
     *
     * 我们需要在客户端上异步执行一些操作。
     * */
    // 运行客户端定时任务
    clientsCron();

    /* Handle background operations on Redis databases. 
     *
     * 处理Redis数据库的后台操作。
     * */
    databasesCron();

    /* Start a scheduled AOF rewrite if this was requested by the user while
     * a BGSAVE was in progress. 
     *
     * 如果在执行BGSAVE时用户请求，则启动计划的AOF重写。*/
    if (!hasActiveChildProcess() &&
        server.aof_rewrite_scheduled)
    {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. 
     *
     * 检查正在进行的后台保存或AOF重写是否已终止。
     * */
    if (hasActiveChildProcess() || ldbPendingChildren())
    {
        checkChildrenDone();
    } else {
        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now. 
         *
         * 如果没有后台保存/重写正在进行中，请检查是否必须立即保存/重写。
         * */
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            /* Save if we reached the given amount of changes,
             * the given amount of seconds, and if the latest bgsave was
             * successful or if, in case of an error, at least
             * CONFIG_BGSAVE_RETRY_DELAY seconds already elapsed. 
             *
             * 如果我们达到了给定的更改量、给定的秒数，并且最近的bgsave成功，或者在出现错
             * 误的情况下，至少已经过了 CONFIG_BGSAVE_RETRY_DELAY 秒，则保 存。
             * */
            if (server.dirty >= sp->changes &&
                server.unixtime-server.lastsave > sp->seconds &&
                (server.unixtime-server.lastbgsave_try >
                 CONFIG_BGSAVE_RETRY_DELAY ||
                 server.lastbgsave_status == C_OK))
            {
                serverLog(LL_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, (int)sp->seconds);
                rdbSaveInfo rsi, *rsiptr;
                rsiptr = rdbPopulateSaveInfo(&rsi);
                rdbSaveBackground(server.rdb_filename,rsiptr);
                break;
            }
        }

        /* Trigger an AOF rewrite if needed. 
         *
         * 如果需要，触发AOF重写。
         * */
        if (server.aof_state == AOF_ON &&
            !hasActiveChildProcess() &&
            server.aof_rewrite_perc &&
            server.aof_current_size > server.aof_rewrite_min_size)
        {
            long long base = server.aof_rewrite_base_size ?
                server.aof_rewrite_base_size : 1;
            long long growth = (server.aof_current_size*100/base) - 100;
            if (growth >= server.aof_rewrite_perc) {
                serverLog(LL_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
        }
    }
    /* Just for the sake of defensive programming, to avoid forgeting to
     * call this function when need. 
     *
     * 只是为了进行防御性编程，以避免在需要时忘记调用此函数。
     * */
    updateDictResizePolicy();


    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. 
     *
     * AOF延迟刷新：如果慢速fsync完成，请在每个cron周期尝试。
     * */

    // 如果有需要，保存 AOF 文件到硬盘
    if (server.aof_flush_postponed_start) flushAppendOnlyFile(0);

    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * a higher frequency. 
     *
     * AOF写入错误：在这种情况下，我们也有一个缓冲区要刷新，并在成功的情况下清除AOF错误，
     * 以使DB再次可写，但在“hz”设置为更高频率的情况下，每秒尝试一次就足够了。
     * */
    run_with_period(1000) {
        if (server.aof_last_write_status == C_ERR)
            flushAppendOnlyFile(0);
    }

    /* Clear the paused clients flag if needed. 
     *
     * 如果需要，请清除暂停的客户端标志。*/
    clientsArePaused(); /* Don't check return value, just use the side effect.
                         *
                         * 不要检查返回值，只使用副作用。*/

    /* Replication cron function -- used to reconnect to master,
     * detect transfer failures, start background RDB transfers and so forth. 
     *
     * 复制cron函数——用于重新连接到master、检测传输失败、启动后台RDB传输等等。
     * */
    run_with_period(1000) replicationCron();

    /* Run the Redis Cluster cron. 
     *
     * 运行集群定期任务。
     * */
    run_with_period(100) {
        if (server.cluster_enabled) clusterCron();
    }

    /* Run the Sentinel timer if we are in sentinel mode. 
     *
     * 如果我们处于哨兵模式，请运行哨兵计时器。
     * */
    if (server.sentinel_mode) sentinelTimer();

    /* Cleanup expired MIGRATE cached sockets. 
     *
     * 清理过期的MIGRATE缓存套接字。
     * */
    run_with_period(1000) {
        migrateCloseTimedoutSockets();
    }

    /* Stop the I/O threads if we don't have enough pending work. 
     *
     * 如果没有足够的挂起工作，请停止I/O线程。
     * */
    stopThreadedIOIfNeeded();

    /* Resize tracking keys table if needed. This is also done at every
     * command execution, but we want to be sure that if the last command
     * executed changes the value via CONFIG SET, the server will perform
     * the operation even if completely idle. 
     *
     * 根据需要调整跟踪关键字表的大小。这也在每次执行命令时完成，但我们希望确保，如果执
     * 行的最后一个命令通过CONFIG SET更改了值，则即使完全空闲，服务器也会执行
     * 该操作。
     * */
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Start a scheduled BGSAVE if the corresponding flag is set. This is
     * useful when we are forced to postpone a BGSAVE because an AOF
     * rewrite is in progress.
     *
     * Note: this code must be after the replicationCron() call above so
     * make sure when refactoring this file to keep this order. This is useful
     * because we want to give priority to RDB savings for replication. 
     *
     * 如果设置了相应的标志，则启动计划的BGSAVE。当我们因为AOF重写正在进行而被
     * 迫推迟BGSAVE时，这很有用。
     *
     * 注意：此代码必须在上面的replicationCron（）调用之后，
     * 因此在重构此文件时请确保保持此顺序。这很有用，因为我们希望优先考虑为复制节省RDB。
     * */
    if (!hasActiveChildProcess() &&
        server.rdb_bgsave_scheduled &&
        (server.unixtime-server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY ||
         server.lastbgsave_status == C_OK))
    {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSaveBackground(server.rdb_filename,rsiptr) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    /* Fire the cron loop modules event. 
     *
     * 激发cron循环模块事件。
     * */
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION,server.hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP,
                          0,
                          &ei);

    server.cronloops++;
    return 1000/server.hz;
}

extern int ProcessingEventsWhileBlocked;

/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors.
 *
 * Note: This function is (currently) called from two functions:
 * 1. aeMain - The main server loop
 * 2. processEventsWhileBlocked - Process clients during RDB/AOF load
 *
 * If it was called from processEventsWhileBlocked we don't want
 * to perform all actions (For example, we don't want to expire
 * keys), but we do need to perform some actions.
 *
 * The most important is freeClientsInAsyncFreeQueue but we also
 * call some other low-risk functions. 
 *
 * 每当Redis进入事件驱动库的主循环时，也就是说，在为就绪文件描述符休眠之前，都
 * 会调用此函数。
 *
 * 注意：此函数（当前）是从两个函数调用的：
 * 1。aeMain-主服务器循环
 * 2。processEventsWhileBlocked-在RDB/AOF加载期间处理客户端
 *
 * 如果它是从processEventsWhileBlocked调用的，我们不想执行所有操作（例如，我们不希望密钥过期），
 * 但我们确实需要执行一些操作。最重要的是freeClientsInAsyncFreeQueue，但我们也调用一
 * 些其他低风险函数。*/
// 每次在运行事件 loop 之前，这个函数都会被运行一次
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used;

    /* Just call a subset of vital functions in case we are re-entering
     * the event loop from processEventsWhileBlocked(). Note that in this
     * case we keep track of the number of events we are processing, since
     * processEventsWhileBlocked() wants to stop ASAP if there are no longer
     * events to handle. 
     *
     * 只需调用重要函数的子集，以防我们从processEventsWhileBlock
     * ed（）重新进入事件循环。请注意，在这种情况下，我们会跟踪正在处理的事件的数量，
     * 因为processEventsWhileBlocked（）希望在没有事件要处理的
     * 情况下尽快停止。*/
    if (ProcessingEventsWhileBlocked) {
        uint64_t processed = 0;
        processed += handleClientsWithPendingReadsUsingThreads();
        processed += tlsProcessPendingData();
        processed += handleClientsWithPendingWrites();
        processed += freeClientsInAsyncFreeQueue();
        server.events_processed_while_blocked += processed;
        return;
    }

    /* Handle precise timeouts of blocked clients. 
     *
     * 处理被阻止客户端的精确超时。
     * */
    handleBlockedClientsTimeout();

    /* We should handle pending reads clients ASAP after event loop. 
     *
     * 我们应该在事件循环后尽快处理挂起的读取客户端。*/
    handleClientsWithPendingReadsUsingThreads();

    /* Handle TLS pending data. (must be done before flushAppendOnlyFile) 
     *
     * 处理TLS挂起的数据。（必须在flushAppendOnlyFile之前完成）*/
    tlsProcessPendingData();

    /* If tls still has pending unread data don't sleep at all. 
     *
     * 如果tls仍然有挂起的未读数据，请不要休眠。*/
    aeSetDontWait(server.el, tlsHasPendingData());

    /* Call the Redis Cluster before sleep function. Note that this function
     * may change the state of Redis Cluster (from ok to fail or vice versa),
     * so it's a good idea to call it before serving the unblocked clients
     * later in this function. 
     *
     * 调用Redis Cluster beforesleep函数。请注意，此函数可能会
     * 更改Redis Cluster的状态（从ok更改为fail，反之亦然），因此最好
     * 在稍后为未阻止的客户端提供服务之前调用它。*/
    if (server.cluster_enabled) clusterBeforeSleep();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). 
     *
     * 运行快速过期周期（如果不需要快速周期，则调用的函数将尽快返回）。*/
    if (server.active_expire_enabled && server.masterhost == NULL)
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    /* Unblock all the clients blocked for synchronous replication
     * in WAIT. 
     *
     * 取消阻止WAIT中为同步复制而阻止的所有客户端。*/
    if (listLength(server.clients_waiting_acks))
        processClientsWaitingReplicas();

    /* Check if there are clients unblocked by modules that implement
     * blocking commands. 
     *
     * 检查是否有被实现阻止命令的模块阻止的客户端。*/
    if (moduleCount()) moduleHandleBlockedClients();

    /* Try to process pending commands for clients that were just unblocked. 
     *
     * 尝试处理刚解除阻止的客户端的挂起命令。*/
    if (listLength(server.unblocked_clients))
        processUnblockedClients();

    /* Send all the slaves an ACK request if at least one client blocked
     * during the previous event loop iteration. Note that we do this after
     * processUnblockedClients(), so if there are multiple pipelined WAITs
     * and the just unblocked WAIT gets blocked again, we don't have to wait
     * a server cron cycle in absence of other event loop events. See #6623. 
     *
     * 如果在上一次事件循环迭代期间至少有一个客户端被阻止，则向所有从属服务器发送ACK
     * 请求。请注意，我们在processUnblockedClients（）之后执行此
     * 操作，因此，如果存在多个流水线WAIT，而刚刚解除阻塞的WAIT再次被阻塞，则在
     * 没有其他事件循环事件的情况下，我们不必等待服务器cron循环。参见#6623。*/
    if (server.get_ack_from_slaves) {
        robj *argv[3];

        argv[0] = createStringObject("REPLCONF",8);
        argv[1] = createStringObject("GETACK",6);
        argv[2] = createStringObject("*",1); /* Not used argument. 
                                              *
                                              * 未使用的参数。*/
        replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3);
        decrRefCount(argv[0]);
        decrRefCount(argv[1]);
        decrRefCount(argv[2]);
        server.get_ack_from_slaves = 0;
    }

    /* Send the invalidation messages to clients participating to the
     * client side caching protocol in broadcasting (BCAST) mode. 
     *
     * 以广播（BCAST）模式向参与客户端缓存协议的客户端发送无效消息。*/
    trackingBroadcastInvalidationMessages();

    /* Write the AOF buffer on disk 
     *
     * 在磁盘上写入AOF缓冲区*/
    flushAppendOnlyFile(0);

    /* Handle writes with pending output buffers. 
     *
     * 使用挂起的输出缓冲区处理写入。*/
    handleClientsWithPendingWritesUsingThreads();

    /* Close clients that need to be closed asynchronous 
     *
     * 关闭需要异步关闭的客户端*/
    freeClientsInAsyncFreeQueue();

    /* Try to process blocked clients every once in while. Example: A module
     * calls RM_SignalKeyAsReady from within a timer callback (So we don't
     * visit processCommand() at all). 
     *
     * 尝试每隔一段时间处理被阻止的客户端。示例：一个模块从计时器回调中调用
     * RM_SignalKeyAsReady（因此我们根本不访问processCommand（））。*/
    handleClientsBlockedOnKeys();

    /* Before we are going to sleep, let the threads access the dataset by
     * releasing the GIL. Redis main thread will not touch anything at this
     * time. 
     *
     * 在我们睡觉之前，让线程通过释放GIL来访问数据集。Redis主线程此时不会接触任
     * 何东西。*/
    if (moduleCount()) moduleReleaseGIL();

    /* Do NOT add anything below moduleReleaseGIL !!! 
     *
     * 不要在模块ReleaseGIL下面添加任何内容！！！*/
}

/* This function is called immediately after the event loop multiplexing
 * API returned, and the control is going to soon return to Redis by invoking
 * the different events callbacks. 
 *
 * 此函数在事件循环复用API返回后立即调用，控件将很快通过调用不同的事件回调返回Redis。*/
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    /* Do NOT add anything above moduleAcquireGIL !!! 
     *
     * 不要在模块AcquireGIL上面添加任何内容！！！*/

    /* Aquire the modules GIL so that their threads won't touch anything. 
     *
     * 获取模块GIL，这样它们的线程就不会碰到任何东西。*/
    if (!ProcessingEventsWhileBlocked) {
        if (moduleCount()) moduleAcquireGIL();
    }
}

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    shared.crlf = createObject(OBJ_STRING,sdsnew("\r\n"));
    shared.ok = createObject(OBJ_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(OBJ_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(OBJ_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(OBJ_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(OBJ_STRING,sdsnew(":1\r\n"));
    shared.emptyarray = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(OBJ_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(OBJ_STRING,sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(OBJ_STRING,sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    shared.wrongtypeerr = createObject(OBJ_STRING,sdsnew(
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
    shared.nokeyerr = createObject(OBJ_STRING,sdsnew(
        "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(OBJ_STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(OBJ_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(OBJ_STRING,sdsnew(
        "-ERR index out of range\r\n"));
    shared.noscripterr = createObject(OBJ_STRING,sdsnew(
        "-NOSCRIPT No matching script. Please use EVAL.\r\n"));
    shared.loadingerr = createObject(OBJ_STRING,sdsnew(
        "-LOADING Redis is loading the dataset in memory\r\n"));
    shared.slowscripterr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.masterdownerr = createObject(OBJ_STRING,sdsnew(
        "-MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.\r\n"));
    shared.bgsaveerr = createObject(OBJ_STRING,sdsnew(
        "-MISCONF Redis is configured to save RDB snapshots, but it is currently not able to persist on disk. Commands that may modify the data set are disabled, because this instance is configured to report errors during writes if RDB snapshotting fails (stop-writes-on-bgsave-error option). Please check the Redis logs for details about the RDB error.\r\n"));
    shared.roslaveerr = createObject(OBJ_STRING,sdsnew(
        "-READONLY You can't write against a read only replica.\r\n"));
    shared.noautherr = createObject(OBJ_STRING,sdsnew(
        "-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(OBJ_STRING,sdsnew(
        "-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(OBJ_STRING,sdsnew(
        "-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING,sdsnew(
        "-NOREPLICAS Not enough good replicas to write.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING,sdsnew(
        "-BUSYKEY Target key name already exists.\r\n"));
    shared.space = createObject(OBJ_STRING,sdsnew(" "));
    shared.colon = createObject(OBJ_STRING,sdsnew(":"));
    shared.plus = createObject(OBJ_STRING,sdsnew("+"));

    /* The shared NULL depends on the protocol version. 
     *
     * 共享的NULL取决于协议版本。*/
    shared.null[0] = NULL;
    shared.null[1] = NULL;
    shared.null[2] = createObject(OBJ_STRING,sdsnew("$-1\r\n"));
    shared.null[3] = createObject(OBJ_STRING,sdsnew("_\r\n"));

    shared.nullarray[0] = NULL;
    shared.nullarray[1] = NULL;
    shared.nullarray[2] = createObject(OBJ_STRING,sdsnew("*-1\r\n"));
    shared.nullarray[3] = createObject(OBJ_STRING,sdsnew("_\r\n"));

    shared.emptymap[0] = NULL;
    shared.emptymap[1] = NULL;
    shared.emptymap[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptymap[3] = createObject(OBJ_STRING,sdsnew("%0\r\n"));

    shared.emptyset[0] = NULL;
    shared.emptyset[1] = NULL;
    shared.emptyset[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptyset[3] = createObject(OBJ_STRING,sdsnew("~0\r\n"));

    for (j = 0; j < PROTO_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str,sizeof(dictid_str),j);
        shared.select[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, dictid_str));
    }
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);
    shared.del = createStringObject("DEL",3);
    shared.unlink = createStringObject("UNLINK",6);
    shared.rpop = createStringObject("RPOP",4);
    shared.lpop = createStringObject("LPOP",4);
    shared.lpush = createStringObject("LPUSH",5);
    shared.rpoplpush = createStringObject("RPOPLPUSH",9);
    shared.zpopmin = createStringObject("ZPOPMIN",7);
    shared.zpopmax = createStringObject("ZPOPMAX",7);
    shared.multi = createStringObject("MULTI",5);
    shared.exec = createStringObject("EXEC",4);
    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] =
            makeObjectShared(createObject(OBJ_STRING,(void*)(long)j));
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"*%d\r\n",j));
        shared.bulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"$%d\r\n",j));
    }
    /* The following two shared objects, minstring and maxstrings, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. 
     *
     * 以下两个共享对象minstring和maxstring实际上并没有用于它们的值，
     * 而是作为一个特殊对象，分别表示ZRANGEBYLEX命令的字符串比较中的最小可能
     * 字符串和最大可能字符串。*/
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

/*
 * 初始化 server 结构
 */
void initServerConfig(void) {
    int j;

    updateCachedTime(1);
    getRandomHexChars(server.runid,CONFIG_RUN_ID_SIZE);
    server.runid[CONFIG_RUN_ID_SIZE] = '\0';
    changeReplicationId();
    clearReplicationId2();
    server.hz = CONFIG_DEFAULT_HZ; /* Initialize it ASAP, even if it may get
                                      updated later after loading the config.
                                      This value may be used before the server
                                      is initialized. 
                                    *
                                    * 尽快初始化它，即使它可能在加载配置后更新。此值可以在初始化服务器之前使用。*/
    server.timezone = getTimeZone(); /* Initialized by tzset(). 
                                      *
                                      * 已由tzset（）初始化。*/
    server.configfile = NULL;
    server.executable = NULL;
    // 判断架构
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;
    server.bindaddr_count = 0;
    server.unixsocketperm = CONFIG_DEFAULT_UNIX_SOCKET_PERM;
    server.ipfd_count = 0;
    server.tlsfd_count = 0;
    server.sofd = -1;
    server.active_expire_enabled = 1;
    server.client_max_querybuf_len = PROTO_MAX_QUERYBUF_LEN;
    server.saveparams = NULL;
    server.loading = 0;
    server.logfile = zstrdup(CONFIG_DEFAULT_LOGFILE);

    // AOF 状态
    server.aof_state = AOF_OFF;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_flush_sleep = 0;
    server.aof_last_fsync = time(NULL);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = C_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1; /* Make sure the first time will not match 
                                  *
                                  * 确保第一次不会匹配*/
    server.aof_flush_postponed_start = 0;
    server.pidfile = NULL;
    server.active_defrag_running = 0;
    server.notify_keyspace_events = 0;
    server.blocked_clients = 0;
    memset(server.blocked_clients_by_type,0,
           sizeof(server.blocked_clients_by_type));
    server.shutdown_asap = 0;
    server.cluster_configfile = zstrdup(CONFIG_DEFAULT_CLUSTER_CONFIG_FILE);
    server.cluster_module_flags = CLUSTER_MODULE_FLAG_NONE;
    server.migrate_cached_sockets = dictCreate(&migrateCacheDictType,NULL);
    server.next_client_id = 1; /* Client IDs, start from 1 .
                                *
                                * 客户端ID，从1开始。*/
    server.loading_process_events_interval_bytes = (1024*1024*2);

    server.lruclock = getLRUClock();
    resetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change  1小时1次更改后保存*/
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes 5分钟和100次更改后保存*/
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes 在1分钟和10000次更改后保存*/

    /* Replication related 
     *
     * 与复制相关*/
    server.masterauth = NULL;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.cached_master = NULL;
    server.master_initial_offset = -1;
    server.repl_state = REPL_STATE_NONE;
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_transfer_s = NULL;
    server.repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    server.repl_down_since = 0; /* Never connected, repl is down since EVER. 
                                 *
                                 * 从未连接过，repl从此关闭。*/
    server.master_repl_offset = 0;

    /* Replication partial resync backlog 
     *
     * 复制部分重新同步囤积*/
    server.repl_backlog = NULL;
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;
    server.repl_backlog_off = 0;
    server.repl_no_slaves_since = time(NULL);

    /* Client output buffer limits 
     *
     * 客户端输出缓冲区限制*/
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++)
        server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    /* Linux OOM Score config 
     *
     * Linux OOM分数配置*/
    for (j = 0; j < CONFIG_OOM_COUNT; j++)
        server.oom_score_adj_values[j] = configOOMScoreAdjValuesDefaults[j];

    /* Double constants initialization 
     *
     * 双常量初始化*/
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    /* Command table -- we initialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * redis.conf using the rename-command directive. 
     *
     * 命令表——我们在这里初始化它，因为它是初始配置的一部分，因为命令名称可以通过redis.conf使用rename命令指令进行更改。*/
    server.commands = dictCreate(&commandTableDictType,NULL);
    server.orig_commands = dictCreate(&commandTableDictType,NULL);
    populateCommandTable();
    server.delCommand = lookupCommandByCString("del");
    server.multiCommand = lookupCommandByCString("multi");
    server.lpushCommand = lookupCommandByCString("lpush");
    server.lpopCommand = lookupCommandByCString("lpop");
    server.rpopCommand = lookupCommandByCString("rpop");
    server.zpopminCommand = lookupCommandByCString("zpopmin");
    server.zpopmaxCommand = lookupCommandByCString("zpopmax");
    server.sremCommand = lookupCommandByCString("srem");
    server.execCommand = lookupCommandByCString("exec");
    server.expireCommand = lookupCommandByCString("expire");
    server.pexpireCommand = lookupCommandByCString("pexpire");
    server.xclaimCommand = lookupCommandByCString("xclaim");
    server.xgroupCommand = lookupCommandByCString("xgroup");
    server.rpoplpushCommand = lookupCommandByCString("rpoplpush");

    /* Debugging 
     *
     * 调试*/
    server.assert_failed = "<no assertion failed>";
    server.assert_file = "<no file>";
    server.assert_line = 0;
    server.bug_report_start = 0;
    server.watchdog_period = 0;

    /* By default we want scripts to be always replicated by effects
     * (single commands executed by the script), and not by sending the
     * script to the slave / AOF. This is the new way starting from
     * Redis 5. However it is possible to revert it via redis.conf. 
     *
     * 默认情况下，我们希望脚本始终通过效果（脚本执行的单个命令）进行复制，而不是通过将
     * 脚本发送到从属/AOF。这是从Redis 5开始的新方式。但是，可以通过redis.conf将其还原。*/
    server.lua_always_replicate_commands = 1;

    initConfigValues();
}

extern char **environ;

/* Restart the server, executing the same executable that started this
 * instance, with the same arguments and configuration file.
 *
 * The function is designed to directly call execve() so that the new
 * server instance will retain the PID of the previous one.
 *
 * The list of flags, that may be bitwise ORed together, alter the
 * behavior of this function:
 *
 * RESTART_SERVER_NONE              No flags.
 * RESTART_SERVER_GRACEFULLY        Do a proper shutdown before restarting.
 * RESTART_SERVER_CONFIG_REWRITE    Rewrite the config file before restarting.
 *
 * On success the function does not return, because the process turns into
 * a different process. On error C_ERR is returned. 
 *
 * 重新启动服务器，使用相同的参数和配置文件执行启动此实例的相同可执行文件。
 *
 * 该函数被设计为直接调用execve（），以便新的服务器实例将保留上一个实例的PID。
 *
 * 可以按位或组合在一起的标志列表会改变此函数的行为：
 * RESTART_SERVER_NONE无标志。
 * RESTART_SERVER_GRACEFULLY在重新启动之前进行适当的关闭。
 * RESTART_SERVER_CONFIG_REWRITE在重新启动之前重写配置文件。
 *
 * 一旦成功，函数就不会返回，因为过程会变成一个不同的过程。返回错误时C_ERR。*/
int restartServer(int flags, mstime_t delay) {
    int j;

    /* Check if we still have accesses to the executable that started this
     * server instance. 
     *
     * 检查我们是否仍然可以访问启动此服务器实例的可执行文件。*/
    if (access(server.executable,X_OK) == -1) {
        serverLog(LL_WARNING,"Can't restart: this process has no "
                             "permissions to execute %s", server.executable);
        return C_ERR;
    }

    /* Config rewriting. 
     *
     * 配置重写。*/
    if (flags & RESTART_SERVER_CONFIG_REWRITE &&
        server.configfile &&
        rewriteConfig(server.configfile, 0) == -1)
    {
        serverLog(LL_WARNING,"Can't restart: configuration rewrite process "
                             "failed");
        return C_ERR;
    }

    /* Perform a proper shutdown. 
     *
     * 进行适当的停机。*/
    if (flags & RESTART_SERVER_GRACEFULLY &&
        prepareForShutdown(SHUTDOWN_NOFLAGS) != C_OK)
    {
        serverLog(LL_WARNING,"Can't restart: error preparing for shutdown");
        return C_ERR;
    }

    /* Close all file descriptors, with the exception of stdin, stdout, strerr
     * which are useful if we restart a Redis server which is not daemonized. 
     *
     * 关闭所有文件描述符，stdin、stdout和strerr除外，如果我们重新启动
     * 未被守护程序化的Redis服务器，这些描述符会很有用。*/
    for (j = 3; j < (int)server.maxclients + 1024; j++) {
        /* Test the descriptor validity before closing it, otherwise
         * Valgrind issues a warning on close(). 
         *
         * 在关闭描述符之前测试描述符的有效性，否则Valgrind会在close（）上发出警告。*/
        if (fcntl(j,F_GETFD) != -1) close(j);
    }

    /* Execute the server with the original command line. 
     *
     * 使用原始命令行执行服务器。*/
    if (delay) usleep(delay*1000);
    zfree(server.exec_argv[0]);
    server.exec_argv[0] = zstrdup(server.executable);
    execve(server.executable,server.exec_argv,environ);

    /* If an error occurred here, there is nothing we can do, but exit. 
     *
     * 如果这里发生错误，我们只能退出。*/
    _exit(1);

    return C_ERR; /* Never reached. 
                   *
                   * 从未到达这里。*/
}

static void readOOMScoreAdj(void) {
#ifdef HAVE_PROC_OOM_SCORE_ADJ
    char buf[64];
    int fd = open("/proc/self/oom_score_adj", O_RDONLY);

    if (fd < 0) return;
    if (read(fd, buf, sizeof(buf)) > 0)
        server.oom_score_adj_base = atoi(buf);
    close(fd);
#endif
}

/* This function will configure the current process's oom_score_adj according
 * to user specified configuration. This is currently implemented on Linux
 * only.
 *
 * A process_class value of -1 implies OOM_CONFIG_MASTER or OOM_CONFIG_REPLICA,
 * depending on current role.
 
 *
 * 此功能将根据用户指定的配置来配置当前进程的oom_score_adj。这目前仅在
 * Linux上实现。process_class值-1表示OOM_CONFIG_MA
 * STER或OOM_CONFIG _REPLICA，具体取决于当前角色。*/
int setOOMScoreAdj(int process_class) {

    if (server.oom_score_adj == OOM_SCORE_ADJ_NO) return C_OK;
    if (process_class == -1)
        process_class = (server.masterhost ? CONFIG_OOM_REPLICA : CONFIG_OOM_MASTER);

    serverAssert(process_class >= 0 && process_class < CONFIG_OOM_COUNT);

#ifdef HAVE_PROC_OOM_SCORE_ADJ
    int fd;
    int val;
    char buf[64];

    val = server.oom_score_adj_values[process_class];
    if (server.oom_score_adj == OOM_SCORE_RELATIVE)
        val += server.oom_score_adj_base;
    if (val > 1000) val = 1000;
    if (val < -1000) val = -1000;

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LOG_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
        if (fd != -1) close(fd);
        return C_ERR;
    }

    close(fd);
    return C_OK;
#else
    /* Unsupported 
     *
     * 不支持*/
    return C_ERR;
#endif
}

/* This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It also reserves a number of file
 * descriptors (CONFIG_MIN_RESERVED_FDS) for extra operations of
 * persistence, listening sockets, log files and so forth.
 *
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * server.maxclients to the value that we can actually handle. 
 *
 * 此函数将尝试将打开文件的最大数量相应地提高到配置的客户端的最大数量。它还保留了许
 * 多文件描述符（CONFIG_MIN_RESERVED_FDS），用于持久性、侦听
 * 套接字、日志文件等的额外操作。如果无法根据配置的最大客户端数量设置相应的限制，该
 * 函数将执行相反的操作，将server.maxclients设置为我们实际可以处理
 * 的值。*/
// 用检测办法，获取最大打开文件的值
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = server.maxclients+CONFIG_MIN_RESERVED_FDS;
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE,&limit) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the current NOFILE limit (%s), assuming 1024 and setting the max clients configuration accordingly.",
            strerror(errno));
        server.maxclients = 1024-CONFIG_MIN_RESERVED_FDS;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* Set the max number of files if the current limit is not enough
         * for our needs. 
         *
         * 如果当前限制不足以满足我们的需求，请设置最大文件数。*/
        if (oldlimit < maxfiles) {
            rlim_t bestlimit;
            int setrlimit_error = 0;

            /* Try to set the file limit to match 'maxfiles' or at least
             * to the higher value supported less than maxfiles. 
             *
             * 请尝试将文件限制设置为匹配“maxfiles”，或者至少设置为支持的比maxfiles更小的值。
             * */
            bestlimit = maxfiles;
            while(bestlimit > oldlimit) {
                rlim_t decr_step = 16;

                limit.rlim_cur = bestlimit;
                limit.rlim_max = bestlimit;
                if (setrlimit(RLIMIT_NOFILE,&limit) != -1) break;
                setrlimit_error = errno;

                /* We failed to set file limit to 'bestlimit'. Try with a
                 * smaller limit decrementing by a few FDs per iteration. 
                 *
                 * 我们未能将文件限制设置为“最佳限制”。尝试使用较小的限制，每次迭代递减几个FD。*/
                if (bestlimit < decr_step) break;
                bestlimit -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. 
             *
             * 假设我们最初得到的限制仍然有效，如果我们最后一次尝试更低的话。*/
            if (bestlimit < oldlimit) bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = server.maxclients;
                server.maxclients = bestlimit-CONFIG_MIN_RESERVED_FDS;
                /* maxclients is unsigned so may overflow: in order
                 * to check if maxclients is now logically less than 1
                 * we test indirectly via bestlimit. 
                 *
                 * maxclient是无符号的，因此可能溢出：为了检查maxclient现在是否在
                 * 逻辑上小于1，我们通过bestlimit间接测试。*/
                if (bestlimit <= CONFIG_MIN_RESERVED_FDS) {
                    serverLog(LL_WARNING,"Your current 'ulimit -n' "
                        "of %llu is not enough for the server to start. "
                        "Please increase your open file limit to at least "
                        "%llu. Exiting.",
                        (unsigned long long) oldlimit,
                        (unsigned long long) maxfiles);
                    exit(1);
                }
                serverLog(LL_WARNING,"You requested maxclients of %d "
                    "requiring at least %llu max file descriptors.",
                    old_maxclients,
                    (unsigned long long) maxfiles);
                serverLog(LL_WARNING,"Server can't set maximum open files "
                    "to %llu because of OS error: %s.",
                    (unsigned long long) maxfiles, strerror(setrlimit_error));
                serverLog(LL_WARNING,"Current maximum open files is %llu. "
                    "maxclients has been reduced to %d to compensate for "
                    "low ulimit. "
                    "If you need higher maxclients increase 'ulimit -n'.",
                    (unsigned long long) bestlimit, server.maxclients);
            } else {
                serverLog(LL_NOTICE,"Increased maximum number of open files "
                    "to %llu (it was originally set to %llu).",
                    (unsigned long long) maxfiles,
                    (unsigned long long) oldlimit);
            }
        }
    }
}

/* Check that server.tcp_backlog can be actually enforced in Linux according
 * to the value of /proc/sys/net/core/somaxconn, or warn about it. 
 *
 * 检查server.tcp_backlog是否可以根据/proc/sys/net/core/somaxconn的值在Linux中实际执行，或者对此发出警告。*/
void checkTcpBacklogSettings(void) {
#ifdef HAVE_PROC_SOMAXCONN
    FILE *fp = fopen("/proc/sys/net/core/somaxconn","r");
    char buf[1024];
    if (!fp) return;
    if (fgets(buf,sizeof(buf),fp) != NULL) {
        int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because /proc/sys/net/core/somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
    fclose(fp);
#endif
}

/* Initialize a set of file descriptors to listen to the specified 'port'
 * binding the addresses specified in the Redis server configuration.
 *
 * The listening file descriptors are stored in the integer array 'fds'
 * and their number is set in '*count'.
 *
 * The addresses to bind are specified in the global server.bindaddr array
 * and their number is server.bindaddr_count. If the server configuration
 * contains no specific addresses to bind, this function will try to
 * bind * (all addresses) for both the IPv4 and IPv6 protocols.
 *
 * On success the function returns C_OK.
 *
 * On error the function returns C_ERR. For the function to be on
 * error, at least one of the server.bindaddr addresses was
 * impossible to bind, or no bind addresses were specified in the server
 * configuration but the function is not able to bind * for at least
 * one of the IPv4 or IPv6 protocols. 
 *
 * 初始化一组文件描述符，以侦听绑定Redis服务器配置中指定地址的指定“端口”。侦
 * 听文件描述符存储在整数数组“fds”中，其编号设置为“*count”。
 *
 * 要绑定的地 址在全局server.bindaddr数组中指定，其编号为server.bind
 * addr_count。如果服务器配置不包含要绑定的特定地址，则此函数将尝试为IPv4和IPv6协议绑定（所有地址）。
 *
 * 成功后，函数返回C_OK。出现错误时，函数返回C_ERR。
 *
 * 对于出现错误的函数，至少有一个服务器.bindaddr地址无法绑定，或者在服务器配置中未指定绑定地址，
 * 但该函数无法绑定IPv4或IPv6协议中的至少一个。
 * */
int listenToPort(int port, int *fds, int *count) {
    int j;

    /* Force binding of 0.0.0.0 if no bind address is specified, always
     * entering the loop if j == 0. 
     *
     * 如果未指定绑定地址，则强制绑定0.0.0.0，如果j==0，则始终进入循环。*/
    if (server.bindaddr_count == 0) server.bindaddr[0] = NULL;
    for (j = 0; j < server.bindaddr_count || j == 0; j++) {
        if (server.bindaddr[j] == NULL) {
            int unsupported = 0;
            /* Bind * for both IPv6 and IPv4, we enter here only if
             * server.bindaddr_count == 0. 
             *
             * 绑定IPv6和IPv4，只有当server.bindaddr_count==0时，我们才会在此处输入。*/
            fds[*count] = anetTcp6Server(server.neterr,port,NULL,
                server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                anetNonBlock(NULL,fds[*count]);
                (*count)++;
            } else if (errno == EAFNOSUPPORT) {
                unsupported++;
                serverLog(LL_WARNING,"Not listening to IPv6: unsupported");
            }

            if (*count == 1 || unsupported) {
                /* Bind the IPv4 address as well. 
                 *
                 * 同时绑定IPv4地址。*/
                fds[*count] = anetTcpServer(server.neterr,port,NULL,
                    server.tcp_backlog);
                if (fds[*count] != ANET_ERR) {
                    anetNonBlock(NULL,fds[*count]);
                    (*count)++;
                } else if (errno == EAFNOSUPPORT) {
                    unsupported++;
                    serverLog(LL_WARNING,"Not listening to IPv4: unsupported");
                }
            }
            /* Exit the loop if we were able to bind * on IPv4 and IPv6,
             * otherwise fds[*count] will be ANET_ERR and we'll print an
             * error and return to the caller with an error. 
             *
             * 如果我们能够绑定IPv4和IPv6，请退出循环，否则fds[*count]将为ANET_ERR，我们将打印一个错误并返回给调用者。*/
            if (*count + unsupported == 2) break;
        } else if (strchr(server.bindaddr[j],':')) {
            /* Bind IPv6 address. 
             *
             * 绑定IPv6地址。*/
            fds[*count] = anetTcp6Server(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        } else {
            /* Bind IPv4 address. 
             *
             * 绑定IPv4地址。*/
            fds[*count] = anetTcpServer(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        }
        if (fds[*count] == ANET_ERR) {
            int net_errno = errno;
            serverLog(LL_WARNING,
                "Could not create server TCP listening socket %s:%d: %s",
                server.bindaddr[j] ? server.bindaddr[j] : "*",
                port, server.neterr);
                if (net_errno == ENOPROTOOPT     || net_errno == EPROTONOSUPPORT ||
                    net_errno == ESOCKTNOSUPPORT || net_errno == EPFNOSUPPORT ||
                    net_errno == EAFNOSUPPORT    || net_errno == EADDRNOTAVAIL)
                    continue;
            return C_ERR;
        }
        anetNonBlock(NULL,fds[*count]);
        (*count)++;
    }
    return C_OK;
}

/* Resets the stats that we expose via INFO or other means that we want
 * to reset via CONFIG RESETSTAT. The function is also used in order to
 * initialize these fields in initServer() at server startup. 
 *
 * 重置我们通过INFO或其他方式暴露的统计信息，我们希望通过CONFIG RESETSTAT重置这些统计信息。
 * 该函数还用于在服务器启动时初始化initServer
 * （）中的这些字段。*/
void resetServerStats(void) {
    int j;

    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_expired_stale_perc = 0;
    server.stat_expired_time_cap_reached_count = 0;
    server.stat_expire_cycle_time_used = 0;
    server.stat_evictedkeys = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_active_defrag_hits = 0;
    server.stat_active_defrag_misses = 0;
    server.stat_active_defrag_key_hits = 0;
    server.stat_active_defrag_key_misses = 0;
    server.stat_active_defrag_scanned = 0;
    server.stat_fork_time = 0;
    server.stat_fork_rate = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    server.stat_io_reads_processed = 0;
    server.stat_total_reads_processed = 0;
    server.stat_io_writes_processed = 0;
    server.stat_total_writes_processed = 0;
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        server.inst_metric[j].idx = 0;
        server.inst_metric[j].last_sample_time = mstime();
        server.inst_metric[j].last_sample_count = 0;
        memset(server.inst_metric[j].samples,0,
            sizeof(server.inst_metric[j].samples));
    }
    server.stat_net_input_bytes = 0;
    server.stat_net_output_bytes = 0;
    server.stat_unexpected_error_replies = 0;
    server.aof_delayed_fsync = 0;
}

/* Make the thread killable at any time, so that kill threads functions
 * can work reliably (default cancelability type is PTHREAD_CANCEL_DEFERRED).
 * Needed for pthread_cancel used by the fast memory test used by the crash report. 
 *
 * 使线程在任何时候都可以kill，这样kill线程函数就可以可靠地工作（默认的cancellability类型是PTHREAD_CANCEL_DEFERRED）。
 * 崩溃报告使用的快速内存测试所使用的pthread_cancel需要。
 * */
void makeThreadKillable(void) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
}

// 初始化服务器功能
void initServer(void) {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();
    makeThreadKillable();

    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            server.syslog_facility);
    }

    /* Initialization after setting defaults from the config system. 
     *
     * 在配置系统中设置默认值后进行初始化。*/
    server.aof_state = server.aof_enabled ? AOF_ON : AOF_OFF;
    server.hz = server.config_hz;
    server.pid = getpid();
    server.in_fork_child = CHILD_TYPE_NONE;
    server.main_thread_id = pthread_self();
    // 当前处理的客户端
    server.current_client = NULL;
    server.fixed_time_expire = 0;
    // 所有客户端
    server.clients = listCreate();
    server.clients_index = raxNew();
    // 要被关闭的客户端
    server.clients_to_close = listCreate();
    // 附属节点
    server.slaves = listCreate();
    // monitor 客户端
    server.monitors = listCreate();
    server.clients_pending_write = listCreate();
    server.clients_pending_read = listCreate();
    server.clients_timeout_table = raxNew();
    server.slaveseldb = -1; /* Force to emit the first SELECT command. 
                             *
                             * 强制发出第一个SELECT命令。*/
    // 被取消阻塞的客户端
    server.unblocked_clients = listCreate();
    // 所有已就绪 key
    server.ready_keys = listCreate();
    server.clients_waiting_acks = listCreate();
    server.get_ack_from_slaves = 0;
    server.clients_paused = 0;
    server.events_processed_while_blocked = 0;
    server.system_memory_size = zmalloc_get_memory_size();

    if ((server.tls_port || server.tls_replication || server.tls_cluster)
                && tlsConfigure(&server.tls_ctx_config) == C_ERR) {
        serverLog(LL_WARNING, "Failed to configure TLS. Check logs for more info.");
        exit(1);
    }

    // 初始化共享对象
    createSharedObjects();
    // 获取最大打开文件数目
    adjustOpenFilesLimit();
    // 初始化事件状态
    server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
    if (server.el == NULL) {
        serverLog(LL_WARNING,
            "Failed creating the event loop. Error message: '%s'",
            strerror(errno));
        exit(1);
    }
    // 初始化数据库
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);

    /* Open the TCP listening socket for the user commands. 
     *
     * 打开用户命令的TCP侦听套接字。*/
    // 初始化网络连接
    if (server.port != 0 &&
        listenToPort(server.port,server.ipfd,&server.ipfd_count) == C_ERR)
        exit(1);
    if (server.tls_port != 0 &&
        listenToPort(server.tls_port,server.tlsfd,&server.tlsfd_count) == C_ERR)
        exit(1);

    /* Open the listening Unix domain socket. 
     *
     * 打开正在侦听的Unix域套接字。*/
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket); /* don't care if this fails 
                                    *
                                    * 不管这是否失败*/
        server.sofd = anetUnixServer(server.neterr,server.unixsocket,
            server.unixsocketperm, server.tcp_backlog);
        if (server.sofd == ANET_ERR) {
            serverLog(LL_WARNING, "Opening Unix socket: %s", server.neterr);
            exit(1);
        }
        anetNonBlock(NULL,server.sofd);
    }

    /* Abort if there are no listening sockets at all. 
     *
     * 如果根本没有侦听套接字，则中止。*/
    if (server.ipfd_count == 0 && server.tlsfd_count == 0 && server.sofd < 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }

    /* Create the Redis databases, and initialize other internal state. 
     *
     * 创建Redis数据库，并初始化其他内部状态。*/
    for (j = 0; j < server.dbnum; j++) {
        // key space
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        // 过期空间
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        server.db[j].expires_cursor = 0;
        // 被阻塞键
        server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        // 可解除阻塞的键
        server.db[j].ready_keys = dictCreate(&objectKeyPointerValueDictType,NULL);
        // 被 WATCH 命令监视的键
        server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);
        // 数据库 ID
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        listSetFreeMethod(server.db[j].defrag_later,(void (*)(void*))sdsfree);
    }
    evictionPoolAlloc(); /* Initialize the LRU keys pool. 
                          *
                          * 初始化LRU key 池。*/
    // pubsub
    server.pubsub_channels = dictCreate(&keylistDictType,NULL);
    server.pubsub_patterns = listCreate();
    server.pubsub_patterns_dict = dictCreate(&keylistDictType,NULL);
    listSetFreeMethod(server.pubsub_patterns,freePubsubPattern);
    listSetMatchMethod(server.pubsub_patterns,listMatchPubsubPattern);
    // CRON 执行计数
    server.cronloops = 0;
    // BGSAVE 执行指示变量
    server.rdb_child_pid = -1;
    // BGREWRITEAOF 执行指示变量
    server.aof_child_pid = -1;
    server.module_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
    server.rdb_bgsave_scheduled = 0;
    server.child_info_pipe[0] = -1;
    server.child_info_pipe[1] = -1;
    server.child_info_data.magic = 0;
    // 初始化 AOF 重写缓存
    aofRewriteBufferReset();
    server.aof_buf = sdsempty();
    // 最后一次成功保存的时间
    server.lastsave = time(NULL); /* At startup we consider the DB saved. 
                                   *
                                   * 在启动时，我们认为DB已保存。*/
    server.lastbgsave_try = 0;    /* At startup we never tried to BGSAVE. 
                                   *
                                   * 在启动时，我们从未尝试过BGSAVE。*/
    // 结束 SAVE 的时间
    server.rdb_save_time_last = -1;
    // 开始 SAVE 的时间
    server.rdb_save_time_start = -1;
    server.dirty = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. 
     *
     * 一些我们不想重置的统计数据：服务器启动时间和峰值内存。*/
    // 统计变量
    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.stat_rdb_cow_bytes = 0;
    server.stat_aof_cow_bytes = 0;
    server.stat_module_cow_bytes = 0;
    for (int j = 0; j < CLIENT_TYPE_COUNT; j++)
        server.stat_clients_type_memory[j] = 0;
    server.cron_malloc_stats.zmalloc_used = 0;
    server.cron_malloc_stats.process_rss = 0;
    server.cron_malloc_stats.allocator_allocated = 0;
    server.cron_malloc_stats.allocator_active = 0;
    server.cron_malloc_stats.allocator_resident = 0;
    server.lastbgsave_status = C_OK;
    server.aof_last_write_status = C_OK;
    server.aof_last_write_errno = 0;
    server.repl_good_slaves_count = 0;

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. 
     *
     * 创建定时器回调，这是我们增量处理许多后台操作的方法，如客户端超时、驱逐未处理的过期密钥等等。*/
    // 关联 server cron 到时间事件
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Create an event handler for accepting new connections in TCP and Unix
     * domain sockets. 
     *
     * 创建一个事件处理程序，用于接受TCP和Unix域套接字中的新连接。*/
    // 关联网络连接事件
    for (j = 0; j < server.ipfd_count; j++) {
        // 注册接收链接的事件和对应的处理函数 acceptTcpHandler
        if (aeCreateFileEvent(server.el, server.ipfd[j], AE_READABLE,
            acceptTcpHandler,NULL) == AE_ERR)
            {
                serverPanic(
                    "Unrecoverable error creating server.ipfd file event.");
            }
    }
    for (j = 0; j < server.tlsfd_count; j++) {
        if (aeCreateFileEvent(server.el, server.tlsfd[j], AE_READABLE,
            acceptTLSHandler,NULL) == AE_ERR)
            {
                serverPanic(
                    "Unrecoverable error creating server.tlsfd file event.");
            }
    }
    if (server.sofd > 0 && aeCreateFileEvent(server.el,server.sofd,AE_READABLE,
        acceptUnixHandler,NULL) == AE_ERR) serverPanic("Unrecoverable error creating server.sofd file event.");


    /* Register a readable event for the pipe used to awake the event loop
     * when a blocked client in a module needs attention. 
     *
     * 当模块中的阻塞客户端需要注意时，为用于唤醒事件循环的管道注册可读事件。*/
    if (aeCreateFileEvent(server.el, server.module_blocked_pipe[0], AE_READABLE,
        moduleBlockedClientPipeReadable,NULL) == AE_ERR) {
            serverPanic(
                "Error registering the readable event for the module "
                "blocked clients subsystem.");
    }

    /* Register before and after sleep handlers (note this needs to be done
     * before loading persistence since it is used by processEventsWhileBlocked. 
     *
     * 注册睡眠前和睡眠后处理程序（注意，这需要在加载持久性之前完成，因为它由processEventsWhileBlocked使用。*/
    // 设置事件执行前要运行的函数
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeSetAfterSleepProc(server.el,afterSleep);

    /* Open the AOF file if needed. 
     *
     * 如果需要，请打开AOF文件。*/
    // 如果 AOF 已打开，那么打开或创建 AOF 文件
    if (server.aof_state == AOF_ON) {
        server.aof_fd = open(server.aof_filename,
                               O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.aof_fd == -1) {
            serverLog(LL_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }

    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the Redis instance for out of memory. 
     *
     * 32位实例的地址空间限制为4GB，因此，如果用户提供的配置中没有明确的限制，我们
     * 将使用“noevision”策略的maxmemory将限制设置为3 GB。这避免了Redis实例因内存不足而发生无用的崩溃。*/
    // 设置内存限制
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        serverLog(LL_WARNING,"Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit with 'noeviction' policy now.");
        server.maxmemory = 3072LL*(1024*1024); /* 3 GB 
                                                *
                                                * 3 GB*/
        server.maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }

    // 如果集群模式已打开，那么初始化集群
    if (server.cluster_enabled) clusterInit();
    replicationScriptCacheInit();
    // 初始化脚本环境
    scriptingInit(1);
    // 初始化慢查询
    slowlogInit();
    latencyMonitorInit();
}

/* Some steps in server initialization need to be done last (after modules
 * are loaded).
 * Specifically, creation of threads due to a race bug in ld.so, in which
 * Thread Local Storage initialization collides with dlopen call.
 * see: https://sourceware.org/bugzilla/show_bug.cgi?id=19329 
 *
 * 服务器初始化中的一些步骤需要最后完成（在加载模块之后）。具体来说，由于ld.so
 * 中的争用错误而创建线程，在该错误中，线程本地存储初始化与dlopen调用冲突。请
 * 参阅：https://sourceware.org/bugzilla/showbug.cgi?id=19329*/
void InitServerLast() {
    bioInit();
    initThreadedIO();
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    server.initial_memory_usage = zmalloc_used_memory();
}

/* Parse the flags string description 'strflags' and set them to the
 * command 'c'. If the flags are all valid C_OK is returned, otherwise
 * C_ERR is returned (yet the recognized flags are set in the command). 
 *
 * 分析标志字符串描述“strflags”，并将其设置为命令“c”。如果标志都是有效
 * 的，则返回C_OK，否则返回C_ERR（但已识别的标志在命令中设置）。*/
int populateCommandTableParseFlags(struct redisCommand *c, char *strflags) {
    int argc;
    sds *argv;

    /* Split the line into arguments for processing. 
     *
     * 将行拆分为要处理的参数。*/
    argv = sdssplitargs(strflags,&argc);
    if (argv == NULL) return C_ERR;

    for (int j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag,"write")) {
            c->flags |= CMD_WRITE|CMD_CATEGORY_WRITE;
        } else if (!strcasecmp(flag,"read-only")) {
            c->flags |= CMD_READONLY|CMD_CATEGORY_READ;
        } else if (!strcasecmp(flag,"use-memory")) {
            c->flags |= CMD_DENYOOM;
        } else if (!strcasecmp(flag,"admin")) {
            c->flags |= CMD_ADMIN|CMD_CATEGORY_ADMIN|CMD_CATEGORY_DANGEROUS;
        } else if (!strcasecmp(flag,"pub-sub")) {
            c->flags |= CMD_PUBSUB|CMD_CATEGORY_PUBSUB;
        } else if (!strcasecmp(flag,"no-script")) {
            c->flags |= CMD_NOSCRIPT;
        } else if (!strcasecmp(flag,"random")) {
            c->flags |= CMD_RANDOM;
        } else if (!strcasecmp(flag,"to-sort")) {
            c->flags |= CMD_SORT_FOR_SCRIPT;
        } else if (!strcasecmp(flag,"ok-loading")) {
            c->flags |= CMD_LOADING;
        } else if (!strcasecmp(flag,"ok-stale")) {
            c->flags |= CMD_STALE;
        } else if (!strcasecmp(flag,"no-monitor")) {
            c->flags |= CMD_SKIP_MONITOR;
        } else if (!strcasecmp(flag,"no-slowlog")) {
            c->flags |= CMD_SKIP_SLOWLOG;
        } else if (!strcasecmp(flag,"cluster-asking")) {
            c->flags |= CMD_ASKING;
        } else if (!strcasecmp(flag,"fast")) {
            c->flags |= CMD_FAST | CMD_CATEGORY_FAST;
        } else if (!strcasecmp(flag,"no-auth")) {
            c->flags |= CMD_NO_AUTH;
        } else {
            /* Parse ACL categories here if the flag name starts with @. 
             *
             * 如果标志名称以@开头，请在此处分析ACL类别。*/
            uint64_t catflag;
            if (flag[0] == '@' &&
                (catflag = ACLGetCommandCategoryFlagByName(flag+1)) != 0)
            {
                c->flags |= catflag;
            } else {
                sdsfreesplitres(argv,argc);
                return C_ERR;
            }
        }
    }
    /* If it's not @fast is @slow in this binary world. 
     *
     * 在这个二进制世界，如果不是“快”就是“慢”。*/
    if (!(c->flags & CMD_CATEGORY_FAST)) c->flags |= CMD_CATEGORY_SLOW;

    sdsfreesplitres(argv,argc);
    return C_OK;
}

/* Populates the Redis Command Table starting from the hard coded list
 * we have on top of server.c file. 
 *
 * 从server.c文件顶部的硬编码列表开始填充Redis命令表。*/
// 根据命令中的文字 FLAG ，为命令设置真正的 FLAG 标签
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;
        int retval1, retval2;

        /* Translate the command string flags description into an actual
         * set of flags. 
         *
         * 将命令字符串标志描述转换为一组实际的标志。*/
        if (populateCommandTableParseFlags(c,c->sflags) == C_ERR)
            serverPanic("Unsupported command flag");

        c->id = ACLGetCommandID(c->name);  /* Assign the ID used for ACL.
                                                     *
                                                     * 分配用于ACL的ID。*/
        retval1 = dictAdd(server.commands, sdsnew(c->name), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in redis.conf. 
         *
         * 填充一个不受redis.conf中重命名命令语句影响的附加字典。*/
        retval2 = dictAdd(server.orig_commands, sdsnew(c->name), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

/*
 * 重置命令状态
 */
void resetCommandTableStats(void) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(server.commands);
    while((de = dictNext(di)) != NULL) {
        c = (struct redisCommand *) dictGetVal(de);
        c->microseconds = 0;
        c->calls = 0;
    }
    dictReleaseIterator(di);

}

/* ========================== Redis OP Array API ============================ */

/*
 * 初始化命令数组
 */
void redisOpArrayInit(redisOpArray *oa) {
    oa->ops = NULL;
    oa->numops = 0;
}

/*
 * 设置命令数组
 */
int redisOpArrayAppend(redisOpArray *oa, struct redisCommand *cmd, int dbid,
                       robj **argv, int argc, int target)
{
    redisOp *op;

    oa->ops = zrealloc(oa->ops,sizeof(redisOp)*(oa->numops+1));
    op = oa->ops+oa->numops;
    op->cmd = cmd;
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

/*
 * 释放命令数组
 */
void redisOpArrayFree(redisOpArray *oa) {
    while(oa->numops) {
        int j;
        redisOp *op;

        oa->numops--;
        op = oa->ops+oa->numops;
        for (j = 0; j < op->argc; j++)
            decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    zfree(oa->ops);
}

/* ====================== Commands lookup and execution ===================== */
/*
 * 根据给定 sds ，查找命令
 */
struct redisCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}

/*
 * 根据给定 C 字符串 ，查找命令
 */
struct redisCommand *lookupCommandByCString(const char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = dictFetchValue(server.commands, name);
    sdsfree(name);
    return cmd;
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * redis.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. 
 *
 * 在当前表中查找命令，如果未找到，还请检入包含不受redis.conf rename命令语句影响的原始命令名的原始表。
 * 重写参数向量的函数（如rewriteClie ntCommandVector（））使用此函数，以便正确设置客户端->cmd指针，
 * 即使命令已重命名。*/
struct redisCommand *lookupCommandOrOriginal(sds name) {
    struct redisCommand *cmd = dictFetchValue(server.commands, name);

    if (!cmd) cmd = dictFetchValue(server.orig_commands,name);
    return cmd;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and Slaves.
 *
 * 传播给定命令到 AOF 或附属节点
 *
 * flags are an xor between:
 * + PROPAGATE_NONE (no propagation of command at all)
 * + PROPAGATE_AOF (propagate into the AOF file if is enabled)
 * + PROPAGATE_REPL (propagate into the replication link)
 *
 * This should not be used inside commands implementation since it will not
 * wrap the resulting commands in MULTI/EXEC. Use instead alsoPropagate(),
 * preventCommandPropagation(), forceCommandPropagation().
 *
 * However for functions that need to (also) propagate out of the context of a
 * command execution, for example when serving a blocked client, you
 * want to use propagate().
 */
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
               int flags)
{
    if (server.aof_state != AOF_OFF && flags & PROPAGATE_AOF)
        feedAppendOnlyFile(cmd,dbid,argv,argc);
    if (flags & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves,dbid,argv,argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * 'cmd' must be a pointer to the Redis command to replicate, dbid is the
 * database ID the command should be propagated into.
 * Arguments of the command to propagate are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function automatically increments ref count of
 * passed objects, so the caller does not need to. 
 *
 * 在命令内部使用，以便在当前命令传播到AOF/Replication之后安排其他命令的传播。
 *
 * 'cmd”必须是指向要复制的Redis命令的指针，dbid是该命令应传
 * 播到的数据库ID。要传播的命令的参数作为len“argc”的redis对象指针数组传递，使用“argv”向量。
 *
 * 该函数不引用传递的“argv”向量，因此由调用方释
 * 放传递的argv（但通常是堆栈分配的）。函数会自动增加传递对象的ref计数，因此调用方不需要这样做。*/
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
                   int target)
{
    robj **argvcopy;
    int j;

    if (server.loading) return; /* No propagation during loading. 
                                 *
                                 * 加载过程中没有传播。*/

    argvcopy = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&server.also_propagate,cmd,dbid,argvcopy,argc,target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * Redis command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. 
 *
 * 可以在Redis命令实现中调用函数forceCommandPropagation），以强制将特定命令执行传播到AOF/Replication中。*/
void forceCommandPropagation(client *c, int flags) {
    if (flags & PROPAGATE_REPL) c->flags |= CLIENT_FORCE_REPL;
    if (flags & PROPAGATE_AOF) c->flags |= CLIENT_FORCE_AOF;
}

/* Avoid that the executed command is propagated at all. This way we
 * are free to just propagate what we want using the alsoPropagate()
 * API. 
 *
 * 避免传播执行的命令。通过这种方式，我们可以自由地使用alsoPropagate（）API来传播我们想要的内容。*/
void preventCommandPropagation(client *c) {
    c->flags |= CLIENT_PREVENT_PROP;
}

/* AOF specific version of preventCommandPropagation(). 
 *
 * 特定于AOF版本的preventCommandPropagation（）。*/
void preventCommandAOF(client *c) {
    c->flags |= CLIENT_PREVENT_AOF_PROP;
}

/* Replication specific version of preventCommandPropagation(). 
 *
 * preventCommandPropagation（）的复制特定版本。*/
void preventCommandReplication(client *c) {
    c->flags |= CLIENT_PREVENT_REPL_PROP;
}

/* Call() is the core of Redis execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_SLOWLOG     Check command speed and log in the slow log if needed.
 * CMD_CALL_STATS       Populate command stats.
 * CMD_CALL_PROPAGATE_AOF   Append command to AOF if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE_REPL  Send command to slaves if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE   Alias for PROPAGATE_AOF|PROPAGATE_REPL.
 * CMD_CALL_FULL        Alias for SLOWLOG|STATS|PROPAGATE.
 *
 * The exact propagation behavior depends on the client flags.
 * Specifically:
 *
 * 1. If the client flags CLIENT_FORCE_AOF or CLIENT_FORCE_REPL are set
 *    and assuming the corresponding CMD_CALL_PROPAGATE_AOF/REPL is set
 *    in the call flags, then the command is propagated even if the
 *    dataset was not affected by the command.
 * 2. If the client flags CLIENT_PREVENT_REPL_PROP or CLIENT_PREVENT_AOF_PROP
 *    are set, the propagation into AOF or to slaves is not performed even
 *    if the command modified the dataset.
 *
 * Note that regardless of the client flags, if CMD_CALL_PROPAGATE_AOF
 * or CMD_CALL_PROPAGATE_REPL are not set, then respectively AOF or
 * slaves propagation will never occur.
 *
 * Client flags are modified by the implementation of a given command
 * using the following API:
 *
 * forceCommandPropagation(client *c, int flags);
 * preventCommandPropagation(client *c);
 * preventCommandAOF(client *c);
 * preventCommandReplication(client *c);
 *
 */
// 执行客户端指定的命令
void call(client *c, int flags) {
    long long dirty;
    ustime_t start, duration;
    int client_old_flags = c->flags;
    struct redisCommand *real_cmd = c->cmd;

    /* Send the command to clients in MONITOR mode if applicable.
     * Administrative commands are considered too dangerous to be shown. 
     *
     * 如果适用，在MONITOR模式下向客户端发送命令。行政命令被认为过于危险，无法显示。*/
    if (listLength(server.monitors) &&
        !server.loading &&
        !(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN)))
    {
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
    }

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. 
     *
     * 初始化：根据需要清除命令必须设置的标志，并初始化阵列以进行其他命令传播。*/
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    redisOpArray prev_also_propagate = server.also_propagate;
    redisOpArrayInit(&server.also_propagate);

    /* Call the command. 
     *
     * 调用命令。*/
    dirty = server.dirty;

    /* Update cache time, in case we have nested calls we want to
     * update only on the first call
     *
     * 更新缓存时间，如果我们有嵌套调用，我们只想在第一次调用时更新*/
    if (server.fixed_time_expire++ == 0) {
        updateCachedTime(0);
    }

    start = server.ustime;
    // 执行命令
    c->cmd->proc(c);
    // 计算执行命令耗费的时间
    duration = ustime()-start;
    // 计算命令造成多少个 key 变成 dirty
    dirty = server.dirty-dirty;
    if (dirty < 0) dirty = 0;

    /* After executing command, we will close the client after writing entire
     * reply if it is set 'CLIENT_CLOSE_AFTER_COMMAND' flag. 
     *
     * 执行命令后，如果设置了“client_close_After_command”标
     * 志，我们将在写入整个回复后关闭客户端。*/
    if (c->flags & CLIENT_CLOSE_AFTER_COMMAND) {
        c->flags &= ~CLIENT_CLOSE_AFTER_COMMAND;
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    }

    /* When EVAL is called loading the AOF we don't want commands called
     * from Lua to go into the slowlog or to populate statistics. 
     *
     * 当调用EVAL加载AOF时，我们不希望从Lua调用的命令进入慢日志或填充统计信息。*/
    // 命令由 AOF 文件在 lua 脚本中执行时
    // 不开启 slowlog 和 统计功能
    if (server.loading && c->flags & CLIENT_LUA)
        flags &= ~(CMD_CALL_SLOWLOG | CMD_CALL_STATS);

    /* If the caller is Lua, we want to force the EVAL caller to propagate
     * the script if the command flag or client flag are forcing the
     * propagation. 
     *
     * 如果调用方是Lua，那么如果命令标志或客户端标志正在强制传播，我们希望强制EVA
     * L调用方传播脚本。*/
    if (c->flags & CLIENT_LUA && server.lua_caller) {
        if (c->flags & CLIENT_FORCE_REPL)
            server.lua_caller->flags |= CLIENT_FORCE_REPL;
        if (c->flags & CLIENT_FORCE_AOF)
            server.lua_caller->flags |= CLIENT_FORCE_AOF;
    }

    /* Log the command into the Slow log if needed, and populate the
     * per-command statistics that we show in INFO commandstats. 
     *
     * 如果需要，将命令记录到Slow日志中，并填充我们在INFO commandsta
     * ts中显示的每个命令的统计信息。*/
    // 根据命令执行耗费的时间，看是否需要将命令添加到 slowlog
    if (flags & CMD_CALL_SLOWLOG && !(c->cmd->flags & CMD_SKIP_SLOWLOG)) {
        char *latency_event = (c->cmd->flags & CMD_FAST) ?
                              "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event,duration/1000);
        slowlogPushEntryIfNeeded(c,c->argv,c->argc,duration);
    }

    // 添加命令到统计数据
    if (flags & CMD_CALL_STATS) {
        /* use the real command that was executed (cmd and lastamc) may be
         * different, in case of MULTI-EXEC or re-written commands such as
         * EXPIRE, GEOADD, etc. 
         *
         * 在MULTI-EXEC或EXPIRE、GEOADD等重写命令的情况下，使用执行的
         * 实际命令（cmd和lastamc）可能不同。*/
        real_cmd->microseconds += duration;
        real_cmd->calls++;
    }

    /* Propagate the command into the AOF and replication link 
     *
     * 将命令传播到AOF和复制链接*/
    if (flags & CMD_CALL_PROPAGATE &&
        (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP)
    {
        int propagate_flags = PROPAGATE_NONE;

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. 
         *
         * 检查命令操作的数据集是否发生变化。如果是，则为复制/AOF传播设置。*/
        if (dirty) propagate_flags |= (PROPAGATE_AOF|PROPAGATE_REPL);

        /* If the client forced AOF / replication of the command, set
         * the flags regardless of the command effects on the data set. 
         *
         * 如果客户端强制AOF/复制命令，请设置标志，而不管命令对数据集的影响如何。*/
        if (c->flags & CLIENT_FORCE_REPL) propagate_flags |= PROPAGATE_REPL;
        if (c->flags & CLIENT_FORCE_AOF) propagate_flags |= PROPAGATE_AOF;

        /* However prevent AOF / replication propagation if the command
         * implementation called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. 
         *
         * 但是，如果命令实现调用了preventCommandPropagation（）或
         * 类似命令，或者如果我们没有调用（）标志，则阻止AOF/复制传播。*/
        if (c->flags & CLIENT_PREVENT_REPL_PROP ||
            !(flags & CMD_CALL_PROPAGATE_REPL))
                propagate_flags &= ~PROPAGATE_REPL;
        if (c->flags & CLIENT_PREVENT_AOF_PROP ||
            !(flags & CMD_CALL_PROPAGATE_AOF))
                propagate_flags &= ~PROPAGATE_AOF;

        /* Call propagate() only if at least one of AOF / replication
         * propagation is needed. Note that modules commands handle replication
         * in an explicit way, so we never replicate them automatically. 
         *
         * 仅当至少需要AOF/复制传播中的一个时，才调用propagation（）。请注意
         * ，模块命令以明确的方式处理复制，因此我们从不自动复制它们。*/
        if (propagate_flags != PROPAGATE_NONE && !(c->cmd->flags & CMD_MODULE))
            propagate(c->cmd,c->db->id,c->argv,c->argc,propagate_flags);
    }

    /* Restore the old replication flags, since call() can be executed
     * recursively. 
     *
     * 恢复旧的复制标志，因为call（）可以递归执行。*/
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    c->flags |= client_old_flags &
        (CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);

    /* Handle the alsoPropagate() API to handle commands that want to propagate
     * multiple separated commands. Note that alsoPropagate() is not affected
     * by CLIENT_PREVENT_PROP flag. 
     *
     * 处理alsoPropagate（）API以处理要传播多个单独命令的命令。请注意，
     * alsoPropagate（）不受CLIENT_PREVENT_PROP标志的影响。*/
    if (server.also_propagate.numops) {
        int j;
        redisOp *rop;

        if (flags & CMD_CALL_PROPAGATE) {
            int multi_emitted = 0;
            /* Wrap the commands in server.also_propagate array,
             * but don't wrap it if we are already in MULTI context,
             * in case the nested MULTI/EXEC.
             *
             * And if the array contains only one command, no need to
             * wrap it, since the single command is atomic. 
             *
             * 将命令包装在server.also_pagate数组中，但如果我们已经在MULTI上下文中，则不要包装它，以防嵌套的MULTI/EXEC。
             * 如果数组只包含一个命令，则无需包装它，因为单个命令是原子命令。*/
            if (server.also_propagate.numops > 1 &&
                !(c->cmd->flags & CMD_MODULE) &&
                !(c->flags & CLIENT_MULTI) &&
                !(flags & CMD_CALL_NOWRAP))
            {
                execCommandPropagateMulti(c);
                multi_emitted = 1;
            }

            for (j = 0; j < server.also_propagate.numops; j++) {
                rop = &server.also_propagate.ops[j];
                int target = rop->target;
                /* Whatever the command wish is, we honor the call() flags. 
                 *
                 * 无论命令的愿望是什么，我们都尊重call（）标志。*/
                if (!(flags&CMD_CALL_PROPAGATE_AOF)) target &= ~PROPAGATE_AOF;
                if (!(flags&CMD_CALL_PROPAGATE_REPL)) target &= ~PROPAGATE_REPL;
                if (target)
                    propagate(rop->cmd,rop->dbid,rop->argv,rop->argc,target);
            }

            if (multi_emitted) {
                execCommandPropagateExec(c);
            }
        }
        redisOpArrayFree(&server.also_propagate);
    }
    server.also_propagate = prev_also_propagate;

    /* If the client has keys tracking enabled for client side caching,
     * make sure to remember the keys it fetched via this command. 
     *
     * 如果客户端为客户端缓存启用了密钥跟踪，请确保记住通过该命令获取的密钥。*/
    if (c->cmd->flags & CMD_READONLY) {
        client *caller = (c->flags & CLIENT_LUA && server.lua_caller) ?
                            server.lua_caller : c;
        if (caller->flags & CLIENT_TRACKING &&
            !(caller->flags & CLIENT_TRACKING_BCAST))
        {
            trackingRememberKeys(caller);
        }
    }

    server.fixed_time_expire--;
    server.stat_numcommands++;

    /* Record peak memory after each command and before the eviction that runs
     * before the next command. 
     *
     * 记录每个命令之后以及下一个命令之前运行的逐出之前的峰值内存。*/
    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used;
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * varios pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * Note: 'reply' is expected to end with \r\n 
 *
 * 当准备执行的命令由于各种执行前检查而需要被拒绝时使用。它将适当的错误返回给客户端
 * 。如果存在事务is，则将其标记为脏，如果命令为EXEC，则中止该事务。注意：“reply”应以\r结尾*/
void rejectCommand(client *c, robj *reply) {
    flagTransaction(c);
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, reply->ptr);
    } else {
        /* using addReplyError* rather than addReply so that the error can be logged. 
         *
         * 使用addReplyError而不是addReply，以便可以记录错误。*/
        addReplyErrorObject(c, reply);
    }
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    flagTransaction(c);
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). 
     *
     * 请确保字符串中没有换行符，否则会发出无效的协议（参数来自用户，可能包含任何字符）
     * 。*/
    sdsmapchars(s, "\r\n", "  ",  2);
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
    } else {
        addReplyErrorSds(c, s);
    }
    sdsfree(s);
}

/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If C_OK is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if C_ERR is returned the client was destroyed (i.e. after QUIT).
 *
 * 如果调用此函数，我们已经读取了整个命令，参数位于客户端 argv/argc 字段中。
 * processCommand（） 执行命令或准备服务器以从客户端批量读取。
 *
 * 如果返回C_OK则客户端仍处于活动状态且有效，调用方可以执行其他操作。
 * 否则，如果返回C_ERR则客户端将被销毁（即在 QUIT 之后）。
 *
 * */
// 执行客户端 C 的命令
int processCommand(client *c) {
    moduleCallCommandFilters(c);

    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc.
     *
     * QUIT 命令是单独处理的。普通命令进程将检查复制，当启用FORCE_REPLICATION并在常规命令进程中实现时，QUIT 将导致问题。
     * */
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        addReply(c,shared.ok);
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        return C_ERR;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth.
     * 现在查找命令并尽快检查有关琐碎错误条件的信息，例如错误的arity，错误的命令名称等。
     * */
    // 获取要执行的命令，
    // 并对命令、命令参数和命令参数的数量进行检查
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        // 命令没找，出错
        sds args = sdsempty();
        int i;
        for (i=1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "`%.*s`, ", 128-(int)sdslen(args), (char*)c->argv[i]->ptr);
        rejectCommandFormat(c,"unknown command `%s`, with args beginning with: %s",
            (char*)c->argv[0]->ptr, args);
        sdsfree(args);
        return C_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        // 命令参数个数出错
        rejectCommandFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return C_OK;
    }

    int is_write_command = (c->cmd->flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    int is_denyoom_command = (c->cmd->flags & CMD_DENYOOM) ||
                             (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    int is_denystale_command = !(c->cmd->flags & CMD_STALE) ||
                               (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    int is_denyloading_command = !(c->cmd->flags & CMD_LOADING) ||
                                 (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));

    if (authRequired(c)) {
        /* AUTH and HELLO and no auth commands are valid even in
         * non-authenticated state. 
         *
         * AUTH和HELLO以及无身份验证命令即使在未通过身份验证的状态下也是有效的。*/
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c,shared.noautherr);
            return C_OK;
        }
    }

    /* Check if the user can run this command according to the current
     * ACLs. 
     *
     * 检查用户是否可以根据当前ACL运行此命令。*/
    int acl_keypos;
    int acl_retval = ACLCheckCommandPerm(c,&acl_keypos);
    if (acl_retval != ACL_OK) {
        addACLLogEntry(c,acl_retval,acl_keypos,NULL);
        if (acl_retval == ACL_DENIED_CMD)
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to run "
                "the '%s' command or its subcommand", c->cmd->name);
        else
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to access "
                "one of the keys used as arguments");
        return C_OK;
    }

    /* If cluster is enabled perform the cluster redirection here.
     * However we don't perform the redirection if:
     * 1) The sender of this command is our master.
     * 2) The command has no key arguments. 
     *
     * 如果启用了群集，请在此处执行群集重定向。
     * 但是，如果出现以下情况，我们不会执行重定向：
     * 1）此命令的发送方是我们的主节点。
     * 2）该命令没有键参数。*/
    if (server.cluster_enabled &&
        !(c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_LUA &&
          server.lua_caller->flags & CLIENT_MASTER) &&
        !(c->cmd->getkeys_proc == NULL && c->cmd->firstkey == 0 &&
          c->cmd->proc != execCommand))
    {
        int hashslot;
        int error_code;
        clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,
                                        &hashslot,&error_code);
        if (n == NULL || n != server.cluster->myself) {
            if (c->cmd->proc == execCommand) {
                discardTransaction(c);
            } else {
                flagTransaction(c);
            }
            clusterRedirectClient(c,n,hashslot,error_code);
            return C_OK;
        }
    }

    /* Handle the maxmemory directive.
     * 处理最大内存指令。
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction.
     *
     * 请注意，如果我们在这里重新进入事件循环，我们不想回收内存，
     * 因为有一个繁忙的 Lua 脚本在超时条件下运行，
     * 以避免由于逐出而导致脚本的传播与 DEL 的传播混合。
     * */

    // 如果命令可能占用大量内存，而且释放内存失败的话，那么抛出错误命令
    if (server.maxmemory && !server.lua_timedout) {

        // 如果内存不足，尝试释放无用内存
        int out_of_memory = freeMemoryIfNeededAndSafe() == C_ERR;
        /* freeMemoryIfNeeded may flush slave output buffers. This may result
         * into a slave, that may be the active client, to be freed.
         *
         * freeMemoryIfNeed可能会刷新从输出缓冲区。这可能会导致从属服务器（可能是活动客户端）被释放
         *
         * */
        if (server.current_client == NULL) return C_ERR;

        int reject_cmd_on_oom = is_denyoom_command;
        /* If client is in MULTI/EXEC context, queuing may consume an unlimited
         * amount of memory, so we want to stop that.
         * However, we never want to reject DISCARD, or even EXEC (unless it
         * contains denied commands, in which case is_denyoom_command is already
         * set.
         *
         * 如果客户端位于 MULTI/EXEC 上下文中，则排队可能会消耗无限量的内存，因此我们希望停止排队。
         * 但是，我们永远不想拒绝 DISCARD，甚至拒绝 EXEC（除非它包含拒绝的命令，
         * 在这种情况下，is_denyoom_command已经设置好了。
         *
         * */
        if (c->flags & CLIENT_MULTI &&
            c->cmd->proc != execCommand &&
            c->cmd->proc != discardCommand) {
            reject_cmd_on_oom = 1;
        }

        if (out_of_memory && reject_cmd_on_oom) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at script start, otherwise if we check OOM
         * until first write within script, memory used by lua stack and
         * arguments might interfere.
         *
         * 在脚本启动时保存out_of_memory结果，否则如果我们检查 OOM 直到第一次在脚本中写入，lua 堆栈和参数使用的内存可能会干扰。
         * */
        if (c->cmd->proc == evalCommand || c->cmd->proc == evalShaCommand) {
            server.lua_oom = out_of_memory;
        }
    }

    /* Make sure to use a reasonable amount of memory for client side
     * caching metadata.
     *
     * 确保为客户端缓存元数据使用合理的内存量。
     * */
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Don't accept write commands if there are problems persisting on disk
     * and if this is a master instance.
     *
     * 如果磁盘上仍然存在问题，并且这是主实例，则不要接受写入命令。
     * */
    int deny_write_type = writeCommandsDeniedByDiskError();
    if (deny_write_type != DISK_ERROR_TYPE_NONE &&
        server.masterhost == NULL &&
        (is_write_command ||c->cmd->proc == pingCommand))
    {
        if (deny_write_type == DISK_ERROR_TYPE_RDB)
            rejectCommand(c, shared.bgsaveerr);
        else
            rejectCommandFormat(c,
                "-MISCONF Errors writing to the AOF file: %s",
                strerror(server.aof_last_write_errno));
        return C_OK;
    }

    /* Don't accept write commands if there are not enough good slaves and
     * user configured the min-slaves-to-write option.
     *
     * 如果没有足够的好从站并且用户配置了最小从站写入选项，则不要接受写入命令。
     * */
    if (server.masterhost == NULL &&
        server.repl_min_slaves_to_write &&
        server.repl_min_slaves_max_lag &&
        is_write_command &&
        server.repl_good_slaves_count < server.repl_min_slaves_to_write)
    {
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    /* Don't accept write commands if this is a read only slave. But
     * accept write commands if this is our master.
     * 如果这是只读从属服务器，则不要接受写入命令。但是，如果这是我们的主节点，请接受写入命令。
     * */
    if (server.masterhost && server.repl_slave_ro &&
        !(c->flags & CLIENT_MASTER) &&
        is_write_command)
    {
        rejectCommand(c, shared.roslaveerr);
        return C_OK;
    }

    /* Only allow a subset of commands in the context of Pub/Sub if the
     * connection is in RESP2 mode. With RESP3 there are no limits.
     * 仅当连接处于 RESP2 模式时，才允许在发布/订阅上下文中执行命令子集。RESP3没有限制。
     * */
    if ((c->flags & CLIENT_PUBSUB && c->resp == 2) &&
        c->cmd->proc != pingCommand &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand) {
        rejectCommandFormat(c,
            "Can't execute '%s': only (P)SUBSCRIBE / "
            "(P)UNSUBSCRIBE / PING / QUIT are allowed in this context",
            c->cmd->name);
        return C_OK;
    }

    /* Only allow commands with flag "t", such as INFO, SLAVEOF and so on,
     * when slave-serve-stale-data is no and we are a slave with a broken
     * link with master.
     *
     * 只允许带有标志“t”的命令，例如INFO，SLAVEOF等，当slave-serv-stale-data为no并且我们是与master链接断开的从属时。
     * */
    // 在同步中，只有主节点可以执行写操作，附属节点不可以
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED &&
        server.repl_serve_stale_data == 0 &&
        is_denystale_command)
    {
        rejectCommand(c, shared.masterdownerr);
        return C_OK;
    }

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag.
     * 正在加载数据库？如果命令没有 CMD_LOADING 标志，则返回错误
     * */
    if (server.loading && is_denyloading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* Lua script too slow? Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022.
     *
     * Lua 脚本太慢？仅允许有限数量的命令。
     * 请注意，我们需要允许事务命令，否则在没有错误检查的情况下发送流水线事务的客户端可能会拒绝 MULTI 加上一些初始命令，
     * 然后超时条件解析，事务的下半部分被执行，请参阅 Github PR #7022。
     * */
    if (server.lua_timedout &&
          c->cmd->proc != authCommand &&
          c->cmd->proc != helloCommand &&
          c->cmd->proc != replconfCommand &&
          c->cmd->proc != multiCommand &&
          c->cmd->proc != discardCommand &&
          c->cmd->proc != watchCommand &&
          c->cmd->proc != unwatchCommand &&
        !(c->cmd->proc == shutdownCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'n') &&
        !(c->cmd->proc == scriptCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'k'))
    {
        rejectCommand(c, shared.slowscripterr);
        return C_OK;
    }

    /* Exec the command 
     *
     * 执行命令*/
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand)
    {
        queueMultiCommand(c);
        addReply(c,shared.queued);
    } else {
        call(c,CMD_CALL_FULL);
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys))
            handleClientsBlockedOnKeys();
    }
    return C_OK;
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. 
 *
 * 关闭监听socket。如果unlik_unix_socket为非零，也要取消unix域套
 * 接字的链接。*/
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (j = 0; j < server.ipfd_count; j++) close(server.ipfd[j]);
    for (j = 0; j < server.tlsfd_count; j++) close(server.tlsfd[j]);
    if (server.sofd != -1) close(server.sofd);
    if (server.cluster_enabled)
        for (j = 0; j < server.cfd_count; j++) close(server.cfd[j]);
    if (unlink_unix_socket && server.unixsocket) {
        serverLog(LL_NOTICE,"Removing the unix socket file.");
        unlink(server.unixsocket); /* don't care if this fails 
                                    *
                                    * 不管这是否失败*/
    }
}

/*
 * 在 Redis 被杀死前要执行的一系列保存和清理动作
 */
int prepareForShutdown(int flags) {
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. 
     *
     * 当服务器在内存中加载数据集时调用SHUTDOWN时，我们需要确保在关闭时不会尝试
     * 保存数据集（否则可能会用半读数据覆盖当前数据库）。此外，在Sentinel模式下，清除SAVE标志并强制NOSAVE。*/
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    int save = flags & SHUTDOWN_SAVE;
    int nosave = flags & SHUTDOWN_NOSAVE;

    serverLog(LL_WARNING,"User requested shutdown...");
    if (server.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

    /* Kill all the Lua debugger forked sessions. 
     *
     * 杀死所有Lua调试器派生的会话。*/
    ldbKillForkedSessions();

    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. 
     *
     * 如果正在进行后台保存，则杀死正在保存的子项。我们希望避免竞争条件，例如，我们的保
     * 存子项可能会覆盖SHUTDOWN所做的同步保存。*/
    // 杀死子进程的 RDB 保存进程，以免覆盖等会可能要保存的 RDB 文件
    if (server.rdb_child_pid != -1) {
        serverLog(LL_WARNING,"There is a child saving an .rdb. Killing it!");
        /* Note that, in killRDBChild, we call rdbRemoveTempFile that will
         * do close fd(in order to unlink file actully) in background thread.
         * The temp rdb file fd may won't be closed when redis exits quickly,
         * but OS will close this fd when process exits. 
         *
         * 注意，在killRDBChild中，我们调用rdbRemoveTempFile，
         * 它将在后台线程中关闭fd（以便实际取消文件链接）。当redis快速退出时，临时rdb文件fd可能不会关闭，
         * 但OS会在进程退出时关闭此fd。*/
        killRDBChild();
    }

    /* Kill module child if there is one. 
     *
     * 杀死模块子级（如果有）。*/
    if (server.module_child_pid != -1) {
        serverLog(LL_WARNING,"There is a module fork child. Killing it!");
        TerminateModuleForkChild(server.module_child_pid,0);
    }

    if (server.aof_state != AOF_OFF) {
        /* Kill the AOF saving child as the AOF we already have may be longer
         * but contains the full dataset anyway. 
         *
         * 杀死保存AOF的子项，因为我们已经拥有的AOF可能更长，但无论如何都包含完整的数据集。*/
        if (server.aof_child_pid != -1) {
            /* If we have AOF enabled but haven't written the AOF yet, don't
             * shutdown or else the dataset will be lost. 
             *
             * 如果我们启用了AOF，但还没有编写AOF，请不要关闭，否则数据集将丢失。*/
            if (server.aof_state == AOF_WAIT_REWRITE) {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                return C_ERR;
            }
            serverLog(LL_WARNING,
                "There is a child rewriting the AOF. Killing it!");
            killAppendOnlyChild();
        }
        /* Append only file: flush buffers and fsync() the AOF at exit 
         *
         * 仅附加文件：刷新缓冲区并在出口处fsync（）AOF*/
        serverLog(LL_NOTICE,"Calling fsync() on the AOF file.");
        flushAppendOnlyFile(1);
        redis_fsync(server.aof_fd);
    }

    /* Create a new RDB file before exiting. 
     *
     * 在退出之前创建一个新的RDB文件。*/
    // 决定是否需要在退出之前保存 RDB
    if ((server.saveparamslen > 0 && !nosave) || save) {
        serverLog(LL_NOTICE,"Saving the final RDB snapshot before exiting.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD)
            redisCommunicateSystemd("STATUS=Saving the final RDB snapshot\n");
        /* Snapshotting. Perform a SYNC SAVE and exit 
         *
         * 快照。执行SYNC SAVE并退出*/
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSave(server.rdb_filename,rsiptr) != C_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... 
             *
             * 哦。。保存错误！我们能做的最好的事情就是继续运行。请注意，如果有后台保存过程，在
             * 下一个cron（）中，Redis将被通知后台保存中止，处理特殊的东西，如等待同步的从节点。。。*/
            serverLog(LL_WARNING,"Error trying to save the DB, can't exit.");
            if (server.supervised_mode == SUPERVISED_SYSTEMD)
                redisCommunicateSystemd("STATUS=Error trying to save the DB, can't exit.\n");
            return C_ERR;
        }
    }

    /* Fire the shutdown modules event. 
     *
     * 激发关闭模块事件。*/
    moduleFireServerEvent(REDISMODULE_EVENT_SHUTDOWN,0,NULL);

    /* Remove the pid file if possible and needed. 
     *
     * 如果可能并且需要，请删除pid文件。*/
    // 移除 DEAMON 进程
    if (server.daemonize || server.pidfile) {
        serverLog(LL_NOTICE,"Removing the pid file.");
        unlink(server.pidfile);
    }

    /* Best effort flush of slave output buffers, so that we hopefully
     * send them pending writes. 
     *
     * 尽最大努力刷新从属输出缓冲区，这样我们就有望向它们发送挂起的写入。*/
    flushSlavesOutputBuffers();

    /* Close the listening sockets. Apparently this allows faster restarts. 
     *
     * 关闭监听插座。显然，这样可以更快地重新启动。*/
    closeListeningSockets(1);
    serverLog(LL_WARNING,"%s is now ready to exit, bye bye...",
        server.sentinel_mode ? "Sentinel" : "Redis");
    return C_OK;
}

/*================================== Commands =============================== */

/* Sometimes Redis cannot accept write commands because there is a persistence
 * error with the RDB or AOF file, and Redis is configured in order to stop
 * accepting writes in such situation. This function returns if such a
 * condition is active, and the type of the condition.
 *
 * Function return values:
 *
 * DISK_ERROR_TYPE_NONE:    No problems, we can accept writes.
 * DISK_ERROR_TYPE_AOF:     Don't accept writes: AOF errors.
 * DISK_ERROR_TYPE_RDB:     Don't accept writes: RDB errors.
 
 *
 * 有时Redis无法接受写入命令，因为RDB或AOF文件存在持久性错误，并且Red
 * is被配置为在这种情况下停止接受写入。如果此类条件处于活动状态，此函数将返回该条件的类型。
 *
 * 函数返回值：
 * DISK_ERROR_TYPE_NONE：没有问题，我们可 以接受写入。
 * DISK_ERROR_TYPE_AOF:不接受写入：AOF错误。
 * DI SK_ERROR_TYPE_RDB:不接受写入：RDB错误。*/
int writeCommandsDeniedByDiskError(void) {
    if (server.stop_writes_on_bgsave_err &&
        server.saveparamslen > 0 &&
        server.lastbgsave_status == C_ERR)
    {
        return DISK_ERROR_TYPE_RDB;
    } else if (server.aof_state != AOF_OFF &&
               server.aof_last_write_status == C_ERR)
    {
        return DISK_ERROR_TYPE_AOF;
    } else {
        return DISK_ERROR_TYPE_NONE;
    }
}

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. 
 *
 * PING命令。如果客户端处于Pub/Sub模式，则其工作方式不同。*/
void pingCommand(client *c) {
    /* The command takes zero or one arguments. 
     *
     * 该命令接受零个或一个参数。*/
    if (c->argc > 2) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return;
    }

    if (c->flags & CLIENT_PUBSUB && c->resp == 2) {
        addReply(c,shared.mbulkhdr[2]);
        addReplyBulkCBuffer(c,"pong",4);
        if (c->argc == 1)
            addReplyBulkCBuffer(c,"",0);
        else
            addReplyBulk(c,c->argv[1]);
    } else {
        if (c->argc == 1)
            addReply(c,shared.pong);
        else
            addReplyBulk(c,c->argv[1]);
    }
}

void echoCommand(client *c) {
    addReplyBulk(c,c->argv[1]);
}

void timeCommand(client *c) {
    struct timeval tv;

    /* gettimeofday() can only fail if &tv is a bad address so we
     * don't check for errors. 
     *
     * gettimeofday（）只能在&tv是错误地址时失败，因此我们不会检查错误。*/
    gettimeofday(&tv,NULL);
    addReplyArrayLen(c,2);
    addReplyBulkLongLong(c,tv.tv_sec);
    addReplyBulkLongLong(c,tv.tv_usec);
}

/* Helper function for addReplyCommand() to output flags. 
 *
 * addReplyCommand（）的Helper函数来输出标志。*/
int addReplyCommandFlag(client *c, struct redisCommand *cmd, int f, char *reply) {
    if (cmd->flags & f) {
        addReplyStatus(c, reply);
        return 1;
    }
    return 0;
}

/* Output the representation of a Redis command. Used by the COMMAND command. 
 *
 * 输出Redis命令的表示形式。由COMMAND命令使用。*/
void addReplyCommand(client *c, struct redisCommand *cmd) {
    if (!cmd) {
        addReplyNull(c);
    } else {
        /* We are adding: command name, arg count, flags, first, last, offset, categories 
         *
         * 我们正在添加：命令名、参数计数、标志、第一个、最后一个、偏移量、类别*/
        addReplyArrayLen(c, 7);
        addReplyBulkCString(c, cmd->name);
        addReplyLongLong(c, cmd->arity);

        int flagcount = 0;
        void *flaglen = addReplyDeferredLen(c);
        flagcount += addReplyCommandFlag(c,cmd,CMD_WRITE, "write");
        flagcount += addReplyCommandFlag(c,cmd,CMD_READONLY, "readonly");
        flagcount += addReplyCommandFlag(c,cmd,CMD_DENYOOM, "denyoom");
        flagcount += addReplyCommandFlag(c,cmd,CMD_ADMIN, "admin");
        flagcount += addReplyCommandFlag(c,cmd,CMD_PUBSUB, "pubsub");
        flagcount += addReplyCommandFlag(c,cmd,CMD_NOSCRIPT, "noscript");
        flagcount += addReplyCommandFlag(c,cmd,CMD_RANDOM, "random");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SORT_FOR_SCRIPT,"sort_for_script");
        flagcount += addReplyCommandFlag(c,cmd,CMD_LOADING, "loading");
        flagcount += addReplyCommandFlag(c,cmd,CMD_STALE, "stale");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SKIP_MONITOR, "skip_monitor");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SKIP_SLOWLOG, "skip_slowlog");
        flagcount += addReplyCommandFlag(c,cmd,CMD_ASKING, "asking");
        flagcount += addReplyCommandFlag(c,cmd,CMD_FAST, "fast");
        flagcount += addReplyCommandFlag(c,cmd,CMD_NO_AUTH, "no_auth");
        if ((cmd->getkeys_proc && !(cmd->flags & CMD_MODULE)) ||
            cmd->flags & CMD_MODULE_GETKEYS)
        {
            addReplyStatus(c, "movablekeys");
            flagcount += 1;
        }
        setDeferredSetLen(c, flaglen, flagcount);

        addReplyLongLong(c, cmd->firstkey);
        addReplyLongLong(c, cmd->lastkey);
        addReplyLongLong(c, cmd->keystep);

        addReplyCommandCategories(c,cmd);
    }
}

/* COMMAND <subcommand> <args> */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"(no subcommand) -- Return details about all Redis commands.",
"COUNT -- Return the total number of commands in this Redis server.",
"GETKEYS <full-command> -- Return the keys from a full Redis command.",
"INFO [command-name ...] -- Return details about multiple Redis commands.",
NULL
        };
        addReplyHelp(c, help);
    } else if (c->argc == 1) {
        addReplyArrayLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommand(c, dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else if (!strcasecmp(c->argv[1]->ptr, "info")) {
        int i;
        addReplyArrayLen(c, c->argc-2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommand(c, dictFetchValue(server.commands, c->argv[i]->ptr));
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "count") && c->argc == 2) {
        addReplyLongLong(c, dictSize(server.commands));
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeys") && c->argc >= 3) {
        struct redisCommand *cmd = lookupCommand(c->argv[2]->ptr);
        getKeysResult result = GETKEYS_RESULT_INIT;
        int j;

        if (!cmd) {
            addReplyError(c,"Invalid command specified");
            return;
        } else if (cmd->getkeys_proc == NULL && cmd->firstkey == 0) {
            addReplyError(c,"The command has no key arguments");
            return;
        } else if ((cmd->arity > 0 && cmd->arity != c->argc-2) ||
                   ((c->argc-2) < -cmd->arity))
        {
            addReplyError(c,"Invalid number of arguments specified for command");
            return;
        }

        if (!getKeysFromCommand(cmd,c->argv+2,c->argc-2,&result)) {
            addReplyError(c,"Invalid arguments specified for command");
        } else {
            addReplyArrayLen(c,result.numkeys);
            for (j = 0; j < result.numkeys; j++) addReplyBulk(c,c->argv[result.keys[j]+2]);
        }
        getKeysFreeResult(&result);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. 
 *
 * 将一定数量的字节转换为100B、2G、100M、4K等形式的人类可读字符串。*/
void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes 
         *
         * 字节*/
        sprintf(s,"%lluB",n);
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    } else if (n < (1024LL*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024);
        sprintf(s,"%.2fT",d);
    } else if (n < (1024LL*1024*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024*1024);
        sprintf(s,"%.2fP",d);
    } else {
        /* Let's hope we never need this 
         *
         * 希望我们永远不需要这个*/
        sprintf(s,"%lluB",n);
    }
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. 
 *
 * 创建INFO命令返回的字符串。这是由INFO命令本身解耦的，因为我们需要报告关于
 * 内存损坏问题的相同信息。*/
sds genRedisInfoString(const char *section) {
    sds info = sdsempty();
    time_t uptime = server.unixtime-server.stat_starttime;
    int j;
    struct rusage self_ru, c_ru;
    int allsections = 0, defsections = 0, everything = 0, modules = 0;
    int sections = 0;

    if (section == NULL) section = "default";
    allsections = strcasecmp(section,"all") == 0;
    defsections = strcasecmp(section,"default") == 0;
    everything = strcasecmp(section,"everything") == 0;
    modules = strcasecmp(section,"modules") == 0;
    if (everything) allsections = 1;

    getrusage(RUSAGE_SELF, &self_ru);
    getrusage(RUSAGE_CHILDREN, &c_ru);

    /* Server 
     *
     * 服务器*/
    if (allsections || defsections || !strcasecmp(section,"server")) {
        static int call_uname = 1;
        static struct utsname name;
        char *mode;

        if (server.cluster_enabled) mode = "cluster";
        else if (server.sentinel_mode) mode = "sentinel";
        else mode = "standalone";

        if (sections++) info = sdscat(info,"\r\n");

        if (call_uname) {
            /* Uname can be slow and is always the same output. Cache it. 
             *
             * Uname可能很慢，并且总是相同的输出。缓存它。*/
            uname(&name);
            call_uname = 0;
        }

        info = sdscatfmt(info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%i\r\n"
            "redis_build_id:%s\r\n"
            "redis_mode:%s\r\n"
            "os:%s %s %s\r\n"
            "arch_bits:%i\r\n"
            "multiplexing_api:%s\r\n"
            "atomicvar_api:%s\r\n"
            "gcc_version:%i.%i.%i\r\n"
            "process_id:%I\r\n"
            "run_id:%s\r\n"
            "tcp_port:%i\r\n"
            "uptime_in_seconds:%I\r\n"
            "uptime_in_days:%I\r\n"
            "hz:%i\r\n"
            "configured_hz:%i\r\n"
            "lru_clock:%u\r\n"
            "executable:%s\r\n"
            "config_file:%s\r\n"
            "io_threads_active:%i\r\n",
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            redisBuildIdString(),
            mode,
            name.sysname, name.release, name.machine,
            server.arch_bits,
            aeGetApiName(),
            REDIS_ATOMIC_API,
#ifdef __GNUC__
            __GNUC__,__GNUC_MINOR__,__GNUC_PATCHLEVEL__,
#else
            0,0,0,
#endif
            (int64_t) getpid(),
            server.runid,
            server.port ? server.port : server.tls_port,
            (int64_t)uptime,
            (int64_t)(uptime/(3600*24)),
            server.hz,
            server.config_hz,
            server.lruclock,
            server.executable ? server.executable : "",
            server.configfile ? server.configfile : "",
            server.io_threads_active);
    }

    /* Clients 
     *
     * 客户端*/
    if (allsections || defsections || !strcasecmp(section,"clients")) {
        size_t maxin, maxout;
        getExpansiveClientsInfo(&maxin,&maxout);
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "connected_clients:%lu\r\n"
            "client_recent_max_input_buffer:%zu\r\n"
            "client_recent_max_output_buffer:%zu\r\n"
            "blocked_clients:%d\r\n"
            "tracking_clients:%d\r\n"
            "clients_in_timeout_table:%llu\r\n",
            listLength(server.clients)-listLength(server.slaves),
            maxin, maxout,
            server.blocked_clients,
            server.tracking_clients,
            (unsigned long long) raxSize(server.clients_timeout_table));
    }

    /* Memory 
     *
     * 内存*/
    if (allsections || defsections || !strcasecmp(section,"memory")) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_scripts_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = server.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = server.lua ? (long long)lua_gc(server.lua,LUA_GCCOUNT,0)*1024 : 0;
        struct redisMemOverhead *mh = getMemoryOverheadData();

        /* Peak memory is updated from time to time by serverCron() so it
         * may happen that the instantaneous value is slightly bigger than
         * the peak value. This may confuse users, so we update the peak
         * if found smaller than the current memory usage. 
         *
         * 峰值内存由serverCron（）不时更新，因此可能会出现瞬时值略大于峰值的情况
         * 。这可能会让用户感到困惑，所以如果发现峰值小于当前内存使用量，我们会更新峰值。*/
        if (zmalloc_used > server.stat_peak_memory)
            server.stat_peak_memory = zmalloc_used;

        bytesToHuman(hmem,zmalloc_used);
        bytesToHuman(peak_hmem,server.stat_peak_memory);
        bytesToHuman(total_system_hmem,total_system_mem);
        bytesToHuman(used_memory_lua_hmem,memory_lua);
        bytesToHuman(used_memory_scripts_hmem,mh->lua_caches);
        bytesToHuman(used_memory_rss_hmem,server.cron_malloc_stats.process_rss);
        bytesToHuman(maxmemory_hmem,server.maxmemory);

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Memory\r\n"
            "used_memory:%zu\r\n"
            "used_memory_human:%s\r\n"
            "used_memory_rss:%zu\r\n"
            "used_memory_rss_human:%s\r\n"
            "used_memory_peak:%zu\r\n"
            "used_memory_peak_human:%s\r\n"
            "used_memory_peak_perc:%.2f%%\r\n"
            "used_memory_overhead:%zu\r\n"
            "used_memory_startup:%zu\r\n"
            "used_memory_dataset:%zu\r\n"
            "used_memory_dataset_perc:%.2f%%\r\n"
            "allocator_allocated:%zu\r\n"
            "allocator_active:%zu\r\n"
            "allocator_resident:%zu\r\n"
            "total_system_memory:%lu\r\n"
            "total_system_memory_human:%s\r\n"
            "used_memory_lua:%lld\r\n"
            "used_memory_lua_human:%s\r\n"
            "used_memory_scripts:%lld\r\n"
            "used_memory_scripts_human:%s\r\n"
            "number_of_cached_scripts:%lu\r\n"
            "maxmemory:%lld\r\n"
            "maxmemory_human:%s\r\n"
            "maxmemory_policy:%s\r\n"
            "allocator_frag_ratio:%.2f\r\n"
            "allocator_frag_bytes:%zu\r\n"
            "allocator_rss_ratio:%.2f\r\n"
            "allocator_rss_bytes:%zd\r\n"
            "rss_overhead_ratio:%.2f\r\n"
            "rss_overhead_bytes:%zd\r\n"
            "mem_fragmentation_ratio:%.2f\r\n"
            "mem_fragmentation_bytes:%zd\r\n"
            "mem_not_counted_for_evict:%zu\r\n"
            "mem_replication_backlog:%zu\r\n"
            "mem_clients_slaves:%zu\r\n"
            "mem_clients_normal:%zu\r\n"
            "mem_aof_buffer:%zu\r\n"
            "mem_allocator:%s\r\n"
            "active_defrag_running:%d\r\n"
            "lazyfree_pending_objects:%zu\r\n",
            zmalloc_used,
            hmem,
            server.cron_malloc_stats.process_rss,
            used_memory_rss_hmem,
            server.stat_peak_memory,
            peak_hmem,
            mh->peak_perc,
            mh->overhead_total,
            mh->startup_allocated,
            mh->dataset,
            mh->dataset_perc,
            server.cron_malloc_stats.allocator_allocated,
            server.cron_malloc_stats.allocator_active,
            server.cron_malloc_stats.allocator_resident,
            (unsigned long)total_system_mem,
            total_system_hmem,
            memory_lua,
            used_memory_lua_hmem,
            (long long) mh->lua_caches,
            used_memory_scripts_hmem,
            dictSize(server.lua_scripts),
            server.maxmemory,
            maxmemory_hmem,
            evict_policy,
            mh->allocator_frag,
            mh->allocator_frag_bytes,
            mh->allocator_rss,
            mh->allocator_rss_bytes,
            mh->rss_extra,
            mh->rss_extra_bytes,
            mh->total_frag,       /* This is the total RSS overhead, including
                                     fragmentation, but not just it. This field
                                     (and the next one) is named like that just
                                     for backward compatibility. 
                                   *
                                   * 这是RSS的总开销，包括碎片，但不仅仅是碎片。为了向后兼容，这个字段（和下一个字段）是这样命名的。*/
            mh->total_frag_bytes,
            freeMemoryGetNotCountedMemory(),
            mh->repl_backlog,
            mh->clients_slaves,
            mh->clients_normal,
            mh->aof_buffer,
            ZMALLOC_LIB,
            server.active_defrag_running,
            lazyfreeGetPendingObjectsCount()
        );
        freeMemoryOverheadData(mh);
    }

    /* Persistence 
     *
     * 持久化*/
    if (allsections || defsections || !strcasecmp(section,"persistence")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Persistence\r\n"
            "loading:%d\r\n"
            "rdb_changes_since_last_save:%lld\r\n"
            "rdb_bgsave_in_progress:%d\r\n"
            "rdb_last_save_time:%jd\r\n"
            "rdb_last_bgsave_status:%s\r\n"
            "rdb_last_bgsave_time_sec:%jd\r\n"
            "rdb_current_bgsave_time_sec:%jd\r\n"
            "rdb_last_cow_size:%zu\r\n"
            "aof_enabled:%d\r\n"
            "aof_rewrite_in_progress:%d\r\n"
            "aof_rewrite_scheduled:%d\r\n"
            "aof_last_rewrite_time_sec:%jd\r\n"
            "aof_current_rewrite_time_sec:%jd\r\n"
            "aof_last_bgrewrite_status:%s\r\n"
            "aof_last_write_status:%s\r\n"
            "aof_last_cow_size:%zu\r\n"
            "module_fork_in_progress:%d\r\n"
            "module_fork_last_cow_size:%zu\r\n",
            server.loading,
            server.dirty,
            server.rdb_child_pid != -1,
            (intmax_t)server.lastsave,
            (server.lastbgsave_status == C_OK) ? "ok" : "err",
            (intmax_t)server.rdb_save_time_last,
            (intmax_t)((server.rdb_child_pid == -1) ?
                -1 : time(NULL)-server.rdb_save_time_start),
            server.stat_rdb_cow_bytes,
            server.aof_state != AOF_OFF,
            server.aof_child_pid != -1,
            server.aof_rewrite_scheduled,
            (intmax_t)server.aof_rewrite_time_last,
            (intmax_t)((server.aof_child_pid == -1) ?
                -1 : time(NULL)-server.aof_rewrite_time_start),
            (server.aof_lastbgrewrite_status == C_OK) ? "ok" : "err",
            (server.aof_last_write_status == C_OK) ? "ok" : "err",
            server.stat_aof_cow_bytes,
            server.module_child_pid != -1,
            server.stat_module_cow_bytes);

        if (server.aof_enabled) {
            info = sdscatprintf(info,
                "aof_current_size:%lld\r\n"
                "aof_base_size:%lld\r\n"
                "aof_pending_rewrite:%d\r\n"
                "aof_buffer_length:%zu\r\n"
                "aof_rewrite_buffer_length:%lu\r\n"
                "aof_pending_bio_fsync:%llu\r\n"
                "aof_delayed_fsync:%lu\r\n",
                (long long) server.aof_current_size,
                (long long) server.aof_rewrite_base_size,
                server.aof_rewrite_scheduled,
                sdslen(server.aof_buf),
                aofRewriteBufferSize(),
                bioPendingJobsOfType(BIO_AOF_FSYNC),
                server.aof_delayed_fsync);
        }

        if (server.loading) {
            double perc;
            time_t eta, elapsed;
            off_t remaining_bytes = server.loading_total_bytes-
                                    server.loading_loaded_bytes;

            perc = ((double)server.loading_loaded_bytes /
                   (server.loading_total_bytes+1)) * 100;

            elapsed = time(NULL)-server.loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info 
                          *
                          * 如果我们没有足够的信息，一个伪造的1秒数字*/
            } else {
                eta = (elapsed*remaining_bytes)/(server.loading_loaded_bytes+1);
            }

            info = sdscatprintf(info,
                "loading_start_time:%jd\r\n"
                "loading_total_bytes:%llu\r\n"
                "loading_loaded_bytes:%llu\r\n"
                "loading_loaded_perc:%.2f\r\n"
                "loading_eta_seconds:%jd\r\n",
                (intmax_t) server.loading_start_time,
                (unsigned long long) server.loading_total_bytes,
                (unsigned long long) server.loading_loaded_bytes,
                perc,
                (intmax_t)eta
            );
        }
    }

    /* Stats */
    if (allsections || defsections || !strcasecmp(section,"stats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Stats\r\n"
            "total_connections_received:%lld\r\n"
            "total_commands_processed:%lld\r\n"
            "instantaneous_ops_per_sec:%lld\r\n"
            "total_net_input_bytes:%lld\r\n"
            "total_net_output_bytes:%lld\r\n"
            "instantaneous_input_kbps:%.2f\r\n"
            "instantaneous_output_kbps:%.2f\r\n"
            "rejected_connections:%lld\r\n"
            "sync_full:%lld\r\n"
            "sync_partial_ok:%lld\r\n"
            "sync_partial_err:%lld\r\n"
            "expired_keys:%lld\r\n"
            "expired_stale_perc:%.2f\r\n"
            "expired_time_cap_reached_count:%lld\r\n"
            "expire_cycle_cpu_milliseconds:%lld\r\n"
            "evicted_keys:%lld\r\n"
            "keyspace_hits:%lld\r\n"
            "keyspace_misses:%lld\r\n"
            "pubsub_channels:%ld\r\n"
            "pubsub_patterns:%lu\r\n"
            "latest_fork_usec:%lld\r\n"
            "migrate_cached_sockets:%ld\r\n"
            "slave_expires_tracked_keys:%zu\r\n"
            "active_defrag_hits:%lld\r\n"
            "active_defrag_misses:%lld\r\n"
            "active_defrag_key_hits:%lld\r\n"
            "active_defrag_key_misses:%lld\r\n"
            "tracking_total_keys:%lld\r\n"
            "tracking_total_items:%lld\r\n"
            "tracking_total_prefixes:%lld\r\n"
            "unexpected_error_replies:%lld\r\n"
            "total_reads_processed:%lld\r\n"
            "total_writes_processed:%lld\r\n"
            "io_threaded_reads_processed:%lld\r\n"
            "io_threaded_writes_processed:%lld\r\n",
            server.stat_numconnections,
            server.stat_numcommands,
            getInstantaneousMetric(STATS_METRIC_COMMAND),
            server.stat_net_input_bytes,
            server.stat_net_output_bytes,
            (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT)/1024,
            (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT)/1024,
            server.stat_rejected_conn,
            server.stat_sync_full,
            server.stat_sync_partial_ok,
            server.stat_sync_partial_err,
            server.stat_expiredkeys,
            server.stat_expired_stale_perc*100,
            server.stat_expired_time_cap_reached_count,
            server.stat_expire_cycle_time_used/1000,
            server.stat_evictedkeys,
            server.stat_keyspace_hits,
            server.stat_keyspace_misses,
            dictSize(server.pubsub_channels),
            listLength(server.pubsub_patterns),
            server.stat_fork_time,
            dictSize(server.migrate_cached_sockets),
            getSlaveKeyWithExpireCount(),
            server.stat_active_defrag_hits,
            server.stat_active_defrag_misses,
            server.stat_active_defrag_key_hits,
            server.stat_active_defrag_key_misses,
            (unsigned long long) trackingGetTotalKeys(),
            (unsigned long long) trackingGetTotalItems(),
            (unsigned long long) trackingGetTotalPrefixes(),
            server.stat_unexpected_error_replies,
            server.stat_total_reads_processed,
            server.stat_total_writes_processed,
            server.stat_io_reads_processed,
            server.stat_io_writes_processed);
    }

    /* Replication 
     *
     * 复制*/
    if (allsections || defsections || !strcasecmp(section,"replication")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Replication\r\n"
            "role:%s\r\n",
            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            long long slave_repl_offset = 1;

            if (server.master)
                slave_repl_offset = server.master->reploff;
            else if (server.cached_master)
                slave_repl_offset = server.cached_master->reploff;

            info = sdscatprintf(info,
                "master_host:%s\r\n"
                "master_port:%d\r\n"
                "master_link_status:%s\r\n"
                "master_last_io_seconds_ago:%d\r\n"
                "master_sync_in_progress:%d\r\n"
                "slave_repl_offset:%lld\r\n"
                ,server.masterhost,
                server.masterport,
                (server.repl_state == REPL_STATE_CONNECTED) ?
                    "up" : "down",
                server.master ?
                ((int)(server.unixtime-server.master->lastinteraction)) : -1,
                server.repl_state == REPL_STATE_TRANSFER,
                slave_repl_offset
            );

            if (server.repl_state == REPL_STATE_TRANSFER) {
                info = sdscatprintf(info,
                    "master_sync_left_bytes:%lld\r\n"
                    "master_sync_last_io_seconds_ago:%d\r\n"
                    , (long long)
                        (server.repl_transfer_size - server.repl_transfer_read),
                    (int)(server.unixtime-server.repl_transfer_lastio)
                );
            }

            if (server.repl_state != REPL_STATE_CONNECTED) {
                info = sdscatprintf(info,
                    "master_link_down_since_seconds:%jd\r\n",
                    (intmax_t)(server.unixtime-server.repl_down_since));
            }
            info = sdscatprintf(info,
                "slave_priority:%d\r\n"
                "slave_read_only:%d\r\n",
                server.slave_priority,
                server.repl_slave_ro);
        }

        info = sdscatprintf(info,
            "connected_slaves:%lu\r\n",
            listLength(server.slaves));

        /* If min-slaves-to-write is active, write the number of slaves
         * currently considered 'good'. 
         *
         * 如果要写入的最小从属服务器处于活动状态，则写入当前被认为“良好”的从属服务器数量
         * 。*/
        if (server.repl_min_slaves_to_write &&
            server.repl_min_slaves_max_lag) {
            info = sdscatprintf(info,
                "min_slaves_good_slaves:%d\r\n",
                server.repl_good_slaves_count);
        }

        if (listLength(server.slaves)) {
            int slaveid = 0;
            listNode *ln;
            listIter li;

            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = listNodeValue(ln);
                char *state = NULL;
                char ip[NET_IP_STR_LEN], *slaveip = slave->slave_ip;
                int port;
                long lag = 0;

                if (slaveip[0] == '\0') {
                    if (connPeerToString(slave->conn,ip,sizeof(ip),&port) == -1)
                        continue;
                    slaveip = ip;
                }
                switch(slave->replstate) {
                case SLAVE_STATE_WAIT_BGSAVE_START:
                case SLAVE_STATE_WAIT_BGSAVE_END:
                    state = "wait_bgsave";
                    break;
                case SLAVE_STATE_SEND_BULK:
                    state = "send_bulk";
                    break;
                case SLAVE_STATE_ONLINE:
                    state = "online";
                    break;
                }
                if (state == NULL) continue;
                if (slave->replstate == SLAVE_STATE_ONLINE)
                    lag = time(NULL) - slave->repl_ack_time;

                info = sdscatprintf(info,
                    "slave%d:ip=%s,port=%d,state=%s,"
                    "offset=%lld,lag=%ld\r\n",
                    slaveid,slaveip,slave->slave_listening_port,state,
                    slave->repl_ack_off, lag);
                slaveid++;
            }
        }
        info = sdscatprintf(info,
            "master_replid:%s\r\n"
            "master_replid2:%s\r\n"
            "master_repl_offset:%lld\r\n"
            "second_repl_offset:%lld\r\n"
            "repl_backlog_active:%d\r\n"
            "repl_backlog_size:%lld\r\n"
            "repl_backlog_first_byte_offset:%lld\r\n"
            "repl_backlog_histlen:%lld\r\n",
            server.replid,
            server.replid2,
            server.master_repl_offset,
            server.second_replid_offset,
            server.repl_backlog != NULL,
            server.repl_backlog_size,
            server.repl_backlog_off,
            server.repl_backlog_histlen);
    }

    /* CPU */
    if (allsections || defsections || !strcasecmp(section,"cpu")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# CPU\r\n"
        "used_cpu_sys:%ld.%06ld\r\n"
        "used_cpu_user:%ld.%06ld\r\n"
        "used_cpu_sys_children:%ld.%06ld\r\n"
        "used_cpu_user_children:%ld.%06ld\r\n",
        (long)self_ru.ru_stime.tv_sec, (long)self_ru.ru_stime.tv_usec,
        (long)self_ru.ru_utime.tv_sec, (long)self_ru.ru_utime.tv_usec,
        (long)c_ru.ru_stime.tv_sec, (long)c_ru.ru_stime.tv_usec,
        (long)c_ru.ru_utime.tv_sec, (long)c_ru.ru_utime.tv_usec);
    }

    /* Modules 
     *
     * 模块*/
    if (allsections || defsections || !strcasecmp(section,"modules")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,"# Modules\r\n");
        info = genModulesInfoString(info);
    }

    /* Command statistics 
     *
     * 命令统计信息*/
    if (allsections || !strcasecmp(section,"commandstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");

        struct redisCommand *c;
        dictEntry *de;
        dictIterator *di;
        di = dictGetSafeIterator(server.commands);
        while((de = dictNext(di)) != NULL) {
            c = (struct redisCommand *) dictGetVal(de);
            if (!c->calls) continue;
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f\r\n",
                c->name, c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls));
        }
        dictReleaseIterator(di);
    }

    /* Cluster 
     *
     * 集群*/
    if (allsections || defsections || !strcasecmp(section,"cluster")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# Cluster\r\n"
        "cluster_enabled:%d\r\n",
        server.cluster_enabled);
    }

    /* Key space 
     *
     * 键空间*/
    if (allsections || defsections || !strcasecmp(section,"keyspace")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys;

            keys = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (keys || vkeys) {
                info = sdscatprintf(info,
                    "db%d:keys=%lld,expires=%lld,avg_ttl=%lld\r\n",
                    j, keys, vkeys, server.db[j].avg_ttl);
            }
        }
    }

    /* Get info from modules.
     * if user asked for "everything" or "modules", or a specific section
     * that's not found yet. 
     *
     * 从模块获取信息。如果用户要求“一切”或“模块”，或尚未找到的特定部分。*/
    if (everything || modules ||
        (!allsections && !defsections && sections==0)) {
        info = modulesCollectInfo(info,
                                  everything || modules ? NULL: section,
                                  0, /* not a crash report 
                                      *
                                      * 不是事故报告*/
                                  sections);
    }
    return info;
}

void infoCommand(client *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    }
    sds info = genRedisInfoString(section);
    addReplyVerbatim(c,info,sdslen(info),"txt");
    sdsfree(info);
}

void monitorCommand(client *c) {
    /* ignore MONITOR if already slave or in monitor mode 
     *
     * 忽略MONITOR（如果已从机或处于监控模式）*/
    if (c->flags & CLIENT_SLAVE) return;

    c->flags |= (CLIENT_SLAVE|CLIENT_MONITOR);
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}

/* =================================== Main! ================================ */

int checkIgnoreWarning(const char *warning) {
    int argc, j;
    sds *argv = sdssplitargs(server.ignore_warnings, &argc);
    if (argv == NULL)
        return 0;

    for (j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag, warning))
            break;
    }
    sdsfreesplitres(argv,argc);
    return j < argc;
}

#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}

void linuxMemoryWarnings(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        serverLog(LL_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
    if (THPIsEnabled()) {
        serverLog(LL_WARNING,"WARNING you have Transparent Huge Pages (THP) support enabled in your kernel. This will create latency and memory usage issues with Redis. To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, and add it to your /etc/rc.local in order to retain the setting after a reboot. Redis must be restarted after THP is disabled (set to 'madvise' or 'never').");
    }
}

#ifdef __arm64__

/* Get size in kilobytes of the Shared_Dirty pages of the calling process for the
 * memory map corresponding to the provided address, or -1 on error. 
 *
 * 获取与所提供地址相对应的内存映射的调用进程的Shared_Dirty页的大小（以
 * KB为单位），错误时为-1。*/
static int smapsGetSharedDirty(unsigned long addr) {
    int ret, in_mapping = 0, val = -1;
    unsigned long from, to;
    char buf[64];
    FILE *f;

    f = fopen("/proc/self/smaps", "r");
    if (!f) return -1;

    while (1) {
        if (!fgets(buf, sizeof(buf), f))
            break;

        ret = sscanf(buf, "%lx-%lx", &from, &to);
        if (ret == 2)
            in_mapping = from <= addr && addr < to;

        if (in_mapping && !memcmp(buf, "Shared_Dirty:", 13)) {
            sscanf(buf, "%*s %d", &val);
            /* If parsing fails, we remain with val == -1 
             *
             * 如果解析失败，我们将使用val==-1*/
            break;
        }
    }

    fclose(f);
    return val;
}

/* Older arm64 Linux kernels have a bug that could lead to data corruption
 * during background save in certain scenarios. This function checks if the
 * kernel is affected.
 * The bug was fixed in commit ff1712f953e27f0b0718762ec17d0adb15c9fd0b
 * titled: "arm64: pgtable: Ensure dirty bit is preserved across pte_wrprotect()"
 * Return -1 on unexpected test failure, 1 if the kernel seems to be affected,
 * and 0 otherwise. 
 *
 * 较旧的arm64 Linux内核有一个错误，在某些情况下，该错误可能会导致后台保
 * 存期间的数据损坏。此函数检查内核是否受到影响。该错误已在提交ff1712f953
 * e27f0b0718762ec17d0adb15c9fd0b中修复，标题为：“a
 * rm64:pgtable:确保在pte_wrprotect（）中保留脏位”在意外
 * 测试失败时返回-1，如果内核似乎受到影响则返回1，否则返回0。*/
int linuxMadvFreeForkBugCheck(void) {
    int ret, pipefd[2] = { -1, -1 };
    pid_t pid;
    char *p = NULL, *q;
    int bug_found = 0;
    long page_size = sysconf(_SC_PAGESIZE);
    long map_size = 3 * page_size;

    /* Create a memory map that's in our full control (not one used by the allocator). 
     *
     * 创建一个完全由我们控制的内存映射（不是分配器使用的）。*/
    p = mmap(NULL, map_size, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        serverLog(LL_WARNING, "Failed to mmap(): %s", strerror(errno));
        return -1;
    }

    q = p + page_size;

    /* Split the memory map in 3 pages by setting their protection as RO|RW|RO to prevent
     * Linux from merging this memory map with adjacent VMAs. */
    ret = mprotect(q, page_size, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to mprotect(): %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Write to the page once to make it resident 
     *
     * 在页面上写一次以使其常驻*/
    *(volatile char*)q = 0;

    /* Tell the kernel that this page is free to be reclaimed. 
     *
     * 告诉内核这个页面是可以自由回收的。*/
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
    ret = madvise(q, page_size, MADV_FREE);
    if (ret < 0) {
        /* MADV_FREE is not available on older kernels that are presumably
         * not affected. 
         *
         * MADV_FREE在可能未受影响的旧内核上不可用。*/
        if (errno == EINVAL) goto exit;

        serverLog(LL_WARNING, "Failed to madvise(): %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Write to the page after being marked for freeing, this is supposed to take
     * ownership of that page again. 
     *
     * 在标记为释放后写入页面，这应该会再次获得该页面的所有权。*/
    *(volatile char*)q = 0;

    /* Create a pipe for the child to return the info to the parent. 
     *
     * 为子进程创建一个管道，以便将信息返回给父进程。*/
    ret = pipe(pipefd);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to create pipe: %s", strerror(errno));
        bug_found = -1;
        goto exit;
    }

    /* Fork the process. 
     *
     *  fork 进程。*/
    pid = fork();
    if (pid < 0) {
        serverLog(LL_WARNING, "Failed to fork: %s", strerror(errno));
        bug_found = -1;
        goto exit;
    } else if (!pid) {
        /* Child: check if the page is marked as dirty, page_size in kb.
         * A value of 0 means the kernel is affected by the bug. 
         *
         * 子进程：检查页面是否标记为脏，page_size以kb为单位。值为0表示内核
         * 受到错误的影响。*/
        ret = smapsGetSharedDirty((unsigned long) q);
        if (!ret)
            bug_found = 1;
        else if (ret == -1)     /* Failed to read 
                                 *
                                 * 读取失败*/
            bug_found = -1;

        if (write(pipefd[1], &bug_found, sizeof(bug_found)) < 0)
            serverLog(LL_WARNING, "Failed to write to parent: %s", strerror(errno));
        exit(0);
    } else {
        /* Read the result from the child. 
         *
         * 从子进程读取结果。*/
        ret = read(pipefd[0], &bug_found, sizeof(bug_found));
        if (ret < 0) {
            serverLog(LL_WARNING, "Failed to read from child: %s", strerror(errno));
            bug_found = -1;
        }

        /* Reap the child pid. 
         *
         * 收割子进程的pid。*/
        waitpid(pid, NULL, 0);
    }

exit:
    /* Cleanup 
     *
     * 清理*/
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);
    if (p != NULL) munmap(p, map_size);

    return bug_found;
}
#endif /* __arm64__ */
#endif /* __linux__ */

void createPidFile(void) {
    /* If pidfile requested, but no pidfile defined, use
     * default pidfile path 
     *
     * 如果请求了pidfile，但未定义任何pidfle，请使用默认的pidfile路
     * 径*/
    if (!server.pidfile) server.pidfile = zstrdup(CONFIG_DEFAULT_PID_FILE);

    /* Try to write the pid file in a best-effort way. 
     *
     * 尝试以尽最大努力的方式编写pid文件。*/
    FILE *fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",(int)getpid());
        fclose(fp);
    }
}

void daemonize(void) {
    int fd;

    // fork 进程
    if (fork() != 0) exit(0); /* parent exits 
                               *
                               * 父进程退出*/
    setsid(); /* create a new session 
               *
               * 创建新会话*/

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. 
     *
     * 每个输出都进入/dev/null。如果Redis已被守护进程化，但配置文件中的“
     * logfile”设置为“stdout”，则根本不会进行日志记录。*/
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

void version(void) {
    printf("Redis server v=%s sha=%s:%d malloc=%s bits=%d build=%llx\n",
        REDIS_VERSION,
        redisGitSHA1(),
        atoi(redisGitDirty()) > 0,
        ZMALLOC_LIB,
        sizeof(long) == 4 ? 32 : 64,
        (unsigned long long) redisBuildId());
    exit(0);
}

void usage(void) {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf] [options]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    fprintf(stderr,"       ./redis-server -v or --version\n");
    fprintf(stderr,"       ./redis-server -h or --help\n");
    fprintf(stderr,"       ./redis-server --test-memory <megabytes>\n\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./redis-server (run the server with default conf)\n");
    fprintf(stderr,"       ./redis-server /etc/redis/6379.conf\n");
    fprintf(stderr,"       ./redis-server --port 7777\n");
    fprintf(stderr,"       ./redis-server --port 7777 --replicaof 127.0.0.1 8888\n");
    fprintf(stderr,"       ./redis-server /etc/myredis.conf --loglevel verbose\n\n");
    fprintf(stderr,"Sentinel mode:\n");
    fprintf(stderr,"       ./redis-server /etc/sentinel.conf --sentinel\n");
    exit(1);
}

void redisAsciiArt(void) {
#include "asciilogo.h"
    char *buf = zmalloc(1024*16);
    char *mode;

    if (server.cluster_enabled) mode = "cluster";
    else if (server.sentinel_mode) mode = "sentinel";
    else mode = "standalone";

    /* Show the ASCII logo if: log file is stdout AND stdout is a
     * tty AND syslog logging is disabled. Also show logo if the user
     * forced us to do so via redis.conf. 
     *
     * 如果：log file是stdout AND stdout是tty AND sy
     * slog日志记录被禁用，则显示ASCII徽标。如果用户通过redis.conf强
     * 迫我们这样做，也要显示徽标。*/
    int show_logo = ((!server.syslog_enabled &&
                      server.logfile[0] == '\0' &&
                      isatty(fileno(stdout))) ||
                     server.always_show_logo);

    if (!show_logo) {
        serverLog(LL_NOTICE,
            "Running mode=%s, port=%d.",
            mode, server.port ? server.port : server.tls_port
        );
    } else {
        snprintf(buf,1024*16,ascii_logo,
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            mode, server.port ? server.port : server.tls_port,
            (long) getpid()
        );
        serverLogRaw(LL_NOTICE|LL_RAW,buf);
    }
    zfree(buf);
}

static void sigShutdownHandler(int sig) {
    char *msg;

    switch (sig) {
    case SIGINT:
        msg = "Received SIGINT scheduling shutdown...";
        break;
    case SIGTERM:
        msg = "Received SIGTERM scheduling shutdown...";
        break;
    default:
        msg = "Received shutdown signal, scheduling shutdown...";
    };

    /* SIGINT is often delivered via Ctrl+C in an interactive session.
     * If we receive the signal the second time, we interpret this as
     * the user really wanting to quit ASAP without waiting to persist
     * on disk. 
     *
     * SIGINT通常在交互式会话中通过Ctrl+C传递。如果我们第二次收到信号，我们
     * 会将其解释为用户真的想尽快退出，而不需要等待在磁盘上保持。*/
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid(), 1);
        exit(1); /* Exit with an error since this was not a clean shutdown. 
                  *
                  * 退出时出错，因为这不是一次干净的关机。*/
    } else if (server.loading) {
        serverLogFromHandler(LL_WARNING, "Received shutdown signal during loading, exiting now.");
        exit(0);
    }

    serverLogFromHandler(LL_WARNING, msg);
    server.shutdown_asap = 1;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. 
     *
     * 如果在SA_flags中设置了SA_SIGINFO标志，则使用 sa_sigaction。否则，将使用sa_handler。*/
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

#ifdef HAVE_BACKTRACE
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
#endif
    return;
}

/* This is the signal handler for children process. It is currently useful
 * in order to track the SIGUSR1, that we send to a child in order to terminate
 * it in a clean way, without the parent detecting an error and stop
 * accepting writes because of a write error condition. 
 *
 * 这是子进程的信号处理程序。为了跟踪SIGUSR1，它目前很有用，我们将其发送给子进程，
 * 以便以干净的方式终止它，而不会让父级检测到错误并因写入错误条件而停止接受写入。*/
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    int level = server.in_fork_child == CHILD_TYPE_MODULE? LL_VERBOSE: LL_WARNING;
    serverLogFromHandler(level, "Received SIGUSR1 in child, exiting now.");
    exitFromChild(SERVER_CHILD_NOERROR_RETVAL);
}

void setupChildSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. 
     *
     * 如果在SA_flags中设置了SA_SIGINFO标志，则使用 sa_sigaction。否则，将使用sa_handler。*/
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigKillChildHandler;
    sigaction(SIGUSR1, &act, NULL);
    return;
}

/* After fork, the child process will inherit the resources
 * of the parent process, e.g. fd(socket or flock) etc.
 * should close the resources not used by the child process, so that if the
 * parent restarts it can bind/lock despite the child possibly still running. 
 *
 * fork之后，子进程将继承父进程的资源，例如fd（socket或flock）等。
 * 应该关闭子进程未使用的资源，这样，如果父进程重新启动，它可以绑定/锁定，尽管子进
 * 程可能仍在运行。*/
void closeClildUnusedResourceAfterFork() {
    closeListeningSockets(0);
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1)
        close(server.cluster_config_file_lock_fd);  /* don't care if this fails 
                                    *
                                    * 不管这是否失败*/

    /* Clear server.pidfile, this is the parent pidfile which should not
     * be touched (or deleted) by the child (on exit / crash) 
     *
     * 清除server.pidfile，这是父级pidfile，子进程不应接触（或删除）
     * （退出/崩溃时）*/
    zfree(server.pidfile);
    server.pidfile = NULL;
}

/* purpose is one of CHILD_TYPE_ types 
 *
 * 用途是CHILD_TYPE_类型之一*/
int redisFork(int purpose) {
    int childpid;
    long long start = ustime();
    // fork 进程
    if ((childpid = fork()) == 0) {
        /* Child */
        server.in_fork_child = purpose;
        setOOMScoreAdj(CONFIG_OOM_BGCHILD);
        setupChildSignalHandlers();
        updateDictResizePolicy();
        closeClildUnusedResourceAfterFork();
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            return -1;
        }
    }
    return childpid;
}

void sendChildCOWInfo(int ptype, char *pname) {
    size_t private_dirty = zmalloc_get_private_dirty(-1);

    if (private_dirty) {
        serverLog(LL_NOTICE,
            "%s: %zu MB of memory used by copy-on-write",
            pname, private_dirty/(1024*1024));
    }

    server.child_info_data.cow_size = private_dirty;
    sendChildInfo(ptype);
}

void memtest(size_t megabytes, int passes);

/* Returns 1 if there is --sentinel among the arguments or if
 * argv[0] contains "redis-sentinel". 
 *
 * 如果参数中有--sentinel，或者argv[0]包含“redis-sentinel”，则返回1。*/
int checkForSentinelMode(int argc, char **argv) {
    int j;

    if (strstr(argv[0],"redis-sentinel") != NULL) return 1;
    for (j = 1; j < argc; j++)
        if (!strcmp(argv[j],"--sentinel")) return 1;
    return 0;
}

/* Function called at startup to load RDB or AOF file in memory. 
 *
 * 函数在启动时调用，以便在内存中加载RDB或AOF文件。*/
void loadDataFromDisk(void) {
    long long start = ustime();
    if (server.aof_state == AOF_ON) {
        if (loadAppendOnlyFile(server.aof_filename) == C_OK)
            serverLog(LL_NOTICE,"DB loaded from append only file: %.3f seconds",(float)(ustime()-start)/1000000);
    } else {
        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        errno = 0; /* Prevent a stale value from affecting error checking 
                    *
                    * 防止过时的值影响错误检查*/
        if (rdbLoad(server.rdb_filename,&rsi,RDBFLAGS_NONE) == C_OK) {
            serverLog(LL_NOTICE,"DB loaded from disk: %.3f seconds",
                (float)(ustime()-start)/1000000);

            /* Restore the replication ID / offset from the RDB file. 
             *
             * 从RDB文件中恢复复制ID/偏移量。*/
            if ((server.masterhost ||
                (server.cluster_enabled &&
                nodeIsSlave(server.cluster->myself))) &&
                rsi.repl_id_is_set &&
                rsi.repl_offset != -1 &&
                /* Note that older implementations may save a repl_stream_db
                 * of -1 inside the RDB file in a wrong way, see more
                 * information in function rdbPopulateSaveInfo. 
                 *
                 * 请注意，旧的实现可能会以错误的方式在RDB文件中保存-1的repl_stream_db，
                 * 请参阅函数rdbPopulateSaveInfo中的更多信息。*/
                rsi.repl_stream_db != -1)
            {
                memcpy(server.replid,rsi.repl_id,sizeof(server.replid));
                server.master_repl_offset = rsi.repl_offset;
                /* If we are a slave, create a cached master from this
                 * information, in order to allow partial resynchronizations
                 * with masters. 
                 *
                 * 如果我们是从设备，请根据此信息创建缓存的主设备，以便允许与主设备进行部分重新同步。*/
                replicationCacheMasterUsingMyself();
                selectDb(server.cached_master,rsi.repl_stream_db);
            }
        } else if (errno != ENOENT) {
            serverLog(LL_WARNING,"Fatal error loading the DB: %s. Exiting.",strerror(errno));
            exit(1);
        }
    }
}

void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING,"Out Of Memory allocating %zu bytes!",
        allocation_size);
    serverPanic("Redis aborting for OUT OF MEMORY. Allocating %zu bytes!", 
        allocation_size);
}

void redisSetProcTitle(char *title) {
#ifdef USE_SETPROCTITLE
    char *server_mode = "";
    if (server.cluster_enabled) server_mode = " [cluster]";
    else if (server.sentinel_mode) server_mode = " [sentinel]";

    setproctitle("%s %s:%d%s",
        title,
        server.bindaddr_count ? server.bindaddr[0] : "*",
        server.port ? server.port : server.tls_port,
        server_mode);
#else
    UNUSED(title);
#endif
}

void redisSetCpuAffinity(const char *cpulist) {
#ifdef USE_SETCPUAFFINITY
    setcpuaffinity(cpulist);
#else
    UNUSED(cpulist);
#endif
}

/*
 * Check whether systemd or upstart have been used to start redis.
 *
 * 检查是否已使用systemd或upstart启动redis。*/

int redisSupervisedUpstart(void) {
    const char *upstart_job = getenv("UPSTART_JOB");

    if (!upstart_job) {
        serverLog(LL_WARNING,
                "upstart supervision requested, but UPSTART_JOB not found");
        return 0;
    }

    serverLog(LL_NOTICE, "supervised by upstart, will stop to signal readiness");
    raise(SIGSTOP);
    unsetenv("UPSTART_JOB");
    return 1;
}

int redisCommunicateSystemd(const char *sd_notify_msg) {
    const char *notify_socket = getenv("NOTIFY_SOCKET");
    if (!notify_socket) {
        serverLog(LL_WARNING,
                "systemd supervision requested, but NOTIFY_SOCKET not found");
    }

    #ifdef HAVE_LIBSYSTEMD
    (void) sd_notify(0, sd_notify_msg);
    #else
    UNUSED(sd_notify_msg);
    #endif
    return 0;
}

int redisIsSupervised(int mode) {
    if (mode == SUPERVISED_AUTODETECT) {
        const char *upstart_job = getenv("UPSTART_JOB");
        const char *notify_socket = getenv("NOTIFY_SOCKET");

        if (upstart_job) {
            redisSupervisedUpstart();
        } else if (notify_socket) {
            server.supervised_mode = SUPERVISED_SYSTEMD;
            serverLog(LL_WARNING,
                "WARNING auto-supervised by systemd - you MUST set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
            return redisCommunicateSystemd("STATUS=Redis is loading...\n");
        }
    } else if (mode == SUPERVISED_UPSTART) {
        return redisSupervisedUpstart();
    } else if (mode == SUPERVISED_SYSTEMD) {
        serverLog(LL_WARNING,
            "WARNING supervised by systemd - you MUST set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
        return redisCommunicateSystemd("STATUS=Redis is loading...\n");
    }

    return 0;
}

int iAmMaster(void) {
    return ((!server.cluster_enabled && server.masterhost == NULL) ||
            (server.cluster_enabled && nodeIsMaster(server.cluster->myself)));
}

int main(int argc, char **argv) {
    struct timeval tv;
    int j;

#ifdef REDIS_TEST
    if (argc == 3 && !strcasecmp(argv[1], "test")) {
        if (!strcasecmp(argv[2], "ziplist")) {
            return ziplistTest(argc, argv);
        } else if (!strcasecmp(argv[2], "quicklist")) {
            quicklistTest(argc, argv);
        } else if (!strcasecmp(argv[2], "intset")) {
            return intsetTest(argc, argv);
        } else if (!strcasecmp(argv[2], "zipmap")) {
            return zipmapTest(argc, argv);
        } else if (!strcasecmp(argv[2], "sha1test")) {
            return sha1Test(argc, argv);
        } else if (!strcasecmp(argv[2], "util")) {
            return utilTest(argc, argv);
        } else if (!strcasecmp(argv[2], "endianconv")) {
            return endianconvTest(argc, argv);
        } else if (!strcasecmp(argv[2], "crc64")) {
            return crc64Test(argc, argv);
        } else if (!strcasecmp(argv[2], "zmalloc")) {
            return zmalloc_test(argc, argv);
        }

        return -1; /* test not found 
                    *
                    * 未找到测试*/
    }
#endif

    /* We need to initialize our libraries, and the server configuration. 
     *
     * 我们需要初始化我们的库和服务器配置。*/
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif
    setlocale(LC_COLLATE,"");
    tzset(); /* Populates 'timezone' global. 
              *
              * 填充“时区”全局。*/
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);
    srand(time(NULL)^getpid());
    gettimeofday(&tv,NULL);
    init_genrand64(((long long) tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());
    crc64_init();

    /* Store umask value. Because umask(2) only offers a set-and-get API we have
     * to reset it and restore it back. We do this early to avoid a potential
     * race condition with threads that could be creating files or directories.
     *
     * 存储umask值。因为umask（2）只提供一个set-and-get API，
     * 所以我们必须重置它并恢复它。我们提前这样做是为了避免可能创建文件或目录的线程出现
     * 潜在的竞争条件。*/
    umask(server.umask = umask(0777));

    uint8_t hashseed[16];
    getRandomBytes(hashseed,sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);
    server.sentinel_mode = checkForSentinelMode(argc,argv);
    // 初始化 server 变量
    initServerConfig();
    ACLInit(); /* The ACL subsystem must be initialized ASAP because the
                  basic networking code and client creation depends on it. 
                *
                * ACL子系统必须尽快初始化，因为基本的网络代码和客户端创建都依赖于它。*/
    moduleInitModulesSystem();
    tlsInit();

    /* Store the executable path and arguments in a safe place in order
     * to be able to restart the server later. 
     *
     * 将可执行路径和参数存储在安全的地方，以便以后能够重新启动服务器。*/
    server.executable = getAbsolutePath(argv[0]);
    server.exec_argv = zmalloc(sizeof(char*)*(argc+1));
    server.exec_argv[argc] = NULL;
    for (j = 0; j < argc; j++) server.exec_argv[j] = zstrdup(argv[j]);

    /* We need to init sentinel right now as parsing the configuration file
     * in sentinel mode will have the effect of populating the sentinel
     * data structures with master nodes to monitor. 
     *
     * 我们现在需要初始化sentinel，因为在sentinel模式下解析配置文件将产
     * 生用要监视的主节点填充sentinel数据结构的效果。*/
    if (server.sentinel_mode) {
        initSentinelConfig();
        initSentinel();
    }

    /* Check if we need to start in redis-check-rdb/aof mode. We just execute
     * the program main. However the program is part of the Redis executable
     * so that we can easily execute an RDB check on loading errors. 
     *
     * 检查我们是否需要在redis-Check rdb/aof模式下启动。我们只是执行
     * 程序main。然而，该程序是Redis可执行文件的一部分，因此我们可以轻松地对加
     * 载错误执行RDB检查。*/
    if (strstr(argv[0],"redis-check-rdb") != NULL)
        redis_check_rdb_main(argc,argv,NULL);
    else if (strstr(argv[0],"redis-check-aof") != NULL)
        redis_check_aof_main(argc,argv);

    // 读入选项和配置文件，并修改服务器配置
    if (argc >= 2) {
        j = 1; /* First option to parse in argv[] 
                *
                * 在argv[]中分析*/
        sds options = sdsempty();
        char *configfile = NULL;

        /* Handle special options --help and --version 
         *
         * 处理特殊选项  --help and --version*/
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0) usage();
        if (strcmp(argv[1], "--test-memory") == 0) {
            if (argc == 3) {
                memtest(atoi(argv[2]),50);
                exit(0);
            } else {
                fprintf(stderr,"Please specify the amount of memory to test in megabytes.\n");
                fprintf(stderr,"Example: ./redis-server --test-memory 4096\n\n");
                exit(1);
            }
        }

        /* First argument is the config file name? 
         *
         * 第一个参数是配置文件名？*/
        if (argv[j][0] != '-' || argv[j][1] != '-') {
            configfile = argv[j];
            server.configfile = getAbsolutePath(configfile);
            /* Replace the config file in server.exec_argv with
             * its absolute path. 
             *
             * 将server.exec_argv中的配置文件替换为其绝对路径。*/
            zfree(server.exec_argv[j]);
            server.exec_argv[j] = zstrdup(server.configfile);
            j++;
        }

        /* All the other options are parsed and conceptually appended to the
         * configuration file. For instance --port 6380 will generate the
         * string "port 6380\n" to be parsed after the actual file name
         * is parsed, if any. 
         *
         * 所有其他选项都经过解析，并在概念上附加到配置文件中。例如，端口6380将在解析实
         * 际文件名（如果有的话）后生成要解析的字符串“端口6380”。*/

        // 读入并分析（parse）配置选项，然后将它们加入到配置文件的最后
        while(j != argc) {
            if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name 
                 *
                 * 选项名称*/
                if (!strcmp(argv[j], "--check-rdb")) {
                    /* Argument has no options, need to skip for parsing. 
                     *
                     * 参数没有选项，需要跳过进行解析。*/
                    j++;
                    continue;
                }
                if (sdslen(options)) options = sdscat(options,"\n");
                options = sdscat(options,argv[j]+2);
                options = sdscat(options," ");
            } else {
                /* Option argument 
                 *
                 * 选项自变量*/
                options = sdscatrepr(options,argv[j],strlen(argv[j]));
                options = sdscat(options," ");
            }
            j++;
        }
        if (server.sentinel_mode && configfile && *configfile == '-') {
            serverLog(LL_WARNING,
                "Sentinel config from STDIN not allowed.");
            serverLog(LL_WARNING,
                "Sentinel needs config file on disk to save state.  Exiting...");
            exit(1);
        }
        // 定义于 config.c ，
        // 用于清空保存 server.saveparams 和 server.saveparamslen
        resetServerSaveParams();
        // 根据配置文件和传入的选项，修改 server 变量（服务器配置）
        loadServerConfig(configfile,options);
        sdsfree(options);
    }

    server.supervised = redisIsSupervised(server.supervised_mode);
    int background = server.daemonize && !server.supervised;
    // 创建 daemon 进程
    if (background) daemonize();

    serverLog(LL_WARNING, "oO0OoO0OoO0Oo Redis is starting oO0OoO0OoO0Oo");
    serverLog(LL_WARNING,
        "Redis version=%s, bits=%d, commit=%s, modified=%d, pid=%d, just started",
            REDIS_VERSION,
            (sizeof(long) == 8) ? 64 : 32,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (int)getpid());

    if (argc == 1) {
        serverLog(LL_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/%s.conf", argv[0], server.sentinel_mode ? "sentinel" : "redis");
    } else {
        serverLog(LL_WARNING, "Configuration loaded");
    }

    readOOMScoreAdj();
    // 初始化服务器功能
    initServer();
    // 打开 PID 文件
    if (background || server.pidfile) createPidFile();
    redisSetProcTitle(argv[0]);
    redisAsciiArt();
    checkTcpBacklogSettings();

    // 如果不在 SENTINEL 模式，那么打印服务器信息
    if (!server.sentinel_mode) {
        /* Things not needed when running in Sentinel mode. 
         *
         * 在Sentinel模式下运行时不需要的东西。*/
        serverLog(LL_WARNING,"Server initialized");
    #ifdef __linux__
        linuxMemoryWarnings();
    #if defined (__arm64__)
        int ret;
        if ((ret = linuxMadvFreeForkBugCheck())) {
            if (ret == 1)
                serverLog(LL_WARNING,"WARNING Your kernel has a bug that could lead to data corruption during background save. "
                                     "Please upgrade to the latest stable kernel.");
            else
                serverLog(LL_WARNING, "Failed to test the kernel for a bug that could lead to data corruption during background save. "
                                      "Your system could be affected, please report this error.");
            if (!checkIgnoreWarning("ARM64-COW-BUG")) {
                serverLog(LL_WARNING,"Redis will now exit to prevent data corruption. "
                                     "Note that it is possible to suppress this warning by setting the following config: ignore-warnings ARM64-COW-BUG");
                exit(1);
            }
        }
    #endif /* __arm64__ */
    #endif /* __linux__ */
        moduleLoadFromQueue();
        ACLLoadUsersAtStartup();
        InitServerLast();
        // 从 RDB 文件或 AOF 文件中载入数据
        loadDataFromDisk();
        if (server.cluster_enabled) {
            if (verifyClusterConfigWithData() == C_ERR) {
                serverLog(LL_WARNING,
                    "You can't have keys in a DB different than DB 0 when in "
                    "Cluster mode. Exiting.");
                exit(1);
            }
        }
        if (server.ipfd_count > 0 || server.tlsfd_count > 0)
            serverLog(LL_NOTICE,"Ready to accept connections");
        if (server.sofd > 0)
            serverLog(LL_NOTICE,"The server is now ready to accept connections at %s", server.unixsocket);
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            if (!server.masterhost) {
                redisCommunicateSystemd("STATUS=Ready to accept connections\n");
                redisCommunicateSystemd("READY=1\n");
            } else {
                redisCommunicateSystemd("STATUS=Waiting for MASTER <-> REPLICA sync\n");
            }
        }
    } else {
        InitServerLast();
        sentinelIsRunning();
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            redisCommunicateSystemd("READY=1\n");
        }
    }

    /* Warning the user about suspicious maxmemory setting. 
     *
     * 警告用户有关可疑的maxmemory设置。*/
    // 打印内存限制警告
    if (server.maxmemory > 0 && server.maxmemory < 1024*1024) {
        serverLog(LL_WARNING,"WARNING: You specified a maxmemory value that is less than 1MB (current value is %llu bytes). Are you sure this is what you really want?", server.maxmemory);
    }

    redisSetCpuAffinity(server.server_cpulist);
    setOOMScoreAdj(-1);

    // 启动服务器循环
    aeMain(server.el);
    // 关闭服务器，删除事件
    aeDeleteEventLoop(server.el);
    return 0;
}

/* The End */
