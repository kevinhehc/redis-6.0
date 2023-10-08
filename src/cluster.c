/* Redis Cluster implementation.
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
#include "cluster.h"
#include "endianconv.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <math.h>

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. 
 *
 * 全局引用我自己可以方便地使代码更加清晰。Myself总是指向server.cluster->Myself，也就是代表这个节点的clusterNode。
 * */
clusterNode *myself = NULL;

clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(connection *conn);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode *n, int slot);
sds clusterGenNodesDescription(int filter);
clusterNode *clusterLookupNode(const char *name);
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);
int clusterNodeSetSlotBit(clusterNode *n, int slot);
void clusterSetMaster(clusterNode *n);
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsMaster(clusterNode *n);
void clusterDelNode(clusterNode *delnode);
sds representClusterNodeFlags(sds ci, uint16_t flags);
uint64_t clusterGetMaxEpoch(void);
int clusterBumpConfigEpochWithoutConsensus(void);
void moduleCallClusterReceivers(const char *sender_id, uint64_t module_id, uint8_t type, const unsigned char *payload, uint32_t len);

#define RCVBUF_INIT_LEN 1024
#define RCVBUF_MAX_PREALLOC (1<<20) /* 1MB 
                                     * */

/* -----------------------------------------------------------------------------
 * Initialization
 * 初始化
 * --------------------------------------------------------------------------*/

/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), C_ERR is returned.
 * If the configuration was loaded from the file, C_OK is returned. 
 *
 * 从“filename”加载群集配置。
 *
 * 如果文件不存在或长度为零（这可能是因为当我们锁定nodes.conf文件时，
 * 如果它不存在，我们会创建一个长度为零的文件来进行锁定），则返回C_ERR。
 * 如果配置是从文件加载的，则返回C_OK。
 * */
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    struct stat sb;
    char *line;
    int maxline, j;

    if (fp == NULL) {
        if (errno == ENOENT) {
            return C_ERR;
        } else {
            serverLog(LL_WARNING,
                "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
            exit(1);
        }
    }

    /* Check if the file is zero-length: if so return C_ERR to signal
     * we have to write the config. 
     *
     * 检查文件是否为零长度：如果为零，则返回C_ERR以表示我们必须写入配置。
     * */
    if (fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        fclose(fp);
        return C_ERR;
    }

    /* Parse the file. Note that single lines of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case, half of the Redis slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * To simplify we allocate 1024+CLUSTER_SLOTS*128 bytes per line. 
     *
     * 分析文件。请注意，集群配置文件的单行可能非常长，因为它们包括节点的所有哈希槽。
     * 这意味着在最坏的情况下，Redis插槽的一半将出现在一行中，可能处于导入或迁移状态，
     * 因此与发送方/接收方的节点ID一起出现。
     *
     * 为了简化，我们每行分配1024+CLUSTER_SLOTS*128个字节。
     * */
    maxline = 1024+CLUSTER_SLOTS*128;
    line = zmalloc(maxline);
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n, *master;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. 
         *
         * 跳过空行，它们可以由用户手动编辑nodes.conf创建，也可以由配置写入过程创建（ 如果在truncate() 调用之前停止 ）。
         * */
        if (line[0] == '\n' || line[0] == '\0') continue;

        /* Split the line into arguments for processing. 
         *
         * 将行拆分为要处理的参数。
         * */
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by Redis. 
         *
         * 处理特殊的“vars”线路。不要假装它是最后一行，即使它实际上是Redis生成的。
         * */
        if (strcasecmp(argv[0],"vars") == 0) {
            if (!(argc % 2)) goto fmterr;
            for (j = 1; j < argc; j += 2) {
                if (strcasecmp(argv[j],"currentEpoch") == 0) {
                    server.cluster->currentEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else if (strcasecmp(argv[j],"lastVoteEpoch") == 0) {
                    server.cluster->lastVoteEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else {
                    serverLog(LL_WARNING,
                        "Skipping unknown cluster config variable '%s'",
                        argv[j]);
                }
            }
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Regular config lines have at least eight fields 
         *
         * 常规配置行至少有八个字段
         * */
        if (argc < 8) {
            sdsfreesplitres(argv,argc);
            goto fmterr;
        }

        /* Create this node if it does not exist 
         *
         * 如果该节点不存在，则创建该节点
         * */
        n = clusterLookupNode(argv[0]);
        if (!n) {
            n = createClusterNode(argv[0],0);
            clusterAddNode(n);
        }
        /* Address and port 
         *
         * 地址和端口
         * */
        if ((p = strrchr(argv[1],':')) == NULL) {
            sdsfreesplitres(argv,argc);
            goto fmterr;
        }
        *p = '\0';
        memcpy(n->ip,argv[1],strlen(argv[1])+1);
        char *port = p+1;
        char *busp = strchr(port,'@');
        if (busp) {
            *busp = '\0';
            busp++;
        }
        n->port = atoi(port);
        /* In older versions of nodes.conf the "@busport" part is missing.
         * In this case we set it to the default offset of 10000 from the
         * base port. 
         *
         * 在nodes.conf的旧版本中，缺少“@busport”部分。
         * 在这种情况下，我们将其设置为从基本端口的默认偏移量10000。
         * */
        n->cport = busp ? atoi(busp) : n->port + CLUSTER_PORT_INCR;

        /* Parse flags 
         *
         * 分析标志
         * */
        p = s = argv[2];
        while(p) {
            p = strchr(s,',');
            if (p) *p = '\0';
            if (!strcasecmp(s,"myself")) {
                serverAssert(server.cluster->myself == NULL);
                myself = server.cluster->myself = n;
                n->flags |= CLUSTER_NODE_MYSELF;
            } else if (!strcasecmp(s,"master")) {
                n->flags |= CLUSTER_NODE_MASTER;
            } else if (!strcasecmp(s,"slave")) {
                n->flags |= CLUSTER_NODE_SLAVE;
            } else if (!strcasecmp(s,"fail?")) {
                n->flags |= CLUSTER_NODE_PFAIL;
            } else if (!strcasecmp(s,"fail")) {
                n->flags |= CLUSTER_NODE_FAIL;
                n->fail_time = mstime();
            } else if (!strcasecmp(s,"handshake")) {
                n->flags |= CLUSTER_NODE_HANDSHAKE;
            } else if (!strcasecmp(s,"noaddr")) {
                n->flags |= CLUSTER_NODE_NOADDR;
            } else if (!strcasecmp(s,"nofailover")) {
                n->flags |= CLUSTER_NODE_NOFAILOVER;
            } else if (!strcasecmp(s,"noflags")) {
                /* nothing to do 
                 *
                 * 不做任何事情
                 * */
            } else {
                serverPanic("Unknown flag in redis cluster config file");
            }
            if (p) s = p+1;
        }

        /* Get master if any. Set the master and populate master's
         * slave list. 
         *
         * 如果有的话，找主节点。设置master并填充master的slave列表。
         * */
        if (argv[3][0] != '-') {
            master = clusterLookupNode(argv[3]);
            if (!master) {
                master = createClusterNode(argv[3],0);
                clusterAddNode(master);
            }
            n->slaveof = master;
            clusterNodeAddSlave(master,n);
        }

        /* Set ping sent / pong received timestamps 
         *
         * 设置发送 ping / 接收 pong 的时间戳
         * */
        if (atoi(argv[4])) n->ping_sent = mstime();
        if (atoi(argv[5])) n->pong_received = mstime();

        /* Set configEpoch for this node. 
         *
         * 为此节点设置configEpoch。
         * */
        n->configEpoch = strtoull(argv[6],NULL,10);

        /* Populate hash slots served by this instance. 
         *
         * 填充此节点提供的哈希槽。
         * */
        for (j = 8; j < argc; j++) {
            int start, stop;

            if (argv[j][0] == '[') {
                /* Here we handle migrating / importing slots 
                 *
                 * 这里我们处理迁移/导入插槽
                 * */
                int slot;
                char direction;
                clusterNode *cn;

                p = strchr(argv[j],'-');
                serverAssert(p != NULL);
                *p = '\0';
                direction = p[1]; /* Either '>' or '<' 
                                   *
                                   * “>”或“<”
                                   * */
                slot = atoi(argv[j]+1);
                if (slot < 0 || slot >= CLUSTER_SLOTS) {
                    sdsfreesplitres(argv,argc);
                    goto fmterr;
                }
                p += 3;
                cn = clusterLookupNode(p);
                if (!cn) {
                    cn = createClusterNode(p,0);
                    clusterAddNode(cn);
                }
                if (direction == '>') {
                    server.cluster->migrating_slots_to[slot] = cn;
                } else {
                    server.cluster->importing_slots_from[slot] = cn;
                }
                continue;
            } else if ((p = strchr(argv[j],'-')) != NULL) {
                *p = '\0';
                start = atoi(argv[j]);
                stop = atoi(p+1);
            } else {
                start = stop = atoi(argv[j]);
            }
            if (start < 0 || start >= CLUSTER_SLOTS ||
                stop < 0 || stop >= CLUSTER_SLOTS)
            {
                sdsfreesplitres(argv,argc);
                goto fmterr;
            }
            while(start <= stop) clusterAddSlot(n, start++);
        }

        sdsfreesplitres(argv,argc);
    }
    /* Config sanity check 
     *
     * 配置健全性检查
     * */
    if (server.cluster->myself == NULL) goto fmterr;

    zfree(line);
    fclose(fp);

    serverLog(LL_NOTICE,"Node configuration loaded, I'm %.40s", myself->name);

    /* Something that should never happen: currentEpoch smaller than
     * the max epoch found in the nodes configuration. However we handle this
     * as some form of protection against manual editing of critical files. 
     *
     * 不应该发生的事情：currentEpoch小于节点配置中的最大epoch。
     * 然而，我们将其视为某种形式的保护，以防止对关键文件进行手动编辑。
     * */
    if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
        server.cluster->currentEpoch = clusterGetMaxEpoch();
    }
    return C_OK;

fmterr:
    serverLog(LL_WARNING,
        "Unrecoverable error: corrupted cluster config file.");
    zfree(line);
    if (fp) fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. 
 *
 * 群集节点配置与Cluster NODES输出完全相同。
 *
 * 此函数写入节点配置并返回0，如果出现错误，则返回1。
 *
 * 注意：从POSIX文件系统语义的角度来看，我们需要以原子方式写入文件，
 * 这样，如果服务器在写入过程中停止或崩溃，我们将以旧文件或新文件结束。
 * 由于我们有完整的可写负载，我们可以使用一次写入来写入整个文件。
 * 如果预先存在的文件更大，我们用换行符填充负载，这些换行符无论如何都会被忽略，然后截断文件。
 * */
int clusterSaveConfig(int do_fsync) {
    sds ci;
    size_t content_size;
    struct stat sb;
    int fd;

    // todo_before_sleep 的状态 去除 CLUSTER_TODO_SAVE_CONFIG，因为是与 反。
    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

    /* Get the nodes description and concatenate our "vars" directive to
     * save currentEpoch and lastVoteEpoch. 
     *
     * 获取节点描述并连接我们的“vars”指令以保存currentEpoch和lastVoteEpoch。
     * */
    ci = clusterGenNodesDescription(CLUSTER_NODE_HANDSHAKE);
    ci = sdscatprintf(ci,"vars currentEpoch %llu lastVoteEpoch %llu\n",
        (unsigned long long) server.cluster->currentEpoch,
        (unsigned long long) server.cluster->lastVoteEpoch);
    content_size = sdslen(ci);

    if ((fd = open(server.cluster_configfile,O_WRONLY|O_CREAT,0644))
        == -1) goto err;

    /* Pad the new payload if the existing file length is greater. 
     *
     * 如果现有文件长度较大，则填充新的有效载荷。
     * */
    if (fstat(fd,&sb) != -1) {
        if (sb.st_size > (off_t)content_size) {
            ci = sdsgrowzero(ci,sb.st_size);
            memset(ci+content_size,'\n',sb.st_size-content_size);
        }
    }
    if (write(fd,ci,sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
    if (do_fsync) {
        // 如果立刻刷盘，那就在取消刷盘的任务
        server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
        fsync(fd);
    }

    /* Truncate the file if needed to remove the final \n padding that
     * is just garbage. 
     *
     * 如果需要，请截断文件，以删除最后的垃圾填充。
     * */
    if (content_size != sdslen(ci) && ftruncate(fd,content_size) == -1) {
        /* ftruncate() failing is not a critical error. 
         *
         * ftruncate（）失败不是一个严重错误。
         * */
    }
    close(fd);
    sdsfree(ci);
    return 0;

err:
    if (fd != -1) close(fd);
    sdsfree(ci);
    return -1;
}

void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == -1) {
        serverLog(LL_WARNING,"Fatal: can't update cluster config file.");
        exit(1);
    }
}

/* Lock the cluster config using flock(), and leaks the file descriptor used to
 * acquire the lock so that the file will be locked forever.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success C_OK is returned, otherwise an error is logged and
 * the function returns C_ERR to signal a lock was not acquired. 
 *
 * 使用flock（）锁定集群配置，并泄漏用于获取锁的文件描述符，这样文件将永远被锁定。
 *
 * 这是因为我们总是用新版本更新nodes.conf，重新打开文件，并在适当的位置写入（稍后用ftruncate（）调整长度）。
 *
 * 成功时返回C_OK，否则记录错误，函数返回C_ERR以表示未获取锁。
 * */
int clusterLockConfig(char *filename) {
/* flock() does not exist on Solaris
 * and a fcntl-based solution won't help, as we constantly re-open that file,
 * which will release _all_ locks anyway
 
 *
 * flock（）在Solaris上不存在，基于fcntl的解决方案也无济于事，因为
 * 我们不断地重新打开该文件，它无论如何都会释放_all_锁
 * */
#if !defined(__sun)
    /* To lock it, we need to open the file in a way it is created if
     * it does not exist, otherwise there is a race condition with other
     * processes. 
     *
     * 要锁定它，我们需要以创建文件的方式打开它（如果它不存在），否则会与其他进程发生竞争。
     * */
    int fd = open(filename,O_WRONLY|O_CREAT,0644);
    if (fd == -1) {
        serverLog(LL_WARNING,
            "Can't open %s in order to acquire a lock: %s",
            filename, strerror(errno));
        return C_ERR;
    }

    if (flock(fd,LOCK_EX|LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            serverLog(LL_WARNING,
                 "Sorry, the cluster configuration file %s is already used "
                 "by a different Redis Cluster node. Please make sure that "
                 "different nodes use different cluster configuration "
                 "files.", filename);
        } else {
            serverLog(LL_WARNING,
                "Impossible to lock %s: %s", filename, strerror(errno));
        }
        close(fd);
        return C_ERR;
    }
    /* Lock acquired: leak the 'fd' by not closing it, so that we'll retain the
     * lock to the file as long as the process exists.
     *
     * After fork, the child process will get the fd opened by the parent process,
     * we need save `fd` to `cluster_config_file_lock_fd`, so that in redisFork(),
     * it will be closed in the child process.
     * If it is not closed, when the main process is killed -9, but the child process
     * (redis-aof-rewrite) is still alive, the fd(lock) will still be held by the
     * child process, and the main process will fail to get lock, means fail to start. 
     *
     * 获取的锁：通过不关闭“fd”来泄漏它，这样只要进程存在，我们就会保留对文件的锁。
     *
     *
     * fork之后，子进程会得到父进程打开的fd，我们需要将“fd”保存到“cluster_config_file_lock_fd”中，
     * 这样在redisFork（）中，它就会在子进程中关闭。如果它没有关闭，当主进程被终止-9，但子进程（reds-aof rewrite）
     * 仍然活着时，fd（锁）仍将由子进程持有，主进程将无法获得锁，意味着无法启动。
     * */
    server.cluster_config_file_lock_fd = fd;
#endif /* __sun */

    return C_OK;
}

/* Some flags (currently just the NOFAILOVER flag) may need to be updated
 * in the "myself" node based on the current configuration of the node,
 * that may change at runtime via CONFIG SET. This function changes the
 * set of flags in myself->flags accordingly. 
 *
 * 一些标志（目前仅为NOFAILOVER标志）可能需要在 myself 节点中根据节点的
 * 当前配置进行更新，这可能会在运行时通过CONFIG SET进行更改。此函数相应地
 * 更改myself->标志中的标志集。
 * */
void clusterUpdateMyselfFlags(void) {
    int oldflags = myself->flags;
    int nofailover = server.cluster_slave_no_failover ?
                     CLUSTER_NODE_NOFAILOVER : 0;
    myself->flags &= ~CLUSTER_NODE_NOFAILOVER;
    myself->flags |= nofailover;
    if (myself->flags != oldflags) {
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE);
    }
}

void clusterInit(void) {
    int saveconf = 0;

    server.cluster = zmalloc(sizeof(clusterState));
    server.cluster->myself = NULL;
    server.cluster->currentEpoch = 0;
    server.cluster->state = CLUSTER_FAIL;
    server.cluster->size = 1;
    server.cluster->todo_before_sleep = 0;
    server.cluster->nodes = dictCreate(&clusterNodesDictType,NULL);
    server.cluster->nodes_black_list =
        dictCreate(&clusterNodesBlackListDictType,NULL);
    server.cluster->failover_auth_time = 0;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_rank = 0;
    server.cluster->failover_auth_epoch = 0;
    server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
    server.cluster->lastVoteEpoch = 0;
    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
        server.cluster->stats_bus_messages_sent[i] = 0;
        server.cluster->stats_bus_messages_received[i] = 0;
    }
    server.cluster->stats_pfail_nodes = 0;
    memset(server.cluster->slots,0, sizeof(server.cluster->slots));
    clusterCloseAllSlots();

    /* Lock the cluster config file to make sure every node uses
     * its own nodes.conf. 
     *
     * 锁定集群配置文件以确保每个节点都使用自己的nodes.conf。
     * */
    server.cluster_config_file_lock_fd = -1;
    if (clusterLockConfig(server.cluster_configfile) == C_ERR)
        exit(1);

    /* Load or create a new nodes configuration. 
     *
     * 加载或创建新的节点配置。
     * */
    if (clusterLoadConfig(server.cluster_configfile) == C_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. 
         *
         * 找不到配置。我们将只使用createClusterNode（）函数提供的随机名称。
         * */
        myself = server.cluster->myself =
            createClusterNode(NULL,CLUSTER_NODE_MYSELF|CLUSTER_NODE_MASTER);
        serverLog(LL_NOTICE,"No cluster configuration found, I'm %.40s",
            myself->name);
        clusterAddNode(myself);
        saveconf = 1;
    }
    if (saveconf) clusterSaveConfigOrDie(1);

    /* We need a listening TCP port for our cluster messaging needs. 
     *
     * 我们需要一个侦听TCP端口来满足集群消息传递的需要。
     * */
    server.cfd_count = 0;

    /* Port sanity check II
     * The other handshake port check is triggered too late to stop
     * us from trying to use a too-high cluster port number. 
     *
     * 端口健全性检查II另一个握手端口检查触发得太晚，无法阻止我们尝试使用过高的群集端口号。
     * */
    int port = server.tls_cluster ? server.tls_port : server.port;
    if (port > (65535-CLUSTER_PORT_INCR)) {
        serverLog(LL_WARNING, "Redis port number too high. "
                   "Cluster communication port is 10,000 port "
                   "numbers higher than your Redis port. "
                   "Your Redis port number must be "
                   "lower than 55535.");
        exit(1);
    }
    if (listenToPort(port+CLUSTER_PORT_INCR,
        server.cfd,&server.cfd_count) == C_ERR)
    {
        exit(1);
    } else {
        int j;

        for (j = 0; j < server.cfd_count; j++) {
            if (aeCreateFileEvent(server.el, server.cfd[j], AE_READABLE,
                clusterAcceptHandler, NULL) == AE_ERR)
                    serverPanic("Unrecoverable error creating Redis Cluster "
                                "file event.");
        }
    }

    /* The slots -> keys map is a radix tree. Initialize it here. 
     *
     * slot->键映射是一个基数树。在此处初始化它。
     * */
    server.cluster->slots_to_keys = raxNew();
    memset(server.cluster->slots_keys_count,0,
           sizeof(server.cluster->slots_keys_count));

    /* Set myself->port / cport to my listening ports, we'll just need to
     * discover the IP address via MEET messages. 
     *
     * 将我自己->port/cport设置为我的侦听端口，我们只需要通过MEET消息发现IP地址。
     * */
    myself->port = port;
    myself->cport = port+CLUSTER_PORT_INCR;
    if (server.cluster_announce_port)
        myself->port = server.cluster_announce_port;
    if (server.cluster_announce_bus_port)
        myself->cport = server.cluster_announce_bus_port;

    server.cluster->mf_end = 0;
    resetManualFailover();
    clusterUpdateMyselfFlags();
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forgotten.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a slave, it turns into a master.
 * 4) Only for hard reset: a new Node ID is generated.
 * 5) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 6) The new configuration is saved and the cluster state updated.
 * 7) If the node was a slave, the whole data set is flushed away. 
 *
 * 重置执行软重置或硬重置的节点：
 * 1） 忘记所有其他节点。
 * 2） 所有分配/打开的插槽都将释放。
 * 3） 如果节点是从节点，则它将变为主节点。
 * 4） 仅用于硬重置：生成一个新的节点ID。
 * 5） 仅用于硬重置：currentEpoch和configEpoch设置为0。
 * 6） 将保存新配置并更新群集状态。
 * 7） 如果节点是从节点，那么整个数据集都会被清除。
 * */
void clusterReset(int hard) {
    dictIterator *di;
    dictEntry *de;
    int j;

    /* Turn into master. 
     *
     * 变成主节点。
     * */
    if (nodeIsSlave(myself)) {
        clusterSetNodeAsMaster(myself);
        replicationUnsetMaster();
        emptyDb(-1,EMPTYDB_NO_FLAGS,NULL);
    }

    /* Close slots, reset manual failover state. 
     *
     * 关闭插槽，重置手动故障切换状态。
     * */
    clusterCloseAllSlots();
    resetManualFailover();

    /* Unassign all the slots. 
     *
     * 取消分配所有插槽。
     * */
    for (j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Forget all the nodes, but myself. 
     *
     * 忘记所有的节点，只有我自己。
     * */
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == myself) continue;
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    /* Hard reset only: set epochs to 0, change node ID. 
     *
     * 仅硬重置：将历元设置为0，更改节点ID。
     * */
    if (hard) {
        sds oldname;

        server.cluster->currentEpoch = 0;
        server.cluster->lastVoteEpoch = 0;
        myself->configEpoch = 0;
        serverLog(LL_WARNING, "configEpoch set to 0 via CLUSTER RESET HARD");

        /* To change the Node ID we need to remove the old name from the
         * nodes table, change the ID, and re-add back with new name. 
         *
         * 要更改节点ID，我们需要从节点表中删除旧名称，更改ID，然后重新添加新名称。
         * */
        oldname = sdsnewlen(myself->name, CLUSTER_NAMELEN);
        dictDelete(server.cluster->nodes,oldname);
        sdsfree(oldname);
        getRandomHexChars(myself->name, CLUSTER_NAMELEN);
        clusterAddNode(myself);
        serverLog(LL_NOTICE,"Node hard reset, now I'm %.40s", myself->name);
    }

    /* Make sure to persist the new config and update the state. 
     *
     * 请确保保留新配置并更新状态。
     * */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE|
                         CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * CLUSTER通信链路
 * -----------------------------------------------------------------------------*/

clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->ctime = mstime();
    link->sndbuf = sdsempty();
    link->rcvbuf = zmalloc(link->rcvbuf_alloc = RCVBUF_INIT_LEN);
    link->rcvbuf_len = 0;
    link->node = node;
    link->conn = NULL;
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. 
 *
 * 释放集群链接，但当然不会释放关联的节点。此函数只会确保与此链接关联的原始节点的“链接”字段设置为NULL。
 * */
void freeClusterLink(clusterLink *link) {
    if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
    }
    sdsfree(link->sndbuf);
    zfree(link->rcvbuf);
    if (link->node)
        link->node->link = NULL;
    zfree(link);
}

static void clusterConnAcceptHandler(connection *conn) {
    clusterLink *link;

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_VERBOSE,
                "Error accepting cluster node connection: %s", connGetLastError(conn));
        connClose(conn);
        return;
    }

    /* Create a link object we use to handle the connection.
     * It gets passed to the readable handler when data is available.
     * Initially the link->node pointer is set to NULL as we don't know
     * which node is, but the right node is references once we know the
     * node identity. 
     *
     * 创建一个用于处理连接的链接对象。当数据可用时，它会被传递给可读处理程序。最初，
     * link->node指针被设置为NULL，因为我们不知道哪个节点是，但一旦我们知道节点标识，正确的节点就是引用。
     * */
    link = createClusterLink(NULL);
    link->conn = conn;
    connSetPrivateData(conn, link);

    /* Register read handler 
     *
     * 寄存器读取处理程序
     * */
    connSetReadHandler(conn, clusterReadHandler);
}

#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    /* If the server is starting up, don't accept cluster connections:
     * UPDATE messages may interact with the database content. 
     *
     * 如果服务器正在启动，不要接受集群连接：UPDATE消息可能会与数据库内容交互。
     * */
    if (server.masterhost == NULL && server.loading) return;

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_VERBOSE,
                    "Error accepting cluster node: %s", server.neterr);
            return;
        }

        connection *conn = server.tls_cluster ?
            connCreateAcceptedTLS(cfd, TLS_CLIENT_AUTH_YES) : connCreateAcceptedSocket(cfd);

        /* Make sure connection is not in an error state 
         *
         * 确保连接未处于错误状态
         * */
        if (connGetState(conn) != CONN_STATE_ACCEPTING) {
            serverLog(LL_VERBOSE,
                "Error creating an accepting connection for cluster node: %s",
                    connGetLastError(conn));
            connClose(conn);
            return;
        }
        connNonBlock(conn);
        connEnableTcpNoDelay(conn);
        connKeepAlive(conn,server.cluster_node_timeout / 1000 * 2);

        /* Use non-blocking I/O for cluster messages. 
         *
         * 对群集消息使用非阻塞I/O。
         * */
        serverLog(LL_VERBOSE,"Accepting cluster node connection from %s:%d", cip, cport);

        /* Accept the connection now.  connAccept() may call our handler directly
         * or schedule it for later depending on connection implementation.
         *
         * 立即接受连接。connAccept（）可以直接调用我们的处理程序，也可以根据连接实现将其安排在以后。
         * */
        if (connAccept(conn, clusterConnAcceptHandler) == C_ERR) {
            if (connGetState(conn) == CONN_STATE_ERROR)
                serverLog(LL_VERBOSE,
                        "Error accepting cluster node connection: %s",
                        connGetLastError(conn));
            connClose(conn);
            return;
        }
    }
}

/* Return the approximated number of sockets we are using in order to
 * take the cluster bus connections. 
 *
 * 返回用于获取集群总线连接的套接字的近似数量。
 * */
unsigned long getClusterConnectionsCount(void) {
    /* We decrement the number of nodes by one, since there is the
     * "myself" node too in the list. Each node uses two file descriptors,
     * one incoming and one outgoing, thus the multiplication by 2. 
     *
     * 我们将节点数量减少一，因为列表中也有“我自己”节点。每个节点使用两个文件描述符，
     * 一个传入，一个传出，因此乘以2。
     * */
    return server.cluster_enabled ?
           ((dictSize(server.cluster->nodes)-1)*2) : 0;
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * 键空间处理
 * ----------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). 
 *
 * 我们有16384个哈希槽。给定键的哈希槽被获得为该键的crc16的最低有效14位。
 *
 * 但是，如果键包含｛…｝模式，则仅对｛和｝之间的部分进行哈希处理。
 * 这在将来可能有助于强制某些键位于同一节点中（假设没有进行重新分发）。
 * */
unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } 
               *
               * ｛和｝的起始-结束索引
               * */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. 
     *
     * 没有“｛”？哈希整个键。这是基本情况。
     * */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. 
     *
     * 找到“｛”？请检查我们是否有相应的“｝”。
     * */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing between {} ? Hash the whole key. 
     *
     * 没有“｝”或在｛｝之间没有？哈希整个键。
     * */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. 
     *
     * 如果我们在这里，它的右边同时有一个{和一个}。哈希介于｛和｝之间的内容。
     * */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * 集群节点API
 * --------------------------------------------------------------------------*/

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. 
 *
 * 使用指定的标志创建一个新的群集节点。如果“nodename”为NULL，则这被视
 * 为第一次握手，并为该节点分配一个随机节点名称（稍后我们将收到第一个pong时会修复该名称）。
 *
 * 节点被创建并返回给用户，但它不会自动添加到节点哈希表中。
 * */
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));

    if (nodename)
        memcpy(node->name, nodename, CLUSTER_NAMELEN);
    else
        getRandomHexChars(node->name, CLUSTER_NAMELEN);
    node->ctime = mstime();
    node->configEpoch = 0;
    node->flags = flags;
    memset(node->slots,0,sizeof(node->slots));
    node->numslots = 0;
    node->numslaves = 0;
    node->slaves = NULL;
    node->slaveof = NULL;
    node->ping_sent = node->pong_received = 0;
    node->data_received = 0;
    node->fail_time = 0;
    node->link = NULL;
    memset(node->ip,0,sizeof(node->ip));
    node->port = 0;
    node->cport = 0;
    node->fail_reports = listCreate();
    node->voted_time = 0;
    node->orphaned_time = 0;
    node->repl_offset_time = 0;
    node->repl_offset = 0;
    listSetFreeMethod(node->fail_reports,zfree);
    return node;
}

/* This function is called every time we get a failure report from a node.
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. 
 *
 * 每当我们从 节点获得故障报告时，都会调用此函数。副作用是填充fail_reports列表（或更新现有报告的时间戳）
 *
 * failing是根据“sender”节点处于故障状态的节点。
 *
 * 如果函数只是更新来自同一发件人的现有故障报告的时间戳，则返回0。如果创建了新的故障报告，则返回1。
 * */
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {
    list *l = failing->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* If a failure report from the same sender already exists, just update
     * the timestamp. 
     *
     * 如果来自同一发件人的故障报告已经存在，只需更新时间戳即可。
     * */
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) {
            fr->time = mstime();
            return 0;
        }
    }

    /* Otherwise create a new report. 
     *
     * 否则，创建一个新报告。
     * */
    fr = zmalloc(sizeof(*fr));
    fr->node = sender;
    fr->time = mstime();
    listAddNodeTail(l,fr);
    return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. 
 *
 * 删除太旧的故障报告，其中太旧意味着合理地早于全局节点超时。
 *
 * 请注意，无论如何，要将节点标记为FAIL，我们需要具有至少早于全局节点超时的本地PFAIL状态，
 * 因此我们不只是信任来自其他节点的故障报告数量。
 * */
void clusterNodeCleanupFailureReports(clusterNode *node) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;
    mstime_t maxtime = server.cluster_node_timeout *
                     CLUSTER_FAIL_REPORT_VALIDITY_MULT;
    mstime_t now = mstime();

    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (now - fr->time > maxtime) listDelNode(l,ln);
    }
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. 
 *
 * 如果“sender”以前认为“node”失败，请删除它的失败报告。当节点通过gossip
 * 通知我们节点从其角度来看是正常的（没有FAIL或PFAIL标志）时，就会调用此函数。
 * 
 * 请注意，该函数被调用的频率相对较高，因为即使在没有节点发生故障的情况下也会被
 * 调用，并且为O（N），但是，当集群正常时，故障报告列表为空，因此该函数在恒定时间内运行。
 * 
 * 如果找到并删除了故障报告，函数将返回1。否则返回0。
 * */
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* Search for a failure report from this sender. 
     *
     * 搜索来自此发件人的故障报告。
     * */
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) break;
    }
    if (!ln) return 0; /* No failure report from this sender. 
                        *
                        * 没有来自此发件人的失败报告。
                        * */

    /* Remove the failure report. 
     *
     * 删除故障报告。
     * */
    listDelNode(l,ln);
    clusterNodeCleanupFailureReports(node);
    return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. 
 *
 * 返回认为“node”失败的外部节点数（不包括此节点），此节点可能也具有PFAIL或FAIL状态。
 * */
int clusterNodeFailureReportsCount(clusterNode *node) {
    clusterNodeCleanupFailureReports(node);
    return listLength(node->fail_reports);
}

