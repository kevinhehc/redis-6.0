```
1、触发时机如下，
 
1.1 收到用户的请求 ----  方法：bgsaveCommand 「文件位置：rdb.c」。
    * fork的时机 「rdbSaveBackground」 

1.2 延迟触发 ---- 「serverCron」的 「server.rdb_bgsave_scheduled」，因为执行bgsave命令的时候，已经有子进程命令在执行了。
    * fork的时机 「rdbSaveBackground」 
     
1.3 在 「server.c 的 serverCron」 里面，遍历配置的参数「for (j = 0; j < server.saveparamslen; j++)」，看是否达到了保存的时候。     
    * fork的时机 「rdbSaveBackground」 
    

1.4 「aof-rewrite」 也会调用 「rewriteAppendOnlyFile--->rdbSaveRio」 进行
     * fork的时机 「rewriteAppendOnlyFileBackground」
```


```
2、完成的处理

2.1 如果子进程退出，并发了信号给父进程，那么，会执行收尾工作 backgroundSaveDoneHandler。  
    触发的过程是 serverCron--->checkChildrenDone--->backgroundSaveDoneHandler。 

```