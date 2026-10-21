/*
 * lwftp 状态机的 Qt 实现。
 *
 * 一次下载的控制连接顺序（RFC 959，方向都是客户端 -> 服务器，除非注明）：
 *   TCP 连接
 *   服务器 220 服务就绪
 *   USER <用户名>
 *   服务器 331 需要口令，或直接 230
 *   PASS <口令>
 *   服务器 230 登录成功
 *   TYPE I          二进制，服务器不改换行
 *   服务器 200
 *   PASV            请服务器打开被动数据端口
 *   服务器 227 (h1,h2,h3,h4,p1,p2)
 *   客户端连接 h1.h2.h3.h4 的端口 p1*256+p2
 *   RETR <远程路径>
 *   服务器 150 或 125，随后从数据连接发文件
 *   服务器关掉数据连接，再在控制连接上发 226
 * 空闲退出时发 QUIT，等 221。
 */

#include "lwftp_qt.h"

#include <QHostAddress>
#include <QNetworkProxy>

namespace {

/* 判断一行是不是以三位应答码开头。FTP 应答码固定三位十进制数。 */
bool startsWithReplyCode(const QByteArray &raw)
{
    if (raw.size() < 3) {
        return false;
    }
    return raw.at(0) >= '0' && raw.at(0) <= '9'
        && raw.at(1) >= '0' && raw.at(1) <= '9'
        && raw.at(2) >= '0' && raw.at(2) <= '9';
}

} // namespace

/* 建立两个直连套接字。FTP 数据端口是 PASV 临时开的，不能走应用层代理。 */
LwftpClient::LwftpClient(QObject *parent)
    : QObject(parent),
      m_control(new QTcpSocket(this)),
      m_data(new QTcpSocket(this)),
      m_port(21),
      m_dataPort(0),
      m_multiCode(0),
      m_inMulti(false),
      m_state(LWFTP_CLOSED),
      m_target(LWFTP_RETR_SENT),
      m_busy(false),
      m_failedGuard(false),
      m_relogin(false),
      m_got226(false),
      m_dataReady(false),
      m_dataDone(false),
      m_usedAlt(false),
      m_ignoreCtrlError(false),
      m_ignoreDataError(false),
      m_pumping(false),
      m_bytes(0),
      m_timeoutMs(30000)
{
    m_control->setProxy(QNetworkProxy::NoProxy);
    m_data->setProxy(QNetworkProxy::NoProxy);
    /* 控制命令很短，关掉 Nagle，避免命令在本地缓冲里多等一个延迟。 */
    m_control->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    m_timer.setSingleShot(true);

    connect(m_control, SIGNAL(connected()),
            this, SLOT(onControlConnected()));
    connect(m_control, SIGNAL(readyRead()),
            this, SLOT(onControlReadyRead()));
    connect(m_control, SIGNAL(disconnected()),
            this, SLOT(onControlDisconnected()));
    /* Qt 5.14 的错误信号是 error(QAbstractSocket::SocketError)，不是 Qt 6 的 errorOccurred。 */
    connect(m_control, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(onControlError(QAbstractSocket::SocketError)));

    connect(m_data, SIGNAL(connected()),
            this, SLOT(onDataConnected()));
    connect(m_data, SIGNAL(readyRead()),
            this, SLOT(onDataReadyRead()));
    connect(m_data, SIGNAL(disconnected()),
            this, SLOT(onDataDisconnected()));
    connect(m_data, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(onDataError(QAbstractSocket::SocketError)));

    connect(&m_timer, SIGNAL(timeout()),
            this, SLOT(onTimeout()));
}

/* 套接字是子对象，这里只收起还开着的文件。 */
LwftpClient::~LwftpClient()
{
    m_timer.stop();
    if (m_file.isOpen()) {
        m_file.close();
    }
}

/* 见头文件。只去掉换行，口令里的空格保留。 */
QString LwftpClient::stripCrlf(const QString &text)
{
    QString out = text;
    out.replace(QLatin1String("\r"), QString());
    out.replace(QLatin1String("\n"), QString());
    return out;
}

