/*
 * lwftp 的下载窗口。
 * 填写服务器、账号、远程路径和本地文件后，点“下载”即可。
 * 网络全部交给 LwftpClient，窗口只显示日志和进度。
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class LwftpClient;

/* 主窗口。一个会话对应一个 LwftpClient。 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);

private slots:
    /* 弹出保存对话框，把选中的路径写回本地文件框。 */
    void onBrowse();
    /* 读取输入并发起一次下载。下载过程中按钮关掉。 */
    void onDownload();
    /* 断开当前 FTP 会话。 */
    void onCloseSession();
    /* 把客户端日志追加到窗口。 */
    void onLog(const QString &line);
    /* 更新状态文字。 */
    void onPhase(const QString &text);
    /* 更新已接收字节数。bytes 是已经写入临时文件的字节。 */
    void onProgress(qint64 bytes);
    /* 下载成功，恢复按钮。 */
    void onSucceeded(const QString &localPath, qint64 bytes);
    /* 下载或登录失败，恢复按钮。 */
    void onFailed(int result, const QString &reason);
    /* 会话关掉且当前没有下载时，恢复按钮。 */
    void onSessionClosed();

private:
    QLineEdit *m_hostEdit;       /* 服务器 IP 或域名 */
    QSpinBox *m_portSpin;        /* 控制端口，默认 21 */
    QLineEdit *m_userEdit;       /* FTP 用户名 */
    QLineEdit *m_passEdit;       /* FTP 口令，输入时不回显 */
    QLineEdit *m_remoteEdit;     /* 远程文件路径，作为 RETR 参数 */
    QLineEdit *m_localEdit;      /* 保存到本地的完整路径 */
    QPushButton *m_browseBtn;
    QPushButton *m_downloadBtn;
    QPushButton *m_closeBtn;
    QLabel *m_status;            /* 当前阶段 */
    QLabel *m_bytes;             /* 已接收字节 */
    QPlainTextEdit *m_log;
    LwftpClient *m_client;
};

#endif
