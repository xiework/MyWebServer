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
    static const int FILENAME_LEN = 200;// //设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 2048;//读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;//写缓冲区大小
    //报文的请求方法，本项目只用到GET和POST
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
    //主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, //请求行
        CHECK_STATE_HEADER, //请求头
        CHECK_STATE_CONTENT //请求体
    };
    //报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,//请求不完整，需要继续读取请求报文数据
        GET_REQUEST,//获得了完整的HTTP请求
        BAD_REQUEST,//HTTP请求报文有语法错误
        NO_RESOURCE,//资源不存在
        FORBIDDEN_REQUEST,//资源不可访问
        FILE_REQUEST,//请求文件存在，且可以访问
        INTERNAL_ERROR,//服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION
    };
    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,//完整读取一行
        LINE_BAD,//报文语法有误
        LINE_OPEN//读取的行不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in& addr, char*, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);//关闭连接
    void process();//解析http报文，生成响应报文，并将对应的客户端套接字监听写事件
    bool read_once();//将客户端发过来的请求内容读到m_read_buf
    bool write();
    sockaddr_in* get_address()
    {
        return &m_address;
    }
    //获取数据库内的所有用户名和密码，保存到users中
    void initmysql_result(connection_pool* connPool);
    int timer_flag;//任务处理失败标志
    int improv;//任务处理标识，为1表示处理了，但不一定成功


private:
    void init();
    HTTP_CODE process_read();//解析http报文，获取url version host content_length m_real_file等属性
    bool process_write(HTTP_CODE ret);//将要发送的http响应报文和文件准备好
    HTTP_CODE parse_request_line(char* text);//解析http请求行，获得请求方法，目标url及http版本号
    HTTP_CODE parse_headers(char* text);//解析http请求的一个头部信息
    HTTP_CODE parse_content(char* text);//如果是post请求，则将请求体内容拷贝到m_string
    HTTP_CODE do_request();//获取请求文件，对m_real_file和m_url进行封装，以及将目标文件映射到内存
    //获取当前分析的行在m_read_buf中的起始位置
    char* get_line() { return m_read_buf + m_start_line; };
    //从状态机，用于分析出一行内容
    //返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char* format, ...);//向写缓冲区写入内容，例状态行，消息报头等，都会调用该函数
    bool add_content(const char* content);//添加响应正文
    bool add_status_line(int status, const char* title);//向缓冲区写入状态行
    bool add_headers(int content_length);//写消息报头
    bool add_content_type();//添加文本类型
    bool add_content_length(int content_length);//添加Content-Length，消息报头的响应报文的长度
    bool add_linger();//添加连接状态，通知浏览器端是保持连接还是关闭
    bool add_blank_line();//添加空行

public:
    static int m_epollfd;//epollfd
    static int m_user_count;//客户端数量
    MYSQL* mysql;//连建数据库的对象
    int m_state;  //表示当前连接对象是读状态还是写状态。读为0, 写为1

private:
    int m_sockfd;//与客户端连接的socket
    sockaddr_in m_address;//客户端地址结构，IP+端口号
    char m_read_buf[READ_BUFFER_SIZE];//读缓冲区
    long m_read_idx;//读缓冲区（m_read_buf）的最后一个字符的下一个位置
    long m_checked_idx;//读取m_read_buf时的位置
    int m_start_line;//m_read_buf中已经解析的字符个数
    char m_write_buf[WRITE_BUFFER_SIZE];//写缓冲区
    int m_write_idx;//写缓冲区（m_write_buf）的最后一个字符的下一个位置
    CHECK_STATE m_check_state;//主状态机的状态
    METHOD m_method;//请求方法
    
    //以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];//请求文件的路径
    char* m_url;//存储请求的资源名（不是资源的地址）
    char* m_version;//存储http版本号
    char* m_host;//请求头的host字段
    long m_content_length;//请求体长度
    bool m_linger; //判断是keep - alive还是close，决定是长连接还是短连接


    char* m_file_address;//请求文件映射到内存的地址
    struct stat m_file_stat;
    /*
        struct iovec {
            ptr_t iov_base;  指向一个缓冲区
            size_t iov_len;  缓冲区大小或容量
        };
    */
    struct iovec m_iv[2];//存储响应报文和响应的数据
    int m_iv_count;//m_iv的大小
    int cgi;        //是否启用的POST
    char* m_string; //存储请求体数据
    int bytes_to_send;//发送的全部数据为响应报文头部信息和文件大小
    int bytes_have_send;//已发送的字节数
    char* doc_root;//资源文件的根目录

    map<string, string> m_users;//存储数据库中所有的用户名和密码
    int m_TRIGMode;//触发模式
    int m_close_log;//日志开关

    char sql_user[100];//数据库用户名
    char sql_passwd[100];//数据库密码
    char sql_name[100];//数据库名
};

#endif