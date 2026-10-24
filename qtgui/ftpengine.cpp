/*
 * ftpengine.cpp — 见 ftpengine.h
 *
 * 库的回调约定（lwftp.c）：
 *  - lwftp_connect : done(INPROGRESS) 同步返回前回调一次，登录成功后 done(LOGGED)；
 *                    失败则 done(ERR_xxx)（密码错误会走 QUIT->221->done(OK)，且无 LOGGED）。
 *  - retrieve/store: done(INPROGRESS) 一次；收到 226 -> done(OK)；
 *                    文件不存在 -> done(ERR_FILENAME)；其它异常 -> done(ERR_xxx)。
 *  - close         : 221 -> done(OK)，之后控制连接关闭。
 * data_sink(handle, NULL, 0) 表示数据连接 EOF；data_source(handle, NULL, n) 是发送确认。
 */
#include "ftpengine.h"
#include "lwftp_port_qt.h"

#include <QHostInfo>
#include <QHostAddress>
#include <QThread>

/* 把移植层的调试输出转发到界面日志（单实例应用，静态转发即可） */
static FtpEngine *g_logInst = nullptr;
static void port_log_forward(const char *line)
{
    if (g_logInst) emit g_logInst->logMessage(QString::fromUtf8(line));
}

FtpEngine::FtpEngine(QObject *parent)
    : QObject(parent), m_phase(PhIdle), m_bytes(0), m_finishReported(false)
{
    memset(&m_sess, 0, sizeof(m_sess));
    lwf_qt_init(this);                    /* 移植层需要知道网络线程对象 */
    g_logInst = this;
    lwf_qt_set_log(&port_log_forward);
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(30000);        /* 30 秒无任何进展判定超时 */
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        if (m_phase == PhRetr || m_phase == PhStor) {
            if (!m_finishReported) {
                m_finishReported = true;
                emit xferFinished(false, QStringLiteral("传输超时"), m_bytes);
            }
            teardown(QStringLiteral("传输超时，已断开"));
        } else if (m_phase == PhConnecting) {
            emit loginFailed(QStringLiteral("连接超时"));
            teardown(QStringLiteral("连接超时"));
        } else if (m_phase == PhClosing) {
            teardown(QString());
        }
    });
}

FtpEngine::~FtpEngine()
{
    lwf_qt_abort_all();
}

void FtpEngine::armWatchdog() { m_watchdog.start(); }

/* ---------------- 对外操作（均在本对象线程内执行） ---------------- */

void FtpEngine::login(const QString &host, quint16 port,
                      const QString &user, const QString &pass)
{
    if (m_phase != PhIdle) {
        emit logMessage(QStringLiteral("当前会话未结束，无法重复登录"));
        return;
    }
    /* 域名解析（本线程短暂阻塞，界面不受影响） */
    QHostAddress addr(host);
    if (addr.isNull()) {
        QHostInfo info = QHostInfo::fromName(host);
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            emit loginFailed(QStringLiteral("无法解析主机：") + host);
            return;
        }
        addr = info.addresses().first();
    }
    memset(&m_sess, 0, sizeof(m_sess));
    m_sess.server_ip = lwftp_port_ip_from_str(addr.toString().toLatin1().constData());
    if (!m_sess.server_ip) {
        emit loginFailed(QStringLiteral("仅支持 IPv4 地址"));
        return;
    }
    m_user = user.toUtf8();
    m_pass = pass.toUtf8();
    m_sess.server_port = port;
    m_sess.user = m_user.constData();
    m_sess.pass = m_pass.constData();
    m_sess.handle = this;
    m_sess.data_sink = &FtpEngine::cb_sink;
    m_sess.data_source = &FtpEngine::cb_source;
    m_sess.done_fn = &FtpEngine::cb_done;
    m_sess.timeout = 30;

    m_phase = PhConnecting;
    emit logMessage(QStringLiteral("正在连接 %1:%2 ...").arg(addr.toString()).arg(port));
    armWatchdog();
    lwftp_connect(&m_sess);   /* done_fn 会同步回调 INPROGRESS */
}

void FtpEngine::logout()
{
    if (m_phase == PhIdle) { emit loggedOut(); return; }
    if (m_phase == PhClosing) return;
    emit logMessage(QStringLiteral("正在断开..."));
    m_phase = PhClosing;
    armWatchdog();
    lwftp_close(&m_sess);     /* 发送 QUIT，等 221 */
}

void FtpEngine::download(const QString &remotePath, const QString &localPath)
{
    startTransfer(remotePath, localPath, false);
}

void FtpEngine::upload(const QString &remotePath, const QString &localPath)
{
    startTransfer(remotePath, localPath, true);
}

void FtpEngine::startTransfer(const QString &remote, const QString &local, bool isUpload)
{
    if (m_phase != PhLoggedIn) {
        emit xferFinished(false, QStringLiteral("尚未登录"), 0);
        return;
    }
    m_file.setFileName(local);
    if (!m_file.open(isUpload ? QIODevice::ReadOnly : QIODevice::WriteOnly)) {
        emit xferFinished(false, QStringLiteral("打不开本地文件：") + local, 0);
        return;
    }
    m_remote = remote.toUtf8();
    m_sess.remote_path = m_remote.constData();
    m_bytes = 0;
    m_lastReport = 0;
    m_finishReported = false;
    m_phase = isUpload ? PhStor : PhRetr;
    emit xferStarted(isUpload ? QStringLiteral("上传") : QStringLiteral("下载"));
    armWatchdog();
    if (isUpload) lwftp_store(&m_sess);
    else          lwftp_retrieve(&m_sess);
}

/* ---------------- 库回调 ---------------- */

void FtpEngine::cb_done(void *handle, int result)
{
    ((FtpEngine *)handle)->onDone(result);
}

