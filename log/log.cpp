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

//�첽��Ҫ�����������еĳ��ȣ�ͬ������Ҫ����
bool Log::init(const char* file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //���������max_queue_size,������Ϊ�첽
    if (max_queue_size >= 1)
    {
        m_is_async = true;//�����첽��ʽ��ͬʱm_is_asyncΪtrue
        //�����첽д����־����Ҫд�����Ϣ�������У������̴߳Ӷ�����ȡ����Ϣд����־��
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;//���߳�id
        //flush_log_threadΪ�ص�����,�����ʾ�����߳��첽д��־
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log = close_log;//�ر���־��־
    m_log_buf_size = log_buf_size;//��־��������С
    m_buf = new char[m_log_buf_size];//��־������
    memset(m_buf, '\0', m_log_buf_size);//��ʼ��Ϊ0
    m_split_lines = split_lines;//��־�������

    time_t t = time(NULL);
    /*
        struct tm {
        int tm_sec;         �룬��Χ�� 0 �� 59                
        int tm_min;         �֣���Χ�� 0 �� 59                
        int tm_hour;        Сʱ����Χ�� 0 �� 23                
        int tm_mday;        һ���еĵڼ��죬��Χ�� 1 �� 31                    
        int tm_mon;         �·ݣ���Χ�� 0 �� 11               
        int tm_year;        �� 1900 �������                
        int tm_wday;        һ���еĵڼ��죬��Χ�� 0 �� 6             
        int tm_yday;        һ���еĵڼ��죬��Χ�� 0 �� 365                    
        int tm_isdst;       ����ʱ                       
        };
    */
    struct tm* sys_tm = localtime(&t);//ʹ��t��ֵ����� tm �ṹ��t��ֵ���ֽ�Ϊ tm �ṹ�����ñ���ʱ����ʾ��
    struct tm my_tm = *sys_tm;

    //��ȡ��־�ļ�
    const char* p = strrchr(file_name, '/');//file_name Ϊ��־�ļ���ŵ�Ŀ¼
    char log_full_name[256] = { 0 };//��־�ļ���

    if (p == NULL)//û��ָ��Ŀ¼�����ڵ�ǰ�ļ����´�����־�ļ�
    {
        //��������պ󿽱���log_full_name
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else//ָ������־�ļ�
    {
        strcpy(log_name, p + 1);//+1ȥ����/��
        strncpy(dir_name, file_name, p - file_name + 1);//��ȡ��־�ļ�Ŀ¼
        //log_full_name = dir_name + ��_��_�� + log_name
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;//��¼��ǰ����

    m_fp = fopen(log_full_name, "a");//��׷�ӷ�ʽ����־�ļ�
    if (m_fp == NULL)//��ʧ��
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char* format, ...)
{
    struct timeval now = { 0, 0 };
    gettimeofday(&now, NULL);//��ȡ��ǰʱ���ʱ���
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
    //д��һ��log����m_count++, m_split_lines�������
    m_mutex.lock();
    m_count++;
    //����������1.������־�ʹ�����־�����ڲ���ͬһ�졣2.��ǰ�����Ѵﵽ���
    //ֻҪ��������һ��������Ҫ�½�һ����־
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {

        char new_log[256] = { 0 };
        //ˢ����־���岢���رյ�ǰ��־
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = { 0 };
        //��־�ļ�������ǰ׺
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        //���ݲ�ͬ��������ȡ��ͬ����־�ļ���
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
        //�µ���־�ļ���
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    va_list valst;//�ɱ��������
    va_start(valst, format);//��ȡ�ɱ����

    string log_str;
    m_mutex.lock();

    //���¶���Ҫд�������
    //д��ľ���ʱ�����ݸ�ʽ
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
        my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
        my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())//������첽��־����д�����ݼ��뵽������
    {
        m_log_queue->push(log_str);
    }
    else//ͬ����־����ֱ��д���ļ���
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
    //ǿ��ˢ��д����������
    fflush(m_fp);
    m_mutex.unlock();
}