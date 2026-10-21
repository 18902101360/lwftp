#include "mainwindow.h"

#include "lwftp_qt.h"

#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

/* 搭好输入项，并把客户端信号接到日志和按钮。 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_hostEdit(new QLineEdit(this)),
      m_portSpin(new QSpinBox(this)),
      m_userEdit(new QLineEdit(this)),
      m_passEdit(new QLineEdit(this)),
      m_remoteEdit(new QLineEdit(this)),
      m_localEdit(new QLineEdit(this)),
      m_browseBtn(new QPushButton(QString::fromUtf8("浏览"), this)),
      m_downloadBtn(new QPushButton(QString::fromUtf8("下载"), this)),
      m_closeBtn(new QPushButton(QString::fromUtf8("断开"), this)),
      m_status(new QLabel(QString::fromUtf8("未连接"), this)),
      m_bytes(new QLabel(QString::fromUtf8("已接收 0 字节"), this)),
      m_log(new QPlainTextEdit(this)),
      m_client(new LwftpClient(this))
{
    setWindowTitle(QStringLiteral("lwftp"));
    resize(760, 540);

    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(21);
    m_userEdit->setText(QStringLiteral("anonymous"));
    m_passEdit->setEchoMode(QLineEdit::Password);
    m_passEdit->setText(QStringLiteral("anonymous@"));
    m_hostEdit->setPlaceholderText(QString::fromUtf8("IP 或域名"));
    m_remoteEdit->setPlaceholderText(QStringLiteral("/pub/file.bin"));
    m_log->setReadOnly(true);
    QFont logFont(QStringLiteral("Monospace"));
    logFont.setStyleHint(QFont::TypeWriter);
    m_log->setFont(logFont);

    QWidget *localRow = new QWidget(this);
    QHBoxLayout *localLay = new QHBoxLayout(localRow);
    localLay->setContentsMargins(0, 0, 0, 0);
    localLay->addWidget(m_localEdit);
    localLay->addWidget(m_browseBtn);

    QFormLayout *form = new QFormLayout();
    form->addRow(QString::fromUtf8("服务器"), m_hostEdit);
    form->addRow(QString::fromUtf8("端口"), m_portSpin);
    form->addRow(QString::fromUtf8("用户名"), m_userEdit);
    form->addRow(QString::fromUtf8("密码"), m_passEdit);
    form->addRow(QString::fromUtf8("远程文件"), m_remoteEdit);
    form->addRow(QString::fromUtf8("本地文件"), localRow);

    QHBoxLayout *btns = new QHBoxLayout();
    btns->addWidget(m_downloadBtn);
    btns->addWidget(m_closeBtn);
    btns->addStretch();

    QWidget *central = new QWidget(this);
    QVBoxLayout *root = new QVBoxLayout(central);
    root->addLayout(form);
    root->addLayout(btns);
    root->addWidget(m_status);
    root->addWidget(m_bytes);
    root->addWidget(m_log, 1);
    setCentralWidget(central);

    connect(m_browseBtn, SIGNAL(clicked()), this, SLOT(onBrowse()));
    connect(m_downloadBtn, SIGNAL(clicked()), this, SLOT(onDownload()));
    connect(m_closeBtn, SIGNAL(clicked()), this, SLOT(onCloseSession()));
    connect(m_remoteEdit, SIGNAL(returnPressed()), this, SLOT(onDownload()));

    connect(m_client, SIGNAL(logLine(QString)), this, SLOT(onLog(QString)));
    connect(m_client, SIGNAL(phaseChanged(QString)), this, SLOT(onPhase(QString)));
    connect(m_client, SIGNAL(progress(qint64)), this, SLOT(onProgress(qint64)));
    connect(m_client, SIGNAL(succeeded(QString,qint64)),
            this, SLOT(onSucceeded(QString,qint64)));
    connect(m_client, SIGNAL(failed(int,QString)),
            this, SLOT(onFailed(int,QString)));
    connect(m_client, SIGNAL(sessionClosed()), this, SLOT(onSessionClosed()));
}

/* 见头文件。取消对话框时不改已有路径。 */
void MainWindow::onBrowse()
{
    const QString path = QFileDialog::getSaveFileName(
                this,
                QString::fromUtf8("保存到"),
                m_localEdit->text());
    if (!path.isEmpty()) {
        m_localEdit->setText(path);
    }
}

/* 见头文件。端口框的范围已经限制在 1 到 65535。 */
void MainWindow::onDownload()
{
    m_downloadBtn->setEnabled(false);
    m_bytes->setText(QString::fromUtf8("已接收 0 字节"));
    m_client->download(m_hostEdit->text(),
                       (quint16)m_portSpin->value(),
                       m_userEdit->text(),
                       m_passEdit->text(),
                       m_remoteEdit->text(),
                       m_localEdit->text());
    /* 参数不完整时客户端会立刻 failed，按钮在那里恢复。若仍在忙则保持禁用。 */
    if (!m_client->isBusy()) {
        m_downloadBtn->setEnabled(true);
    }
}

/* 见头文件。 */
void MainWindow::onCloseSession()
{
    m_client->closeSession();
}

/* 见头文件。 */
void MainWindow::onLog(const QString &line)
{
    m_log->appendPlainText(line);
}

/* 见头文件。 */
void MainWindow::onPhase(const QString &text)
{
    m_status->setText(text);
}

/* 见头文件。 */
void MainWindow::onProgress(qint64 bytes)
{
    m_bytes->setText(QString::fromUtf8("已接收 %1 字节").arg(QString::number(bytes)));
}

/* 见头文件。 */
void MainWindow::onSucceeded(const QString &localPath, qint64 bytes)
{
    m_downloadBtn->setEnabled(true);
    m_status->setText(QString::fromUtf8("已保存 %1（%2 字节）")
                      .arg(localPath, QString::number(bytes)));
}

/* 见头文件。result 目前只用于和 lwftp 结果码对齐，界面直接显示 reason。 */
void MainWindow::onFailed(int result, const QString &reason)
{
    Q_UNUSED(result);
    m_downloadBtn->setEnabled(true);
    m_status->setText(reason);
}

/* 见头文件。失败路径会自己恢复按钮，这里避免把“正在重连”误显示成空闲。 */
void MainWindow::onSessionClosed()
{
    if (m_client->isBusy()) {
        return;
    }
    m_downloadBtn->setEnabled(true);
    m_status->setText(QString::fromUtf8("未连接"));
}
