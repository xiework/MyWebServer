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
//������Դ��
struct client_data
{
    sockaddr_in address;//�ͻ��˵�ַ�ṹ
    int sockfd;//�ͻ����ļ�������
    util_timer* timer;//��ʱ������
};
//��ʱ����
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    //��ʱʱ��
    time_t expire;
    //�ص�����
    void (*cb_func)(client_data*);
    //������Դ
    client_data* user_data;
    //ǰ��ʱ��
    util_timer* prev;
    //��̶�ʱ��
    util_timer* next;
};

//��ʱ��������
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    //��Ӷ�ʱ�����ڲ�����˽�г�Աadd_timer
    void add_timer(util_timer* timer);
    
    //�����ͻ��˶�Ӧ�Ķ�ʱ���������е�λ�ã��Ա�����������
    void adjust_timer(util_timer* timer);

    //�Ӷ�ʱ��������˫������ɾ��
    void del_timer(util_timer* timer);

    //�鿴��ʱ���������Ƿ��г�ʱ�Ŀͻ�������
    void tick();

private:
    //����½��,lst_head���Ӹý���λ���������
    void add_timer(util_timer* timer, util_timer* lst_head);
    //ͷ���
    util_timer* head;
    //β���
    util_timer* tail;
};
//������
class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //���ļ����������÷�����
    int setnonblocking(int fd);

    //���ں��¼���ע����¼���ETģʽ��ѡ����EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //�źŴ�����
    static void sig_handler(int sig);

    //�����źź���
    void addsig(int sig, void(handler)(int), bool restart = true);

    //��ʱ�����������¶�ʱ�Բ��ϴ���SIGALRM�ź�
    void timer_handler();

    //��Զ˷�����Ϣ�����رնԶ��ļ�������
    void show_error(int connfd, const char* info);

public:
    static int* u_pipefd;//�ܵ��ļ�������
    sort_timer_lst m_timer_lst;//��ʱ��������˫���б�
    static int u_epollfd;//epollfd
    int m_TIMESLOT;//��С��ʱ��λ
};

//�رն�Ӧ�ͻ��˵ļ������رն�Ӧ�ͻ��˵��ļ�������
void cb_func(client_data* user_data);

#endif