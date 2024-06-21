#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port, string user, string passWord, string databaseName,
        int log_write, int opt_linger, int trigmode, int sql_num,
        int thread_num, int close_log, int actor_model);

    void thread_pool();//创建线程池
    void sql_pool();//初始化数据库连接池，并读取表
    void log_write();//开启日志
    void trig_mode();//设置触发模式
    void eventListen();//创建listenfd, epoll, listenfd加入到epoll,设置关闭连接方式，管道，注册信号事件
    void eventLoop();//运行服务器
    void timer(int connfd, struct sockaddr_in client_address);//为客户端初始化定时器
    void adjust_timer(util_timer* timer);//新的定时器在链表上的位置进行调整
    void deal_timer(util_timer* timer, int sockfd);//关闭对应客户端的监听，关闭对应客户端的文件描述符，将对应的定时器从定时器容器（双向链表）删除
    bool dealclientdata();//连接客户端，设置其定时器
    bool dealwithsignal(bool& timeout, bool& stop_server);//更新timeout(超时标志)和stop_server(服务器关闭标志)
    void dealwithread(int sockfd);//根据不同并发模式来处理接收的数据
    void dealwithwrite(int sockfd);//根据不同并发模式来发送的数据

public:
    //基础
    int m_port;//服务器端口号
    char* m_root;//工作目录
    int m_log_write;//日志写入方式（同步或异步）
    int m_close_log;//是否关闭日志标志
    int m_actormodel;//并发模式

    int m_pipefd[2];//管道文件描述符
    int m_epollfd;//epoll监听描述符
    http_conn* users;//一个数组，存储客户端对象

    //数据库相关
    connection_pool* m_connPool;//数据库连接池对象
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;//数据库连接池数量

    //线程池相关
    threadpool<http_conn>* m_pool;
    int m_thread_num;//线程池数量

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];//监听事件集合

    int m_listenfd;//连接客户端套接字
    int m_OPT_LINGER;//优雅关闭链接
    int m_TRIGMode;//触发组合模式
    int m_LISTENTrigmode;//LISTENT触发模式
    int m_CONNTrigmode;//CONNT触发模式

    //定时器相关
    client_data* users_timer;//连接资源类
    Utils utils;//工具类
};
#endif
