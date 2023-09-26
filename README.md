redis 源码注释，源码分析，流程分析。全网最新，最全！！！
中文注释字数30万！！！
```
1、流程分析主要在 md 目录，直接全局查找文件名就行：

AOF-1-版本.md
AOF-2-相关的属性和配置.md
AOF-3-基础流程.md
AOF-4-重写REWRITE.md
RDB-1-相关的属性和配置.md
RDB-2-基础流程.md

命令表的填充和查找流程.md
客户端链接流程和命令处理流程.md

数据类型.md
结构.md
创建.md
编码的改变.md

最大使用内存maxmemory.md
异步删除unlink.md
守护线程daemonize.md
过期策略流程.md


2、核心的类
server.c ： 核心的流程，初始化，serverCron
object.c :  基础对象的定义，各种数据的创建
ae.c ： 整个 redis 
ae_epoll.c ： linux 下 nio 的封装
networking.c : io线程的各种逻辑和网络交互的逻辑
bio.h : 多线程的定义
bio.c ：多线程的实现逻辑
aof.c ： aof持久化的逻辑
rdb.c ： rdb持久化的逻辑
lazyfree.c ：延迟释放的逻辑


sds.c ： 字符串的实现逻辑
adlist.c ：链表的实现逻辑
ziplist.c ：压缩列表的实现逻辑

t_string.c ：应用层数据的 string 的实现逻辑
t_hash.c ：应用层数据的 hash 的实现逻辑
t_list.c ：应用层数据的 list 的实现逻辑
t_set.c ：应用层数据的 set 的实现逻辑
t_zset.c ：应用层数据的 zset 的实现逻辑


3、原来 README.md 改名为 README-BACKUP.md 了
```