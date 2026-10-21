/*
 * main.cpp — lwftp Qt 界面客户端入口。
 *
 * 普通模式：打开主窗口。
 * 自动测试模式（供命令行验证，不弹窗交互）：
 *   lwftp_gui --auto <host> <port> <user> <pass> <dl|ul> <remote> <local>
 * 结果以 "AUTOTEST RESULT ok=.. bytes=.." 打到标准输出。
 */
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <QTextStream>
#include "ftpengine.h"
#include "mainwindow.h"

/* 自动测试驱动：登录 -> 传输 -> 退出 */
class AutoTest : public QObject
{
    Q_OBJECT
public:
    AutoTest(FtpEngine *engine, const QStringList &a, QObject *parent = nullptr)
        : QObject(parent), m_engine(engine)
    {
        m_host = a.value(0); m_port = (quint16)a.value(1).toUShort();
        m_user = a.value(2); m_pass = a.value(3);
        m_upload = (a.value(4) == "ul");
        m_remote = a.value(5); m_local = a.value(6);

        connect(m_engine, &FtpEngine::loggedIn, this, [this]() {
            out() << "AT: logged in" << '\n';
            if (m_upload) emit requestUpload(m_remote, m_local);
            else          emit requestDownload(m_remote, m_local);
        });
        connect(m_engine, &FtpEngine::loginFailed, this, [this](const QString &r) {
            out() << "AUTOTEST RESULT ok=0 stage=login reason=" << r << '\n';
            QCoreApplication::exit(2);
        });
        connect(m_engine, &FtpEngine::xferFinished, this,
                [this](bool ok, const QString &msg, qint64 bytes) {
            out() << "AUTOTEST RESULT ok=" << (ok ? 1 : 0)
                  << " bytes=" << bytes << " msg=" << msg << '\n';
            QCoreApplication::exit(ok ? 0 : 3);
        });
        /* 兜底：60 秒总超时 */
        QTimer::singleShot(60000, this, []() {
            out() << "AUTOTEST RESULT ok=0 stage=timeout" << '\n';
            QCoreApplication::exit(4);
        });
        /* 通过自身信号跨线程发给引擎 */
        connect(this, &AutoTest::requestLogin, m_engine, &FtpEngine::login);
        connect(this, &AutoTest::requestDownload, m_engine, &FtpEngine::download);
        connect(this, &AutoTest::requestUpload, m_engine, &FtpEngine::upload);
    }
    void start() { emit requestLogin(m_host, m_port, m_user, m_pass); }

signals:
    void requestLogin(const QString &, quint16, const QString &, const QString &);
    void requestDownload(const QString &, const QString &);
    void requestUpload(const QString &, const QString &);

private:
    static QTextStream &out() { static QTextStream s(stdout); return s; }
    FtpEngine *m_engine;
    QString m_host, m_user, m_pass, m_remote, m_local;
    quint16 m_port;
    bool m_upload;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QThread netThread;
    netThread.start();
    FtpEngine *engine = new FtpEngine;   /* 无父对象，退出时统一销毁 */
    engine->moveToThread(&netThread);

    int rc;
    QStringList args = QCoreApplication::arguments();
    if (args.contains("--auto")) {
        QStringList a = args.mid(args.indexOf("--auto") + 1);  /* a[0]==host */
        if (a.size() < 7) {
            QTextStream(stderr) << "usage: --auto host port user pass dl|ul remote local\n";
            return 64;
        }
        AutoTest at(engine, a);
        QTimer::singleShot(0, &at, &AutoTest::start);
        rc = app.exec();
    } else {
        MainWindow w;
        /* GUI -> 引擎（跨线程自动排队） */
        QObject::connect(&w, &MainWindow::loginRequested, engine, &FtpEngine::login);
        QObject::connect(&w, &MainWindow::logoutRequested, engine, &FtpEngine::logout);
        QObject::connect(&w, &MainWindow::downloadRequested, engine, &FtpEngine::download);
        QObject::connect(&w, &MainWindow::uploadRequested, engine, &FtpEngine::upload);
        /* 引擎 -> GUI */
        QObject::connect(engine, &FtpEngine::logMessage, &w, &MainWindow::appendLog);
        QObject::connect(engine, &FtpEngine::loggedIn, &w, &MainWindow::onLoggedIn);
        QObject::connect(engine, &FtpEngine::loginFailed, &w, &MainWindow::onLoginFailed);
        QObject::connect(engine, &FtpEngine::loggedOut, &w, &MainWindow::onLoggedOut);
        QObject::connect(engine, &FtpEngine::xferStarted, &w, &MainWindow::onXferStarted);
        QObject::connect(engine, &FtpEngine::xferProgress, &w, &MainWindow::onXferProgress);
        QObject::connect(engine, &FtpEngine::xferFinished, &w, &MainWindow::onXferFinished);
        w.show();
        rc = app.exec();
    }

    netThread.quit();
    netThread.wait(3000);
    delete engine;
    return rc;
}

#include "main.moc"
