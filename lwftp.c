/*
 * lwftp.c : a lightweight FTP client
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
 * 本版本已去除 lwIP 依赖（最小化修改：仅替换网络原语，状态机逻辑保持不变）：
 *  - tcp_pcb/pbuf/tcp_* 等 lwIP 调用换成 lwftp_port.h 的中立接口；
 *  - 收包由 pbuf 链改为 (data,len) 借用缓冲，回调期间有效，无需释放。
 */

#include <string.h>
#include <stdlib.h>
#include "lwftp.h"

#define LWFTP_LOG(msg)  lwftp_port_log msg

#define PTRNLEN(s)  s,(sizeof(s)-1)

static void lwftp_control_process(lwftp_session_t *s, const char *data, uint32_t len);

/** Close control or data pcb
 * @param pointer to pcb
 */
static void lwftp_pcb_close(lwftp_port_pcb_t *tpcb)
{
  lwftp_port_err(tpcb, NULL);
  lwftp_port_recv(tpcb, NULL);
  lwftp_port_sent(tpcb, NULL);
  lwftp_port_close(tpcb);
}

/** Send data
 * @param pointer to lwftp session data
 */
static int lwftp_send_next_data(lwftp_session_t *s)
{
  const char *data;
  int len = 0;
  int error = LWFTP_PORT_OK;

  if (s->data_source) {
    len = s->data_source(s->handle, &data, lwftp_port_mss(s->data_pcb));
    if (len) {
      error = lwftp_port_write(s->data_pcb, data, (uint16_t)len);
      if (error!=LWFTP_PORT_OK) {
        LWFTP_LOG(("lwftp:write failure (%s), not implemented\n",lwftp_port_strerror(error)));
      }
    }
  }
  if (!len) {
    LWFTP_LOG(("lwftp:end of file\n"));
    lwftp_pcb_close(s->data_pcb);
    s->data_pcb = NULL;
  }
  return LWFTP_PORT_OK;
}

/** Handle data connection incoming data
 * @param pointer to lwftp session data
 * @param pointer to data (NULL on EOF)
 * @param number of bytes
 */
static void lwftp_data_recv(void *arg, lwftp_port_pcb_t *tpcb, const char *data, uint32_t len)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;
  (void)tpcb;
  if (data) {
    if (s->data_sink) {
      s->data_sink(s->handle, data, len);
    } else {
      LWFTP_LOG(("lwftp: sinking %d bytes\n",len));
    }
  } else {
    // NULL data means EOF. Close is postponed after
    // the session state machine updates. No need to close right here.
    // Instead we kindly tell data sink we are done
    if (s->data_sink) {
      s->data_sink(s->handle, NULL, 0);
    }
  }
}

/** Handle data connection acknowledge of sent data
 * @param pointer to lwftp session data
 * @param number of bytes sent
 */
static void lwftp_data_sent(void *arg, lwftp_port_pcb_t *tpcb, uint16_t len)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;
  (void)tpcb;

  if ( s->data_source ) {
    s->data_source(s->handle, NULL, len);
  }
  lwftp_send_next_data(s);
}

/** Handle data connection error
 * @param pointer to lwftp session data
 * @param state of connection
 */
static void lwftp_data_err(void *arg, int err)
{
  (void)err;
  if (arg != NULL) {
    lwftp_session_t *s = (lwftp_session_t*)arg;
    LWFTP_LOG(("lwftp:failed/error connecting for data to server (%s)\n",lwftp_port_strerror(err)));
    s->data_pcb = NULL; // No need to de-allocate PCB
    if (s->control_state==LWFTP_XFERING) { // gracefully move control session ahead
      s->control_state = LWFTP_DATAEND;
      lwftp_control_process(s, NULL, 0);
    }
  }
}

/** Process newly connected PCB
 * @param pointer to lwftp session data
 * @param pointer to PCB
 * @param state of connection
 */
static void lwftp_data_connected(void *arg, lwftp_port_pcb_t *tpcb, int err)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;
  (void)tpcb;

  if ( err == LWFTP_PORT_OK ) {
    LWFTP_LOG(("lwftp:connected for data to server\n"));
    s->data_state = LWFTP_CONNECTED;
  } else {
    LWFTP_LOG(("lwftp:err in data_connected (%s)\n",lwftp_port_strerror(err)));
  }
}

