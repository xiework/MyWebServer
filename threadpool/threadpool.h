#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool* connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T* request, int state);//添加请求对象到请求队列中
    bool append_p(T* request);//添加请求对象到请求队列中

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t* m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T*> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool* m_connPool;  //数据库
    int m_actor_model;          //并发模型切换
};
template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool* connPool, int thread_number, int max_requests) : m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL), m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];//线程池数组
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)//创建线程
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))//设置分离模式，不需要主线程回收，可以自动完成
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

//添加请求对象到请求队列中
//request连接对象
template <typename T>
bool threadpool<T>::append(T* request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;//请求队列达到上限
    }
    request->m_state = state;//更新读写状态（读还是写）
    m_workqueue.push_back(request);//将该连接对象添加到请求队列中
    m_queuelocker.unlock();
    m_queuestat.post();//唤醒等待处理任务的线程
    return true;
}

//添加请求对象到请求队列中
template <typename T>
bool threadpool<T>::append_p(T* request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

//子线程调用函数
template <typename T>
void* threadpool<T>::worker(void* arg)
{
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

//子线程执行
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();//为0表示没有任务，阻塞等待
        m_queuelocker.lock();
        if (m_workqueue.empty())//判断请求队列是否为空
        {
            m_queuelocker.unlock();
            continue;
        }
        //取出队首元素
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        if (1 == m_actor_model)//reactor并发模式
        {
            if (0 == request->m_state)//m_state为0表示读，为1表示写
            {
                if (request->read_once())//读取客户端发来的http请求报文
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);//取出一个可用数据库连接
                    request->process();//解析http报文，生成响应报文，并对客户端套接字监听写事件
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())//发送响应报文成功
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else//proactor并发模式
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);//取出一个可用数据库连接
            request->process();//解析http报文，生成响应报文，并对客户端套接字监听写事件
        }
    }
}
#endif
