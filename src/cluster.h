#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster data structures, defines, exported API.
 * Redis集群数据结构，定义，导出API
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOTS 16384
#define CLUSTER_OK 0          /* Everything looks ok 
                               *
                               * 一切看起来都很好
                               * */
#define CLUSTER_FAIL 1        /* The cluster can't work 
                               *
                               * 群集无法工作
                               * */
#define CLUSTER_NAMELEN 40    /* sha1 hex length 
                               *
                               * sha1六角长度
                               * */
#define CLUSTER_PORT_INCR 10000 /* Cluster port = baseport + PORT_INCR 
                                 *
                                 * 集群端口=基本端口+port_INCR
                                 * */

/* The following defines are amount of time, sometimes expressed as
 * multiplicators of the node timeout value (when ending with MULT). 
 *
 * 以下定义是时间量，有时表示为节点超时值的乘数（以MULT结尾时）。
 * */
#define CLUSTER_FAIL_REPORT_VALIDITY_MULT 2 /* Fail report validity. 
                                             *
                                             * 报告有效性失败。
                                             * */
#define CLUSTER_FAIL_UNDO_TIME_MULT 2 /* Undo fail if master is back. 
                                       *
                                       * 如果master返回，则撤消失败。
                                       * */
#define CLUSTER_FAIL_UNDO_TIME_ADD 10 /* Some additional time. 
                                       *
                                       * 一些额外的时间。
                                       * */
#define CLUSTER_FAILOVER_DELAY 5 /* Seconds 
                                  *
                                  * 秒
                                  * */
#define CLUSTER_MF_TIMEOUT 5000 /* Milliseconds to do a manual failover. 
                                 *
                                 * 进行手动故障切换的毫秒数。
                                 * */
#define CLUSTER_MF_PAUSE_MULT 2 /* Master pause manual failover mult. 
                                 *
                                 * 主暂停手动故障转移倍数。
                                 * */
#define CLUSTER_SLAVE_MIGRATION_DELAY 5000 /* Delay for slave migration. 
                                            *
                                            * 从属迁移延迟。
                                            * */

/* Redirection errors returned by getNodeByQuery(). 
 *
 * getNodeByQuery（）返回重定向错误。
 * */
#define CLUSTER_REDIR_NONE 0          /* Node can serve the request. 
                                       *
                                       * 节点可以为请求提供服务。
                                       * */
#define CLUSTER_REDIR_CROSS_SLOT 1    /* -CROSSSLOT request. 
                                       *
                                       * -CROSSSLOT请求。
                                       * */
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required 
                                       *
                                       * -TryAgain重定向是必需的
                                       * */
#define CLUSTER_REDIR_ASK 3           /* -ASK redirection required. 
                                       *
                                       * -需要ASK重定向。
                                       * */
#define CLUSTER_REDIR_MOVED 4         /* -MOVED redirection required. 
                                       *
                                       * -需要MOVED重定向。
                                       * */
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. 
                                       *
                                       * -CLUSTERDOWN，全局状态。
                                       * */
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, unbound slot. 
                                       *
                                       * CLUSTERDOWN，未绑定插槽。
                                       * */
#define CLUSTER_REDIR_DOWN_RO_STATE 7 /* -CLUSTERDOWN, allow reads. 
                                       *
                                       * -CLUSTERDOWN，允许读取。
                                       * */

struct clusterNode;

/* clusterLink encapsulates everything needed to talk with a remote node. 
 *
 * clusterLink封装了与远程节点进行通信所需的一切。
 * */
typedef struct clusterLink {
    mstime_t ctime;             /* Link creation time 
                                 *
                                 * 链接创建时间
                                 * */
    connection *conn;           /* Connection to remote node 
                                 *
                                 * 连接到远程节点
                                 * */
    sds sndbuf;                 /* Packet send buffer 
                                 *
                                 * 数据包发送缓冲区
                                 * */
    char *rcvbuf;               /* Packet reception buffer 
                                 *
                                 * 数据包接收缓冲器
                                 * */
    size_t rcvbuf_len;          /* Used size of rcvbuf 
                                 *
                                 * 已使用的rcvbuf大小
                                 * */
    size_t rcvbuf_alloc;        /* Used size of rcvbuf 
                                 *
                                 * 已使用的rcvbuf大小
                                 * */
    struct clusterNode *node;   /* Node related to this link if any, or NULL 
                                 *
                                 * 与此链接相关的节点（如果有）或NULL
                                 * */
} clusterLink;

