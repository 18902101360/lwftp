/*
 * lwftp 的 Qt 移植。
 *
 * 原版 lwftp.c 用 lwIP 的 raw TCP。这里保持同一套 FTP 状态机，
 * 控制连接和数据连接都改成 QTcpSocket。界面通过 download() 拉一个文件。
 *
 * 命令和应答按 RFC 959：命令以 <CRLF> 结束，被动模式用 PASV 的
 * (h1,h2,h3,h4,p1,p2)，端口 = p1 * 256 + p2。传输用 TYPE I（二进制）。
 *
 * Copyright (c) 2014 GEZEDO
 * Qt 移植沿用原 lwftp 的 BSD 许可，见仓库根目录 lwftp.c。
 */

#ifndef LWFTP_QT_H
#define LWFTP_QT_H

#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

/* 结果码与 lwftp.h 的 lwftp_results 对齐，方便和原版对照。 */
enum LwftpResult {
    LWFTP_RESULT_OK = 0,
    LWFTP_RESULT_INPROGRESS,
    LWFTP_RESULT_LOGGED,
    LWFTP_RESULT_ERR_UNKNOWN,   /* 未归类的错误 */
    LWFTP_RESULT_ERR_ARGUMENT,  /* 参数空缺或正在忙 */
    LWFTP_RESULT_ERR_MEMORY,
    LWFTP_RESULT_ERR_CONNECT,   /* 控制连接或数据连接失败 */
    LWFTP_RESULT_ERR_HOSTNAME,  /* 主机名解析失败 */
    LWFTP_RESULT_ERR_CLOSED,    /* 对端提前关掉连接 */
    LWFTP_RESULT_ERR_TIMEOUT,   /* 超时，单位毫秒，见 m_timeoutMs */
    LWFTP_RESULT_ERR_SRVR_RESP, /* 服务器应答码不符合当前状态 */
    LWFTP_RESULT_ERR_INTERNAL,
    LWFTP_RESULT_ERR_LOCAL,     /* 本地文件打不开或写失败 */
    LWFTP_RESULT_ERR_FILENAME   /* 服务器没有这个远程路径，应答 550 */
};

/* 控制连接状态，顺序与 lwftp.h 的 lwftp_state_t 一致。 */
enum LwftpState {
    LWFTP_CLOSED = 0,   /* 没有控制连接 */
    LWFTP_CONNECTED,    /* TCP 已连上，等 220 */
    LWFTP_USER_SENT,    /* 已发 USER，等 331 或 230 */
    LWFTP_PASS_SENT,    /* 已发 PASS，等 230 */
    LWFTP_LOGGED,       /* 已登录，可以发传输命令 */
    LWFTP_TYPE_SENT,    /* 已发 TYPE I，等 200 */
    LWFTP_PASV_SENT,    /* 已发 PASV，等 227 */
    LWFTP_RETR_SENT,    /* 已发 RETR，等 150/125 */
    LWFTP_STOR_SENT,    /* 原版有 STOR。本界面只做下载，不进入这个状态 */
    LWFTP_XFERING,      /* 数据连接上正在收文件 */
    LWFTP_DATAEND,      /* 传输收尾 */
    LWFTP_QUIT,         /* 准备发 QUIT */
    LWFTP_QUIT_SENT,    /* 已发 QUIT，等 221 */
    LWFTP_CLOSING       /* 正在关掉控制连接 */
};

/*
 * LwftpClient：一条 FTP 会话。
 * 给主窗口用。所有套接字回调都在创建它所在的线程（界面线程）上。
 */
class LwftpClient : public QObject
{
    Q_OBJECT

public:
    explicit LwftpClient(QObject *parent = 0);
    ~LwftpClient();

    /*
     * 连接、登录并下载一个文件。
     * host 可以是 IP 或域名，port 一般是 21。
     * remotePath 是服务器上的路径，localPath 是要保存到的本地文件。
     * 调用后立刻返回，结果走 succeeded 或 failed。
     */
    void download(const QString &host, quint16 port,
                  const QString &user, const QString &pass,
                  const QString &remotePath, const QString &localPath);

    /* 关掉当前会话。正在下载时记为失败。已登录且空闲时发 QUIT。 */
    void closeSession();

    /* 正在连接、登录或收文件时为 true。界面用它决定要不要改按钮。 */
    bool isBusy() const;

    LwftpState state() const;

signals:
    /* 给人看的阶段说明，例如“正在下载”。 */
    void phaseChanged(const QString &text);
    /* 协议收发和状态日志，一行一条。 */
    void logLine(const QString &line);
    /* 已经写入临时文件的字节数。 */
    void progress(qint64 bytes);
    /* 文件已保存到 localPath。 */
    void succeeded(const QString &localPath, qint64 bytes);
    /* result 是 LwftpResult。reason 是给界面的说明。 */
    void failed(int result, const QString &reason);
    /* 控制连接已关掉，且不是失败路径。 */
    void sessionClosed();

private slots:
    /* 控制连接 TCP 已建立。之后等服务器的 220。 */
    void onControlConnected();
    /* 控制连接收到数据，拼进行缓冲。 */
    void onControlReadyRead();
    /* 控制连接断开。正常 QUIT 和异常断开都从这里走。 */
    void onControlDisconnected();
    /* 控制连接套接字错误。对端正常关闭不在这里处理。 */
    void onControlError(QAbstractSocket::SocketError err);

