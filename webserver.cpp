#include "webserver.h"

WebServer::WebServer()//分配客户端空间及定时器，以及设置工作目录
{
    //http_conn类对象，客户端连接对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    //将当前工作目录下的绝对路径复制到server_path中
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char*)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);//将root添加到m_root的结尾

    //定时器
    ////每有一个客户端连接，就会产生一个定时器，所以定时器的最大数量与客户端的最大数量一样
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

//初始化
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
    int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;//服务器端口号
    m_user = user;//数据库用户
    m_passWord = passWord;//数据库密码
    m_databaseName = databaseName;//数据库名
    m_sql_num = sql_num;//数据库连接池数量
    m_thread_num = thread_num;//线程池数量
    m_log_write = log_write;//日志写入方式
    m_OPT_LINGER = opt_linger;//优雅关闭链接
    m_TRIGMode = trigmode;//触发组合模式
    m_close_log = close_log;//是否关闭日志标志
    m_actormodel = actor_model;//并发模型选择
}

//设置触发模式
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

//开启日志
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)//异步
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

//初始化数据库连接池，并读取表
void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();//获取数据库连接池静态对象
    //创建m_sql_num个数据库连接对象
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库并读取表，获取所有用户的用户名和密码
    users->initmysql_result(m_connPool);
}

//创建线程池
void WebServer::thread_pool()
{
    //创建线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

//创建listenfd, epoll, listenfd加入到epoll,设置关闭连接方式，管道，注册信号事件
void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);//创建socket
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        //如果有数据残留在socket发送缓冲区中则系统将继续发送这些数据给对方，等待被确认，然后返回。
        struct linger tmp = { 0, 1 };
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        /*
            将连接的关闭设置一个超时。如果socket发送缓冲区中仍残留数据，进程进入睡眠，
            内核进入定时状态去尽量去发送这些数据。在超时之前，如果所有数据都发送完且被对方确认，
            内核用正常的FIN|ACK|FIN|ACK四个分组来关闭该连接，close()成功返回。如果超时之时，
            数据仍然未能成功发送及被确认，强制关闭
        */
        struct linger tmp = { 1, 1 };
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    //地址结构
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    //设置端口复用
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));//绑定地址结构
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);//设置连接上限
    assert(ret >= 0);

    utils.init(TIMESLOT);//设置最小超时单位

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5); // 创建监听树
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);//添加套接字
    http_conn::m_epollfd = m_epollfd;

    //socketpair()函数用于创建一对无名的、相互连接的套接子
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);
    /*
    * SIGPIPE产生：
        就是客户端程序向服务器端程序发送了消息，然后关闭客户端，
        服务器端返回消息的时候就会收到内核给的SIGPIPE信号。
        根据信号的默认处理规则SIGPIPE信号的默认执行动作是terminate(终止、退出),所以client会退出。
        若不想服务端退出可以把SIGPIPE设为SIG_IGN
    */
    utils.addsig(SIGPIPE, SIG_IGN);
    /*
    * SIGALRM产生：
        在Linux系统下，每个进程都有惟一的一个定时器，该定时器提供了以秒为单位的定时功能。
        在定时器设置的超时时间到达后，调用alarm的进程将收到SIGALRM信号。
    */
    utils.addsig(SIGALRM, utils.sig_handler, false);
    /*
    * SIGTERM
        在基于 Linux 的操作系统中用于终止进程。SIGTERM 信号提供了一种优雅的方式来终止程序，
        使其有机会准备关闭并执行清理任务，
        或者在某些情况下拒绝关闭。Unix/Linux 进程可以以多种方式处理 SIGTERM，包括阻塞和忽略。
        当用户执行 kill 时，操作系统会在后台向进程发送
    */
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);//定时器

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

//为客户端初始化定时器
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    //初始化连接资源类
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    util_timer* timer = new util_timer;//创建定时器对象
    timer->user_data = &users_timer[connfd];//初始化连接资源
    timer->cb_func = cb_func;//绑定回调函数
    time_t cur = time(NULL);//创建当前时间戳
    //定时器超时时间 = 浏览器和服务器连接时刻 + 固定时间(TIMESLOT)，
    //可以看出，定时器使用绝对时间作为超时值，这里alarm设置为5秒，连接超时为15秒。
    timer->expire = cur + 3 * TIMESLOT;
    //设置定时器属性
    users_timer[connfd].timer = timer;

    //将客户端的定时器添加到双向链表中，以监视
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer* timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