/* 见头文件。只认括号里的六个十进制数。 */
bool LwftpClient::parsePasv(const QString &text, QString *host, quint16 *port)
{
    int left = text.lastIndexOf(QLatin1Char('('));
    int right = text.lastIndexOf(QLatin1Char(')'));
    if (left < 0 || right <= left || !host || !port) {
        return false;
    }

    const QStringList parts = text.mid(left + 1, right - left - 1).split(QLatin1Char(','));
    if (parts.size() < 6) {
        return false;
    }

    QStringList ipParts;
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        int n = parts.at(i).trimmed().toInt(&ok);
        if (!ok || n < 0 || n > 255) {
            return false;
        }
        ipParts << QString::number(n);
    }

    bool ok1 = false;
    bool ok2 = false;
    int p1 = parts.at(4).trimmed().toInt(&ok1);
    int p2 = parts.at(5).trimmed().toInt(&ok2);
    if (!ok1 || !ok2 || p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) {
        return false;
    }

    *host = ipParts.join(QLatin1Char('.'));
    *port = (quint16)(p1 * 256 + p2);
    /* 端口 0 不能连。 */
    return *port != 0;
}

/* 正在登录或传输。QUIT 等待期间不算忙，除非还带着一次下载。 */
bool LwftpClient::isBusy() const
{
    return m_busy;
}

/* 当前控制连接状态。 */
LwftpState LwftpClient::state() const
{
    return m_state;
}

/* 见头文件。已登录且服务器没变时直接 RETR，否则重新登录。 */
void LwftpClient::download(const QString &host, quint16 port,
                           const QString &user, const QString &pass,
                           const QString &remotePath, const QString &localPath)
{
    if (m_busy) {
        emit failed(LWFTP_RESULT_ERR_ARGUMENT, QString::fromUtf8("正在下载，请等这次结束"));
        return;
    }

    const QString host2 = stripCrlf(host).trimmed();
    const quint16 port2 = port;
    const QString user2 = stripCrlf(user);
    const QString pass2 = stripCrlf(pass);
    const QString remote2 = stripCrlf(remotePath).trimmed();
    const QString local2 = stripCrlf(localPath).trimmed();

    if (host2.isEmpty() || user2.isEmpty() || remote2.isEmpty()
            || local2.isEmpty() || port2 == 0) {
        emit failed(LWFTP_RESULT_ERR_ARGUMENT, QString::fromUtf8("请把服务器、账号、远程路径和本地文件填完整"));
        return;
    }

    /*
     * 仍是同一条已登录的控制连接时直接 RETR。
     * 比较放在改写成员之前，否则换服务器也会被当成同一次会话。
     */
    const bool sameSession = (m_state == LWFTP_LOGGED
                              && m_control->state() == QAbstractSocket::ConnectedState
                              && m_host == host2
                              && m_port == port2
                              && m_user == user2
                              && m_pass == pass2);

    if (m_file.isOpen()) {
        m_file.close();
    }
    /* 先写入 .part，成功后再换成用户选的文件名，避免半截文件占住目标路径。 */
    m_partPath = local2 + QLatin1String(".part");
    m_file.setFileName(m_partPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_partPath.clear();
        emit failed(LWFTP_RESULT_ERR_LOCAL,
                    QString::fromUtf8("打不开本地文件：") + m_file.errorString());
        return;
    }

    m_host = host2;
    m_port = port2;
    m_user = user2;
    m_pass = pass2;
    m_remotePath = remote2;
    m_localPath = local2;
    m_busy = true;
    m_got226 = false;
    m_dataReady = false;
    m_dataDone = false;
    m_usedAlt = false;
    m_altDataHost.clear();
    m_bytes = 0;
    emit progress(0);

    if (sameSession) {
        startRetr();
        return;
    }

    /* 还连着别的会话时先 QUIT，断开后再用本次的账号连接。 */
    if (m_state != LWFTP_CLOSED && m_control->state() != QAbstractSocket::UnconnectedState) {
        m_relogin = true;
        if (!sendLine(QStringLiteral("QUIT"))) {
            return;
        }
        m_state = LWFTP_QUIT_SENT;
        return;
    }

    startConnect();
}

