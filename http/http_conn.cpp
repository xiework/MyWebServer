#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char* ok_200_title = "OK";//成功
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char* error_403_title = "Forbidden";//资源不可访问
const char* error_403_form = "You do not have permission to get file form this server.\n";
const char* error_404_title = "Not Found";//资源找不到
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";//内部出错
const char* error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;//存储数据库中所有用户的用户名和密码

//获取数据库中所有用户的用户名和密码
void http_conn::initmysql_result(connection_pool* connPool)
{
    //先从连接池中取一个连接
    MYSQL* mysql = NULL;
    //mysql中保存了一个空闲的连接对象
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES* result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);//取消监听
    close(fd);//关闭描述符
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    /*
    * EPOLLONESHOT:
        不同的线程或者进程在处理同一个SOCKET的事件，这会使程序的健壮性大降低而编程的复杂度大大增加,
        EPOLLONESHOT可以解决这个问题
        在epoll上注册这个事件，注册这个事件后，如果在处理写成当前的SOCKET后不再重新注册相关事件，
        那么这个事件就不再响应了或者说触发了。要想重新注册事件则需要调用epoll_ctl重置文件描述符上的事件，
        这样前面的socket就不会出现竞态这样就可以通过手动的方式来保证同一SOCKET只能被一个线程处理，
        不会跨越多个线程。
    * EPOLLRDHUP：
        通过EPOLLRDHUP属性，来判断是否对端已经关闭，这样可以减少一次系统调用。
        在2.6.17的内核版本之前，只能再通过调用一次recv函数来判断
    */
    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;//表示客户端的数量
int http_conn::m_epollfd = -1;//epollfd

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr, char* root, int TRIGMode,
    int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析一行格式是否正确，不分析内容是否正确
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前是\r字符，则有可能会读取到完整行
        if (temp == '\r')
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            //下一个字符是\n，将\r\n改为\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')//读到"\n"的可能性，是读到\r正好循环结束
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')//判断上一个读到的字符是否为"\r"
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //当前字节既不是\r，也不是\n
    //表示接收不完整，需要继续接收，返回LINE_OPEN
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//将客户端发过来的请求内容读到m_read_buf
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);//返回读取了多少字节
        
        if (bytes_read <= 0)
        {
            return false;
        }

        m_read_idx += bytes_read;

        return true;
    }
    //ET读数据，一次性读取完缓冲区，读到errno == EAGAINE 为止
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    //假设 GET \t /562f25980001b1b106000338.jpg \t HTTP/1.1
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //请求行中最先含有空格和\t任一字符的位置并返回
    //依次检验字符串 text 中的字符，当被检验字符在字符串 "\t" 中也包含时，则停止检验，并返回该字符位置
    m_url = strpbrk(text, " \t");
    if (!m_url)//不包含"\t"
    {
        return BAD_REQUEST;
    }
    
    char* method = text;
    //确定请求方式
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;//是否启用post标志
    }
    else
        return BAD_REQUEST;
    //将该位置改为\0，用于将前面数据取出
    *m_url++ = '\0';
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    //strspn(str,group):就是从str的第一个元素开始往后数，看str中是不是连续往后每个字符都在group中可以找到。
    //到第一个不在gruop的元素为止。看从str第一个开始，前面的字符有几个在group中。
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    //使用与判断请求方式的相同逻辑，判断HTTP版本号
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    //对请求资源前7个字符进行判断
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //strchr() 用于查找字符串中的一个字符，并返回该字符在字符串中第一次出现的位置。
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    //一般的不会带有上述两种符号(http:// https://)，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)//等于1,表示只有一个"/"，后面没有跟请求资源
        //默认跳到注册页面
        strcat(m_url, "judge.html");//把 第二个参数 所指向的字符串追加到 m_url 所指向的字符串的结尾。
    m_check_state = CHECK_STATE_HEADER;//请求行分析完，更新主状态机，处理请求头
    return NO_REQUEST;//因为没有分析完请求头和空行及请求数据
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    //在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，
    //若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。
    if (text[0] == '\0')
    {
        if (m_content_length != 0)//判断是否有请求体，如果有表示请求不完整
        {
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");//跳过多个空行和\t
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
//如果是post请求，则将请求体内容拷贝到m_string
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//解析http报文，获取url version host content_length m_real_file等属性
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;//从状态机状态
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        
        //text为当前分析的行在m_read_buf中的起始位置
        text = get_line();
        m_start_line = m_checked_idx;//m_start_line表示当前分析的行的结束位置，也是下一行的起始位置
        LOG_INFO("%s", text);//写日志
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            //分析请求行
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            //分析请求头
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            //分析请求体
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//获取请求文件，对m_real_file和m_url进行封装，以及将目标文件映射到内存
http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char* p = strrchr(m_url, '/');

    //处理cgi，*(p+1)为2表示登录校验，*(p+1)为3表示注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        //m_url_real为请求的文件名
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");//"/"拷贝到m_url_real
        strcat(m_url_real, m_url + 2);//"m_url+2"追加到m_url_real尾部
        //组装出请求文件的绝对路径，根目录+文件名
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        //提取出用户名
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';
        //提取出密码
        int j = 0;
        for (i = i + 8; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            //先写插入语句
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())//在user(map)中寻找是否存在重名，为true表示没有
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");//登录网页
                else
                    strcpy(m_url, "/registerError.html");//出错网页
            }
            else
                strcpy(m_url, "/registerError.html");//出错网页
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");//欢迎页面
            else
                strcpy(m_url, "/logError.html");//登录错误页面
        }
    }

    if (*(p + 1) == '0')//请求注册
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");//注册页面
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')//请求登录
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')//请求图片
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')//请求视频
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else {
        //按理说，这条语句一般不会执行，但也有可能用户请求了不存在的资源
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }
        

    if (stat(m_real_file, &m_file_stat) < 0)//获取文件信息
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))//判断用户是否有读权限
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))//判断请求的资源是否为目录
        return BAD_REQUEST;
    //O_RDONLY 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    //通过mmap将该文件映射到内存中，提高文件的访问速度
    /*
        start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址
        length：映射区的长度
        prot：期望的内存保护标志，不能与文件的打开模式冲突，PROT_READ 表示页内容可以被读取
        flags：指定映射对象的类型，映射选项和映射页是否可以共享
            MAP_PRIVATE 建立一个写入时拷贝的私有映射，内存区域的写入不会影响到原文件
        fd：有效的文件描述符，一般是由open()函数返回
        off_toffset：被映射对象内容的起点
    */
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);//关闭文件描述符
    return FILE_REQUEST;
}

