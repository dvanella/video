/*****************************************************************************
 * connmgr.c: HTTP/TLS VLC connection manager
 *****************************************************************************
 * Copyright © 2015 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <vlc_common.h>
#include <vlc_tls.h>
#include <vlc_http.h>
#include <vlc_interrupt.h>
#include "transport.h"
#include "conn.h"
#include "connmgr.h"
#include "message.h"

#pragma GCC visibility push(default)

struct vlc_https_connecting
{
    vlc_tls_creds_t *creds;
    const char *host;
    unsigned port;
    bool http2;
    vlc_sem_t done;
};

static void *vlc_https_connect_thread(void *data)
{
    struct vlc_https_connecting *c = data;
    vlc_tls_t *tls;

    tls = vlc_https_connect(c->creds, c->host, c->port, &c->http2);
    vlc_sem_post(&c->done);
    return tls;
}

/** Interruptible vlc_https_connect() */
static vlc_tls_t *vlc_https_connect_i11e(vlc_tls_creds_t *creds,
                                         const char *host, unsigned port,
                                         bool *restrict http_two)
{
    struct vlc_https_connecting c;
    vlc_thread_t th;

    c.creds = creds;
    c.host = host;
    c.port = port;
    vlc_sem_init(&c.done, 0);

    if (vlc_clone(&th, vlc_https_connect_thread, &c,
                  VLC_THREAD_PRIORITY_INPUT))
        return NULL;

    /* This would be much simpler if vlc_join_i11e() existed. */
    void *res;

    if (vlc_sem_wait_i11e(&c.done))
        vlc_cancel(th);
    vlc_join(th, &res);
    vlc_sem_destroy(&c.done);

    if (res == VLC_THREAD_CANCELED)
        res = NULL;
    if (res != NULL)
        *http_two = c.http2;
    return res;
}

struct vlc_http_connecting
{
    vlc_object_t *obj;
    const char *host;
    unsigned port;
    vlc_sem_t done;
};

static void *vlc_http_connect_thread(void *data)
{
    struct vlc_http_connecting *c = data;
    vlc_tls_t *tls;

    tls = vlc_http_connect(c->obj, c->host, c->port);
    vlc_sem_post(&c->done);
    return tls;
}

/** Interruptible vlc_http_connect() */
static vlc_tls_t *vlc_http_connect_i11e(vlc_object_t *obj,
                                        const char *host, unsigned port)
{
    struct vlc_http_connecting c;
    vlc_thread_t th;

    c.obj = obj;
    c.host = host;
    c.port = port;
    vlc_sem_init(&c.done, 0);

    if (vlc_clone(&th, vlc_http_connect_thread, &c, VLC_THREAD_PRIORITY_INPUT))
        return NULL;

    void *res;

    if (vlc_sem_wait_i11e(&c.done))
        vlc_cancel(th);
    vlc_join(th, &res);
    vlc_sem_destroy(&c.done);

    if (res == VLC_THREAD_CANCELED)
        res = NULL;
    return res;
}


struct vlc_http_mgr
{
    vlc_object_t *obj;
    vlc_tls_creds_t *creds;
    vlc_http_cookie_jar_t *jar;
    struct vlc_http_conn *conn;
    bool use_h2c;
};

static struct vlc_http_conn *vlc_http_mgr_find(struct vlc_http_mgr *mgr,
                                               const char *host, unsigned port)
{
    (void) host; (void) port;
    return mgr->conn;
}

static void vlc_http_mgr_release(struct vlc_http_mgr *mgr,
                                 struct vlc_http_conn *conn)
{
    assert(mgr->conn == conn);
    mgr->conn = NULL;

    vlc_http_conn_release(conn);
}

static
struct vlc_http_msg *vlc_http_mgr_reuse(struct vlc_http_mgr *mgr,
                                        const char *host, unsigned port,
                                        const struct vlc_http_msg *req)
{
    struct vlc_http_conn *conn = vlc_http_mgr_find(mgr, host, port);
    if (conn == NULL)
        return NULL;

    struct vlc_http_stream *stream = vlc_http_stream_open(conn, req);
    if (stream != NULL)
    {
        struct vlc_http_msg *m = vlc_http_stream_read_headers(stream);
        if (m != NULL)
            return m;

        vlc_http_stream_close(stream, false);
        /* NOTE: If the request were not idempotent, we would not know if it
         * was processed by the other end. Thus POST is not used/supported so
         * far, and CONNECT is treated as if it were idempotent (which works
         * fine here). */
    }
    /* Get rid of closing or reset connection */
    vlc_http_mgr_release(mgr, conn);
    return NULL;
}

