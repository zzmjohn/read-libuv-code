/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* Expose glibc-specific EAI_* error codes. Needs to be defined before we
 * include any headers.
 */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include "uv.h"
#include "internal.h"

#include <errno.h>
#include <stddef.h> /* NULL */
#include <stdlib.h>
#include <string.h>
#include <net/if.h> /* if_indextoname() */

/* EAI_* constants. */
#include <netdb.h>


int uv__getaddrinfo_translate_error(int sys_err) {
  switch (sys_err) {
  case 0: return 0;
#if defined(EAI_ADDRFAMILY)
  case EAI_ADDRFAMILY: return UV_EAI_ADDRFAMILY;
#endif
#if defined(EAI_AGAIN)
  case EAI_AGAIN: return UV_EAI_AGAIN;
#endif
#if defined(EAI_BADFLAGS)
  case EAI_BADFLAGS: return UV_EAI_BADFLAGS;
#endif
#if defined(EAI_BADHINTS)
  case EAI_BADHINTS: return UV_EAI_BADHINTS;
#endif
#if defined(EAI_CANCELED)
  case EAI_CANCELED: return UV_EAI_CANCELED;
#endif
#if defined(EAI_FAIL)
  case EAI_FAIL: return UV_EAI_FAIL;
#endif
#if defined(EAI_FAMILY)
  case EAI_FAMILY: return UV_EAI_FAMILY;
#endif
#if defined(EAI_MEMORY)
  case EAI_MEMORY: return UV_EAI_MEMORY;
#endif
#if defined(EAI_NODATA)
  case EAI_NODATA: return UV_EAI_NODATA;
#endif
#if defined(EAI_NONAME)
# if !defined(EAI_NODATA) || EAI_NODATA != EAI_NONAME
  case EAI_NONAME: return UV_EAI_NONAME;
# endif
#endif
#if defined(EAI_OVERFLOW)
  case EAI_OVERFLOW: return UV_EAI_OVERFLOW;
#endif
#if defined(EAI_PROTOCOL)
  case EAI_PROTOCOL: return UV_EAI_PROTOCOL;
#endif
#if defined(EAI_SERVICE)
  case EAI_SERVICE: return UV_EAI_SERVICE;
#endif
#if defined(EAI_SOCKTYPE)
  case EAI_SOCKTYPE: return UV_EAI_SOCKTYPE;
#endif
#if defined(EAI_SYSTEM)
  case EAI_SYSTEM: return UV__ERR(errno);
#endif
  }
  assert(!"unknown EAI_* error code");
  abort();
  return 0;  /* Pacify compiler. */
}

// dns解析的工作函数
static void uv__getaddrinfo_work(struct uv__work* w) {
  uv_getaddrinfo_t* req;
  int err;
  // 根据结构体的字段获取结构体首地址
  req = container_of(w, uv_getaddrinfo_t, work_req);
  // 阻塞在这
  err = getaddrinfo(req->hostname, req->service, req->hints, &req->addrinfo);
  req->retcode = uv__getaddrinfo_translate_error(err);
}

// dns解析完执行的函数
static void uv__getaddrinfo_done(struct uv__work* w, int status) {
  uv_getaddrinfo_t* req;

  req = container_of(w, uv_getaddrinfo_t, work_req);
  uv__req_unregister(req->loop, req);

  /* See initialization in uv_getaddrinfo(). */
  // 释放初始化时申请的内存
  if (req->hints)
    uv__free(req->hints);
  else if (req->service)
    uv__free(req->service);
  else if (req->hostname)
    uv__free(req->hostname);
  else
    assert(0);

  req->hints = NULL;
  req->service = NULL;
  req->hostname = NULL;

  if (status == UV_ECANCELED) {
    assert(req->retcode == 0);
    req->retcode = UV_EAI_CANCELED;
  }
  // 执行上层回调
  if (req->cb)
    req->cb(req, req->retcode, req->addrinfo);
}

// dns解析的入口函数
int uv_getaddrinfo(uv_loop_t* loop,
                  // 上层传进来的req
                   uv_getaddrinfo_t* req,
                   // 解析完后的上层回调
                   uv_getaddrinfo_cb cb,
                   // 需要解析的名字
                   const char* hostname,
                   // 查询的过滤条件：服务名。比如http smtp。也可以是一个端口。见下面注释
                   const char* service,
                   // 其他查询过滤条件
                   const struct addrinfo* hints) {
  /*
    service 
           netstat         15/tcp
           qotd            17/tcp          quote
           msp             18/tcp          # message send protocol
           msp             18/udp          # message send protocol
           chargen         19/tcp          ttytst source
           chargen         19/udp          ttytst source
           ftp             21/tcp
           telnet          23/tcp

    struct addrinfo {
          // 各种标记
          int              ai_flags;
          // 下面三个参数对应socket函数的三个入参。
          int              ai_family;
          int              ai_socktype;
          int              ai_protocol;
          // ai_addr结构体长度 
          socklen_t        ai_addrlen;
          struct sockaddr *ai_addr;
          char            *ai_canonname;
          struct addrinfo *ai_next;
    };
  */
  size_t hostname_len;
  size_t service_len;
  size_t hints_len;
  size_t len;
  char* buf;

  if (req == NULL || (hostname == NULL && service == NULL))
    return UV_EINVAL;

  hostname_len = hostname ? strlen(hostname) + 1 : 0;
  service_len = service ? strlen(service) + 1 : 0;
  hints_len = hints ? sizeof(*hints) : 0;
  buf = uv__malloc(hostname_len + service_len + hints_len);

  if (buf == NULL)
    return UV_ENOMEM;

  uv__req_init(loop, req, UV_GETADDRINFO);
  req->loop = loop;
  // 设置请求的回调
  req->cb = cb;
  req->addrinfo = NULL;
  req->hints = NULL;
  req->service = NULL;
  req->hostname = NULL;
  req->retcode = 0;

  /* order matters, see uv_getaddrinfo_done() */
  len = 0;

  if (hints) {
    req->hints = memcpy(buf + len, hints, sizeof(*hints));
    len += sizeof(*hints);
  }

  if (service) {
    req->service = memcpy(buf + len, service, service_len);
    len += service_len;
  }

  if (hostname)
    req->hostname = memcpy(buf + len, hostname, hostname_len);
  // 传了cb是异步
  if (cb) {
    uv__work_submit(loop,
                    &req->work_req,
                    UV__WORK_SLOW_IO,
                    uv__getaddrinfo_work,
                    uv__getaddrinfo_done);
    return 0;
  } else {
    // 阻塞式查询，然后执行回调
    uv__getaddrinfo_work(&req->work_req);
    uv__getaddrinfo_done(&req->work_req, 0);
    return req->retcode;
  }
}


void uv_freeaddrinfo(struct addrinfo* ai) {
  if (ai)
    freeaddrinfo(ai);
}


int uv_if_indextoname(unsigned int ifindex, char* buffer, size_t* size) {
  char ifname_buf[UV_IF_NAMESIZE];
  size_t len;

  if (buffer == NULL || size == NULL || *size == 0)
    return UV_EINVAL;

  if (if_indextoname(ifindex, ifname_buf) == NULL)
    return UV__ERR(errno);

  len = strnlen(ifname_buf, sizeof(ifname_buf));

  if (*size <= len) {
    *size = len + 1;
    return UV_ENOBUFS;
  }

  memcpy(buffer, ifname_buf, len);
  buffer[len] = '\0';
  *size = len;

  return 0;
}

int uv_if_indextoiid(unsigned int ifindex, char* buffer, size_t* size) {
  return uv_if_indextoname(ifindex, buffer, size);
}
