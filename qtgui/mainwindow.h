/*
 * mainwindow.h — lwftp Qt 界面客户端主窗口
 * 仅负责界面交互；所有 FTP 动作通过信号发给网络线程中的 LwftpEngine。
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class QLineEdit;
class QSpinBox;
class QPushButton;
class QPlainTextEdit;
class QProgressBar;
class QLabel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

signals:
    /* 发往引擎（跨线程自动排队） */
    void loginRequested(const QString &host, quint16 port,
                        const QString &user, const QString &pass);
    void logoutRequested();
    void downloadRequested(const QString &remotePath, const QString &localPath);
    void uploadRequested(const QString &remotePath, const QString &localPath);

public slots:
    /* 来自引擎 */
    void appendLog(const QString &text);
    void onLoggedIn();
    void onLoginFailed(const QString &reason);
    void onLoggedOut();
    void onXferStarted(const QString &kind);
    void onXferProgress(qint64 bytes);
    void onXferFinished(bool ok, const QString &message, qint64 bytes);

private slots:
    void onConnectClicked();
    void onBrowseClicked();
    void onDownloadClicked();
    void onUploadClicked();

private:
    void setBusy(bool busy);

    QLineEdit   *m_hostEdit;
    QSpinBox    *m_portSpin;
    QLineEdit   *m_userEdit;
    QLineEdit   *m_passEdit;
    QPushButton *m_connectBtn;
    QLineEdit   *m_remoteEdit;
    QLineEdit   *m_localEdit;
    QPushButton *m_browseBtn;
    QPushButton *m_downloadBtn;
    QPushButton *m_uploadBtn;
    QProgressBar *m_progress;
    QLabel      *m_bytesLabel;
    QPlainTextEdit *m_log;

    bool m_connected;
    bool m_transferring;
};

#endif /* MAINWINDOW_H */
