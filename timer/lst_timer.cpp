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

//��Ӷ�ʱ�����ڲ�����˽�г�Աadd_timer
void sort_timer_lst::add_timer(util_timer* timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)//���ͷ���Ϊ�գ���ʾû��Ԫ��
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)//����¼ӵĽ���ʱ���С��ͷ����ʱ��������½����Ϊͷ���
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    //�������˽�г�Ա�������ڲ����
    add_timer(timer, head);
}

//�����ͻ��˶�Ӧ�Ķ�ʱ���������е�λ�ã��Ա�����������
void sort_timer_lst::adjust_timer(util_timer* timer)
{
    if (!timer)
    {
        return;
    }
    util_timer* tmp = timer->next;//��ȡ��������һ�����ĳ�ʱʱ��
    if (!tmp || (timer->expire < tmp->expire))//�������һ����ʱʱ��̣����õ���
    {
        return;
    }
    if (timer == head)//���Ҫ�����Ľ��Ϊͷ���
    {
        //�Ƚ�Ҫ�����Ľ��ɾ��
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        //�ٽ��ý�����²�������
        add_timer(timer, head);
    }
    else
    {
        ////�Ƚ�Ҫ�����Ľ��ɾ��
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        //�ٽ��ý�����²�������
        add_timer(timer, timer->next);
    }
}

//�Ӷ�ʱ��������˫������ɾ��
void sort_timer_lst::del_timer(util_timer* timer)
{
    if (!timer)//����ָ��Ϊ��
    {
        return;
    }
    if ((timer == head) && (timer == tail))//ֻ��һ�����
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)//ɾ��ͷ���
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)//ɾ��β���
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    //ɾ���м���
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

//�鿴��ʱ���������Ƿ��г�ʱ�Ŀͻ�������
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    time_t cur = time(NULL);//��ȡ��ǰ��ʱ���
    util_timer* tmp = head;
    while (tmp)
    {
        //�����ǰ�������Ķ�ʱ������û�г�ʱ���ͽ���ѭ��
        if (cur < tmp->expire)
        {
            break;
        }
        //������ûص��������رն�Ӧ�ͻ��˵ļ������رն�Ӧ�ͻ��˵��ļ�������
        tmp->cb_func(tmp->user_data);
        //ɾ���ý��
        head = tmp->next;
        if (head)//headΪNULL����ʾ��ǰ����Ϊ��
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

//����½��,lst_head���Ӹý���λ���������
void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head)
{
    util_timer* prev = lst_head;
    util_timer* tmp = prev->next;
    while (tmp)//���½����뵽�м�
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
    if (!tmp)//���½����뵽β��
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

//������С��ʱ��λ
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//���ļ����������÷�����
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//���ں��¼���ע����¼���ETģʽ��ѡ����EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        //EPOLLRDHUP�����ж϶Զ��Ƿ�ر�
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        //EPOLLONESHOT ���Ա��ⲻͬ���̻߳��߽����ڴ���ͬһ��SOCKET���¼�
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);//���÷�����
}

//�źŴ���������Զ˷����ź�
void Utils::sig_handler(int sig)
{
    //Ϊ��֤�����Ŀ������ԣ�����ԭ����errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

//���ã�ע�ᣩ�źź���
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        //SA_RESTART��������õ�ǰ���̽���ִ��û��ִ����ϵ�ϵͳ���ú���
        sa.sa_flags |= SA_RESTART;//�ɴ��ź��жϵ�ϵͳ�����Զ�������
    sigfillset(&sa.sa_mask);//��ʹû���ź���Ҫ���Σ�ҲҪ��ʼ�������Ա��sigemptyset()����sa_mask��һ���źż�
    assert(sigaction(sig, &sa, NULL) != -1);//assert���ԣ�sigactionע���źŴ�����
}

//��ʱ�����������¶�ʱ�Բ��ϴ���SIGALRM�ź�
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);//�������ö�ʱ��
}

//��Զ˷�����Ϣ�����رնԶ��ļ�������
void Utils::show_error(int connfd, const char* info)
{
    send(connfd, info, strlen(info), 0);//������Ϣ
    close(connfd);//�ر�����
}

int* Utils::u_pipefd = 0;//�ܵ��ļ�������
int Utils::u_epollfd = 0;//epollfd

class Utils;
//�رն�Ӧ�ͻ��˵ļ������رն�Ӧ�ͻ��˵��ļ�������
void cb_func(client_data* user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);//ȡ������
    assert(user_data);
    close(user_data->sockfd);//�ر�����
    http_conn::m_user_count--;//�û�����һ
}