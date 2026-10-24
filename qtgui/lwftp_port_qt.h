/*
 * lwftp_port_qt.h — lwftp_port.h 的 Qt 实现附加的管理接口
 * （lwftp_port.h 契约之外的初始化/清理函数）
 */
#ifndef LWFTP_PORT_QT_H
#define LWFTP_PORT_QT_H

class QObject;

/* 指定网络线程对象：所有 QTcpSocket 与回调都运行在该线程 */
void lwf_qt_init(QObject *networkThreadObject);

/* 强制销毁所有现存连接（超时/退出时兜底） */
void lwf_qt_abort_all();

/* 接管库与移植层的调试输出（默认打到 stderr） */
typedef void (*lwf_qt_log_fn)(const char *line);
void lwf_qt_set_log(lwf_qt_log_fn fn);

#endif /* LWFTP_PORT_QT_H */
