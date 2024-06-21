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

const int MAX_FD = 65536;           //����ļ�������
const int MAX_EVENT_NUMBER = 10000; //����¼���
const int TIMESLOT = 5;             //��С��ʱ��λ

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port, string user, string passWord, string databaseName,
        int log_write, int opt_linger, int trigmode, int sql_num,
        int thread_num, int close_log, int actor_model);

    void thread_pool();//�����̳߳�
    void sql_pool();//��ʼ�����ݿ����ӳأ�����ȡ��
    void log_write();//������־
    void trig_mode();//���ô���ģʽ
    void eventListen();//����listenfd, epoll, listenfd���뵽epoll,���ùر����ӷ�ʽ���ܵ���ע���ź��¼�
    void eventLoop();//���з�����
    void timer(int connfd, struct sockaddr_in client_address);//Ϊ�ͻ��˳�ʼ����ʱ��
    void adjust_timer(util_timer* timer);//�µĶ�ʱ���������ϵ�λ�ý��е���
    void deal_timer(util_timer* timer, int sockfd);//�رն�Ӧ�ͻ��˵ļ������رն�Ӧ�ͻ��˵��ļ�������������Ӧ�Ķ�ʱ���Ӷ�ʱ��������˫������ɾ��
    bool dealclientdata();//���ӿͻ��ˣ������䶨ʱ��
    bool dealwithsignal(bool& timeout, bool& stop_server);//����timeout(��ʱ��־)��stop_server(�������رձ�־)
    void dealwithread(int sockfd);//���ݲ�ͬ����ģʽ��������յ�����
    void dealwithwrite(int sockfd);//���ݲ�ͬ����ģʽ�����͵�����

public:
    //����
    int m_port;//�������˿ں�
    char* m_root;//����Ŀ¼
    int m_log_write;//��־д�뷽ʽ��ͬ�����첽��
    int m_close_log;//�Ƿ�ر���־��־
    int m_actormodel;//����ģʽ

    int m_pipefd[2];//�ܵ��ļ�������
    int m_epollfd;//epoll����������
    http_conn* users;//һ�����飬�洢�ͻ��˶���

    //���ݿ����
    connection_pool* m_connPool;//���ݿ����ӳض���
    string m_user;         //��½���ݿ��û���
    string m_passWord;     //��½���ݿ�����
    string m_databaseName; //ʹ�����ݿ���
    int m_sql_num;//���ݿ����ӳ�����

    //�̳߳����
    threadpool<http_conn>* m_pool;
    int m_thread_num;//�̳߳�����

    //epoll_event���
    epoll_event events[MAX_EVENT_NUMBER];//�����¼�����

    int m_listenfd;//���ӿͻ����׽���
    int m_OPT_LINGER;//���Źر�����
    int m_TRIGMode;//�������ģʽ
    int m_LISTENTrigmode;//LISTENT����ģʽ
    int m_CONNTrigmode;//CONNT����ģʽ

    //��ʱ�����
    client_data* users_timer;//������Դ��
    Utils utils;//������
};
#endif