/* 见头文件。 */
void LwftpClient::closeSession()
{
    if (m_busy) {
        fail(LWFTP_RESULT_ERR_CLOSED, QString::fromUtf8("已断开"));
        return;
    }
    if (m_state == LWFTP_CLOSED || m_control->state() == QAbstractSocket::UnconnectedState) {
        m_state = LWFTP_CLOSED;
        return;
    }
    if (!sendLine(QStringLiteral("QUIT"))) {
        return;
    }
    m_state = LWFTP_QUIT_SENT;
}

/* 见头文件。日志里不写出口令。 */
bool LwftpClient::sendLine(const QString &cmd)
{
    QByteArray bin = cmd.toUtf8();
    /* RFC 959：命令以 CRLF 结束。 */
    bin += "\r\n";

    QString show = cmd;
    if (show.startsWith(QLatin1String("PASS "))) {
        show = QStringLiteral("PASS ***");
    }
    emit logLine(QStringLiteral("C: ") + show);

    if (m_control->write(bin) != bin.size()) {
        fail(LWFTP_RESULT_ERR_INTERNAL, QString::fromUtf8("控制连接发送失败"));
        return false;
    }
    m_control->flush();
    armTimeout();
    return true;
}

/* 见头文件。 */
void LwftpClient::startConnect()
{
    m_ctrlBuf.clear();
    m_multiText.clear();
    m_inMulti = false;
    m_multiCode = 0;
    m_state = LWFTP_CLOSED;

    if (m_control->state() != QAbstractSocket::UnconnectedState) {
        m_ignoreCtrlError = true;
        m_control->abort();
        m_ignoreCtrlError = false;
    }

    emit phaseChanged(QString::fromUtf8("正在连接 ") + m_host);
    emit logLine(QString::fromUtf8("连接 %1:%2").arg(m_host).arg(m_port));
    m_control->connectToHost(m_host, m_port);
    armTimeout();
}

/* 见头文件。TYPE I 表示二进制图像模式。 */
void LwftpClient::startRetr()
{
    m_got226 = false;
    m_dataReady = false;
    m_dataDone = false;
    m_usedAlt = false;
    m_altDataHost.clear();
    emit phaseChanged(QString::fromUtf8("正在下载"));
    if (!sendLine(QStringLiteral("TYPE I"))) {
        return;
    }
    m_state = LWFTP_TYPE_SENT;
    m_target = LWFTP_RETR_SENT;
}

/* 见头文件。NAT 后面的服务器经常在 227 里给一个客户端到不了的内网地址。 */
void LwftpClient::openData(const QString &pasvHost, quint16 pasvPort)
{
    m_dataPort = pasvPort;
    m_dataReady = false;
    m_dataDone = false;
    m_usedAlt = false;
    m_altDataHost.clear();

    QString target = pasvHost;
    QHostAddress peer = m_control->peerAddress();
    quint32 peer4 = peer.toIPv4Address();
    quint32 pasv4 = QHostAddress(pasvHost).toIPv4Address();
    const bool sameAddr = (pasv4 != 0 && peer4 != 0 && pasv4 == peer4) || pasvHost == m_host;
    if (!sameAddr) {
        m_altDataHost = pasvHost;
        target = m_host;
        emit logLine(QString::fromUtf8("PASV 地址 %1 和控制连接不是同一个地址，数据连接先用 %2，端口 %3")
                     .arg(pasvHost, m_host)
                     .arg(pasvPort));
    }

    m_dataHost = target;
    resetDataSocket();
    emit logLine(QString::fromUtf8("连接数据端口 %1:%2").arg(target).arg(pasvPort));
    m_data->connectToHost(target, pasvPort);
    armTimeout();
}

