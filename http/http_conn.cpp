#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

//����http��Ӧ��һЩ״̬��Ϣ
const char* ok_200_title = "OK";//�ɹ�
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char* error_403_title = "Forbidden";//��Դ���ɷ���
const char* error_403_form = "You do not have permission to get file form this server.\n";
const char* error_404_title = "Not Found";//��Դ�Ҳ���
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";//�ڲ�����
const char* error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;//�洢���ݿ��������û����û���������

//��ȡ���ݿ��������û����û���������
void http_conn::initmysql_result(connection_pool* connPool)
{
    //�ȴ����ӳ���ȡһ������
    MYSQL* mysql = NULL;
    //mysql�б�����һ�����е����Ӷ���
    connectionRAII mysqlcon(&mysql, connPool);

    //��user���м���username��passwd���ݣ������������
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //�ӱ��м��������Ľ����
    MYSQL_RES* result = mysql_store_result(mysql);

    //���ؽ�����е�����
    int num_fields = mysql_num_fields(result);

    //���������ֶνṹ������
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    //�ӽ�����л�ȡ��һ�У�����Ӧ���û��������룬����map��
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//���ļ����������÷�����
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//���ں��¼���ע����¼���ETģʽ��ѡ����EPOLLONESHOT
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

//���ں�ʱ���ɾ��������
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);//ȡ������
    close(fd);//�ر�������
}

//���¼�����ΪEPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    /*
    * EPOLLONESHOT:
        ��ͬ���̻߳��߽����ڴ���ͬһ��SOCKET���¼������ʹ����Ľ�׳�Դ󽵵Ͷ���̵ĸ��Ӷȴ������,
        EPOLLONESHOT���Խ���������
        ��epoll��ע������¼���ע������¼�������ڴ���д�ɵ�ǰ��SOCKET��������ע������¼���
        ��ô����¼��Ͳ�����Ӧ�˻���˵�����ˡ�Ҫ������ע���¼�����Ҫ����epoll_ctl�����ļ��������ϵ��¼���
        ����ǰ���socket�Ͳ�����־�̬�����Ϳ���ͨ���ֶ��ķ�ʽ����֤ͬһSOCKETֻ�ܱ�һ���̴߳���
        �����Խ����̡߳�
    * EPOLLRDHUP��
        ͨ��EPOLLRDHUP���ԣ����ж��Ƿ�Զ��Ѿ��رգ��������Լ���һ��ϵͳ���á�
        ��2.6.17���ں˰汾֮ǰ��ֻ����ͨ������һ��recv�������ж�
    */
    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;//��ʾ�ͻ��˵�����
int http_conn::m_epollfd = -1;//epollfd

//�ر����ӣ��ر�һ�����ӣ��ͻ�������һ
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

//��ʼ������,�ⲿ���ó�ʼ���׽��ֵ�ַ
void http_conn::init(int sockfd, const sockaddr_in& addr, char* root, int TRIGMode,
    int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //�������������������ʱ����������վ��Ŀ¼�����http��Ӧ��ʽ������߷��ʵ��ļ���������ȫΪ��
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//��ʼ���½��ܵ�����
//check_stateĬ��Ϊ����������״̬
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

//��״̬�������ڷ���һ�и�ʽ�Ƿ���ȷ�������������Ƿ���ȷ
//����ֵΪ�еĶ�ȡ״̬����LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //tempΪ��Ҫ�������ֽ�
        temp = m_read_buf[m_checked_idx];
        //�����ǰ��\r�ַ������п��ܻ��ȡ��������
        if (temp == '\r')
        {
            //��һ���ַ��ﵽ��buffer��β������ղ���������Ҫ��������
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            //��һ���ַ���\n����\r\n��Ϊ\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')//����"\n"�Ŀ����ԣ��Ƕ���\r����ѭ������
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')//�ж���һ���������ַ��Ƿ�Ϊ"\r"
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //��ǰ�ֽڼȲ���\r��Ҳ����\n
    //��ʾ���ղ���������Ҫ�������գ�����LINE_OPEN
    return LINE_OPEN;
}

//ѭ����ȡ�ͻ����ݣ�ֱ�������ݿɶ���Է��ر�����
//���ͻ��˷��������������ݶ���m_read_buf
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT��ȡ����
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);//���ض�ȡ�˶����ֽ�
        
        if (bytes_read <= 0)
        {
            return false;
        }

        m_read_idx += bytes_read;

        return true;
    }
    //ET�����ݣ�һ���Զ�ȡ�껺����������errno == EAGAINE Ϊֹ
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

