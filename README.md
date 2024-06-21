# MyWebServer
利用I/O复用技术epoll与线程池实现多线程的Reactor和模拟Proactor高并发模型  
利用状态机解析HTTP请求报文，实现处理静态资源的请求，并发送响应报文  
基于双向链表实现的定时器，关闭超时的非活动连接  
利用单例模式与阻塞队列实现异步的日志系统，记录服务器运行状态  
利用RAII机制实现了数据库连接池，减少数据库连接建立与关闭的开销，同时实现了用户注册登录功能  
# 说明
此项目为网上开源项目，作者为 [@qinguoyi](https://github.com/qinguoyi)  
源项目地址 [https://github.com/qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer)
