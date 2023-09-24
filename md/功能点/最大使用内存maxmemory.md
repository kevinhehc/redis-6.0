```
1、判断的地方
 
---> 方法：processCommand    「文件位置：server.c」

---> 方法：freeMemoryIfNeededAndSafe    「文件位置：evict.c」

---> 方法：freeMemoryIfNeeded    「文件位置：evict.c」
```



```
2、maxmemory 的计算：

2.1 maxmemory 存储在 【static size_t used_memory = 0】「文件位置：zmalloc.c」 
 
2.2 每次分配内存的时候会累加 「zmalloc() ---> update_zmalloc_stat_alloc()」 「文件位置：zmalloc.c」 

2.2 每次释放内存的时候会减少 「zrealloc() ---> update_zmalloc_stat_free()」 「文件位置：zmalloc.c」 
    
```


```

3、杂项：

3.1 大部分的逻辑在 evict.c 文件里面
 
3.2 基于 LRU 和 LFU 做的淘汰策略

```




```
10、默认机制：如果没有限制内存大小，在 32 位机器上限制使用内存为 3G，为了预防 redis 崩溃
 
---> 代码：【if (server.arch_bits == 32 && server.maxmemory == 0)】    「文件位置：server.c」 
```