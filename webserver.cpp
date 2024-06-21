#include "webserver.h"

WebServer::WebServer()//����ͻ��˿ռ估��ʱ�����Լ����ù���Ŀ¼
{
    //http_conn����󣬿ͻ������Ӷ���
    users = new http_conn[MAX_FD];

    //root�ļ���·��
    char server_path[200];
    //����ǰ����Ŀ¼�µľ���·�����Ƶ�server_path��
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char*)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);//��root��ӵ�m_root�Ľ�β

    //��ʱ��
    ////ÿ��һ���ͻ������ӣ��ͻ����һ����ʱ�������Զ�ʱ�������������ͻ��˵��������һ��
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

//��ʼ��
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
    int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;//�������˿ں�
    m_user = user;//���ݿ��û�
    m_passWord = passWord;//���ݿ�����
    m_databaseName = databaseName;//���ݿ���
    m_sql_num = sql_num;//���ݿ����ӳ�����
    m_thread_num = thread_num;//�̳߳�����
    m_log_write = log_write;//��־д�뷽ʽ
    m_OPT_LINGER = opt_linger;//���Źر�����
    m_TRIGMode = trigmode;//�������ģʽ
    m_close_log = close_log;//�Ƿ�ر���־��־
    m_actormodel = actor_model;//����ģ��ѡ��
}

//���ô���ģʽ
void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

//������־
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //��ʼ����־
        if (1 == m_log_write)//�첽
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

//��ʼ�����ݿ����ӳأ�����ȡ��
void WebServer::sql_pool()
{
    //��ʼ�����ݿ����ӳ�
    m_connPool = connection_pool::GetInstance();//��ȡ���ݿ����ӳؾ�̬����
    //����m_sql_num�����ݿ����Ӷ���
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //��ʼ�����ݿⲢ��ȡ����ȡ�����û����û���������
    users->initmysql_result(m_connPool);
}

//�����̳߳�
void WebServer::thread_pool()
{
    //�����̳߳�
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

//����listenfd, epoll, listenfd���뵽epoll,���ùر����ӷ�ʽ���ܵ���ע���ź��¼�
void WebServer::eventListen()
{
    //�����̻�������
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);//����socket
    assert(m_listenfd >= 0);

    //���Źر�����
    if (0 == m_OPT_LINGER)
    {
        //��������ݲ�����socket���ͻ���������ϵͳ������������Щ���ݸ��Է����ȴ���ȷ�ϣ�Ȼ�󷵻ء�
        struct linger tmp = { 0, 1 };
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        /*
            �����ӵĹر�����һ����ʱ�����socket���ͻ��������Բ������ݣ����̽���˯�ߣ�
            �ں˽��붨ʱ״̬ȥ����ȥ������Щ���ݡ��ڳ�ʱ֮ǰ������������ݶ��������ұ��Է�ȷ�ϣ�
            �ں���������FIN|ACK|FIN|ACK�ĸ��������رո����ӣ�close()�ɹ����ء������ʱ֮ʱ��
            ������Ȼδ�ܳɹ����ͼ���ȷ�ϣ�ǿ�ƹر�
        */
        struct linger tmp = { 1, 1 };
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    //��ַ�ṹ
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    //���ö˿ڸ���
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));//�󶨵�ַ�ṹ
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);//������������
    assert(ret >= 0);

    utils.init(TIMESLOT);//������С��ʱ��λ

    //epoll�����ں��¼���
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5); // ����������
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);//����׽���
    http_conn::m_epollfd = m_epollfd;

    //socketpair()�������ڴ���һ�������ġ��໥���ӵ��׽���
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);
    /*
    * SIGPIPE������
        ���ǿͻ��˳�����������˳���������Ϣ��Ȼ��رտͻ��ˣ�
        �������˷�����Ϣ��ʱ��ͻ��յ��ں˸���SIGPIPE�źš�
        �����źŵ�Ĭ�ϴ������SIGPIPE�źŵ�Ĭ��ִ�ж�����terminate(��ֹ���˳�),����client���˳���
        �����������˳����԰�SIGPIPE��ΪSIG_IGN
    */
    utils.addsig(SIGPIPE, SIG_IGN);
    /*
    * SIGALRM������
        ��Linuxϵͳ�£�ÿ�����̶���Ωһ��һ����ʱ�����ö�ʱ���ṩ������Ϊ��λ�Ķ�ʱ���ܡ�
        �ڶ�ʱ�����õĳ�ʱʱ�䵽��󣬵���alarm�Ľ��̽��յ�SIGALRM�źš�
    */
    utils.addsig(SIGALRM, utils.sig_handler, false);
    /*
    * SIGTERM
        �ڻ��� Linux �Ĳ���ϵͳ��������ֹ���̡�SIGTERM �ź��ṩ��һ�����ŵķ�ʽ����ֹ����
        ʹ���л���׼���رղ�ִ����������
        ������ĳЩ����¾ܾ��رա�Unix/Linux ���̿����Զ��ַ�ʽ���� SIGTERM�����������ͺ��ԡ�
        ���û�ִ�� kill ʱ������ϵͳ���ں�̨����̷���
    */
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);//��ʱ��

    //������,�źź���������������
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

