#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

connection_pool* connection_pool::GetInstance()
{
	static connection_pool connPool;//���ݿ����ӳض���
	return &connPool;
}

//�����ʼ��
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;//������ַ
	m_Port = Port;//���ݿ�˿ں�
	m_User = User;//�û���
	m_PassWord = PassWord;//����
	m_DatabaseName = DBName;//���ݿ���
	m_close_log = close_log;//��־����

	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL* con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		connList.push_back(con);//���ӳ�������������ݿ�Ķ���
		++m_FreeConn;
	}

	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//��������ʱ�������ݿ����ӳ��з���һ���������ӣ�����ʹ�úͿ���������
MYSQL* connection_pool::GetConnection()
{
	MYSQL* con = NULL;

	if (0 == connList.size())//�޿�������
		return NULL;

	reserve.wait();//��һ��������������У���������

	lock.lock();

	con = connList.front();//��ȡ�����һ��
	connList.pop_front();//������һ��Ԫ��

	--m_FreeConn;//������������һ
	++m_CurConn;//��ʹ����������һ

	lock.unlock();
	return con;
}

//�ͷŵ�ǰʹ�õ�����
bool connection_pool::ReleaseConnection(MYSQL* con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);//��con���¼��뵽������
	++m_FreeConn;//������������
	--m_CurConn;//ʹ�����Ӽ���

	lock.unlock();

	reserve.post();//sem��1
	return true;
}

//�������ݿ����ӳ�
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL*>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL* con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

//��ǰ���е�������
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

////ȡ��һ���������Ӷ���
connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {
	*SQL = connPool->GetConnection();//���ؿ������Ӷ����ָ��

	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
	poolRAII->ReleaseConnection(conRAII);
}