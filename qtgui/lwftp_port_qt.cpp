/*
 * lwftp_port_qt.cpp — 用 QTcpSocket 实现 lwftp_port.h 移植接口。
 *
 * 设计要点：
 *  - 所有 QTcpSocket 都创建并运行于“网络线程”（lwf_qt_init 指定的对象所在线程）。
 *  - 收包投递：在 '\n' 处切分（保证 FTP 控制应答一行一次回调，避免 150/226
 *    粘包被库丢掉），普通数据按 32KB 上限切块；借用缓冲，回调期间有效。
 *  - 对端 FIN：先把残余缓冲全部投递，再以 data==NULL 回调表示 EOF；句柄
 *    不立即释放，等库调用 lwftp_port_close（与库的原生命周期一致）。
 *  - 致命错误：上报 err 回调后直接销毁（此后库不会再碰该句柄）。
 *  - 句柄统一在 QTcpSocket destroyed 时释放，杜绝重复释放。
 *  - 控制/数据排序：服务器按“数据 -> FIN -> 226”的顺序发送，226 到达时
 *    全部数据必然已到达本机；但 Qt 按 socket 分别调度，226 可能先被处理，
 *    库见到 226 会立刻关闭数据句柄丢弃尾部数据。因此只要数据连接“收过
 *    数据且尚未见到 FIN”，控制应答就推迟一拍事件循环，让数据先行投递。
 */
#include <QTcpSocket>
#include <QAbstractSocket>
#include <QHostAddress>
#include <QThread>
#include <QSet>
#include <QByteArray>
#include <QTimer>
#include <QMetaObject>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "../lwftp_port.h"
#include "lwftp_port_qt.h"

#define CHUNK_CAP 32768   /* 单次投递最大字节数 */
#define TX_MSS    32768   /* 上传块大小：库每收到一次 sent 确认就喂 mss 字节，
                           在途数据约 2×MSS；块太小会把传输变成事件往返拉锯。
                           上限约束：两次确认间隔的 drained 总量 < 2×MSS，
                           须 <= 65535（sent 回调 len 为 uint16），32768 正好卡满 */

struct lwftp_port_pcb {
    QTcpSocket *sock;
    QByteArray *buf;
    void *arg;
    lwftp_port_recv_fn      recv_cb;
    lwftp_port_sent_fn      sent_cb;
    lwftp_port_err_fn       err_cb;
    lwftp_port_connected_fn connected_cb;
    qint64   pending_sent;/* 已 drained 但未上报的字节数（门控累计，保证计数精确） */
    uint16_t mss;
    uint8_t  dead;        /* 致命错误已上报，等待销毁 */
    uint8_t  want_close;  /* close 已调用，等待断开后销毁 */
    uint8_t  fin_seen;    /* 对端 FIN 已投递 */
    uint8_t  is_control;  /* 会话中首个句柄（lwftp 先建控制连接） */
    uint8_t  saw_data;    /* 数据连接收到过字节（用于控制应答排序） */
};

static QSet<lwftp_port_pcb_t *> g_pcbs;
static QObject   *g_host = NULL;      /* 位于网络线程的对象，用于跨线程投递 */
static QThread   *g_netThread = NULL;
static lwf_qt_log_fn g_logfn = NULL;

extern "C" void lwftp_port_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    if (g_logfn) g_logfn(buf);
    else fprintf(stderr, "[lwftp] %s\n", buf);
}

void lwf_qt_set_log(lwf_qt_log_fn fn) { g_logfn = fn; }

extern "C" const char *lwftp_port_strerror(int err)
{
    switch (err) {
    case LWFTP_PORT_OK:       return "Ok.";
    case LWFTP_PORT_ERR_MEM:  return "Out of memory.";
    case LWFTP_PORT_ERR_BUF:  return "Buffer error.";
    case LWFTP_PORT_ERR_CONN: return "Not connected.";
    default:                  return "Unknown error.";
    }
}