//Ϊ�ͻ��˳�ʼ����ʱ��
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    //��ʼ��������Դ��
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //��ʼ��client_data����
    //������ʱ�������ûص������ͳ�ʱʱ�䣬���û����ݣ�����ʱ����ӵ�������
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    util_timer* timer = new util_timer;//������ʱ������
    timer->user_data = &users_timer[connfd];//��ʼ��������Դ
    timer->cb_func = cb_func;//�󶨻ص�����
    time_t cur = time(NULL);//������ǰʱ���
    //��ʱ����ʱʱ�� = ������ͷ���������ʱ�� + �̶�ʱ��(TIMESLOT)��
    //���Կ�������ʱ��ʹ�þ���ʱ����Ϊ��ʱֵ������alarm����Ϊ5�룬���ӳ�ʱΪ15�롣
    timer->expire = cur + 3 * TIMESLOT;
    //���ö�ʱ������
    users_timer[connfd].timer = timer;

    //���ͻ��˵Ķ�ʱ����ӵ�˫�������У��Լ���
    utils.m_timer_lst.add_timer(timer);
}

//�������ݴ��䣬�򽫶�ʱ�������ӳ�3����λ
//�����µĶ�ʱ���������ϵ�λ�ý��е���
void WebServer::adjust_timer(util_timer* timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

//�رն�Ӧ�ͻ��˵ļ������رն�Ӧ�ͻ��˵��ļ�������������Ӧ�Ķ�ʱ���Ӷ�ʱ��������˫������ɾ��
void WebServer::deal_timer(util_timer* timer, int sockfd)
{
    //�رն�Ӧ�ͻ��˵ļ������رն�Ӧ�ͻ��˵��ļ�������
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        //����Ӧ�Ķ�ʱ���Ӷ�ʱ��������˫������ɾ��
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//���ӿͻ��ˣ������䶨ʱ��
bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)//Ϊ0��ʾLT����
    {
        int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);//��ͻ��˽�������
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)//���ӵĿͻ����������������ֵ
        {
            utils.show_error(connfd, "Internal server busy");//�����������ӵĿͻ��˷��������Ѵ����޵���Ϣ
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);//��ʼ��ĳһ�ͻ��˶�ʱ��
    }

    else
    {
        //���ʹ��while�����Ǵ�����accept�¼���ֱ��һֱ��ͣ��acceptֱ��accept����-1��Ҳ����û�����������ˣ�����Ч�ʸ��ߡ�
        while (1)//ET�����������whileѭ�����������пͻ��������ϵ����
        {
            int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

//����timeout(��ʱʱ��)��stop_server(�������رձ�־)
bool WebServer::dealwithsignal(bool& timeout, bool& stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)//����
    {
        return false;
    }
    else if (ret == 0)//������������
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM://��ʱ
            {
                timeout = true;
                break;
            }
            case SIGTERM://�رշ����
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

//���ݲ�ͬ����ģʽ��������յ�����
void WebServer::dealwithread(int sockfd)
{
    util_timer* timer = users_timer[sockfd].timer;

    //reactor����ģʽ
    if (1 == m_actormodel)
    {
        if (timer)
        {
            //�����ÿͻ��˵Ķ�ʱ��
            adjust_timer(timer);
        }

        //����⵽���¼��������¼������������
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)//Ϊfalse��ʾ����û�д������
            {
                if (1 == users[sockfd].timer_flag)//Ϊtrue��ʾ������ʧ��
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor����ģʽ
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //����⵽���¼��������¼������������
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                ////�����ÿͻ��˵Ķ�ʱ��
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

//���ݲ�ͬ����ģʽ�����͵�����
void WebServer::dealwithwrite(int sockfd)
{
    util_timer* timer = users_timer[sockfd].timer;
    //reactor����ģʽ
    if (1 == m_actormodel)
    {
        if (timer)
        {
            //�����ÿͻ��˵Ķ�ʱ��
            adjust_timer(timer);
        }
        //����⵽д�¼��������¼������������
        m_pool->append(users + sockfd, 1);

        //�ȴ����߳����������
        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                //�����ÿͻ��˵Ķ�ʱ��
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

//���з�����
void WebServer::eventLoop()
{
    //��ʱ��־
    bool timeout = false;
    //ѭ������
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);//�����¼�
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //�����µ��Ŀͻ�����
            if (sockfd == m_listenfd)
            {
                //���ӿͻ��ˣ������䶨ʱ��
                //true��ʾ���ӳɹ�
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            /*
                  EPOLLHUP����ʾ��Ӧ���ļ����������Ҷϣ�
                  EPOLLERR����ʾ��Ӧ���ļ���������������
                  EPOLLRDHUP�����ж��Ƿ�Զ��Ѿ��رգ�
                  �жϼ��������¼��Ƿ�Ϊ�����е�һ��������رնԶ˵�����
            */
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //�������˹ر����ӣ��Ƴ���Ӧ�Ķ�ʱ��
                util_timer* timer = users_timer[sockfd].timer;//��ȡ�Զ˶�ʱ��
                deal_timer(timer, sockfd);//�ر�������Ӻ����������Դ
            }
            //�����ź�
            //������������ǹܵ��Ķ����Ҽ����¼�ΪEPOLLIN��
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                //����timeout��stop_server
                //��ʱ��رշ����
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //����ͻ������Ͻ��յ�������
            else if (events[i].events & EPOLLIN)
            {
                //���ݲ�ͬ����ģʽ��������յ�����
                dealwithread(sockfd);
            }
            //�����͸��ͻ�������
            else if (events[i].events & EPOLLOUT)
            {
                //���ݲ�ͬ����ģʽ�����͵�����
                dealwithwrite(sockfd);
            }
        }
        /*
            ���̵߳���alarm(5),5�������SIGALRM�źţ��ͻ�����Զ�����źŴ�������
            ��SIGALRM�ź�ֵ�����ܵ�����ʱ����������ܵ����¼����ͻ����¼�������
            ��ʱ�����dealwithsignal������������timeout��Ϊtrue��
            ����true�ͻ�֪���ѹ�ȥ5�룬����timer_handler�鿴��ʱ���������Ƿ��г�ʱ�Ŀͻ�������
        */
        if (timeout)//Ϊtrue��ʾ��ʱ
        {
            //��ʱ�����������¶�ʱ�Բ��ϴ���SIGALRM�ź�
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            
            timeout = false;
        }
    }
}
