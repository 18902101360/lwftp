# lwftp（去 lwIP 依赖版）

轻量级 FTP 客户端库。原版 [lwftp](https://github.com/Gezedo/lwftp)（Laurent Gonzalez，BSD 许可）
直接绑定 lwIP 的 raw API；本版本把全部网络收发抽象成中立 C 移植接口
[`lwftp_port.h`](lwftp_port.h)，核心 [`lwftp.c`](lwftp.c) 不再依赖任何 TCP/IP 协议栈，
可以移植到 Qt、单片机裸机/RTOS + AT 模块等任何具备 TCP 能力的平台。

库本体是一个事件驱动的状态机，支持 4 个基本操作：`connect` / `store` / `retrieve` / `close`。
要求服务器支持二进制传输（TYPE I）和被动模式（PASV）。

特点：

- **零动态内存**：库内无 `malloc`，会话结构由调用方分配（可静态全局），唯一栈缓冲 160 字节
- **无 OS/线程依赖**：所有回调在同一"网络上下文"触发，裸机主循环即可驱动
- **无内部定时器**：超时由应用层看门狗负责
- 核心约 700 行 C

## 目录结构

| 文件 | 说明 |
|---|---|
| `lwftp.h` / `lwftp.c` | 平台无关的 FTP 客户端核心（移植时不需要改） |
| `lwftp_port.h` | 移植接口：15 个函数，由目标平台实现 |
| `qtgui/` | Qt5 参考实现：`lwftp_port_qt.cpp`（QTcpSocket 移植层）+ 中文界面客户端 |
| `qt/` | 旧版 Qt 客户端（已过时，仅存档） |

## 库 API 速览

```c
lwftp_session_t s;
memset(&s, 0, sizeof(s));
s.server_ip   = lwftp_port_ip_from_str("192.168.1.10");  /* 主机字节序 IPv4 */
s.server_port = 21;
s.user = "user";  s.pass = "pass";
s.handle      = &my_ctx;             /* 透传给全部回调 */
s.data_sink   = my_sink;             /* 下载：写数据；ptr==NULL 表示 EOF */
s.data_source = my_source;           /* 上传：供数据；pptr==NULL 表示发送确认 */
s.done_fn     = my_done;             /* 各阶段结果上报 */
lwftp_connect(&s);                   /* 之后 done_fn(LOGGED) 表示登录成功 */
lwftp_retrieve(&s); / lwftp_store(&s); / lwftp_close(&s);
```

结果码见 `lwftp.h` 的 `enum lwftp_results`（`INPROGRESS`/`LOGGED`/`OK`/各类错误）。

**data_source 契约**（上传）：`*pptr` 非空时是供数请求，返回可用字节数（≤ maxlen）；
`*pptr == NULL` 时是发送确认通知，`maxlen` 为自上次调用起已确认的字节数（用于进度统计）。

**data_sink 契约**（下载）：`ptr` 非空时是数据块（借用缓冲，回调返回后失效）；
`ptr == NULL` 表示数据连接 EOF。

---

## 移植指南

移植 = **用你的平台的 TCP 设施实现 `lwftp_port.h` 里的 15 个函数**，核心一行不改。

### 1. 接口契约（关键语义，逐条落实）

| 函数 | 契约要点 |
|---|---|
| `lwftp_port_new` | 创建不透明句柄。库同时最多 2 个（控制+数据），可用静态池 |
| `lwftp_port_arg` / `recv` / `sent` / `err` | 登记回调指针 |
| `lwftp_port_connect` | **异步**连接，结果经 `connected` 回调上报；`ip` 为主机字节序 |
| `lwftp_port_write` | 受理发送。**返回 OK 即承诺不丢数据**：库对 write 失败只记日志不重试，缓冲满时应排队/等待，不要轻易报错 |
| `lwftp_port_flush` | 立即把缓冲尽量发出（协议栈无缓冲概念时可为空操作） |
| `lwftp_port_close` | 把已缓冲数据发完再 FIN，之后句柄失效。**close 后不得再触发 sent 回调**（库已视其为关闭，会误操作空指针） |
| `lwftp_port_mss` | 单次喂数上限 = 上传泵的步进。按底层一次发送的能力选取（见第 3 节） |
| `lwftp_port_post` | 把 `fn(arg)` 投递到"网络上下文"执行；若当前已在该上下文（裸机主循环），直接同步调用即可 |
| `lwftp_port_ip_from_str` / `strerror` / `log` | 琐碎辅助函数 |

**recv 回调** `(arg, pcb, data, len)`：

- `data` 是**借用缓冲**，仅在回调期间有效，需要留存就自己拷贝；
- `data == NULL` 表示对端关闭（EOF）——RETR 的收尾完全依赖它，必须实现。

**sent 回调** `(arg, pcb, len)`：

- 发送确认语义，是上传状态机的泵：**每触发一次，库喂下一块**（大小 ≤ mss）；
- 可以做低水位门控合并回调以减少事件往返，但 `len` 的累计必须精确——它驱动上层进度统计；
  注意 `len` 是 `uint16`，两次回调间隔内确认总量不能超过 65535。

**err 回调**：致命错误。**调用后句柄立即失效**，库保证此后不再触碰它（包括不再 close）。

### 2. 上下文模型

- 所有回调（recv/sent/err/connected/done_fn/data_source/data_sink）都在同一个
  **"网络上下文"**（网络线程、协议栈任务或裸机主循环）中触发；
- 应用从其它上下文调用 `lwftp_connect/retrieve/store/close` 是安全的——库内部用
  `lwftp_port_post` 把后续动作转回网络上下文；
- 库不带超时机制：应用层挂一个看门狗，任何进展（收到数据/确认/结果）就喂狗，
  超时则调用 `lwftp_close` 或直接销毁会话（参考 `qtgui/ftpengine.cpp` 的 30 秒看门狗）。

### 3. 参考实现：Qt 移植层（`qtgui/lwftp_port_qt.cpp`，约 280 行）

- 所有 `QTcpSocket` 活在专用网络线程，跨线程经 `post` → `QMetaObject::invokeMethod`；
- **控制连接按行切分**（保证一条 FTP 应答一次回调，避免 150/226 粘包被状态机误读），
  **数据连接按 32KB 整块**（二进制内容里的 `0x0A` 不应造成碎块）；
- 服务器按"数据 → FIN → 226"发送，226 可能被 Qt 先调度：数据连接"收过数据且未见 FIN"时
  控制应答推迟一拍事件循环，防止库提前关闭数据连接丢尾部；
- 上传泵：`mss = 32768`，`bytesWritten` 低水位门控（Qt 缓冲低于一个 mss 才上报确认），
  既防慢服务器时缓冲无界膨胀，又保证进度字节精确；
- **Windows 大坑**：`QTcpSocket` 默认采用系统代理，而代理绕过列表通常不含 `169.254.*`
  等直连地址，连接会被静默劫走报 `The proxy type is invalid for this operation`——
  务必 `setProxy(QNetworkProxy::NoProxy)`；
- 调试时把 `errorString()` 打到日志，别把所有错误都映射成一个码。

### 4. 移植到单片机（AT 模块）

接口 → AT 指令映射（以 ESP 系 AT 固件为例）：

| 移植接口 | AT 实现 |
|---|---|
| `new` | 从静态池取链路号（link id） |
| `connect` | `AT+CIPSTART=<id>,"TCP",ip,port`，`CONNECT OK` → connected 回调 |
| `write` | `AT+CIPSEND=<id>,<len>` → 等 `>` → 吐数据；内部小队列 |
| `sent` 回调 | 收到 `SEND OK` 时触发 |
| `flush` | 空操作 |
| `close` | 发送队列排空后 `AT+CIPCLOSE=<id>` |
| `mss` | 模块单次 `CIPSEND` 上限（ESP8266 为 2048，可填 1460/2048） |
| recv / EOF | 解析 `+IPD:<id>,<len>:` 定长帧，载荷直递回调；`CLOSED` → `recv(NULL)` |
| err 回调 | `CLOSED`/`ERROR`/链路易位等 URC 时触发 |
| `post` | 裸机主循环里直接同步调用 |

外加一个 AT 引擎：UART 中断收字节 → 环形缓冲 → 行/帧解析状态机 → 分发 URC。
总计约 300~500 行 C。

注意事项：

1. **模块必须支持多连接**（`AT+CIPMUX=1`，≥2 条 TCP）：FTP 同时挂控制+数据两条连接；
   数据通道是客户端主动连 PASV 地址，纯出栈方向，不需要监听。NB-IoT 模块多为 UDP/单连接，不适合。
2. **必须走定长收发**（`CIPSEND <len>` + `+IPD`），不要用透传模式——`+++` 逃逸序列会损坏二进制文件。
3. **波特率是吞吐天花板**：115200 ≈ 11 KB/s，建议提到 460800/921600。
4. **上传吞吐 ≈ mss ÷ 单条 `CIPSEND` 往返时间**（库每收到一次 sent 才喂下一块），块取大些划算。
5. **发送队列要留够深度**：`write` 失败库不重试，这一块数据就丢了。

---

## Qt 客户端构建与自测

```bat
rem 以 Qt 5.14.1 MinGW64 为例
set PATH=D:\Qt\Qt5.14.1\5.14.1\mingw73_64\bin;D:\Qt\Qt5.14.1\Tools\mingw730_64\bin;%PATH%
qmake ..\lwftp\qtgui\qtgui.pro
mingw32-make
```

自带命令行自动测试模式（无界面，便于回归）：

```bat
lwftp_gui.exe --auto <host> <port> <user> <pass> dl|ul <remote> <local>
rem 成功输出 AUTOTEST RESULT ok=1 bytes=<字节数>；失败 ok=0 并带原因
```

## 许可与致谢

- 原版 lwftp：Copyright (c) 2014 GEZEDO / Laurent Gonzalez，BSD-3 许可（见 `lwftp.c` 头部）。
- 本仓库的修改与移植层代码沿用同一许可。