extern "C" uint32_t lwftp_port_ip_from_str(const char *cp)
{
    unsigned int a, b, c, d;
    if (!cp) return 0;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

/* ---------------- 收包投递 ---------------- */

/* 投递接收数据：控制连接按行切分（FTP 应答一行一次回调），数据连接按
 * 32KB 大块投递（二进制内容里的 0x0A 不应造成碎块）；flushAll 时连同行尾
 * 残余一并投递 */
static void port_deliver(lwftp_port_pcb_t *pcb, bool flushAll)
{
    QByteArray *buf = pcb->buf;
    while (pcb->recv_cb && buf->size() > 0) {
        int n = 0;
        if (pcb->is_control) {
            int nl = buf->indexOf('\n');
            if (nl >= 0)     n = nl + 1;         /* 一整行应答 */
            else if (flushAll) n = buf->size();  /* 收尾残余 */
            else break;
        } else {
            n = qMin(buf->size(), CHUNK_CAP);    /* 数据连接：大块 */
        }
        /* 借用缓冲投递（回调期间有效）；回调可能关闭句柄（recv_cb 置 NULL） */
        const char *data = buf->constData();
        pcb->recv_cb(pcb->arg, pcb, data, (uint32_t)n);
        if (pcb->dead) return;
        buf->remove(0, n);
    }
}

/* 控制应答投递前的排序保证（详见文件头注释） */
static void port_try_deliver_control(lwftp_port_pcb_t *pcb)
{
    const auto pcbs = g_pcbs;   /* 拷贝遍历，投递期间集合不变 */
    for (lwftp_port_pcb_t *o : pcbs) {
        if (o == pcb || o->dead || !o->sock) continue;
        if (o->saw_data && o->recv_cb &&
            o->sock->state() == QAbstractSocket::ConnectedState) {
            /* 数据连接还在收数据且未见 FIN：推迟一拍，让数据先投递 */
            QTimer::singleShot(0, pcb->sock, [pcb]() {
                if (!pcb->dead && g_pcbs.contains(pcb))
                    port_try_deliver_control(pcb);
            });
            return;
        }
    }
    port_deliver(pcb, false);
}

static void port_on_ready(lwftp_port_pcb_t *pcb)
{
    if (pcb->dead) return;
    pcb->buf->append(pcb->sock->readAll());
    if (pcb->is_control) {
        port_try_deliver_control(pcb);
    } else {
        pcb->saw_data = 1;
        port_deliver(pcb, false);
    }
}

static void port_on_disconnected(lwftp_port_pcb_t *pcb)
{
    QTcpSocket *sock = pcb->sock;
    if (!pcb->dead) {
        /* Qt 只在读尽 FIN 之前的全部字节后才发 disconnected，此处取尽即可 */
        pcb->buf->append(sock->readAll());
        port_deliver(pcb, true);
        if (!pcb->dead && pcb->recv_cb && !pcb->fin_seen) {
            pcb->fin_seen = 1;
            pcb->recv_cb(pcb->arg, pcb, NULL, 0);      /* EOF */
        }
    }
    if (pcb->want_close) sock->deleteLater();          /* 本方主动关过 -> 收尾 */
    /* 否则等库里 lwftp_port_close 时销毁 */
}

static void port_on_error(lwftp_port_pcb_t *pcb, QAbstractSocket::SocketError e)
{
    if (pcb->dead) return;
    if (e == QAbstractSocket::RemoteHostClosedError) return;   /* 等同 FIN */
    pcb->dead = 1;
    lwftp_port_err_fn cb = pcb->err_cb;
    if (cb) cb(pcb->arg, LWFTP_PORT_ERR_CONN);     /* 库收到 err 后不再 close */
    pcb->sock->deleteLater();                      /* destroyed 时释放句柄 */
}

static void port_on_destroyed(lwftp_port_pcb_t *pcb)
{
    g_pcbs.remove(pcb);
    delete pcb->buf;
    delete pcb;
}

/* ---------------- 移植接口实现 ---------------- */

extern "C" lwftp_port_pcb_t *lwftp_port_new(void)
{
    lwftp_port_pcb_t *pcb = new lwftp_port_pcb_t;
    memset(pcb, 0, sizeof(*pcb));
    pcb->mss = TX_MSS;
    pcb->is_control = g_pcbs.isEmpty() ? 1 : 0;   /* lwftp 总是先建控制连接 */
    g_pcbs.insert(pcb);
    return pcb;
}

extern "C" void lwftp_port_arg(lwftp_port_pcb_t *pcb, void *arg)     { pcb->arg = arg; }
extern "C" void lwftp_port_recv(lwftp_port_pcb_t *pcb, lwftp_port_recv_fn f) { pcb->recv_cb = f; }
extern "C" void lwftp_port_sent(lwftp_port_pcb_t *pcb, lwftp_port_sent_fn f) { pcb->sent_cb = f; }
extern "C" void lwftp_port_err(lwftp_port_pcb_t *pcb, lwftp_port_err_fn f)   { pcb->err_cb = f; }
extern "C" uint16_t lwftp_port_mss(lwftp_port_pcb_t *pcb) { return pcb->mss; }

extern "C" int lwftp_port_connect(lwftp_port_pcb_t *pcb, uint32_t ip,
                                  uint16_t port, lwftp_port_connected_fn fn)
{
    QTcpSocket *sock = new QTcpSocket();   /* 当前线程即网络线程 */
    pcb->sock = sock;
    pcb->buf = new QByteArray;
    pcb->connected_cb = fn;

    QObject::connect(sock, &QTcpSocket::readyRead, sock, [pcb]() { port_on_ready(pcb); });
    QObject::connect(sock, &QTcpSocket::disconnected, sock, [pcb]() { port_on_disconnected(pcb); });
    /* Qt 5.14 中信号名为 error（5.15 起更名 errorOccurred） */
    QObject::connect(sock, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                     sock, [pcb](QAbstractSocket::SocketError e) { port_on_error(pcb, e); });
    QObject::connect(sock, &QTcpSocket::connected, sock, [pcb]() {
        if (!pcb->dead && pcb->connected_cb)
            pcb->connected_cb(pcb->arg, pcb, LWFTP_PORT_OK);
    });
    QObject::connect(sock, &QTcpSocket::bytesWritten, sock, [pcb](qint64 n) {
        if (pcb->dead || !pcb->sent_cb) return;
        pcb->pending_sent += n;
        /* 低水位门控：Qt 发送缓冲低于一个 MSS 时才上报确认（库随即泵下一块）。
           否则慢服务器下每 drained 一点就又写一大块，缓冲会无界膨胀。
           两次上报间隔内 drained 总量 < 2×MSS，uint16 计数不会溢出。 */
        if (pcb->sock->bytesToWrite() < pcb->mss) {
            qint64 total = pcb->pending_sent;
            pcb->pending_sent = 0;
            pcb->sent_cb(pcb->arg, pcb, (uint16_t)qMin<qint64>(total, 0xFFFF));
        }
    });
    QObject::connect(sock, &QObject::destroyed, sock, [pcb]() { port_on_destroyed(pcb); });

    sock->connectToHost(QHostAddress(ip).toString(), port);
    return LWFTP_PORT_OK;
}

extern "C" int lwftp_port_write(lwftp_port_pcb_t *pcb, const void *data, uint16_t len)
{
    if (pcb->dead) return LWFTP_PORT_ERR_CONN;
    QTcpSocket *sock = pcb->sock;
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) return LWFTP_PORT_ERR_CONN;
    qint64 n = sock->write((const char *)data, len);
    if (n < 0) return LWFTP_PORT_ERR_MEM;
    /* QTcpSocket 内部缓冲，通常一次全部收下；未收齐视为内存不足 */
    return (n == len) ? LWFTP_PORT_OK : LWFTP_PORT_ERR_MEM;
}