    /* 数据连接 TCP 已建立。文件内容从这条连接来。 */
    void onDataConnected();
    /* 数据连接收到一块文件内容，写入本地临时文件。 */
    void onDataReadyRead();
    /* 数据连接断开。需要和 226 都到齐才算下载完成。 */
    void onDataDisconnected();
    /* 数据连接失败。连不上时改试另一个地址，只试一次。 */
    void onDataError(QAbstractSocket::SocketError err);

    /* m_timeoutMs 内没有新的收发。 */
    void onTimeout();

private:
    /* 去掉 \r 和 \n，避免一条输入被拆成两条 FTP 命令。 */
    static QString stripCrlf(const QString &text);
    /*
     * 解析 PASV 应答正文里的地址。
     * 格式 (h1,h2,h3,h4,p1,p2)，端口 = p1 * 256 + p2。RFC 959。
     * 解析成功时写入 host 和 port。
     */
    static bool parsePasv(const QString &text, QString *host, quint16 *port);

    /*
     * 发送一条控制命令。cmd 不含 CRLF，函数内部补上。PASS 写日志时打码。
     * 发送失败时已经调用 fail()，调用者不要再改状态。
     */
    bool sendLine(const QString &cmd);
    /* 发起控制连接。调用前 m_host、m_port、账号已经填好。 */
    void startConnect();
    /* 已登录后发送 TYPE I，开始本次下载。 */
    void startRetr();
    /*
     * 按 227 打开数据连接，随后由调用者发送 RETR。
     * 地址和当前控制连接不一致时，优先连用户填写的主机，端口仍用 PASV 的端口。
     */
    void openData(const QString &pasvHost, quint16 pasvPort);
    /* 从行缓冲里取出完整行并解析。 */
    void pumpLines();
    /* 处理控制连接上的一行。多行应答在这里拼完再交给状态机。 */
    void handleControlLine(const QByteArray &raw);
    /* 状态机。code 是三位 FTP 应答码，text 是应答正文。 */
    void processReply(int code, const QString &text);
    /* 226 和数据连接关闭都到了，就把临时文件改成用户指定的路径。 */
    void tryComplete();
    /* 结束本次操作。会关掉两个套接字，并删掉未完成的临时文件。 */
    void fail(int result, const QString &reason);
    /* 重新开始空闲计时。 */
    void armTimeout();
    /* 数据套接字恢复到可再次 connect 的状态。 */
    void resetDataSocket();

    QTcpSocket *m_control; /* 控制连接，默认端口 21，只走命令和应答 */
    QTcpSocket *m_data;    /* 数据连接，端口来自 PASV，只走文件字节 */

    QString m_host;        /* 用户填的服务器，域名或 IP */
    quint16 m_port;        /* 控制端口，一般是 21 */
    QString m_user;        /* USER 参数 */
    QString m_pass;        /* PASS 参数 */
    QString m_remotePath;  /* RETR 参数，服务器上的路径 */
    QString m_localPath;   /* 成功后的最终本地路径 */
    QString m_partPath;    /* 下载过程中的临时文件，完成后改名为 m_localPath */

    QString m_dataHost;    /* 当前正在连的数据主机 */
    quint16 m_dataPort;    /* PASV 给出的数据端口 */
    QString m_altDataHost; /* 首选地址失败时再试的另一个主机，空表示没有 */

    QFile m_file;          /* 打开的是 m_partPath */
    QByteArray m_ctrlBuf;  /* 控制连接粘包缓冲，凑齐一行再解析 */
    QString m_multiText;   /* 多行应答（码后面是 '-'）已经拼上的正文 */
    int m_multiCode;       /* 多行应答的应答码，收齐前不变 */
    bool m_inMulti;        /* 正在收一条多行应答 */

    LwftpState m_state;
    LwftpState m_target;   /* TYPE/PASV 之后要进入的传输状态，下载时是 RETR */

    bool m_busy;           /* 从 download() 到成功或失败 */
    bool m_failedGuard;    /* fail() 重入保护，套接字 abort 会同步回调 */
    bool m_relogin;        /* 换了服务器，QUIT 完成后再连接 */
    bool m_got226;         /* 服务器已用 226 表示传输完成 */
    bool m_dataReady;      /* 数据连接 TCP 已建立 */
    bool m_dataDone;       /* 数据连接已断开 */
    bool m_usedAlt;        /* 已经改试过备用数据地址 */
    bool m_ignoreCtrlError;/* 主动 abort 控制连接时忽略错误信号 */
    bool m_ignoreDataError;/* 主动 abort 数据连接时忽略错误信号 */
    bool m_pumping;        /* 正在拆控制连接的行，避免 readyRead 重入 */

    qint64 m_bytes;        /* 已写入临时文件的字节数 */
    int m_timeoutMs;       /* 无收发超时，毫秒 */
    QTimer m_timer;
};

#endif