/** Open data connection for passive transfer
 * @param pointer to lwftp session data
 * @param pointer to PASV response text (not NUL-terminated)
 * @param length of response text
 */
static int lwftp_data_open(lwftp_session_t *s, const char *data, uint32_t len)
{
  int error;
  char *ptr;
  char line[160];
  uint32_t ip;
  uint16_t data_port;

  // Copy to a NUL-terminated buffer for strtoul/strchr
  if (len >= sizeof(line)) len = sizeof(line)-1;
  memcpy(line, data, len);
  line[len] = 0;

  // Find server connection parameter
  ptr = strchr(line, '(');
  if (!ptr) return LWFTP_PORT_ERR_BUF;
  do {
    unsigned int a = strtoul(ptr+1,&ptr,10);
    unsigned int b = strtoul(ptr+1,&ptr,10);
    unsigned int c = strtoul(ptr+1,&ptr,10);
    unsigned int d = strtoul(ptr+1,&ptr,10);
    ip = ((a&0xff)<<24)|((b&0xff)<<16)|((c&0xff)<<8)|(d&0xff);
  } while(0);
  data_port  = strtoul(ptr+1,&ptr,10) << 8;
  data_port |= strtoul(ptr+1,&ptr,10) & 255;
  if (*ptr!=')') return LWFTP_PORT_ERR_BUF;

  // Open data session
  lwftp_port_arg(s->data_pcb, s);
  lwftp_port_err(s->data_pcb, lwftp_data_err);
  lwftp_port_recv(s->data_pcb, lwftp_data_recv);
  lwftp_port_sent(s->data_pcb, lwftp_data_sent);
  error = lwftp_port_connect(s->data_pcb, ip, data_port, lwftp_data_connected);
  return error;
}

/** Send a message to control connection
 * @param pointer to lwftp session data
 * @param pointer to message string
 * @param message length
 */
static int lwftp_send_msg(lwftp_session_t *s, const char* msg, size_t len)
{
  int error;

  LWFTP_LOG(("lwftp:sending %s",msg));
  error = lwftp_port_write(s->control_pcb, msg, (uint16_t)len);
  if ( error != LWFTP_PORT_OK ) {
      LWFTP_LOG(("lwftp:cannot write (%s)\n",lwftp_port_strerror(error)));
  }
  return error;
}

/** Close data connection
 * @param pointer to lwftp session data
 * @param result to pass to callback fn (if called)
 */
static void lwftp_data_close(lwftp_session_t *s, int result)
{
  if (s->data_pcb) {
    lwftp_pcb_close(s->data_pcb);
    s->data_pcb = NULL;
  }
  if ( s->done_fn ) {
    s->done_fn(s->handle, result);
  }
}

/** Close control connection
 * @param pointer to lwftp session data
 * @param result to pass to callback fn (if called)
 */
static void lwftp_control_close(lwftp_session_t *s, int result)
{
  if (s->data_pcb) {
    lwftp_pcb_close(s->data_pcb);
    s->data_pcb = NULL;
  }
  if (s->control_pcb) {
    lwftp_pcb_close(s->control_pcb);
    s->control_pcb = NULL;
  }
  s->control_state = LWFTP_CLOSED;
  if ( (result >= 0) && s->done_fn ) {
    s->done_fn(s->handle, result);
  }
}

/** Main client state machine
 * @param pointer to lwftp session data
 * @param pointer to incoming response text (NULL = no response)
 * @param length of response text
 */
