/*
 * lwftp_port.h : lwftp 的平台移植接口（纯 C）
 *
 * lwftp.c 只依赖本头文件声明的这一小组函数；在不同平台上移植时，
 * 只需用该平台的 TCP 设施实现这些函数即可（例如用 Qt 的 QTcpSocket，
 * 见 qtgui/lwftp_port_qt.cpp）。
 *
 * 线程模型：所有回调都在“网络线程”中触发；lwftp_port_post() 用于把
 * 函数调用投递到该线程执行。
 */
#ifndef LWFTP_PORT_H
#define LWFTP_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码 */
#define LWFTP_PORT_OK        0    /* 成功 */
#define LWFTP_PORT_ERR_MEM  -1    /* 内存不足 */
#define LWFTP_PORT_ERR_BUF  -2    /* 数据格式错误 */
#define LWFTP_PORT_ERR_CONN -3    /* 连接失败/未连接 */

/* 不透明连接句柄（协议控制块），由移植层定义 */
typedef struct lwftp_port_pcb lwftp_port_pcb_t;

/* 接收回调：data==NULL 表示对端关闭（EOF）；data 仅在回调期间有效 */
typedef void (*lwftp_port_recv_fn)(void *arg, lwftp_port_pcb_t *pcb,
                                   const char *data, uint32_t len);
/* 发送确认回调：len 为本次被确认的字节数 */
typedef void (*lwftp_port_sent_fn)(void *arg, lwftp_port_pcb_t *pcb, uint16_t len);
/* 错误回调：调用后句柄即失效，无需也不许再 close */
typedef void (*lwftp_port_err_fn)(void *arg, int err);
/* 连接完成回调：err==LWFTP_PORT_OK 表示成功 */
typedef void (*lwftp_port_connected_fn)(void *arg, lwftp_port_pcb_t *pcb, int err);

/* 点分十进制 -> IPv4（主机字节序），失败返回 0 */
uint32_t lwftp_port_ip_from_str(const char *cp);

lwftp_port_pcb_t *lwftp_port_new(void);
void  lwftp_port_arg(lwftp_port_pcb_t *pcb, void *arg);
void  lwftp_port_recv(lwftp_port_pcb_t *pcb, lwftp_port_recv_fn fn);
void  lwftp_port_sent(lwftp_port_pcb_t *pcb, lwftp_port_sent_fn fn);
void  lwftp_port_err(lwftp_port_pcb_t *pcb, lwftp_port_err_fn fn);

/* 发起连接（异步），结果经 connected/err 回调上报 */
int   lwftp_port_connect(lwftp_port_pcb_t *pcb, uint32_t ip, uint16_t port,
                         lwftp_port_connected_fn fn);
/* 发送数据，成功返回 LWFTP_PORT_OK */
int   lwftp_port_write(lwftp_port_pcb_t *pcb, const void *data, uint16_t len);
/* 立刻把缓冲的数据尽量发出去（可选实现，允许为空操作） */
void  lwftp_port_flush(lwftp_port_pcb_t *pcb);
/* 关闭并释放句柄：先发完缓冲数据再 FIN；之后句柄失效 */
void  lwftp_port_close(lwftp_port_pcb_t *pcb);
/* 最大报文段长度，用于上层决定每次喂多少数据 */
uint16_t lwftp_port_mss(lwftp_port_pcb_t *pcb);

/* 把 fn(arg) 投递到网络线程执行（若已在网络线程则直接执行） */
typedef void (*lwftp_port_task_fn)(void *arg);
int   lwftp_port_post(lwftp_port_task_fn fn, void *arg);

/* 错误码描述（仅用于日志） */
const char *lwftp_port_strerror(int err);
/* 调试输出（移植层默认打到 stderr，应用可接管） */
void  lwftp_port_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* LWFTP_PORT_H */
