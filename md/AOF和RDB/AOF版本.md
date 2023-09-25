```
redis 4.0 之前的版本是纯 aof 的累加和 rewrite 逻辑

redis 4.0 + 是 混合持久化 rdb+追加

redis 7.0 是 multi part aof，基于混合持久化做了一定的优化，2022年 发布的，在用的也少，复杂度增加了，但是性能提高了

```