static void lwftp_control_process(lwftp_session_t *s, const char *data, uint32_t len)
{
  unsigned int response = 0;
  int result = LWFTP_RESULT_ERR_SRVR_RESP;
  (void)len;

  // Try to get response number
  if (data) {
    response = strtoul(data, NULL, 10);
    LWFTP_LOG(("lwftp:got response %d\n",response));
  }

  switch (s->control_state) {
    case LWFTP_CONNECTED:
      if (response>0) {
        if (response==220) {
          lwftp_send_msg(s, PTRNLEN("USER "));
          lwftp_send_msg(s, s->user, strlen(s->user));
          lwftp_send_msg(s, PTRNLEN("\r\n"));
          s->control_state = LWFTP_USER_SENT;
        } else {
          s->control_state = LWFTP_QUIT;
        }
      }
      break;
    case LWFTP_USER_SENT:
      if (response>0) {
        if (response==331) {
          lwftp_send_msg(s, PTRNLEN("PASS "));
          lwftp_send_msg(s, s->pass, strlen(s->pass));
          lwftp_send_msg(s, PTRNLEN("\r\n"));
          s->control_state = LWFTP_PASS_SENT;
        } else {
          s->control_state = LWFTP_QUIT;
        }
      }
      break;
    case LWFTP_PASS_SENT:
      if (response>0) {
        if (response==230) {
          s->control_state = LWFTP_LOGGED;
          LWFTP_LOG(("lwftp: now logged in\n"));
          if (s->done_fn) {
              s->done_fn(s->handle, LWFTP_RESULT_LOGGED);
          }
        } else {
          s->control_state = LWFTP_QUIT;
        }
      }
      break;
    case LWFTP_TYPE_SENT:
      if (response>0) {
        if (response==200) {
          lwftp_send_msg(s, PTRNLEN("PASV\r\n"));
          s->control_state = LWFTP_PASV_SENT;
        } else {
          s->control_state = LWFTP_QUIT;
        }
      }
      break;
    case LWFTP_PASV_SENT:
      if (response>0) {
        if (response==227) {
          lwftp_data_open(s,data,len);
          switch (s->target_state) {
            case LWFTP_STOR_SENT:
              lwftp_send_msg(s, PTRNLEN("STOR "));
              break;
            case LWFTP_RETR_SENT:
              lwftp_send_msg(s, PTRNLEN("RETR "));
              break;
            default:
              LWFTP_LOG(("Unexpected internal state"));
              s->target_state = LWFTP_QUIT;
            }
          lwftp_send_msg(s, s->remote_path, strlen(s->remote_path));
          lwftp_send_msg(s, PTRNLEN("\r\n"));
          s->control_state = s->target_state;
        } else {
          s->control_state = LWFTP_QUIT;
        }
      }
      break;
    case LWFTP_RETR_SENT:
      if (response>0) {
        if (response==150) {
          s->control_state = LWFTP_XFERING;
        } else if (response==550) {
            s->control_state = LWFTP_DATAEND;
            result = LWFTP_RESULT_ERR_FILENAME;
            LWFTP_LOG(("lwftp: Failed to open file '%s'\n", s->remote_path));
        }
        else {
          s->control_state = LWFTP_DATAEND;
          LWFTP_LOG(("lwftp:expected 150, received %d\n",response));
        }
      }
      break;
    case LWFTP_STOR_SENT:
      if (response>0) {
        if (response==150) {
          s->control_state = LWFTP_XFERING;
          lwftp_data_sent(s,NULL,0);
        } else {
          s->control_state = LWFTP_DATAEND;
          LWFTP_LOG(("lwftp:expected 150, received %d\n",response));
        }
      }
      break;
    case LWFTP_XFERING:
      if (response>0) {
        if (response==226) {
          result = LWFTP_RESULT_OK;
        } else {
          result = LWFTP_RESULT_ERR_CLOSED;
          LWFTP_LOG(("lwftp:expected 226, received %d\n",response));
        }
        s->control_state = LWFTP_DATAEND;
      }
      break;
    case LWFTP_DATAEND:
      LWFTP_LOG(("forced end of data session\n"));
      break;
    case LWFTP_QUIT_SENT:
      if (response>0) {
        if (response==221) {
          result = LWFTP_RESULT_OK;
        } else {
          result = LWFTP_RESULT_ERR_UNKNOWN;
          LWFTP_LOG(("lwftp:expected 221, received %d\n",response));
        }
        s->control_state = LWFTP_CLOSING;
      }
      break;
    default:
      LWFTP_LOG(("lwftp:unhandled state (%d)\n",s->control_state));
  }

  // Handle second step in state machine
  switch ( s->control_state ) {
    case LWFTP_DATAEND:
      lwftp_data_close(s, result);
      s->control_state = LWFTP_LOGGED;
      break;
    case LWFTP_QUIT:
      lwftp_send_msg(s, PTRNLEN("QUIT\r\n"));
      lwftp_port_flush(s->control_pcb);
      s->control_state = LWFTP_QUIT_SENT;
      break;
    case LWFTP_CLOSING:
      // this function frees s, no use of s is allowed after
      lwftp_control_close(s, result);
    default:;
  }
}

/** Start a RETR data session
 * @param pointer to lwftp session
 */