int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
    int j;

    for (j = 0; j < master->numslaves; j++) {
        if (master->slaves[j] == slave) {
            if ((j+1) < master->numslaves) {
                int remaining_slaves = (master->numslaves - j) - 1;
                memmove(master->slaves+j,master->slaves+(j+1),
                        (sizeof(*master->slaves) * remaining_slaves));
            }
            master->numslaves--;
            if (master->numslaves == 0)
                master->flags &= ~CLUSTER_NODE_MIGRATE_TO;
            return C_OK;
        }
    }
    return C_ERR;
}

int clusterNodeAddSlave(clusterNode *master, clusterNode *slave) {
    int j;

    /* If it's already a slave, don't add it again. 
     *
     * 如果它已经是一个从节点，请不要再添加它。
     * */
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] == slave) return C_ERR;
    master->slaves = zrealloc(master->slaves,
        sizeof(clusterNode*)*(master->numslaves+1));
    master->slaves[master->numslaves] = slave;
    master->numslaves++;
    master->flags |= CLUSTER_NODE_MIGRATE_TO;
    return C_OK;
}

int clusterCountNonFailingSlaves(clusterNode *n) {
    int j, okslaves = 0;

    for (j = 0; j < n->numslaves; j++)
        if (!nodeFailed(n->slaves[j])) okslaves++;
    return okslaves;
}

/* Low level cleanup of the node structure. Only called by clusterDelNode(). 
 *
 * 节点结构的低级别清理。仅由clusterDelNode（）调用。
 * */
void freeClusterNode(clusterNode *n) {
    sds nodename;
    int j;

    /* If the node has associated slaves, we have to set
     * all the slaves->slaveof fields to NULL (unknown). 
     *
     * 如果节点有关联的slave，我们必须将所有slave->slaveof字段设置为 NULL（未知）。
     * */
    for (j = 0; j < n->numslaves; j++)
        n->slaves[j]->slaveof = NULL;

    /* Remove this node from the list of slaves of its master. 
     *
     * 将此节点从其主节点的从节点列表中删除。
     * */
    if (nodeIsSlave(n) && n->slaveof) clusterNodeRemoveSlave(n->slaveof,n);

    /* Unlink from the set of nodes. 
     *
     * 取消与节点集的链接。
     * */
    nodename = sdsnewlen(n->name, CLUSTER_NAMELEN);
    serverAssert(dictDelete(server.cluster->nodes,nodename) == DICT_OK);
    sdsfree(nodename);

    /* Release link and associated data structures. 
     *
     * 发布链接和相关的数据结构。
     * */
    if (n->link) freeClusterLink(n->link);
    listRelease(n->fail_reports);
    zfree(n->slaves);
    zfree(n);
}

/* Add a node to the nodes hash table 
 *
 * 将节点添加到节点哈希表
 * */
int clusterAddNode(clusterNode *node) {
    int retval;

    retval = dictAdd(server.cluster->nodes,
            sdsnewlen(node->name,CLUSTER_NAMELEN), node);
    return (retval == DICT_OK) ? C_OK : C_ERR;
}

/* Remove a node from the cluster. The function performs the high level
 * cleanup, calling freeClusterNode() for the low level cleanup.
 * Here we do the following:
 *
 * 1) Mark all the slots handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node and referenced by
 *    other nodes.
 * 3) Free the node with freeClusterNode() that will in turn remove it
 *    from the hash table and from the list of slaves of its master, if
 *    it is a slave node.
 
 *
 * 从群集中删除节点。函数执行高级清理，调用freeClusterNode（）进行低
 * 级清理。在这里，我们执行以下操作：
 * 
 * 1） 将其处理的所有插槽标记为未分配。
 * 
 * 2） 删除此节点发送并被其他节点引用的所有故障报告。
 * 
 * 3） 使用freeClusterNode（）释放节点，如果该节点是从节点，则会将其从哈希表和主节点的从节点列表中删除。
 * */
void clusterDelNode(clusterNode *delnode) {
    int j;
    dictIterator *di;
    dictEntry *de;

    /* 1) Mark slots as unassigned. 
     *
     * 1） 将插槽标记为未分配。
     * */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (server.cluster->importing_slots_from[j] == delnode)
            server.cluster->importing_slots_from[j] = NULL;
        if (server.cluster->migrating_slots_to[j] == delnode)
            server.cluster->migrating_slots_to[j] = NULL;
        if (server.cluster->slots[j] == delnode)
            clusterDelSlot(j);
    }

    /* 2) Remove failure reports. 
     *
     * 2） 删除故障报告。
     * */
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == delnode) continue;
        clusterNodeDelFailureReport(node,delnode);
    }
    dictReleaseIterator(di);

    /* 3) Free the node, unlinking it from the cluster. 
     *
     * 3） 释放节点，将其从集群中取消链接。
     * */
    freeClusterNode(delnode);
}

/* Node lookup by name 
 *
 * 按名称查找节点
 * */
clusterNode *clusterLookupNode(const char *name) {
    sds s = sdsnewlen(name, CLUSTER_NAMELEN);
    dictEntry *de;

    de = dictFind(server.cluster->nodes,s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. 
 *
 * 这只在握手后使用。当我们通过CLUSTER MEET连接给定的IP/PORT时，
 * 我们还没有节点名称，所以我们随机选择一个，并在收到PONG请求时使用此函数修复它。
 * */
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->name, CLUSTER_NAMELEN);

    serverLog(LL_DEBUG,"Renaming node %.40s into %.40s",
        node->name, newname);
    retval = dictDelete(server.cluster->nodes, s);
    sdsfree(s);
    serverAssert(retval == DICT_OK);
    memcpy(node->name, newname, CLUSTER_NAMELEN);
    clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER config epoch handling
 * CLUSTER配置epoch处理
 * --------------------------------------------------------------------------*/

/* Return the greatest configEpoch found in the cluster, or the current
 * epoch if greater than any node configEpoch. 
 *
 * 返回在集群中找到的最大configEpoch，如果大于任何节点configEpoch，则返回当前epoch。
 * */
uint64_t clusterGetMaxEpoch(void) {
    uint64_t max = 0;
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->configEpoch > max) max = node->configEpoch;
    }
    dictReleaseIterator(di);
    if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
    return max;
}

/* If this node epoch is zero or is not already the greatest across the
 * cluster (from the POV of the local configuration), this function will:
 *
 * 1) Generate a new config epoch, incrementing the current epoch.
 * 2) Assign the new epoch to this node, WITHOUT any consensus.
 * 3) Persist the configuration on disk before sending packets with the
 *    new configuration.
 *
 * If the new config epoch is generated and assigned, C_OK is returned,
 * otherwise C_ERR is returned (since the node has already the greatest
 * configuration around) and no operation is performed.
 *
 * Important note: this function violates the principle that config epochs
 * should be generated with consensus and should be unique across the cluster.
 * However Redis Cluster uses this auto-generated new config epochs in two
 * cases:
 *
 * 1) When slots are closed after importing. Otherwise resharding would be
 *    too expensive.
 * 2) When CLUSTER FAILOVER is called with options that force a slave to
 *    failover its master even if there is not master majority able to
 *    create a new configuration epoch.
 *
 * Redis Cluster will not explode using this function, even in the case of
 * a collision between this node and another node, generating the same
 * configuration epoch unilaterally, because the config epoch conflict
 * resolution algorithm will eventually move colliding nodes to different
 * config epochs. However using this function may violate the "last failover
 * wins" rule, so should only be used with care. 
 *
 * 如果此节点epoch为零，或者还不是集群中最大的（从本地配置的POV），则此函数将：
 * 
 * 1） 生成一个新的配置epoch，递增当前epoch。
 * 2） 在没有任何共识的情况下，将新纪元分配给这个节点。
 * 3） 在使用新配置发送数据包之前，请将配置保存在磁盘上。如果生成并分配了新的配置epoch，则返回C_OK，
 *     否则返回C_ERR（因为节点已经有了最大的配置），并且不执行任何操作。
 * 
 * 重要提示：此函数违反了配置历元应在一致的情况下生成的原则，并且应在整个集群中是唯一的。
 * 然而，Redis Cluster在两种情况下使用这种自动生成的新配置时期：
 * 
 * 1）导入后关闭插槽时。否则，重新发货的成本太高。
 * 
 * 2） 当调用CLUSTER FAILOVER时，即使没有能够创建新配置epoch的主节点，也可以使用强制从节点对其主节点进行故障切换的选项。
 * 
 * Redis Cluster使用该功能不会爆炸，即使在该节点和另一个节点发生冲突的情况下，也
 * 不会单方面生成相同的配置历元，因为配置历元冲突解决算法最终会将冲突节点移动到不同
 * 的配置历世。但是，使用此功能可能会违反“最后一次故障转移获胜”规则，因此应谨慎使 用。
 * */
int clusterBumpConfigEpochWithoutConsensus(void) {
    uint64_t maxEpoch = clusterGetMaxEpoch();

    if (myself->configEpoch == 0 ||
        myself->configEpoch != maxEpoch)
    {
        server.cluster->currentEpoch++;
        myself->configEpoch = server.cluster->currentEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);
        serverLog(LL_WARNING,
            "New configEpoch set to %llu",
            (unsigned long long) myself->configEpoch);
        return C_OK;
    } else {
        return C_ERR;
    }
}

/* This function is called when this node is a master, and we receive from
 * another master a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different slaves get the same config
 * epoch during a failover election, because the slaves need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for masters serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * masters having different configuration epochs whatever happened, since
 * nothing is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 
 *
 * 当这个节点是主节点，并且我们从另一个主节点接收到与我们的配置epoch相等的配置
 * epoc时，就会调用这个函数。背景在故障转移选举期间，不同的从节点不可能获得相同的
 * 配置时期，因为从节点需要获得多数票。
 * 
 * 然而，当我们执行集群的手动重新调度时，节点将为
 * 自己分配一个配置epoch，而无需征求同意。通常，当集群工作正常并由系统管理员监
 * 督时，就会进行重新调度。然而，故障转移可能正好发生在我们重新调度插槽的节点为自己
 * 分配新配置epoch的时候，但在它能够传播之前。因此，从技术上讲，在这种情况下，
 * 两个节点可能以相同的配置epoch结束。另一种可能性是，实现中存在导致这种情况发
 * 生的错误。此外，当创建一个新集群时，所有节点都以相同的configEpoch开始。
 * 
 * 此冲突解决代码允许节点在自动启动时自动以不同的configEpoch结束。在所
 * 有情况下，我们都希望有一个自动解决这个问题的机制作为保障。为不同的插槽集提供服务
 * 的主节点使用相同的配置epoch是无害的，但如果节点由于某种原因（手动错误或软件错
 * 误）而在没有适当的故障转移过程的情况下结束为相同的插槽提供服务，则是有害的。
 * 
 * 总的来说，我们希望一个系统最终总是以不同的主节点结束，无论发生什么，都有不同的配置时期，
 * 因为在分布式系统中，没有什么比大脑分裂更糟糕的了。行为当调用此函数时，如果与具
 * 有冲突epoch的另一个节点（“sender”节点）相比，此节点的node ID
 * 在字典上更小，则它将为自己分配当前在节点中检测到的最大配置epoch加1。
 * 
 * 这意味着，即使有多个节点发生冲突，节点ID最大的节点也不会向前移动，因此最终所有节点都
 * 以不同的配置epoch结束。
 * */
void clusterHandleConfigEpochCollision(clusterNode *sender) {
    /* Prerequisites: nodes have the same configEpoch and are both masters. 
     *
     * 先决条件：节点具有相同的configEpoch，并且都是主节点。
     * */
    if (sender->configEpoch != myself->configEpoch ||
        !nodeIsMaster(sender) || !nodeIsMaster(myself)) return;
    /* Don't act if the colliding node has a smaller Node ID. 
     *
     * 如果碰撞节点的节点ID较小，则不要执行操作。
     * */
    if (memcmp(sender->name,myself->name,CLUSTER_NAMELEN) <= 0) return;
    /* Get the next ID available at the best of this node knowledge. 
     *
     * 根据此节点的最佳知识获取下一个可用ID。
     * */
    server.cluster->currentEpoch++;
    myself->configEpoch = server.cluster->currentEpoch;
    clusterSaveConfigOrDie(1);
    serverLog(LL_VERBOSE,
        "WARNING: configEpoch collision with node %.40s."
        " configEpoch set to %llu",
        sender->name,
        (unsigned long long) myself->configEpoch);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not readded before some time elapsed (this time is specified
 * in seconds in CLUSTER_BLACKLIST_TTL).
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * Currently the CLUSTER_BLACKLIST_TTL is set to 1 minute, this means
 * that redis-trib has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 * 
 * CLUSTER节点黑名单
 *
 * 节点黑名单只是确保具有给定节点ID的给定节点在经过一段时间之前
 * 不会被读取的一种方式（该时间在CLUSTER_blacklist_TTL中以秒为单位指定）。
 *
 * 当我们想从集群中完全删除一个节点时，这很有用：当调用cluster FORGET时，
 * 它还会将该节点放入黑名单，这样，即使我们收到来自其他节点的gossip消
 * 息，这些消息仍然记得我们要删除的节点，我们也不会在一段时间前重新添加它。
 *
 * 目前，CLUSTER_BLACKLIST_TTL设置为1分钟，这意味着redis-trib有
 * 60秒的时间向集群中的节点发送CLUSTER FORGET消息，而不需要处理
 * 其他节点将节点重新添加回我们已经发送了FORGET命令的节点的问题，以及可以将节
 * 点重新添加为值的时间
 * */

#define CLUSTER_BLACKLIST_TTL 60      /* 1 minute. 
                                       *
                                       * 1分钟。
                                       * */


/* Before of the addNode() or Exists() operations we always remove expired
 * entries from the black list. This is an O(N) operation but it is not a
 * problem since add / exists operations are called very infrequently and
 * the hash table is supposed to contain very little elements at max.
 * However without the cleanup during long uptime and with some automated
 * node add/removal procedures, entries could accumulate. 
 *
 * 在addNode（）或Exists（）操作之前，我们总是从黑名单中删除过期的条目。
 * 这是一个O（N）操作，但这不是问题，因为添加/存在操作很少被调用，哈希表最多只包含很少的元素。
 * 但是，如果在长时间运行期间不进行清理，并且使用一些自动的节点添加/删除过程，条目可能会累积。
 * */
void clusterBlacklistCleanup(void) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes_black_list);
    while((de = dictNext(di)) != NULL) {
        int64_t expire = dictGetUnsignedIntegerVal(de);

        if (expire < server.unixtime)
            dictDelete(server.cluster->nodes_black_list,dictGetKey(de));
    }
    dictReleaseIterator(di);
}

/* Cleanup the blacklist and add a new node ID to the black list. 
 *
 * 清理黑名单，并在黑名单中添加一个新的节点ID。
 * */
void clusterBlacklistAddNode(clusterNode *node) {
    dictEntry *de;
    sds id = sdsnewlen(node->name,CLUSTER_NAMELEN);

    clusterBlacklistCleanup();
    if (dictAdd(server.cluster->nodes_black_list,id,NULL) == DICT_OK) {
        /* If the key was added, duplicate the sds string representation of
         * the key for the next lookup. We'll free it at the end. 
         *
         * 如果添加了键，请为下一次查找复制键的sds字符串表示。我们会在最后释放它。
         * */
        id = sdsdup(id);
    }
    de = dictFind(server.cluster->nodes_black_list,id);
    dictSetUnsignedIntegerVal(de,time(NULL)+CLUSTER_BLACKLIST_TTL);
    sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. 
 *
 * 如果黑名单中存在指定的节点ID，则返回非零。您不需要在这里传递sds字符串，任何
 * 指向40字节的指针都可以工作。
 * */
int clusterBlacklistExists(char *nodeid) {
    sds id = sdsnewlen(nodeid,CLUSTER_NAMELEN);
    int retval;

    clusterBlacklistCleanup();
    retval = dictFind(server.cluster->nodes_black_list,id) != NULL;
    sdsfree(id);
    return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * 集群消息交换-PING/PONG和 gossip
 * ----------------------------------------------------------------------------- 
 * */

/* This function checks if a given node should be marked as FAIL.
 * It happens if the following conditions are met:
 *
 * 1) We received enough failure reports from other master nodes via gossip.
 *    Enough means that the majority of the masters signaled the node is
 *    down recently.
 * 2) We believe this node is in PFAIL state.
 *
 * If a failure is detected we also inform the whole cluster about this
 * event trying to force every other node to set the FAIL flag for the node.
 *
 * Note that the form of agreement used here is weak, as we collect the majority
 * of masters state during some time, and even if we force agreement by
 * propagating the FAIL message, because of partitions we may not reach every
 * node. However:
 *
 * 1) Either we reach the majority and eventually the FAIL state will propagate
 *    to all the cluster.
 * 2) Or there is no majority so no slave promotion will be authorized and the
 *    FAIL flag will be cleared after some time.
 
 *
 * 此功能检查给定节点是否应标记为FAIL。如果满足以下条件，就会发生这种情况：
 * 
 * 1）我们通过gossip从其他主节点收到了足够多的故障报告。足够意味着大多数主节点最近都发出了
 *    节点关闭的信号。
 * 
 * 2）我们认为此节点处于PFAIL状态。如果检测到故障，我们还会通知整个集群该事件，
 * 试图强制其他每个节点为该节点设置FAIL标志。请注意，这里使用的协议形式很弱，
 * 因为我们在一段时间内收集了大多数主状态，即使我们通过传播FAIL消息来强制达成协议，
 * 由于分区的原因，我们可能无法到达每个节点。然而：
 * 
 * 1）要么我们达到大多数，最终FAIL状态将传播到所有集群。
 * 
 * 2） 或者没有多数，因此不会授权从属升级，一段时间后FAIL标志将被清除。
 * */
void markNodeAsFailingIfNeeded(clusterNode *node) {
    int failures;
    int needed_quorum = (server.cluster->size / 2) + 1;

    if (!nodeTimedOut(node)) return; /* We can reach it. 
                                      *
                                      * 我们可以到达。
                                      * */
    if (nodeFailed(node)) return; /* Already FAILing. 
                                   *
                                   * 已经失败。
                                   * */

    failures = clusterNodeFailureReportsCount(node);
    /* Also count myself as a voter if I'm a master. 
     *
     * 如果我是一个主节点，也要把自己算作一个选民。
     * */
    if (nodeIsMaster(myself)) failures++;
    if (failures < needed_quorum) return; /* No weak agreement from masters. 
                                           *
                                           * 主节点们没有软弱的协议。
                                           * */

    serverLog(LL_NOTICE,
        "Marking node %.40s as failing (quorum reached).", node->name);

    /* Mark the node as failing. 
     *
     * 将节点标记为失败。
     * */
    node->flags &= ~CLUSTER_NODE_PFAIL;
    node->flags |= CLUSTER_NODE_FAIL;
    node->fail_time = mstime();

    /* Broadcast the failing node name to everybody, forcing all the other
     * reachable nodes to flag the node as FAIL.
     * We do that even if this node is a replica and not a master: anyway
     * the failing state is triggered collecting failure reports from masters,
     * so here the replica is only helping propagating this status. 
     *
     * 向所有人广播故障节点名称，强制所有其他可到达的节点将该节点标记为FAIL。即使这
     * 个节点是副本而不是主节点，我们也会这样做：无论如何，故障状态都是从主节点收集故障
     * 报告时触发的，所以在这里，副本只是帮助传播这种状态。
     * */
    clusterSendFail(node->name);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. 
 *
 * 只有当节点被标记为FAIL时，才会调用此函数，但我们可以再次访问它。
 * 它检查是否存在撤消FAIL状态的条件。
 *
 * 收到了这个节点的pong消息才会调用这个方法
 * */
void clearNodeFailureIfNeeded(clusterNode *node) {
    mstime_t now = mstime();

    serverAssert(nodeFailed(node));

    /* For slaves we always clear the FAIL flag if we can contact the
     * node again. 
     *
     * 对于从节点，如果我们可以再次联系节点，我们总是清除FAIL标志。
     * */
    if (nodeIsSlave(node) || node->numslots == 0) {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: %s is reachable again.",
                node->name,
                nodeIsSlave(node) ? "replica" : "master without slots");
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }

    /* If it is a master and...
     * 1) The FAIL state is old enough.
     * 2) It is yet serving slots from our point of view (not failed over).
     * Apparently no one is going to fix these slots, clear the FAIL flag. 
     *
     * 如果它是一个主节点和。。。
     * 1） FAIL状态已足够旧。
     * 2） 从我们的角度来看，它仍然在提供槽位（没有失败转移）。显然没有人会修复这些插槽，清除FAIL标志。
     * */
    if (nodeIsMaster(node) && node->numslots > 0 &&
        (now - node->fail_time) >
        (server.cluster_node_timeout * CLUSTER_FAIL_UNDO_TIME_MULT))
    {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: is reachable again and nobody is serving its slots after some time.",
                node->name);
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }
}

/* Return true if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. 
 *
 * 如果已经有一个处于HANDSHAKE状态的节点与指定的ip地址和端口号匹配，则返回true。
 * 使用此函数是为了避免多次为同一地址添加新的握手节点。
 * */
int clusterHandshakeInProgress(char *ip, int port, int cport) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!nodeInHandshake(node)) continue;
        if (!strcasecmp(node->ip,ip) &&
            node->port == port &&
            node->cport == cport) break;
    }
    dictReleaseIterator(di);
    return de != NULL;
}

/* Start a handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * EAGAIN - There is already a handshake in progress for this address.
 * EINVAL - IP or port are not valid. 
 *
 * 如果没有正在进行的握手，请使用指定的地址开始握手。
 * 如果握手实际上已启动，则返回非零。
 *
 * 返回On error zero，并将errno设置为以下值之一：
 * EAGAIN-此地址已在进行握手。
 * EINVAL-IP或端口无效。
 * */
int clusterStartHandshake(char *ip, int port, int cport) {
    clusterNode *n;
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP sanity check 
     *
     * IP健全性检查
     * */
    if (inet_pton(AF_INET,ip,
            &(((struct sockaddr_in *)&sa)->sin_addr)))
    {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6,ip,
            &(((struct sockaddr_in6 *)&sa)->sin6_addr)))
    {
        sa.ss_family = AF_INET6;
    } else {
        errno = EINVAL;
        return 0;
    }

    /* Port sanity check 
     *
     * 端口健全性检查
     * */
    if (port <= 0 || port > 65535 || cport <= 0 || cport > 65535) {
        errno = EINVAL;
        return 0;
    }

    /* Set norm_ip as the normalized string representation of the node
     * IP address. 
     *
     * 将norm_ip设置为节点ip地址的标准化字符串表示形式。
     * */
    memset(norm_ip,0,NET_IP_STR_LEN);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET,
            (void*)&(((struct sockaddr_in *)&sa)->sin_addr),
            norm_ip,NET_IP_STR_LEN);
    else
        inet_ntop(AF_INET6,
            (void*)&(((struct sockaddr_in6 *)&sa)->sin6_addr),
            norm_ip,NET_IP_STR_LEN);

    if (clusterHandshakeInProgress(norm_ip,port,cport)) {
        errno = EAGAIN;
        return 0;
    }

    /* Add the node with a random address (NULL as first argument to
     * createClusterNode()). Everything will be fixed during the
     * handshake. 
     *
     * 添加具有随机地址的节点（NULL作为createClusterNode（）的第一个参数）。在握手过程中，一切都会得到解决。
     * */
    n = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_MEET);
    memcpy(n->ip,norm_ip,sizeof(n->ip));
    n->port = port;
    n->cport = cport;
    clusterAddNode(n);
    return 1;
}

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. 
 *
 * 处理PING或PONG数据包的gossip部分。
 * 请注意，此函数假设调用方已经检查了数据包的健全性，而不是gossip部分的内容，而是长度。
 * */
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
    uint16_t count = ntohs(hdr->count);
    clusterMsgDataGossip *g = (clusterMsgDataGossip*) hdr->data.ping.gossip;
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender);

    while(count--) {
        uint16_t flags = ntohs(g->flags);
        clusterNode *node;
        sds ci;

        if (server.verbosity == LL_DEBUG) {
            ci = representClusterNodeFlags(sdsempty(), flags);
            serverLog(LL_DEBUG,"GOSSIP %.40s %s:%d@%d %s",
                g->nodename,
                g->ip,
                ntohs(g->port),
                ntohs(g->cport),
                ci);
            sdsfree(ci);
        }

        /* Update our state accordingly to the gossip sections 
         *
         * 根据gossip部分更新我们的状态
         * */
        node = clusterLookupNode(g->nodename);
        if (node) {
            /* We already know this node.
               Handle failure reports, only when the sender is a master. 
             *
             * 我们已经知道这个节点。只有当发送方是主节点时，才能处理故障报告。
             * */
            if (sender && nodeIsMaster(sender) && node != myself) {
                if (flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) {
                    if (clusterNodeAddFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s as not reachable.",
                            sender->name, node->name);
                    }
                    markNodeAsFailingIfNeeded(node);
                } else {
                    if (clusterNodeDelFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s is back online.",
                            sender->name, node->name);
                    }
                }
            }

            /* If from our POV the node is up (no failure flags are set),
             * we have no pending ping for the node, nor we have failure
             * reports for this node, update the last pong time with the
             * one we see from the other nodes. 
             *
             * 如果从我们的POV来看，节点已启动（未设置故障标志），则我们没有该节点的挂起ping，
             * 也没有该节点上的故障报告，请使用我们从其他节点看到的时间更新最后一次PONG时间。
             * */
            if (!(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                node->ping_sent == 0 &&
                clusterNodeFailureReportsCount(node) == 0)
            {
                mstime_t pongtime = ntohl(g->pong_received);
                pongtime *= 1000; /* Convert back to milliseconds. 
                                   *
                                   * 转换回毫秒。
                                   * */

                /* Replace the pong time with the received one only if
                 * it's greater than our view but is not in the future
                 * (with 500 milliseconds tolerance) from the POV of our
                 * clock. 
                 *
                 * 只有当pong时间大于我们的视图，但不在未来（具有500毫秒的容差）时，才用接收
                 * 到的时间替换pong时间。
                 * */
                if (pongtime <= (server.mstime+500) &&
                    pongtime > node->pong_received)
                {
                    node->pong_received = pongtime;
                }
            }

            /* If we already know this node, but it is not reachable, and
             * we see a different address in the gossip section of a node that
             * can talk with this other node, update the address, disconnect
             * the old link if any, so that we'll attempt to connect with the
             * new address. 
             *
             * 如果我们已经知道这个节点，但它不可访问，并且我们在节点的gossip部分看到一个不同的地
             * 址，该地址可以与另一个节点通信，更新地址，断开旧链接（如果有的话），这样我们就可
             * 以尝试连接新地址。
             * */
            if (node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL) &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                (strcasecmp(node->ip,g->ip) ||
                 node->port != ntohs(g->port) ||
                 node->cport != ntohs(g->cport)))
            {
                if (node->link) freeClusterLink(node->link);
                memcpy(node->ip,g->ip,NET_IP_STR_LEN);
                node->port = ntohs(g->port);
                node->cport = ntohs(g->cport);
                node->flags &= ~CLUSTER_NODE_NOADDR;
            }
        } else {
            /* If it's not in NOADDR state and we don't have it, we
             * add it to our trusted dict with exact nodeid and flag.
             * Note that we cannot simply start a handshake against
             * this IP/PORT pairs, since IP/PORT can be reused already,
             * otherwise we risk joining another cluster.
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster. 
             *
             * 如果它不处于NOADDR状态，并且我们没有它，我们会将它添加到具有确切nodeid和标志的可信dict中。
             * 请注意，我们不能简单地针对这个IP/PORT对启动握手，因为IP/PORT已经可以重用了，
             * 否则我们就有加入另一个集群的风险。
             *
             * 请注意，我们要求这个gossip消息的发送者是我们集群中的一个众所周知的节点，
             * 否则我们就有加入另一个集群的风险。
             * */
            if (sender &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !clusterBlacklistExists(g->nodename))
            {
                clusterNode *node;
                node = createClusterNode(g->nodename, flags);
                memcpy(node->ip,g->ip,NET_IP_STR_LEN);
                node->port = ntohs(g->port);
                node->cport = ntohs(g->cport);
                clusterAddNode(node);
            }
        }

        /* Next node 
         *
         * 下一个节点
         * */
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes.
 * If 'announced_ip' length is non-zero, it is used instead of extracting
 * the IP from the socket peer address. 
 *
 * IP->字符串转换。'buf’应该至少是46个字节。如果“announced_ip”长度为非零，则使用该长度，而不是从套接字对等地址提取ip。
 * */
void nodeIp2String(char *buf, clusterLink *link, char *announced_ip) {
    if (announced_ip[0] != '\0') {
        memcpy(buf,announced_ip,NET_IP_STR_LEN);
        buf[NET_IP_STR_LEN-1] = '\0'; /* We are not sure the input is sane. 
                                       *
                                       * 我们不确定输入是否正常。
                                       * */
    } else {
        connPeerToString(link->conn, buf, NET_IP_STR_LEN, NULL);
    }
}

/* Update the node address to the IP address that can be extracted
 * from link->fd, or if hdr->myip is non empty, to the address the node
 * is announcing us. The port is taken from the packet header as well.
 *
 * If the address or port changed, disconnect the node link so that we'll
 * connect again to the new address.
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. 
 *
 * 将节点地址更新为可以从link->fd中提取的IP地址，或者如果hdr->myip不为空，则更新为节点通知我们的地址。
 * 端口也取自数据包标头。
 *
 * 如果地址或端口发生更改，请断开节点链接，以便我们再次连接到新地址。
 *
 * 如果ip/端口对已经正确，则根本不执行任何操作。
 *
 * 如果节点地址仍然相同，则函数返回0，否则返回1。
 * */
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link,
                              clusterMsg *hdr)
{
    char ip[NET_IP_STR_LEN] = {0};
    int port = ntohs(hdr->port);
    int cport = ntohs(hdr->cport);

    /* We don't proceed if the link is the same as the sender link, as this
     * function is designed to see if the node link is consistent with the
     * symmetric link that is used to receive PINGs from the node.
     *
     * As a side effect this function never frees the passed 'link', so
     * it is safe to call during packet processing. 
     *
     * 如果链接与发送方链接相同，我们不会继续，
     * 因为此函数旨在查看节点链接是否与用于从节点接收PING的对称链接一致。
     *
     * 作为副作用，此函数永远不会释放传递的“链接”，因此在数据包处理过程中调用是安全的。
     * */
    if (link == node->link) return 0;

    nodeIp2String(ip,link,hdr->myip);
    if (node->port == port && node->cport == cport &&
        strcmp(ip,node->ip) == 0) return 0;

    /* IP / port is different, update it. 
     *
     * IP/端口不同，请更新它。
     * */
    memcpy(node->ip,ip,sizeof(ip));
    node->port = port;
    node->cport = cport;
    if (node->link) freeClusterLink(node->link);
    node->flags &= ~CLUSTER_NODE_NOADDR;
    serverLog(LL_WARNING,"Address updated for node %.40s, now %s:%d",
        node->name, node->ip, node->port);

    /* Check if this is our master and we have to change the
     * replication target as well. 
     *
     * 检查这是否是我们的主节点，我们还必须更改复制目标。
     * */
    if (nodeIsSlave(myself) && myself->slaveof == node)
        replicationSetMaster(node->ip, node->port);
    return 1;
}

/* Reconfigure the specified node 'n' as a master. This function is called when
 * a node that we believed to be a slave is now acting as master in order to
 * update the state of the node. 
 *
 * 将指定的节点“n”重新配置为主节点。当我们认为是从节点的节点现在充当主节点以更新
 * 节点的状态时，就会调用此函数。
 * */
