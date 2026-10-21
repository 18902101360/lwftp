#include "mainwindow.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_connected(false), m_transferring(false)
{
    setWindowTitle(QStringLiteral("lwftp 客户端（原版库 + Qt 界面）"));

    m_hostEdit = new QLineEdit(QStringLiteral("127.0.0.1"));
    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(21);
    m_userEdit = new QLineEdit(QStringLiteral("admin"));
    m_passEdit = new QLineEdit(QStringLiteral("qt"));
    m_passEdit->setEchoMode(QLineEdit::Password);
    m_connectBtn = new QPushButton(QStringLiteral("连接"));

    m_remoteEdit = new QLineEdit;
    m_remoteEdit->setPlaceholderText(QStringLiteral("/服务器上的文件路径"));
    m_localEdit = new QLineEdit;
    m_localEdit->setPlaceholderText(QStringLiteral("本地文件路径"));
    m_browseBtn = new QPushButton(QStringLiteral("浏览..."));

    m_downloadBtn = new QPushButton(QStringLiteral("下载(远程->本地)"));
    m_uploadBtn = new QPushButton(QStringLiteral("上传(本地->远程)"));

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_bytesLabel = new QLabel(QStringLiteral("0 字节"));

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);

    /* 布局 */
    QFormLayout *form = new QFormLayout;
    form->addRow(QStringLiteral("主机"), m_hostEdit);
    form->addRow(QStringLiteral("端口"), m_portSpin);
    form->addRow(QStringLiteral("用户名"), m_userEdit);
    form->addRow(QStringLiteral("密码"), m_passEdit);

    QHBoxLayout *localRow = new QHBoxLayout;
    localRow->addWidget(m_localEdit, 1);
    localRow->addWidget(m_browseBtn);

    QFormLayout *fileForm = new QFormLayout;
    fileForm->addRow(QStringLiteral("远程文件"), m_remoteEdit);
    fileForm->addRow(QStringLiteral("本地文件"), localRow);

    QHBoxLayout *xferRow = new QHBoxLayout;
    xferRow->addWidget(m_downloadBtn);
    xferRow->addWidget(m_uploadBtn);

    QHBoxLayout *progRow = new QHBoxLayout;
    progRow->addWidget(m_progress, 1);
    progRow->addWidget(m_bytesLabel);

    QVBoxLayout *root = new QVBoxLayout;
    root->addLayout(form);
    root->addWidget(m_connectBtn);
    root->addLayout(fileForm);
    root->addLayout(xferRow);
    root->addLayout(progRow);
    root->addWidget(m_log, 1);

    QWidget *central = new QWidget;
    central->setLayout(root);
    setCentralWidget(central);
    resize(560, 480);

    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_browseBtn, &QPushButton::clicked, this, &MainWindow::onBrowseClicked);
    connect(m_downloadBtn, &QPushButton::clicked, this, &MainWindow::onDownloadClicked);
    connect(m_uploadBtn, &QPushButton::clicked, this, &MainWindow::onUploadClicked);

    onLoggedOut();   /* 初始按钮状态 */
}

void MainWindow::onConnectClicked()
{
    if (m_connected) {
        emit logoutRequested();
        return;
    }
    m_connectBtn->setEnabled(false);
    appendLog(QStringLiteral("连接中..."));
    emit loginRequested(m_hostEdit->text().trimmed(),
                        (quint16)m_portSpin->value(),
                        m_userEdit->text(), m_passEdit->text());
}

void MainWindow::onBrowseClicked()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("选择本地文件"));
    if (!path.isEmpty()) m_localEdit->setText(path);
}

void MainWindow::onDownloadClicked()
{
    setBusy(true);
    emit downloadRequested(m_remoteEdit->text().trimmed(), m_localEdit->text().trimmed());
}

void MainWindow::onUploadClicked()
{
    setBusy(true);
    emit uploadRequested(m_remoteEdit->text().trimmed(), m_localEdit->text().trimmed());
}

void MainWindow::setBusy(bool busy)
{
    m_transferring = busy;
    m_downloadBtn->setEnabled(m_connected && !busy);
    m_uploadBtn->setEnabled(m_connected && !busy);
    m_progress->setRange(0, busy ? 0 : 100);   /* 传输中显示忙碌条 */
    if (busy) m_bytesLabel->setText(QStringLiteral("0 字节"));
}

/* ---------------- 引擎信号 ---------------- */

void MainWindow::appendLog(const QString &text)
{
    m_log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ")
                           + text);
}

void MainWindow::onLoggedIn()
{
    m_connected = true;
    m_connectBtn->setEnabled(true);
    m_connectBtn->setText(QStringLiteral("断开"));
    m_downloadBtn->setEnabled(true);
    m_uploadBtn->setEnabled(true);
    appendLog(QStringLiteral("已登录，可以开始传输"));
}

void MainWindow::onLoginFailed(const QString &reason)
{
    m_connectBtn->setEnabled(true);
    appendLog(QStringLiteral("登录失败：") + reason);
}

void MainWindow::onLoggedOut()
{
    m_connected = false;
    m_transferring = false;
    m_connectBtn->setEnabled(true);
    m_connectBtn->setText(QStringLiteral("连接"));
    m_downloadBtn->setEnabled(false);
    m_uploadBtn->setEnabled(false);
    m_progress->setRange(0, 100);
}

void MainWindow::onXferStarted(const QString &kind)
{
    appendLog(kind + QStringLiteral("开始..."));
}

void MainWindow::onXferProgress(qint64 bytes)
{
    m_bytesLabel->setText(QStringLiteral("%1 字节").arg(bytes));
}

void MainWindow::onXferFinished(bool ok, const QString &message, qint64 bytes)
{
    setBusy(false);
    appendLog((ok ? QStringLiteral("传输完成：") : QStringLiteral("传输失败："))
              + message + QStringLiteral("，共 %1 字节").arg(bytes));
}