/* Cluster node flags and macros. 
 *
 * 群集节点标志和宏。
 * */
#define CLUSTER_NODE_MASTER 1     /* The node is a master 
                                   *
                                   * 节点是主节点
                                   * */
#define CLUSTER_NODE_SLAVE 2      /* The node is a slave 
                                   *
                                   * 节点是从节点
                                   * */
#define CLUSTER_NODE_PFAIL 4      /* Failure? Need acknowledge 
                                   *
                                   * 失败需要确认
                                   * */
#define CLUSTER_NODE_FAIL 8       /* The node is believed to be malfunctioning 
                                   *
                                   * 据信该节点出现故障
                                   * */
#define CLUSTER_NODE_MYSELF 16    /* This node is myself 
                                   *
                                   * 这个节点就是我自己
                                   * */
#define CLUSTER_NODE_HANDSHAKE 32 /* We have still to exchange the first ping 
                                   *
                                   * 我们还需要交换第一个ping
                                   * */
#define CLUSTER_NODE_NOADDR   64  /* We don't know the address of this node 
                                   *
                                   * 我们不知道这个节点的地址
                                   * */
#define CLUSTER_NODE_MEET 128     /* Send a MEET message to this node 
                                   *
                                   * 向该节点发送MEET消息
                                   * */
#define CLUSTER_NODE_MIGRATE_TO 256 /* Master eligible for replica migration. 
                                     *
                                     * 符合复制副本迁移条件的主节点。
                                     * */
#define CLUSTER_NODE_NOFAILOVER 512 /* Slave will not try to failover. 
                                     *
                                     * 从属服务器不会尝试故障转移。
                                     * */
#define CLUSTER_NODE_NULL_NAME "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"

#define nodeIsMaster(n) ((n)->flags & CLUSTER_NODE_MASTER)
#define nodeIsSlave(n) ((n)->flags & CLUSTER_NODE_SLAVE)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeWithoutAddr(n) ((n)->flags & CLUSTER_NODE_NOADDR)
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)
#define nodeCantFailover(n) ((n)->flags & CLUSTER_NODE_NOFAILOVER)

/* Reasons why a slave is not able to failover. 
 *
 * 从属服务器无法进行故障切换的原因。
 * */
#define CLUSTER_CANT_FAILOVER_NONE 0
#define CLUSTER_CANT_FAILOVER_DATA_AGE 1
#define CLUSTER_CANT_FAILOVER_WAITING_DELAY 2
#define CLUSTER_CANT_FAILOVER_EXPIRED 3
#define CLUSTER_CANT_FAILOVER_WAITING_VOTES 4
#define CLUSTER_CANT_FAILOVER_RELOG_PERIOD (60*5) /* seconds. 
                                                   *
                                                   * 秒。
                                                   * */

/* clusterState todo_before_sleep flags. 
 *
 * clusterState todo_before_sleep标志。
 * */
#define CLUSTER_TODO_HANDLE_FAILOVER (1<<0) // 处理故障转移
#define CLUSTER_TODO_UPDATE_STATE (1<<1)    // 处理状态更新
#define CLUSTER_TODO_SAVE_CONFIG (1<<2)     // 处理配置保存
#define CLUSTER_TODO_FSYNC_CONFIG (1<<3)    // 处理配置刷盘

/* Message types.
 *
 * Note that the PING, PONG and MEET messages are actually the same exact
 * kind of packet. PONG is the reply to ping, in the exact format as a PING,
 * while MEET is a special PING that forces the receiver to add the sender
 * as a node (if it is not already in the list). 
 *
 * 消息类型。请注意，PING、PONG和MEET消息实际上是完全相同类型的数据包。
 * PONG是对ping的回复，格式与ping完全相同，而MEET是一种特殊的ping，它迫使接收方将发送方添加为节点（如果它还不在列表中）。
 * */
#define CLUSTERMSG_TYPE_PING 0          /* Ping  
                                         * */
