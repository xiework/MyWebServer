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
	static connection_pool connPool;//数据库连接池对象
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;//主机地址
	m_Port = Port;//数据库端口号
	m_User = User;//用户名
	m_PassWord = PassWord;//密码
	m_DatabaseName = DBName;//数据库名
	m_close_log = close_log;//日志开关

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
		connList.push_back(con);//连接池添加已连接数据库的对象
		++m_FreeConn;
	}

	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL* connection_pool::GetConnection()
{
	MYSQL* con = NULL;

	if (0 == connList.size())//无可用连接
		return NULL;

	reserve.wait();//减一，如果大于则运行，否则阻塞

	lock.lock();

	con = connList.front();//获取链表第一个
	connList.pop_front();//弹出第一个元素

	--m_FreeConn;//空闲连接数减一
	++m_CurConn;//已使用连接数加一

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL* con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);//将con重新加入到链表中
	++m_FreeConn;//空闲连接增加
	--m_CurConn;//使用连接减少

	lock.unlock();

	reserve.post();//sem加1
	return true;
}

//销毁数据库连接池
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

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

////取出一个空闲连接对象
connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {
	*SQL = connPool->GetConnection();//返回空闲连接对象的指针

	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
	poolRAII->ReleaseConnection(conRAII);
}