//����http�����У�������󷽷���Ŀ��url��http�汾��
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    //���� GET \t /562f25980001b1b106000338.jpg \t HTTP/1.1
    //��HTTP�����У�����������˵����������,Ҫ���ʵ���Դ�Լ���ʹ�õ�HTTP�汾�����и�������֮��ͨ��\t��ո�ָ���
    //�����������Ⱥ��пո��\t��һ�ַ���λ�ò�����
    //���μ����ַ��� text �е��ַ������������ַ����ַ��� "\t" ��Ҳ����ʱ����ֹͣ���飬�����ظ��ַ�λ��
    m_url = strpbrk(text, " \t");
    if (!m_url)//������"\t"
    {
        return BAD_REQUEST;
    }
    
    char* method = text;
    //ȷ������ʽ
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;//�Ƿ�����post��־
    }
    else
        return BAD_REQUEST;
    //����λ�ø�Ϊ\0�����ڽ�ǰ������ȡ��
    *m_url++ = '\0';
    //m_url��ʱ�����˵�һ���ո��\t�ַ�������֪��֮���Ƿ���
    //��m_url���ƫ�ƣ�ͨ�����ң����������ո��\t�ַ���ָ��������Դ�ĵ�һ���ַ�
    //strspn(str,group):���Ǵ�str�ĵ�һ��Ԫ�ؿ�ʼ����������str���ǲ�����������ÿ���ַ�����group�п����ҵ���
    //����һ������gruop��Ԫ��Ϊֹ������str��һ����ʼ��ǰ����ַ��м�����group�С�
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    //ʹ�����ж�����ʽ����ͬ�߼����ж�HTTP�汾��
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    //��������Դǰ7���ַ������ж�
    //������Ҫ����Щ���ĵ�������Դ�л����http://��������Ҫ������������е�������
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //strchr() ���ڲ����ַ����е�һ���ַ��������ظ��ַ����ַ����е�һ�γ��ֵ�λ�á�
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    //һ��Ĳ�������������ַ���(http:// https://)��ֱ���ǵ�����/��/�����������Դ
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //��urlΪ/ʱ����ʾ�жϽ���
    if (strlen(m_url) == 1)//����1,��ʾֻ��һ��"/"������û�и�������Դ
        //Ĭ������ע��ҳ��
        strcat(m_url, "judge.html");//�� �ڶ������� ��ָ����ַ���׷�ӵ� m_url ��ָ����ַ����Ľ�β��
    m_check_state = CHECK_STATE_HEADER;//�����з����꣬������״̬������������ͷ
    return NO_REQUEST;//��Ϊû�з���������ͷ�Ϳ��м���������
}

