```
1、replicationCron 的执行时机

---> 方法：Main 「文件位置：server.c」

---> 方法：aeMain   「文件位置：ae.c」

---> 方法：aeProcessEvents   「文件位置：ae.c」

---> 方法：processTimeEvents    「文件位置：ae.c」

---> 方法：serverCron    「文件位置：server.c」   【run_with_period(1000) 保证了一秒刷一次的逻辑 replicationCron】

---> 方法：replicationCron   「文件位置：replication.c」

```


```
2、写入增量缓存区的核心方法：feedReplicationBacklog

2.1
---> 方法：replicationCron   「文件位置：replication.c」----------会触发「replicationFeedSlaves」
---> 方法：propagateExpire   「文件位置：db.c」-------------------会触发「replicationFeedSlaves」
---> 代码：server.get_ack_from_slaves   「文件位置：server.c」----会触发「replicationFeedSlaves」
---> 方法：propagate   「文件位置：server.c」---------------------会触发「replicationFeedSlaves」

2.2
---> 方法：replicationFeedSlaves   「文件位置：replication.c」

2.3
---> 方法：replicationFeedSlaves   「文件位置：feedReplicationBacklog.c」

```


```
3、把增量缓冲区发送出去的核心方法：addReplyReplicationBacklog

---> 方法：syncCommand   「文件位置：replication.c」

---> 方法：masterTryPartialResynchronization   「文件位置：replication.c」

---> 方法：addReplyReplicationBacklog   「文件位置：replication.c」
```