/* 丢掉旧的数据连接，准备重新 connectToHost。 */
void LwftpClient::resetDataSocket()
{
    if (m_data->state() == QAbstractSocket::UnconnectedState) {
        return;
    }
    m_ignoreDataError = true;
    m_data->abort();
    m_ignoreDataError = false;
}

/* 见头文件。 */
void LwftpClient::pumpLines()
{
    if (m_pumping) {
        return;
    }
    m_pumping = true;
    while (!m_failedGuard && m_state != LWFTP_CLOSED) {
        int nl = m_ctrlBuf.indexOf('\n');
        if (nl < 0) {
            break;
        }
        QByteArray line = m_ctrlBuf.left(nl);
        m_ctrlBuf.remove(0, nl + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        handleControlLine(line);
    }
    m_pumping = false;
}

/* 见头文件。分隔符 '-' 表示多行未完，空格表示这条应答结束。 */
void LwftpClient::handleControlLine(const QByteArray &raw)
{
    emit logLine(QStringLiteral("S: ") + QString::fromUtf8(raw));

    if (!startsWithReplyCode(raw)) {
        /* 多行正文的中间行可以不以应答码开头。 */
        if (m_inMulti) {
            m_multiText += QString::fromUtf8(raw);
            m_multiText += QLatin1Char('\n');
        }
        return;
    }

    int code = (raw.at(0) - '0') * 100 + (raw.at(1) - '0') * 10 + (raw.at(2) - '0');
    QChar sep = (raw.size() >= 4) ? QChar(raw.at(3)) : QChar(QLatin1Char(' '));
    QString text = (raw.size() > 4) ? QString::fromUtf8(raw.mid(4)) : QString();

    if (sep == QLatin1Char('-')) {
        m_inMulti = true;
        m_multiCode = code;
        m_multiText += text;
        m_multiText += QLatin1Char('\n');
        return;
    }

    if (m_inMulti && code != m_multiCode) {
        m_multiText += QString::fromUtf8(raw);
        m_multiText += QLatin1Char('\n');
        return;
    }

    m_multiText += text;
    QString full = m_multiText;
    m_multiText.clear();
    m_inMulti = false;
    m_multiCode = 0;
    processReply(code, full);
}

/* 登录成功后如果这次调用还要下载，就接着发 TYPE。 */
void LwftpClient::processReply(int code, const QString &text)
{
    switch (m_state) {
    case LWFTP_CONNECTED:
        /* 220：服务就绪，可以发 USER。1xx 只是中间提示，继续等 220。 */
        if (code == 220) {
            if (!sendLine(QStringLiteral("USER ") + m_user)) {
                return;
            }
            m_state = LWFTP_USER_SENT;
            emit phaseChanged(QString::fromUtf8("正在登录"));
        } else if (code >= 100 && code < 200) {
            break;
        } else {
            fail(LWFTP_RESULT_ERR_SRVR_RESP,
                 QString::fromUtf8("服务器拒绝连接（%1）").arg(code));
        }
        break;

    case LWFTP_USER_SENT:
        /* 331：需要 PASS。230：服务器不要求口令，已经登录。 */
        if (code == 331) {
            if (!sendLine(QStringLiteral("PASS ") + m_pass)) {
                return;
            }
            m_state = LWFTP_PASS_SENT;
        } else if (code == 230 || code == 202) {
            m_state = LWFTP_LOGGED;
            emit phaseChanged(QString::fromUtf8("已登录"));
            if (m_busy) {
                startRetr();
            }
        } else {
            fail(LWFTP_RESULT_ERR_SRVR_RESP,
                 QString::fromUtf8("登录被拒绝（%1 %2）").arg(code).arg(text));
        }
        break;

    case LWFTP_PASS_SENT:
        /* 230：登录成功。530 一类的码都算登录失败。 */
        if (code == 230 || code == 202) {
            m_state = LWFTP_LOGGED;
            emit phaseChanged(QString::fromUtf8("已登录"));
            if (m_busy) {
                startRetr();
            }
        } else {
            fail(LWFTP_RESULT_ERR_SRVR_RESP,
                 QString::fromUtf8("登录被拒绝（%1 %2）").arg(code).arg(text));
        }
        break;

    case LWFTP_TYPE_SENT:
        /* 200：TYPE 被接受。随后要被动模式，这样数据连接由我们连出去。 */
        if (code == 200) {
            if (!sendLine(QStringLiteral("PASV"))) {
                return;
            }
            m_state = LWFTP_PASV_SENT;
        } else {
            fail(LWFTP_RESULT_ERR_SRVR_RESP,
                 QString::fromUtf8("服务器不接受二进制模式（%1）").arg(code));
        }
        break;

    case LWFTP_PASV_SENT:
        /* 227：正文里带数据连接地址。解析失败就停在已登录，不发 RETR。 */
        if (code == 227) {
            QString pasvHost;
            quint16 pasvPort = 0;
            if (!parsePasv(text, &pasvHost, &pasvPort)) {
                m_busy = false;
                m_state = LWFTP_LOGGED;
                m_timer.stop();
                if (m_file.isOpen()) {
                    m_file.close();
                }
                if (!m_partPath.isEmpty()) {
                    QFile::remove(m_partPath);
                    m_partPath.clear();
                }
                emit failed(LWFTP_RESULT_ERR_SRVR_RESP,
                            QString::fromUtf8("PASV 应答里没有可用地址"));
                break;
            }
            openData(pasvHost, pasvPort);
            if (m_target != LWFTP_RETR_SENT) {
                fail(LWFTP_RESULT_ERR_INTERNAL, QString::fromUtf8("内部状态错误"));
                break;
            }
            /* RETR 的参数是远程路径，UTF-8 原样发送。 */
            if (!sendLine(QStringLiteral("RETR ") + m_remotePath)) {
                return;
            }
            m_state = LWFTP_RETR_SENT;
        } else {
            fail(LWFTP_RESULT_ERR_SRVR_RESP,
                 QString::fromUtf8("服务器不支持被动模式（%1）").arg(code));
        }
        break;

    case LWFTP_RETR_SENT:
        /*
         * 150：即将打开数据连接并发送文件。
         * 125：数据连接已经打开。
         * 550：路径不存在或没有权限。
         * 226：有的服务器把 150 和 226 紧挨着发来，这里也认。
         */
        if (code == 150 || code == 125) {
            m_state = LWFTP_XFERING;
            tryComplete();
        } else if (code == 226) {
            m_state = LWFTP_XFERING;
            m_got226 = true;
            tryComplete();
        } else if (code == 550) {
            m_busy = false;
            m_state = LWFTP_LOGGED;
            m_timer.stop();
            m_ignoreDataError = true;
            m_data->abort();
            m_ignoreDataError = false;
            if (m_file.isOpen()) {
                m_file.close();
            }
            if (!m_partPath.isEmpty()) {
                QFile::remove(m_partPath);
                m_partPath.clear();
            }
            emit failed(LWFTP_RESULT_ERR_FILENAME,
                        QString::fromUtf8("服务器没有这个文件（550 %1）").arg(text));
        } else {
            fail(LWFTP_RESULT_ERR_SRVR_RESP,
                 QString::fromUtf8("无法开始下载（%1 %2）").arg(code).arg(text));
        }
        break;

    case LWFTP_XFERING:
        /* 226：传输完成。数据连接可能还没把断开事件送上来，等齐再改名。 */
        if (code == 226) {
            m_got226 = true;
            tryComplete();
        } else if (code >= 100 && code < 200) {
            break;
        } else {
            fail(LWFTP_RESULT_ERR_CLOSED,
                 QString::fromUtf8("传输被服务器中断（%1）").arg(code));
        }
        break;

    case LWFTP_QUIT_SENT:
        /* 221：服务关闭控制连接。换服务器时接着去连新的。 */
        m_state = LWFTP_CLOSING;
        if (m_control->state() != QAbstractSocket::UnconnectedState) {
            m_control->disconnectFromHost();
        } else {
            onControlDisconnected();
        }
        break;

    default:
        emit logLine(QString::fromUtf8("未处理的状态 %1，应答 %2").arg((int)m_state).arg(code));
        break;
    }
}

/* 见头文件。两个条件都成立才改名，避免把还在传输的文件当成完成。 */
void LwftpClient::tryComplete()
{
    if (!m_busy || !m_got226 || !m_dataDone) {
        return;
    }

    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }

    if (QFile::exists(m_localPath)) {
        QFile::remove(m_localPath);
    }
    if (!QFile::rename(m_partPath, m_localPath)) {
        m_busy = false;
        m_state = LWFTP_LOGGED;
        m_timer.stop();
        emit failed(LWFTP_RESULT_ERR_LOCAL,
                    QString::fromUtf8("无法保存到 %1，数据留在 %2").arg(m_localPath, m_partPath));
        return;
    }

    m_partPath.clear();
    m_busy = false;
    m_state = LWFTP_LOGGED;
    m_timer.stop();
    emit phaseChanged(QString::fromUtf8("下载完成"));
    emit succeeded(m_localPath, m_bytes);
}