static struct vlc_http_msg *vlc_https_request(struct vlc_http_mgr *mgr,
                                              const char *host, unsigned port,
                                              const struct vlc_http_msg *req)
{
    if (mgr->creds == NULL && mgr->conn != NULL)
        return NULL; /* switch from HTTP to HTTPS not implemented */

    if (mgr->creds == NULL)
    {   /* First TLS connection: load x509 credentials */
        mgr->creds = vlc_tls_ClientCreate(mgr->obj);
        if (mgr->creds == NULL)
            return NULL;
    }

    /* TODO? non-idempotent request support */
    struct vlc_http_msg *resp = vlc_http_mgr_reuse(mgr, host, port, req);
    if (resp != NULL)
        return resp; /* existing connection reused */

    bool http2;
    vlc_tls_t *tls = vlc_https_connect_i11e(mgr->creds, host, port, &http2);
    if (tls == NULL)
        return NULL;

    struct vlc_http_conn *conn;

    /* For HTTPS, TLS-ALPN determines whether HTTP version 2.0 ("h2") or 1.1
     * ("http/1.1") is used.
     * NOTE: If the negotiated protocol is explicitly "http/1.1", HTTP 1.0
     * should not be used. HTTP 1.0 should only be used if ALPN is not
     * supported by the server.
     * NOTE: We do not enforce TLS version 1.2 for HTTP 2.0 explicitly.
     */
    if (http2)
        conn = vlc_h2_conn_create(tls);
    else
        conn = vlc_h1_conn_create(tls);

    if (unlikely(conn == NULL))
    {
        vlc_tls_Close(tls);
        return NULL;
    }

    mgr->conn = conn;

    return vlc_http_mgr_reuse(mgr, host, port, req);
}

static struct vlc_http_msg *vlc_http_request(struct vlc_http_mgr *mgr,
                                             const char *host, unsigned port,
                                             const struct vlc_http_msg *req)
{
    if (mgr->creds != NULL && mgr->conn != NULL)
        return NULL; /* switch from HTTPS to HTTP not implemented */

    struct vlc_http_msg *resp = vlc_http_mgr_reuse(mgr, host, port, req);
    if (resp != NULL)
        return resp;

    vlc_tls_t *tls = vlc_http_connect_i11e(mgr->obj, host, port);
    if (tls == NULL)
        return NULL;

    struct vlc_http_conn *conn;

    if (mgr->use_h2c)
        conn = vlc_h2_conn_create(tls);
    else
        conn = vlc_h1_conn_create(tls);

    if (unlikely(conn == NULL))
    {
        vlc_tls_Close(tls);
        return NULL;
    }

    mgr->conn = conn;

    return vlc_http_mgr_reuse(mgr, host, port, req);
}

struct vlc_http_msg *vlc_http_mgr_request(struct vlc_http_mgr *mgr, bool https,
                                          const char *host, unsigned port,
                                          const struct vlc_http_msg *m)
{
    return (https ? vlc_https_request : vlc_http_request)(mgr, host, port, m);
}

int vlc_http_mgr_send_cookies(struct vlc_http_mgr *mgr, bool https,
                              const char *host, const char *path,
                              struct vlc_http_msg *req)
{
    int ret = 0;

    if (mgr->jar != NULL)
    {
        char *cookies = vlc_http_cookies_fetch(mgr->jar, https, host, path);
        if (cookies != NULL)
        {
            msg_Dbg(mgr->obj, "retrieved cookies: %s", cookies);
            ret = vlc_http_msg_add_header(req, "Cookie", "%s", cookies);
            free(cookies);
        }
    }
    return ret;
}

void vlc_http_mgr_recv_cookies(struct vlc_http_mgr *mgr, bool https,
                               const char *host, const char *path,
                               const struct vlc_http_msg *resp)
{
    if (mgr->jar != NULL)
    {   /* FIXME: fold multiple Set-Cookies headers with ';' */
        const char *cookies = vlc_http_msg_get_header(resp, "Set-Cookie");
        if (cookies != NULL
         && vlc_http_cookies_store(mgr->jar, cookies, https, host, path))
            msg_Dbg(mgr->obj, "stored cookie: %s", cookies);
    }
}

struct vlc_http_mgr *vlc_http_mgr_create(vlc_object_t *obj,
                                         vlc_http_cookie_jar_t *jar, bool h2c)
{
    struct vlc_http_mgr *mgr = malloc(sizeof (*mgr));
    if (unlikely(mgr == NULL))
        return NULL;

    mgr->obj = obj;
    mgr->creds = NULL;
    mgr->jar = jar;
    mgr->conn = NULL;
    mgr->use_h2c = h2c;
    return mgr;
}

void vlc_http_mgr_destroy(struct vlc_http_mgr *mgr)
{
    if (mgr->conn != NULL)
        vlc_http_mgr_release(mgr, mgr->conn);
    if (mgr->creds != NULL)
        vlc_tls_Delete(mgr->creds);
    free(mgr);
}