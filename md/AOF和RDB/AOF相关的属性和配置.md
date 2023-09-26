```c
// 「文件位置：server.h」

struct redisServer {
    /* AOF persistence AOF持久化 */
    int aof_enabled;                  /* AOF开关配置 ---------- AOF configuration */
    int aof_state;                    /* AOF状态 ---------- (ON|OFF|WAIT_REWRITE) */
    int aof_fsync;                    /* fsync（）策略的类型 ---------- Kind of fsync() policy */
    char *aof_filename;               /* AOF文件的名称 ---------- Name of the AOF file */
    int aof_no_fsync_on_rewrite;      /* 如果正在进行rewrite，请不要进行fsync。默认0 ---------- fsync if a rewrite is in prog. */
    int aof_rewrite_perc;             /* 如果%增长大于M，则重写AOF。。。  ---------- Rewrite AOF if % growth is > M and... */
    off_t aof_rewrite_min_size;       /* AOF文件是至少N个字节。 ----------  the AOF file is at least N bytes. */
    off_t aof_rewrite_base_size;      /* 最新启动或重写时的AOF大小。 ----------  AOF size on latest startup or rewrite.  */
    off_t aof_current_size;           /* AOF当前大小，通过读取aof文件获取的大小。  ----------   AOF current size. */
    off_t aof_fsync_offset;           /* 已同步到磁盘的AOF偏移量，只有通过aof_current_size赋值。 
                                       * offset which is already synced to disk. */
    int aof_flush_sleep;              /* 在刷盘前睡眠一下。（用于测试） ---------- Micros to sleep before flush. (used by tests) */
    int aof_rewrite_scheduled;        /* BGSAVE终止后进行重写。---------- Rewrite once BGSAVE terminates. */
    pid_t aof_child_pid;              /* PID如果重写过程 ---------- PID if rewriting process    */
    list *aof_rewrite_buf_blocks;     /* 在AOF重写期间，把数据也写入到这个缓冲区。 ---------- Hold changes during an AOF rewrite. */
    sds aof_buf;                      /* AOF缓冲区，在进入事件循环之前写入 ---------- AOF buffer, written before entering the event loop */
    int aof_fd;                       /* 当前所选AOF文件的文件描述符 ---------- File descriptor of currently selected AOF file */
    int aof_selected_db;              /* AOF中当前选择的DB ---------- Currently selected DB in AOF */
    time_t aof_flush_postponed_start; /* 延迟AOF刷新的UNIX时间。当配置是异步刷盘，但是到了刷盘时间，已有异步刷盘任务，还没结束，就延迟。
                                       * UNIX time of postponed AOF flush */
    time_t aof_last_fsync;            /* 最后一次fsync（）的UNIX时间 ---------- UNIX time of last fsync() */
    time_t aof_rewrite_time_last;     /* 上次AOF重写运行所用的时间。 ---------- Time used by last AOF rewrite run. */
    time_t aof_rewrite_time_start;    /* 当前AOF重写开始时间。 ---------- Current AOF rewrite start time. */
    int aof_lastbgrewrite_status;     /* C_OK or C_ERR  */
    unsigned long aof_delayed_fsync;  /* 延迟AOF fsync（）计数器  ---------- delayed AOF fsync() counter */
    int aof_rewrite_incremental_fsync;/* 在aof重写时递增fsync？ ---------- fsync incrementally while aof rewriting? */
    int rdb_save_incremental_fsync;   /* 在rdb保存时递增fsync？ ---------- fsync incrementally while rdb saving? */
    int aof_last_write_status;        /* C_OK or C_ERR  */
    int aof_last_write_errno;         /* 如果aof_last_write_status为ERR，则有效  ---------- Valid if aof_last_write_status is ERR */
    int aof_load_truncated;           /* 不要在意外的AOF EOF上停下来。---------- Don't stop on unexpected AOF EOF. */
    int aof_use_rdb_preamble;         /* 在AOF重写时使用RDB前导码。---------- Use RDB preamble on AOF rewrites. */
    
    /* AOF pipes used to communicate between parent and child during rewrite.
     * 在重写期间，用于在父级和子级之间进行通信的AOF管道。
     * */
    int aof_pipe_write_data_to_child;
    int aof_pipe_read_data_from_parent;
    int aof_pipe_write_ack_to_parent;
    int aof_pipe_read_ack_from_child;
    int aof_pipe_write_ack_to_child;
    int aof_pipe_read_ack_from_parent;
    int aof_stop_sending_diff;      /* If true stop sending accumulated diffs to child process.   
                                     * 如果为true，则停止向子进程发送累积的diff。*/
    
    sds aof_child_diff;             /*  AOF差分累加器子进程侧。---------- AOF diff accumulator child side.  */



    /* Pipe and data structures for child -> parent info sharing.
     * 用于子级->父级信息共享的管道和数据结构。*/
    int child_info_pipe[2];         /* 用于写入child_info_data的管道。  Pipe used to write the child_info_data.*/
    struct {
        int process_type;           /* AOF or RDB child?  ---------- AOF还是RDB进程？*/
        size_t cow_size;            /* Copy on write size. ---------- 写入时复制大小。*/
        unsigned long long magic;   /* Magic value to make sure data is valid. ---------- 确保数据有效的魔术值。*/
    } child_info_data;
    
    /* AOF/复制中的命令传播 Propagation of commands in AOF / replication */
    redisOpArray also_propagate;    /* 要传播的附加命令。---------- Additional command to propagate. */
}
```