/* 见头文件。先置保护再 abort，避免错误信号里再进一次 fail。 */
void LwftpClient::fail(int result, const QString &reason)
{
    if (m_failedGuard) {
        return;
    }
    m_failedGuard = true;
    m_busy = false;
    m_relogin = false;
    m_timer.stop();

    if (m_file.isOpen()) {
        m_file.close();
    }
    if (!m_partPath.isEmpty()) {
        QFile::remove(m_partPath);
        m_partPath.clear();
    }

    m_ignoreDataError = true;
    m_ignoreCtrlError = true;
    m_data->abort();
    m_control->abort();
    m_ignoreDataError = false;
    m_ignoreCtrlError = false;

    m_state = LWFTP_CLOSED;
    m_failedGuard = false;

    emit logLine(reason);
    emit phaseChanged(reason);
    emit failed(result, reason);
}

/* 见头文件。 */
void LwftpClient::armTimeout()
{
    m_timer.start(m_timeoutMs);
}

/* TCP 已通。220 如果已经进了缓冲，这里补拆一次。 */
void LwftpClient::onControlConnected()
{
    if (m_failedGuard) {
        return;
    }
    m_state = LWFTP_CONNECTED;
    emit logLine(QString::fromUtf8("控制连接已建立"));
    armTimeout();
    pumpLines();
}