#define CLUSTERMSG_TYPE_PONG 1          /* Pong (reply to Ping) 
                                         *
                                         * Pong（回复Ping）
                                         * */
#define CLUSTERMSG_TYPE_MEET 2          /* Meet "let's join" message 
                                         *
                                         * 满足“让我们加入”的信息
                                         * */
#define CLUSTERMSG_TYPE_FAIL 3          /* Mark node xxx as failing 
                                         *
                                         * 将节点xxx标记为失败
                                         * */
#define CLUSTERMSG_TYPE_PUBLISH 4       /* Pub/Sub Publish propagation 
                                         *
                                         * 发布/子发布传播
                                         * */
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5 /* May I failover? 
                                                 *
                                                 * 我可以进行故障切换吗？
                                                 * */
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK 6     /* Yes, you have my vote 
                                                 *
                                                 * 是的，你有我的投票权
                                                 * */
#define CLUSTERMSG_TYPE_UPDATE 7        /* Another node slots configuration 
                                         *
                                         * 另一个节点插槽配置
                                         * */
#define CLUSTERMSG_TYPE_MFSTART 8       /* Pause clients for manual failover 
                                         *
                                         * 暂停客户端进行手动故障切换
                                         * */
#define CLUSTERMSG_TYPE_MODULE 9        /* Module cluster API message. 
                                         *
                                         * 模块群集API消息。
                                         * */
#define CLUSTERMSG_TYPE_COUNT 10        /* Total number of message types. 
                                         *
                                         * 消息类型的总数。
                                         * */

/* Flags that a module can set in order to prevent certain Redis Cluster
 * features to be enabled. Useful when implementing a different distributed
 * system on top of Redis Cluster message bus, using modules. 
 *
 * 模块可以设置的标志，以防止启用某些Redis群集功能。在Redis Cluster消息总线上使用模块实现不同的分布式系统时非常有用。
 * */
#define CLUSTER_MODULE_FLAG_NONE 0
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1<<1)
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1<<2)

/* This structure represent elements of node->fail_reports. 
 *
 * 此结构表示node->fail_reports的元素。
 * */
typedef struct clusterNodeFailReport {
    struct clusterNode *node;  /* Node reporting the failure condition. 
                                *
                                * 报告故障状况的节点。
                                * */
    mstime_t time;             /* Time of the last report from this node. 
                                *
                                * 此节点上一次报告的时间。
                                * */
} clusterNodeFailReport;

typedef struct clusterNode {
    mstime_t ctime; /* Node object creation time. 
                     *
                     * 节点对象创建时间。
                     * */
    char name[CLUSTER_NAMELEN]; /* Node name, hex string, sha1-size 
                                 *
                                 * 节点名称，十六进制字符串，sha1大小
                                 * */
    int flags;      /* CLUSTER_NODE_... 
                     * */
    uint64_t configEpoch; /* Last configEpoch observed for this node 
                           *
                           * 观察到此节点的上次configEpoch
                           * */
    unsigned char slots[CLUSTER_SLOTS/8]; /* slots handled by this node 
                                           *
                                           * 此节点处理的插槽
                                           * */
    int numslots;   /* Number of slots handled by this node 
                     *
                     * 此节点处理的插槽数
                     * */
    int numslaves;  /* Number of slave nodes, if this is a master 
                     *
                     * 从属节点的数量，如果这是主节点
                     * */
    struct clusterNode **slaves; /* pointers to slave nodes 
                                  *
                                  * 指向从属节点的指针
                                  * */
    struct clusterNode *slaveof; /* pointer to the master node. Note that it
                                    may be NULL even if the node is a slave
                                    if we don't have the master node in our
                                    tables. 
                                  *
                                  * 指向主节点的指针。注意，如果表中没有主节点，即使节点是从节点，它也可能为NULL。
                                  * */
    mstime_t ping_sent;      /* Unix time we sent latest ping 
                              *
                              * Unix时间我们发送了最新的ping
                              * 如果已经收到PONG，那么node->ping_sent为零
                              * */
    mstime_t pong_received;  /* Unix time we received the pong 
                              *
                              * Unix时间我们收到pong
                              * */
    mstime_t data_received;  /* Unix time we received any data 
                              *
                              * Unix时间我们收到任何数据
                              * */
    mstime_t fail_time;      /* Unix time when FAIL flag was set 
                              *
                              * 设置FAIL标志的Unix时间
                              * */
    mstime_t voted_time;     /* Last time we voted for a slave of this master 
                              *
                              * 上次我们投票给这个主节点的从节点
                              * */
    mstime_t repl_offset_time;  /* Unix time we received offset for this node 
                                 *
                                 * Unix时间，我们收到此节点的偏移量
                                 * */
    mstime_t orphaned_time;     /* Starting time of orphaned master condition 
                                 *
                                 * 称为孤儿主节点状态的开始时间
                                 * */
    long long repl_offset;      /* Last known repl offset for this node. 
                                 *
                                 * 此节点的最后一个已知repl偏移量。
                                 * */
    char ip[NET_IP_STR_LEN];  /* Latest known IP address of this node 
                               *
                               * 此节点的最新已知IP地址
                               * */
    int port;                   /* Latest known clients port of this node 
                                 *
                                 * 此节点的最新已知客户端端口
                                 * */
    int cport;                  /* Latest known cluster port of this node. 
                                 *
                                 * 此节点的最新已知群集端口。
                                 * */
    clusterLink *link;          /* TCP/IP link with this node 
                                 *
                                 * 与此节点的TCP/IP链接
                                 * */
    list *fail_reports;         /* List of nodes signaling this as failing 
                                 *
                                 * 发出故障信号的节点列表
                                 * */
} clusterNode;