void clusterSetNodeAsMaster(clusterNode *n) {
    if (nodeIsMaster(n)) return;

    if (n->slaveof) {
        clusterNodeRemoveSlave(n->slaveof,n);
        if (n != myself) n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    n->flags &= ~CLUSTER_NODE_SLAVE;
    n->flags |= CLUSTER_NODE_MASTER;
    n->slaveof = NULL;

    /* Update config and state. 
     *
     * 更新配置和状态。
     * */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a master configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actually the "Sender" of the information, like in the
 * case we receive the info via an UPDATE packet. 
 *
 * 当我们通过PING、PONG或UPDATE数据包接收到主配置时，会调用此函数。
 * 我们收到的是一个节点，该节点的configEpoch，以及在该configEpoch下声明的插槽集。
 *
 *
 * 我们所做的是用与本地配置相比更新的配置重新绑定插槽，
 * 如果需要，我们将自己变成节点的副本（更多信息请参阅功能注释）。
 *
 * 'sender' 是我们收到配置更新的节点。有时它实际上不是信息的“发送者”，
 * 比如我们通过UPDATE数据包接收信息的情况。
 * */
void clusterUpdateSlotsConfigWith(clusterNode *sender, uint64_t senderConfigEpoch, unsigned char *slots) {
    int j;
    clusterNode *curmaster, *newmaster = NULL;
    /* The dirty slots list is a list of slots for which we lose the ownership
     * while having still keys inside. This usually happens after a failover
     * or after a manual cluster reconfiguration operated by the admin.
     *
     * If the update message is not able to demote a master to slave (in this
     * case we'll resync with the master updating the whole key space), we
     * need to delete all the keys in the slots we lost ownership. 
     *
     * 脏插槽列表是一个插槽列表，我们失去了这些插槽的所有权，但里面仍然有键。
     * 这种情况通常发生在故障切换之后或管理员手动重新配置集群之后。
     *
     * 如果更新消息无法将master降级为slave（在这种情况下，我们将与master重新同步，更新整个键空间），
     * 我们需要删除我们失去所有权的插槽中的所有键。
     * */
    uint16_t dirty_slots[CLUSTER_SLOTS];
    int dirty_slots_count = 0;

    /* Here we set curmaster to this node or the node this node
     * replicates to if it's a slave. In the for loop we are
     * interested to check if slots are taken away from curmaster. 
     *
     * 在这里，我们将curmaster设置为该节点或该节点复制到的节点（如果它是从节点）。
     * 在for循环中，我们有兴趣检查是否从curmaster中删除了插槽。
     * */
    curmaster = nodeIsMaster(myself) ? myself : myself->slaveof;

    if (sender == myself) {
        serverLog(LL_WARNING,"Discarding UPDATE message about myself.");
        return;
    }

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(slots,j)) {
            /* The slot is already bound to the sender of this message. 
             *
             * 插槽已绑定到此邮件的发件人。
             * */
            if (server.cluster->slots[j] == sender) continue;

            /* The slot is in importing state, it should be modified only
             * manually via redis-trib (example: a resharding is in progress
             * and the migrating side slot was already closed and is advertising
             * a new config. We still want the slot to be closed manually). 
             *
             * 插槽处于导入状态，只能通过redis-trib手动修改（例如：正在进行重新处理，
             * 迁移侧插槽已经关闭，正在发布新配置。我们仍然希望手动关闭插槽）。
             * */
            if (server.cluster->importing_slots_from[j]) continue;

            /* We rebind the slot to the new node claiming it if:
             * 1) The slot was unassigned or the new node claims it with a
             *    greater configEpoch.
             * 2) We are not currently importing the slot. 
             *
             * 如果出现以下情况，我们将插槽重新绑定到声明该插槽的新节点：
             * 1）插槽未分配或新节点使用更大的configEpoch声明该插槽。
             * 2） 我们目前没有导入插槽。
             * */
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->configEpoch < senderConfigEpoch)
            {
                /* Was this slot mine, and still contains keys? Mark it as
                 * a dirty slot. 
                 *
                 * 这个槽是我的吗，里面还有键？将其标记为脏槽。
                 * */
                if (server.cluster->slots[j] == myself &&
                    countKeysInSlot(j) &&
                    sender != myself)
                {
                    dirty_slots[dirty_slots_count] = j;
                    dirty_slots_count++;
                }

                if (server.cluster->slots[j] == curmaster)
                    newmaster = sender;
                clusterDelSlot(j);
                clusterAddSlot(sender,j);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE|
                                     CLUSTER_TODO_FSYNC_CONFIG);
            }
        }
    }

    /* After updating the slots configuration, don't do any actual change
     * in the state of the server if a module disabled Redis Cluster
     * keys redirections. 
     *
     * 更新插槽配置后，如果某个模块禁用了Redis Cluster键重定向，则不要对服务器的状态进行任何实际更改。
     * */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return;

    /* If at least one slot was reassigned from a node to another node
     * with a greater configEpoch, it is possible that:
     * 1) We are a master left without slots. This means that we were
     *    failed over and we should turn into a replica of the new
     *    master.
     * 2) We are a slave and our master is left without slots. We need
     *    to replicate to the new slots owner. 
     *
     * 如果至少有一个插槽从一个节点重新分配到另一个具有更大configEpoch的节点，则可能：
     *
     * 1 ）我们是一个没有插槽的主节点。这意味着我们被失败了，我们应该变成新主节点的复制品。
     *
     * 2） 我们是从节点，我们的主节点没有空位。我们需要复制到新的插槽所有者。
     * */
    if (newmaster && curmaster->numslots == 0) {
        serverLog(LL_WARNING,
            "Configuration change detected. Reconfiguring myself "
            "as a replica of %.40s", sender->name);
        clusterSetMaster(sender);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
    } else if (dirty_slots_count) {
        /* If we are here, we received an update message which removed
         * ownership for certain slots we still have keys about, but still
         * we are serving some slots, so this master node was not demoted to
         * a slave.
         *
         * In order to maintain a consistent state between keys and slots
         * we need to remove all the keys from the slots we lost. 
         *
         * 如果我们在这里，我们收到了一条更新消息，
         * 该消息删除了我们仍有键的某些插槽的所有权，
         * 但我们仍在为一些插槽提供服务，因此该主节点没有降级为从节点。
         *
         * 为了保持键和插槽之间的一致状态，我们需要从丢失的插槽中删除所有键。
         * */
        for (j = 0; j < dirty_slots_count; j++)
            delKeysInSlot(dirty_slots[j]);
    }
}

/* When this function is called, there is a packet to process starting
 * at node->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). 
 *
 * 当调用此函数时，将从node->rcvbuf开始处理一个数据包。
 * 释放缓冲区取决于调用方，因此该函数应该只处理处理数据包的更高级别的事务，
 * 并在需要时修改集群状态。
 *
 * 如果数据包处理后链路仍然有效，则函数返回1，
 * 否则，如果由于数据包处理导致某些不一致错误
 * （例如从错误的发送方ID接收到的PONG）而释放了链路，则返回0。
 * */
int clusterProcessPacket(clusterLink *link) {
    clusterMsg *hdr = (clusterMsg*) link->rcvbuf;
    uint32_t totlen = ntohl(hdr->totlen);
    uint16_t type = ntohs(hdr->type);
    mstime_t now = mstime();

    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->stats_bus_messages_received[type]++;
    serverLog(LL_DEBUG,"--- Processing packet of type %d, %lu bytes",
        type, (unsigned long) totlen);

    /* Perform sanity checks 
     *
     * 执行健全性检查
     * */
    if (totlen < 16) return 1; /* At least signature, version, totlen, count. 
                                *
                                * 至少签名、版本、总数、计数。
                                * */
    if (totlen > link->rcvbuf_len) return 1;

    if (ntohs(hdr->ver) != CLUSTER_PROTO_VER) {
        /* Can't handle messages of different versions. 
         *
         * 无法处理不同版本的消息。
         * */
        return 1;
    }

    uint16_t flags = ntohs(hdr->flags);
    uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;
    clusterNode *sender;

    // 根据消息的类型，判断长度是否合法 start
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        uint16_t count = ntohs(hdr->count);
        uint32_t explen; /* expected length of this packet 
                          *
                          * 此数据包的预期长度
                          * */

        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip)*count);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataFail);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataPublish) -
                8 +
                ntohl(hdr->data.publish.msg.channel_len) +
                ntohl(hdr->data.publish.msg.message_len);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
               type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
               type == CLUSTERMSG_TYPE_MFSTART)
    {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataUpdate);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgModule) -
                3 + ntohl(hdr->data.module.msg.len);
        if (totlen != explen) return 1;
    }
    // 根据消息的类型，判断长度是否合法 end


    /* Check if the sender is a known node. Note that for incoming connections
     * we don't store link->node information, but resolve the node by the
     * ID in the header each time in the current implementation. 
     *
     * 检查发件人是否为已知节点。请注意，对于传入连接，我们不存储link->node信息，
     * 而是在当前实现中每次通过标头中的ID解析节点。
     * */
    sender = clusterLookupNode(hdr->sender);

    /* Update the last time we saw any data from this node. We
     * use this in order to avoid detecting a timeout from a node that
     * is just sending a lot of data in the cluster bus, for instance
     * because of Pub/Sub. 
     *
     * 更新上次看到来自此节点的任何数据的时间。我们使用它是为了避免检测到一个节点的超时，
     * 该节点只是在集群总线中发送大量数据，例如因为Pub/Sub。
     * */
    if (sender) sender->data_received = now;

    if (sender && !nodeInHandshake(sender)) {
        /* Update our currentEpoch if we see a newer epoch in the cluster. 
         *
         * 如果我们在集群中看到新的epoch，请更新我们的currentEpoch。
         * */
        senderCurrentEpoch = ntohu64(hdr->currentEpoch);
        senderConfigEpoch = ntohu64(hdr->configEpoch);
        if (senderCurrentEpoch > server.cluster->currentEpoch)
            server.cluster->currentEpoch = senderCurrentEpoch;
        /* Update the sender configEpoch if it is publishing a newer one. 
         *
         * 如果发送方正在发布更新的configEpoch，请更新它。
         * */
        if (senderConfigEpoch > sender->configEpoch) {
            sender->configEpoch = senderConfigEpoch;
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_FSYNC_CONFIG);
        }
        /* Update the replication offset info for this node. 
         *
         * 更新此节点的复制偏移量信息。
         * */
        sender->repl_offset = ntohu64(hdr->offset);
        sender->repl_offset_time = now;
        /* If we are a slave performing a manual failover and our master
         * sent its offset while already paused, populate the MF state. 
         *
         * 如果我们是执行手动故障转移的从节点，并且我们的主节点在暂停时发送了偏移量，则填充MF状态。
         * */
        if (server.cluster->mf_end &&
            nodeIsSlave(myself) &&
            myself->slaveof == sender &&
            hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
            server.cluster->mf_master_offset == 0)
        {
            server.cluster->mf_master_offset = sender->repl_offset;
            serverLog(LL_WARNING,
                "Received replication offset for paused "
                "master manual failover: %lld",
                server.cluster->mf_master_offset);
        }
    }

    /* Initial processing of PING and MEET requests replying with a PONG. 
     *
     * PING和MEET请求的初始处理以PONG进行回复。
     * */
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        serverLog(LL_DEBUG,"Ping packet received: %p", (void*)link->node);

        /* We use incoming MEET messages in order to set the address
         * for 'myself', since only other cluster nodes will send us
         * MEET messages on handshakes, when the cluster joins, or
         * later if we changed address, and those nodes will use our
         * official address to connect to us. So by obtaining this address
         * from the socket is a simple way to discover / update our own
         * address in the cluster without it being hardcoded in the config.
         *
         * However if we don't have an address at all, we update the address
         * even with a normal PING packet. If it's wrong it will be fixed
         * by MEET later. 
         *
         * 我们使用传入的MEET消息来设置 myself 的地址，因为只有其他集群节点会在握手时
         * 向我们发送MEET消息，当集群加入时，或者稍后如果我们更改了地址，
         * 这些节点会使用我们的官方地址连接到我们。
         *
         * 因此，从套接字中获取这个地址是一种简单的方法，可以在集群中发现/更新我们自己的地址，
         * 而无需在配置中对其进行硬编码。
         *
         * 然而，如果我们根本没有地址，即使使用普通的PING数据包，我们也会更新地址。
         * 如果它是错误的，它将由MEET稍后修复。
         * */
        if ((type == CLUSTERMSG_TYPE_MEET || myself->ip[0] == '\0') &&
            server.cluster_announce_ip == NULL)
        {
            char ip[NET_IP_STR_LEN];

            if (connSockName(link->conn,ip,sizeof(ip),NULL) != -1 &&
                strcmp(ip,myself->ip))
            {
                memcpy(myself->ip,ip,NET_IP_STR_LEN);
                serverLog(LL_WARNING,"IP address for this node updated to %s",
                    myself->ip);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            }
        }

        /* Add this node if it is new for us and the msg type is MEET.
         * In this stage we don't try to add the node with the right
         * flags, slaveof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the node. 
         *
         * 如果这个节点对我们来说是新的，并且消息类型是MEET，请添加它。
         * 在这个阶段，我们不尝试添加具有正确标志、slave-of指针等的节点，
         * 因为当我们从节点接收PONG时，这些细节将得到解决。
         * */
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            clusterNode *node;

            node = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE);
            nodeIp2String(node->ip,link,hdr->myip);
            node->port = ntohs(hdr->port);
            node->cport = ntohs(hdr->cport);
            clusterAddNode(node);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }

        /* If this is a MEET packet from an unknown node, we still process
         * the gossip section here since we have to trust the sender because
         * of the message type. 
         *
         * 如果这是来自未知节点的MEET数据包，我们仍然在这里处理gossip部分，
         * 因为由于消息类型的原因，我们必须信任发送者。
         * */
        if (!sender && type == CLUSTERMSG_TYPE_MEET)
            clusterProcessGossipSection(hdr,link);

        /* Anyway reply with a PONG 
         *
         * 无论如何，用PONG回复
         * */
        clusterSendPing(link,CLUSTERMSG_TYPE_PONG);
    }

    /* PING, PONG, MEET: process config information. 
     *
     * PING、PONG、MEET：进程配置信息。
     * */
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        serverLog(LL_DEBUG,"%s packet received: %p",
            type == CLUSTERMSG_TYPE_PING ? "ping" : "pong",
            (void*)link->node);
        if (link->node) {
            if (nodeInHandshake(link->node)) {
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. 
                 *
                 * 如果我们已经有了这个节点，请尝试用新的节点更改节点的IP/端口。
                 * */
                if (sender) {
                    serverLog(LL_VERBOSE,
                        "Handshake: we already know node %.40s, "
                        "updating the address if needed.", sender->name);
                    if (nodeUpdateAddressIfNeeded(sender,link,hdr))
                    {
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                             CLUSTER_TODO_UPDATE_STATE);
                    }
                    /* Free this node as we already have it. This will
                     * cause the link to be freed as well. 
                     *
                     * 释放这个节点，因为我们已经有了它。这将导致链接也被释放。
                     * */
                    clusterDelNode(link->node);
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was a handshake stage. 
                 *
                 * 如果这是握手阶段，首先要做的是用正确的节点名称替换随机名称。
                 * */
                clusterRenameNode(link->node, hdr->sender);
                serverLog(LL_DEBUG,"Handshake with node %.40s completed.",
                    link->node->name);
                link->node->flags &= ~CLUSTER_NODE_HANDSHAKE;
                link->node->flags |= flags&(CLUSTER_NODE_MASTER|CLUSTER_NODE_SLAVE);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            } else if (memcmp(link->node->name,hdr->sender,
                        CLUSTER_NAMELEN) != 0)
            {
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. 
                 *
                 * 如果回复具有不匹配的节点ID，我们断开该节点的连接，并将其设置为没有关联地址。
                 * */
                serverLog(LL_DEBUG,"PONG contains mismatching sender ID. About node %.40s added %d ms ago, having flags %d",
                    link->node->name,
                    (int)(now-(link->node->ctime)),
                    link->node->flags);
                link->node->flags |= CLUSTER_NODE_NOADDR;
                link->node->ip[0] = '\0';
                link->node->port = 0;
                link->node->cport = 0;
                freeClusterLink(link);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                return 0;
            }
        }

        /* Copy the CLUSTER_NODE_NOFAILOVER flag from what the sender
         * announced. This is a dynamic flag that we receive from the
         * sender, and the latest status must be trusted. We need it to
         * be propagated because the slave ranking used to understand the
         * delay of each slave in the voting process, needs to know
         * what are the instances really competing. 
         *
         * 复制发件人声明的 CLUSTER_NODE_NOFAILOVER 标志。
         * 这是我们从发件人处收到的动态标志，并且最新状态必须是可信的。
         * 我们需要传播它，因为用于了解每个从节点在投票过程中的延迟的从节点排名需要知道真正竞争的节点是什么。
         * */
        if (sender) {
            int nofailover = flags & CLUSTER_NODE_NOFAILOVER;
            sender->flags &= ~CLUSTER_NODE_NOFAILOVER;
            sender->flags |= nofailover;
        }

        /* Update the node address if it changed. 
         *
         * 如果节点地址发生更改，请更新该地址。
         * */
        if (sender && type == CLUSTERMSG_TYPE_PING &&
            !nodeInHandshake(sender) &&
            nodeUpdateAddressIfNeeded(sender,link,hdr))
        {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_UPDATE_STATE);
        }

        /* Update our info about the node 
         *
         * 更新我们关于节点的信息
         * */
        if (link->node && type == CLUSTERMSG_TYPE_PONG) {
            link->node->pong_received = now;
            link->node->ping_sent = 0;

            /* The PFAIL condition can be reversed without external
             * help if it is momentary (that is, if it does not
             * turn into a FAIL state).
             *
             * The FAIL condition is also reversible under specific
             * conditions detected by clearNodeFailureIfNeeded(). 
             *
             * 如果PFAIL状态是瞬时的（即，如果它没有变为FAIL状态），则可以在没有外部帮
             * 助的情况下逆转。FAIL条件在clearNodeFailureIfNeedd（）
             * 检测到的特定条件下也是可逆的。
             * */
            if (nodeTimedOut(link->node)) {
                link->node->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            } else if (nodeFailed(link->node)) {
                clearNodeFailureIfNeeded(link->node);
            }
        }

        /* Check for role switch: slave -> master or master -> slave. 
         *
         * 检查角色切换：从->主或主->从。
         * */
        if (sender) {
            if (!memcmp(hdr->slaveof,CLUSTER_NODE_NULL_NAME,
                sizeof(hdr->slaveof)))
            {
                /* Node is a master. 
                 *
                 * 节点是主节点。
                 * */
                clusterSetNodeAsMaster(sender);
            } else {
                /* Node is a slave. 
                 *
                 * 节点是从节点。
                 * */
                clusterNode *master = clusterLookupNode(hdr->slaveof);

                if (nodeIsMaster(sender)) {
                    /* Master turned into a slave! Reconfigure the node. 
                     *
                     * 主节点变成了从节点！重新配置节点。
                     * */
                    clusterDelNodeSlots(sender);
                    sender->flags &= ~(CLUSTER_NODE_MASTER|
                                       CLUSTER_NODE_MIGRATE_TO);
                    sender->flags |= CLUSTER_NODE_SLAVE;

                    /* Update config and state. 
                     *
                     * 更新配置和状态。
                     * */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                         CLUSTER_TODO_UPDATE_STATE);
                }

                /* Master node changed for this slave? 
                 *
                 * 此从节点的主节点已更改？
                 * */
                if (master && sender->slaveof != master) {
                    if (sender->slaveof)
                        clusterNodeRemoveSlave(sender->slaveof,sender);
                    clusterNodeAddSlave(master,sender);
                    sender->slaveof = master;

                    /* Update config. 
                     *
                     * 更新配置。
                     * */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                }
            }
        }

        /* Update our info about served slots.
         *
         * Note: this MUST happen after we update the master/slave state
         * so that CLUSTER_NODE_MASTER flag will be set. 
         *
         * 更新我们服务的槽位信息。注意：这必须在我们更新主/从状态之后发生，以便设置 CLUSTER_NODE_MASTER 标志。
         * */

        /* Many checks are only needed if the set of served slots this
         * instance claims is different compared to the set of slots we have
         * for it. Check this ASAP to avoid other computational expansive
         * checks later. 
         *
         * 只有当此节点声明的服务插槽集与我们为其提供的插槽集不同时，才需要进行许多检查。请
         * 尽快检查，以避免以后进行其他计算扩展检查。
         * */
        clusterNode *sender_master = NULL; /* Sender or its master if slave. 
                                            *
                                            * 发送方或其主节点方（如果为从控方）。
                                            * */
        int dirty_slots = 0; /* Sender claimed slots don't match my view? 
                              *
                              * 发件人声称的插槽与我的视图不匹配？
                              * */

        if (sender) {
            sender_master = nodeIsMaster(sender) ? sender : sender->slaveof;
            if (sender_master) {
                dirty_slots = memcmp(sender_master->slots,
                        hdr->myslots,sizeof(hdr->myslots)) != 0;
            }
        }

        /* 1) If the sender of the message is a master, and we detected that
         *    the set of slots it claims changed, scan the slots to see if we
         *    need to update our configuration. 
         *
         * 1） 如果消息的发送方是主消息，并且我们检测到它声称的插槽集发生了更改，请扫描插槽，看看是否需要更新配置。
         * */
        if (sender && nodeIsMaster(sender) && dirty_slots)
            clusterUpdateSlotsConfigWith(sender,senderConfigEpoch,hdr->myslots);

        /* 2) We also check for the reverse condition, that is, the sender
         *    claims to serve slots we know are served by a master with a
         *    greater configEpoch. If this happens we inform the sender.
         *
         * This is useful because sometimes after a partition heals, a
         * reappearing master may be the last one to claim a given set of
         * hash slots, but with a configuration that other instances know to
         * be deprecated. Example:
         *
         * A and B are master and slave for slots 1,2,3.
         * A is partitioned away, B gets promoted.
         * B is partitioned away, and A returns available.
         *
         * Usually B would PING A publishing its set of served slots and its
         * configEpoch, but because of the partition B can't inform A of the
         * new configuration, so other nodes that have an updated table must
         * do it. In this way A will stop to act as a master (or can try to
         * failover if there are the conditions to win the election). 
         *
         * 2）
         * 我们还检查了相反的条件，
         * 即发送方声称为我们知道由具有更大configEpoch的主节点提供服务的插槽提供服务。
         * 如果发生这种情况，我们会通知发件人。
         * 这很有用，因为有时在分区修复后，重新出现的主节点可能是最后一个声明给定哈希槽集的主节点，
         * 但其他节点知道其配置不推荐使用。
         *
         * 示例：
         * 对于插槽1、2、3，A和B是主插槽和从插槽。
         * A被瓜分了，B升职了。
         * B被分区，A返回可用。
         *
         * 通常，B会PING A发布其服务插槽集和configEpoch，
         * 但由于分区的原因，B无法通知A新的配置，因此具有更新表的其他节点必须这样做。
         * 这样，A将停止充当主节点（或者，如果有条件赢得选举，可以尝试故障转移）。
         * */
        if (sender && dirty_slots) {
            int j;

            for (j = 0; j < CLUSTER_SLOTS; j++) {
                if (bitmapTestBit(hdr->myslots,j)) {
                    if (server.cluster->slots[j] == sender ||
                        server.cluster->slots[j] == NULL) continue;
                    if (server.cluster->slots[j]->configEpoch >
                        senderConfigEpoch)
                    {
                        serverLog(LL_VERBOSE,
                            "Node %.40s has old slots configuration, sending "
                            "an UPDATE message about %.40s",
                                sender->name, server.cluster->slots[j]->name);
                        clusterSendUpdate(sender->link,
                            server.cluster->slots[j]);

                        /* TODO: instead of exiting the loop send every other
                         * UPDATE packet for other nodes that are the new owner
                         * of sender's slots. 
                         *
                         * TODO:不是退出循环，而是每隔一个UPDATE数据包发送一次给作为发送方插槽新
                         * 所有者的其他节点。
                         * */
                        break;
                    }
                }
            }
        }

        /* If our config epoch collides with the sender's try to fix
         * the problem. 
         *
         * 如果我们的配置epoch与发送方的冲突，请尝试解决问题。
         * */
        if (sender &&
            nodeIsMaster(myself) && nodeIsMaster(sender) &&
            senderConfigEpoch == myself->configEpoch)
        {
            clusterHandleConfigEpochCollision(sender);
        }

        /* Get info from the gossip section 
         *
         * 从gossip区获取信息
         * */
        if (sender) clusterProcessGossipSection(hdr,link);
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        clusterNode *failing;

        if (sender) {
            failing = clusterLookupNode(hdr->data.fail.about.nodename);
            if (failing &&
                !(failing->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_MYSELF)))
            {
                serverLog(LL_NOTICE,
                    "FAIL message received from %.40s about %.40s",
                    hdr->sender, hdr->data.fail.about.nodename);
                failing->flags |= CLUSTER_NODE_FAIL;
                failing->fail_time = now;
                failing->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            }
        } else {
            serverLog(LL_NOTICE,
                "Ignoring FAIL message from unknown node %.40s about %.40s",
                hdr->sender, hdr->data.fail.about.nodename);
        }
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        robj *channel, *message;
        uint32_t channel_len, message_len;

        /* Don't bother creating useless objects if there are no
         * Pub/Sub subscribers. 
         *
         * 如果没有Pub/Sub订阅者，就不要麻烦创建无用的对象。
         * */
        if (dictSize(server.pubsub_channels) ||
           listLength(server.pubsub_patterns))
        {
            channel_len = ntohl(hdr->data.publish.msg.channel_len);
            message_len = ntohl(hdr->data.publish.msg.message_len);
            channel = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data,channel_len);
            message = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data+channel_len,
                        message_len);
            pubsubPublishMessage(channel,message);
            decrRefCount(channel);
            decrRefCount(message);
        }
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
        if (!sender) return 1;  /* We don't know that node. 
                                 *
                                 * 我们不知道那个节点。
                                 * */
        clusterSendFailoverAuthIfNeeded(sender,hdr);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
        if (!sender) return 1;  /* We don't know that node. 
                                 *
                                 * 我们不知道那个节点。
                                 * */
        /* We consider this vote only if the sender is a master serving
         * a non zero number of slots, and its currentEpoch is greater or
         * equal to epoch where this node started the election. 
         *
         * 只有当发送方是服务于非零数量插槽的主节点，并且其当前epoch大于或等于该节点开始
         * 选举的epoch时，我们才考虑此投票。
         * */
        if (nodeIsMaster(sender) && sender->numslots > 0 &&
            senderCurrentEpoch >= server.cluster->failover_auth_epoch)
        {
            server.cluster->failover_auth_count++;
            /* Maybe we reached a quorum here, set a flag to make sure
             * we check ASAP. 
             *
             * 也许我们达到了法定人数，设置一个标志以确保我们尽快检查。
             * */
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
    } else if (type == CLUSTERMSG_TYPE_MFSTART) {
        /* This message is acceptable only if I'm a master and the sender
         * is one of my slaves. 
         *
         * 只有当我是主节点并且发件人是我的从节点时，这个消息才是可以接受的。
         * */
        if (!sender || sender->slaveof != myself) return 1;
        /* Manual failover requested from slaves. Initialize the state
         * accordingly. 
         *
         * 从节点请求手动故障转移。相应地初始化状态。
         * */
        resetManualFailover();
        server.cluster->mf_end = now + CLUSTER_MF_TIMEOUT;
        server.cluster->mf_slave = sender;
        pauseClients(now+(CLUSTER_MF_TIMEOUT*CLUSTER_MF_PAUSE_MULT));
        serverLog(LL_WARNING,"Manual failover requested by replica %.40s.",
            sender->name);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        clusterNode *n; /* The node the update is about. 
                         *
                         * 更新涉及的节点。
                         * */
        uint64_t reportedConfigEpoch =
                    ntohu64(hdr->data.update.nodecfg.configEpoch);

        if (!sender) return 1;  /* We don't know the sender. 
                                 *
                                 * 我们不知道发件人。
                                 * */
        n = clusterLookupNode(hdr->data.update.nodecfg.nodename);
        if (!n) return 1;   /* We don't know the reported node. 
                             *
                             * 我们不知道报告的节点。
                             * */
        if (n->configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. 
                                                              *
                                                              * 没有什么新鲜事。
                                                              * */

        /* If in our current config the node is a slave, set it as a master. 
         *
         * 如果在我们当前的配置中，节点是从节点，则将其设置为主节点。
         * */
        if (nodeIsSlave(n)) clusterSetNodeAsMaster(n);

        /* Update the node's configEpoch. 
         *
         * 更新节点的configEpoch。
         * */
        n->configEpoch = reportedConfigEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);

        /* Check the bitmap of served slots and update our
         * config accordingly. 
         *
         * 检查服务插槽的位图，并相应地更新我们的配置。
         * */
        clusterUpdateSlotsConfigWith(n,reportedConfigEpoch,
            hdr->data.update.nodecfg.slots);
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        if (!sender) return 1;  /* Protect the module from unknown nodes. 
                                 *
                                 * 保护模块不受未知节点的影响。
                                 * */
        /* We need to route this message back to the right module subscribed
         * for the right message type. 
         *
         * 我们需要将此消息路由回为正确的消息类型订阅的正确模块。
         * */
        uint64_t module_id = hdr->data.module.msg.module_id; /* Endian-safe ID 
                                                              *
                                                              * Endian安全ID
                                                              * */
        uint32_t len = ntohl(hdr->data.module.msg.len);
        uint8_t type = hdr->data.module.msg.type;
        unsigned char *payload = hdr->data.module.msg.bulk_data;
        moduleCallClusterReceivers(sender->name,module_id,type,payload,len);
    } else {
        serverLog(LL_WARNING,"Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. 
 *
 * 当我们检测到此节点的链接丢失时，会调用此函数。我们将节点设置为不再连接。群集Cron将检测到此连接，并尝试重新连接。
 * 相反，如果该节点是用于接受查询的临时节点，我们会在出错时完全释放该节点。
 * */
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. 
 *
 * 发送数据。这是使用一个由write（）消耗的琐碎发送缓冲区来处理的。由于这是一个
 * 流量非常低的通道，我们不会试图过多地优化速度。
 * */
void clusterWriteHandler(connection *conn) {
    clusterLink *link = connGetPrivateData(conn);
    ssize_t nwritten;

    nwritten = connWrite(conn, link->sndbuf, sdslen(link->sndbuf));
    if (nwritten <= 0) {
        serverLog(LL_DEBUG,"I/O error writing to node link: %s",
            (nwritten == -1) ? connGetLastError(conn) : "short write");
        handleLinkIOError(link);
        return;
    }
    sdsrange(link->sndbuf,nwritten,-1);
    if (sdslen(link->sndbuf) == 0)
        connSetWriteHandler(link->conn, NULL);
}

/* A connect handler that gets called when a connection to another node
 * gets established.
 
 *
 * 当建立到另一个节点的连接时调用的连接处理程序。
 * */
void clusterLinkConnectHandler(connection *conn) {
    clusterLink *link = connGetPrivateData(conn);
    clusterNode *node = link->node;

    /* Check if connection succeeded 
     *
     * 检查连接是否成功
     * */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_VERBOSE, "Connection with Node %.40s at %s:%d failed: %s",
                node->name, node->ip, node->cport,
                connGetLastError(conn));
        freeClusterLink(link);
        return;
    }

    /* Register a read handler from now on 
     *
     * 从现在起注册读取处理程序
     * */
    connSetReadHandler(conn, clusterReadHandler);

    /* Queue a PING in the new connection ASAP: this is crucial
     * to avoid false positives in failure detection.
     *
     * If the node is flagged as MEET, we send a MEET message instead
     * of a PING one, to force the receiver to add us in its node
     * table. 
     *
     * 尽快在新连接中排队PING：这对于避免故障检测中的误报至关重要。如果节点被标记为
     * MEET，我们会发送一条MEET消息，而不是PING消息，以迫使接收方将我们添加
     * 到其节点表中。
     * */
    mstime_t old_ping_sent = node->ping_sent;
    clusterSendPing(link, node->flags & CLUSTER_NODE_MEET ?
            CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
    if (old_ping_sent) {
        /* If there was an active ping before the link was
         * disconnected, we want to restore the ping time, otherwise
         * replaced by the clusterSendPing() call. 
         *
         * 如果在断开链接之前有一个活动的ping，我们希望恢复ping时间，否则用clusterSendPing（）调用代替。
         * */
        node->ping_sent = old_ping_sent;
    }
    /* We can clear the flag after the first packet is sent.
     * If we'll never receive a PONG, we'll never send new packets
     * to this node. Instead after the PONG is received and we
     * are no longer in meet/handshake status, we want to send
     * normal PING packets. 
     *
     * 我们可以在发送第一个数据包后清除标志。如果我们永远不会收到PONG，我们就永远不
     * 会向这个节点发送新的数据包。相反，在接收到PONG并且我们不再处于会面/握手状态
     * 之后，我们想要发送正常的PING分组。
     * */
    node->flags &= ~CLUSTER_NODE_MEET;

    serverLog(LL_DEBUG,"Connecting with Node %.40s at %s:%d",
            node->name, node->ip, node->cport);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. 
 *
 * 读取数据。尝试先读取标头的第一个字段，以检查数据包的完整长度。
 * 当整个数据包在内存中时，此函数将调用函数来处理数据包。等等
 * */
void clusterReadHandler(connection *conn) {
    clusterMsg buf[1];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = connGetPrivateData(conn);
    unsigned int readlen, rcvbuflen;

    while(1) { /* Read as long as there is data to read. 
                *
                * 只要有数据要读就读。
                * */
        rcvbuflen = link->rcvbuf_len;
        if (rcvbuflen < 8) {
            /* First, obtain the first 8 bytes to get the full message
             * length. 
             *
             * 首先，获取前8个字节以获得完整的消息长度。
             * */
            readlen = 8 - rcvbuflen;
        } else {
            /* Finally read the full message. 
             *
             * 最后阅读全文。
             * */
            hdr = (clusterMsg*) link->rcvbuf;
            if (rcvbuflen == 8) {
                /* Perform some sanity check on the message signature
                 * and length. 
                 *
                 * 对消息签名和长度执行一些健全性检查。
                 * */
                if (memcmp(hdr->sig,"RCmb",4) != 0 ||
                    ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN)
                {
                    serverLog(LL_WARNING,
                        "Bad message length or signature received "
                        "from Cluster bus.");
                    handleLinkIOError(link);
                    return;
                }
            }
            readlen = ntohl(hdr->totlen) - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        nread = connRead(conn,buf,readlen);
        if (nread == -1 && (connGetState(conn) == CONN_STATE_CONNECTED)) return; /* No more data ready. 
                                                                                  *
                                                                                  * 没有更多的数据准备就绪。
                                                                                  * */

        if (nread <= 0) {
            /* I/O error... 
             *
             * I/O错误。。。
             * */
            serverLog(LL_DEBUG,"I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : connGetLastError(conn));
            handleLinkIOError(link);
            return;
        } else {
            /* Read data and recast the pointer to the new buffer. 
             *
             * 读取数据并将指针重新转换到新的缓冲区。
             * */
            size_t unused = link->rcvbuf_alloc - link->rcvbuf_len;
            if ((size_t)nread > unused) {
                size_t required = link->rcvbuf_len + nread;
                /* If less than 1mb, grow to twice the needed size, if larger grow by 1mb. 
                 *
                 * 如果小于1mb，则生长到所需大小的两倍，如果较大，则生长1mb。
                 * */
                link->rcvbuf_alloc = required < RCVBUF_MAX_PREALLOC ? required * 2: required + RCVBUF_MAX_PREALLOC;
                link->rcvbuf = zrealloc(link->rcvbuf, link->rcvbuf_alloc);
            }
            memcpy(link->rcvbuf + link->rcvbuf_len, buf, nread);
            link->rcvbuf_len += nread;
            hdr = (clusterMsg*) link->rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. 
         *
         * 获得的总长度？处理此数据包。
         * */
        if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->totlen)) {
            if (clusterProcessPacket(link)) {
                if (link->rcvbuf_alloc > RCVBUF_INIT_LEN) {
                    zfree(link->rcvbuf);
                    link->rcvbuf = zmalloc(link->rcvbuf_alloc = RCVBUF_INIT_LEN);
                }
                link->rcvbuf_len = 0;
            } else {
                return; /* Link no longer valid. 
                         *
                         * 链接不再有效。
                         * */
            }
        }
    }
}

/* Put stuff into the send buffer.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. 
 *
 * 把东西放进发送缓冲区。可以保证，该函数永远不会对无效的链接产生副作用，因此可以安
 * 全地从稍后处理同一链接的事件处理程序中调用该函数。
 * */
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {
    if (sdslen(link->sndbuf) == 0 && msglen != 0)
        connSetWriteHandlerWithBarrier(link->conn, clusterWriteHandler, 1);

    link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);

    /* Populate sent messages stats. 
     *
     * 填充已发送消息的统计信息。
     * */
    clusterMsg *hdr = (clusterMsg*) msg;
    uint16_t type = ntohs(hdr->type);
    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->stats_bus_messages_sent[type]++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. 
 *
 * 向具有连接链路的集群中的所有节点发送消息。可以保证该函数永远不会对某些节点->链接产生副作用，
 * 因此可以安全地从稍后处理节点链接的事件处理程序中调用该函数。
 * */
void clusterBroadcastMessage(void *buf, size_t len) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!node->link) continue;
        if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
            continue;
        clusterSendMessage(node->link,buf,len);
    }
    dictReleaseIterator(di);
}

