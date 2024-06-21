#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"


class http_conn
{
public:
    static const int FILENAME_LEN = 200;// //���ö�ȡ�ļ�������m_real_file��С
    static const int READ_BUFFER_SIZE = 2048;//����������С
    static const int WRITE_BUFFER_SIZE = 1024;//д��������С
    //���ĵ����󷽷�������Ŀֻ�õ�GET��POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //��״̬����״̬
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, //������
        CHECK_STATE_HEADER, //����ͷ
        CHECK_STATE_CONTENT //������
    };
    //���Ľ����Ľ��
    enum HTTP_CODE
    {
        NO_REQUEST,//������������Ҫ������ȡ����������
        GET_REQUEST,//�����������HTTP����
        BAD_REQUEST,//HTTP���������﷨����
        NO_RESOURCE,//��Դ������
        FORBIDDEN_REQUEST,//��Դ���ɷ���
        FILE_REQUEST,//�����ļ����ڣ��ҿ��Է���
        INTERNAL_ERROR,//�������ڲ����󣬸ý������״̬���߼�switch��default�£�һ�㲻�ᴥ��
        CLOSED_CONNECTION
    };
    //��״̬����״̬
    enum LINE_STATUS
    {
        LINE_OK = 0,//������ȡһ��
        LINE_BAD,//�����﷨����
        LINE_OPEN//��ȡ���в�����
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in& addr, char*, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);//�ر�����
    void process();//����http���ģ�������Ӧ���ģ�������Ӧ�Ŀͻ����׽��ּ���д�¼�
    bool read_once();//���ͻ��˷��������������ݶ���m_read_buf
    bool write();
    sockaddr_in* get_address()
    {
        return &m_address;
    }
    //��ȡ���ݿ��ڵ������û��������룬���浽users��
    void initmysql_result(connection_pool* connPool);
    int timer_flag;//������ʧ�ܱ�־
    int improv;//�������ʶ��Ϊ1��ʾ�����ˣ�����һ���ɹ�


private:
    void init();
    HTTP_CODE process_read();//����http���ģ���ȡurl version host content_length m_real_file������
    bool process_write(HTTP_CODE ret);//��Ҫ���͵�http��Ӧ���ĺ��ļ�׼����
    HTTP_CODE parse_request_line(char* text);//����http�����У�������󷽷���Ŀ��url��http�汾��
    HTTP_CODE parse_headers(char* text);//����http�����һ��ͷ����Ϣ
    HTTP_CODE parse_content(char* text);//�����post���������������ݿ�����m_string
    HTTP_CODE do_request();//��ȡ�����ļ�����m_real_file��m_url���з�װ���Լ���Ŀ���ļ�ӳ�䵽�ڴ�
    //��ȡ��ǰ����������m_read_buf�е���ʼλ��
    char* get_line() { return m_read_buf + m_start_line; };
    //��״̬�������ڷ�����һ������
    //����ֵΪ�еĶ�ȡ״̬����LINE_OK,LINE_BAD,LINE_OPEN
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char* format, ...);//��д������д�����ݣ���״̬�У���Ϣ��ͷ�ȣ�������øú���
    bool add_content(const char* content);//�����Ӧ����
    bool add_status_line(int status, const char* title);//�򻺳���д��״̬��
    bool add_headers(int content_length);//д��Ϣ��ͷ
    bool add_content_type();//����ı�����
    bool add_content_length(int content_length);//���Content-Length����Ϣ��ͷ����Ӧ���ĵĳ���
    bool add_linger();//�������״̬��֪ͨ��������Ǳ������ӻ��ǹر�
    bool add_blank_line();//��ӿ���

public:
    static int m_epollfd;//epollfd
    static int m_user_count;//�ͻ�������
    MYSQL* mysql;//�������ݿ�Ķ���
    int m_state;  //��ʾ��ǰ���Ӷ����Ƕ�״̬����д״̬����Ϊ0, дΪ1

private:
    int m_sockfd;//��ͻ������ӵ�socket
    sockaddr_in m_address;//�ͻ��˵�ַ�ṹ��IP+�˿ں�
    char m_read_buf[READ_BUFFER_SIZE];//��������
    long m_read_idx;//����������m_read_buf�������һ���ַ�����һ��λ��
    long m_checked_idx;//��ȡm_read_bufʱ��λ��
    int m_start_line;//m_read_buf���Ѿ��������ַ�����
    char m_write_buf[WRITE_BUFFER_SIZE];//д������
    int m_write_idx;//д��������m_write_buf�������һ���ַ�����һ��λ��
    CHECK_STATE m_check_state;//��״̬����״̬
    METHOD m_method;//���󷽷�
    
    //����Ϊ�����������ж�Ӧ��6������
    char m_real_file[FILENAME_LEN];//�����ļ���·��
    char* m_url;//�洢�������Դ����������Դ�ĵ�ַ��
    char* m_version;//�洢http�汾��
    char* m_host;//����ͷ��host�ֶ�
    long m_content_length;//�����峤��
    bool m_linger; //�ж���keep - alive����close�������ǳ����ӻ��Ƕ�����


    char* m_file_address;//�����ļ�ӳ�䵽�ڴ�ĵ�ַ
    struct stat m_file_stat;
    /*
        struct iovec {
            ptr_t iov_base;  ָ��һ��������
            size_t iov_len;  ��������С������
        };
    */
    struct iovec m_iv[2];//�洢��Ӧ���ĺ���Ӧ������
    int m_iv_count;//m_iv�Ĵ�С
    int cgi;        //�Ƿ����õ�POST
    char* m_string; //�洢����������
    int bytes_to_send;//���͵�ȫ������Ϊ��Ӧ����ͷ����Ϣ���ļ���С
    int bytes_have_send;//�ѷ��͵��ֽ���
    char* doc_root;//��Դ�ļ��ĸ�Ŀ¼

    map<string, string> m_users;//�洢���ݿ������е��û���������
    int m_TRIGMode;//����ģʽ
    int m_close_log;//��־����

    char sql_user[100];//���ݿ��û���
    char sql_passwd[100];//���ݿ�����
    char sql_name[100];//���ݿ���
};

#endif