/* 见头文件。 */
void LwftpClient::onControlReadyRead()
{
    m_ctrlBuf.append(m_control->readAll());
    if (m_state == LWFTP_CLOSED) {
        /* 还没收到 connected 时先攒着，等 onControlConnected 再解析。 */
        return;
    }
    armTimeout();
    pumpLines();
}

/* 见头文件。 */
void LwftpClient::onControlDisconnected()
{
    if (m_failedGuard || m_ignoreCtrlError) {
        m_state = LWFTP_CLOSED;
        return;
    }

    /* 正常 QUIT，或者对端在 221 之后先断开。 */
    if (m_state == LWFTP_QUIT_SENT || m_state == LWFTP_CLOSING || m_state == LWFTP_CLOSED) {
        m_state = LWFTP_CLOSED;
        m_timer.stop();
        if (m_relogin) {
            m_relogin = false;
            startConnect();
            return;
        }
        emit logLine(QString::fromUtf8("控制连接已关闭"));
        emit phaseChanged(QString::fromUtf8("未连接"));
        emit sessionClosed();
        return;
    }

    fail(LWFTP_RESULT_ERR_CLOSED, QString::fromUtf8("服务器关闭了控制连接"));
}

/* 见头文件。RemoteHostClosedError 交给 disconnected。 */
void LwftpClient::onControlError(QAbstractSocket::SocketError err)
{
    if (m_failedGuard || m_ignoreCtrlError) {
        return;
    }
    if (err == QAbstractSocket::RemoteHostClosedError) {
        return;
    }
    if (err == QAbstractSocket::HostNotFoundError) {
        fail(LWFTP_RESULT_ERR_HOSTNAME, QString::fromUtf8("无法解析主机 ") + m_host);
        return;
    }
    if (m_state == LWFTP_CLOSED || m_state == LWFTP_CONNECTED) {
        fail(LWFTP_RESULT_ERR_CONNECT,
             QString::fromUtf8("连接服务器失败：") + m_control->errorString());
        return;
    }
    fail(LWFTP_RESULT_ERR_CLOSED,
         QString::fromUtf8("控制连接错误：") + m_control->errorString());
}