/* Build the message header. hdr must point to a buffer at least
 * sizeof(clusterMsg) in bytes. 
 *
 * 生成消息标头。hdr必须指向大小至少为（clusterMsg）（以字节为单位）的
 * 缓冲区。
 * */
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen = 0;
    uint64_t offset;
    clusterNode *master;

    /* If this node is a master, we send its slots bitmap and configEpoch.
     * If this node is a slave we send the master's information instead (the
     * node is flagged as slave so the receiver knows that it is NOT really
     * in charge for this slots. 
     *
     * 如果这个节点是一个主节点，我们会发送它的插槽位图和configEpoch。如果这
     * 个节点是从节点，我们会发送主节点的信息（该节点被标记为从节点，这样接收器就知道它
     * 并没有真正负责这个插槽。
     * */
    master = (nodeIsSlave(myself) && myself->slaveof) ?
              myself->slaveof : myself;

    memset(hdr,0,sizeof(*hdr));
    hdr->ver = htons(CLUSTER_PROTO_VER);
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';
    hdr->type = htons(type);
    memcpy(hdr->sender,myself->name,CLUSTER_NAMELEN);

    /* If cluster-announce-ip option is enabled, force the receivers of our
     * packets to use the specified address for this node. Otherwise if the
     * first byte is zero, they'll do auto discovery. 
     *
     * 如果启用了集群通告ip选项，则强制数据包的接收器使用此节点的指定地址。否则，如果
     * 第一个字节为零，他们将进行自动发现。
     * */
    memset(hdr->myip,0,NET_IP_STR_LEN);
    if (server.cluster_announce_ip) {
        strncpy(hdr->myip,server.cluster_announce_ip,NET_IP_STR_LEN);
        hdr->myip[NET_IP_STR_LEN-1] = '\0';
    }

    /* Handle cluster-announce-port as well. 
     *
     * 同时处理集群公告端口。
     * */
    int port = server.tls_cluster ? server.tls_port : server.port;
    int announced_port = server.cluster_announce_port ?
                         server.cluster_announce_port : port;
    int announced_cport = server.cluster_announce_bus_port ?
                          server.cluster_announce_bus_port :
                          (port + CLUSTER_PORT_INCR);

    memcpy(hdr->myslots,master->slots,sizeof(hdr->myslots));
    memset(hdr->slaveof,0,CLUSTER_NAMELEN);
    if (myself->slaveof != NULL)
        memcpy(hdr->slaveof,myself->slaveof->name, CLUSTER_NAMELEN);
    hdr->port = htons(announced_port);
    hdr->cport = htons(announced_cport);
    hdr->flags = htons(myself->flags);
    hdr->state = server.cluster->state;

    /* Set the currentEpoch and configEpochs. 
     *
     * 设置currentEpoch和configEpoch。
     * */
    hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
    hdr->configEpoch = htonu64(master->configEpoch);

    /* Set the replication offset. 
     *
     * 设置复制偏移量。
     * */
    if (nodeIsSlave(myself))
        offset = replicationGetSlaveOffset();
    else
        offset = server.master_repl_offset;
    hdr->offset = htonu64(offset);

    /* Set the message flags. 
     *
     * 设置消息标志。
     * */
    if (nodeIsMaster(myself) && server.cluster->mf_end)
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

    /* Compute the message length for certain messages. For other messages
     * this is up to the caller. 
     *
     * 计算某些消息的消息长度。对于其他消息，这取决于呼叫者。
     * */
    if (type == CLUSTERMSG_TYPE_FAIL) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataFail);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataUpdate);
    }
    hdr->totlen = htonl(totlen);
    /* For PING, PONG, and MEET, fixing the totlen field is up to the caller. 
     *
     * 对于PING、PONG和MEET，修复totlen字段取决于调用者。
     * */
}

/* Return non zero if the node is already present in the gossip section of the
 * message pointed by 'hdr' and having 'count' gossip entries. Otherwise
 * zero is returned. Helper for clusterSendPing(). 
 *
 * 如果节点已经存在于“hdr”指向的消息的gossip部分中，并且具有“count”个gossip
 * 条目，则返回非零。否则返回零。clusterSendPing（）的帮助程序。
 * */
int clusterNodeIsInGossipSection(clusterMsg *hdr, int count, clusterNode *n) {
    int j;
    for (j = 0; j < count; j++) {
        if (memcmp(hdr->data.ping.gossip[j].nodename,n->name,
                CLUSTER_NAMELEN) == 0) break;
    }
    return j != count;
}

/* Set the i-th entry of the gossip section in the message pointed by 'hdr'
 * to the info of the specified node 'n'. 
 *
 * 将“hdr”指向的消息中gossip部分的第i个条目设置为指定节点“n”的信息。
 * */
void clusterSetGossipEntry(clusterMsg *hdr, int i, clusterNode *n) {
    clusterMsgDataGossip *gossip;
    gossip = &(hdr->data.ping.gossip[i]);
    memcpy(gossip->nodename,n->name,CLUSTER_NAMELEN);
    gossip->ping_sent = htonl(n->ping_sent/1000);
    gossip->pong_received = htonl(n->pong_received/1000);
    memcpy(gossip->ip,n->ip,sizeof(n->ip));
    gossip->port = htons(n->port);
    gossip->cport = htons(n->cport);
    gossip->flags = htons(n->flags);
    gossip->notused1 = 0;
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip information. 
 *
 * 向指定节点发送PING或PONG数据包，确保添加足够的gossip信息。
 * */
void clusterSendPing(clusterLink *link, int type) {
    unsigned char *buf;
    clusterMsg *hdr;
    int gossipcount = 0; /* Number of gossip sections added so far. 
                          *
                          * 到目前为止添加的gossip部分的数量。
                          * */
    int wanted; /* Number of gossip sections we want to append if possible. 
                 *
                 * 如果可能的话，我们希望附加的gossip部分的数量。
                 * */
    int totlen; /* Total packet length. 
                 *
                 * 数据包总长度。
                 * */
    /* freshnodes is the max number of nodes we can hope to append at all:
     * nodes available minus two (ourself and the node we are sending the
     * message to). However practically there may be less valid nodes since
     * nodes in handshake state, disconnected, are not considered. 
     *
     * freshnodes是我们希望附加的最大节点数：可用节点减去两个（我们自己和我们
     * 发送消息的节点）。然而，实际上可能存在无效节点，因为不考虑处于握手状态、断开连接
     * 的节点。
     * */
    int freshnodes = dictSize(server.cluster->nodes)-2;

    /* How many gossip sections we want to add? 1/10 of the number of nodes
     * and anyway at least 3. Why 1/10?
     *
     * If we have N masters, with N/10 entries, and we consider that in
     * node_timeout we exchange with each other node at least 4 packets
     * (we ping in the worst case in node_timeout/2 time, and we also
     * receive two pings from the host), we have a total of 8 packets
     * in the node_timeout*2 failure reports validity time. So we have
     * that, for a single PFAIL node, we can expect to receive the following
     * number of failure reports (in the specified window of time):
     *
     * PROB * GOSSIP_ENTRIES_PER_PACKET * TOTAL_PACKETS:
     *
     * PROB = probability of being featured in a single gossip entry,
     *        which is 1 / NUM_OF_NODES.
     * ENTRIES = 10.
     * TOTAL_PACKETS = 2 * 4 * NUM_OF_MASTERS.
     *
     * If we assume we have just masters (so num of nodes and num of masters
     * is the same), with 1/10 we always get over the majority, and specifically
     * 80% of the number of nodes, to account for many masters failing at the
     * same time.
     *
     * Since we have non-voting slaves that lower the probability of an entry
     * to feature our node, we set the number of entries per packet as
     * 10% of the total nodes we have. 
     *
     * 我们想增加多少gossip板块？节点数量的1/10，并且无论如何至少为3。为什么是1/10？
     * 如果我们有N个主节点，具有N/10个条目，并且我们认为在node_timeout中，
     * 我们与每个其他节点交换至少4个数据包（在最坏的情况下，我们在node_ttimeout/2时间内进行ping，
     * 并且我们还从主节点接收两次ping），那么在
     * node_timeout*2故障报告有效时间内，我们总共有8个数据包。因此，对于
     * 单个PFAIL节点，我们可以预期（在指定的时间窗口内）收到以下数量的故障报告：
     * PROB GOSSIP_ENTRIES_PER_PACKET TOTAL_PACKETS:PROB=在单个gossip条目中出现的概率，即1/NUM_of_NODES。
     * 条目=10。TOTAL_PACKETS＝2 4 NUM_OF_MASTERS。如果
     * 我们假设我们只有主节点（因此节点数量和主节点数量相同），在1/10的情况下，我们
     * 总是超过大多数，特别是节点数量的80%，以解释许多主节点同时失败的原因。由于我们
     * 有无投票权的从节点，它降低了条目以我们的节点为特征的概率，因此我们将每个数据包的
     * 条目数量设置为我们拥有的节点总数的10%。
     * */
    wanted = floor(dictSize(server.cluster->nodes)/10);
    if (wanted < 3) wanted = 3;
    if (wanted > freshnodes) wanted = freshnodes;

    /* Include all the nodes in PFAIL state, so that failure reports are
     * faster to propagate to go from PFAIL to FAIL state. 
     *
     * 包括PFAIL状态下的所有节点，以便故障报告能够更快地从PFAIL传播到FAIL状态。
     * */
    int pfail_wanted = server.cluster->stats_pfail_nodes;

    /* Compute the maximum totlen to allocate our buffer. We'll fix the totlen
     * later according to the number of gossip sections we really were able
     * to put inside the packet. 
     *
     * 计算分配缓冲区的最大总长度。我们稍后会根据我们真正能够放入包中的gossip部分的数量来
     * 修复这个总数。
     * */
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*(wanted+pfail_wanted));
    /* Note: clusterBuildMessageHdr() expects the buffer to be always at least
     * sizeof(clusterMsg) or more. 
     *
     * 注意：clusterBuildMessageHdr（）要求缓冲区的大小始终至少为（clusterMsg）或更大。
     * */
    if (totlen < (int)sizeof(clusterMsg)) totlen = sizeof(clusterMsg);
    buf = zcalloc(totlen);
    hdr = (clusterMsg*) buf;

    /* Populate the header. 
     *
     * 填充标头。
     * */
    if (link->node && type == CLUSTERMSG_TYPE_PING)
        link->node->ping_sent = mstime();
    clusterBuildMessageHdr(hdr,type);

    /* Populate the gossip fields 
     *
     * 填充gossip字段
     * */
    int maxiterations = wanted*3;
    while(freshnodes > 0 && gossipcount < wanted && maxiterations--) {
        dictEntry *de = dictGetRandomKey(server.cluster->nodes);
        clusterNode *this = dictGetVal(de);

        /* Don't include this node: the whole packet header is about us
         * already, so we just gossip about other nodes. 
         *
         * 不要包括这个节点：整个数据包头已经是关于我们的了，所以我们只是在gossip其他节点。
         * */
        if (this == myself) continue;

        /* PFAIL nodes will be added later. 
         *
         * 稍后将添加PFAIL节点。
         * */
        if (this->flags & CLUSTER_NODE_PFAIL) continue;

        /* In the gossip section don't include:
         * 1) Nodes in HANDSHAKE state.
         * 3) Nodes with the NOADDR flag set.
         * 4) Disconnected nodes if they don't have configured slots.
         
         *
         * 在gossip部分中，不要包括：
         * 1）处于HANDSHAKE状态的节点。
         * 3） 设置了NOADDR标志的节点。
         * 4） 如果节点没有配置插槽，则断开连接。
         * */
        if (this->flags & (CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_NOADDR) ||
            (this->link == NULL && this->numslots == 0))
        {
            freshnodes--; /* Technically not correct, but saves CPU. 
                           *
                           * 技术上不正确，但节省了CPU。
                           * */
            continue;
        }

        /* Do not add a node we already have. 
         *
         * 不要添加我们已有的节点。
         * */
        if (clusterNodeIsInGossipSection(hdr,gossipcount,this)) continue;

        /* Add it 
         *
         * 添加它
         * */
        clusterSetGossipEntry(hdr,gossipcount,this);
        freshnodes--;
        gossipcount++;
    }

    /* If there are PFAIL nodes, add them at the end. 
     *
     * 如果有PFAIL节点，请在末尾添加它们。
     * */
    if (pfail_wanted) {
        dictIterator *di;
        dictEntry *de;

        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL && pfail_wanted > 0) {
            clusterNode *node = dictGetVal(de);
            if (node->flags & CLUSTER_NODE_HANDSHAKE) continue;
            if (node->flags & CLUSTER_NODE_NOADDR) continue;
            if (!(node->flags & CLUSTER_NODE_PFAIL)) continue;
            clusterSetGossipEntry(hdr,gossipcount,node);
            freshnodes--;
            gossipcount++;
            /* We take the count of the slots we allocated, since the
             * PFAIL stats may not match perfectly with the current number
             * of PFAIL nodes. 
             *
             * 我们对分配的插槽进行计数，因为PFAIL统计数据可能与当前PFAIL节点数不完全
             * 匹配。
             * */
            pfail_wanted--;
        }
        dictReleaseIterator(di);
    }

    /* Ready to send... fix the totlen fiend and queue the message in the
     * output buffer. 
     *
     * 准备发送。。。修复totlen恶魔并将消息排入输出缓冲区。
     * */
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*gossipcount);
    hdr->count = htons(gossipcount);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(link,buf,totlen);
    zfree(buf);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * In Redis Cluster pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a slave promotion).
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_SLAVES -> All slaves in my master-slaves ring.
 
 *
 * 向每个未处于握手状态并且我们有有效链路的连接节点发送PONG数据包。在Redis中，
 * 集群端口不仅用于故障检测，还用于承载重要的配置信息。因此，当配置发生变化时，
 * 广播pong是有用的，我们希望尽快让集群知道（例如，在从属升级之后）。
 *
 * “target”参数使用以下定义指定接收节点：
 *  CLUSTER_BROADCAST_ALL-> 所有已知节点。
 *  CLUSTER_BROADCAT_LOCAL_SLAVES-> 是我的从节点 或者 和我是同一个主节点之下。
 * */
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1
void clusterBroadcastPong(int target) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!node->link) continue;
        if (node == myself || nodeInHandshake(node)) continue;
        if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
            int local_slave =
                // 是从节点
                nodeIsSlave(node) &&
                // 有归属的主节点
                node->slaveof &&
                // 是我的从节点 或者 和我是同一个主节点之下
                (node->slaveof == myself || node->slaveof == myself->slaveof);
            if (!local_slave) continue;
        }
        clusterSendPing(node->link,CLUSTERMSG_TYPE_PONG);
    }
    dictReleaseIterator(di);
}

/* Send a PUBLISH message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. 
 *
 * 发送PUBLISH消息。如果链接为NULL，则消息将广播到整个集群。
 * */
void clusterSendPublish(clusterLink *link, robj *channel, robj *message) {
    unsigned char *payload;
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;
    uint32_t channel_len, message_len;

    channel = getDecodedObject(channel);
    message = getDecodedObject(message);
    channel_len = sdslen(channel->ptr);
    message_len = sdslen(message->ptr);

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_PUBLISH);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;

    hdr->data.publish.msg.channel_len = htonl(channel_len);
    hdr->data.publish.msg.message_len = htonl(message_len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible 
     *
     * 如果可能，请尝试使用本地缓冲区
     * */
    if (totlen < sizeof(buf)) {
        payload = (unsigned char*)buf;
    } else {
        payload = zmalloc(totlen);
        memcpy(payload,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) payload;
    }
    memcpy(hdr->data.publish.msg.bulk_data,channel->ptr,sdslen(channel->ptr));
    memcpy(hdr->data.publish.msg.bulk_data+sdslen(channel->ptr),
        message->ptr,sdslen(message->ptr));

    if (link)
        clusterSendMessage(link,payload,totlen);
    else
        clusterBroadcastMessage(payload,totlen);

    decrRefCount(channel);
    decrRefCount(message);
    if (payload != (unsigned char*)buf) zfree(payload);
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (CLUSTER_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to CLUSTER_NODE_FAIL and ask all the other
 * nodes to do the same ASAP.
 *
 * 向我们能够联系的所有节点发送FAIL消息。
 * 当我们检测到一个节点出现故障时，就会发送FAIL消息（CLUSTER_NODE_PFAIL），
 * 并且我们还会收到对此的gossip确认：我们将节点状态切换到 CLUSTER_NODE_FAIL，并要求所有其他节点尽快执行相同操作。
 * */
void clusterSendFail(char *nodename) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAIL);
    memcpy(hdr->data.fail.about.nodename,nodename,CLUSTER_NAMELEN);
    clusterBroadcastMessage(buf,ntohl(hdr->totlen));
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. 
 *
 * 对指定的 Link 发送 node 的 UPDATE 消息，
 * 包括节点名称、插槽位图和configEpoch信息。
 * */
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;

    if (link == NULL) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_UPDATE);
    memcpy(hdr->data.update.nodecfg.nodename,node->name,CLUSTER_NAMELEN);
    hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);
    memcpy(hdr->data.update.nodecfg.slots,node->slots,sizeof(node->slots));
    clusterSendMessage(link,(unsigned char*)buf,ntohl(hdr->totlen));
}

/* Send a MODULE message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. 
 *
 * 发送MODULE消息。如果链接为NULL，则消息将广播到整个集群。
 * */
void clusterSendModule(clusterLink *link, uint64_t module_id, uint8_t type,
                       unsigned char *payload, uint32_t len) {
    unsigned char *heapbuf;
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MODULE);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgModule) - 3 + len;

    hdr->data.module.msg.module_id = module_id; /* Already endian adjusted. 
                                                 *
                                                 * 已经调整了endian。
                                                 * */
    hdr->data.module.msg.type = type;
    hdr->data.module.msg.len = htonl(len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible 
     *
     * 如果可能，请尝试使用本地缓冲区
     * */
    if (totlen < sizeof(buf)) {
        heapbuf = (unsigned char*)buf;
    } else {
        heapbuf = zmalloc(totlen);
        memcpy(heapbuf,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) heapbuf;
    }
    memcpy(hdr->data.module.msg.bulk_data,payload,len);

    if (link)
        clusterSendMessage(link,heapbuf,totlen);
    else
        clusterBroadcastMessage(heapbuf,totlen);

    if (heapbuf != (unsigned char*)buf) zfree(heapbuf);
}

/* This function gets a cluster node ID string as target, the same way the nodes
 * addresses are represented in the modules side, resolves the node, and sends
 * the message. If the target is NULL the message is broadcasted.
 *
 * The function returns C_OK if the target is valid, otherwise C_ERR is
 * returned. 
 *
 * 此函数获取一个集群节点ID字符串作为目标，与模块端表示节点地址的方式相同，解析节
 * 点并发送消息。如果目标为NULL，则广播消息。
 *
 * 如果目标有效，则函数返回C_OK，否则返回C_ERR。
 * */
int clusterSendModuleMessageToTarget(const char *target,
                                     uint64_t module_id,
                                     uint8_t type,
                                     unsigned char *payload,
                                     uint32_t len
                                     ) {
    clusterNode *node = NULL;

    if (target != NULL) {
        node = clusterLookupNode(target);
        if (node == NULL || node->link == NULL) return C_ERR;
    }

    clusterSendModule(target ? node->link : NULL,
                      module_id, type, payload, len);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * For now we do very little, just propagating PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.

 * CLUSTER Pub/Sub 支持
 *
 * 目前我们只做很少的工作，只是在整个集群中传播PUBLISH消息。
 * 在未来，我们将努力变得更智能，避免在没有接收到给定频道的情况下将这些消息传播到主节点
 * */
void clusterPropagatePublish(robj *channel, robj *message) {
    clusterSendPublish(NULL, channel, message);
}

/* -----------------------------------------------------------------------------
 * SLAVE node specific functions
 * SLAVE节点特定功能
 * --------------------------------------------------------------------------*/

/* This function sends a FAILOVER_AUTH_REQUEST message to every node in order to
 * see if there is the quorum for this slave instance to failover its failing
 * master.
 *
 * Note that we send the failover request to everybody, master and slave nodes,
 * but only the masters are supposed to reply to our query. 
 *
 * 此函数向每个节点发送FAILOVER_AUTH_REQUEST消息，来获取 故障转移的选票。
 *
 * 请注意，我们将故障转移请求发送给每个人,
 * 包括主节点和从节点，但只有主节点应该回复我们的查询。
 * */
void clusterRequestFailoverAuth(void) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
    /* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
     * in the header to communicate the nodes receiving the message that
     * they should authorized the failover even if the master is working. 
     *
     * 如果这是手动故障转移，请在标头中设置CLUSTERMSG_FLAG0_FORCHECK位，以便与接收到消息的节点进行通信，
     * 即 即使主节点正在工作，它们也应该授权故障转移。
     * */
    if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterBroadcastMessage(buf,totlen);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. 
 *
 * 向指定节点发送FAILOVER_AUTH_ACK消息。
 * */
void clusterSendFailoverAuth(clusterNode *node) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,(unsigned char*)buf,totlen);
}

/* Send a MFSTART message to the specified node. 
 *
 * 向指定节点发送MFSTART消息。
 * */
void clusterSendMFStart(clusterNode *node) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MFSTART);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,(unsigned char*)buf,totlen);
}

/* Vote for the node asking for our vote if there are the conditions. 
 *
 * 如果有条件，请投票给要求我们投票的节点。
 * */
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {
    clusterNode *master = node->slaveof;
    uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);
    uint64_t requestConfigEpoch = ntohu64(request->configEpoch);
    unsigned char *claimed_slots = request->myslots;
    int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
    int j;

    /* IF we are not a master serving at least 1 slot, we don't have the
     * right to vote, as the cluster size in Redis Cluster is the number
     * of masters serving at least one slot, and quorum is the cluster
     * size + 1 
     *
     * 如果我们不是服务于至少1个插槽的主节点，我们就没有投票权，
     * 因为Redis cluster中的集群大小是服务于至少一个插槽的主节点数量，
     * quorum是集群大小+1
     * */
    if (nodeIsSlave(myself) || myself->numslots == 0) return;

    /* Request epoch must be >= our currentEpoch.
     * Note that it is impossible for it to actually be greater since
     * our currentEpoch was updated as a side effect of receiving this
     * request, if the request epoch was greater. 
     *
     * 请求epoch必须>=我们的当前epoch。请注意，如果请求epoch更大，那么
     * 它实际上不可能更大，因为我们的currentEpoch是作为接收此请求的副作用而更新的。
     * */
    if (requestCurrentEpoch < server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
            "Failover auth denied to %.40s: reqEpoch (%llu) < curEpoch(%llu)",
            node->name,
            (unsigned long long) requestCurrentEpoch,
            (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* I already voted for this epoch? Return ASAP. 
     *
     * 我已经投票支持这个时代了？尽快返回。
     * */
    if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: already voted for epoch %llu",
                node->name,
                (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* Node must be a slave and its master down.
     * The master can be non failing if the request is flagged
     * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). 
     *
     * 节点必须是从节点，并且其主节点已关闭。如果使用CLUSTERMSG_FLAG0_FORCECK（手动故障转移）标记请求，则主节点可能不会失败。
     * */
    if (nodeIsMaster(node) || master == NULL ||
        (!nodeFailed(master) && !force_ack))
    {
        if (nodeIsMaster(node)) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: it is a master node",
                    node->name);
        } else if (master == NULL) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: I don't know its master",
                    node->name);
        } else if (!nodeFailed(master)) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: its master is up",
                    node->name);
        }
        return;
    }

    /* We did not voted for a slave about this master for two
     * times the node timeout. This is not strictly needed for correctness
     * of the algorithm but makes the base case more linear. 
     *
     * 在节点超时的两次时间里，我们没有投票给这个主节点的从节点。这对于算法的正确性不是
     * 严格需要的，而是使基本情况更加线性。
     * */
    if (mstime() - node->slaveof->voted_time < server.cluster_node_timeout * 2)
    {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "can't vote about this master before %lld milliseconds",
                node->name,
                (long long) ((server.cluster_node_timeout*2)-
                             (mstime() - node->slaveof->voted_time)));
        return;
    }

    /* The slave requesting the vote must have a configEpoch for the claimed
     * slots that is >= the one of the masters currently serving the same
     * slots in the current configuration. 
     *
     * 请求投票的从节点必须为声明的插槽具有一个configEpoch，该插槽>=当前配置
     * 中为相同插槽提供服务的主节点之一。
     * */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(claimed_slots, j) == 0) continue;
        if (server.cluster->slots[j] == NULL ||
            server.cluster->slots[j]->configEpoch <= requestConfigEpoch)
        {
            continue;
        }
        /* If we reached this point we found a slot that in our current slots
         * is served by a master with a greater configEpoch than the one claimed
         * by the slave requesting our vote. Refuse to vote for this slave. 
         *
         * 如果我们达到这一点，我们发现在我们当前的插槽中，一个主插槽的configEpoch比请求我们投票的从插槽所声称的要大。
         * 拒绝投票给这个从节点。
         * */
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "slot %d epoch (%llu) > reqEpoch (%llu)",
                node->name, j,
                (unsigned long long) server.cluster->slots[j]->configEpoch,
                (unsigned long long) requestConfigEpoch);
        return;
    }

    /* We can vote for this slave. 
     *
     * 我们可以投票给这个从节点。
     * */
    server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
    node->slaveof->voted_time = mstime();
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_FSYNC_CONFIG);
    clusterSendFailoverAuth(node);
    serverLog(LL_WARNING, "Failover auth granted to %.40s for epoch %llu",
        node->name, (unsigned long long) server.cluster->currentEpoch);
}

/* This function returns the "rank" of this instance, a slave, in the context
 * of its master-slaves ring. The rank of the slave is given by the number of
 * other slaves for the same master that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A slave with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple slaves may have the same rank, in case they have the same offset.
 *
 * The slave rank is used to add a delay to start an election in order to
 * get voted and replace a failing master. Slaves with better replication
 * offsets are more likely to win. 
 *
 * 此函数返回此节点（从节点）在其主从环上下文中的“等级”。从节点的级别由同一主节点的其他
 * 从节点的数量给定，这些从节点与本地主节点相比具有更好的复制偏移量（更好意味着更大，因此
 * 它们要求更多的数据）。等级为0的从节点是具有最大（最新）复制偏移量的从节点,
 * 依此类推。请注意，由于等级是如何计算的，多个从节点可能具有相同的等级，以防它们具有
 * 相同的偏移。从节点等级用于增加开始选举的延迟，以便获得投票并替换失败的主节点。复制偏
 * 移量更好的从节点更有可能获胜。
 * */
int clusterGetSlaveRank(void) {
    long long myoffset;
    int j, rank = 0;
    clusterNode *master;

    serverAssert(nodeIsSlave(myself));
    master = myself->slaveof;
    if (master == NULL) return 0; /* Never called by slaves without master. 
                                   *
                                   * 没有主节点，从节点永远不会召唤。
                                   * */

    myoffset = replicationGetSlaveOffset();
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] != myself &&
            !nodeCantFailover(master->slaves[j]) &&
            master->slaves[j]->repl_offset > myoffset) rank++;
    return rank;
}

