```
1、会累加到 aof 的数据的流程

---> 方法：aeMain   「文件位置：ae.c」
 
---> 方法：aeProcessEvents   「文件位置：ae.c」
 
---> 方法：aeApiPoll   「文件位置：ae.c」

---> 方法：readQueryFromClient   「文件位置：networking.c」
 
---> 方法：processInputBuffer   「文件位置：networking.c」
 
---> 方法：processCommand    「文件位置：server.c」

---> 方法：call    「文件位置：server.c」  【除了这个路线，其他地方也有调】

---> 方法：propagate    「文件位置：server.c」 【除了这个路线，其他地方也有调】

---> 方法：feedAppendOnlyFile    「文件位置：aof.c」 【将数据追加到 server.aof_buf】
------> 方法：aofRewriteBufferAppend    「文件位置：aof.c」
                                       【如果 AOF 正在重写正在，那么也将新 buf 追加到 AOF 重写缓存aof_rewrite_buf_blocks中】
                                       【如果 aof_stop_sending_diff = 0，通过管道发送 aof 差分数据到子线程，减少主线程对差分的写文件】
```



```
2、刷盘流程和时机 ---- 每秒 

---> 方法：Main 「文件位置：server.c」

---> 方法：aeMain   「文件位置：ae.c」
 
---> 方法：aeProcessEvents   「文件位置：ae.c」

---> 方法：processTimeEvents    「文件位置：ae.c」

---> 方法：serverCron    「文件位置：server.c」   【run_with_period(1000) 保证了一秒刷一次的逻辑 flushAppendOnlyFile】

---> 方法：flushAppendOnlyFile   「文件位置：aof.c」
```


```
3、刷盘流程和时机 ---- 一直


---> 方法：Main 「文件位置：server.c」

---> 方法：aeMain   「文件位置：ae.c」
 
---> 方法：aeProcessEvents   「文件位置：ae.c」

---> 方法：beforeSleep    「文件位置：server.c」

---> 方法：flushAppendOnlyFile   「文件位置：aof.c」
```