typedef struct clusterState {
    clusterNode *myself;  /* This node 
                           *
                           * 此节点
                           * */
    uint64_t currentEpoch; // 当前的年代
    int state;            /* CLUSTER_OK, CLUSTER_FAIL, ... 
                           * */
    int size;             /* Num of master nodes with at least one slot 
                           *
                           * 至少有一个插槽的主节点数
                           * */
    dict *nodes;          /* Hash table of name -> clusterNode structures 
                           *
                           * 名称->clusterNode结构的哈希表
                           * */
    dict *nodes_black_list; /* Nodes we don't re-add for a few seconds. 
                             *
                             * 我们在几秒钟内不会重新添加节点。
                             * */
    clusterNode *migrating_slots_to[CLUSTER_SLOTS];
    clusterNode *importing_slots_from[CLUSTER_SLOTS];
    clusterNode *slots[CLUSTER_SLOTS];
    uint64_t slots_keys_count[CLUSTER_SLOTS];
    rax *slots_to_keys;
    /* The following fields are used to take the slave state on elections. 
     *
     * 以下字段用于对从节点状态进行选举。
     * */
    mstime_t failover_auth_time; /* Time of previous or next election. 
                                  *
                                  * 上次或下次选举的时间。
                                  * */
    int failover_auth_count;    /* Number of votes received so far. 
                                 *
                                 * 到目前为止收到的票数。
                                 * */
    int failover_auth_sent;     /* True if we already asked for votes. 
                                 *
                                 * 如果我们已经要求投票，这个值=true。
                                 * */
    int failover_auth_rank;     /* This slave rank for current auth request. 
                                 *
                                 * 当前身份验证请求的此从属级别。越小越可能成为主节点
                                 * */
    uint64_t failover_auth_epoch; /* Epoch of the current election. 
                                   *
                                   * 当前选举的时代。
                                   * */
    int cant_failover_reason;   /* Why a slave is currently not able to
                                   failover. See the CANT_FAILOVER_* macros. 
                                 *
                                 * 从属服务器当前无法进行故障切换的原因。请参见 CANT_FAILOVER_* macros.
                                 * */
    /* Manual failover state in common. 
     *
     * 常见的手动故障转移状态。
     * */
    mstime_t mf_end;            /* Manual failover time limit (ms unixtime).
                                   It is zero if there is no MF in progress. 
                                 *
                                 * 手动故障转移限制的结束时间ms。
                                 * 如果没有正在进行的手动故障转移，则为零。
                                 * */
    /* Manual failover state of master. 
     *
     * 主节点的手动故障转移状态。
     * */
    clusterNode *mf_slave;      /* Slave performing the manual failover. 
                                 *
                                 * 执行手动故障切换的从属设备。
                                 * */
    /* Manual failover state of slave. 
     *
     * 从节点的手动故障转移状态。
     * */
    long long mf_master_offset; /* Master offset the slave needs to start MF
                                   or zero if still not received. 
                                 *
                                 * 主节点偏移-从节点需要启动MF或零（如果仍未收到）。
                                 * */
    int mf_can_start;           /* If non-zero signal that the manual failover
                                   can start requesting masters vote. 
                                 *
                                 * 如果非零信号表明手动故障转移可以开始请求主投票。
                                 * */
    /* The following fields are used by masters to take state on elections. 
     *
     * 以下字段由主节点用来进行选举记录状态。
     * */
    uint64_t lastVoteEpoch;     /* Epoch of the last vote granted. 
                                 *
                                 * 最后一次投票的年代。
                                 * */
    int todo_before_sleep; /* Things to do in clusterBeforeSleep(). 
                            *
                            * 集群中要做的事情BeforeSleep（）。
                            * */
    /* Messages received and sent by type. 
     *
     * 按类型接收和发送的消息。
     * */
    long long stats_bus_messages_sent[CLUSTERMSG_TYPE_COUNT];
    long long stats_bus_messages_received[CLUSTERMSG_TYPE_COUNT];
    long long stats_pfail_nodes;    /* Number of nodes in PFAIL status,
                                       excluding nodes without address. 
                                     *
                                     * 处于PFAIL状态的节点数，不包括没有地址的节点。
                                     * */
} clusterState;

