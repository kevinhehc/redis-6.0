```c

// 「文件位置：server.h」

struct redisServer {
    /* RDB persistence  RDB持久性 */
    long long dirty;                /* 上次保存后对数据库的更改 ---------- Changes to DB from the last save */
    long long dirty_before_bgsave;  /* 用于在失败的BGSAVE上还原脏 ---------- Used to restore dirty on failed BGSAVE */
    pid_t rdb_child_pid;            /* RDB保存子进程的PID ---------- PID of RDB saving child */
    struct saveparam *saveparams;   /* RDB的保存点数组 ---------- Save points array for RDB */
    int saveparamslen;              /* 保存点数 ---------- Number of saving points */
    char *rdb_filename;             /* RDB文件的名称 ---------- Name of RDB file */
    int rdb_compression;            /* 在RDB中使用压缩？---------- Use compression in RDB? */
    int rdb_checksum;               /* 是否使用RDB校验和？---------- Use RDB checksum? */
    int rdb_del_sync_files;         /* 如果实例不使用持久性，则删除仅用于SYNC的RDB文件。 
                                     * Remove RDB files used only for SYNC ifthe instance does not use persistence.*/
    time_t lastsave;                /* 上次成功保存的Unix时间 ---------- Unix time of last successful save */
    time_t lastbgsave_try;          /* 上次尝试bgsave的Unix时间 ---------- Unix time of last attempted bgsave */
    time_t rdb_save_time_last;      /* 上次RDB存储运行所用的时间。---------- Time used by last RDB save run. */
    time_t rdb_save_time_start;     /* 当前RDB保存开始时间。---------- Current RDB save start time. */
    int rdb_bgsave_scheduled;       /* 如果为true，则在可能的情况下执行BGSAVE。---------- BGSAVE when possible if true. */
    int rdb_child_type;             /* 按活动子项保存的类型。---------- Type of save by active child. */
    int lastbgsave_status;          /* C_OK or C_ERR */
    int stop_writes_on_bgsave_err;  /* 如果不能BGSAVE，则不允许写入 ---------- Don't allow writes if can't BGSAVE*/
    int rdb_pipe_read;              /* 用于传输RDB数据的RDB管道  ---------- RDB pipe used to transfer the rdb data */
                                    /* 到无盘repl中的父进程。---------- to the parent process in diskless repl. */
    int rdb_child_exit_pipe;        /* 由无盘父进程使用，允许子进程退出。 
                                     * Used by the diskless parent allow child exit.*/
    connection **rdb_pipe_conns;    /* 当前的连接 ---------- Connections which are currently the */
    int rdb_pipe_numconns;          /* 无盘rdb fork-child的目标。---------- target of diskless rdb fork child. */
    int rdb_pipe_numconns_writing;  /* 具有挂起写入的rdb连接数。---------- Number of rdb conns with pending writes. */
    char *rdb_pipe_buff;            /* 在无盘复制中，此缓冲区保存数据 
                                     * In diskless replication, this buffer holds data */
    int rdb_pipe_bufflen;           /* 这是从rdb管道中读取的。---------- that was read from the the rdb pipe. */
    int rdb_key_save_delay;         /* 写入RDB时键之间的延迟（以微秒为单位）。（用于测试） 
                                     * Delay in microseconds between keys while writing the RDB. (for testings) */
    int key_load_delay;             /* 加载aof或rdb时键之间的延迟（以微秒为单位）。（用于测试）
                                     * Delay in microseconds between keys while loading aof or rdb. (for testings) 
                                     * */
    
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
};
```