/* This function is called by clusterHandleSlaveFailover() in order to
 * let the slave log why it is not able to failover. Sometimes there are
 * not the conditions, but since the failover function is called again and
 * again, we can't log the same things continuously.
 *
 * This function works by logging only if a given set of conditions are
 * true:
 *
 * 1) The reason for which the failover can't be initiated changed.
 *    The reasons also include a NONE reason we reset the state to
 *    when the slave finds that its master is fine (no FAIL flag).
 * 2) Also, the log is emitted again if the master is still down and
 *    the reason for not failing over is still the same, but more than
 *    CLUSTER_CANT_FAILOVER_RELOG_PERIOD seconds elapsed.
 * 3) Finally, the function only logs if the slave is down for more than
 *    five seconds + NODE_TIMEOUT. This way nothing is logged when a
 *    failover starts in a reasonable time.
 *
 * The function is called with the reason why the slave can't failover
 * which is one of the integer macros CLUSTER_CANT_FAILOVER_*.
 *
 * The function is guaranteed to be called only if 'myself' is a slave. 
 *
 * clusterHandleSlaveFailover（）调用此函数，以便让从属服
 * 务器记录其无法进行故障切换的原因。有时没有条件，但由于故障转移函数被一次又一次地
 * 调用，我们无法连续记录相同的事情。只有在给定的一组条件为真时，此函数才能通过日志
 * 记录工作：
 * 
 * 1）无法启动故障转移的原因已更改。原因还包括一个NONE原因，当从节点
 * 发现其主节点正常时，我们将状态重置为NONE（无FAIL标志）。
 * 
 * 2） 此外，如果主节点仍然关闭，并且没有故障转移的原因仍然相同，但超过CLUSTER_CANT_F
 * AILOVER_RELOG_PERIOD秒，则会再次发出日志。
 * 
 * 3） 最后，该函数仅在从节点停机超过5秒+NODE_TIMEOUT时记录。这样，当故障转移在合理的
 * 时间内启动时，就不会记录任何内容。调用该函数的原因是从属无法进行故障切换，这是整
 * 数宏CLUSTER_CANT_failover_*之一。只有当“myself”是
 * 从函数时，才能保证调用该函数。
 * */
void clusterLogCantFailover(int reason) {
    char *msg;
    static time_t lastlog_time = 0;
    mstime_t nolog_fail_time = server.cluster_node_timeout + 5000;

    /* Don't log if we have the same reason for some time. 
     *
     * 如果我们在一段时间内有同样的原因，请不要登录。
     * */
    if (reason == server.cluster->cant_failover_reason &&
        time(NULL)-lastlog_time < CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
        return;

    server.cluster->cant_failover_reason = reason;

    /* We also don't emit any log if the master failed no long ago, the
     * goal of this function is to log slaves in a stalled condition for
     * a long time. 
     *
     * 如果主节点不久前发生故障，我们也不会发出任何日志，该函数的目标是记录长期处于停滞状
     * 态的从节点。
     * */
    if (myself->slaveof &&
        nodeFailed(myself->slaveof) &&
        (mstime() - myself->slaveof->fail_time) < nolog_fail_time) return;

    switch(reason) {
    case CLUSTER_CANT_FAILOVER_DATA_AGE:
        msg = "Disconnected from master for longer than allowed. "
              "Please check the 'cluster-replica-validity-factor' configuration "
              "option.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_DELAY:
        msg = "Waiting the delay before I can start a new failover.";
        break;
    case CLUSTER_CANT_FAILOVER_EXPIRED:
        msg = "Failover attempt expired.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_VOTES:
        msg = "Waiting for votes, but majority still not reached.";
        break;
    default:
        msg = "Unknown reason code.";
        break;
    }
    lastlog_time = time(NULL);
    serverLog(LL_WARNING,"Currently unable to failover: %s", msg);
}

/* This function implements the final part of automatic and manual failovers,
 * where the slave grabs its master's hash slots, and propagates the new
 * configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. 
 *
 * 该函数实现了自动和手动故障切换的最后一部分，在该部分中，从节点获取其主节点的哈希槽，并传播新配置。
 * 
 * 请注意，由调用者来确保节点已经获得了新的配置epoch。
 * */
void clusterFailoverReplaceYourMaster(void) {
    int j;
    clusterNode *oldmaster = myself->slaveof;

    if (nodeIsMaster(myself) || oldmaster == NULL) return;

    /* 1) Turn this node into a master. 
     *
     * 1） 将此节点转换为主节点。
     * */
    clusterSetNodeAsMaster(myself);
    replicationUnsetMaster();

    /* 2) Claim all the slots assigned to our master. 
     *
     * 2） 申请分配给我们的主节点的所有插槽。
     * */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeGetSlotBit(oldmaster,j)) {
            clusterDelSlot(j);
            clusterAddSlot(myself,j);
        }
    }

    /* 3) Update state and save config. 
     *
     * 3） 更新状态并保存配置。
     * */
    clusterUpdateState();
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state
     *    accordingly and detect that we switched to master role. 
     *
     * 4） Pong所有其他节点，以便它们可以相应地更新状态，并检测到我们切换到了master角色。
     * */
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) If there was a manual failover in progress, clear the state. 
     *
     * 5） 如果正在进行手动故障切换，请清除该状态。
     * */
    resetManualFailover();
}

/* This function is called if we are a slave node and our master serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * The gaol of this function is:
 * 1) To check if we are able to perform a failover, is our data updated?
 * 2) Try to get elected by masters.
 * 3) Perform the failover informing all the other nodes.
 
 *
 * 如果我们是从节点，并且我们的主节点为非零数量的哈希槽提供服务，则调用此函数。该功
 * 能的核心是：
 * 1） 检查我们是否能够执行故障转移，我们的数据是否更新？
 * 2） 试着被主节点们选出来。
 * 3） 执行故障转移，通知所有其他节点。
 * */
void clusterHandleSlaveFailover(void) {
    // data_age 是指当前数据与主节点同步的间隔，如果间隔太大，就不符合故障转移了
    mstime_t data_age;
    mstime_t auth_age = mstime() - server.cluster->failover_auth_time;
    int needed_quorum = (server.cluster->size / 2) + 1;
    int manual_failover = server.cluster->mf_end != 0 &&
                          server.cluster->mf_can_start;
    mstime_t auth_timeout, auth_retry_time;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

    /* Compute the failover timeout (the max time we have to send votes
     * and wait for replies), and the failover retry time (the time to wait
     * before trying to get voted again).
     *
     * Timeout is MAX(NODE_TIMEOUT*2,2000) milliseconds.
     * Retry is two times the Timeout.
     
     *
     * 计算故障转移超时（我们必须发送投票并等待回复的最长时间）和故障转移重试时间（尝试
     * 再次获得投票之前等待的时间）。超时为MAX（NODE_Timeout*22000）毫秒。重试是超时的两倍。
     * */
    auth_timeout = server.cluster_node_timeout*2;
    if (auth_timeout < 2000) auth_timeout = 2000;
    auth_retry_time = auth_timeout*2;

    /* Pre conditions to run the function, that must be met both in case
     * of an automatic or manual failover:
     * 1) We are a slave.
     * 2) Our master is flagged as FAIL, or this is a manual failover.
     * 3) We don't have the no failover configuration set, and this is
     *    not a manual failover.
     * 4) It is serving slots. 
     *
     * 运行功能的先决条件，在自动或手动故障切换的情况下都必须满足：
     * 1） 我们是从节点。
     * 2） 我们的主节点被标记为FAIL，或者这是手动故障转移。
     * 3） 我们没有故障切换配置，而且这不是手动故障切换。
     * 4） 它有提供插槽。
     * */
    if (nodeIsMaster(myself) ||
        myself->slaveof == NULL ||
        (!nodeFailed(myself->slaveof) && !manual_failover) ||
        (server.cluster_slave_no_failover && !manual_failover) ||
        myself->slaveof->numslots == 0)
    {
        /* There are no reasons to failover, so we set the reason why we
         * are returning without failing over to NONE. 
         *
         * 没有故障切换的原因，所以我们将在不进行故障切换的情况下返回的原因设置为NONE。
         * */
        server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
        return;
    }

    /* Set data_age to the number of seconds we are disconnected from
     * the master. 
     *
     * 将data_age设置为我们与主节点断开连接的秒数。
     * */
    if (server.repl_state == REPL_STATE_CONNECTED) {
        data_age = (mstime_t)(server.unixtime - server.master->lastinteraction)
                   * 1000;
    } else {
        data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
    }

    /* Remove the node timeout from the data age as it is fine that we are
     * disconnected from our master at least for the time it was down to be
     * flagged as FAIL, that's the baseline. 
     *
     * 从数据期限中删除节点超时，因为我们与主节点断开连接是可以的，至少在它被标记为FAIL的时间内，这是基线。
     * */
    if (data_age > server.cluster_node_timeout)
        data_age -= server.cluster_node_timeout;

    /* Check if our data is recent enough according to the slave validity
     * factor configured by the user.
     *
     * Check bypassed for manual failovers. 
     *
     * 根据用户配置的从节点有效性因素，检查我们的数据是否足够新。检查旁路是否存在手动故障切换。
     *
     * 如果 data_age 间隔太大，就不符合故障转移了
     * */
    if (server.cluster_slave_validity_factor &&
        data_age >
        (((mstime_t)server.repl_ping_slave_period * 1000) +
         (server.cluster_node_timeout * server.cluster_slave_validity_factor)))
    {
        if (!manual_failover) {
            clusterLogCantFailover(CLUSTER_CANT_FAILOVER_DATA_AGE);
            return;
        }
    }

    /* If the previous failover attempt timeout and the retry time has
     * elapsed, we can setup a new one. 
     *
     * 如果上一次故障转移尝试超时并且重试时间已过，我们可以设置一个新的故障转移尝试。
     * */
    if (auth_age > auth_retry_time) {
        server.cluster->failover_auth_time = mstime() +
            500 + /* Fixed delay of 500 milliseconds, let FAIL msg propagate. 
                   *
                   * 修正了500毫秒的延迟，让FAIL消息传播。
                   * */
            random() % 500; /* Random delay between 0 and 500 milliseconds. 
                             *
                             * 0到500毫秒之间的随机延迟。
                             * */
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_sent = 0;
        server.cluster->failover_auth_rank = clusterGetSlaveRank();
        /* We add another delay that is proportional to the slave rank.
         * Specifically 1 second * rank. This way slaves that have a probably
         * less updated replication offset, are penalized. 
         *
         * 我们添加了另一个与从属等级成比例的延迟。特别是1秒的排名。这样，更新复制偏移量可能较少的从节点就会受到惩罚。
         * */
        server.cluster->failover_auth_time +=
            server.cluster->failover_auth_rank * 1000;
        /* However if this is a manual failover, no delay is needed. 
         *
         * 但是，如果这是手动故障切换，则不需要延迟。
         * */
        if (server.cluster->mf_end) {
            server.cluster->failover_auth_time = mstime();
            server.cluster->failover_auth_rank = 0;
	    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
        serverLog(LL_WARNING,
            "Start of election delayed for %lld milliseconds "
            "(rank #%d, offset %lld).",
            server.cluster->failover_auth_time - mstime(),
            server.cluster->failover_auth_rank,
            replicationGetSlaveOffset());
        /* Now that we have a scheduled election, broadcast our offset
         * to all the other slaves so that they'll updated their offsets
         * if our offset is better. 
         *
         * 既然我们已经安排好了选举，就把我们的补偿广播给所有其他从节点，这样如果我们的偏移更好，他们就会更新他们的偏移。
         * */
        clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
        return;
    }

    /* It is possible that we received more updated offsets from other
     * slaves for the same master since we computed our election delay.
     * Update the delay if our rank changed.
     *
     * Not performed if this is a manual failover. 
     *
     * 由于我们计算了选举延迟，我们可能从同一主节点的其他从节点那里收到了更多更新的偏移。
     * 如果我们的排名发生变化，请更新延迟。如果这是手动故障切换，则不执行。
     * */
    if (server.cluster->failover_auth_sent == 0 &&
        server.cluster->mf_end == 0)
    {
        int newrank = clusterGetSlaveRank();
        if (newrank > server.cluster->failover_auth_rank) {
            long long added_delay =
                (newrank - server.cluster->failover_auth_rank) * 1000;
            server.cluster->failover_auth_time += added_delay;
            server.cluster->failover_auth_rank = newrank;
            serverLog(LL_WARNING,
                "Replica rank updated to #%d, added %lld milliseconds of delay.",
                newrank, added_delay);
        }
    }

    /* Return ASAP if we can't still start the election. 
     *
     * 如果我们还不能开始选举，请尽快返回。
     * */
    if (mstime() < server.cluster->failover_auth_time) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_DELAY);
        return;
    }

    /* Return ASAP if the election is too old to be valid. 
     *
     * 如果选举太旧而无法生效，请尽快返回。
     * */
    if (auth_age > auth_timeout) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_EXPIRED);
        return;
    }

    /* Ask for votes if needed. 
     *
     * 如果需要的话，要求投票。
     * */
    if (server.cluster->failover_auth_sent == 0) {
        server.cluster->currentEpoch++;
        server.cluster->failover_auth_epoch = server.cluster->currentEpoch;
        serverLog(LL_WARNING,"Starting a failover election for epoch %llu.",
            (unsigned long long) server.cluster->currentEpoch);
        clusterRequestFailoverAuth();
        server.cluster->failover_auth_sent = 1;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
        return; /* Wait for replies. 
                 *
                 * 等待答复。
                 * */
    }

    /* Check if we reached the quorum. 
     *
     * 检查我们是否达到法定人数。
     * */
    if (server.cluster->failover_auth_count >= needed_quorum) {
        /* We have the quorum, we can finally failover the master. 
         *
         * 我们有了法定人数，我们终于可以对主节点进行故障切换了。
         * */

        serverLog(LL_WARNING,
            "Failover election won: I'm the new master.");

        /* Update my configEpoch to the epoch of the election. 
         *
         * 将我的configEpoch更新到选举时代。
         * */
        if (myself->configEpoch < server.cluster->failover_auth_epoch) {
            myself->configEpoch = server.cluster->failover_auth_epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu after successful failover",
                (unsigned long long) myself->configEpoch);
        }

        /* Take responsibility for the cluster slots. 
         *
         * 负责集群插槽。
         * */
        clusterFailoverReplaceYourMaster();
    } else {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_VOTES);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER slave migration
 *
 * Slave migration is the process that allows a slave of a master that is
 * already covered by at least another slave, to "migrate" to a master that
 * is orphaned, that is, left with no working slaves.
 *
 * CLUSTER slave migration slave migrate是一个过程，
 * 它允许已经被至少另一个slave覆盖的master的slave“迁移”到一个孤立
 * 的master，也就是说，没有工作的slave
 * */

/* This function is responsible to decide if this replica should be migrated
 * to a different (orphaned) master. It is called by the clusterCron() function
 * only if:
 *
 * 1) We are a slave node.
 * 2) It was detected that there is at least one orphaned master in
 *    the cluster.
 * 3) We are a slave of one of the masters with the greatest number of
 *    slaves.
 *
 * This checks are performed by the caller since it requires to iterate
 * the nodes anyway, so we spend time into clusterHandleSlaveMigration()
 * if definitely needed.
 *
 * The function is called with a pre-computed max_slaves, that is the max
 * number of working (not in FAIL state) slaves for a single master.
 *
 * Additional conditions for migration are examined inside the function.
 
 *
 * 此功能负责决定是否应将此复制副本迁移到另一个（孤立的）主节点。只有当：
 * 1） 我们是从节点时，clusterCron（）函数才会调用它。
 * 2） 检测到群集中至少有一个孤立主节点。
 * 3） 我们是从节点数量最多的主节点之一的从节点。
 * 
 * 这些检查是由调用方执行的，因为它无论如何都需要迭代节点，所以如果确实需要的话，
 * 我们会花时间在clusterHandleSlaveMigration（）中。
 * 使用预先计算的max_slaves调用该函数，即单个主节点的最大工作（不处于FAIL状态）从节点数量。
 * 迁移的附加条件在函数中进行检查。
 * */
void clusterHandleSlaveMigration(int max_slaves) {
    int j, okslaves = 0;
    // mymaster 我目前的主节点
    // target 想要成为target节点的从节点
    // candidate 要进行迁移的候选人
    clusterNode *mymaster = myself->slaveof, *target = NULL, *candidate = NULL;
    dictIterator *di;
    dictEntry *de;

    /* Step 1: Don't migrate if the cluster state is not ok. 
     *
     * 步骤1：如果集群状态不正常，则不要迁移。
     * */
    if (server.cluster->state != CLUSTER_OK) return;

    /* Step 2: Don't migrate if my master will not be left with at least
     *         'migration-barrier' slaves after my migration. 
     *
     * 第二步：如果我的主节点在我迁移后不会留下至少“迁移屏障”从节点，就不要迁移。
     * */
    if (mymaster == NULL) return;
    for (j = 0; j < mymaster->numslaves; j++)
        if (!nodeFailed(mymaster->slaves[j]) &&
            !nodeTimedOut(mymaster->slaves[j])) okslaves++;
    if (okslaves <= server.cluster_migration_barrier) return;

    /* Step 3: Identify a candidate for migration, and check if among the
     * masters with the greatest number of ok slaves, I'm the one with the
     * smallest node ID (the "candidate slave").
     *
     * Note: this means that eventually a replica migration will occur
     * since slaves that are reachable again always have their FAIL flag
     * cleared, so eventually there must be a candidate. At the same time
     * this does not mean that there are no race conditions possible (two
     * slaves migrating at the same time), but this is unlikely to
     * happen, and harmless when happens. 
     *
     * 步骤3：确定迁移的候选者，并检查在ok slave数量最多的master中，我是否是节点ID最小的那个（“候选者slave”）。
     *
     * 注意：这意味着最终会发生副本迁移，因为可以再次访问的从节点总是会清除其FAIL标志，所以最终必须有一个候选者。
     *      同时，这并不意味着不可能有种族条件（两个从节点同时迁移），但这不太可能发生，而且发生时是无害的。
     * */
    candidate = myself;
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int okslaves = 0, is_orphaned = 1;

        /* We want to migrate only if this master is working, orphaned, and
         * used to have slaves or if failed over a master that had slaves
         * (MIGRATE_TO flag). This way we only migrate to instances that were
         * supposed to have replicas. 
         *
         * 只有当该主节点正在工作、成为孤立主节点并且曾经有从属主节点时，或者如果故障转移到有从属
         * 主节点（migrate_to标志）时，我们才希望迁移。通过这种方式，我们只迁移到本应具有副本的节点。
         * */
        if (nodeIsSlave(node) || nodeFailed(node)) is_orphaned = 0;
        if (!(node->flags & CLUSTER_NODE_MIGRATE_TO)) is_orphaned = 0;

        /* Check number of working slaves. 
         *
         * 检查工作从节点的数量。
         * */
        if (nodeIsMaster(node)) okslaves = clusterCountNonFailingSlaves(node);
        if (okslaves > 0) is_orphaned = 0;

        if (is_orphaned) {
            if (!target && node->numslots > 0) target = node;

            /* Track the starting time of the orphaned condition for this
             * master. 
             *
             * 跟踪此主节点形状的孤立条件的开始时间。
             * */
            if (!node->orphaned_time) node->orphaned_time = mstime();
        } else {
            node->orphaned_time = 0;
        }

        /* Check if I'm the slave candidate for the migration: attached
         * to a master with the maximum number of slaves and with the smallest
         * node ID. 
         *
         * 检查我是否是迁移的从属候选者：连接到具有最大从属数量和「最小节点ID」的主节点。
         * 「最小节点ID」指的是名字。
         * */
        if (okslaves == max_slaves) {
            for (j = 0; j < node->numslaves; j++) {
                if (memcmp(node->slaves[j]->name,
                           candidate->name,
                           CLUSTER_NAMELEN) < 0)
                {
                    candidate = node->slaves[j];
                }
            }
        }
    }
    dictReleaseIterator(di);

    /* Step 4: perform the migration if there is a target, and if I'm the
     * candidate, but only if the master is continuously orphaned for a
     * couple of seconds, so that during failovers, we give some time to
     * the natural slaves of this instance to advertise their switch from
     * the old master to the new one. 
     *
     * 步骤4：如果有目标，并且我是候选，则执行迁移，但前提是主节点持续孤立几秒钟，
     * 这样在故障切换期间，我们会给这个节点的自然从节点一些时间，让它们从旧主节点切换到新主节点。
     * */
    if (target && candidate == myself &&
        (mstime()-target->orphaned_time) > CLUSTER_SLAVE_MIGRATION_DELAY &&
       !(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
    {
        serverLog(LL_WARNING,"Migrating to orphaned master %.40s",
            target->name);
        clusterSetMaster(target);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by slaves during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Slave sends a MFSTART message to the master requesting to pause clients
 *    for two times the manual failover timeout CLUSTER_MF_TIMEOUT.
 *    When master is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Slave waits for master to send its replication offset flagged as PAUSED.
 * 4) If slave received the offset from the master, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleSlaveFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force masters to vote for a slave that has a
 *    working master.
 *
 * From the point of view of the master things are simpler: when a
 * PAUSE_CLIENTS packet is received the master sets mf_end as well and
 * the sender in mf_slave. During the time limit for the manual failover
 * the master will just send PINGs more often to this slave, flagged with
 * the PAUSED flag, so that the slave will set mf_master_offset when receiving
 * a packet from the master with this flag set.
 *
 * The gaol of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous master-slave replication.
 * -------------------------------------------------------------------------- 
 *
 * CLUSTER手动故障转移
 * 
 * 这是从节点在手动故障转移过程中执行的重要步骤：
 *
 * 1）用户发送 CLUSTER FAILOVER  命令。故障转移状态已初始化，将mf_end设置为我们
 *    将中止尝试的毫秒unix时间。
 *
 * 2）从节点向主节点发送MFSTART消息，请求暂停客户端两次手动故障转移超时 CLUSTER_MF_TIMEOUT。
 *    当master 暂停进行手动故障转移时，它也开始用 CLUSTERMSG_FLAG0_PAUSED 标记数据包。
 *
 * 3） 从节点等待主节点发送其标记为PAUSED的复制偏移量。
 *
 * 4） 如果slave从master接收到偏移量，并且偏移量匹配，则mf_can_start设置为1，
 *     clusterHandleSlaveFailover（）将照常执行故障转移，不同的是，投票请求将被修改，
 *     以强制master投票给具有工作master的slave。
 *     
 *  在主节点的角度来看，事情更简单：当接收到 PAUSE_CLIENTS 数据包时，
 *  主节点也将mf_end和发送方设置在mf_slave中。在手动故障转移的时间限制期间，
 *  主节点只会更频繁地向该从节点发送带有PAUSED标志的PING，
 *  以便从节点在接收到来自设置了该标志的主节点的数据包时设置mf_master_offset。
 *  
 * 手动故障切换的目标是执行快速故障切换，而不会因异步主从复制而导致数据丢失
 * -------------------------------------------------------------------------
 * */

/* Reset the manual failover state. This works for both masters and slaves
 * as all the state about manual failover is cleared.
 *
 * The function can be used both to initialize the manual failover state at
 * startup or to abort a manual failover in progress. 
 *
 * 重置手动故障切换状态。这对主节点和从节点都有效，因为手动故障转移的所有状态都已清除。
 * 
 * 该功能既可用于在启动时初始化手动故障转移状态，也可用于中止正在进行的手动故障转移。
 * */
void resetManualFailover(void) {
    if (server.cluster->mf_end && clientsArePaused()) {
        server.clients_pause_end_time = 0;

        // 再次调用一次是因为客户端还处理阻塞状态，
        // 但是 server.clients_pause_end_time >  server.mstime
        // 上一句 「server.clients_pause_end_time = 0;」 又刚刚置为0
        clientsArePaused(); /* Just use the side effect of the function. 
                             *
                             * 只需使用函数的副作用。
                             * */
    }
    server.cluster->mf_end = 0; /* No manual failover in progress. 
                                 *
                                 * 没有进行手动故障切换。
                                 * */
    server.cluster->mf_can_start = 0;
    server.cluster->mf_slave = NULL;
    server.cluster->mf_master_offset = 0;
}

/* If a manual failover timed out, abort it. 
 *
 * 如果手动故障转移超时，请中止它。
 * */
void manualFailoverCheckTimeout(void) {
    if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
        serverLog(LL_WARNING,"Manual failover timed out.");
        resetManualFailover();
    }
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. 
 *
 * 该函数是从集群cron函数调用的，以便继续使用手动故障转移状态机。
 * */
void clusterHandleManualFailover(void) {
    /* Return ASAP if no manual failover is in progress. 
     *
     * 如果未进行手动故障转移，请尽快返回。
     * */
    if (server.cluster->mf_end == 0) return;

    /* If mf_can_start is non-zero, the failover was already triggered so the
     * next steps are performed by clusterHandleSlaveFailover(). 
     *
     * 如果mf_can_start为非零，则故障转移已被触发，因此接下来的步骤由clusterHandleSlaveFailover（）执行。
     * */
    if (server.cluster->mf_can_start) return;

    if (server.cluster->mf_master_offset == 0) return; /* Wait for offset... 
                                                        *
                                                        * 等待偏移。。。
                                                        * */

    if (server.cluster->mf_master_offset == replicationGetSlaveOffset()) {
        /* Our replication offset matches the master replication offset
         * announced after clients were paused. We can start the failover. 
         *
         * 我们的复制偏移量与客户端暂停后宣布的主复制偏移量相匹配。我们可以启动故障转移。
         * */
        server.cluster->mf_can_start = 1;
        serverLog(LL_WARNING,
            "All master replication stream processed, "
            "manual failover can start.");
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * --------------------------------------------------------------------------
 * */

/* This is executed 10 times every second 
 *
 * 每秒执行10次，
 * 100ms执行一次
 * */
void clusterCron(void) {
    dictIterator *di;
    dictEntry *de;
    int update_state = 0;
    int orphaned_masters; /* How many masters there are without ok slaves. 
                           *
                           * 有多少主节点没有ok从节点。
                           * */
    int max_slaves; /* Max number of ok slaves for a single master. 
                     *
                     * 单个主节点的ok从节点的最大数量。
                     * */
    int this_slaves; /* Number of ok slaves for our master (if we are slave). 
                      *
                      * 我们主节点的ok从节点数量（如果我们是从节点）。
                      * */
    mstime_t min_pong = 0, now = mstime();
    clusterNode *min_pong_node = NULL;
    static unsigned long long iteration = 0;
    mstime_t handshake_timeout;

    iteration++; /* Number of times this function was called so far. 
                  *
                  * 到目前为止调用此函数的次数。
                  * */

    /* We want to take myself->ip in sync with the cluster-announce-ip option.
     * The option can be set at runtime via CONFIG SET, so we periodically check
     * if the option changed to reflect this into myself->ip. 
     *
     * 我们想让 myself->ip 与 cluster-announce-ip选项同步。
     * 该选项可以在运行时通过 CONFIG  SET 进行设置，
     * 因此我们定期检查该选项是否已更改以将其反映到 myself->ip 中。
     * */
    {
        static char *prev_ip = NULL;
        char *curr_ip = server.cluster_announce_ip;
        int changed = 0;

        if (prev_ip == NULL && curr_ip != NULL) changed = 1;
        else if (prev_ip != NULL && curr_ip == NULL) changed = 1;
        else if (prev_ip && curr_ip && strcmp(prev_ip,curr_ip)) changed = 1;

        if (changed) {
            if (prev_ip) zfree(prev_ip);
            prev_ip = curr_ip;

            if (curr_ip) {
                /* We always take a copy of the previous IP address, by
                 * duplicating the string. This way later we can check if
                 * the address really changed. 
                 *
                 * 我们总是通过复制字符串来复制以前的IP地址。这样以后我们就可以检查地址是否真的变
                 * 了。
                 * */
                prev_ip = zstrdup(prev_ip);
                strncpy(myself->ip,server.cluster_announce_ip,NET_IP_STR_LEN);
                myself->ip[NET_IP_STR_LEN-1] = '\0';
            } else {
                myself->ip[0] = '\0'; /* Force autodetection. 
                                       *
                                       * 强制自动检测。
                                       * */
            }
        }
    }

    /* The handshake timeout is the time after which a handshake node that was
     * not turned into a normal node is removed from the nodes. Usually it is
     * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
     * the value of 1 second. 
     *
     * 握手超时是 「未转换为正常节点的握手节点」 在节点列表中删除的时间。
     * 通常它只是NODE_TIMEOUT值，
     * 但当NODE_TIME OUT太小时，我们使用1秒的值。
     * */
    handshake_timeout = server.cluster_node_timeout;
    if (handshake_timeout < 1000) handshake_timeout = 1000;

    /* Update myself flags. 
     *
     * 更新我自己的标志。
     * */
    // 这里主要是如果手动配置了不能从节点故障转移，就更新 myself->flags 标志
    clusterUpdateMyselfFlags();

    /* Check if we have disconnected nodes and re-establish the connection.
     * Also update a few stats while we are here, that can be used to make
     * better decisions in other part of the code. 
     *
     * 检查是否有断开连接的节点，然后重新建立连接。当我们在这里的时候，还要更新一些统计
     * 数据，这些数据可以用来在代码的其他部分做出更好的决定。
     * */
    di = dictGetSafeIterator(server.cluster->nodes);
    server.cluster->stats_pfail_nodes = 0;
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        /* Not interested in reconnecting the link with myself or nodes
         * for which we have no address. 
         *
         * 对「我自己」或「我们没有地址的节点」不感兴趣。
         * */
        if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR)) continue;

        if (node->flags & CLUSTER_NODE_PFAIL)
            server.cluster->stats_pfail_nodes++;

        /* A Node in HANDSHAKE state has a limited lifespan equal to the
         * configured node timeout. 
         *
         * 处于HANDSHAKE状态的节点，但是握手超时了
         * */
        if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
            clusterDelNode(node);
            continue;
        }

        if (node->link == NULL) {
            clusterLink *link = createClusterLink(node);
            link->conn = server.tls_cluster ? connCreateTLS() : connCreateSocket();
            connSetPrivateData(link->conn, link);
            if (connConnect(link->conn, node->ip, node->cport, NET_FIRST_BIND_ADDR,
                        clusterLinkConnectHandler) == -1) {
                /* We got a synchronous error from connect before
                 * clusterSendPing() had a chance to be called.
                 * If node->ping_sent is zero, failure detection can't work,
                 * so we claim we actually sent a ping now (that will
                 * be really sent as soon as the link is obtained). 
                 *
                 * 在clusterSendPing（）有机会被调用之前，我们从connect中得到
                 * 了一个同步错误。如果node->ping_sent为零，故障检测就无法工作，所以
                 * 我们声称我们现在实际上发送了一个ping（一旦获得链接，就会立即发送）。
                 * */
                if (node->ping_sent == 0) node->ping_sent = mstime();
                serverLog(LL_DEBUG, "Unable to connect to "
                    "Cluster Node [%s]:%d -> %s", node->ip,
                    node->cport, server.neterr);

                freeClusterLink(link);
                continue;
            }
            node->link = link;
        }
    }
    dictReleaseIterator(di);

    /* Ping some random node 1 time every 10 iterations, so that we usually ping
     * one random node every second. 
     *
     * 每10次迭代Ping某个随机节点1次，因此我们通常每秒Ping一个随机节点。
     * */
    if (!(iteration % 10)) {
        int j;

        /* Check a few random nodes and ping the one with the oldest
         * pong_received time. 
         *
         * 检查几个随机节点，并ping具有最早pong_received时间的节点。
         * */
        for (j = 0; j < 5; j++) {
            de = dictGetRandomKey(server.cluster->nodes);
            clusterNode *this = dictGetVal(de);

            /* Don't ping nodes disconnected or with a ping currently active. 
             *
             * 不要ping断开连接的节点或ping当前处于活动状态的节点。
             * */
            if (this->link == NULL || this->ping_sent != 0) continue;
            if (this->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
                continue;
            if (min_pong_node == NULL || min_pong > this->pong_received) {
                min_pong_node = this;
                min_pong = this->pong_received;
            }
        }
        if (min_pong_node) {
            serverLog(LL_DEBUG,"Pinging node %.40s", min_pong_node->name);
            clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
        }
    }

    /* Iterate nodes to check if we need to flag something as failing.
     * This loop is also responsible to:
     * 1) Check if there are orphaned masters (masters without non failing
     *    slaves).
     * 2) Count the max number of non failing slaves for a single master.
     * 3) Count the number of slaves for our master, if we are a slave. 
     *
     * 迭代节点以检查是否需要将某些内容标记为失败。
     * 此循环还负责：
     * 1） 检查是否存在孤立的主节点器（没有非故障从节点的主节点）。
     * 2） 计算单个主节点的最大非故障从节点数量。
     * 3） 如果我们是从节点，请为主节点计算从节点的数量。
     * */
    orphaned_masters = 0;
    max_slaves = 0;
    this_slaves = 0;
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        now = mstime(); /* Use an updated time at every iteration. 
                         *
                         * 在每次迭代中使用更新的时间。
                         * */

        if (node->flags &
            (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE))
                continue;

        /* Orphaned master check, useful only if the current instance
         * is a slave that may migrate to another master. 
         *
         * 孤立主节点检查，仅当 当前节点 是可能迁移到另一个主节点的从节点时才有用。
         * */
        if (nodeIsSlave(myself) && nodeIsMaster(node) && !nodeFailed(node)) {
            int okslaves = clusterCountNonFailingSlaves(node);

            /* A master is orphaned if it is serving a non-zero number of
             * slots, have no working slaves, but used to have at least one
             * slave, or failed over a master that used to have slaves. 
             *
             * 如果主节点为非零数量的插槽提供服务，没有工作的从节点，但曾经至少有一个从节点，或
             * 者故障转移到曾经有从节点的主节点，则该主节点将成为孤立的。
             * */
            if (okslaves == 0 && node->numslots > 0 &&
                node->flags & CLUSTER_NODE_MIGRATE_TO)
            {
                orphaned_masters++;
            }
            if (okslaves > max_slaves) max_slaves = okslaves;
            if (nodeIsSlave(myself) && myself->slaveof == node)
                this_slaves = okslaves;
        }

        /* If we are not receiving any data for more than half the cluster
         * timeout, reconnect the link: maybe there is a connection
         * issue even if the node is alive. 
         *
         * 如果我们在超过一半的集群超时时间内没有接收到任何数据，请重新连接链接：
         * 即使节点处于活动状态，也可能存在连接问题。
         * */
        mstime_t ping_delay = now - node->ping_sent;
        mstime_t data_delay = now - node->data_received;
        if (node->link && /* is connected 
                           *
                           * 已连接
                           * */
            now - node->link->ctime >
            server.cluster_node_timeout && /* was not already reconnected 
                                            *
                                            * 尚未重新连接
                                            * */
            node->ping_sent && /* we already sent a ping 
                                *
                                * 我们已经发送了ping
                                * */
            /* and we are waiting for the pong more than timeout/2 
             *
             * 我们在等待 pong 超过超时/2
             * */
            ping_delay > server.cluster_node_timeout/2 &&
            /* and in such interval we are not seeing any traffic at all. 
             *
             * 在这样的时间间隔内，我们根本看不到任何交通。
             * */
            data_delay > server.cluster_node_timeout/2)
        {
            /* Disconnect the link, it will be reconnected automatically. 
             *
             * 断开链接，它将自动重新连接。
             * */
            freeClusterLink(node->link);
        }

        /* If we have currently no active ping in this instance, and the
         * received PONG is older than half the cluster timeout, send
         * a new ping now, to ensure all the nodes are pinged without
         * a too big delay. 
         *
         * 如果在这种情况下，我们当前没有活动的ping，并且接收到的PONG早于集群超时的
         * 一半，那么现在发送一个新的ping，以确保所有节点都被ping，而不会有太大的延
         * 迟。
         * */
        if (node->link &&
            node->ping_sent == 0 &&
            (now - node->pong_received) > server.cluster_node_timeout/2)
        {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* If we are a master and one of the slaves requested a manual
         * failover, ping it continuously. 
         *
         * 如果我们是一个主节点，并且其中一个从节点请求手动故障转移，则会持续ping它。
         * */
        if (server.cluster->mf_end &&
            nodeIsMaster(myself) &&
            server.cluster->mf_slave == node &&
            node->link)
        {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* Check only if we have an active ping for this instance. 
         *
         * 仅当我们对此节点有活动ping时才进行检查。
         * */
        if (node->ping_sent == 0) continue;

        /* Check if this node looks unreachable.
         * Note that if we already received the PONG, then node->ping_sent
         * is zero, so can't reach this code at all, so we don't risk of
         * checking for a PONG delay if we didn't sent the PING.
         *
         * We also consider every incoming data as proof of liveness, since
         * our cluster bus link is also used for data: under heavy data
         * load pong delays are possible. 
         *
         * 检查此节点是否看起来无法访问。
         *
         * 请注意，如果我们已经收到PONG，那么node->ping_sent为零，因此根本无法到达该代码，
         * 因此如果我们没有发送ping，我们就不会有检查PONG延迟的风险。
         *
         * 我们还将每个传入的数据都视为活跃性的证明，因为我们的集群总线链路也用于数据：在重数据负载下，可能会出现pong延迟。
         * */
        mstime_t node_delay = (ping_delay < data_delay) ? ping_delay :
                                                          data_delay;

        if (node_delay > server.cluster_node_timeout) {
            /* Timeout reached. Set the node as possibly failing if it is
             * not already in this state. 
             *
             * 超时。如果节点尚未处于此状态，则将其设置为可能失败。
             * */
            if (!(node->flags & (CLUSTER_NODE_PFAIL|CLUSTER_NODE_FAIL))) {
                serverLog(LL_DEBUG,"*** NODE %.40s possibly failing",
                    node->name);
                node->flags |= CLUSTER_NODE_PFAIL;
                update_state = 1;
            }
        }
    }
    dictReleaseIterator(di);

    /* If we are a slave node but the replication is still turned off,
     * enable it if we know the address of our master and it appears to
     * be up. 
     *
     * 如果我们是从节点，但复制仍然关闭，
     * 那么如果我们知道主节点的地址并且它似乎已启动，请启用它。
     * */
    if (nodeIsSlave(myself) &&
        server.masterhost == NULL &&
        myself->slaveof &&
        nodeHasAddr(myself->slaveof))
    {
        replicationSetMaster(myself->slaveof->ip, myself->slaveof->port);
    }

    /* Abort a manual failover if the timeout is reached. 
     *
     * 如果达到超时，则中止手动故障转移。
     * */
    manualFailoverCheckTimeout();

    if (nodeIsSlave(myself)) {
        clusterHandleManualFailover();
        if (!(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
            clusterHandleSlaveFailover();
        /* If there are orphaned slaves, and we are a slave among the masters
         * with the max number of non-failing slaves, consider migrating to
         * the orphaned masters. Note that it does not make sense to try
         * a migration if there is no master with at least *two* working
         * slaves. 
         *
         * 如果有孤儿从节点，而我们是拥有最多未失败从节点的主节点中的从节点，请考虑迁移到孤儿主节点。
         * 请注意，如果没有至少有两个工作从节点的主节点，那么尝试迁移是没有意义的。
         * */
        if (orphaned_masters && max_slaves >= 2 && this_slaves == max_slaves)
            clusterHandleSlaveMigration(max_slaves);
    }

    if (update_state || server.cluster->state == CLUSTER_FAIL)
        clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. 
 *
 * 在事件处理程序返回事件睡眠之前调用此函数。执行必须尽快完成的操作以响应触发的事件，
 * 但在事件处理程序内部执行这些操作不安全，或者执行在回复客户端之前需要一次性完成
 * 的潜在扩展任务，这是非常有用的。
 * */
void clusterBeforeSleep(void) {
    /* Handle failover, this is needed when it is likely that there is already
     * the quorum from masters in order to react fast. 
     *
     * 处理故障转移，当可能已经有来自主节点的仲裁以便快速反应时，需要这样做。
     * */
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_HANDLE_FAILOVER)
        clusterHandleSlaveFailover();

    /* Update the cluster state. 
     *
     * 更新群集状态。
     * */
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_UPDATE_STATE)
        clusterUpdateState();

    /* Save the config, possibly using fsync. 
     *
     * 保存配置，可能使用fsync。
     * */
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_SAVE_CONFIG) {
        int fsync = server.cluster->todo_before_sleep &
                    CLUSTER_TODO_FSYNC_CONFIG;
        clusterSaveConfigOrDie(fsync);
    }

    /* Reset our flags (not strictly needed since every single function
     * called for flags set should be able to clear its flag). 
     *
     * 重置我们的标志（不是严格需要的，因为每个为标志集调用的函数都应该能够清除其标志）
     * 。
     * */
    server.cluster->todo_before_sleep = 0;
}

void clusterDoBeforeSleep(int flags) {
    server.cluster->todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * 插槽管理
 * -------------------------------------------------------------------------- */

/* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
 * otherwise 0. 
 *
 * 测试通用位图中的位“pos”。如果位已设置，则返回1，否则返回0。
 * */
int bitmapTestBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    return (bitmap[byte] & (1<<bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. 
 *
 * 将位设置在位图中的位置“pos”。
 * */
void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] |= 1<<bit;
}

/* Clear the bit at position 'pos' in a bitmap. 
 *
 * 清除位图中位置“pos”处的位。
 * */
void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] &= ~(1<<bit);
}

/* Return non-zero if there is at least one master with slaves in the cluster.
 * Otherwise zero is returned. Used by clusterNodeSetSlotBit() to set the
 * MIGRATE_TO flag the when a master gets the first slot. 
 *
 * 如果群集中至少有一个具有从节点的主节点，则返回非零。否则返回零。clusterNodeSetSlotBit（）
 * 用于在主节点获得第一个插槽时设置 MIGRATE_TO 标志。
 * */
int clusterMastersHaveSlaves(void) {
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    int slaves = 0;
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (nodeIsSlave(node)) continue;
        slaves += node->numslaves;
    }
    dictReleaseIterator(di);
    return slaves != 0;
}

