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