#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;
//连接资源类
struct client_data
{
    sockaddr_in address;//客户端地址结构
    int sockfd;//客户端文件描述符
    util_timer* timer;//定时器对象
};
//定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    //超时时间
    time_t expire;
    //回调函数
    void (*cb_func)(client_data*);
    //连接资源
    client_data* user_data;
    //前向定时器
    util_timer* prev;
    //后继定时器
    util_timer* next;
};

//定时器容器类
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    //添加定时器，内部调用私有成员add_timer
    void add_timer(util_timer* timer);
    
    //调整客户端对应的定时器在容器中的位置，以保持容器升序
    void adjust_timer(util_timer* timer);

    //从定时器容器（双向链表）删除
    void del_timer(util_timer* timer);

    //查看定时器容器中是否有超时的客户端连接
    void tick();

private:
    //添加新结点,lst_head：从该结点的位置往后插入
    void add_timer(util_timer* timer, util_timer* lst_head);
    //头结点
    util_timer* head;
    //尾结点
    util_timer* tail;
};
//工具类
class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    //向对端发送消息，并关闭对端文件描述符
    void show_error(int connfd, const char* info);

public:
    static int* u_pipefd;//管道文件描述符
    sort_timer_lst m_timer_lst;//定时器容器（双向列表）
    static int u_epollfd;//epollfd
    int m_TIMESLOT;//最小超时单位
};

//关闭对应客户端的监听，关闭对应客户端的文件描述符
void cb_func(client_data* user_data);

#endif