/* Set the slot bit and return the old value. 
 *
 * 设置槽位并返回旧值。
 * */
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots,slot);
    bitmapSetBit(n->slots,slot);
    if (!old) {
        n->numslots++;
        /* When a master gets its first slot, even if it has no slaves,
         * it gets flagged with MIGRATE_TO, that is, the master is a valid
         * target for replicas migration, if and only if at least one of
         * the other masters has slaves right now.
         *
         * Normally masters are valid targets of replica migration if:
         * 1. The used to have slaves (but no longer have).
         * 2. They are slaves failing over a master that used to have slaves.
         *
         * However new masters with slots assigned are considered valid
         * migration targets if the rest of the cluster is not a slave-less.
         *
         * See https://github.com/antirez/redis/issues/3043 for more info. 
         *
         * 当一个主节点获得它的第一个插槽时，即使它没有从节点，它也会被标记为MIGRATE_TO，也就是说，
         * 如果并且只有当其他主节点中至少有一个现在有从节点时，该主节点才是副本迁移的有效目标。
         * 通常，如果出现以下情况，则主节点形状成为复制副本迁移的有效目标：1。他
         * 们曾经有从节点（但现在没有了）。2.他们是从节点，辜负了曾经有从节点的主节点。但是，如果
         * 集群的其余部分不是从节点，则分配了插槽的新主节点被视为有效的迁移目标。看见https://github.com/antirez/redis/issues/3043
         * 了解更多信息。
         * */
        if (n->numslots == 1 && clusterMastersHaveSlaves())
            n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    return old;
}

/* Clear the slot bit and return the old value. 
 *
 * 清除槽位并返回旧值。
 * */
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots,slot);
    bitmapClearBit(n->slots,slot);
    if (old) n->numslots--;
    return old;
}

/* Return the slot bit from the cluster node structure. 
 *
 * 从集群节点结构中返回槽位。
 * */
int clusterNodeGetSlotBit(clusterNode *n, int slot) {
    return bitmapTestBit(n->slots,slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return C_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and C_ERR is returned. 
 *
 * 将指定的插槽添加到节点“n”将服务的插槽列表中。如果操作成功结束，则返回C_OK。如果插槽已分配给另一个节点，则视为错误，并返回C_ERR。
 * */
int clusterAddSlot(clusterNode *n, int slot) {
    if (server.cluster->slots[slot]) return C_ERR;
    clusterNodeSetSlotBit(n,slot);
    server.cluster->slots[slot] = n;
    return C_OK;
}

/* Delete the specified slot marking it as unassigned.
 * Returns C_OK if the slot was assigned, otherwise if the slot was
 * already unassigned C_ERR is returned. 
 *
 * 删除将其标记为未分配的指定插槽。如果已分配插槽，则返回C_OK；否则，如果已取消
 * 分配插槽，返回C_ERR。
 * */
int clusterDelSlot(int slot) {
    clusterNode *n = server.cluster->slots[slot];

    if (!n) return C_ERR;
    serverAssert(clusterNodeClearSlotBit(n,slot) == 1);
    server.cluster->slots[slot] = NULL;
    return C_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. 
 *
 * 删除与指定节点关联的所有插槽。将返回已删除插槽的数量。
 * */
int clusterDelNodeSlots(clusterNode *node) {
    int deleted = 0, j;

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeGetSlotBit(node,j)) {
            clusterDelSlot(j);
            deleted++;
        }
    }
    return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a master into slave. 
 *
 * 清除所有插槽的迁移/导入状态。这在初始化和将主节点转换为从节点时非常有用。
 * */
void clusterCloseAllSlots(void) {
    memset(server.cluster->migrating_slots_to,0,
        sizeof(server.cluster->migrating_slots_to));
    memset(server.cluster->importing_slots_from,0,
        sizeof(server.cluster->importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function
 * 集群状态评估功能
 * -------------------------------------------------------------------------- */

/* The following are defines that are only used in the evaluation function
 * and are based on heuristics. Actually the main point about the rejoin and
 * writable delay is that they should be a few orders of magnitude larger
 * than the network latency. 
 *
 * 以下是仅在评估函数中使用的定义，它们基于启发式。实际上，关于重新加入和可写延迟的
 * 要点是，它们应该比网络延迟大几个数量级。
 * */
#define CLUSTER_MAX_REJOIN_DELAY 5000
#define CLUSTER_MIN_REJOIN_DELAY 500
#define CLUSTER_WRITABLE_DELAY 2000

void clusterUpdateState(void) {
    int j, new_state;
    int reachable_masters = 0;
    static mstime_t among_minority_time;
    static mstime_t first_call_time = 0;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

    /* If this is a master node, wait some time before turning the state
     * into OK, since it is not a good idea to rejoin the cluster as a writable
     * master, after a reboot, without giving the cluster a chance to
     * reconfigure this node. Note that the delay is calculated starting from
     * the first call to this function and not since the server start, in order
     * to don't count the DB loading time. 
     *
     * 如果这是一个主节点，请等待一段时间，然后将状态变为OK，因为在重新启动后，以可写
     * 主节点的身份重新加入集群，而不给集群重新配置此节点的机会，这不是一个好主意。请注
     * 意，延迟是从第一次调用该函数开始计算的，而不是从节点启动开始计算的。
     * */
    if (first_call_time == 0) first_call_time = mstime();
    if (nodeIsMaster(myself) &&
        server.cluster->state == CLUSTER_FAIL &&
        mstime() - first_call_time < CLUSTER_WRITABLE_DELAY) return;

    /* Start assuming the state is OK. We'll turn it into FAIL if there
     * are the right conditions. 
     *
     * 开始假设状态正常。如果条件合适，我们将把它变成FAIL。
     * */
    new_state = CLUSTER_OK;

    /* Check if all the slots are covered. 
     *
     * 检查是否覆盖了所有插槽。
     * */
    if (server.cluster_require_full_coverage) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->flags & (CLUSTER_NODE_FAIL))
            {
                new_state = CLUSTER_FAIL;
                break;
            }
        }
    }

    /* Compute the cluster size, that is the number of master nodes
     * serving at least a single slot.
     *
     * At the same time count the number of reachable masters having
     * at least one slot. 
     *
     * 计算集群大小，即至少为单个插槽提供服务的主节点的数量。同时，计算具有至少一个插槽
     * 的可到达主节点的数量。
     * */
    {
        dictIterator *di;
        dictEntry *de;

        server.cluster->size = 0;
        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);

            if (nodeIsMaster(node) && node->numslots) {
                server.cluster->size++;
                if ((node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) == 0)
                    reachable_masters++;
            }
        }
        dictReleaseIterator(di);
    }

    /* If we are in a minority partition, change the cluster state
     * to FAIL. 
     *
     * 如果我们在少数分区中，请将集群状态更改为FAIL。
     * */
    {
        int needed_quorum = (server.cluster->size / 2) + 1;

        if (reachable_masters < needed_quorum) {
            new_state = CLUSTER_FAIL;
            among_minority_time = mstime();
        }
    }

    /* Log a state change 
     *
     * 记录状态更改
     * */
    if (new_state != server.cluster->state) {
        mstime_t rejoin_delay = server.cluster_node_timeout;

        /* If the instance is a master and was partitioned away with the
         * minority, don't let it accept queries for some time after the
         * partition heals, to make sure there is enough time to receive
         * a configuration update. 
         *
         * 如果节点是一个主节点，并且被少数节点分区，那么在分区修复后的一段时间内不要让它接
         * 受查询，以确保有足够的时间接收配置更新。
         * */
        if (rejoin_delay > CLUSTER_MAX_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MAX_REJOIN_DELAY;
        if (rejoin_delay < CLUSTER_MIN_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MIN_REJOIN_DELAY;

        if (new_state == CLUSTER_OK &&
            nodeIsMaster(myself) &&
            mstime() - among_minority_time < rejoin_delay)
        {
            return;
        }

        /* Change the state and log the event. 
         *
         * 更改状态并记录事件。
         * */
        serverLog(LL_WARNING,"Cluster state changed: %s",
            new_state == CLUSTER_OK ? "ok" : "fail");
        server.cluster->state = new_state;
    }
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for, the
 *    following happens:
 *    A) If no other node is in charge according to the current cluster
 *       configuration, we add these slots to our node.
 *    B) If according to our config other nodes are already in charge for
 *       this slots, we set the slots as IMPORTING from our point of view
 *       in order to justify we have those slots, and in order to make
 *       redis-trib aware of the issue, so that it can try to fix it.
 * 2) If we find data in a DB different than DB0 we return C_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns C_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, C_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. 
 *
 * 该函数在节点启动后调用，以验证从磁盘加载的数据是否与群集配置一致：
 *
 * 1）如果我们找到了我们不负责的哈希槽的键，则会发生以下情况：
 *    A）如果根据当前群集配置，没有其他节点负责，则将这些槽添加到我们的节点中。
 *    B）如果根据我们的配置，其他节点已经负责这个插槽，我们从我们的角度将插槽设置为IMPORTING，
 *    以证明我们有这些插槽，并使redis-trib意识到这个问题，以便它可以尝试修复它。
 *
 * 2）如果我们在不同于DB0的数据库中发现数据，我们会返回C_ERR，以向调用方发出信号，
 *    表明它应该退出服务器并返回错误消息或采取其他操作。
 *
 *   函数始终返回C_OK，即使它将尝试更正“1”中描述的错误。但是，如果在不同于DB0的数据库中发现数据，则返回C_ERR。
 *
 *   该函数还使用日志记录功能来警告用户内存中的数据与集群配置之间的不同步。
 * */
int verifyClusterConfigWithData(void) {
    int j;
    int update_config = 0;

    /* Return ASAP if a module disabled cluster redirections. In that case
     * every master can store keys about every possible hash slot. 
     *
     * 如果模块禁用了群集重定向，请尽快返回。在这种情况下，每个主节点都可以存储关于每个可
     * 能的哈希槽的键。
     * */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return C_OK;

    /* If this node is a slave, don't perform the check at all as we
     * completely depend on the replication stream. 
     *
     * 如果该节点是从节点，则根本不执行检查，因为我们完全依赖于复制流。
     * */
    if (nodeIsSlave(myself)) return C_OK;

    /* Make sure we only have keys in DB0. 
     *
     * 请确保我们在DB0中只有键。
     * */
    for (j = 1; j < server.dbnum; j++) {
        if (dictSize(server.db[j].dict)) return C_ERR;
    }

    /* Check that all the slots we see populated memory have a corresponding
     * entry in the cluster table. Otherwise fix the table. 
     *
     * 检查我们看到的所有填充内存的插槽在集群表中是否都有相应的条目。否则，请修复表。
     * */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (!countKeysInSlot(j)) continue; /* No keys in this slot. 
                                            *
                                            * 此插槽中没有键。
                                            * */
        /* Check if we are assigned to this slot or if we are importing it.
         * In both cases check the next slot as the configuration makes
         * sense. 
         *
         * 检查我们是否被分配到这个插槽或是否正在导入它。在这两种情况下，都要检查下一个插槽
         * ，因为配置是合理的。
         * */
        if (server.cluster->slots[j] == myself ||
            server.cluster->importing_slots_from[j] != NULL) continue;

        /* If we are here data and cluster config don't agree, and we have
         * slot 'j' populated even if we are not importing it, nor we are
         * assigned to this slot. Fix this condition. 
         *
         * 如果我们在这里，数据和集群配置不一致，并且我们已经填充了插槽'j'，即使我们没有
         * 导入它，也没有分配给这个插槽。修复此情况。
         * */

        update_config++;
        /* Case A: slot is unassigned. Take responsibility for it. 
         *
         * 情况A：插槽未分配。为此承担责任。
         * */
        if (server.cluster->slots[j] == NULL) {
            serverLog(LL_WARNING, "I have keys for unassigned slot %d. "
                                    "Taking responsibility for it.",j);
            clusterAddSlot(myself,j);
        } else {
            serverLog(LL_WARNING, "I have keys for slot %d, but the slot is "
                                    "assigned to another node. "
                                    "Setting it to importing state.",j);
            server.cluster->importing_slots_from[j] = server.cluster->slots[j];
        }
    }
    if (update_config) clusterSaveConfigOrDie(1);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * SLAVE nodes handling
 * 从属节点处理
 * --------------------------------------------------------------------------*/

/* Set the specified node 'n' as master for this node.
 * If this node is currently a master, it is turned into a slave. 
 *
 * 将指定的节点 n 设置为此节点的主节点。如果 myself 是主节点，则它将变为从属节点。
 * */
void clusterSetMaster(clusterNode *n) {
    serverAssert(n != myself);
    serverAssert(myself->numslots == 0);

    if (nodeIsMaster(myself)) {
        myself->flags &= ~(CLUSTER_NODE_MASTER|CLUSTER_NODE_MIGRATE_TO);
        myself->flags |= CLUSTER_NODE_SLAVE;
        clusterCloseAllSlots();
    } else {
        if (myself->slaveof)
            clusterNodeRemoveSlave(myself->slaveof,myself);
    }
    myself->slaveof = n;
    clusterNodeAddSlave(n,myself);
    replicationSetMaster(n->ip, n->port);
    resetManualFailover();
}

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions.
 * 节点到字符串表示函数
 * -------------------------------------------------------------------------- */

struct redisNodeFlags {
    uint16_t flag;
    char *name;
};

static struct redisNodeFlags redisNodeFlagsTable[] = {
    {CLUSTER_NODE_MYSELF,       "myself,"},
    {CLUSTER_NODE_MASTER,       "master,"},
    {CLUSTER_NODE_SLAVE,        "slave,"},
    {CLUSTER_NODE_PFAIL,        "fail?,"},
    {CLUSTER_NODE_FAIL,         "fail,"},
    {CLUSTER_NODE_HANDSHAKE,    "handshake,"},
    {CLUSTER_NODE_NOADDR,       "noaddr,"},
    {CLUSTER_NODE_NOFAILOVER,   "nofailover,"}
};

/* Concatenate the comma separated list of node flags to the given SDS
 * string 'ci'. 
 *
 * 将 flags 转为 redisNodeFlags 中指定的文字，用逗号隔开串起来返回，如果啥都没就返回 noflags
 * */
sds representClusterNodeFlags(sds ci, uint16_t flags) {
    size_t orig_len = sdslen(ci);
    int i, size = sizeof(redisNodeFlagsTable)/sizeof(struct redisNodeFlags);
    for (i = 0; i < size; i++) {
        struct redisNodeFlags *nodeflag = redisNodeFlagsTable + i;
        if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
    }
    /* If no flag was added, add the "noflags" special flag. 
     *
     * 如果没有添加标志，请添加“noflags”特殊标志。
     * */
    if (sdslen(ci) == orig_len) ci = sdscat(ci,"noflags,");
    sdsIncrLen(ci,-1); /* Remove trailing comma. 
                        *
                        * 删除尾部逗号。
                        * */
    return ci;
}

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * The function returns the string representation as an SDS string. 
 *
 * 生成指定群集节点的类似csv的表示形式。
 * 有关详细信息，请参阅clusterGenNodesDescription（）顶部注释。
 * 函数以SDS字符串的形式返回字符串表示。
 * */
sds clusterGenNodeDescription(clusterNode *node) {
    int j, start;
    sds ci;

    /* Node coordinates 
     *
     * 节点坐标
     * */
    ci = sdscatprintf(sdsempty(),"%.40s %s:%d@%d ",
        node->name,
        node->ip,
        node->port,
        node->cport);

    /* Flags 
     *
     * 标志
     * */
    ci = representClusterNodeFlags(ci, node->flags);

    /* Slave of... or just "-" 
     *
     * …的从节点。。。或者只是“-”
     * */
    if (node->slaveof)
        ci = sdscatprintf(ci," %.40s ",node->slaveof->name);
    else
        ci = sdscatlen(ci," - ",3);

    unsigned long long nodeEpoch = node->configEpoch;
    if (nodeIsSlave(node) && node->slaveof) {
        nodeEpoch = node->slaveof->configEpoch;
    }
    /* Latency from the POV of this node, config epoch, link status 
     *
     * 此节点的POV延迟、配置epoch、链接状态
     * */
    ci = sdscatprintf(ci,"%lld %lld %llu %s",
        (long long) node->ping_sent,
        (long long) node->pong_received,
        nodeEpoch,
        (node->link || node->flags & CLUSTER_NODE_MYSELF) ?
                    "connected" : "disconnected");

    /* Slots served by this instance 
     *
     * 此节点提供的插槽
     * */
    start = -1;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        int bit;

        if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
            if (start == -1) start = j;
        }
        if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
            if (bit && j == CLUSTER_SLOTS-1) j++;

            if (start == j-1) {
                ci = sdscatprintf(ci," %d",start);
            } else {
                ci = sdscatprintf(ci," %d-%d",start,j-1);
            }
            start = -1;
        }
    }

    /* Just for MYSELF node we also dump info about slots that
     * we are migrating to other instances or importing from other
     * instances. 
     *
     * 仅对于MYSELF节点，我们还转储有关正在迁移到其他节点或从其他节点导入的插槽的
     * 信息。
     * */
    if (node->flags & CLUSTER_NODE_MYSELF) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->migrating_slots_to[j]) {
                ci = sdscatprintf(ci," [%d->-%.40s]",j,
                    server.cluster->migrating_slots_to[j]->name);
            } else if (server.cluster->importing_slots_from[j]) {
                ci = sdscatprintf(ci," [%d-<-%.40s]",j,
                    server.cluster->importing_slots_from[j]->name);
            }
        }
    }
    return ci;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. 
 *
 * 生成我们知道的节点的类似csv的表示，包括 "myself" 节点，并返回包含该表示的SDS字符串（由调用方释放）。
 *
 * 与“filter”中指定的至少一个节点标志匹配的所有节点都将从输出中排除，
 * 因此使用零作为过滤器将包括表示中的所有已知节点，包括处于HANDSHAKE状态的节点。
 *
 * 使用此函数获得的表示用于CLUSTER NODES函数的输出，并作为给定节点的集群配置文件（NODES.conf）的格式。
 * */
sds clusterGenNodesDescription(int filter) {
    sds ci = sdsempty(), ni;
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node->flags & filter) continue;
        ni = clusterGenNodeDescription(node);
        ci = sdscatsds(ci,ni);
        sdsfree(ni);
        ci = sdscatlen(ci,"\n",1);
    }
    dictReleaseIterator(di);
    return ci;
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * CLUSTER命令
 * --------------------------------------------------------------------------
 * */

const char *clusterGetMessageTypeString(int type) {
    switch(type) {
    case CLUSTERMSG_TYPE_PING: return "ping";
    case CLUSTERMSG_TYPE_PONG: return "pong";
    case CLUSTERMSG_TYPE_MEET: return "meet";
    case CLUSTERMSG_TYPE_FAIL: return "fail";
    case CLUSTERMSG_TYPE_PUBLISH: return "publish";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST: return "auth-req";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK: return "auth-ack";
    case CLUSTERMSG_TYPE_UPDATE: return "update";
    case CLUSTERMSG_TYPE_MFSTART: return "mfstart";
    case CLUSTERMSG_TYPE_MODULE: return "module";
    }
    return "unknown";
}

int getSlotOrReply(client *c, robj *o) {
    long long slot;

    if (getLongLongFromObject(o,&slot) != C_OK ||
        slot < 0 || slot >= CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

void clusterReplyMultiBulkSlots(client *c) {
    /* Format: 1) 1) start slot
     *            2) end slot
     *            3) 1) master IP
     *               2) master port
     *               3) node ID
     *            4) 1) replica IP
     *               2) replica port
     *               3) node ID
     *           ... continued until done
     * */

    int num_masters = 0;
    void *slot_replylen = addReplyDeferredLen(c);

    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int j = 0, start = -1;
        int i, nested_elements = 0;

        /* Skip slaves (that are iterated when producing the output of their
         * master) and  masters not serving any slot. 
         *
         * 跳过从节点（在产生其主节点的输出时迭代）和不为任何插槽服务的主节点。
         * */
        if (!nodeIsMaster(node) || node->numslots == 0) continue;

        for(i = 0; i < node->numslaves; i++) {
            if (nodeFailed(node->slaves[i])) continue;
            nested_elements++;
        }

        for (j = 0; j < CLUSTER_SLOTS; j++) {
            int bit, i;

            if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
                if (start == -1) start = j;
            }
            if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
                addReplyArrayLen(c, nested_elements + 3); /* slots (2) + master addr (1). 
                                                           *
                                                           * 插槽（2）+主地址（1）。
                                                           * */

                if (bit && j == CLUSTER_SLOTS-1) j++;

                /* If slot exists in output map, add to it's list.
                 * else, create a new output map for this slot 
                 *
                 * 如果插槽存在于输出映射中，请添加到它的列表中。否则，为该插槽创建一个新的输出映射
                 * */
                if (start == j-1) {
                    addReplyLongLong(c, start); /* only one slot; low==high 
                                                 *
                                                 * 只有一个插槽；低==高
                                                 * */
                    addReplyLongLong(c, start);
                } else {
                    addReplyLongLong(c, start); /* low 
                                                 *
                                                 * 低的
                                                 * */
                    addReplyLongLong(c, j-1);   /* high 
                                                 *
                                                 * 高的
                                                 * */
                }
                start = -1;

                /* First node reply position is always the master 
                 *
                 * 第一个节点的回复位置始终为主
                 * */
                addReplyArrayLen(c, 3);
                addReplyBulkCString(c, node->ip);
                addReplyLongLong(c, node->port);
                addReplyBulkCBuffer(c, node->name, CLUSTER_NAMELEN);

                /* Remaining nodes in reply are replicas for slot range 
                 *
                 * 回复中的剩余节点是插槽范围的副本
                 * */
                for (i = 0; i < node->numslaves; i++) {
                    /* This loop is copy/pasted from clusterGenNodeDescription()
                     * with modifications for per-slot node aggregation 
                     *
                     * 此循环是从clusterGenNodeDescription（）复制/粘贴的，并
                     * 对每个槽节点聚合进行了修改
                     * */
                    if (nodeFailed(node->slaves[i])) continue;
                    addReplyArrayLen(c, 3);
                    addReplyBulkCString(c, node->slaves[i]->ip);
                    addReplyLongLong(c, node->slaves[i]->port);
                    addReplyBulkCBuffer(c, node->slaves[i]->name, CLUSTER_NAMELEN);
                }
                num_masters++;
            }
        }
    }
    dictReleaseIterator(di);
    setDeferredArrayLen(c, slot_replylen, num_masters);
}

void clusterCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ADDSLOTS <slot> [slot ...] -- Assign slots to current node.",
"BUMPEPOCH -- Advance the cluster config epoch.",
"COUNT-failure-reports <node-id> -- Return number of failure reports for <node-id>.",
"COUNTKEYSINSLOT <slot> - Return the number of keys in <slot>.",
"DELSLOTS <slot> [slot ...] -- Delete slots information from current node.",
"FAILOVER [force|takeover] -- Promote current replica node to being a master.",
"FORGET <node-id> -- Remove a node from the cluster.",
"GETKEYSINSLOT <slot> <count> -- Return key names stored by current node in a slot.",
"FLUSHSLOTS -- Delete current node own slots information.",
"INFO - Return information about the cluster.",
"KEYSLOT <key> -- Return the hash slot for <key>.",
"MEET <ip> <port> [bus-port] -- Connect nodes into a working cluster.",
"MYID -- Return the node id.",
"NODES -- Return cluster configuration seen by node. Output format:",
"    <id> <ip:port> <flags> <master> <pings> <pongs> <epoch> <link> <slot> ... <slot>",
"REPLICATE <node-id> -- Configure current node as replica to <node-id>.",
"RESET [hard|soft] -- Reset current node (default: soft).",
"SET-config-epoch <epoch> - Set config epoch of current node.",
"SETSLOT <slot> (importing|migrating|stable|node <node-id>) -- Set slot state.",
"REPLICAS <node-id> -- Return <node-id> replicas.",
"SAVECONFIG - Force saving cluster configuration on disk.",
"SLOTS -- Return information about slots range mappings. Each range is made of:",
"    start, end, master and replicas IP addresses, ports and ids",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"meet") && (c->argc == 4 || c->argc == 5)) {
        /* CLUSTER MEET <ip> <port> [cport] 
         *
         * CLUSTER MEET<ip><port>[cport]
         * */
        long long port, cport;

        if (getLongLongFromObject(c->argv[3], &port) != C_OK) {
            addReplyErrorFormat(c,"Invalid TCP base port specified: %s",
                                (char*)c->argv[3]->ptr);
            return;
        }

        if (c->argc == 5) {
            if (getLongLongFromObject(c->argv[4], &cport) != C_OK) {
                addReplyErrorFormat(c,"Invalid TCP bus port specified: %s",
                                    (char*)c->argv[4]->ptr);
                return;
            }
        } else {
            cport = port + CLUSTER_PORT_INCR;
        }

        if (clusterStartHandshake(c->argv[2]->ptr,port,cport) == 0 &&
            errno == EINVAL)
        {
            addReplyErrorFormat(c,"Invalid node address specified: %s:%s",
                            (char*)c->argv[2]->ptr, (char*)c->argv[3]->ptr);
        } else {
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"nodes") && c->argc == 2) {
        /* CLUSTER NODES 
         *
         * 群集节点
         * */
        sds nodes = clusterGenNodesDescription(0);
        addReplyVerbatim(c,nodes,sdslen(nodes),"txt");
        sdsfree(nodes);
    } else if (!strcasecmp(c->argv[1]->ptr,"myid") && c->argc == 2) {
        /* CLUSTER MYID 
         *
         * 集群MYID
         * */
        addReplyBulkCBuffer(c,myself->name, CLUSTER_NAMELEN);
    } else if (!strcasecmp(c->argv[1]->ptr,"slots") && c->argc == 2) {
        /* CLUSTER SLOTS 
         *
         * 群集插槽
         * */
        clusterReplyMultiBulkSlots(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"flushslots") && c->argc == 2) {
        /* CLUSTER FLUSHSLOTS 
         *
         * 群集flushslot
         * */
        if (dictSize(server.db[0].dict) != 0) {
            addReplyError(c,"DB must be empty to perform CLUSTER FLUSHSLOTS.");
            return;
        }
        clusterDelNodeSlots(myself);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr,"addslots") ||
               !strcasecmp(c->argv[1]->ptr,"delslots")) && c->argc >= 3)
    {
        /* CLUSTER ADDSLOTS <slot> [slot] ... */
        /* CLUSTER DELSLOTS <slot> [slot] ... */
        int j, slot;
        unsigned char *slots = zmalloc(CLUSTER_SLOTS);
        int del = !strcasecmp(c->argv[1]->ptr,"delslots");

        memset(slots,0,CLUSTER_SLOTS);
        /* Check that all the arguments are parseable and that all the
         * slots are not already busy. 
         *
         * 检查所有的参数是否都是可解析的，并且所有的插槽是否都已繁忙。
         * */
        for (j = 2; j < c->argc; j++) {
            if ((slot = getSlotOrReply(c,c->argv[j])) == -1) {
                zfree(slots);
                return;
            }
            if (del && server.cluster->slots[slot] == NULL) {
                addReplyErrorFormat(c,"Slot %d is already unassigned", slot);
                zfree(slots);
                return;
            } else if (!del && server.cluster->slots[slot]) {
                addReplyErrorFormat(c,"Slot %d is already busy", slot);
                zfree(slots);
                return;
            }
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",
                    (int)slot);
                zfree(slots);
                return;
            }
        }
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (slots[j]) {
                int retval;

                /* If this slot was set as importing we can clear this
                 * state as now we are the real owner of the slot. 
                 *
                 * 如果这个插槽被设置为导入，我们可以清除这个状态，因为现在我们是插槽的真正所有者。
                 * */
                if (server.cluster->importing_slots_from[j])
                    server.cluster->importing_slots_from[j] = NULL;

                retval = del ? clusterDelSlot(j) :
                               clusterAddSlot(myself,j);
                serverAssertWithInfo(c,NULL,retval == C_OK);
            }
        }
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"setslot") && c->argc >= 4) {
        /* SETSLOT 10 MIGRATING <node ID> */
        /* SETSLOT 10 IMPORTING <node ID> */
        /* SETSLOT 10 STABLE */
        /* SETSLOT 10 NODE <node ID> */
        int slot;
        clusterNode *n;

        if (nodeIsSlave(myself)) {
            addReplyError(c,"Please use SETSLOT only with masters.");
            return;
        }

        if ((slot = getSlotOrReply(c,c->argv[2])) == -1) return;

        if (!strcasecmp(c->argv[3]->ptr,"migrating") && c->argc == 5) {
            if (server.cluster->slots[slot] != myself) {
                addReplyErrorFormat(c,"I'm not the owner of hash slot %u",slot);
                return;
            }
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            server.cluster->migrating_slots_to[slot] = n;
        } else if (!strcasecmp(c->argv[3]->ptr,"importing") && c->argc == 5) {
            if (server.cluster->slots[slot] == myself) {
                addReplyErrorFormat(c,
                    "I'm already the owner of hash slot %u",slot);
                return;
            }
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            server.cluster->importing_slots_from[slot] = n;
        } else if (!strcasecmp(c->argv[3]->ptr,"stable") && c->argc == 4) {
            /* CLUSTER SETSLOT <SLOT> STABLE */
            server.cluster->importing_slots_from[slot] = NULL;
            server.cluster->migrating_slots_to[slot] = NULL;
        } else if (!strcasecmp(c->argv[3]->ptr,"node") && c->argc == 5) {
            /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
            clusterNode *n = clusterLookupNode(c->argv[4]->ptr);

            if (!n) {
                addReplyErrorFormat(c,"Unknown node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            /* If this hash slot was served by 'myself' before to switch
             * make sure there are no longer local keys for this hash slot. 
             *
             * 如果此哈希槽在切换之前由“我自己”提供服务，请确保此哈希槽不再有本地键。
             * */
            if (server.cluster->slots[slot] == myself && n != myself) {
                if (countKeysInSlot(slot) != 0) {
                    addReplyErrorFormat(c,
                        "Can't assign hashslot %d to a different node "
                        "while I still hold keys for this hash slot.", slot);
                    return;
                }
            }
            /* If this slot is in migrating status but we have no keys
             * for it assigning the slot to another node will clear
             * the migrating status. 
             *
             * 如果此插槽处于迁移状态，但我们没有用于它的键，则将插槽分配给另一个节点将清除迁
             * 移状态。
             * */
            if (countKeysInSlot(slot) == 0 &&
                server.cluster->migrating_slots_to[slot])
                server.cluster->migrating_slots_to[slot] = NULL;

            /* If this node was importing this slot, assigning the slot to
             * itself also clears the importing status. 
             *
             * 如果此节点正在导入此插槽，则将插槽分配给其自身也会清除导入状态。
             * */
            if (n == myself &&
                server.cluster->importing_slots_from[slot])
            {
                /* This slot was manually migrated, set this node configEpoch
                 * to a new epoch so that the new version can be propagated
                 * by the cluster.
                 *
                 * Note that if this ever results in a collision with another
                 * node getting the same configEpoch, for example because a
                 * failover happens at the same time we close the slot, the
                 * configEpoch collision resolution will fix it assigning
                 * a different epoch to each node. 
                 *
                 * 此插槽是手动迁移的，请将此节点configEpoch设置为新的epoch，以便集
                 * 群可以传播新版本。请注意，如果这导致与获得相同configEpoch的另一个节点
                 * 发生冲突，例如，因为故障转移发生在我们关闭插槽的同时，configEpoch冲突
                 * 解决方案将修复为每个节点分配不同epoch的问题。
                 * */
                if (clusterBumpConfigEpochWithoutConsensus() == C_OK) {
                    serverLog(LL_WARNING,
                        "configEpoch updated after importing slot %d", slot);
                }
                server.cluster->importing_slots_from[slot] = NULL;
            }
            clusterDelSlot(slot);
            clusterAddSlot(n,slot);
        } else {
            addReplyError(c,
                "Invalid CLUSTER SETSLOT action or number of arguments. Try CLUSTER HELP");
            return;
        }
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_UPDATE_STATE);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"bumpepoch") && c->argc == 2) {
        /* CLUSTER BUMPEPOCH 
         *
         * 集束炸弹
         * */
        int retval = clusterBumpConfigEpochWithoutConsensus();
        sds reply = sdscatprintf(sdsempty(),"+%s %llu\r\n",
                (retval == C_OK) ? "BUMPED" : "STILL",
                (unsigned long long) myself->configEpoch);
        addReplySds(c,reply);
    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLUSTER INFO 
         *
         * 群集信息
         * */
        char *statestr[] = {"ok","fail","needhelp"};
        int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
        uint64_t myepoch;
        int j;

        for (j = 0; j < CLUSTER_SLOTS; j++) {
            clusterNode *n = server.cluster->slots[j];

            if (n == NULL) continue;
            slots_assigned++;
            if (nodeFailed(n)) {
                slots_fail++;
            } else if (nodeTimedOut(n)) {
                slots_pfail++;
            } else {
                slots_ok++;
            }
        }

        myepoch = (nodeIsSlave(myself) && myself->slaveof) ?
                  myself->slaveof->configEpoch : myself->configEpoch;

        sds info = sdscatprintf(sdsempty(),
            "cluster_state:%s\r\n"
            "cluster_slots_assigned:%d\r\n"
            "cluster_slots_ok:%d\r\n"
            "cluster_slots_pfail:%d\r\n"
            "cluster_slots_fail:%d\r\n"
            "cluster_known_nodes:%lu\r\n"
            "cluster_size:%d\r\n"
            "cluster_current_epoch:%llu\r\n"
            "cluster_my_epoch:%llu\r\n"
            , statestr[server.cluster->state],
            slots_assigned,
            slots_ok,
            slots_pfail,
            slots_fail,
            dictSize(server.cluster->nodes),
            server.cluster->size,
            (unsigned long long) server.cluster->currentEpoch,
            (unsigned long long) myepoch
        );

        /* Show stats about messages sent and received. 
         *
         * 显示有关已发送和已接收邮件的统计信息。
         * */
        long long tot_msg_sent = 0;
        long long tot_msg_received = 0;

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->stats_bus_messages_sent[i] == 0) continue;
            tot_msg_sent += server.cluster->stats_bus_messages_sent[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_sent:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->stats_bus_messages_sent[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_sent:%lld\r\n", tot_msg_sent);

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->stats_bus_messages_received[i] == 0) continue;
            tot_msg_received += server.cluster->stats_bus_messages_received[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_received:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->stats_bus_messages_received[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_received:%lld\r\n", tot_msg_received);

        /* Produce the reply protocol. 
         *
         * 生成回复协议。
         * */
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(info);
    } else if (!strcasecmp(c->argv[1]->ptr,"saveconfig") && c->argc == 2) {
        int retval = clusterSaveConfig(1);

        if (retval == 0)
            addReply(c,shared.ok);
        else
            addReplyErrorFormat(c,"error saving the cluster node config: %s",
                strerror(errno));
    } else if (!strcasecmp(c->argv[1]->ptr,"keyslot") && c->argc == 3) {
        /* CLUSTER KEYSLOT <key> 
         *
         * 集群键槽<key>
         * */
        sds key = c->argv[2]->ptr;

        addReplyLongLong(c,keyHashSlot(key,sdslen(key)));
    } else if (!strcasecmp(c->argv[1]->ptr,"countkeysinslot") && c->argc == 3) {
        /* CLUSTER COUNTKEYSINSLOT <slot> 
         *
         * CLUSTER COUNTKEYSINSLOT<slot>
         * */
        long long slot;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS) {
            addReplyError(c,"Invalid slot");
            return;
        }
        addReplyLongLong(c,countKeysInSlot(slot));
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeysinslot") && c->argc == 4) {
        /* CLUSTER GETKEYSINSLOT <slot> <count> 
         *
         * 集群GETKEYSINSLOT＜slot＞＜count＞
         * */
        long long maxkeys, slot;
        unsigned int numkeys, j;
        robj **keys;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (getLongLongFromObjectOrReply(c,c->argv[3],&maxkeys,NULL)
            != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS || maxkeys < 0) {
            addReplyError(c,"Invalid slot or number of keys");
            return;
        }

        /* Avoid allocating more than needed in case of large COUNT argument
         * and smaller actual number of keys. 
         *
         * 在COUNT参数较大而实际键数较少的情况下，避免分配超出所需的数量。
         * */
        unsigned int keys_in_slot = countKeysInSlot(slot);
        if (maxkeys > keys_in_slot) maxkeys = keys_in_slot;

        keys = zmalloc(sizeof(robj*)*maxkeys);
        numkeys = getKeysInSlot(slot, keys, maxkeys);
        addReplyArrayLen(c,numkeys);
        for (j = 0; j < numkeys; j++) {
            addReplyBulk(c,keys[j]);
            decrRefCount(keys[j]);
        }
        zfree(keys);
    } else if (!strcasecmp(c->argv[1]->ptr,"forget") && c->argc == 3) {
        /* CLUSTER FORGET <NODE ID> 
         *
         * 集群遗忘＜NODE ID＞
         * */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else if (n == myself) {
            addReplyError(c,"I tried hard but I can't forget myself...");
            return;
        } else if (nodeIsSlave(myself) && myself->slaveof == n) {
            addReplyError(c,"Can't forget my master!");
            return;
        }
        clusterBlacklistAddNode(n);
        clusterDelNode(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"replicate") && c->argc == 3) {
        /* CLUSTER REPLICATE <NODE ID> 
         *
         * 集群复制＜NODE ID＞
         * */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        /* Lookup the specified node in our table. 
         *
         * 在我们的表中查找指定的节点。
         * */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        /* I can't replicate myself. 
         *
         * 我无法复制自己。
         * */
        if (n == myself) {
            addReplyError(c,"Can't replicate myself");
            return;
        }

        /* Can't replicate a slave. 
         *
         * 无法复制从节点。
         * */
        if (nodeIsSlave(n)) {
            addReplyError(c,"I can only replicate a master, not a replica.");
            return;
        }

        /* If the instance is currently a master, it should have no assigned
         * slots nor keys to accept to replicate some other node.
         * Slaves can switch to another master without issues. 
         *
         * 如果节点当前是主节点，那么它应该没有分配的插槽或键来接受以复制其他节点。从属服
         * 务器可以毫无问题地切换到另一个主节点。
         * */
        if (nodeIsMaster(myself) &&
            (myself->numslots != 0 || dictSize(server.db[0].dict) != 0)) {
            addReplyError(c,
                "To set a master the node must be empty and "
                "without assigned slots.");
            return;
        }

        /* Set the master. 
         *
         * 设置主节点。
         * */
        clusterSetMaster(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr,"slaves") ||
                !strcasecmp(c->argv[1]->ptr,"replicas")) && c->argc == 3) {
        /* CLUSTER SLAVES <NODE ID> 
         *
         * 集群从属＜NODE ID＞
         * */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
        int j;

        /* Lookup the specified node in our table. 
         *
         * 在我们的表中查找指定的节点。
         * */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        if (nodeIsSlave(n)) {
            addReplyError(c,"The specified node is not a master");
            return;
        }

        addReplyArrayLen(c,n->numslaves);
        for (j = 0; j < n->numslaves; j++) {
            sds ni = clusterGenNodeDescription(n->slaves[j]);
            addReplyBulkCString(c,ni);
            sdsfree(ni);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"count-failure-reports") &&
               c->argc == 3)
    {
        /* CLUSTER COUNT-FAILURE-REPORTS <NODE ID> 
         *
         * 集群计数失败报告<NODE-ID>
         * */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else {
            addReplyLongLong(c,clusterNodeFailureReportsCount(n));
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"failover") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER FAILOVER [FORCE|TAKEOVER] */
        int force = 0, takeover = 0;

        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"force")) {
                force = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"takeover")) {
                takeover = 1;
                force = 1; /* Takeover also implies force. 
                            *
                            * 接管也意味着武力。
                            * */
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Check preconditions. 
         *
         * 检查先决条件。
         * */
        if (nodeIsMaster(myself)) {
            addReplyError(c,"You should send CLUSTER FAILOVER to a replica");
            return;
        } else if (myself->slaveof == NULL) {
            addReplyError(c,"I'm a replica but my master is unknown to me");
            return;
        } else if (!force &&
                   (nodeFailed(myself->slaveof) ||
                    myself->slaveof->link == NULL))
        {
            addReplyError(c,"Master is down or failed, "
                            "please use CLUSTER FAILOVER FORCE");
            return;
        }
        resetManualFailover();
        server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;

        if (takeover) {
            /* A takeover does not perform any initial check. It just
             * generates a new configuration epoch for this node without
             * consensus, claims the master's slots, and broadcast the new
             * configuration. 
             *
             * 接管不执行任何初始检查。它只是在没有达成共识的情况下为该节点生成一个新的配置ep
             * och，声明主节点的插槽，并广播新配置。
             * */
            serverLog(LL_WARNING,"Taking over the master (user request).");
            clusterBumpConfigEpochWithoutConsensus();
            clusterFailoverReplaceYourMaster();
        } else if (force) {
            /* If this is a forced failover, we don't need to talk with our
             * master to agree about the offset. We just failover taking over
             * it without coordination. 
             *
             * 如果这是一个强制故障转移，我们不需要与我们的主节点讨论偏移量。我们只是在没有协调的
             * 情况下进行故障切换。
             * */
            serverLog(LL_WARNING,"Forced failover user request accepted.");
            server.cluster->mf_can_start = 1;
        } else {
            serverLog(LL_WARNING,"Manual failover user request accepted.");
            clusterSendMFStart(myself->slaveof);
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"set-config-epoch") && c->argc == 3)
    {
        /* CLUSTER SET-CONFIG-EPOCH <epoch>
         *
         * The user is allowed to set the config epoch only when a node is
         * totally fresh: no config epoch, no other known node, and so forth.
         * This happens at cluster creation time to start with a cluster where
         * every node has a different node ID, without to rely on the conflicts
         * resolution system which is too slow when a big cluster is created. 
         *
         * CLUSTER SET-CONFIG-EPOCH＜EPOCH＞只有当一个节点完全
         * 新鲜时，才允许用户设置配置EPOCH：没有配置EPOCH，没有其他已知节点，等等。
         * 这发生在集群创建时，从每个节点都有不同节点ID的集群开始，
         * 而不必依赖冲突解决系统，因为在创建大型集群时，冲突解决系统太慢。
         * */
        long long epoch;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&epoch,NULL) != C_OK)
            return;

        if (epoch < 0) {
            addReplyErrorFormat(c,"Invalid config epoch specified: %lld",epoch);
        } else if (dictSize(server.cluster->nodes) > 1) {
            addReplyError(c,"The user can assign a config epoch only when the "
                            "node does not know any other node.");
        } else if (myself->configEpoch != 0) {
            addReplyError(c,"Node config epoch is already non-zero");
        } else {
            myself->configEpoch = epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
                (unsigned long long) myself->configEpoch);

            if (server.cluster->currentEpoch < (uint64_t)epoch)
                server.cluster->currentEpoch = epoch;
            /* No need to fsync the config here since in the unlucky event
             * of a failure to persist the config, the conflict resolution code
             * will assign a unique config to this node. 
             *
             * 无需在此处对配置进行fsync，因为在未能持久化配置的不幸事件中，
             * 冲突解决代码将为该节点分配一个唯一的配置。
             * */
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                                 CLUSTER_TODO_SAVE_CONFIG);
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"reset") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER RESET [SOFT|HARD] */
        int hard = 0;

        /* Parse soft/hard argument. Default is soft. 
         *
         * 分析软/硬参数。默认为软。
         * */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"hard")) {
                hard = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"soft")) {
                hard = 0;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Slaves can be reset while containing data, but not master nodes
         * that must be empty. 
         *
         * 包含数据时可以重置从节点，但不能重置必须为空的主节点。
         * */
        if (nodeIsMaster(myself) && dictSize(c->db->dict) != 0) {
            addReplyError(c,"CLUSTER RESET can't be called with "
                            "master nodes containing keys");
            return;
        }
        clusterReset(hard);
        addReply(c,shared.ok);
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * DUMP、RESTORE和MIGRATE命令
 * --------------------------------------------------------------------------
 * */

/* Generates a DUMP-format representation of the object 'o', adding it to the
 * io stream pointed by 'rio'. This function can't fail. 
 *
 * 生成对象“o”的DUMP格式表示，并将其添加到“rio”指向的io流中。这个功能不会失败。
 * */
void createDumpPayload(rio *payload, robj *o, robj *key) {
    unsigned char buf[2];
    uint64_t crc;

    /* Serialize the object in an RDB-like format. It consist of an object type
     * byte followed by the serialized object. This is understood by RESTORE. 
     *
     * 以类似RDB的格式序列化对象。它由对象类型字节和序列化对象组成。RESTORE可以理解这一点。
     * */
    rioInitWithBuffer(payload,sdsempty());
    serverAssert(rdbSaveObjectType(payload,o));
    serverAssert(rdbSaveObject(payload,o,key));

    /* Write the footer, this is how it looks like:
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     */

    /* RDB version 
     *
     * RDB版本
     * */
    buf[0] = RDB_VERSION & 0xff;
    buf[1] = (RDB_VERSION >> 8) & 0xff;
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,buf,2);

    /* CRC64 */
    crc = crc64(0,(unsigned char*)payload->io.buffer.ptr,
                sdslen(payload->io.buffer.ptr));
    memrev64ifbe(&crc);
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,&crc,8);
}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 * If the DUMP payload looks valid C_OK is returned, otherwise C_ERR
 * is returned. 
 *
 * 验证转储负载的RDB版本是否与此Redis节点的版本匹配，以及校验和是否正常。如
 * 果dump负载看起来有效，则返回C_ok，否则返回C_ERR。
 * */
int verifyDumpPayload(unsigned char *p, size_t len) {
    unsigned char *footer;
    uint16_t rdbver;
    uint64_t crc;

    /* At least 2 bytes of RDB version and 8 of CRC64 should be present. 
     *
     * RDB版本应至少有2个字节，CRC64应至少有8个字节。
     * */
    if (len < 10) return C_ERR;
    footer = p+(len-10);

    /* Verify RDB version 
     *
     * 验证RDB版本
     * */
    rdbver = (footer[1] << 8) | footer[0];
    if (rdbver > RDB_VERSION) return C_ERR;

    /* Verify CRC64 
     *
     * 验证CRC64
     * */
    crc = crc64(0,p,len-8);
    memrev64ifbe(&crc);
    return (memcmp(&crc,footer+2,8) == 0) ? C_OK : C_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. 
 *
 * DUMP keyname DUMP实际上没有被Redis Cluster使用，但
 * 它是RESTORE的明显补充，可以用于不同的应用程序。
 * */
void dumpCommand(client *c) {
    robj *o;
    rio payload;

    /* Check if the key is here. 
     *
     * 检查键是否在这里。
     * */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReplyNull(c);
        return;
    }

    /* Create the DUMP encoded representation. 
     *
     * 创建DUMP编码的表示。
     * */
    createDumpPayload(&payload,o,c->argv[1]);

    /* Transfer to the client 
     *
     * 转移到客户
     * */
    addReplyBulkSds(c,payload.io.buffer.ptr);
    return;
}

/* RESTORE key ttl serialized-value [REPLACE] 
 *
 * RESTORE键ttl序列化值〔REPLACE〕
 * */
void restoreCommand(client *c) {
    long long ttl, lfu_freq = -1, lru_idle = -1, lru_clock = -1;
    rio payload;
    int j, type, replace = 0, absttl = 0;
    robj *obj;

    /* Parse additional options 
     *
     * 分析其他选项
     * */
    for (j = 4; j < c->argc; j++) {
        int additional = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"absttl")) {
            absttl = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"idletime") && additional >= 1 &&
                   lfu_freq == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lru_idle,NULL)
                    != C_OK) return;
            if (lru_idle < 0) {
                addReplyError(c,"Invalid IDLETIME value, must be >= 0");
                return;
            }
            lru_clock = LRU_CLOCK();
            j++; /* Consume additional arg. 
                  *
                  * 消耗额外的arg。
                  * */
        } else if (!strcasecmp(c->argv[j]->ptr,"freq") && additional >= 1 &&
                   lru_idle == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lfu_freq,NULL)
                    != C_OK) return;
            if (lfu_freq < 0 || lfu_freq > 255) {
                addReplyError(c,"Invalid FREQ value, must be >= 0 and <= 255");
                return;
            }
            j++; /* Consume additional arg. 
                  *
                  * 消耗额外的arg。
                  * */
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Make sure this key does not already exist here... 
     *
     * 请确保此处不存在此键。。。
     * */
    robj *key = c->argv[1];
    if (!replace && lookupKeyWrite(c->db,key) != NULL) {
        addReply(c,shared.busykeyerr);
        return;
    }

    /* Check if the TTL value makes sense 
     *
     * 检查TTL值是否合理
     * */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != C_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* Verify RDB version and data checksum. 
     *
     * 验证RDB版本和数据校验和。
     * */
    if (verifyDumpPayload(c->argv[3]->ptr,sdslen(c->argv[3]->ptr)) == C_ERR)
    {
        addReplyError(c,"DUMP payload version or checksum are wrong");
        return;
    }

    rioInitWithBuffer(&payload,c->argv[3]->ptr);
    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((obj = rdbLoadObject(type,&payload,key->ptr)) == NULL))
    {
        addReplyError(c,"Bad data format");
        return;
    }

    /* Remove the old key if needed. 
     *
     * 如果需要，取下旧键。
     * */
    int deleted = 0;
    if (replace)
        deleted = dbDelete(c->db,key);

    if (ttl && !absttl) ttl+=mstime();
    if (ttl && checkAlreadyExpired(ttl)) {
        if (deleted) {
            rewriteClientCommandVector(c,2,shared.del,key);
            signalModifiedKey(c,c->db,key);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
            server.dirty++;
        }
        decrRefCount(obj);
        addReply(c, shared.ok);
        return;
    }

    /* Create the key and set the TTL if any 
     *
     * 创建键并设置TTL（如果有的话）
     * */
    dbAdd(c->db,key,obj);
    if (ttl) {
        setExpire(c,c->db,key,ttl);
    }
    objectSetLRUOrLFU(obj,lfu_freq,lru_idle,lru_clock,1000);
    signalModifiedKey(c,c->db,key);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"restore",key,c->db->id);
    addReply(c,shared.ok);
    server.dirty++;
}

/* MIGRATE socket cache implementation.
 *
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. 
 *
 * MIGRATE套接字缓存实现。我们在host:ip和TCP套接字之间绘制了一个映射，
 * 最近我们用来连接到这个节点。当达到我们缓存的最大数量时，这些套接字就会关闭，
 * 当它们存在超过几秒钟时，serverCron（）中也会关闭。
 * */
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. 
                                       *
                                       * 缓存中的最大项目数。
                                       * */
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached sockets after 10 sec. 
                                     *
                                     * 10秒后关闭缓存的套接字。
                                     * */

typedef struct migrateCachedSocket {
    connection *conn;
    long last_dbid;
    time_t last_use_time;
} migrateCachedSocket;

/* Return a migrateCachedSocket containing a TCP socket connected with the
 * target instance, possibly returning a cached one.
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be created from scratch
 * the next time. 
 *
 * 返回一个migrateCachedSocket，其中包含一个与目标节点连接的TCP套接字，
 * 可能会返回一个缓存的套接字。如果无法建立连接，此函数负责向客户端发送错误。
 * 在这种情况下，返回-1。否则，一旦成功，就会返回套接字，调用方不应在使用后尝试释放它。
 * 如果调用方在使用套接字时检测到错误，则应调用migrateCloseSocket()，以便下次从头开始创建连接。
 * */
migrateCachedSocket* migrateGetSocket(client *c, robj *host, robj *port, long timeout) {
    connection *conn;
    sds name = sdsempty();
    migrateCachedSocket *cs;

    /* Check if we have an already cached socket for this ip:port pair. 
     *
     * 检查我们是否已经为这个ip:port对缓存了套接字。
     * */
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (cs) {
        sdsfree(name);
        cs->last_use_time = server.unixtime;
        return cs;
    }

    /* No cached socket, create one. 
     *
     * 没有缓存套接字，请创建一个。
     * */
    if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
        /* Too many items, drop one at random. 
         *
         * 条目太多，请随意丢弃一个。
         * */
        dictEntry *de = dictGetRandomKey(server.migrate_cached_sockets);
        cs = dictGetVal(de);
        connClose(cs->conn);
        zfree(cs);
        dictDelete(server.migrate_cached_sockets,dictGetKey(de));
    }

    /* Create the socket 
     *
     * 创建套接字
     * */
    conn = server.tls_cluster ? connCreateTLS() : connCreateSocket();
    if (connBlockingConnect(conn, c->argv[1]->ptr, atoi(c->argv[2]->ptr), timeout)
            != C_OK) {
        addReplySds(c,
            sdsnew("-IOERR error or timeout connecting to the client\r\n"));
        connClose(conn);
        sdsfree(name);
        return NULL;
    }
    connEnableTcpNoDelay(conn);

    /* Add to the cache and return it to the caller. 
     *
     * 添加到缓存并将其返回给调用方。
     * */
    cs = zmalloc(sizeof(*cs));
    cs->conn = conn;

    cs->last_dbid = -1;
    cs->last_use_time = server.unixtime;
    dictAdd(server.migrate_cached_sockets,name,cs);
    return cs;
}

/* Free a migrate cached connection. 
 *
 * 释放迁移缓存的连接。
 * */
void migrateCloseSocket(robj *host, robj *port) {
    sds name = sdsempty();
    migrateCachedSocket *cs;

    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (!cs) {
        sdsfree(name);
        return;
    }

    connClose(cs->conn);
    zfree(cs);
    dictDelete(server.migrate_cached_sockets,name);
    sdsfree(name);
}

void migrateCloseTimedoutSockets(void) {
    dictIterator *di = dictGetSafeIterator(server.migrate_cached_sockets);
    dictEntry *de;

    while((de = dictNext(di)) != NULL) {
        migrateCachedSocket *cs = dictGetVal(de);

        if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
            connClose(cs->conn);
            zfree(cs);
            dictDelete(server.migrate_cached_sockets,dictGetKey(de));
        }
    }
    dictReleaseIterator(di);
}

/* MIGRATE host port key dbid timeout [COPY | REPLACE | AUTH password |
 *         AUTH2 username password]
 *
 * On in the multiple keys form:
 *
 * MIGRATE host port "" dbid timeout [COPY | REPLACE | AUTH password |
 *         AUTH2 username password] KEYS key1 key2 ... keyN */