unsigned FtpEngine::cb_sink(void *handle, const char *data, unsigned len)
{
    return ((FtpEngine *)handle)->onSink(data, len);
}

unsigned FtpEngine::cb_source(void *handle, const char **data, unsigned len)
{
    return ((FtpEngine *)handle)->onSource(data, len);
}

unsigned FtpEngine::onSink(const char *data, unsigned len)
{
    armWatchdog();
    if (!data) {                       /* 数据连接 EOF */
        if (m_file.isOpen()) m_file.flush();
        return 0;
    }
    if (m_file.isOpen()) {
        qint64 n = m_file.write(data, len);
        if (n != (qint64)len) {
            emit logMessage(QStringLiteral("写本地文件失败"));
            return 0;
        }
    }
    m_bytes += len;
    reportProgress();
    return len;
}

void FtpEngine::reportProgress()
{
    /* 限流：每 256KB 才发一次进度，避免跨线程信号洪峰拖慢界面 */
    if (m_bytes - m_lastReport >= 256 * 1024) {
        m_lastReport = m_bytes;
        emit xferProgress(m_bytes);
    }
}

unsigned FtpEngine::onSource(const char **data, unsigned len)
{
    armWatchdog();
    if (!data) {                       /* 发送确认：len 为已确认字节数 */
        m_bytes += len;
        reportProgress();
        return 0;
    }
    if (!m_file.isOpen()) return 0;
    qint64 n = m_file.read(m_txBuf, qMin<unsigned>(len, sizeof(m_txBuf)));
    if (n <= 0) return 0;              /* 文件读完，库会关闭数据连接 */
    *data = m_txBuf;
    return (unsigned)n;
}

void FtpEngine::onDone(int result)
{
    armWatchdog();
    switch (m_phase) {
    case PhConnecting:
        if (result == LWFTP_RESULT_LOGGED) {
            m_phase = PhLoggedIn;
            m_watchdog.stop();
            emit logMessage(QStringLiteral("登录成功"));
            emit loggedIn();
        } else if (result == LWFTP_RESULT_INPROGRESS) {
            /* 连接进行中 */
        } else {
            /* 含密码错误（库会以 QUIT->221->done(OK) 收尾，但没收到 LOGGED） */
            m_phase = PhIdle;
            m_watchdog.stop();
            QString why = (result == LWFTP_RESULT_OK)
                    ? QStringLiteral("登录被拒绝（用户名或密码错误）")
                    : QString::fromUtf8(resultText(result));
            emit loginFailed(why);
            emit loggedOut();
        }
        break;

    case PhRetr:
    case PhStor:
        if (result == LWFTP_RESULT_INPROGRESS) break;
        if (m_finishReported) break;               /* 超时已上报过 */
        m_finishReported = true;
        if (result == LWFTP_RESULT_OK) {
            /* 上传收尾时最后一块的 sent 确认可能被 close 截掉（移植层关后
               不再泵数），按本地文件大小对齐最终字节数 */
            if (m_phase == PhStor && m_file.isOpen())
                m_bytes = m_file.size();
            finishTransfer(true, QStringLiteral("完成"));
        } else {
            finishTransfer(false, QString::fromUtf8(resultText(result)));
        }
        break;

    case PhClosing:
        m_watchdog.stop();
        m_phase = PhIdle;
        emit logMessage(QStringLiteral("已断开"));
        emit loggedOut();
        break;

    default:
        break;
    }
}

void FtpEngine::finishTransfer(bool ok, const QString &message)
{
    if (m_file.isOpen()) m_file.close();
    m_watchdog.stop();
    emit xferFinished(ok, message, m_bytes);
    /* 传输结束后库回到 LWFTP_LOGGED，可继续下一次传输；
       若控制连接已断（如 ERR_CLOSED），库状态为 CLOSED，会话结束 */
    if (m_sess.control_state == LWFTP_CLOSED) {
        m_phase = PhIdle;
        emit loggedOut();
    } else {
        m_phase = PhLoggedIn;
    }
}

void FtpEngine::teardown(const QString &reason)
{
    m_watchdog.stop();
    if (!reason.isEmpty()) emit logMessage(reason);
    lwf_qt_abort_all();                  /* 销毁全部句柄，回调不再触发 */
    if (m_file.isOpen()) m_file.close();
    memset(&m_sess, 0, sizeof(m_sess));
    m_phase = PhIdle;
    emit loggedOut();
}

const char *FtpEngine::resultText(int result)
{
    switch (result) {
    case LWFTP_RESULT_OK:           return "OK";
    case LWFTP_RESULT_INPROGRESS:   return "INPROGRESS";
    case LWFTP_RESULT_LOGGED:       return "LOGGED";
    case LWFTP_RESULT_ERR_UNKNOWN:  return "未知错误";
    case LWFTP_RESULT_ERR_ARGUMENT: return "参数错误";
    case LWFTP_RESULT_ERR_MEMORY:   return "内存不足";
    case LWFTP_RESULT_ERR_CONNECT:  return "连接服务器失败";
    case LWFTP_RESULT_ERR_HOSTNAME: return "主机名解析失败";
    case LWFTP_RESULT_ERR_CLOSED:   return "连接被服务器关闭";
    case LWFTP_RESULT_ERR_TIMEOUT:  return "连接超时";
    case LWFTP_RESULT_ERR_SRVR_RESP:return "服务器应答异常";
    case LWFTP_RESULT_ERR_INTERNAL: return "内部错误";
    case LWFTP_RESULT_ERR_LOCAL:    return "本地存储错误";
    case LWFTP_RESULT_ERR_FILENAME: return "服务器上找不到该文件";
    default:                        return "未知结果";
    }
}
