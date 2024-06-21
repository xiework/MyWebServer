#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char* file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;//设置异步方式，同时m_is_async为true
        //用于异步写入日志，将要写入的消息存放入队列，由子线程从队列中取出消息写入日志中
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;//子线程id
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log = close_log;//关闭日志标志
    m_log_buf_size = log_buf_size;//日志缓冲区大小
    m_buf = new char[m_log_buf_size];//日志缓冲区
    memset(m_buf, '\0', m_log_buf_size);//初始化为0
    m_split_lines = split_lines;//日志最大行数

    time_t t = time(NULL);
    /*
        struct tm {
        int tm_sec;         秒，范围从 0 到 59                
        int tm_min;         分，范围从 0 到 59                
        int tm_hour;        小时，范围从 0 到 23                
        int tm_mday;        一月中的第几天，范围从 1 到 31                    
        int tm_mon;         月份，范围从 0 到 11               
        int tm_year;        自 1900 起的年数                
        int tm_wday;        一周中的第几天，范围从 0 到 6             
        int tm_yday;        一年中的第几天，范围从 0 到 365                    
        int tm_isdst;       夏令时                       
        };
    */
    struct tm* sys_tm = localtime(&t);//使用t的值来填充 tm 结构。t的值被分解为 tm 结构，并用本地时区表示。
    struct tm my_tm = *sys_tm;

    //获取日志文件
    const char* p = strrchr(file_name, '/');//file_name 为日志文件存放的目录
    char log_full_name[256] = { 0 };//日志文件名

    if (p == NULL)//没有指定目录，则在当前文件夹下创建日志文件
    {
        //添加年月日后拷贝到log_full_name
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else//指定了日志文件
    {
        strcpy(log_name, p + 1);//+1去掉“/”
        strncpy(dir_name, file_name, p - file_name + 1);//获取日志文件目录
        //log_full_name = dir_name + 年_月_日 + log_name
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;//记录当前日期

    m_fp = fopen(log_full_name, "a");//以追加方式打开日志文件
    if (m_fp == NULL)//打开失败
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char* format, ...)
{
    struct timeval now = { 0, 0 };
    gettimeofday(&now, NULL);//获取当前时间的时间戳
    time_t t = now.tv_sec;
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = { 0 };
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;
    //两个条件，1.更新日志和创建日志的日期不是同一天。2.当前行数已达到最大
    //只要满足其中一个条件就要新建一个日志
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {

        char new_log[256] = { 0 };
        //刷新日志缓冲并，关闭当前日志
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = { 0 };
        //日志文件的日期前缀
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        //根据不同的条件来取不同的日志文件名
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        //新的日志文件名
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    va_list valst;//可变参数队列
    va_start(valst, format);//获取可变参数

    string log_str;
    m_mutex.lock();

    //以下都是要写入的内容
    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
        my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
        my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())//如果是异步日志，则将写入内容加入到队列中
    {
        m_log_queue->push(log_str);
    }
    else//同步日志，则直接写入文件中
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}