void migrateCommand(client *c) {
    migrateCachedSocket *cs;
    int copy = 0, replace = 0, j;
    char *username = NULL;
    char *password = NULL;
    long timeout;
    long dbid;
    robj **ov = NULL; /* Objects to migrate. 
                       *
                       * 要迁移的对象。
                       * */
    robj **kv = NULL; /* Key names. 
                       *
                       * 键名称。
                       * */
    robj **newargv = NULL; /* Used to rewrite the command as DEL ... keys ... 
                            *
                            * 用于将命令重写为DEL。。。键。。。
                            * */
    rio cmd, payload;
    int may_retry = 1;
    int write_error = 0;
    int argv_rewritten = 0;

    /* To support the KEYS option we need the following additional state. 
     *
     * 为了支持KEYS选项，我们需要以下附加状态。
     * */
    int first_key = 3; /* Argument index of the first key. 
                        *
                        * 第一个键的参数索引。
                        * */
    int num_keys = 1;  /* By default only migrate the 'key' argument. 
                        *
                        * 默认情况下，仅迁移“key”参数。
                        * */

    /* Parse additional options 
     *
     * 分析其他选项
     * */
    for (j = 6; j < c->argc; j++) {
        int moreargs = (c->argc-1) - j;
        if (!strcasecmp(c->argv[j]->ptr,"copy")) {
            copy = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"auth")) {
            if (!moreargs) {
                addReply(c,shared.syntaxerr);
                return;
            }
            j++;
            password = c->argv[j]->ptr;
        } else if (!strcasecmp(c->argv[j]->ptr,"auth2")) {
            if (moreargs < 2) {
                addReply(c,shared.syntaxerr);
                return;
            }
            username = c->argv[++j]->ptr;
            password = c->argv[++j]->ptr;
        } else if (!strcasecmp(c->argv[j]->ptr,"keys")) {
            if (sdslen(c->argv[3]->ptr) != 0) {
                addReplyError(c,
                    "When using MIGRATE KEYS option, the key argument"
                    " must be set to the empty string");
                return;
            }
            first_key = j+1;
            num_keys = c->argc - j - 1;
            break; /* All the remaining args are keys. 
                    *
                    * 剩下的所有参数都是键。
                    * */
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Sanity check 
     *
     * 卫生检查
     * */
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != C_OK ||
        getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != C_OK)
    {
        return;
    }
    if (timeout <= 0) timeout = 1000;

    /* Check if the keys are here. If at least one key is to migrate, do it
     * otherwise if all the keys are missing reply with "NOKEY" to signal
     * the caller there was nothing to migrate. We don't return an error in
     * this case, since often this is due to a normal condition like the key
     * expiring in the meantime. 
     *
     * 检查键是否在这里。如果至少有一个键要迁移，否则，如果所有键都丢失，则使用“
     * NOKEY”回复，以向调用方发出无需迁移的信号。在这种情况下，我们不会返回错误，
     * 因为这通常是由于正常情况造成的，比如键在此期间过期。
     * */
    ov = zrealloc(ov,sizeof(robj*)*num_keys);
    kv = zrealloc(kv,sizeof(robj*)*num_keys);
    int oi = 0;

    for (j = 0; j < num_keys; j++) {
        if ((ov[oi] = lookupKeyRead(c->db,c->argv[first_key+j])) != NULL) {
            kv[oi] = c->argv[first_key+j];
            oi++;
        }
    }
    num_keys = oi;
    if (num_keys == 0) {
        zfree(ov); zfree(kv);
        addReplySds(c,sdsnew("+NOKEY\r\n"));
        return;
    }

try_again:
    write_error = 0;

    /* Connect 
     *
     * 连接
     * */
    cs = migrateGetSocket(c,c->argv[1],c->argv[2],timeout);
    if (cs == NULL) {
        zfree(ov); zfree(kv);
        return; /* error sent to the client by migrateGetSocket() 
                 *
                 * migrateGetSocket（）发送到客户端的错误
                 * */
    }

    rioInitWithBuffer(&cmd,sdsempty());

    /* Authentication 
     *
     * 身份验证
     * */
    if (password) {
        int arity = username ? 3 : 2;
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',arity));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"AUTH",4));
        if (username) {
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,username,
                                 sdslen(username)));
        }
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,password,
            sdslen(password)));
    }

    /* Send the SELECT command if the current DB is not already selected. 
     *
     * 如果尚未选择当前数据库，则发送SELECT命令。
     * */
    int select = cs->last_dbid != dbid; /* Should we emit SELECT? 
                                         *
                                         * 我们应该发出SELECT吗？
                                         * */
    if (select) {
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',2));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"SELECT",6));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,dbid));
    }

    int non_expired = 0; /* Number of keys that we'll find non expired.
                            Note that serializing large keys may take some time
                            so certain keys that were found non expired by the
                            lookupKey() function, may be expired later. 
                          *
                          * 我们将发现未过期的键数。请注意，序列化大键可能需要一些时间，因此lookupKey（）
                          * 函数发现未过期的某些键可能会在以后过期。
                          * */

    /* Create RESTORE payload and generate the protocol to call the command. 
     *
     * 创建RESTORE负载并生成用于调用该命令的协议。
     * */
    for (j = 0; j < num_keys; j++) {
        long long ttl = 0;
        long long expireat = getExpire(c->db,kv[j]);

        if (expireat != -1) {
            ttl = expireat-mstime();
            if (ttl < 0) {
                continue;
            }
            if (ttl < 1) ttl = 1;
        }

        /* Relocate valid (non expired) keys into the array in successive
         * positions to remove holes created by the keys that were present
         * in the first lookup but are now expired after the second lookup. 
         *
         * 将有效（未过期）键重新定位到阵列中的连续位置，以移除由第一次查找中存在但在第二
         * 次查找后过期的键创建的孔。
         * */
        kv[non_expired++] = kv[j];

        serverAssertWithInfo(c,NULL,
            rioWriteBulkCount(&cmd,'*',replace ? 5 : 4));

        if (server.cluster_enabled)
            serverAssertWithInfo(c,NULL,
                rioWriteBulkString(&cmd,"RESTORE-ASKING",14));
        else
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"RESTORE",7));
        serverAssertWithInfo(c,NULL,sdsEncodedObject(kv[j]));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,kv[j]->ptr,
                sdslen(kv[j]->ptr)));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,ttl));

        /* Emit the payload argument, that is the serialized object using
         * the DUMP format. 
         *
         * 发出有效负载参数，即使用DUMP格式的序列化对象。
         * */
        createDumpPayload(&payload,ov[j],kv[j]);
        serverAssertWithInfo(c,NULL,
            rioWriteBulkString(&cmd,payload.io.buffer.ptr,
                               sdslen(payload.io.buffer.ptr)));
        sdsfree(payload.io.buffer.ptr);

        /* Add the REPLACE option to the RESTORE command if it was specified
         * as a MIGRATE option. 
         *
         * 如果REPLACE选项被指定为MIGRATE选项，请将其添加到RESTORE命令
         * 中。
         * */
        if (replace)
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"REPLACE",7));
    }

    /* Fix the actual number of keys we are migrating. 
     *
     * 修复我们正在迁移的键的实际数量。
     * */
    num_keys = non_expired;

    /* Transfer the query to the other node in 64K chunks. 
     *
     * 将查询以64K块的形式传输到另一个节点。
     * */
    errno = 0;
    {
        sds buf = cmd.io.buffer.ptr;
        size_t pos = 0, towrite;
        int nwritten = 0;

        while ((towrite = sdslen(buf)-pos) > 0) {
            towrite = (towrite > (64*1024) ? (64*1024) : towrite);
            nwritten = connSyncWrite(cs->conn,buf+pos,towrite,timeout);
            if (nwritten != (signed)towrite) {
                write_error = 1;
                goto socket_err;
            }
            pos += nwritten;
        }
    }

    char buf0[1024]; /* Auth reply. 
                      *
                      * 授权回复。
                      * */
    char buf1[1024]; /* Select reply. 
                      *
                      * 选择回复。
                      * */
    char buf2[1024]; /* Restore reply. 
                      *
                      * 恢复回复。
                      * */

    /* Read the AUTH reply if needed. 
     *
     * 如果需要，请阅读AUTH回复。
     * */
    if (password && connSyncReadLine(cs->conn, buf0, sizeof(buf0), timeout) <= 0)
        goto socket_err;

    /* Read the SELECT reply if needed. 
     *
     * 如果需要，请阅读SELECT回复。
     * */
    if (select && connSyncReadLine(cs->conn, buf1, sizeof(buf1), timeout) <= 0)
        goto socket_err;

    /* Read the RESTORE replies. 
     *
     * 读取RESTORE回复。
     * */
    int error_from_target = 0;
    int socket_error = 0;
    int del_idx = 1; /* Index of the key argument for the replicated DEL op. 
                      *
                      * 复制的DEL操作的键参数的索引。
                      * */

    /* Allocate the new argument vector that will replace the current command,
     * to propagate the MIGRATE as a DEL command (if no COPY option was given).
     * We allocate num_keys+1 because the additional argument is for "DEL"
     * command name itself. 
     *
     * 分配将替换当前命令的新参数向量，以将MIGRATE作为DEL命令传播（如果没有提
     * 供COPY选项）。我们分配num_keys+1是因为额外的参数用于“DEL”命令
     * 名本身。
     * */
    if (!copy) newargv = zmalloc(sizeof(robj*)*(num_keys+1));

    for (j = 0; j < num_keys; j++) {
        if (connSyncReadLine(cs->conn, buf2, sizeof(buf2), timeout) <= 0) {
            socket_error = 1;
            break;
        }
        if ((password && buf0[0] == '-') ||
            (select && buf1[0] == '-') ||
            buf2[0] == '-')
        {
            /* On error assume that last_dbid is no longer valid. 
             *
             * 出现错误时，假设last_dbid不再有效。
             * */
            if (!error_from_target) {
                cs->last_dbid = -1;
                char *errbuf;
                if (password && buf0[0] == '-') errbuf = buf0;
                else if (select && buf1[0] == '-') errbuf = buf1;
                else errbuf = buf2;

                error_from_target = 1;
                addReplyErrorFormat(c,"Target instance replied with error: %s",
                    errbuf+1);
            }
        } else {
            if (!copy) {
                /* No COPY option: remove the local key, signal the change. 
                 *
                 * 无复制选项：删除本地键，发出更改信号。
                 * */
                dbDelete(c->db,kv[j]);
                signalModifiedKey(c,c->db,kv[j]);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",kv[j],c->db->id);
                server.dirty++;

                /* Populate the argument vector to replace the old one. 
                 *
                 * 填充参数向量以替换旧的参数向量。
                 * */
                newargv[del_idx++] = kv[j];
                incrRefCount(kv[j]);
            }
        }
    }

    /* On socket error, if we want to retry, do it now before rewriting the
     * command vector. We only retry if we are sure nothing was processed
     * and we failed to read the first reply (j == 0 test). 
     *
     * 对于套接字错误，如果我们想重试，请在重写命令向量之前立即重试。只有当我们确信没有
     * 处理任何内容，并且未能读取第一个回复（j==0测试）时，我们才会重试。
     * */
    if (!error_from_target && socket_error && j == 0 && may_retry &&
        errno != ETIMEDOUT)
    {
        goto socket_err; /* A retry is guaranteed because of tested conditions.
                          *
                          * 由于测试条件，保证重试。
                          * */
    }

    /* On socket errors, close the migration socket now that we still have
     * the original host/port in the ARGV. Later the original command may be
     * rewritten to DEL and will be too later. 
     *
     * 关于套接字错误，现在ARGV中仍有原始主节点/端口，请关闭迁移套接字。稍后，原始命
     * 令可能被重写为DEL，并且将太晚。
     * */
    if (socket_error) migrateCloseSocket(c->argv[1],c->argv[2]);

    if (!copy) {
        /* Translate MIGRATE as DEL for replication/AOF. Note that we do
         * this only for the keys for which we received an acknowledgement
         * from the receiving Redis server, by using the del_idx index. 
         *
         * 将MIGRATE转换为复制/AOF的DEL。请注意，我们只对从接收Redis服务
         * 器收到确认的键执行此操作，方法是使用del_idx索引。
         * */
        if (del_idx > 1) {
            newargv[0] = createStringObject("DEL",3);
            /* Note that the following call takes ownership of newargv. 
             *
             * 请注意，以下调用获取newargv的所有权。
             * */
            replaceClientCommandVector(c,del_idx,newargv);
            argv_rewritten = 1;
        } else {
            /* No key transfer acknowledged, no need to rewrite as DEL. 
             *
             * 未确认键传输，无需重写为DEL。
             * */
            zfree(newargv);
        }
        newargv = NULL; /* Make it safe to call zfree() on it in the future. 
                         *
                         * 以后可以安全地对其调用zfree（）。
                         * */
    }

    /* If we are here and a socket error happened, we don't want to retry.
     * Just signal the problem to the client, but only do it if we did not
     * already queue a different error reported by the destination server. 
     *
     * 如果我们在这里，并且发生了套接字错误，我们不想重试。只需向客户端发出问题信号，但
     * 前提是我们还没有对目标服务器报告的不同错误进行排队。
     * */
    if (!error_from_target && socket_error) {
        may_retry = 0;
        goto socket_err;
    }

    if (!error_from_target) {
        /* Success! Update the last_dbid in migrateCachedSocket, so that we can
         * avoid SELECT the next time if the target DB is the same. Reply +OK.
         *
         * Note: If we reached this point, even if socket_error is true
         * still the SELECT command succeeded (otherwise the code jumps to
         * socket_err label. 
         *
         * 成功更新migrateCachedSocket中的last_dbid，这样，如果
         * 目标数据库相同，我们就可以避免下次选择。回复+确定。注意：如果我们达到了这一点，
         * 即使socket_error为true，SELECT命令仍然成功（否则代码跳到socket_err标签。
         * */
        cs->last_dbid = dbid;
        addReply(c,shared.ok);
    } else {
        /* On error we already sent it in the for loop above, and set
         * the currently selected socket to -1 to force SELECT the next time. 
         *
         * 在出现错误时，我们已经在上面的for循环中发送了它，并将当前选择的套接字设置为-1，以便下次强制SELECT。
         * */
    }

    sdsfree(cmd.io.buffer.ptr);
    zfree(ov); zfree(kv); zfree(newargv);
    return;

/* On socket errors we try to close the cached socket and try again.
 * It is very common for the cached socket to get closed, if just reopening
 * it works it's a shame to notify the error to the caller. 
 *
 * 在出现套接字错误时，我们会尝试关闭缓存的套接字，然后重试。缓存的套接字关闭是很常
 * 见的，如果只是重新打开它就可以了，那么将错误通知给调用者是很遗憾的。
 * */
socket_err:
    /* Cleanup we want to perform in both the retry and no retry case.
     * Note: Closing the migrate socket will also force SELECT next time. 
     *
     * 我们希望在重试和不重试的情况下都执行清理。注意：关闭迁移套接字也将在下次强制执行
     * SELECT。
     * */
    sdsfree(cmd.io.buffer.ptr);

    /* If the command was rewritten as DEL and there was a socket error,
     * we already closed the socket earlier. While migrateCloseSocket()
     * is idempotent, the host/port arguments are now gone, so don't do it
     * again. 
     *
     * 如果命令被重写为DEL，并且出现了套接字错误，那么我们已经提前关闭了套接字。虽然
     * migrateCloseSocket（）是幂等的，但主节点/端口参数现在已经不存在了，所以不要再这样做了。
     * */
    if (!argv_rewritten) migrateCloseSocket(c->argv[1],c->argv[2]);
    zfree(newargv);
    newargv = NULL; /* This will get reallocated on retry. 
                     *
                     * 重试时将重新分配。
                     * */

    /* Retry only if it's not a timeout and we never attempted a retry
     * (or the code jumping here did not set may_retry to zero). 
     *
     * 只有当它不是超时并且我们从未尝试过重试时才重试（或者跳到这里的代码没有将may_Retry设置为零）。
     * */
    if (errno != ETIMEDOUT && may_retry) {
        may_retry = 0;
        goto try_again;
    }

    /* Cleanup we want to do if no retry is attempted. 
     *
     * 如果没有尝试重试，我们要进行清理。
     * */
    zfree(ov); zfree(kv);
    addReplySds(c,
        sdscatprintf(sdsempty(),
            "-IOERR error or timeout %s to target instance\r\n",
            write_error ? "writing" : "reading"));
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * 与服务/重定向客户端相关的群集功能
 * --------------------------------------------------------------------------*/

/* The ASKING command is required after a -ASK redirection.
 * The client should issue ASKING before to actually send the command to
 * the target instance. See the Redis Cluster specification for more
 * information. 
 *
 * ASKING命令在-ASK重定向之后是必需的。客户端应该在实际向目标节点发送命令
 * 之前发出ASKING。有关详细信息，请参阅Redis Cluster规范。
 * */
void askingCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_ASKING;
    addReply(c,shared.ok);
}

/* The READONLY command is used by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. 
 *
 * READONLY命令用于客户端进入只读模式。在这种模式下，只要客户端使用只读命令
 * 访问从属主节点提供的键，从属主节点就不会重定向客户端。
 * */
void readonlyCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* The READWRITE command just clears the READONLY command state. 
 *
 * READWRITE命令只是清除READONLY命令状态。
 * */
void readwriteCommand(client *c) {
    c->flags &= ~CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like LPOPRPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be performed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to CLUSTER_REDIR_ASK or
 * CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * CLUSTER_REDIR_UNSTABLE if the request contains multiple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress).
 *
 * CLUSTER_REDIR_DOWN_UNBOUND if the request addresses a slot which is
 * not bound to any node. In this case the cluster global state should be
 * already "down" but it is fragile to rely on the update of the global state,
 * so we also handle it here.
 *
 * CLUSTER_REDIR_DOWN_STATE and CLUSTER_REDIR_DOWN_RO_STATE if the cluster is
 * down but the user attempts to execute a command that addresses one or more keys. */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *error_code) {
    clusterNode *n = NULL;
    robj *firstkey = NULL;
    int multiple_keys = 0;
    multiState *ms, _ms;
    multiCmd mc;
    int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0;

    /* Allow any key to be set if a module disabled cluster redirections. 
     *
     * 如果模块禁用了群集重定向，则允许设置任何键。
     * */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return myself;

    /* Set error code optimistically for the base case. 
     *
     * 为基本情况乐观地设置错误代码。
     * */
    if (error_code) *error_code = CLUSTER_REDIR_NONE;

    /* Modules can turn off Redis Cluster redirection: this is useful
     * when writing a module that implements a completely different
     * distributed system. 
     *
     * 模块可以关闭Redis集群重定向：这在编写实现完全不同分布式系统的模块时很有用。
     * */

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything 
     *
     * 我们处理所有的情况，就好像它们是EXEC命令一样，所以我们有一个通用的代码路径
     * */
    if (cmd->proc == execCommand) {
        /* If CLIENT_MULTI flag is not set EXEC is just going to return an
         * error. 
         *
         * 如果CLIENT_MULTI标志未设置，EXEC只会返回一个错误。
         * */
        if (!(c->flags & CLIENT_MULTI)) return myself;
        ms = &c->mstate;
    } else {
        /* In order to have a single codepath create a fake Multi State
         * structure if the client is not in MULTI/EXEC state, this way
         * we have a single codepath below. 
         *
         * 如果客户端不处于Multi/EXEC状态，为了让单个代码路径创建一个伪多状态结构，我们在下面有一个单独的代码路径。
         * */
        ms = &_ms;
        _ms.commands = &mc;
        _ms.count = 1;
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
    }

    /* Check that all the keys are in the same hash slot, and obtain this
     * slot and the node associated. 
     *
     * 检查所有键是否都在同一个哈希槽中，并获取该槽和关联的节点。
     * */
    for (i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, *keyindex, numkeys, j;

        mcmd = ms->commands[i].cmd;
        margc = ms->commands[i].argc;
        margv = ms->commands[i].argv;

        getKeysResult result = GETKEYS_RESULT_INIT;
        numkeys = getKeysFromCommand(mcmd,margv,margc,&result);
        keyindex = result.keys;

        for (j = 0; j < numkeys; j++) {
            robj *thiskey = margv[keyindex[j]];
            int thisslot = keyHashSlot((char*)thiskey->ptr,
                                       sdslen(thiskey->ptr));

            if (firstkey == NULL) {
                /* This is the first key we see. Check what is the slot
                 * and node. 
                 *
                 * 这是我们看到的第一把键。检查插槽和节点是什么。
                 * */
                firstkey = thiskey;
                slot = thisslot;
                n = server.cluster->slots[slot];

                /* Error: If a slot is not served, we are in "cluster down"
                 * state. However the state is yet to be updated, so this was
                 * not trapped earlier in processCommand(). Report the same
                 * error to the client. 
                 *
                 * 错误：如果没有提供插槽，则我们处于“集群关闭”状态。然而，该状态尚未更新，因此这
                 * 在processCommand（）中没有被捕获。向客户端报告相同的错误。
                 * */
                if (n == NULL) {
                    getKeysFreeResult(&result);
                    if (error_code)
                        *error_code = CLUSTER_REDIR_DOWN_UNBOUND;
                    return NULL;
                }

                /* If we are migrating or importing this slot, we need to check
                 * if we have all the keys in the request (the only way we
                 * can safely serve the request, otherwise we return a TRYAGAIN
                 * error). To do so we set the importing/migrating state and
                 * increment a counter for every missing key. 
                 *
                 * 如果我们正在迁移或导入此插槽，我们需要检查请求中是否包含所有键（这是我们可以安
                 * 全地为请求提供服务的唯一方法，否则我们将返回TRYAGAIN错误）。为此，我们设
                 * 置导入/迁移状态，并为每个丢失的键增加一个计数器。
                 * */
                if (n == myself &&
                    server.cluster->migrating_slots_to[slot] != NULL)
                {
                    migrating_slot = 1;
                } else if (server.cluster->importing_slots_from[slot] != NULL) {
                    importing_slot = 1;
                }
            } else {
                /* If it is not the first key, make sure it is exactly
                 * the same key as the first we saw. 
                 *
                 * 如果它不是第一把键，请确保它与我们看到的第一把键完全相同。
                 * */
                if (!equalStringObjects(firstkey,thiskey)) {
                    if (slot != thisslot) {
                        /* Error: multiple keys from different slots. 
                         *
                         * 错误：来自不同插槽的多个键。
                         * */
                        getKeysFreeResult(&result);
                        if (error_code)
                            *error_code = CLUSTER_REDIR_CROSS_SLOT;
                        return NULL;
                    } else {
                        /* Flag this request as one with multiple different
                         * keys. 
                         *
                         * 将此请求标记为具有多个不同键的请求。
                         * */
                        multiple_keys = 1;
                    }
                }
            }

            /* Migrating / Importing slot? Count keys we don't have. 
             *
             * 正在迁移/导入插槽？数数我们没有的键。
             * */
            if ((migrating_slot || importing_slot) &&
                lookupKeyRead(&server.db[0],thiskey) == NULL)
            {
                missing_keys++;
            }
        }
        getKeysFreeResult(&result);
    }

    /* No key at all in command? then we can serve the request
     * without redirections or errors in all the cases. 
     *
     * 根本没有键在指挥？那么在所有情况下，我们都可以在没有重定向或错误的情况下提供请求。
     * */
    if (n == NULL) return myself;

    /* Cluster is globally down but we got keys? We only serve the request
     * if it is a read command and when allow_reads_when_down is enabled. 
     *
     * 集群全局关闭，但我们有键？只有当请求是读取命令并且启用了allow_reads
     * _when_down时，我们才会提供该请求。
     * */
    if (server.cluster->state != CLUSTER_OK) {
        if (!server.cluster_allow_reads_when_down) {
            /* The cluster is configured to block commands when the
             * cluster is down. 
             *
             * 集群配置为在集群关闭时阻止命令。
             * */
            if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
            return NULL;
        } else if (!(cmd->flags & CMD_READONLY) && !(cmd->proc == evalCommand)
                && !(cmd->proc == evalShaCommand))
        {
            /* The cluster is configured to allow read only commands
             * but this command is neither readonly, nor EVAL or
             * EVALSHA. 
             *
             * 集群配置为允许只读命令，但此命令既不是只读的，也不是EVAL或EVALSHA。
             * */
            if (error_code) *error_code = CLUSTER_REDIR_DOWN_RO_STATE;
            return NULL;
        } else {
            /* Fall through and allow the command to be executed:
             * this happens when server.cluster_allow_reads_when_down is
             * true and the command is a readonly command or EVAL / EVALSHA. 
             *
             * 失败并允许执行该命令：当server.cluster_allow_reads_w
             * hen_down为true并且该命令是只读命令或EVAL/EVALSHA时，就会
             * 发生这种情况。
             * */
        }
    }

    /* Return the hashslot by reference. 
     *
     * 通过引用返回hashslot。
     * */
    if (hashslot) *hashslot = slot;

    /* MIGRATE always works in the context of the local node if the slot
     * is open (migrating or importing state). We need to be able to freely
     * move keys among instances in this case. 
     *
     * 如果插槽处于打开状态（迁移或导入状态），则MIGRATE始终在本地节点的上下文中
     * 工作。在这种情况下，我们需要能够在节点之间自由移动键。
     * */
    if ((migrating_slot || importing_slot) && cmd->proc == migrateCommand)
        return myself;

    /* If we don't have all the keys and we are migrating the slot, send
     * an ASK redirection. 
     *
     * 如果我们没有所有的键，并且正在迁移插槽，请发送ASK重定向。
     * */
    if (migrating_slot && missing_keys) {
        if (error_code) *error_code = CLUSTER_REDIR_ASK;
        return server.cluster->migrating_slots_to[slot];
    }

    /* If we are receiving the slot, and the client correctly flagged the
     * request as "ASKING", we can serve the request. However if the request
     * involves multiple keys and we don't have them all, the only option is
     * to send a TRYAGAIN error. 
     *
     * 如果我们正在接收插槽，并且客户端正确地将请求标记为“ASKING”，我们就可以为
     * 该请求提供服务。但是，如果请求涉及多个键，而我们没有全部键，那么唯一的选择就
     * 是发送TRYAGAIN错误。
     * */
    if (importing_slot &&
        (c->flags & CLIENT_ASKING || cmd->flags & CMD_ASKING))
    {
        if (multiple_keys && missing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            return myself;
        }
    }

    /* Handle the read-only client case reading from a slave: if this
     * node is a slave and the request is about a hash slot our master
     * is serving, we can reply without redirection. 
     *
     * 处理从从节点读取的只读客户端案例：如果这个节点是从节点，并且请求是关于我们的
     * 主节点正在服务的哈希槽的，我们可以在不重定向的情况下进行回复。
     * */
    int is_readonly_command = (c->cmd->flags & CMD_READONLY) ||
                              (c->cmd->proc == execCommand && !(c->mstate.cmd_inv_flags & CMD_READONLY));
    if (c->flags & CLIENT_READONLY &&
        (is_readonly_command || cmd->proc == evalCommand ||
         cmd->proc == evalShaCommand) &&
        nodeIsSlave(myself) &&
        myself->slaveof == n)
    {
        return myself;
    }

    /* Base case: just return the right node. However if this node is not
     * myself, set error_code to MOVED since we need to issue a redirection. */
    if (n != myself && error_code) *error_code = CLUSTER_REDIR_MOVED;
    return n;
}

/* Send the client the right redirection code, according to error_code
 * that should be set to one of CLUSTER_REDIR_* macros.
 *
 * If CLUSTER_REDIR_ASK or CLUSTER_REDIR_MOVED error codes
 * are used, then the node 'n' should not be NULL, but should be the
 * node we want to mention in the redirection. Moreover hashslot should
 * be set to the hash slot that caused the redirection. */
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code) {
    if (error_code == CLUSTER_REDIR_CROSS_SLOT) {
        addReplySds(c,sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_UNSTABLE) {
        /* The request spawns multiple keys in the same slot,
         * but the slot is not "stable" currently as there is
         * a migration or import in progress. 
         *
         * 该请求在同一个插槽中生成多个键，但该插槽当前不“稳定”，因为正在进行迁移或导入
         * 。
         * */
        addReplySds(c,sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
        addReplySds(c,sdsnew("-CLUSTERDOWN The cluster is down\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_RO_STATE) {
        addReplySds(c,sdsnew("-CLUSTERDOWN The cluster is down and only accepts read commands\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_UNBOUND) {
        addReplySds(c,sdsnew("-CLUSTERDOWN Hash slot not served\r\n"));
    } else if (error_code == CLUSTER_REDIR_MOVED ||
               error_code == CLUSTER_REDIR_ASK)
    {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-%s %d %s:%d\r\n",
            (error_code == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
            hashslot,n->ip,n->port));
    } else {
        serverPanic("getNodeByQuery() unknown error.");
    }
}

/* This function is called by the function processing clients incrementally
 * to detect timeouts, in order to handle the following case:
 *
 * 1) A client blocks with BLPOP or similar blocking operation.
 * 2) The master migrates the hash slot elsewhere or turns into a slave.
 * 3) The client may remain blocked forever (or up to the max timeout time)
 *    waiting for a key change that will never happen.
 *
 * If the client is found to be blocked into a hash slot this node no
 * longer handles, the client is sent a redirection error, and the function
 * returns 1. Otherwise 0 is returned and no operation is performed. 
 *
 * 此函数由处理客户端的函数增量调用，以检测超时，以便处理以下情况：
 * 
 * 1）客户端使用BLPOP或类似的阻塞操作进行阻塞。
 * 2） 主节点将散列槽迁移到其他位置，或者变成从节点。
 * 3） 客户端可能会永远处于阻塞状态（或最长超时时间），等待永远不会发生的密钥更改。
 *     
 * 如果发现客户端被阻塞到该节点不再处理的哈希槽中，则会向客户端发送重定向错误，函数返回1。否则返回0，并且不执行任何操作。
 * */
int clusterRedirectBlockedClientIfNeeded(client *c) {
    if (c->flags & CLIENT_BLOCKED &&
        (c->btype == BLOCKED_LIST ||
         c->btype == BLOCKED_ZSET ||
         c->btype == BLOCKED_STREAM))
    {
        dictEntry *de;
        dictIterator *di;

        /* If the cluster is down, unblock the client with the right error.
         * If the cluster is configured to allow reads on cluster down, we
         * still want to emit this error since a write will be required
         * to unblock them which may never come.  
         *
         * 如果群集已关闭，请使用正确的错误取消阻止客户端。如果集群被配置为允许在集群关闭时
         * 进行读取，我们仍然希望发出此错误，因为需要写操作来解锁它们，而这可能永远不会发生
         * 。
         * */
        if (server.cluster->state == CLUSTER_FAIL) {
            clusterRedirectClient(c,NULL,0,CLUSTER_REDIR_DOWN_STATE);
            return 1;
        }

        /* All keys must belong to the same slot, so check first key only. 
         *
         * 所有键都必须属于同一个插槽，所以只检查第一个键。
         * */
        di = dictGetIterator(c->bpop.keys);
        if ((de = dictNext(di)) != NULL) {
            robj *key = dictGetKey(de);
            int slot = keyHashSlot((char*)key->ptr, sdslen(key->ptr));
            clusterNode *node = server.cluster->slots[slot];

            /* if the client is read-only and attempting to access key that our
             * replica can handle, allow it. 
             *
             * 如果客户端是只读的，并且试图访问我们的复制副本可以处理的键，请允许它。
             * */
            if ((c->flags & CLIENT_READONLY) &&
                (c->lastcmd->flags & CMD_READONLY) &&
                nodeIsSlave(myself) && myself->slaveof == node)
            {
                node = myself;
            }

            /* We send an error and unblock the client if:
             * 1) The slot is unassigned, emitting a cluster down error.
             * 2) The slot is not handled by this node, nor being imported. 
             *
             * 如果出现以下情况，我们会发送一个错误并取消阻止客户端：
             * 1）插槽未分配，发出集群关闭错误。
             * 2） 此节点不处理插槽，也不导入插槽。
             * */
            if (node != myself &&
                server.cluster->importing_slots_from[slot] == NULL)
            {
                if (node == NULL) {
                    clusterRedirectClient(c,NULL,0,
                        CLUSTER_REDIR_DOWN_UNBOUND);
                } else {
                    clusterRedirectClient(c,node,slot,
                        CLUSTER_REDIR_MOVED);
                }
                dictReleaseIterator(di);
                return 1;
            }
        }
        dictReleaseIterator(di);
    }
    return 0;
}