/* Redis cluster messages header 
 *
 * Redis集群消息头
 * */

/* Initially we don't know our "name", but we'll find it once we connect
 * to the first node, using the getsockname() function. Then we'll use this
 * address for all the next messages. 
 *
 * 最初我们不知道我们的“名称”，但一旦我们使用getsockname（）函数连接到
 * 第一个节点，我们就会找到它。然后，我们将使用此地址处理所有后续邮件。
 * */
typedef struct {
    char nodename[CLUSTER_NAMELEN];
    uint32_t ping_sent;
    uint32_t pong_received;
    char ip[NET_IP_STR_LEN];  /* IP address last time it was seen 
                               *
                               * 上次看到的IP地址
                               * */
    uint16_t port;              /* base port last time it was seen 
                                 *
                                 * 上次看到它时的基地端口
                                 * */
    uint16_t cport;             /* cluster port last time it was seen 
                                 *
                                 * 上次看到群集端口时
                                 * */
    uint16_t flags;             /* node->flags copy 
                                 *
                                 * node->标志复制
                                 * */
    uint32_t notused1;
} clusterMsgDataGossip;

typedef struct {
    char nodename[CLUSTER_NAMELEN];
} clusterMsgDataFail;

typedef struct {
    uint32_t channel_len;
    uint32_t message_len;
    unsigned char bulk_data[8]; /* 8 bytes just as placeholder. 
                                 *
                                 * 8字节作为占位符。
                                 * */
} clusterMsgDataPublish;

typedef struct {
    uint64_t configEpoch; /* Config epoch of the specified instance. 
                           *
                           * 配置指定实例的epoch。
                           * */
    char nodename[CLUSTER_NAMELEN]; /* Name of the slots owner. 
                                     *
                                     * 插槽所有者的名称。
                                     * */
    unsigned char slots[CLUSTER_SLOTS/8]; /* Slots bitmap. 
                                           *
                                           * 插槽位图。
                                           * */
} clusterMsgDataUpdate;

typedef struct {
    uint64_t module_id;     /* ID of the sender module. 
                             *
                             * 发送器模块的ID。
                             * */
    uint32_t len;           /* ID of the sender module. 
                             *
                             * 发送器模块的ID。
                             * */
    uint8_t type;           /* Type from 0 to 255. 
                             *
                             * 键入0到255之间的值。
                             * */
    unsigned char bulk_data[3]; /* 3 bytes just as placeholder. 
                                 *
                                 * 3个字节作为占位符。
                                 * */
} clusterMsgModule;

union clusterMsgData {
    /* PING, MEET and PONG
     * */
    struct {
        /* Array of N clusterMsgDataGossip structures 
         *
         * N个clusterMsgDataGossip结构的数组
         * */
        clusterMsgDataGossip gossip[1];
    } ping;