static void lwftp_start_RETR(void *arg)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;

  if ( s->control_state == LWFTP_LOGGED ) {
    lwftp_send_msg(s, PTRNLEN("TYPE I\r\n"));
    s->control_state = LWFTP_TYPE_SENT;
    s->target_state = LWFTP_RETR_SENT;
  } else {
    LWFTP_LOG(("Unexpected condition"));
    if (s->done_fn) s->done_fn(s->handle, LWFTP_RESULT_ERR_INTERNAL);
  }
}

/** Start a STOR data session
 * @param pointer to lwftp session
 */
static void lwftp_start_STOR(void *arg)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;

  if ( s->control_state == LWFTP_LOGGED ) {
    lwftp_send_msg(s, PTRNLEN("TYPE I\r\n"));
    s->control_state = LWFTP_TYPE_SENT;
    s->target_state = LWFTP_STOR_SENT;
  } else {
    LWFTP_LOG(("Unexpected condition"));
    if (s->done_fn) s->done_fn(s->handle, LWFTP_RESULT_ERR_INTERNAL);
  }
}

/** Send QUIT to terminate control session
 * @param pointer to lwftp session
 */
static void lwftp_send_QUIT(void *arg)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;

  if (s->control_pcb) {
    lwftp_send_msg(s, PTRNLEN("QUIT\r\n"));
    lwftp_port_flush(s->control_pcb);
    s->control_state = LWFTP_QUIT_SENT;
  }
}

/** Handle control connection incoming data
 * @param pointer to lwftp session data
 * @param pointer to response text (NULL = connection closed by peer)
 * @param length of response text
 */
static void lwftp_control_recv(void *arg, lwftp_port_pcb_t *tpcb, const char *data, uint32_t len)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;
  (void)tpcb;

  if (data) {
    lwftp_control_process(s, data, len);
  } else {
    LWFTP_LOG(("lwftp:connection closed by remote host\n"));
    lwftp_control_close(s, LWFTP_RESULT_ERR_CLOSED);
  }
}

/** Handle control connection acknowledge of sent data
 * @param pointer to lwftp session data
 * @param number of bytes sent
 */
static void lwftp_control_sent(void *arg, lwftp_port_pcb_t *tpcb, uint16_t len)
{
  (void)arg; (void)tpcb;
  LWFTP_LOG(("lwftp:successfully sent %d bytes\n",len));
}

/** Handle control connection error
 * @param pointer to lwftp session data
 * @param state of connection
 */
static void lwftp_control_err(void *arg, int err)
{
  (void)err;
  if (arg != NULL) {
    lwftp_session_t *s = (lwftp_session_t*)arg;
    int result;
    if( s->control_state == LWFTP_CLOSED ) {
      LWFTP_LOG(("lwftp:failed to connect to server (%s)\n",lwftp_port_strerror(err)));
      result = LWFTP_RESULT_ERR_CONNECT;
    } else {
      LWFTP_LOG(("lwftp:connection closed by remote host\n"));
      result = LWFTP_RESULT_ERR_CLOSED;
    }
    s->control_pcb = NULL; // No need to de-allocate PCB
    lwftp_control_close(s, result);
  }
}


/** Process newly connected PCB
 * @param pointer to lwftp session data
 * @param pointer to PCB
 * @param state of connection
 */
static void lwftp_control_connected(void *arg, lwftp_port_pcb_t *tpcb, int err)
{
  lwftp_session_t *s = (lwftp_session_t*)arg;
  (void)tpcb;

  if ( err == LWFTP_PORT_OK ) {
    LWFTP_LOG(("lwftp:connected to server\n"));
      s->control_state = LWFTP_CONNECTED;
  } else {
    LWFTP_LOG(("lwftp:err in control_connected (%s)\n",lwftp_port_strerror(err)));
  }
}


/** Open a control session
 * @param Session structure
 */