//移除内存映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);//移除内存映射
        m_file_address = 0;
    }
}

//发送响应报文
bool http_conn::write()
{
    int temp = 0;
   
    int newadd = 0;
    
    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    
    while (1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);
        
        //正常发送，temp为发送的字节数
        if (temp > 0)
        {
            //更新已发送字节
            bytes_have_send += temp;
            //偏移文件iovec的指针,newadd>=0
            newadd = bytes_have_send - m_write_idx;
        }
        if (temp <= -1)
        {
            //判断客户端缓冲区是否满了
            if (errno == EAGAIN)//若eagain则满了，更新iovec结构体的指针和长度，
                //并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），
                //因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
            {
                //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
                if (bytes_have_send >= m_iv[0].iov_len)
                {
                    //不再继续发送头部信息
                    m_iv[0].iov_len = 0;
                    //有可能目标文件有一部分发送出去，还剩下一部分，下面是指向剩余部分的地址和长度
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;//剩余的字节数
                }
                //继续发送第一个iovec头部信息的数据
                else
                {
                    //同样发送一部分后，缓冲区满了
                    m_iv[0].iov_base = m_write_buf + bytes_have_send;
                    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }
                //重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            //如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }
        
        //更新已发送字节数，剩余的字节数
        bytes_to_send -= temp;
        
        //判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            //移除内存映射
            unmap();
            
            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            
            //浏览器的请求为长连接
            if (m_linger)
            {
                //重新初始化HTTP对象
                init();
                return true;
            }
            else//如果是短连接，则关闭连接
            {
                return false;
            }
        }
    }
}

//向写缓冲区写入内容，例状态行，消息报头等，都会调用该函数
bool http_conn::add_response(const char* format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    //定义可变参数列表
    va_list arg_list;
    //初始化可变参数列表
    va_start(arg_list, format);
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    ////如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    //更新m_write_idx
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

//向缓冲区写入状态行
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//写消息报头
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
        add_blank_line();
}

//添加Content-Length，消息报头的响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//添加响应正文
bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

//将要发送的http响应报文和文件准备好
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {   //状态行
        add_status_line(500, error_500_title);
        //消息报头
        add_headers(strlen(error_500_form));
        //响应正文
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        //判断请求的资源是否存在
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;//指向要发送的http报文
            m_iv[0].iov_len = m_write_idx;//报文的长度
            m_iv[1].iov_base = m_file_address;//资源文件的内存地址
            m_iv[1].iov_len = m_file_stat.st_size;//资源文件的大小
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            //如果请求的资源大小为0，则返回空白html文件
            const char* ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
        break;
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//解析http报文，生成响应报文，并将对应的客户端套接字监听写事件
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        //重置事件，重新读取
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        //报错了，关闭连接
        close_conn();
    }
    //重新注册该m_sockfd，监听其写事件
    //触发条件：如果当连接可用后，且缓存区不满的情况下，
    //调用epoll_ctl将fd重新注册到epoll事件池(使用EPOLL_CTL_MOD)，这时也会触发EPOLLOUT时间。
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}