//����http�����һ��ͷ����Ϣ
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    //�ڱ����У�����ͷ�Ϳ��еĴ���ʹ�õ�ͬһ������������ͨ���жϵ�ǰ��text��λ�ǲ���\0�ַ���
    //���ǣ����ʾ��ǰ������ǿ��У������ǣ����ʾ��ǰ�����������ͷ��
    if (text[0] == '\0')
    {
        if (m_content_length != 0)//�ж��Ƿ��������壬����б�ʾ��������
        {
            //POST��Ҫ��ת����Ϣ�崦��״̬
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");//����������к�\t
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

//�ж�http�����Ƿ���������
//�����post���������������ݿ�����m_string
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST���������Ϊ������û���������
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//����http���ģ���ȡurl version host content_length m_real_file������
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;//��״̬��״̬
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        //m_start_line��ÿһ����������m_read_buf�е���ʼλ��
        //m_checked_idx��ʾ��״̬����m_read_buf�ж�ȡ��λ��
        
        //textΪ��ǰ����������m_read_buf�е���ʼλ��
        text = get_line();
        m_start_line = m_checked_idx;//m_start_line��ʾ��ǰ�������еĽ���λ�ã�Ҳ����һ�е���ʼλ��
        LOG_INFO("%s", text);//д��־
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            //����������
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            //��������ͷ
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
            //����������
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

//��ȡ�����ļ�����m_real_file��m_url���з�װ���Լ���Ŀ���ļ�ӳ�䵽�ڴ�
http_conn::HTTP_CODE http_conn::do_request()
{
    //����ʼ����m_real_file��ֵΪ��վ��Ŀ¼
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char* p = strrchr(m_url, '/');

    //����cgi��*(p+1)Ϊ2��ʾ��¼У�飬*(p+1)Ϊ3��ʾע��У��
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //���ݱ�־�ж��ǵ�¼��⻹��ע����
        char flag = m_url[1];

        //m_url_realΪ������ļ���
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");//"/"������m_url_real
        strcat(m_url_real, m_url + 2);//"m_url+2"׷�ӵ�m_url_realβ��
        //��װ�������ļ��ľ���·������Ŀ¼+�ļ���
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //���û�����������ȡ����
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        //��ȡ���û���
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';
        //��ȡ������
        int j = 0;
        for (i = i + 8; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //�����ע�ᣬ�ȼ�����ݿ����Ƿ���������
            //û�������ģ�������������
            //��д�������
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())//��user(map)��Ѱ���Ƿ����������Ϊtrue��ʾû��
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");//��¼��ҳ
                else
                    strcpy(m_url, "/registerError.html");//������ҳ
            }
            else
                strcpy(m_url, "/registerError.html");//������ҳ
        }
        //����ǵ�¼��ֱ���ж�
        //���������������û����������ڱ��п��Բ��ҵ�������1�����򷵻�0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");//��ӭҳ��
            else
                strcpy(m_url, "/logError.html");//��¼����ҳ��
        }
    }

    if (*(p + 1) == '0')//����ע��
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");//ע��ҳ��
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')//�����¼
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')//����ͼƬ
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')//������Ƶ
    {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else {
        //����˵���������һ�㲻��ִ�У���Ҳ�п����û������˲����ڵ���Դ
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }
        

    if (stat(m_real_file, &m_file_stat) < 0)//��ȡ�ļ���Ϣ
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))//�ж��û��Ƿ��ж�Ȩ��
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))//�ж��������Դ�Ƿ�ΪĿ¼
        return BAD_REQUEST;
    //O_RDONLY ��ֻ����ʽ���ļ�
    int fd = open(m_real_file, O_RDONLY);
    //ͨ��mmap�����ļ�ӳ�䵽�ڴ��У�����ļ��ķ����ٶ�
    /*
        start��ӳ�����Ŀ�ʼ��ַ������Ϊ0ʱ��ʾ��ϵͳ����ӳ��������ʼ��ַ
        length��ӳ�����ĳ���
        prot���������ڴ汣����־���������ļ��Ĵ�ģʽ��ͻ��PROT_READ ��ʾҳ���ݿ��Ա���ȡ
        flags��ָ��ӳ���������ͣ�ӳ��ѡ���ӳ��ҳ�Ƿ���Թ���
            MAP_PRIVATE ����һ��д��ʱ������˽��ӳ�䣬�ڴ������д�벻��Ӱ�쵽ԭ�ļ�
        fd����Ч���ļ���������һ������open()��������
        off_toffset����ӳ��������ݵ����
    */
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);//�ر��ļ�������
    return FILE_REQUEST;
}

//�Ƴ��ڴ�ӳ��
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);//�Ƴ��ڴ�ӳ��
        m_file_address = 0;
    }
}