int lwftp_connect(lwftp_session_t *s)
{
  int error;
  enum lwftp_results retval = LWFTP_RESULT_ERR_UNKNOWN;

  // Check user supplied data
  if ( (s->control_state!=LWFTP_CLOSED) ||
       s->control_pcb ||
       s->data_pcb ||
       !s->user ||
       !s->pass )
  {
    LWFTP_LOG(("lwftp:invalid control session\n"));
    retval = LWFTP_RESULT_ERR_ARGUMENT;
    goto exit;
  }
  // Get sessions pcb
  s->control_pcb = lwftp_port_new();
  if (!s->control_pcb) {
    LWFTP_LOG(("lwftp:cannot alloc control_pcb (low memory?)\n"));
    retval = LWFTP_RESULT_ERR_MEMORY;
    goto exit;
  }
  // Open control session
  lwftp_port_arg(s->control_pcb, s);
  lwftp_port_err(s->control_pcb, lwftp_control_err);
  lwftp_port_recv(s->control_pcb, lwftp_control_recv);
  lwftp_port_sent(s->control_pcb, lwftp_control_sent);
  error = lwftp_port_connect(s->control_pcb, s->server_ip, s->server_port, lwftp_control_connected);
  if ( error == LWFTP_PORT_OK ) {
    retval = LWFTP_RESULT_INPROGRESS;
    goto exit;
  }

  // Release pcbs in case of failure
  LWFTP_LOG(("lwftp:cannot connect control_pcb (%s)\n", lwftp_port_strerror(error)));
  lwftp_control_close(s, -1);

exit:
  if (s->done_fn) s->done_fn(s->handle, retval);
  return retval;
}


/** Retrieve data from a remote file
 * @param Session structure
 */
int lwftp_retrieve(lwftp_session_t *s)
{
  int error;
  enum lwftp_results retval = LWFTP_RESULT_ERR_UNKNOWN;

  // Check user supplied data
  if ( (s->control_state!=LWFTP_LOGGED) ||
       !s->remote_path ||
       s->data_pcb )
  {
    LWFTP_LOG(("lwftp:invalid session data\n"));
    retval = LWFTP_RESULT_ERR_ARGUMENT;
    goto exit;
  }
  // Get data pcb
  s->data_pcb = lwftp_port_new();
  if (!s->data_pcb) {
    LWFTP_LOG(("lwftp:cannot alloc data_pcb (low memory?)\n"));
    retval = LWFTP_RESULT_ERR_MEMORY;
    goto exit;
  }
  // Initiate transfer
  error = lwftp_port_post(lwftp_start_RETR, s);
  if ( error == LWFTP_PORT_OK ) {
    retval = LWFTP_RESULT_INPROGRESS;
  } else {
    LWFTP_LOG(("cannot start RETR (%s)",lwftp_port_strerror(error)));
    retval = LWFTP_RESULT_ERR_INTERNAL;
  }

exit:
  if (s->done_fn) s->done_fn(s->handle, retval);
  return retval;
}


/** Store data to a remote file
 * @param Session structure
 */
int lwftp_store(lwftp_session_t *s)
{
  int error;
  enum lwftp_results retval = LWFTP_RESULT_ERR_UNKNOWN;

  // Check user supplied data
  if ( (s->control_state!=LWFTP_LOGGED) ||
       !s->remote_path ||
       s->data_pcb )
  {
    LWFTP_LOG(("lwftp:invalid session data\n"));
    retval = LWFTP_RESULT_ERR_ARGUMENT;
    goto exit;
  }
  // Get data pcb
  s->data_pcb = lwftp_port_new();
  if (!s->data_pcb) {
    LWFTP_LOG(("lwftp:cannot alloc data_pcb (low memory?)\n"));
    retval = LWFTP_RESULT_ERR_MEMORY;
    goto exit;
  }
  // Initiate transfer
  error = lwftp_port_post(lwftp_start_STOR, s);
  if ( error == LWFTP_PORT_OK ) {
    retval = LWFTP_RESULT_INPROGRESS;
  } else {
    LWFTP_LOG(("cannot start STOR (%s)",lwftp_port_strerror(error)));
    retval = LWFTP_RESULT_ERR_INTERNAL;
  }

exit:
  if (s->done_fn) s->done_fn(s->handle, retval);
  return retval;
}


/** Terminate FTP session
 * @param Session structure
 */
void lwftp_close(lwftp_session_t *s)
{
  int error;

  // Nothing to do when already closed
  if ( s->control_state == LWFTP_CLOSED ) return;

  // Initiate transfer
  error = lwftp_port_post(lwftp_send_QUIT, s);
  if ( error != LWFTP_PORT_OK ) {
    // This is a critical error, try to close anyway
    // polling process may save us
    LWFTP_LOG(("cannot request for close"));
    s->control_state = LWFTP_QUIT;
  }
}
