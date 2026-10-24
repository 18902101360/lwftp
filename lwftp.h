/*
 * lwftp.h : a lightweight FTP client
 *
 * Copyright (c) 2014 GEZEDO
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * Author: Laurent GONZALEZ <lwip@gezedo.com>
 *
 */

/*
 * 本版本已去除 lwIP 依赖：网络收发全部通过 lwftp_port.h 的中立 C 接口，
 * 由应用方用任意 TCP 设施实现（本仓库用 Qt 的 QTcpSocket 实现）。
 */

#ifndef LWFTP_H
#define LWFTP_H

#include <stdint.h>
#include "lwftp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

enum lwftp_results {
  LWFTP_RESULT_OK=0,
  LWFTP_RESULT_INPROGRESS,
  LWFTP_RESULT_LOGGED,
  LWFTP_RESULT_ERR_UNKNOWN,   /** Unknown error */
  LWFTP_RESULT_ERR_ARGUMENT,  /** Wrong argument */
  LWFTP_RESULT_ERR_MEMORY,    /** Out of memory */
  LWFTP_RESULT_ERR_CONNECT,   /** Connection to server failed */
  LWFTP_RESULT_ERR_HOSTNAME,  /** Failed to resolve server hostname */
  LWFTP_RESULT_ERR_CLOSED,    /** Connection unexpectedly closed by remote server */
  LWFTP_RESULT_ERR_TIMEOUT,   /** Connection timed out (server didn't respond in time) */
  LWFTP_RESULT_ERR_SRVR_RESP, /** Server responded with an unknown response code */
  LWFTP_RESULT_ERR_INTERNAL,  /** Internal network stack error */
  LWFTP_RESULT_ERR_LOCAL,     /** Local storage error */
  LWFTP_RESULT_ERR_FILENAME   /** Remote host could not find file */
};

/** LWFTP control connection state */
typedef enum  {
  LWFTP_CLOSED=0,
  LWFTP_CONNECTED,
  LWFTP_USER_SENT,
  LWFTP_PASS_SENT,
  LWFTP_LOGGED,
  LWFTP_TYPE_SENT,
  LWFTP_PASV_SENT,
  LWFTP_RETR_SENT,
  LWFTP_STOR_SENT,
  LWFTP_XFERING,
  LWFTP_DATAEND,
  LWFTP_QUIT,
  LWFTP_QUIT_SENT,
  LWFTP_CLOSING,
} lwftp_state_t;

/** LWFTP session structure */
typedef struct {
  // User interface
  uint32_t      server_ip;      /* 主机字节序 IPv4，可用 lwftp_port_ip_from_str() */
  uint16_t      server_port;
  const char    *remote_path;
  const char    *user;
  const char    *pass;
  void          *handle;
  unsigned      (*data_source)(void*, const char**, unsigned);
  unsigned      (*data_sink)(void*, const char*, unsigned);
  void          (*done_fn)(void*, int);
  unsigned      timeout;
  // Internal data
  lwftp_state_t   control_state;
  lwftp_state_t   target_state;
  lwftp_state_t   data_state;
  lwftp_port_pcb_t *control_pcb;
  lwftp_port_pcb_t *data_pcb;
} lwftp_session_t;

// LWFTP API
int  lwftp_connect(lwftp_session_t *s);
int  lwftp_store(lwftp_session_t *s);
int  lwftp_retrieve(lwftp_session_t *s);
void lwftp_close(lwftp_session_t *s);

#ifdef __cplusplus
}
#endif

#endif // LWFTP_H
