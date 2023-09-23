```
一、Redis daemonize介绍

1、daemonize介绍

 A、redis.conf配置文件中daemonize守护线程，默认是NO。
 B、daemonize是用来指定redis是否要用守护线程的方式启动。


2、daemonize 设置yes或者no区别

daemonize:yes: redis采用的是单进程多线程的模式。当redis.conf中选项daemonize设置成yes时，
               代表开启守护进程模式。在该模式下，redis会在后台运行，
               并将进程pid号写入至redis.conf选项pidfile设置的文件中，
               此时redis将一直运行，除非手动kill该进程。
daemonize:no: 当daemonize选项设置成no时，当前界面将进入redis的命令行界面，
              exit强制退出或者关闭连接工具(putty,xshell等)都会导致redis进程退出。
              
3、源码位置：server.c 
 「void daemonize(void)」
```