extern "C" void lwftp_port_flush(lwftp_port_pcb_t *pcb)
{
    if (!pcb->dead && pcb->sock) pcb->sock->flush();
}

extern "C" void lwftp_port_close(lwftp_port_pcb_t *pcb)
{
    if (pcb->dead) return;                        /* 已在销毁流程 */
    pcb->want_close = 1;
    pcb->sent_cb = NULL;   /* 关后不再泵数：余量 drained 的 bytesWritten 不应再
                            触发库的发送回调（库此时视句柄已关，会误关空指针） */
    QTcpSocket *sock = pcb->sock;
    if (!sock || sock->state() == QAbstractSocket::UnconnectedState) {
        sock->deleteLater();                      /* 已断开 -> 直接收尾 */
    } else {
        sock->disconnectFromHost();               /* 先发完缓冲再 FIN */
    }
}

extern "C" int lwftp_port_post(lwftp_port_task_fn fn, void *arg)
{
    if (g_netThread && QThread::currentThread() == g_netThread) {
        fn(arg);                                  /* 已在网络线程：直接执行 */
    } else if (g_host) {
        QMetaObject::invokeMethod(g_host, [fn, arg]() { fn(arg); }, Qt::QueuedConnection);
    } else {
        fn(arg);                                  /* 未初始化：退化为同步 */
    }
    return LWFTP_PORT_OK;
}

/* ---------------- 管理接口 ---------------- */

void lwf_qt_init(QObject *networkThreadObject)
{
    g_host = networkThreadObject;
    g_netThread = g_host ? g_host->thread() : NULL;
}

void lwf_qt_abort_all()
{
    /* 标记死亡并销毁全部句柄（其回调不会再被触发） */
    const auto pcbs = g_pcbs;
    for (lwftp_port_pcb_t *pcb : pcbs) {
        pcb->dead = 1;
        if (pcb->sock) pcb->sock->deleteLater();
        else port_on_destroyed(pcb);
    }
}
