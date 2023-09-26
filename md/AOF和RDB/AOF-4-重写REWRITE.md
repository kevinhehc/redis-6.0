```
1、触发的地方

1.1 收到了客户端的命令，方法：bgrewriteaofCommand 「文件位置：aof.c」 ---> rewriteAppendOnlyFileBackground

1.2 由于本来想进行rewrite的，但是延后了，设置「server.aof_rewrite_scheduled = 1」，后面在「serverCron」循环的时候会判断，并执行。

1.3 由于大于设定的值增长值 
    「if (growth >= server.aof_rewrite_perc) 」  「文件位置：server.c」
```


```
2、重写的流程

2.1  fork子进程，保存一次rdb文件, 「rewriteAppendOnlyFile」「文件位置：aof.c」

2.2  父进程把增量数据保存在 「server.aof_rewrite_buf_blocks」 中

2.3  增量通过管道传输部分：
     * 如果 「server.aof_stop_sending_diff」为0，则调用「aofChildWriteDiffData」
       不断的发送 「aof_rewrite_buf_blocks」 增量数据，减少主进程的刷盘。
     * 注册发送事件「 aeCreateFileEvent(server.el, server.aof_pipe_write_data_to_child, AE_WRITABLE, aofChildWriteDiffData」。
     * 子进程通过「aofReadDiffFromParent」方法读取父进程发过来的数据，条件是读取有数据的次数少于20，而且读取时间少于 1000ms。
     
2.4  如果子进程退出，并发了信号给父进程，那么，会执行收尾工作 backgroundRewriteDoneHandler。  
     触发的过程是 serverCron--->checkChildrenDone--->backgroundRewriteDoneHandler。
```