//关闭对应客户端的监听，关闭对应客户端的文件描述符，将对应的定时器从定时器容器（双向链表）删除
void WebServer::deal_timer(util_timer* timer, int sockfd)
{
    //关闭对应客户端的监听，关闭对应客户端的文件描述符
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        //将对应的定时器从定时器容器（双向链表）删除
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//连接客户端，设置其定时器
bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)//为0表示LT触发
    {
        int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);//与客户端建立连接
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)//连接的客户端数量超过了最大值
        {
            utils.show_error(connfd, "Internal server busy");//向发送请求连接的客户端发送连接已达上限的消息
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);//初始化某一客户端定时器
    }

    else
    {
        //如果使用while，我们触发了accept事件就直接一直不停的accept直到accept返回-1，也就是没有新连接来了，这样效率更高。
        while (1)//ET触发，如果不while循环，则会出现有客户端连不上的情况
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

//更新timeout(超时时长)和stop_server(服务器关闭标志)
bool WebServer::dealwithsignal(bool& timeout, bool& stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)//出错
    {
        return false;
    }
    else if (ret == 0)//缓冲区无数据
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM://超时
            {
                timeout = true;
                break;
            }
            case SIGTERM://关闭服务端
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

//根据不同并发模式来处理接收的数据
void WebServer::dealwithread(int sockfd)
{
    util_timer* timer = users_timer[sockfd].timer;

    //reactor并发模式
    if (1 == m_actormodel)
    {
        if (timer)
        {
            //调整该客户端的定时器
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)//为false表示任务没有处理完成
            {
                if (1 == users[sockfd].timer_flag)//为true表示任务处理失败
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
        //proactor并发模式
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                ////调整该客户端的定时器
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

//根据不同并发模式来发送的数据
void WebServer::dealwithwrite(int sockfd)
{
    util_timer* timer = users_timer[sockfd].timer;
    //reactor并发模式
    if (1 == m_actormodel)
    {
        if (timer)
        {
            //调整该客户端的定时器
            adjust_timer(timer);
        }
        //若监测到写事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 1);

        //等待子线程任务处理结束
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
                //调整该客户端的定时器
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

//运行服务器
void WebServer::eventLoop()
{
    //超时标志
    bool timeout = false;
    //循环条件
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);//监听事件
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                //连接客户端，设置其定时器
                //true表示连接成功
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            /*
                  EPOLLHUP：表示对应的文件描述符被挂断；
                  EPOLLERR：表示对应的文件描述符发生错误；
                  EPOLLRDHUP：来判断是否对端已经关闭；
                  判断监听到的事件是否为三个中的一个，有则关闭对端的连接
            */
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer* timer = users_timer[sockfd].timer;//获取对端定时器
                deal_timer(timer, sockfd);//关闭相关连接和销毁相关资源
            }
            //处理信号
            //如果监听到的是管道的读端且监听事件为EPOLLIN，
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                //更新timeout或stop_server
                //超时或关闭服务端
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                //根据不同并发模式来处理接收的数据
                dealwithread(sockfd);
            }
            //处理发送给客户的数据
            else if (events[i].events & EPOLLOUT)
            {
                //根据不同并发模式来发送的数据
                dealwithwrite(sockfd);
            }
        }
        /*
            主线程调用alarm(5),5秒后会产生SIGALRM信号，就会掉用自定义的信号处理函数，
            将SIGALRM信号值传给管道，此时正监听这个管道读事件，就会有事件产生。
            此时会调用dealwithsignal（）函数，将timeout改为true。
            根据true就会知道已过去5秒，调用timer_handler查看定时器容器中是否有超时的客户端连接
        */
        if (timeout)//为true表示超时
        {
            //定时处理任务，重新定时以不断触发SIGALRM信号
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            
            timeout = false;
        }
    }
}
