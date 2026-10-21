/*
 * ftpengine.h — 在专用线程中驱动 lwftp 库的 Qt 封装。
 *
 * GUI 通过信号/槽（跨线程自动排队）调用 login/download/upload/logout；
 * 库的 done_fn / data_sink / data_source 回调在网络线程内转成 Qt 信号发回。
 */
#ifndef FTPENGINE_H
#define FTPENGINE_H

#include <QObject>
#include <QFile>
#include <QByteArray>
#include <QTimer>
#include "../lwftp.h"   /* lwftp 库（已去除 lwIP 依赖） */

class FtpEngine : public QObject
{
    Q_OBJECT
public:
    explicit FtpEngine(QObject *parent = nullptr);
    ~FtpEngine();

public slots:
    void login(const QString &host, quint16 port,
               const QString &user, const QString &pass);
    void logout();
    void download(const QString &remotePath, const QString &localPath);
    void upload(const QString &remotePath, const QString &localPath);

signals:
    void logMessage(const QString &text);                 /* 运行日志 */
    void loggedIn();                                      /* 登录成功 */
    void loginFailed(const QString &reason);              /* 登录/连接失败 */
    void loggedOut();                                     /* 会话结束（主动或异常） */
    void xferStarted(const QString &kind);                /* 传输开始: 下载/上传 */
    void xferProgress(qint64 bytes);                      /* 已传输字节数 */
    void xferFinished(bool ok, const QString &message, qint64 bytes);

private:
    enum Phase { PhIdle, PhConnecting, PhLoggedIn, PhRetr, PhStor, PhClosing };

    /* 库回调（C 桥接，handle 即 this） */
    static void     cb_done(void *handle, int result);
    static unsigned cb_sink(void *handle, const char *data, unsigned len);
    static unsigned cb_source(void *handle, const char **data, unsigned len);

    void onDone(int result);
    unsigned onSink(const char *data, unsigned len);
    unsigned onSource(const char **data, unsigned len);

    void startTransfer(const QString &remote, const QString &local, bool isUpload);
    void finishTransfer(bool ok, const QString &message);
    void teardown(const QString &reason);
    void armWatchdog();
    void reportProgress();
    static const char *resultText(int result);

    lwftp_session_t m_sess;     /* lwftp 库会话结构 */
    Phase     m_phase;
    QFile     m_file;
    QByteArray m_user, m_pass, m_remote;  /* 库只保存指针，内容由成员持有 */
    char      m_txBuf[2048];    /* 上传读盘缓冲（>= mss） */
    qint64    m_bytes;
    qint64    m_lastReport;     /* 上次进度信号时的字节数（限流用） */
    bool      m_finishReported; /* 防止超时与完成重复上报 */
    QTimer    m_watchdog;
};

#endif /* FTPENGINE_H */
