#ifndef AIDA64_H
#define AIDA64_H

#include <stdint.h>

/* 启动 AIDA64 监控任务，连接指定 IP:port 的 AIDA64 RemoteSensor SSE */
void aida64_monitor_start(const char *ip, uint16_t port);

/* 停止监控 */
void aida64_monitor_stop(void);

/* 查询连接状态 */
int aida64_monitor_isconnect(void);

#endif /* AIDA64_H */