/* 见头文件。 */
void LwftpClient::onDataConnected()
{
    if (m_failedGuard || m_ignoreDataError) {
        return;
    }
    m_dataReady = true;
    emit logLine(QString::fromUtf8("数据连接已建立"));
    armTimeout();
}

/* 按二进制原样写入，不做换行转换。TYPE I 约定服务器也不转换。 */
void LwftpClient::onDataReadyRead()
{
    if (m_failedGuard || m_ignoreDataError || !m_file.isOpen()) {
        m_data->readAll();
        return;
    }

    const QByteArray chunk = m_data->readAll();
    if (chunk.isEmpty()) {
        return;
    }
    const qint64 n = m_file.write(chunk);
    if (n != chunk.size()) {
        fail(LWFTP_RESULT_ERR_LOCAL, QString::fromUtf8("写入本地文件失败"));
        return;
    }
    m_bytes += n;
    emit progress(m_bytes);
    armTimeout();
}

/* 见头文件。断开前把套接字里剩下的字节读完。 */
void LwftpClient::onDataDisconnected()
{
    if (m_failedGuard || m_ignoreDataError) {
        return;
    }
    if (m_file.isOpen() && m_data->bytesAvailable() > 0) {
        onDataReadyRead();
    }
    m_dataDone = true;
    emit logLine(QString::fromUtf8("数据连接已关闭"));
    tryComplete();
}

/* 见头文件。首选地址失败时改连 PASV 原文里的地址，只改一次。 */
void LwftpClient::onDataError(QAbstractSocket::SocketError err)
{
    if (m_failedGuard || m_ignoreDataError) {
        return;
    }
    if (err == QAbstractSocket::RemoteHostClosedError) {
        return;
    }

    if (!m_dataReady && !m_usedAlt && !m_altDataHost.isEmpty()
            && (m_state == LWFTP_RETR_SENT || m_state == LWFTP_PASV_SENT || m_state == LWFTP_XFERING)) {
        m_usedAlt = true;
        const QString alt = m_altDataHost;
        m_dataHost = alt;
        emit logLine(QString::fromUtf8("数据连接改试 PASV 地址 %1:%2").arg(alt).arg(m_dataPort));
        m_ignoreDataError = true;
        m_data->abort();
        m_ignoreDataError = false;
        m_data->connectToHost(alt, m_dataPort);
        armTimeout();
        return;
    }

    if (m_busy) {
        fail(LWFTP_RESULT_ERR_CONNECT,
             QString::fromUtf8("数据连接失败：") + m_data->errorString());
    }
}

/* 见头文件。空闲退出时服务器不回 221，就直接关掉。 */
void LwftpClient::onTimeout()
{
    if (!m_busy && (m_state == LWFTP_QUIT_SENT || m_state == LWFTP_CLOSING)) {
        m_ignoreCtrlError = true;
        m_control->abort();
        m_ignoreCtrlError = false;
        m_state = LWFTP_CLOSED;
        emit phaseChanged(QString::fromUtf8("未连接"));
        emit sessionClosed();
        return;
    }
    fail(LWFTP_RESULT_ERR_TIMEOUT, QString::fromUtf8("超时，服务器没有及时应答"));
}
