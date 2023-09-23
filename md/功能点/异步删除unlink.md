```
---> 方法：unlinkCommand   「文件位置：db.c」【调用 delGenericCommand 的时候，指定是懒删除】
 
---> 方法：delGenericCommand   「文件位置：db.c」
 
------> 方法：expireIfNeeded    「文件位置：db.c」【看下是否过期了】
------> 方法：dbAsyncDelete    「文件位置：lazyfree.c」 【异步删除的前提是要删除的数据 free_effort > 64】
----------> 方法：lazyfreeGetFreeEffort    「文件位置：lazyfree.c」 【计算数据的 free_effort 】

------> 方法：signalModifiedKey   「文件位置：db.c」【发送通知给监听的客户端】
```