#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer* tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

//添加定时器，内部调用私有成员add_timer
void sort_timer_lst::add_timer(util_timer* timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)//如果头结点为空，表示没有元素
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)//如果新加的结点的时间戳小于头结点的时间戳，则新结点设为头结点
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    //否则调用私有成员，调整内部结点
    add_timer(timer, head);
}

//调整客户端对应的定时器在容器中的位置，以保持容器升序
void sort_timer_lst::adjust_timer(util_timer* timer)
{
    if (!timer)
    {
        return;
    }
    util_timer* tmp = timer->next;//获取调整结点的一个结点的超时时间
    if (!tmp || (timer->expire < tmp->expire))//如果比下一个超时时间短，不用调整
    {
        return;
    }
    if (timer == head)//如果要调整的结点为头结点
    {
        //先将要调整的结点删除
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        //再将该结点重新插入容器
        add_timer(timer, head);
    }
    else
    {
        ////先将要调整的结点删除
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        //再将该结点重新插入容器
        add_timer(timer, timer->next);
    }
}

//从定时器容器（双向链表）删除
void sort_timer_lst::del_timer(util_timer* timer)
{
    if (!timer)//传入指针为空
    {
        return;
    }
    if ((timer == head) && (timer == tail))//只有一个结点
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)//删除头结点
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)//删除尾结点
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    //删除中间结点
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

//查看定时器容器中是否有超时的客户端连接
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    time_t cur = time(NULL);//获取当前的时间戳
    util_timer* tmp = head;
    while (tmp)
    {
        //如果当前遍历到的定时器对象没有超时，就结束循环
        if (cur < tmp->expire)
        {
            break;
        }
        //否则调用回调函数，关闭对应客户端的监听，关闭对应客户端的文件描述符
        tmp->cb_func(tmp->user_data);
        //删除该结点
        head = tmp->next;
        if (head)//head为NULL，表示当前容器为空
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

//添加新结点,lst_head：从该结点的位置往后插入
void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head)
{
    util_timer* prev = lst_head;
    util_timer* tmp = prev->next;
    while (tmp)//将新结点插入到中间
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)//将新结点插入到尾部
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

//设置最小超时单位
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        //EPOLLRDHUP可以判断对端是否关闭
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        //EPOLLONESHOT 可以避免不同的线程或者进程在处理同一个SOCKET的事件
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);//设置非阻塞
}

//信号处理函数，向对端发送信号
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

//设置（注册）信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        //SA_RESTART这个可以让当前进程接着执行没有执行完毕的系统调用函数
        sa.sa_flags |= SA_RESTART;//由此信号中断的系统调用自动重启动
    sigfillset(&sa.sa_mask);//即使没有信号需要屏蔽，也要初始化这个成员（sigemptyset()），sa_mask是一个信号集
    assert(sigaction(sig, &sa, NULL) != -1);//assert断言，sigaction注册信号处理函数
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);//重新设置定时器
}

//向对端发送消息，并关闭对端文件描述符
void Utils::show_error(int connfd, const char* info)
{
    send(connfd, info, strlen(info), 0);//发送信息
    close(connfd);//关闭连接
}

int* Utils::u_pipefd = 0;//管道文件描述符
int Utils::u_epollfd = 0;//epollfd

class Utils;
//关闭对应客户端的监听，关闭对应客户端的文件描述符
void cb_func(client_data* user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);//取消监听
    assert(user_data);
    close(user_data->sockfd);//关闭连接
    http_conn::m_user_count--;//用户数减一
}