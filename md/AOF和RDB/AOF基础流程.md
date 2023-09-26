```
1、会累加到 aof 的数据的流程

---> 方法：aeMain   「文件位置：ae.c」
 
---> 方法：aeProcessEvents   「文件位置：ae.c」
 
---> 方法：aeApiPoll   「文件位置：ae.c」

---> 方法：readQueryFromClient   「文件位置：networking.c」
 
---> 方法：processInputBuffer   「文件位置：networking.c」
 
---> 方法：processCommand    「文件位置：server.c」

---> 方法：call    「文件位置：server.c」

---> 方法：propagate    「文件位置：server.c」

---> 方法：feedAppendOnlyFile    「文件位置：aof.c」
```