//������Ӧ����
bool http_conn::write()
{
    int temp = 0;
   
    int newadd = 0;
    
    //��Ҫ���͵����ݳ���Ϊ0
    //��ʾ��Ӧ����Ϊ�գ�һ�㲻������������
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    
    while (1)
    {
        //����Ӧ���ĵ�״̬�С���Ϣͷ�����к���Ӧ���ķ��͸��������
        temp = writev(m_sockfd, m_iv, m_iv_count);
        
        //�������ͣ�tempΪ���͵��ֽ���
        if (temp > 0)
        {
            //�����ѷ����ֽ�
            bytes_have_send += temp;
            //ƫ���ļ�iovec��ָ��,newadd>=0
            newadd = bytes_have_send - m_write_idx;
        }
        if (temp <= -1)
        {
            //�жϿͻ��˻������Ƿ�����
            if (errno == EAGAIN)//��eagain�����ˣ�����iovec�ṹ���ָ��ͳ��ȣ�
                //��ע��д�¼����ȴ���һ��д�¼���������д�������Ӳ���д��Ϊ��д������epollout����
                //����ڴ��ڼ��޷��������յ�ͬһ�û�����һ���󣬵����Ա�֤���ӵ������ԡ�
            {
                //��һ��iovecͷ����Ϣ�������ѷ����꣬���͵ڶ���iovec����
                if (bytes_have_send >= m_iv[0].iov_len)
                {
                    //���ټ�������ͷ����Ϣ
                    m_iv[0].iov_len = 0;
                    //�п���Ŀ���ļ���һ���ַ��ͳ�ȥ����ʣ��һ���֣�������ָ��ʣ�ಿ�ֵĵ�ַ�ͳ���
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;//ʣ����ֽ���
                }
                //�������͵�һ��iovecͷ����Ϣ������
                else
                {
                    //ͬ������һ���ֺ󣬻���������
                    m_iv[0].iov_base = m_write_buf + bytes_have_send;
                    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }
                //����ע��д�¼�
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            //�������ʧ�ܣ������ǻ��������⣬ȡ��ӳ��
            unmap();
            return false;
        }
        
        //�����ѷ����ֽ�����ʣ����ֽ���
        bytes_to_send -= temp;
        
        //�ж�������������ȫ��������
        if (bytes_to_send <= 0)
        {
            //�Ƴ��ڴ�ӳ��
            unmap();
            
            //��epoll��������EPOLLONESHOT�¼�
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            
            //�����������Ϊ������
            if (m_linger)
            {
                //���³�ʼ��HTTP����
                init();
                return true;
            }
            else//����Ƕ����ӣ���ر�����
            {
                return false;
            }
        }
    }
}

//��д������д�����ݣ���״̬�У���Ϣ��ͷ�ȣ�������øú���
bool http_conn::add_response(const char* format, ...)
{
    //���д�����ݳ���m_write_buf��С�򱨴�
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    //����ɱ�����б�
    va_list arg_list;
    //��ʼ���ɱ�����б�
    va_start(arg_list, format);
    //������format�ӿɱ�����б�д�뻺����д������д�����ݵĳ���
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    ////���д������ݳ��ȳ���������ʣ��ռ䣬�򱨴�
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    //����m_write_idx
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

//�򻺳���д��״̬��
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//д��Ϣ��ͷ
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
        add_blank_line();
}

//���Content-Length����Ϣ��ͷ����Ӧ���ĵĳ���
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

//����ı����ͣ�������html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

//�������״̬��֪ͨ��������Ǳ������ӻ��ǹر�
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

//��ӿ���
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//�����Ӧ����
bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

//��Ҫ���͵�http��Ӧ���ĺ��ļ�׼����
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {   //״̬��
        add_status_line(500, error_500_title);
        //��Ϣ��ͷ
        add_headers(strlen(error_500_form));
        //��Ӧ����
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
        //�ж��������Դ�Ƿ����
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;//ָ��Ҫ���͵�http����
            m_iv[0].iov_len = m_write_idx;//���ĵĳ���
            m_iv[1].iov_base = m_file_address;//��Դ�ļ����ڴ��ַ
            m_iv[1].iov_len = m_file_stat.st_size;//��Դ�ļ��Ĵ�С
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            //����������Դ��СΪ0���򷵻ؿհ�html�ļ�
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

//����http���ģ�������Ӧ���ģ�������Ӧ�Ŀͻ����׽��ּ���д�¼�
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        //�����¼������¶�ȡ
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        //�����ˣ��ر�����
        close_conn();
    }
    //����ע���m_sockfd��������д�¼�
    //������������������ӿ��ú��һ���������������£�
    //����epoll_ctl��fd����ע�ᵽepoll�¼���(ʹ��EPOLL_CTL_MOD)����ʱҲ�ᴥ��EPOLLOUTʱ�䡣
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}