    /* FAIL 
     *
     * 失败
     * */
    struct {
        clusterMsgDataFail about;
    } fail;

    /* PUBLISH 
     *
     * 发布
     * */
    struct {
        clusterMsgDataPublish msg;
    } publish;

    /* UPDATE 
     *
     * 更新
     * */
    struct {
        clusterMsgDataUpdate nodecfg;
    } update;

    /* MODULE 
     *
     * 模块
     * */
    struct {
        clusterMsgModule msg;
    } module;
};

#define CLUSTER_PROTO_VER 1 /* Cluster bus protocol version. 
                             *
                             * 群集总线协议版本。
                             * */

typedef struct {
    char sig[4];        /* Signature "RCmb" (Redis Cluster message bus). 
                         *
                         * 签名“RCmb”（Redis Cluster消息总线）。
                         * */
    uint32_t totlen;    /* Total length of this message 
                         *
                         * 此邮件的总长度
                         * */
    uint16_t ver;       /* Protocol version, currently set to 1. 
                         *
                         * 协议版本，当前设置为1。
                         * */
    uint16_t port;      /* TCP base port number. 
                         *
                         * TCP基本端口号。
                         * */
    uint16_t type;      /* Message type 
                         *
                         * 消息类型
                         * */
    uint16_t count;     /* Only used for some kind of messages. 
                         *
                         * 仅用于某些类型的消息。
                         * */
    uint64_t currentEpoch;  /* The epoch accordingly to the sending node. 
                             *
                             * 与发送节点相应的历元。
                             * */
    uint64_t configEpoch;   /* The config epoch if it's a master, or the last
                               epoch advertised by its master if it is a
                               slave. 
                             *
                             * 配置历元（如果它是主历元），或者如果它是从历元，则由其主历元通告的最后一个历元。
                             * */
    uint64_t offset;    /* Master replication offset if node is a master or
                           processed replication offset if node is a slave. 
                         *
                         * 如果节点是主节点，则主复制偏移量；如果节点是从节点，则处理的复制偏移量。
                         * */
    char sender[CLUSTER_NAMELEN]; /* Name of the sender node 
                                   *
                                   * 发送方节点的名称
                                   * */
    unsigned char myslots[CLUSTER_SLOTS/8];
    char slaveof[CLUSTER_NAMELEN];
    char myip[NET_IP_STR_LEN];    /* Sender IP, if not all zeroed. 
                                   *
                                   * 发件人IP，如果不是全部为零。
                                   * */
    char notused1[34];  /* 34 bytes reserved for future usage. 
                         *
                         * 保留34字节以备将来使用。
                         * */
    uint16_t cport;      /* Sender TCP cluster bus port 
                          *
                          * 发送器TCP群集总线端口
                          * */
    uint16_t flags;      /* Sender node flags 
                          *
                          * 发件人节点标志
                          * */
    unsigned char state; /* Cluster state from the POV of the sender 
                          *
                          * 来自发送方POV的群集状态
                          * */
    unsigned char mflags[3]; /* Message flags: CLUSTERMSG_FLAG[012]_... 
                              *
                              * 消息标志：CLUSTERMSG_FLAG[012]_。。。
                              * */
    union clusterMsgData data;
} clusterMsg;

#define CLUSTERMSG_MIN_LEN (sizeof(clusterMsg)-sizeof(union clusterMsgData))

/* Message flags better specify the packet content or are used to
 * provide some information about the node state. 
 *
 * 消息标志更好地指定数据包内容或用于提供有关节点状态的一些信息。
 * */
#define CLUSTERMSG_FLAG0_PAUSED (1<<0) /* Master paused for manual failover. 
                                        *
                                        * 主节点已暂停以进行手动故障转移。
                                        * */
#define CLUSTERMSG_FLAG0_FORCEACK (1<<1) /* Give ACK to AUTH_REQUEST even if
                                            master is up. 
                                          *
                                          * 即使主节点已启动，也向AUTH_REQUEST发出ACK。
                                          * */

/* ---------------------- API exported outside cluster.c --------------------
 * ---------------------- API导出到 cluster.c --------------------------------
 * */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);
unsigned long getClusterConnectionsCount(void);

#endif /* __CLUSTER_H */
