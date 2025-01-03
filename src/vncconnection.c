/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>

#include "vncconnection.h"
#include "vncconnectionenums.h"
#include "vncmarshal.h"
#include "vncutil.h"

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "coroutine.h"
#include "d3des.h"

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#ifdef HAVE_SASL
#include <sasl/sasl.h>
#endif

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#include <zlib.h>

#include "dh.h"

#if GLIB_CHECK_VERSION(2, 31, 0)
#define g_mutex_new() g_new0(GMutex, 1)
#define g_mutex_free(m) g_free(m)
#endif

/*
 * When GNUTLS >= 2.12, we must not initialize gcrypt threading
 * because GNUTLS will do that itself, *provided* it is built
 * against gcrypt, and not nettle.
 * When GNUTLS < 2.12 we must always initialize gcrypt threading
 * When GNUTLS > 3.0 we must always initialize gcrypt threading
 *
 * But....
 *
 * When gcrypt >= 1.6.0 we must not initialize gcrypt threading
 * because gcrypt will do that itself.
 *
 * So we need to init grypt threading if
 *
 *   - gcrypt < 1.6.0
 *
 *   and either
 *
 *   - gnutls does not use gcrypt
 *
 *   or
 *
 *   - gnutls < 2.12
 */
#ifndef GNUTLS_VERSION_NUMBER
#ifndef LIBGNUTLS_VERSION_NUMBER
#error "GNUTLS >= 2.2.0 required to build GTK-VNC"
#else
#define GNUTLS_VERSION_NUMBER LIBGNUTLS_VERSION_NUMBER
#endif
#endif

#if ((!defined(HAVE_GNUTLS_GCRYPT) ||            \
      (GNUTLS_VERSION_NUMBER < 0x020c00)) &&     \
     (!defined(GCRYPT_VERSION_NUMBER) ||         \
      (GCRYPT_VERSION_NUMBER < 0x010600)))
#define VNC_INIT_GCRYPT_THREADING
#else
#undef VNC_INIT_GCRYPT_THREADING
#endif


struct wait_queue
{
    gboolean waiting;
    struct coroutine *context;
};

typedef enum {
    VNC_CONNECTION_SERVER_MESSAGE_FRAMEBUFFER_UPDATE = 0,
    VNC_CONNECTION_SERVER_MESSAGE_SET_COLOR_MAP_ENTRIES = 1,
    VNC_CONNECTION_SERVER_MESSAGE_BELL = 2,
    VNC_CONNECTION_SERVER_MESSAGE_SERVER_CUT_TEXT = 3,
    VNC_CONNECTION_SERVER_MESSAGE_QEMU = 255,
} VncConnectionServerMessage;

typedef enum {
    VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO = 1,
} VncConnectionServerMessageQEMU;

typedef enum {
    VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO_STOP = 0,
    VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO_START = 1,
    VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO_DATA = 2,
} VncConnectionServerMessageQEMUAudio;


typedef enum {
    VNC_CONNECTION_CLIENT_MESSAGE_SET_PIXEL_FORMAT = 0,
    VNC_CONNECTION_CLIENT_MESSAGE_SET_ENCODINGS = 2,
    VNC_CONNECTION_CLIENT_MESSAGE_FRAMEBUFFER_UPDATE_REQUEST = 3,
    VNC_CONNECTION_CLIENT_MESSAGE_KEY = 4,
    VNC_CONNECTION_CLIENT_MESSAGE_POINTER = 5,
    VNC_CONNECTION_CLIENT_MESSAGE_CUT_TEXT = 6,
    VNC_CONNECTION_CLIENT_MESSAGE_QEMU = 255,
} VncConnectionClientMessage;

typedef enum {
    VNC_CONNECTION_CLIENT_MESSAGE_QEMU_KEY = 0,
    VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO = 1,
} VncConnectionClientMessageQEMU;

typedef enum {
    VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO_ENABLE = 0,
    VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO_DISABLE = 1,
    VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO_SET_FORMAT = 2,
} VncConnectionClientMessageQEMUAudio;


typedef void vnc_connection_rich_cursor_blt_func(VncConnection *conn, guint8 *, guint8 *,
                                                 guint8 *, int, guint16, guint16);

typedef void vnc_connection_tight_compute_predicted_func(VncConnection *conn, guint8 *,
                                                         guint8 *, guint8 *,
                                                         guint8 *);

typedef void vnc_connection_tight_sum_pixel_func(VncConnection *conn, guint8 *, guint8 *);
static void vnc_connection_close(VncConnection *conn);
static void vnc_connection_set_error(VncConnection *conn,
                                     const char *format,
                                     ...) G_GNUC_PRINTF(2, 3);

/*
 * A special GSource impl which allows us to wait on a certain
 * condition to be satisfied. This is effectively a boolean test
 * run on each iteration of the main loop. So whenever a file has
 * new I/O, or a timer occurs, etc we'll do the check. This is
 * pretty efficient compared to a normal GLib Idle func which has
 * to busy wait on a timeout, since our condition is only checked
 * when some other source's state changes
 */
typedef gboolean (*g_condition_wait_func)(gpointer);

struct g_condition_wait_source
{
    GSource src;
    struct coroutine *co;
    g_condition_wait_func func;
    gpointer data;
};

#define VNC_CONNECTION_GET_PRIVATE(obj)                                 \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_CONNECTION, VncConnectionPrivate))


struct _VncConnectionPrivate
{
    struct coroutine coroutine;
    guint open_id;
    GSocket *sock;
    GSocketAddress *addr;
    int fd;
    char *host;
    char *port;
    VncPixelFormat fmt;
    char *error;
    gboolean coroutine_stop;
    int width;
    int height;
    char *name;

    int major;
    int minor;
    gnutls_session_t tls_session;

    /* Auth related params */
    unsigned int auth_type;
    unsigned int auth_subtype;
    char *cred_username;
    char *cred_password;
    char *cred_x509_cacert;
    char *cred_x509_cacrl;
    char *cred_x509_cert;
    char *cred_x509_key;
    gboolean want_cred_username;
    gboolean want_cred_password;
    gboolean want_cred_x509;

#ifdef HAVE_SASL
    sasl_conn_t *saslconn;      /* SASL context */
    const char *saslDecoded;
    unsigned int saslDecodedLength;
    unsigned int saslDecodedOffset;
#endif

    char read_buffer[4096];
    size_t read_offset;
    size_t read_size;

    char write_buffer[4096];
    size_t write_offset;

    VncFramebuffer *fb;
    gboolean fbSwapRemote;

    VncCursor *cursor;
    gboolean absPointer;
    gboolean sharedFlag;

    vnc_connection_rich_cursor_blt_func *rich_cursor_blt;
    vnc_connection_tight_compute_predicted_func *tight_compute_predicted;
    vnc_connection_tight_sum_pixel_func *tight_sum_pixel;

    int wait_interruptable;
    struct wait_queue wait;

    char *xmit_buffer;
    int xmit_buffer_capacity;
    int xmit_buffer_size;

    z_stream *strm;
    z_stream streams[5];

    size_t uncompressed_offset;
    size_t uncompressed_size;
    guint8 uncompressed_buffer[4096];

    size_t compressed_length;
    guint8 *compressed_buffer;

    guint8 zrle_pi;
    int zrle_pi_bits;

    int ledstate;
    gboolean has_ext_key_event;

    struct {
        gboolean incremental;
        guint16 x;
        guint16 y;
        guint16 width;
        guint16 height;
    } lastUpdateRequest;

    gboolean has_audio;
    gboolean audio_format_pending;
    gboolean audio_enable_pending;
    gboolean audio_disable_pending;
    VncAudioFormat audio_format;
    VncAudio *audio;
    VncAudioSample *audio_sample;
    guint audio_timer;
};

G_DEFINE_TYPE(VncConnection, vnc_connection, G_TYPE_OBJECT);


enum {
    VNC_CURSOR_CHANGED,
    VNC_POINTER_MODE_CHANGED,
    VNC_BELL,
    VNC_SERVER_CUT_TEXT,
    VNC_FRAMEBUFFER_UPDATE,
    VNC_DESKTOP_RESIZE,
    VNC_PIXEL_FORMAT_CHANGED,
    VNC_LED_STATE,

    VNC_AUTH_FAILURE,
    VNC_AUTH_UNSUPPORTED,
    VNC_AUTH_CREDENTIAL,
    VNC_AUTH_CHOOSE_TYPE,
    VNC_AUTH_CHOOSE_SUBTYPE,

    VNC_CONNECTED,
    VNC_INITIALIZED,
    VNC_DISCONNECTED,
    VNC_ERROR,

    VNC_LAST_SIGNAL,
};

static guint signals[VNC_LAST_SIGNAL] = { 0, 0, 0, 0,
                                          0, 0, 0, 0,
                                          0, 0, 0, 0,
                                          0, 0, 0 };

#define nibhi(a) (((a) >> 4) & 0x0F)
#define niblo(a) ((a) & 0x0F)


/* Main loop helper functions */
static gboolean g_io_wait_helper(GSocket *sock G_GNUC_UNUSED,
                                 GIOCondition cond,
                                 gpointer data)
{
    struct coroutine *to = data;
    coroutine_yieldto(to, &cond);
    return FALSE;
}

static GIOCondition g_io_wait(GSocket *sock, GIOCondition cond)
{
    GIOCondition *ret;
    GSource *src = g_socket_create_source(sock,
                                          cond | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                          NULL);
    g_source_set_callback(src, (GSourceFunc)g_io_wait_helper, coroutine_self(), NULL);
    g_source_attach(src, NULL);
    ret = coroutine_yield(NULL);
    g_source_unref(src);
    return *ret;
}


static GIOCondition g_io_wait_interruptable(struct wait_queue *wait,
                                            GSocket *sock,
                                            GIOCondition cond)
{
    GIOCondition *ret;
    gint id;

    wait->context = coroutine_self();
    GSource *src = g_socket_create_source(sock,
                                          cond | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                          NULL);
    g_source_set_callback(src, (GSourceFunc)g_io_wait_helper,
                          wait->context, NULL);
    id = g_source_attach(src, NULL);
    wait->waiting = TRUE;
    ret = coroutine_yield(NULL);
    g_source_unref(src);
    wait->waiting = FALSE;

    if (ret == NULL) {
        g_source_remove(id);
        return 0;
    } else
        return *ret;
}

static void g_io_wakeup(struct wait_queue *wait)
{
    if (wait->waiting)
        coroutine_yieldto(wait->context, NULL);
}


/*
 * Call immediately before the main loop does an iteration. Returns
 * true if the condition we're checking is ready for dispatch
 */
static gboolean g_condition_wait_prepare(GSource *src,
                                         int *timeout) {
    struct g_condition_wait_source *vsrc = (struct g_condition_wait_source *)src;
    *timeout = -1;
    return vsrc->func(vsrc->data);
}

/*
 * Call immediately after the main loop does an iteration. Returns
 * true if the condition we're checking is ready for dispatch
 */
static gboolean g_condition_wait_check(GSource *src)
{
    struct g_condition_wait_source *vsrc = (struct g_condition_wait_source *)src;
    return vsrc->func(vsrc->data);
}

static gboolean g_condition_wait_dispatch(GSource *src G_GNUC_UNUSED,
                                          GSourceFunc cb,
                                          gpointer data) {
    return cb(data);
}

GSourceFuncs waitFuncs = {
    .prepare = g_condition_wait_prepare,
    .check = g_condition_wait_check,
    .dispatch = g_condition_wait_dispatch,
};

static gboolean g_condition_wait_helper(gpointer data)
{
    struct coroutine *co = (struct coroutine *)data;
    coroutine_yieldto(co, NULL);
    return FALSE;
}

static gboolean g_condition_wait(g_condition_wait_func func, gpointer data)
{
    GSource *src;
    struct g_condition_wait_source *vsrc;

    /* Short-circuit check in case we've got it ahead of time */
    if (func(data)) {
        return TRUE;
    }

    /*
     * Don't have it, so yield to the main loop, checking the condition
     * on each iteration of the main loop
     */
    src = g_source_new(&waitFuncs, sizeof(struct g_condition_wait_source));
    vsrc = (struct g_condition_wait_source *)src;

    vsrc->func = func;
    vsrc->data = data;
    vsrc->co = coroutine_self();

    g_source_attach(src, NULL);
    g_source_set_callback(src, g_condition_wait_helper, coroutine_self(), NULL);
    coroutine_yield(NULL);
    g_source_unref(src);

    return TRUE;
}


enum {
    PROP_0,
    PROP_FRAMEBUFFER,
};


static void vnc_connection_get_property(GObject *object,
                                        guint prop_id,
                                        GValue *value,
                                        GParamSpec *pspec)
{
    VncConnection *conn = VNC_CONNECTION(object);
    VncConnectionPrivate *priv = conn->priv;

    switch (prop_id) {
    case PROP_FRAMEBUFFER:
        g_value_set_object(value, priv->fb);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void vnc_connection_set_property(GObject *object,
                                        guint prop_id,
                                        const GValue *value,
                                        GParamSpec *pspec)
{
    VncConnection *conn = VNC_CONNECTION(object);

    switch (prop_id) {
    case PROP_FRAMEBUFFER:
        vnc_connection_set_framebuffer(conn, g_value_get_object(value));
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

struct signal_data
{
    VncConnection *conn;
    struct coroutine *caller;

    int signum;

    union {
        VncCursor *cursor;
        gboolean absPointer;
        const char *text;
        int ledstate;
        struct {
            int x;
            int y;
            int width;
            int height;
        } area;
        struct {
            int width;
            int height;
        } size;
        VncPixelFormat *pixelFormat;
        const char *authReason;
        unsigned int authUnsupported;
        GValueArray *authCred;
        GValueArray *authTypes;
        const char *message;
    } params;
};

static gboolean do_vnc_connection_emit_main_context(gpointer opaque)
{
    struct signal_data *data = opaque;

    VNC_DEBUG("Emit main context %d", data->signum);
    switch (data->signum) {
    case VNC_CURSOR_CHANGED:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.cursor);
        break;

    case VNC_POINTER_MODE_CHANGED:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.absPointer);
        break;

    case VNC_BELL:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0);
        break;

    case VNC_SERVER_CUT_TEXT:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.text);
        break;

    case VNC_FRAMEBUFFER_UPDATE:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.area.x,
                      data->params.area.y,
                      data->params.area.width,
                      data->params.area.height);
        break;

    case VNC_DESKTOP_RESIZE:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.size.width,
                      data->params.size.height);
        break;

    case VNC_PIXEL_FORMAT_CHANGED:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.pixelFormat);
        break;

    case VNC_LED_STATE:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.ledstate);
        break;

    case VNC_AUTH_FAILURE:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.authReason);
        break;

    case VNC_AUTH_UNSUPPORTED:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.authUnsupported);
        break;

    case VNC_AUTH_CREDENTIAL:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.authCred);
        break;

    case VNC_AUTH_CHOOSE_TYPE:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.authTypes);
        break;

    case VNC_AUTH_CHOOSE_SUBTYPE:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->conn->priv->auth_type,
                      data->params.authTypes);
        break;

    case VNC_CONNECTED:
    case VNC_INITIALIZED:
    case VNC_DISCONNECTED:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0);
        break;

    case VNC_ERROR:
        g_signal_emit(G_OBJECT(data->conn),
                      signals[data->signum],
                      0,
                      data->params.message);
        break;

    default:
        g_warn_if_reached();
    }

    coroutine_yieldto(data->caller, NULL);

    return FALSE;
}

static void vnc_connection_emit_main_context(VncConnection *conn,
                                             int signum,
                                             struct signal_data *data)
{
    data->conn = conn;
    data->caller = coroutine_self();
    data->signum = signum;

    g_idle_add(do_vnc_connection_emit_main_context, data);

    /* This switches to the system coroutine context, lets
     * the idle function run to dispatch the signal, and
     * finally returns once complete. ie this is synchronous
     * from the POV of the VNC coroutine despite there being
     * an idle function involved
     */
    coroutine_yield(NULL);
}


static G_GNUC_PRINTF(2, 3) void vnc_connection_set_error(VncConnection *conn,
                                                         const char *format,
                                                         ...)
{
    va_list args;
    struct signal_data s;

    va_start(args, format);

    g_free(conn->priv->error);
    conn->priv->error = g_strdup_vprintf(format, args);
    va_end(args);
    conn->priv->coroutine_stop = TRUE;

    VNC_DEBUG("Error: %s", conn->priv->error);

    s.params.message = conn->priv->error;
    vnc_connection_emit_main_context(conn, VNC_ERROR, &s);
}


static gboolean vnc_connection_use_compression(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->compressed_buffer != NULL;
}

static int vnc_connection_zread(VncConnection *conn, void *buffer, size_t size)
{
    VncConnectionPrivate *priv = conn->priv;
    char *ptr = buffer;
    size_t offset = 0;

    while (offset < size) {
        /* if data is available in the uncompressed buffer, then
         * copy */
        if (priv->uncompressed_size - priv->uncompressed_offset) {
            size_t len = MIN(priv->uncompressed_size - priv->uncompressed_offset,
                             size - offset);

            memcpy(ptr + offset,
                   priv->uncompressed_buffer + priv->uncompressed_offset,
                   len);

            priv->uncompressed_offset += len;
            offset += len;
        } else {
            int err;

            priv->strm->next_in = priv->compressed_buffer;
            priv->strm->avail_in = priv->compressed_length;
            priv->strm->next_out = priv->uncompressed_buffer;
            priv->strm->avail_out = sizeof(priv->uncompressed_buffer);

            /* inflate as much as possible */
            err = inflate(priv->strm, Z_SYNC_FLUSH);
            if (err != Z_OK) {
                errno = EIO;
                return -1;
            }

            priv->uncompressed_offset = 0;
            priv->uncompressed_size = (guint8 *)priv->strm->next_out - priv->uncompressed_buffer;
            priv->compressed_length -= (guint8 *)priv->strm->next_in - priv->compressed_buffer;
            priv->compressed_buffer = priv->strm->next_in;
        }
    }

    return offset;
}

/* IO functions */


/*
 * Read at least 1 more byte of data straight off the wire
 * into the requested buffer.
 */
static int vnc_connection_read_wire(VncConnection *conn, void *data, size_t len)
{
    VncConnectionPrivate *priv = conn->priv;
    int ret;
    gboolean blocking = FALSE;

 reread:

    if (priv->coroutine_stop) return -EINVAL;

    if (priv->tls_session) {
        ret = gnutls_read(priv->tls_session, data, len);
        if (ret < 0) {
            if (ret == GNUTLS_E_AGAIN)
                blocking = TRUE;
            ret = -1;
        }
    } else {
        GError *error = NULL;
        ret = g_socket_receive(priv->sock,
                               data, len,
                               NULL, &error);
        if (ret < 0) {
            if (error) {
                VNC_DEBUG("Read error %s", error->message);
                if (error->code == G_IO_ERROR_WOULD_BLOCK)
                    blocking = TRUE;
                g_error_free(error);
            } else {
                VNC_DEBUG("Read error unknown");
            }
            ret = -1;
        }
    }

    if (ret == -1) {
        if (blocking) {
            if (priv->wait_interruptable) {
                if (!g_io_wait_interruptable(&priv->wait,
                                             priv->sock, G_IO_IN)) {
                    return -EAGAIN;
                }
            } else {
                g_io_wait(priv->sock, G_IO_IN);
            }
            blocking = FALSE;
            goto reread;
        } else {
            vnc_connection_set_error(conn, "%s", "Unable to read from server");
            return -errno;
        }
    }
    if (ret == 0) {
        VNC_DEBUG("Closing the connection: vnc_connection_read() - ret=0");
        vnc_connection_set_error(conn, "%s", "Server closed the connection");
        return -EPIPE;
    }
    //VNC_DEBUG("Read wire %p %d -> %d", data, len, ret);

    return ret;
}


#ifdef HAVE_SASL
/*
 * Read at least 1 more byte of data out of the SASL decrypted
 * data buffer, into the internal read buffer
 */
static int vnc_connection_read_sasl(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    size_t want;

    //VNC_DEBUG("Read SASL %p size %d offset %d", priv->saslDecoded,
    //           priv->saslDecodedLength, priv->saslDecodedOffset);
    if (priv->saslDecoded == NULL) {
        char *encoded;
        int encodedLen;
        int err, ret;

        encodedLen = 8192;
        encoded = g_new0(char, encodedLen);

        ret = vnc_connection_read_wire(conn, encoded, encodedLen);
        if (ret < 0) {
            g_free(encoded);
            return ret;
        }

        err = sasl_decode(priv->saslconn, encoded, ret,
                          &priv->saslDecoded, &priv->saslDecodedLength);
        g_free(encoded);
        if (err != SASL_OK) {
            vnc_connection_set_error(conn,
                                     "Failed to decode SASL data %s",
                                     sasl_errstring(err, NULL, NULL));
            return -EINVAL;
        }
        priv->saslDecodedOffset = 0;
    }

    want = priv->saslDecodedLength - priv->saslDecodedOffset;
    if (want > sizeof(priv->read_buffer))
        want = sizeof(priv->read_buffer);

    memcpy(priv->read_buffer,
           priv->saslDecoded + priv->saslDecodedOffset,
           want);
    priv->saslDecodedOffset += want;
    if (priv->saslDecodedOffset == priv->saslDecodedLength) {
        priv->saslDecodedLength = priv->saslDecodedOffset = 0;
        priv->saslDecoded = NULL;
    }

    return want;
}
#endif


/*
 * Read at least 1 more byte of data straight off the wire
 * into the internal read buffer
 */
static int vnc_connection_read_plain(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    //VNC_DEBUG("Read plain %d", sizeof(priv->read_buffer));
    return vnc_connection_read_wire(conn, priv->read_buffer, sizeof(priv->read_buffer));
}

/*
 * Read at least 1 more byte of data into the internal read_buffer
 */
static int vnc_connection_read_buf(VncConnection *conn)
{
#ifdef HAVE_SASL
    VncConnectionPrivate *priv = conn->priv;

    if (priv->saslconn)
        return vnc_connection_read_sasl(conn);
    else
#endif
        return vnc_connection_read_plain(conn);
}

/*
 * Fill the 'data' buffer up with exactly 'len' bytes worth of data
 *
 * Must only be called from the VNC coroutine
 */
static int vnc_connection_read(VncConnection *conn, void *data, size_t len)
{
    VncConnectionPrivate *priv = conn->priv;
    char *ptr = data;
    size_t offset = 0;

    if (priv->coroutine_stop) return -EINVAL;

    while (offset < len) {
        size_t tmp;

        /* compressed data is buffered independently of the read buffer
         * so we must by-pass it */
        if (vnc_connection_use_compression(conn)) {
            int ret = vnc_connection_zread(conn, ptr + offset, len);
            if (ret == -1) {
                vnc_connection_set_error(conn, "%s", "Failure decompressing data");
                return -errno;
            }
            offset += ret;
            continue;
        } else if (priv->read_offset == priv->read_size) {
            int ret = vnc_connection_read_buf(conn);

            if (ret < 0)
                return ret;
            priv->read_offset = 0;
            priv->read_size = ret;
        }

        tmp = MIN(priv->read_size - priv->read_offset, len - offset);

        memcpy(ptr + offset, priv->read_buffer + priv->read_offset, tmp);

        priv->read_offset += tmp;
        offset += tmp;
    }

    return 0;
}

/*
 * Write all 'data' of length 'datalen' bytes out to
 * the wire
 */
static void vnc_connection_flush_wire(VncConnection *conn,
                                      const void *data,
                                      size_t datalen)
{
    VncConnectionPrivate *priv = conn->priv;
    const char *ptr = data;
    size_t offset = 0;
    //VNC_DEBUG("Flush write %p %d", data, datalen);
    while (offset < datalen) {
        int ret;
        gboolean blocking = FALSE;

        if (priv->coroutine_stop) return;

        if (priv->tls_session) {
            ret = gnutls_write(priv->tls_session,
                               ptr+offset,
                               datalen-offset);
            if (ret < 0) {
                if (ret == GNUTLS_E_AGAIN)
                    blocking = TRUE;
                ret = -1;
            }
        } else {
            GError *error = NULL;
            ret = g_socket_send(priv->sock,
                                ptr+offset,
                                datalen-offset,
                                NULL, &error);
            if (ret < 0) {
                if (error) {
                    if (error->code == G_IO_ERROR_WOULD_BLOCK)
                        blocking = TRUE;
                    g_error_free(error);
                }
                ret = -1;
            }
        }
        if (ret == -1) {
            if (blocking) {
                g_io_wait(priv->sock, G_IO_OUT);
            } else {
                vnc_connection_set_error(conn, "%s", "Failed to flush data");
                return;
            }
        }
        if (ret == 0) {
            vnc_connection_set_error(conn, "%s", "Failed to any flush data");
            return;
        }
        offset += ret;
    }
}


#ifdef HAVE_SASL
/*
 * Encode all buffered data, write all encrypted data out
 * to the wire
 */
static void vnc_connection_flush_sasl(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    const char *output;
    unsigned int outputlen;
    int err;

    err = sasl_encode(priv->saslconn,
                      priv->write_buffer,
                      priv->write_offset,
                      &output, &outputlen);
    if (err != SASL_OK) {
        vnc_connection_set_error(conn, "Failed to encode SASL data %s",
                                 sasl_errstring(err, NULL, NULL));
        return;
    }
    //VNC_DEBUG("Flush SASL %d: %p %d", priv->write_offset, output, outputlen);
    vnc_connection_flush_wire(conn, output, outputlen);
}
#endif

/*
 * Write all buffered data straight out to the wire
 */
static void vnc_connection_flush_plain(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    //VNC_DEBUG("Flush plain %d", priv->write_offset);
    vnc_connection_flush_wire(conn,
                              priv->write_buffer,
                              priv->write_offset);
}


/*
 * Write all buffered data out to the wire
 */
static void vnc_connection_flush(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

#ifdef HAVE_SASL
    if (priv->saslconn)
        vnc_connection_flush_sasl(conn);
    else
#endif
        vnc_connection_flush_plain(conn);
    priv->write_offset = 0;
}


/*
 * Must only be called from the VNC coroutine
 */
static void vnc_connection_write(VncConnection *conn, const void *data, size_t len)
{
    VncConnectionPrivate *priv = conn->priv;
    const char *ptr = data;
    size_t offset = 0;

    while (offset < len) {
        ssize_t tmp;

        if (priv->write_offset == sizeof(priv->write_buffer)) {
            vnc_connection_flush(conn);
        }

        tmp = MIN(sizeof(priv->write_buffer) - priv->write_offset,
                  len - offset);

        memcpy(priv->write_buffer+priv->write_offset, ptr + offset, tmp);

        priv->write_offset += tmp;
        offset += tmp;
    }
}


static ssize_t vnc_connection_tls_push(gnutls_transport_ptr_t transport,
                                       const void *data,
                                       size_t len) {
    VncConnection *conn = transport;
    VncConnectionPrivate *priv = conn->priv;
    int ret;
    GError *error = NULL;

    if (!priv->sock) {
        VNC_DEBUG("Unexpected TLS push on closed socket");
        errno = EBADF;
        return -1;
    }

    ret = g_socket_send(priv->sock, data, len, NULL, &error);
    if (ret < 0) {
        if (error) {
            if (error->code == G_IO_ERROR_WOULD_BLOCK)
                errno = EAGAIN; /* For gnutls compat */
            else
                VNC_DEBUG("Read error %s", error->message);
            g_error_free(error);
        }
        return -1;
    }
    return ret;
}


static ssize_t vnc_connection_tls_pull(gnutls_transport_ptr_t transport,
                                       void *data,
                                       size_t len) {
    VncConnection *conn = transport;
    VncConnectionPrivate *priv = conn->priv;
    int ret;
    GError *error = NULL;

    if (!priv->sock) {
        VNC_DEBUG("Unexpected TLS pull on closed socket");
        errno = EBADF;
        return -1;
    }

    ret = g_socket_receive(priv->sock, data, len, NULL, &error);
    if (ret < 0) {
        if (error) {
            if (error->code == G_IO_ERROR_WOULD_BLOCK)
                errno = EAGAIN; /* For gnutls compat */
            else
                VNC_DEBUG("Read error %s", error->message);
            g_error_free(error);
        }
        return -1;
    }
    return ret;
}

static size_t vnc_connection_pixel_size(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->fmt.bits_per_pixel / 8;
}

/*
 * Must only be called from the VNC coroutine
 */
static void vnc_connection_read_pixel(VncConnection *conn, guint8 *pixel)
{
    vnc_connection_read(conn, pixel, vnc_connection_pixel_size(conn));
}

/*
 * Must only be called from the VNC coroutine
 */
static guint8 vnc_connection_read_u8(VncConnection *conn)
{
    guint8 value = 0;
    vnc_connection_read(conn, &value, sizeof(value));
    return value;
}

/*
 * Must only be called from the VNC coroutine
 */
static int vnc_connection_read_u8_interruptable(VncConnection *conn, guint8 *value)
{
    VncConnectionPrivate *priv = conn->priv;
    int ret;

    priv->wait_interruptable = 1;
    ret = vnc_connection_read(conn, value, sizeof(*value));
    priv->wait_interruptable = 0;

    return ret;
}

/*
 * Must only be called from the VNC coroutine
 */
static guint16 vnc_connection_read_u16(VncConnection *conn)
{
    guint16 value = 0;
    vnc_connection_read(conn, &value, sizeof(value));
    return g_ntohs(value);
}

/*
 * Must only be called from the VNC coroutine
 */
static guint32 vnc_connection_read_u32(VncConnection *conn)
{
    guint32 value = 0;
    vnc_connection_read(conn, &value, sizeof(value));
    return g_ntohl(value);
}

/*
 * Must only be called from the VNC coroutine
 */
static gint32 vnc_connection_read_s32(VncConnection *conn)
{
    gint32 value = 0;
    vnc_connection_read(conn, &value, sizeof(value));
    return g_ntohl(value);
}

/*
 * Must only be called from the VNC coroutine
 */
static void vnc_connection_write_u8(VncConnection *conn, guint8 value)
{
    vnc_connection_write(conn, &value, sizeof(value));
}

/*
 * Must only be called from the VNC coroutine
 */
static void vnc_connection_write_u16(VncConnection *conn, guint16 value)
{
    value = g_htons(value);
    vnc_connection_write(conn, &value, sizeof(value));
}

/*
 * Must only be called from the VNC coroutine
 */
static void vnc_connection_write_u32(VncConnection *conn, guint32 value)
{
    value = g_htonl(value);
    vnc_connection_write(conn, &value, sizeof(value));
}

#define DH_BITS 1024
static gnutls_dh_params_t dh_params;

#if 0
static void vnc_connection_debug_gnutls_log(int level, const char* str) {
    VNC_DEBUG("%d %s", level, str);
}
#endif

#ifdef VNC_INIT_GCRYPT_THREADING
static int gvnc_tls_mutex_init (void **priv)
{                                                                             \
    GMutex *lock = NULL;
    lock = g_mutex_new();
    *priv = lock;
    return 0;
}

static int gvnc_tls_mutex_destroy(void **priv)
{
    GMutex *lock = *priv;
    g_mutex_free(lock);
    return 0;
}

static int gvnc_tls_mutex_lock(void **priv)
{
    GMutex *lock = *priv;
    g_mutex_lock(lock);
    return 0;
}

static int gvnc_tls_mutex_unlock(void **priv)
{
    GMutex *lock = *priv;
    g_mutex_unlock(lock);
    return 0;
}

static struct gcry_thread_cbs gvnc_thread_impl = {
    (GCRY_THREAD_OPTION_PTHREAD | (GCRY_THREAD_OPTION_VERSION << 8)),
    NULL,
    gvnc_tls_mutex_init,
    gvnc_tls_mutex_destroy,
    gvnc_tls_mutex_lock,
    gvnc_tls_mutex_unlock,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
#endif /* VNC_INIT_GCRYPT_THREADING */


static gboolean vnc_connection_tls_initialize(void)
{
    static int tlsinitialized = 0;

    if (tlsinitialized)
        return TRUE;

    if (g_thread_supported()) {
#ifdef VNC_INIT_GCRYPT_THREADING
        gcry_control(GCRYCTL_SET_THREAD_CBS, &gvnc_thread_impl);
#endif /* VNC_INIT_GCRYPT_THREADING */
        gcry_check_version(NULL);
    }

    if (gnutls_global_init () < 0)
        return FALSE;

    if (gnutls_dh_params_init (&dh_params) < 0)
        return FALSE;
    if (gnutls_dh_params_generate2 (dh_params, DH_BITS) < 0)
        return FALSE;

#if 0
    if (debug_enabled) {
        gnutls_global_set_log_level(10);
        gnutls_global_set_log_function(vnc_connection_debug_gnutls_log);
    }
#endif

    tlsinitialized = TRUE;

    return TRUE;
}

static gnutls_anon_client_credentials vnc_connection_tls_initialize_anon_cred(void)
{
    gnutls_anon_client_credentials anon_cred;
    int ret;

    if ((ret = gnutls_anon_allocate_client_credentials(&anon_cred)) < 0) {
        VNC_DEBUG("Cannot allocate credentials %s", gnutls_strerror(ret));
        return NULL;
    }

    return anon_cred;
}

static gnutls_certificate_credentials_t vnc_connection_tls_initialize_cert_cred(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    gnutls_certificate_credentials_t x509_cred;
    int ret;

    if ((ret = gnutls_certificate_allocate_credentials(&x509_cred)) < 0) {
        VNC_DEBUG("Cannot allocate credentials %s", gnutls_strerror(ret));
        return NULL;
    }
    if (priv->cred_x509_cacert) {
        if ((ret = gnutls_certificate_set_x509_trust_file(x509_cred,
                                                          priv->cred_x509_cacert,
                                                          GNUTLS_X509_FMT_PEM)) < 0) {
            VNC_DEBUG("Cannot load CA certificate %s", gnutls_strerror(ret));
            return NULL;
        }
    } else {
#if GNUTLS_VERSION_NUMBER >= 0x030000
        VNC_DEBUG("No CA certificate provided; trying the system trust store instead");

        if ((ret = gnutls_certificate_set_x509_system_trust(x509_cred)) < 0) {
            VNC_DEBUG("Cannot load system trust: %s", gnutls_strerror(ret));
            return NULL;
        }

        VNC_DEBUG("Using the system trust store and CRL");
#else
        VNC_DEBUG("No CA certificate provided and system trust not supported");
        return NULL;
#endif
    }

    if (priv->cred_x509_cert && priv->cred_x509_key) {
        if ((ret = gnutls_certificate_set_x509_key_file (x509_cred,
                                                         priv->cred_x509_cert,
                                                         priv->cred_x509_key,
                                                         GNUTLS_X509_FMT_PEM)) < 0) {
            VNC_DEBUG("Cannot load certificate & key %s", gnutls_strerror(ret));
            return NULL;
        }
    } else {
        VNC_DEBUG("No client cert or key provided");
    }

    if (priv->cred_x509_cacrl) {
        if ((ret = gnutls_certificate_set_x509_crl_file(x509_cred,
                                                        priv->cred_x509_cacrl,
                                                        GNUTLS_X509_FMT_PEM)) < 0) {
            VNC_DEBUG("Cannot load CRL %s", gnutls_strerror(ret));
            return NULL;
        }
    } else {
        VNC_DEBUG("No CA revocation list provided");
    }

    gnutls_certificate_set_dh_params (x509_cred, dh_params);

    return x509_cred;
}

static int vnc_connection_validate_certificate(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    int ret;
    unsigned int status;
    const gnutls_datum_t *certs;
    unsigned int nCerts, i;
    time_t now;

    VNC_DEBUG("Validating");
    if ((ret = gnutls_certificate_verify_peers2 (priv->tls_session, &status)) < 0) {
        vnc_connection_set_error(conn, "Failed to verify peer %s", gnutls_strerror(ret));
        return FALSE;
    }

    if ((now = time(NULL)) == ((time_t)-1)) {
        vnc_connection_set_error(conn, "%s", "Failed to get current time");
        return FALSE;
    }

    if (status != 0) {
        if (status & GNUTLS_CERT_INVALID) {
            vnc_connection_set_error(conn, "%s", "The certificate is not trusted");
        } else if (status & GNUTLS_CERT_SIGNER_NOT_FOUND) {
            vnc_connection_set_error(conn, "%s", "The certificate hasn't got a known issuer");
        } else if (status & GNUTLS_CERT_REVOKED) {
            vnc_connection_set_error(conn, "%s", "The certificate has been revoked");
        } else if (status & GNUTLS_CERT_INSECURE_ALGORITHM) {
            vnc_connection_set_error(conn, "%s", "The certificate uses an insecure algorithm");
        } else {
            vnc_connection_set_error(conn, "%s", "The certificate is not valid");
        }
        return FALSE;
    } else {
        VNC_DEBUG("Certificate is valid.");
    }

    if (gnutls_certificate_type_get(priv->tls_session) != GNUTLS_CRT_X509) {
        vnc_connection_set_error(conn, "%s", "Only x509 certificates are supported");
        return FALSE;
    }

    if (!(certs = gnutls_certificate_get_peers(priv->tls_session, &nCerts))) {
        vnc_connection_set_error(conn, "%s", "Unable to query certificate peers");
        return FALSE;
    }

    for (i = 0 ; i < nCerts ; i++) {
        gnutls_x509_crt_t cert;
        VNC_DEBUG ("Checking chain %u", i);
        if (gnutls_x509_crt_init (&cert) < 0) {
            vnc_connection_set_error(conn, "%s", "Unable to initialize cert");
            return FALSE;
        }

        if (gnutls_x509_crt_import(cert, &certs[i], GNUTLS_X509_FMT_DER) < 0) {
            gnutls_x509_crt_deinit (cert);
            vnc_connection_set_error(conn, "%s", "Unable to import certificate");
            return FALSE;
        }

        if (gnutls_x509_crt_get_expiration_time (cert) < now) {
            vnc_connection_set_error(conn, "%s", "The certificate has expired");
            gnutls_x509_crt_deinit (cert);
            return FALSE;
        }

        if (gnutls_x509_crt_get_activation_time (cert) > now) {
            vnc_connection_set_error(conn, "%s", "The certificate is not yet activated");
            gnutls_x509_crt_deinit (cert);
            return FALSE;
        }

        if (i == 0) {
            if (!priv->host) {
                vnc_connection_set_error(conn, "%s", "No hostname provided for certificate verification");
                gnutls_x509_crt_deinit (cert);
                return FALSE;
            }
            if (!gnutls_x509_crt_check_hostname (cert, priv->host)) {
                vnc_connection_set_error(conn,
                                         "The certificate's owner does not match hostname '%s'",
                                         priv->host);
                gnutls_x509_crt_deinit (cert);
                return FALSE;
            }
        }
    }

    return TRUE;
}


static void vnc_connection_read_pixel_format(VncConnection *conn, VncPixelFormat *fmt)
{
    guint8 pad[3];

    fmt->bits_per_pixel  = vnc_connection_read_u8(conn);
    fmt->depth           = vnc_connection_read_u8(conn);
    fmt->byte_order      = vnc_connection_read_u8(conn) ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
    fmt->true_color_flag = vnc_connection_read_u8(conn);

    fmt->red_max         = vnc_connection_read_u16(conn);
    fmt->green_max       = vnc_connection_read_u16(conn);
    fmt->blue_max        = vnc_connection_read_u16(conn);

    fmt->red_shift       = vnc_connection_read_u8(conn);
    fmt->green_shift     = vnc_connection_read_u8(conn);
    fmt->blue_shift      = vnc_connection_read_u8(conn);

    vnc_connection_read(conn, pad, 3);

    VNC_DEBUG("Pixel format BPP: %d,  Depth: %d, Byte order: %d, True color: %d\n"
              "             Mask  red: %3d, green: %3d, blue: %3d\n"
              "             Shift red: %3d, green: %3d, blue: %3d",
              fmt->bits_per_pixel, fmt->depth, fmt->byte_order, fmt->true_color_flag,
              fmt->red_max, fmt->green_max, fmt->blue_max,
              fmt->red_shift, fmt->green_shift, fmt->blue_shift);
}

static void vnc_connection_ledstate_change(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    priv->ledstate = vnc_connection_read_u8(conn);

    VNC_DEBUG("LED state: %d\n", priv->ledstate);

    sigdata.params.ledstate = priv->ledstate;
    vnc_connection_emit_main_context(conn, VNC_LED_STATE, &sigdata);
}

/* initialize function */

/**
 * vnc_connection_has_error:
 * @conn: (transfer none): the connection object
 *
 * Determine if the current connection is in an error
 * state
 *
 * Returns: TRUE if an error has occurred, FALSE otherwise
 */
gboolean vnc_connection_has_error(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->coroutine_stop;
}

/**
 * vnc_connection_get_pixel_format:
 * @conn: (transfer none): the connection object
 *
 * Get a specification of the current pixel format
 *
 * Returns: (transfer none): the current pixel format
 */
const VncPixelFormat *vnc_connection_get_pixel_format(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return &priv->fmt;
}


/**
 * vnc_connection_set_shared:
 * @conn: (transfer none): the connection object
 * @sharedFlag: the new sharing state
 *
 * Set the shared state for the connection. A TRUE value
 * allow allow this client to co-exist with other existing
 * clients. A FALSE value will cause other clients to be
 * dropped
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_shared(VncConnection *conn, gboolean sharedFlag)
{
    VncConnectionPrivate *priv = conn->priv;

    if (vnc_connection_is_open(conn))
        return FALSE;

    priv->sharedFlag = sharedFlag;

    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_get_shared:
 * @conn: (transfer none): the connection object
 *
 * Get the sharing state for the connection
 *
 * Returns: TRUE if other clients are permitted, FALSE otherwise
 */
gboolean vnc_connection_get_shared(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->sharedFlag;
}


/*
 * Must only be called from the SYSTEM coroutine
 */
static void vnc_connection_buffered_write(VncConnection *conn, const void *data, size_t size)
{
    VncConnectionPrivate *priv = conn->priv;
    size_t left;

    left = priv->xmit_buffer_capacity - priv->xmit_buffer_size;
    if (left < size) {
        priv->xmit_buffer_capacity += size + 4095;
        priv->xmit_buffer_capacity &= ~4095;

        priv->xmit_buffer = g_realloc(priv->xmit_buffer, priv->xmit_buffer_capacity);
    }

    memcpy(&priv->xmit_buffer[priv->xmit_buffer_size],
           data, size);

    priv->xmit_buffer_size += size;
}

/*
 * Must only be called from the SYSTEM coroutine
 */
static void vnc_connection_buffered_write_u8(VncConnection *conn, guint8 value)
{
    vnc_connection_buffered_write(conn, &value, 1);
}

/*
 * Must only be called from the SYSTEM coroutine
 */
static void vnc_connection_buffered_write_u16(VncConnection *conn, guint16 value)
{
    value = g_htons(value);
    vnc_connection_buffered_write(conn, &value, 2);
}

/*
 * Must only be called from the SYSTEM coroutine
 */
static void vnc_connection_buffered_write_u32(VncConnection *conn, guint32 value)
{
    value = g_htonl(value);
    vnc_connection_buffered_write(conn, &value, 4);
}

/*
 * Must only be called from the SYSTEM coroutine
 */
static void vnc_connection_buffered_write_s32(VncConnection *conn, gint32 value)
{
    value = g_htonl(value);
    vnc_connection_buffered_write(conn, &value, 4);
}

/*
 * Must only be called from the SYSTEM coroutine
 */
static void vnc_connection_buffered_flush(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    g_io_wakeup(&priv->wait);
}

/**
 * vnc_connection_set_pixel_format:
 * @conn: (transfer none): the connection object
 * @fmt: (transfer none): the new pixel format
 *
 * Tell the server what pixel format  to use for
 * framebuffer updates. It is only safe to use this
 * when no framebuffer updates are pending, otherwise
 * it is impossible to determine when the server has
 * switched over to using the new format.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_pixel_format(VncConnection *conn,
                                         const VncPixelFormat *fmt)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 pad[3] = {0};

    vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_SET_PIXEL_FORMAT);
    vnc_connection_buffered_write(conn, pad, 3);

    vnc_connection_buffered_write_u8(conn, fmt->bits_per_pixel);
    vnc_connection_buffered_write_u8(conn, fmt->depth);
    vnc_connection_buffered_write_u8(conn, fmt->byte_order == G_BIG_ENDIAN ? 1 : 0);
    vnc_connection_buffered_write_u8(conn, fmt->true_color_flag);

    vnc_connection_buffered_write_u16(conn, fmt->red_max);
    vnc_connection_buffered_write_u16(conn, fmt->green_max);
    vnc_connection_buffered_write_u16(conn, fmt->blue_max);

    vnc_connection_buffered_write_u8(conn, fmt->red_shift);
    vnc_connection_buffered_write_u8(conn, fmt->green_shift);
    vnc_connection_buffered_write_u8(conn, fmt->blue_shift);

    vnc_connection_buffered_write(conn, pad, 3);
    vnc_connection_buffered_flush(conn);

    memcpy(&priv->fmt, fmt, sizeof(*fmt));

    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_set_audio:
 * @conn: (transfer none): the connection object
 * @audio: (transfer none): the audio sink
 *
 * Set the audio sink to use for playing back audio from
 * the remote session.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_audio(VncConnection *conn,
                                  VncAudio *audio)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->audio)
        g_object_unref(priv->audio);
    priv->audio = audio;
    if (priv->audio)
        g_object_ref(priv->audio);

    return !vnc_connection_has_error(conn);
}


static void vnc_connection_send_audio_format(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU);
    vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO);
    vnc_connection_buffered_write_u16(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO_SET_FORMAT);

    vnc_connection_buffered_write_u8(conn,  priv->audio_format.format);
    vnc_connection_buffered_write_u8(conn,  priv->audio_format.nchannels);
    vnc_connection_buffered_write_u32(conn, priv->audio_format.frequency);
    vnc_connection_buffered_flush(conn);
    priv->audio_format_pending=FALSE;
}

/**
 * vnc_connection_set_audio_format:
 * @conn: (transfer none): the connection object
 * @fmt: (transfer none): the audio format
 *
 * Set the audio format specification to use for playback
 * from the remote session. The format should only be set
 * when the audio stream is not active, otherwise it will
 * be impossible to determine when the server has switched
 * to sending data in the new format
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_audio_format(VncConnection *conn,
                                         const VncAudioFormat *fmt)
{
    VncConnectionPrivate *priv = conn->priv;

    memcpy(&priv->audio_format, fmt, sizeof(*fmt));
    priv->audio_format_pending = TRUE;

    if (priv->has_audio)
        vnc_connection_send_audio_format(conn);

    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_get_audio_format:
 * @conn: (transfer none): the connection object
 *
 * Get the current audio format specification
 *
 * Returns: (transfer none): the current audio format
 */
const VncAudioFormat *vnc_connection_get_audio_format(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    return &priv->audio_format;
}


/**
 * vnc_connection_audio_enable:
 * @conn: (transfer none): the connection object
 *
 * Tell the server that it is permitted to send audio
 * data.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_audio_enable(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->has_audio)
        {
            vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU);
            vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO);
            vnc_connection_buffered_write_u16(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO_ENABLE);
            vnc_connection_buffered_flush(conn);
            priv->audio_enable_pending=FALSE;
        }
    else
        priv->audio_enable_pending=TRUE;
    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_audio_disable:
 * @conn: (transfer none): the connection object
 *
 * Tell the server that it is no longer permitted to send
 * audio. The client may continue to receive audio for a
 * time after this, since packets may already be in flight.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_audio_disable(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    priv->audio_disable_pending=TRUE;

    if (priv->has_audio)
        {
            vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU);
            vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO);
            vnc_connection_buffered_write_u16(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU_AUDIO_DISABLE);
            vnc_connection_buffered_flush(conn);
            priv->audio_disable_pending=FALSE;
        }
    else
        priv->audio_disable_pending=TRUE;
    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_set_encodings:
 * @conn: (transfer none): the connection object
 * @n_encoding: number of entries in @encoding
 * @encoding: (transfer none)(array length=n_encoding): the list of permitted encodings
 *
 * Inform the server of the list of encodings that it is
 * allowed to send. This should be done before requesting
 * any framebuffer updates
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_encodings(VncConnection *conn, int n_encoding, gint32 *encoding)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 pad[1] = {0};
    int i, skip_zrle=0;

    /*
     * RealVNC server is broken for ZRLE in some pixel formats.
     * Specifically if you have a format with either R, G or B
     * components with a max value > 255, it still uses a CPIXEL
     * of 3 bytes, even though the colour requirs 4 bytes. It
     * thus messes up the colours of the server in a way we can't
     * recover from on the client. Most VNC clients don't see this
     * problem since they send a 'set pixel format' message instead
     * of running with the server's default format.
     *
     * So we kill off ZRLE encoding for problematic pixel formats
     */
    for (i = 0; i < n_encoding; i++)
        if (priv->fmt.depth == 32 &&
            (priv->fmt.red_max > 255 ||
             priv->fmt.blue_max > 255 ||
             priv->fmt.green_max > 255) &&
            encoding[i] == VNC_CONNECTION_ENCODING_ZRLE) {
            VNC_DEBUG("Dropping ZRLE encoding for broken pixel format");
            skip_zrle++;
        }

    priv->has_ext_key_event = FALSE;
    priv->has_audio = FALSE;
    vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_SET_ENCODINGS);
    vnc_connection_buffered_write(conn, pad, 1);
    vnc_connection_buffered_write_u16(conn, n_encoding - skip_zrle);
    for (i = 0; i < n_encoding; i++) {
        if (skip_zrle && encoding[i] == VNC_CONNECTION_ENCODING_ZRLE)
            continue;
        vnc_connection_buffered_write_s32(conn, encoding[i]);
    }
    vnc_connection_buffered_flush(conn);
    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_framebuffer_update_request:
 * @conn: (transfer none): the connection object
 * @incremental: TRUE to only receive region with changes
 * @x: horizontal offset to region of update
 * @y: vertical offset to region of update
 * @width: horizontal size of region of update
 * @height: vertical size of region of update
 *
 * Request that the server send a framebuffer update when the
 * region positioned at (@x, @y) wth size (@width, @height)
 * sees damage. The update sent may be a subset of the region
 * requested, if @incremental is FALSE.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_framebuffer_update_request(VncConnection *conn,
                                                   gboolean incremental,
                                                   guint16 x, guint16 y,
                                                   guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Requesting framebuffer update at %d,%d size %dx%d, incremental %d",
              x, y, width, height, (int)incremental);

    priv->lastUpdateRequest.incremental = incremental;
    priv->lastUpdateRequest.x = x;
    priv->lastUpdateRequest.y = y;
    priv->lastUpdateRequest.width = width;
    priv->lastUpdateRequest.height = height;

    vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_FRAMEBUFFER_UPDATE_REQUEST);
    vnc_connection_buffered_write_u8(conn, incremental ? 1 : 0);
    vnc_connection_buffered_write_u16(conn, x);
    vnc_connection_buffered_write_u16(conn, y);
    vnc_connection_buffered_write_u16(conn, width);
    vnc_connection_buffered_write_u16(conn, height);
    vnc_connection_buffered_flush(conn);

    return !vnc_connection_has_error(conn);
}


/*
 * This is called when getting a psuedo-encoding message that
 * is not a desktop size, pixel format change.
 */
static gboolean
vnc_connection_resend_framebuffer_update_request(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Re-requesting framebuffer update at %d,%d size %dx%d, incremental %d",
              priv->lastUpdateRequest.x,
              priv->lastUpdateRequest.y,
              priv->lastUpdateRequest.width,
              priv->lastUpdateRequest.height,
              (int)priv->lastUpdateRequest.incremental);

    vnc_connection_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_FRAMEBUFFER_UPDATE_REQUEST);
    vnc_connection_write_u8(conn, priv->lastUpdateRequest.incremental ? 1 : 0);
    vnc_connection_write_u16(conn, priv->lastUpdateRequest.x);
    vnc_connection_write_u16(conn, priv->lastUpdateRequest.y);
    vnc_connection_write_u16(conn, priv->lastUpdateRequest.width);
    vnc_connection_write_u16(conn, priv->lastUpdateRequest.height);
    vnc_connection_flush(conn);

    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_key_event:
 * @conn: (transfer none): the connection object
 * @down_flag: TRUE if this is a key press, FALSE for a key release
 * @key: the X11 key code
 * @scancode: the XT scan code
 *
 * Send a key press/release event to the server. By default the
 * event will be sent with the X11 key code from @key. If the
 * extended key event protocol extension is active, the @scancode
 * will be sent instead.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_key_event(VncConnection *conn, gboolean down_flag,
                                  guint32 key, guint16 scancode)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 pad[2] = {0};

    VNC_DEBUG("Key event %u %u %d Extended: %d", key, scancode, down_flag, priv->has_ext_key_event);
    if (priv->has_ext_key_event) {
        vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU);
        vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_QEMU_KEY);
        vnc_connection_buffered_write_u16(conn, down_flag ? 1 : 0);
        vnc_connection_buffered_write_u32(conn, key);
        vnc_connection_buffered_write_u32(conn, scancode);
    } else {
        vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_KEY);
        vnc_connection_buffered_write_u8(conn, down_flag ? 1 : 0);
        vnc_connection_buffered_write(conn, pad, 2);
        vnc_connection_buffered_write_u32(conn, key);
    }

    vnc_connection_buffered_flush(conn);
    return !vnc_connection_has_error(conn);
}

/**
 * vnc_connection_pointer_event:
 * @conn: (transfer none): the connection object
 * @button_mask: the new state of the buttons
 * @x: the new horizontal position of the pointer
 * @y: the new veritical position of the pointer
 *
 * Send a pointer event to the server, reflecting either movement
 * of the pointer, or a change in state of its buttons, or both.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_pointer_event(VncConnection *conn, guint8 button_mask,
                                      guint16 x, guint16 y)
{
    vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_POINTER);
    vnc_connection_buffered_write_u8(conn, button_mask);
    vnc_connection_buffered_write_u16(conn, x);
    vnc_connection_buffered_write_u16(conn, y);
    vnc_connection_buffered_flush(conn);
    return !vnc_connection_has_error(conn);
}

/**
 * vnc_connection_cut_text:
 * @conn: (transfer none): the connection object
 * @data: (transfer none)(array length=@length): the data to send
 * @length: the length of @data
 *
 * Send updated clipboard text to the server. The encoding of @data
 * is undefined by the protocol specification, but recommended practice
 * is to use UTF-8
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_client_cut_text(VncConnection *conn,
                                        const void *data, size_t length)
{
    guint8 pad[3] = {0};

    vnc_connection_buffered_write_u8(conn, VNC_CONNECTION_CLIENT_MESSAGE_CUT_TEXT);
    vnc_connection_buffered_write(conn, pad, 3);
    vnc_connection_buffered_write_u32(conn, length);
    vnc_connection_buffered_write(conn, data, length);
    vnc_connection_buffered_flush(conn);
    return !vnc_connection_has_error(conn);
}


static guint8 vnc_connection_swap_rfb_8(VncConnection *conn G_GNUC_UNUSED, guint8 pixel)
{
    return pixel;
}

/* VNC server RFB  format ->  local host native format */
static guint16 vnc_connection_swap_rfb_16(VncConnection *conn, guint16 pixel)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->fbSwapRemote)
        return  (((pixel >> 8) & 0xFF) << 0) |
            (((pixel >> 0) & 0xFF) << 8);
    else
        return pixel;
}

/* VNC server RFB  format ->  local host native format */
static guint32 vnc_connection_swap_rfb_32(VncConnection *conn, guint32 pixel)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->fbSwapRemote)
        return  (((pixel >> 24) & 0xFF) <<  0) |
            (((pixel >> 16) & 0xFF) <<  8) |
            (((pixel >>  8) & 0xFF) << 16) |
            (((pixel >>  0) & 0xFF) << 24);
    else
        return pixel;
}

#define SRC 8
#define DST 8
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 16
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 32
#include "vncconnectionblt.h"
#undef SRC
#undef DST


#define SRC 16
#define DST 8
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 16
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 32
#include "vncconnectionblt.h"
#undef SRC
#undef DST


#define SRC 32
#define DST 8
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 32
#define DST 16
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 32
#define DST 32
#include "vncconnectionblt.h"
#undef SRC
#undef DST

static vnc_connection_rich_cursor_blt_func *vnc_connection_rich_cursor_blt_table[3] = {
    vnc_connection_rich_cursor_blt_8x32,
    vnc_connection_rich_cursor_blt_16x32,
    vnc_connection_rich_cursor_blt_32x32,
};

static vnc_connection_tight_compute_predicted_func *vnc_connection_tight_compute_predicted_table[3] = {
    (vnc_connection_tight_compute_predicted_func *)vnc_connection_tight_compute_predicted_8x8,
    (vnc_connection_tight_compute_predicted_func *)vnc_connection_tight_compute_predicted_16x16,
    (vnc_connection_tight_compute_predicted_func *)vnc_connection_tight_compute_predicted_32x32,
};

static vnc_connection_tight_sum_pixel_func *vnc_connection_tight_sum_pixel_table[3] = {
    (vnc_connection_tight_sum_pixel_func *)vnc_connection_tight_sum_pixel_8x8,
    (vnc_connection_tight_sum_pixel_func *)vnc_connection_tight_sum_pixel_16x16,
    (vnc_connection_tight_sum_pixel_func *)vnc_connection_tight_sum_pixel_32x32,
};


static void vnc_connection_raw_update(VncConnection *conn,
                                      guint16 x, guint16 y,
                                      guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;

    /* optimize for perfect match between server/client
       FWIW, in the local case, we ought to be doing a write
       directly from the source framebuffer and a read directly
       into the client framebuffer
    */
    if (vnc_framebuffer_perfect_format_match(priv->fb)) {
        int i;
        int rowstride = vnc_framebuffer_get_rowstride(priv->fb);
        guint8 *dst = vnc_framebuffer_get_buffer(priv->fb);

        dst += (y * rowstride) + (x * (priv->fmt.bits_per_pixel/8));

        for (i = 0; i < height; i++) {
            vnc_connection_read(conn, dst,
                                width * (priv->fmt.bits_per_pixel/8));
            dst += rowstride;
        }
    } else {
        guint8 *dst;
        int i;

        dst = g_malloc(width * (priv->fmt.bits_per_pixel / 8));
        for (i = 0; i < height; i++) {
            vnc_connection_read(conn, dst, width * (priv->fmt.bits_per_pixel / 8));
            vnc_framebuffer_blt(priv->fb, dst, 0, x, y + i, width, 1);
        }
        g_free(dst);
    }
}

static void vnc_connection_copyrect_update(VncConnection *conn,
                                           guint16 dst_x, guint16 dst_y,
                                           guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    int src_x, src_y;

    src_x = vnc_connection_read_u16(conn);
    src_y = vnc_connection_read_u16(conn);

    vnc_framebuffer_copyrect(priv->fb,
                             src_x, src_y,
                             dst_x, dst_y,
                             width, height);
}

static void vnc_connection_hextile_rect(VncConnection *conn,
                                        guint8 flags,
                                        guint16 x, guint16 y,
                                        guint16 width, guint16 height,
                                        guint8 *fg, guint8 *bg)
{
    VncConnectionPrivate *priv = conn->priv;
    int i;

    if (flags & 0x01) {
        vnc_connection_raw_update(conn, x, y, width, height);
    } else {
        /* Background Specified */
        if (flags & 0x02)
            vnc_connection_read_pixel(conn, bg);

        /* Foreground Specified */
        if (flags & 0x04)
            vnc_connection_read_pixel(conn, fg);

        vnc_framebuffer_fill(priv->fb, bg, x, y, width, height);

        /* AnySubrects */
        if (flags & 0x08) {
            guint8 n_rects = vnc_connection_read_u8(conn);

            for (i = 0; i < n_rects; i++) {
                guint8 xy, wh;

                /* SubrectsColored */
                if (flags & 0x10)
                    vnc_connection_read_pixel(conn, fg);

                xy = vnc_connection_read_u8(conn);
                wh = vnc_connection_read_u8(conn);

                vnc_framebuffer_fill(priv->fb, fg,
                                     x + nibhi(xy), y + niblo(xy),
                                     nibhi(wh) + 1, niblo(wh) + 1);
            }
        }
    }
}


static void vnc_connection_hextile_update(VncConnection *conn,
                                          guint16 x, guint16 y,
                                          guint16 width, guint16 height)
{
    guint8 fg[4];
    guint8 bg[4];

    int j;
    for (j = 0; j < height; j += 16) {
        int i;
        for (i = 0; i < width; i += 16) {
            guint8 flags;
            int w = MIN(16, width - i);
            int h = MIN(16, height - j);

            flags = vnc_connection_read_u8(conn);
            vnc_connection_hextile_rect(conn, flags,
                                        x + i, y + j,
                                        w, h,
                                        fg, bg);
        }
    }
}

static void vnc_connection_rre_update(VncConnection *conn,
                                      guint16 x, guint16 y,
                                      guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 bg[4];
    guint32 num;
    guint32 i;

    num = vnc_connection_read_u32(conn);
    vnc_connection_read_pixel(conn, bg);
    vnc_framebuffer_fill(priv->fb, bg, x, y, width, height);

    for (i = 0; i < num; i++) {
        guint8 fg[4];
        guint16 sub_x, sub_y, sub_w, sub_h;

        vnc_connection_read_pixel(conn, fg);
        sub_x = vnc_connection_read_u16(conn);
        sub_y = vnc_connection_read_u16(conn);
        sub_w = vnc_connection_read_u16(conn);
        sub_h = vnc_connection_read_u16(conn);

        vnc_framebuffer_fill(priv->fb, fg,
                             x + sub_x, y + sub_y, sub_w, sub_h);
    }
}

/* CPIXELs are optimized slightly.  32-bit pixel values are packed into 24-bit
 * values. */
static void vnc_connection_read_cpixel(VncConnection *conn, guint8 *pixel)
{
    VncConnectionPrivate *priv = conn->priv;
    int bpp = vnc_connection_pixel_size(conn);

    memset(pixel, 0, bpp);

    if (bpp == 4 && priv->fmt.true_color_flag) {
        int fitsInMSB = ((priv->fmt.red_shift > 7) &&
                         (priv->fmt.green_shift > 7) &&
                         (priv->fmt.blue_shift > 7));
        int fitsInLSB = (((priv->fmt.red_max << priv->fmt.red_shift) < (1 << 24)) &&
                         ((priv->fmt.green_max << priv->fmt.green_shift) < (1 << 24)) &&
                         ((priv->fmt.blue_max << priv->fmt.blue_shift) < (1 << 24)));

        /*
         * We need to analyse the shifts to see if they fit in 3 bytes,
         * rather than looking at the declared  'depth' for the format
         * because despite what the RFB spec says, this is what RealVNC
         * server actually does in practice.
         */
        if (fitsInMSB || fitsInLSB) {
            bpp = 3;
            if (priv->fmt.depth == 24 &&
                priv->fmt.byte_order == G_BIG_ENDIAN)
                pixel++;
        }
    }

    vnc_connection_read(conn, pixel, bpp);
}

static void vnc_connection_zrle_update_tile_blit(VncConnection *conn,
                                                 guint16 x, guint16 y,
                                                 guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 *blit_data;
    int i, bpp;

    blit_data = g_new0(guint8, 4*64*64);

    bpp = vnc_connection_pixel_size(conn);

    for (i = 0; i < width * height; i++)
        vnc_connection_read_cpixel(conn, blit_data + (i * bpp));

    vnc_framebuffer_blt(priv->fb, blit_data, width * bpp, x, y, width, height);

    g_free(blit_data);
}

static guint8 vnc_connection_read_zrle_pi(VncConnection *conn, int palette_size)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 pi = 0;

    if (priv->zrle_pi_bits == 0) {
        priv->zrle_pi = vnc_connection_read_u8(conn);
        priv->zrle_pi_bits = 8;
    }
    if ( palette_size == 2) {
        pi = (priv->zrle_pi >> (priv->zrle_pi_bits - 1)) & 1;
        priv->zrle_pi_bits -= 1;
    } else if ((palette_size == 3) || (palette_size == 4)) {
        pi = (priv->zrle_pi >> (priv->zrle_pi_bits - 2)) & 3;
        priv->zrle_pi_bits -= 2;
    } else if ((palette_size >=5) && (palette_size <=16)){
        pi = (priv->zrle_pi >> (priv->zrle_pi_bits - 4)) & 15;
        priv->zrle_pi_bits -= 4;
    }

    return pi;
}

static void vnc_connection_zrle_update_tile_palette(VncConnection *conn,
                                                    guint8 palette_size,
                                                    guint16 x, guint16 y,
                                                    guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 palette[128][4];
    int i, j;

    for (i = 0; i < palette_size; i++)
        vnc_connection_read_cpixel(conn, palette[i]);

    for (j = 0; j < height; j++) {
        /* discard any padding bits */
        priv->zrle_pi_bits = 0;

        for (i = 0; i < width; i++) {
            int ind = vnc_connection_read_zrle_pi(conn, palette_size);

            vnc_framebuffer_set_pixel_at(priv->fb, palette[ind & 0x7F],
                                         x + i, y + j);
        }
    }
}

static int vnc_connection_read_zrle_rl(VncConnection *conn)
{
    int rl = 1;
    guint8 b;

    do {
        b = vnc_connection_read_u8(conn);
        rl += b;
    } while (!vnc_connection_has_error(conn) && b == 255);

    return rl;
}

static void vnc_connection_zrle_update_tile_rle(VncConnection *conn,
                                                guint16 x, guint16 y,
                                                guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    int i, j, rl = 0;
    guint8 pixel[4];

    for (j = 0; j < height; j++) {
        for (i = 0; i < width; i++) {
            if (rl == 0) {
                vnc_connection_read_cpixel(conn, pixel);
                rl = vnc_connection_read_zrle_rl(conn);
            }
            vnc_framebuffer_set_pixel_at(priv->fb, pixel, x + i, y + j);
            rl -= 1;
        }
    }
}

static void vnc_connection_zrle_update_tile_prle(VncConnection *conn,
                                                 guint8 palette_size,
                                                 guint16 x, guint16 y,
                                                 guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    int i, j, rl = 0;
    guint8 palette[128][4];
    guint8 pi = 0;

    for (i = 0; i < palette_size; i++)
        vnc_connection_read_cpixel(conn, palette[i]);

    for (j = 0; j < height; j++) {
        for (i = 0; i < width; i++) {
            if (rl == 0) {
                pi = vnc_connection_read_u8(conn);
                if (pi & 0x80) {
                    rl = vnc_connection_read_zrle_rl(conn);
                    pi &= 0x7F;
                } else
                    rl = 1;
            }

            vnc_framebuffer_set_pixel_at(priv->fb, palette[pi], x + i, y + j);
            rl -= 1;
        }
    }
}

static void vnc_connection_zrle_update_tile(VncConnection *conn, guint16 x, guint16 y,
                                            guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 subencoding = vnc_connection_read_u8(conn);
    guint8 pixel[4];

    if (subencoding == 0 ) {
        /* Raw pixel data */
        vnc_connection_zrle_update_tile_blit(conn, x, y, width, height);
    } else if (subencoding == 1) {
        /* Solid tile of a single color */
        vnc_connection_read_cpixel(conn, pixel);
        vnc_framebuffer_fill(priv->fb, pixel, x, y, width, height);
    } else if ((subencoding >= 2) && (subencoding <= 16)) {
        /* Packed palette types */
        vnc_connection_zrle_update_tile_palette(conn, subencoding,
                                                x, y, width, height);
    } else if ((subencoding >= 17) && (subencoding <= 127)) {
        /* FIXME raise error? */
    } else if (subencoding == 128) {
        /* Plain RLE */
        vnc_connection_zrle_update_tile_rle(conn, x, y, width, height);
    } else if (subencoding == 129) {

    } else if (subencoding >= 130) {
        /* Palette RLE */
        vnc_connection_zrle_update_tile_prle(conn, subencoding - 128,
                                             x, y, width, height);
    }
}

static void vnc_connection_zrle_update(VncConnection *conn,
                                       guint16 x, guint16 y,
                                       guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    guint32 length;
    guint16 i, j;
    guint8 *zlib_data;

    length = vnc_connection_read_u32(conn);
    zlib_data = g_malloc(length);
    vnc_connection_read(conn, zlib_data, length);

    /* setup subsequent calls to vnc_connection_read*() to use the compressed data */
    priv->uncompressed_offset = 0;
    priv->uncompressed_size = 0;
    priv->compressed_length = length;
    priv->compressed_buffer = zlib_data;
    priv->strm = &priv->streams[0];

    for (j = 0; j < height; j += 64) {
        for (i = 0; i < width; i += 64) {
            guint16 w, h;

            w = MIN(width - i, 64);
            h = MIN(height - j, 64);
            vnc_connection_zrle_update_tile(conn, x + i, y + j, w, h);
        }
    }

    priv->strm = NULL;
    priv->uncompressed_offset = 0;
    priv->uncompressed_size = 0;
    priv->compressed_length = 0;
    priv->compressed_buffer = NULL;

    g_free(zlib_data);
}

static guint32 vnc_connection_read_cint(VncConnection *conn)
{
    guint32 value = 0;
    guint8 val;

    val = vnc_connection_read_u8(conn);
    value = (val & 0x7F);
    if (!(val & 0x80))
        return value;

    val = vnc_connection_read_u8(conn);
    value |= (val & 0x7F) << 7;

    if (!(val & 0x80))
        return value;

    value |= vnc_connection_read_u8(conn) << 14;

    return value;
}

static int vnc_connection_tpixel_size(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->fmt.depth == 24)
        return 3;
    return priv->fmt.bits_per_pixel / 8;
}

static void vnc_connection_read_tpixel(VncConnection *conn, guint8 *pixel)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->fmt.depth == 24) {
        guint32 val;
        vnc_connection_read(conn, pixel, 3);
        val = (pixel[0] << priv->fmt.red_shift)
            | (pixel[1] << priv->fmt.green_shift)
            | (pixel[2] << priv->fmt.blue_shift);

        if (priv->fmt.byte_order != G_BYTE_ORDER)
            val =   (((val >>  0) & 0xFF) << 24) |
                (((val >>  8) & 0xFF) << 16) |
                (((val >> 16) & 0xFF) << 8) |
                (((val >> 24) & 0xFF) << 0);

        memcpy(pixel, &val, 4);
    } else
        vnc_connection_read_pixel(conn, pixel);
}

static void vnc_connection_tight_update_copy(VncConnection *conn,
                                             guint16 x, guint16 y,
                                             guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 pixel[4];
    int i, j;

    for (j = 0; j < height; j++) {
        for (i = 0; i < width; i++) {
            vnc_connection_read_tpixel(conn, pixel);
            vnc_framebuffer_set_pixel_at(priv->fb, pixel, x + i, y + j);
        }
    }
}

static int vnc_connection_tight_get_pi(VncConnection *conn, guint8 *ra,
                                       int i, guint8 palette_size)
{
    if (palette_size == 2) {
        if ((i % 8) == 0)
            *ra = vnc_connection_read_u8(conn);
        return (*ra >> (7 - (i % 8))) & 1;
    }

    return vnc_connection_read_u8(conn);
}

static void vnc_connection_tight_update_palette(VncConnection *conn,
                                                int palette_size, guint8 *palette,
                                                guint16 x, guint16 y,
                                                guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    int i, j;

    for (j = 0; j < height; j++) {
        guint8 ra = 0;

        for (i = 0; i < width; i++) {
            guint8 ind;

            ind = vnc_connection_tight_get_pi(conn, &ra, i, palette_size);
            vnc_framebuffer_set_pixel_at(priv->fb, &palette[ind * 4], x + i, y + j);
        }
    }
}

static void vnc_connection_tight_compute_predicted(VncConnection *conn, guint8 *ppixel,
                                                   guint8 *lp, guint8 *cp,
                                                   guint8 *llp)
{
    VncConnectionPrivate *priv = conn->priv;

    priv->tight_compute_predicted(conn, ppixel, lp, cp, llp);
}

static void vnc_connection_tight_sum_pixel(VncConnection *conn,
                                           guint8 *lhs, guint8 *rhs)
{
    VncConnectionPrivate *priv = conn->priv;

    priv->tight_sum_pixel(conn, lhs, rhs);
}

static void vnc_connection_tight_update_gradient(VncConnection *conn,
                                                 guint16 x, guint16 y,
                                                 guint16 width, guint16 height)
{
    int i, j;
    guint8 zero_pixel[4];
    guint8 *last_row, *row;
    int bpp;
    VncConnectionPrivate *priv = conn->priv;

    bpp = vnc_connection_pixel_size(conn);
    last_row = g_malloc(width * bpp);
    row = g_malloc(width * bpp);

    memset(last_row, 0, width * bpp);
    memset(zero_pixel, 0, 4);

    for (j = 0; j < height; j++) {
        guint8 *tmp_row;
        guint8 *llp, *lp;

        /* use zero pixels for the edge cases */
        llp = zero_pixel;
        lp = zero_pixel;

        for (i = 0; i < width; i++) {
            guint8 predicted_pixel[4];

            /* compute predicted pixel value */
            vnc_connection_tight_compute_predicted(conn, predicted_pixel,
                                                   lp, last_row + i * bpp,
                                                   llp);

            /* read the difference pixel from the wire */
            vnc_connection_read_tpixel(conn, row + i * bpp);

            /* sum the predicted pixel and the difference to get
             * the original pixel value */
            vnc_connection_tight_sum_pixel(conn, row + i * bpp,
                                           predicted_pixel);

            llp = last_row + i * bpp;
            lp = row + i * bpp;
        }

        /* write out row of pixel data */
        vnc_framebuffer_blt(priv->fb, row, width * bpp, x, y + j, width, 1);

        /* swap last row and current row */
        tmp_row = last_row;
        last_row = row;
        row = tmp_row;
    }

    g_free(row);
    g_free(last_row);
}


static void vnc_connection_tight_update_jpeg(VncConnection *conn, guint16 x, guint16 y,
                                             guint16 width, guint16 height,
                                             guint8 *data, size_t length)
{
    VncConnectionPrivate *priv = conn->priv;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    GdkPixbuf *p;

    if (!gdk_pixbuf_loader_write(loader, data, length, NULL)) {
        vnc_connection_set_error(conn, "%s", "Unable to decode jpeg data");
        return;
    }

    gdk_pixbuf_loader_close(loader, NULL);

    p = g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
    g_object_unref(loader);

    vnc_framebuffer_rgb24_blt(priv->fb,
                              gdk_pixbuf_get_pixels(p),
                              gdk_pixbuf_get_rowstride(p),
                              x, y, width, height);

    g_object_unref(p);
}

static void vnc_connection_tight_update(VncConnection *conn,
                                        guint16 x, guint16 y,
                                        guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 ccontrol;
    guint8 pixel[4];
    int i;

    ccontrol = vnc_connection_read_u8(conn);

    for (i = 0; i < 4; i++) {
        if (ccontrol & (1 << i)) {
            inflateEnd(&priv->streams[i + 1]);
            inflateInit(&priv->streams[i + 1]);
        }
    }

    ccontrol >>= 4;
    ccontrol &= 0x0F;

    if (ccontrol <= 7) {
        /* basic */
        guint8 filter_id = 0;
        guint32 data_size, zlib_length;
        guint8 *zlib_data = NULL;
        guint8 palette[256][4];
        int palette_size = 0;

        if (ccontrol & 0x04)
            filter_id = vnc_connection_read_u8(conn);

        priv->strm = &priv->streams[(ccontrol & 0x03) + 1];

        if (filter_id == 1) {
            palette_size = vnc_connection_read_u8(conn);
            palette_size += 1;
            for (i = 0; i < palette_size; i++)
                vnc_connection_read_tpixel(conn, palette[i]);
        }

        if (filter_id == 1) {
            if (palette_size == 2)
                data_size = ((width + 7) / 8) * height;
            else
                data_size = width * height;
        } else
            data_size = width * height * vnc_connection_tpixel_size(conn);

        if (data_size >= 12) {
            zlib_length = vnc_connection_read_cint(conn);
            zlib_data = g_malloc(zlib_length);

            vnc_connection_read(conn, zlib_data, zlib_length);

            priv->uncompressed_offset = 0;
            priv->uncompressed_size = 0;
            priv->compressed_length = zlib_length;
            priv->compressed_buffer = zlib_data;
        }

        switch (filter_id) {
        case 0: /* copy */
            vnc_connection_tight_update_copy(conn, x, y, width, height);
            break;
        case 1: /* palette */
            vnc_connection_tight_update_palette(conn, palette_size,
                                                (guint8 *)palette,
                                                x, y, width, height);
            break;
        case 2: /* gradient */
            vnc_connection_tight_update_gradient(conn, x, y, width, height);
            break;
        default: /* error */
            vnc_connection_set_error(conn, "Unexpected tight filter id %d",
                                     filter_id);
            break;
        }

        if (data_size >= 12) {
            priv->uncompressed_offset = 0;
            priv->uncompressed_size = 0;
            priv->compressed_length = 0;
            priv->compressed_buffer = NULL;

            g_free(zlib_data);
        }

        priv->strm = NULL;
    } else if (ccontrol == 8) {
        /* fill */
        /* FIXME check each width; endianness */
        vnc_connection_read_tpixel(conn, pixel);
        vnc_framebuffer_fill(priv->fb, pixel, x, y, width, height);
    } else if (ccontrol == 9) {
        /* jpeg */
        guint32 length;
        guint8 *jpeg_data;

        length = vnc_connection_read_cint(conn);
        jpeg_data = g_malloc(length);
        vnc_connection_read(conn, jpeg_data, length);
        vnc_connection_tight_update_jpeg(conn, x, y, width, height,
                                         jpeg_data, length);
        g_free(jpeg_data);
    } else {
        vnc_connection_set_error(conn, "Unexpected tight ccontrol %d",
                                 ccontrol);
    }
}

static void vnc_connection_update(VncConnection *conn, int x, int y, int width, int height)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    if (priv->coroutine_stop)
        return;

    VNC_DEBUG("Notify update area (%dx%d) at location %d,%d", width, height, x, y);

    sigdata.params.area.x = x;
    sigdata.params.area.y = y;
    sigdata.params.area.width = width;
    sigdata.params.area.height = height;
    vnc_connection_emit_main_context(conn, VNC_FRAMEBUFFER_UPDATE, &sigdata);
}


static void vnc_connection_bell(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    if (priv->coroutine_stop)
        return;

    VNC_DEBUG("Server beep");

    vnc_connection_emit_main_context(conn, VNC_BELL, &sigdata);
}

static void vnc_connection_server_cut_text(VncConnection *conn,
                                           const void *data,
                                           size_t len)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;
    GString *text;

    if (priv->coroutine_stop)
        return;

    text = g_string_new_len ((const gchar *)data, len);
    sigdata.params.text = text->str;

    vnc_connection_emit_main_context(conn, VNC_SERVER_CUT_TEXT, &sigdata);

    g_string_free(text, TRUE);
}

static void vnc_connection_resize(VncConnection *conn, int width, int height)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    if (priv->coroutine_stop)
        return;

    priv->width = width;
    priv->height = height;

    sigdata.params.size.width = width;
    sigdata.params.size.height = height;
    vnc_connection_emit_main_context(conn, VNC_DESKTOP_RESIZE, &sigdata);
}

static void vnc_connection_pixel_format(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    if (priv->coroutine_stop)
        return;

    sigdata.params.pixelFormat = &priv->fmt;
    vnc_connection_emit_main_context(conn, VNC_PIXEL_FORMAT_CHANGED, &sigdata);
}

static void vnc_connection_pointer_type_change(VncConnection *conn, gboolean absPointer)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    VNC_DEBUG("Pointer mode %s", absPointer ? "absolute" : "relative");

    if (priv->absPointer == absPointer)
        return;
    priv->absPointer = absPointer;

    if (priv->coroutine_stop)
        return;

    sigdata.params.absPointer = absPointer;
    vnc_connection_emit_main_context(conn, VNC_POINTER_MODE_CHANGED, &sigdata);
}

static void vnc_connection_rich_cursor_blt(VncConnection *conn, guint8 *pixbuf,
                                           guint8 *image, guint8 *mask,
                                           int pitch, guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;

    priv->rich_cursor_blt(conn, pixbuf, image, mask, pitch, width, height);
}

static void vnc_connection_rich_cursor(VncConnection *conn, int x, int y, int width, int height)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    if (priv->cursor) {
        g_object_unref(priv->cursor);
        priv->cursor = NULL;
    }

    if (width && height) {
        guint8 *pixbuf = NULL;
        guint8 *image = NULL, *mask = NULL;
        int imagelen, masklen;

        imagelen = width * height * (priv->fmt.bits_per_pixel / 8);
        masklen = ((width + 7)/8) * height;

        image = g_malloc(imagelen);
        mask = g_malloc(masklen);
        pixbuf = g_malloc(width * height * 4); /* RGB-A 8bit */

        vnc_connection_read(conn, image, imagelen);
        vnc_connection_read(conn, mask, masklen);

        vnc_connection_rich_cursor_blt(conn, pixbuf, image, mask,
                                       width * (priv->fmt.bits_per_pixel/8),
                                       width, height);

        g_free(image);
        g_free(mask);

        priv->cursor = vnc_cursor_new(pixbuf, x, y, width, height);
    }

    if (priv->coroutine_stop)
        return;

    sigdata.params.cursor = priv->cursor;

    vnc_connection_emit_main_context(conn, VNC_CURSOR_CHANGED, &sigdata);
}

static void vnc_connection_xcursor(VncConnection *conn, int x, int y, int width, int height)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;

    if (priv->cursor) {
        g_object_unref(priv->cursor);
        priv->cursor = NULL;
    }

    if (width && height) {
        guint8 *pixbuf = NULL;
        guint8 *data, *mask, *datap, *maskp;
        guint32 *pixp;
        int rowlen;
        int x1, y1;
        guint8 fgrgb[3], bgrgb[3];
        guint32 fg, bg;
        vnc_connection_read(conn, fgrgb, 3);
        vnc_connection_read(conn, bgrgb, 3);
        fg = (255 << 24) | (fgrgb[0] << 16) | (fgrgb[1] << 8) | fgrgb[2];
        bg = (255 << 24) | (bgrgb[0] << 16) | (bgrgb[1] << 8) | bgrgb[2];

        rowlen = ((width + 7)/8);
        data = g_malloc(rowlen*height);
        mask = g_malloc(rowlen*height);
        pixbuf = g_malloc(width * height * 4); /* RGB-A 8bit */

        vnc_connection_read(conn, data, rowlen*height);
        vnc_connection_read(conn, mask, rowlen*height);
        datap = data;
        maskp = mask;
        pixp = (guint32*)pixbuf;
        for (y1 = 0; y1 < height; y1++) {
            for (x1 = 0; x1 < width; x1++) {
                *pixp++ = ((maskp[x1 / 8] >> (7-(x1 % 8))) & 1) ?
                    (((datap[x1 / 8] >> (7-(x1 % 8))) & 1) ? fg : bg) : 0;
            }
            datap += rowlen;
            maskp += rowlen;
        }
        g_free(data);
        g_free(mask);

        priv->cursor = vnc_cursor_new(pixbuf, x, y, width, height);
    }

    if (priv->coroutine_stop)
        return;

    sigdata.params.cursor = priv->cursor;

    vnc_connection_emit_main_context(conn, VNC_CURSOR_CHANGED, &sigdata);
}

static void vnc_connection_ext_key_event(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Keyboard mode extended");
    priv->has_ext_key_event = TRUE;
}


static gboolean vnc_connection_validate_boundary(VncConnection *conn,
                                                 guint16 x, guint16 y,
                                                 guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;

    if ((x + width) > priv->width || (y + height) > priv->height) {
        vnc_connection_set_error(conn, "Framebuffer update %dx%d at %d,%d "
                                 "outside boundary %dx%d",
                                 width, height, x, y, priv->width, priv->height);
    }

    return !vnc_connection_has_error(conn);
}


static gboolean vnc_connection_framebuffer_update(VncConnection *conn, gint32 etype,
                                                  guint16 x, guint16 y,
                                                  guint16 width, guint16 height)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("FramebufferUpdate type=%d area (%dx%d) at location %d,%d",
              etype, width, height, x, y);

    if (vnc_connection_has_error(conn))
        return !vnc_connection_has_error(conn);

    switch (etype) {
    case VNC_CONNECTION_ENCODING_RAW:
        if (!vnc_connection_validate_boundary(conn, x, y, width, height))
            break;
        vnc_connection_raw_update(conn, x, y, width, height);
        vnc_connection_update(conn, x, y, width, height);
        break;
    case VNC_CONNECTION_ENCODING_COPY_RECT:
        if (!vnc_connection_validate_boundary(conn, x, y, width, height))
            break;
        vnc_connection_copyrect_update(conn, x, y, width, height);
        vnc_connection_update(conn, x, y, width, height);
        break;
    case VNC_CONNECTION_ENCODING_RRE:
        if (!vnc_connection_validate_boundary(conn, x, y, width, height))
            break;
        vnc_connection_rre_update(conn, x, y, width, height);
        vnc_connection_update(conn, x, y, width, height);
        break;
    case VNC_CONNECTION_ENCODING_HEXTILE:
        if (!vnc_connection_validate_boundary(conn, x, y, width, height))
            break;
        vnc_connection_hextile_update(conn, x, y, width, height);
        vnc_connection_update(conn, x, y, width, height);
        break;
    case VNC_CONNECTION_ENCODING_ZRLE:
        if (!vnc_connection_validate_boundary(conn, x, y, width, height))
            break;
        vnc_connection_zrle_update(conn, x, y, width, height);
        vnc_connection_update(conn, x, y, width, height);
        break;
    case VNC_CONNECTION_ENCODING_TIGHT:
        if (!vnc_connection_validate_boundary(conn, x, y, width, height))
            break;
        vnc_connection_tight_update(conn, x, y, width, height);
        vnc_connection_update(conn, x, y, width, height);
        break;
    case VNC_CONNECTION_ENCODING_DESKTOP_RESIZE:
        vnc_connection_resize(conn, width, height);
        break;
    case VNC_CONNECTION_ENCODING_POINTER_CHANGE:
        vnc_connection_pointer_type_change(conn, x);
        vnc_connection_resend_framebuffer_update_request(conn);
        break;
    case VNC_CONNECTION_ENCODING_LED_STATE:
        vnc_connection_ledstate_change(conn);
        vnc_connection_resend_framebuffer_update_request(conn);
        break;
    case VNC_CONNECTION_ENCODING_WMVi:
        vnc_connection_read_pixel_format(conn, &priv->fmt);
        vnc_connection_pixel_format(conn);
        break;
    case VNC_CONNECTION_ENCODING_RICH_CURSOR:
        vnc_connection_rich_cursor(conn, x, y, width, height);
        vnc_connection_resend_framebuffer_update_request(conn);
        break;
    case VNC_CONNECTION_ENCODING_XCURSOR:
        vnc_connection_xcursor(conn, x, y, width, height);
        vnc_connection_resend_framebuffer_update_request(conn);
        break;
    case VNC_CONNECTION_ENCODING_EXT_KEY_EVENT:
        vnc_connection_ext_key_event(conn);
        vnc_connection_resend_framebuffer_update_request(conn);
        break;
    case VNC_CONNECTION_ENCODING_AUDIO:
        VNC_DEBUG("Audio encoding support");
        priv->has_audio=TRUE;

        if (priv->audio_disable_pending)
            vnc_connection_audio_disable(conn);
        if (priv->audio_format_pending)
            vnc_connection_send_audio_format(conn);
        if (priv->audio_enable_pending)
            vnc_connection_audio_enable(conn);
        break;
    default:
        vnc_connection_set_error(conn, "Received an unknown encoding type: %d", etype);
        break;
    }

    return !vnc_connection_has_error(conn);
}


static gboolean vnc_connection_audio_timer(gpointer opaque)
{
    VncConnection *conn = opaque;
    VncConnectionPrivate *priv = conn->priv;

    priv->audio_timer = 0;
    if (!priv->audio_sample)
        return FALSE;

    VNC_DEBUG("Audio tick %u\n", priv->audio_sample->length);

    if (priv->audio)
        vnc_audio_playback_data(priv->audio, priv->audio_sample);

    vnc_audio_sample_free(priv->audio_sample);
    priv->audio_sample = NULL;
    return FALSE;
}


struct audio_action_data
{
    VncConnection *conn;
    struct coroutine *caller;
    enum {
        VNC_AUDIO_PLAYBACK_STOP = 0,
        VNC_AUDIO_PLAYBACK_START = 1,
        VNC_AUDIO_PLAYBACK_DATA = 2,
    } action;
};

static gboolean do_vnc_connection_audio_action(gpointer opaque)
{
    struct audio_action_data *data = opaque;
    VncConnectionPrivate *priv = data->conn->priv;

    VNC_DEBUG("Audio action main context %u", data->action);

    switch (data->action) {
    case VNC_AUDIO_PLAYBACK_STOP:
        vnc_audio_playback_stop(priv->audio);
        break;
    case VNC_AUDIO_PLAYBACK_START:
        vnc_audio_playback_start(priv->audio, &priv->audio_format);
        break;
    case VNC_AUDIO_PLAYBACK_DATA:
        vnc_audio_playback_data(priv->audio, priv->audio_sample);
        break;
    default:
        g_warn_if_reached();
    }

    coroutine_yieldto(data->caller, NULL);
    return FALSE;
}

static void vnc_connection_audio_action(VncConnection *conn,
                                        int action)
{
    struct audio_action_data data = {
        conn,
        coroutine_self(),
        action,
    };

    VNC_DEBUG("Emit audio action %d\n", action);

    g_idle_add(do_vnc_connection_audio_action, &data);

    /* This switches to the system coroutine context, lets
     * the idle function run to dispatch the action, and
     * finally returns once complete. ie this is synchronous
     * from the POV of the VNC coroutine despite there being
     * an idle function involved
     */
    coroutine_yield(NULL);
}


static gboolean vnc_connection_server_message(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 msg;
    int ret;

    if (vnc_connection_has_error(conn))
        return !vnc_connection_has_error(conn);

    /* NB: make sure that all server message functions
       handle has_error appropriately */

    do {
        if (priv->xmit_buffer_size) {
            vnc_connection_write(conn, priv->xmit_buffer, priv->xmit_buffer_size);
            vnc_connection_flush(conn);
            priv->xmit_buffer_size = 0;
        }
    } while ((ret = vnc_connection_read_u8_interruptable(conn, &msg)) == -EAGAIN);

    if (ret < 0) {
        VNC_DEBUG("Aborting message processing on error");
        return !vnc_connection_has_error(conn);
    }

    switch (msg) {
    case VNC_CONNECTION_SERVER_MESSAGE_FRAMEBUFFER_UPDATE: {
        guint8 pad[1];
        guint16 n_rects;
        int i;

        vnc_connection_read(conn, pad, 1);
        n_rects = vnc_connection_read_u16(conn);
        for (i = 0; i < n_rects; i++) {
            guint16 x, y, w, h;
            gint32 etype;

            x = vnc_connection_read_u16(conn);
            y = vnc_connection_read_u16(conn);
            w = vnc_connection_read_u16(conn);
            h = vnc_connection_read_u16(conn);
            etype = vnc_connection_read_s32(conn);

            if (!vnc_connection_framebuffer_update(conn, etype, x, y, w, h))
                break;
        }
    }        break;
    case VNC_CONNECTION_SERVER_MESSAGE_SET_COLOR_MAP_ENTRIES: {
        guint16 first_color;
        guint16 n_colors;
        guint8 pad[1];
        VncColorMap *map;
        int i;

        vnc_connection_read(conn, pad, 1);
        first_color = vnc_connection_read_u16(conn);
        n_colors = vnc_connection_read_u16(conn);

        VNC_DEBUG("Colour map from %d with %d entries",
                  first_color, n_colors);
        map = vnc_color_map_new(first_color, n_colors);

        for (i = 0; i < n_colors; i++) {
            guint16 red, green, blue;

            red = vnc_connection_read_u16(conn);
            green = vnc_connection_read_u16(conn);
            blue = vnc_connection_read_u16(conn);

            vnc_color_map_set(map,
                              i + first_color,
                              red, green, blue);
        }

        vnc_framebuffer_set_color_map(priv->fb, map);
        vnc_color_map_free(map);
    }        break;
    case VNC_CONNECTION_SERVER_MESSAGE_BELL:
        vnc_connection_bell(conn);
        break;
    case VNC_CONNECTION_SERVER_MESSAGE_SERVER_CUT_TEXT: {
        guint8 pad[3];
        guint32 n_text;
        char *data;

        vnc_connection_read(conn, pad, 3);
        n_text = vnc_connection_read_u32(conn);
        if (n_text > (32 << 20)) {
            vnc_connection_set_error(conn, "Cut text length %u longer than permitted %d",
                                     n_text, (32 << 20));
            break;
        }

        data = g_new(char, n_text + 1);
        vnc_connection_read(conn, data, n_text);
        data[n_text] = 0;

        vnc_connection_server_cut_text(conn, data, n_text);
        g_free(data);
    }        break;
    case VNC_CONNECTION_SERVER_MESSAGE_QEMU: {
        guint8  n_type;

        n_type = vnc_connection_read_u8(conn);

        if (priv->coroutine_stop)
            break;

        switch (n_type) {
        case VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO: {
            guint16 n_subtype;
            guint32 n_length;

            n_subtype = vnc_connection_read_u16(conn);
            switch (n_subtype) {
            case VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO_DATA:
                n_length = vnc_connection_read_u32(conn);
                if (n_length > (1024*1024)) {
                    vnc_connection_set_error(conn,
                                             "Audio sample length %u longer than permitted %d",
                                             n_length, 1024 * 1024);
                    break;
                }
                if (priv->coroutine_stop)
                    break;

                if (!priv->audio) {
                    vnc_connection_set_error(conn, "%s",
                                             "No audio playback sink configured");
                    break;
                }
                if (priv->audio_sample &&
                    ((priv->audio_sample->capacity - priv->audio_sample->length) < n_length)) {
                    g_source_remove(priv->audio_timer);
                    vnc_connection_audio_action(conn, VNC_AUDIO_PLAYBACK_DATA);
                    vnc_audio_sample_free(priv->audio_sample);
                    priv->audio_sample = NULL;
                }
                if (!priv->audio_sample) {
                    priv->audio_sample = vnc_audio_sample_new(1024*1024);
                    priv->audio_timer = g_timeout_add(50,
                                                      vnc_connection_audio_timer,
                                                      conn);
                }

                vnc_connection_read(conn,
                                    priv->audio_sample->data + priv->audio_sample->length,
                                    n_length);
                priv->audio_sample->length += n_length;
                break;
            case VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO_START:
                if (priv->audio)
                    vnc_connection_audio_action(conn, VNC_AUDIO_PLAYBACK_START);
                else
                    vnc_connection_set_error(conn, "%s", "No audio sink configured");
                break;
            case VNC_CONNECTION_SERVER_MESSAGE_QEMU_AUDIO_STOP:
                if (priv->audio) {
                    if (priv->audio_sample) {
                        g_source_remove(priv->audio_timer);
                        vnc_connection_audio_action(conn, VNC_AUDIO_PLAYBACK_DATA);
                        vnc_audio_sample_free(priv->audio_sample);
                        priv->audio_sample = NULL;
                    }
                    vnc_connection_audio_action(conn, VNC_AUDIO_PLAYBACK_STOP);
                } else {
                    vnc_connection_set_error(conn, "%s", "No audio sink configured");
                }
                break;
            default:
                vnc_connection_set_error(conn, "Received unknown QEMU audio message: %u", (int)n_subtype);
                break;
            }
        }       break;
        default:
            vnc_connection_set_error(conn, "Received an unknown QEMU message: %u", n_type);
        }
    } break;
    default:
        vnc_connection_set_error(conn, "Received an unknown message: %u", msg);
        break;
    }

    return !vnc_connection_has_error(conn);
}

static gboolean vnc_connection_has_credentials(gpointer data)
{
    VncConnection *conn = data;
    VncConnectionPrivate *priv = conn->priv;

    if (priv->coroutine_stop)
        return TRUE;
    if (priv->want_cred_username && !priv->cred_username)
        return FALSE;
    if (priv->want_cred_password && !priv->cred_password)
        return FALSE;
    /*
     * For x509 we require a minimum of the CA cert
     * if using GNUTLS < 3.0. With newer GNUTLS we'll
     * fallback to the system trust, so don't need to
     * explicitly check for a CA cert.
     */
#if GNUTLS_VERSION_NUMBER < 0x030000
    if (priv->want_cred_x509 && !priv->cred_x509_cacert)
        return FALSE;
#endif
    return TRUE;
}

static gboolean vnc_connection_gather_credentials(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->coroutine_stop)
        return FALSE;

    if (!vnc_connection_has_credentials(conn)) {
        GValueArray *authCred;
        GValue username, password, clientname;
        struct signal_data sigdata;

        memset(&username, 0, sizeof(username));
        memset(&password, 0, sizeof(password));
        memset(&clientname, 0, sizeof(clientname));

        authCred = g_value_array_new(0);
        if (priv->want_cred_username) {
            g_value_init(&username, VNC_TYPE_CONNECTION_CREDENTIAL);
            g_value_set_enum(&username, VNC_CONNECTION_CREDENTIAL_USERNAME);
            authCred = g_value_array_append(authCred, &username);
        }
        if (priv->want_cred_password) {
            g_value_init(&password, VNC_TYPE_CONNECTION_CREDENTIAL);
            g_value_set_enum(&password, VNC_CONNECTION_CREDENTIAL_PASSWORD);
            authCred = g_value_array_append(authCred, &password);
        }
        if (priv->want_cred_x509) {
            g_value_init(&clientname, VNC_TYPE_CONNECTION_CREDENTIAL);
            g_value_set_enum(&clientname, VNC_CONNECTION_CREDENTIAL_CLIENTNAME);
            authCred = g_value_array_append(authCred, &clientname);
        }

        sigdata.params.authCred = authCred;
        VNC_DEBUG("Requesting missing credentials");
        vnc_connection_emit_main_context(conn, VNC_AUTH_CREDENTIAL, &sigdata);

        g_value_array_free(authCred);

        if (priv->coroutine_stop)
            return FALSE;
        VNC_DEBUG("Waiting for missing credentials");
        g_condition_wait(vnc_connection_has_credentials, conn);
        VNC_DEBUG("Got all credentials");
    }
    return !vnc_connection_has_error(conn);
}


static gboolean vnc_connection_check_auth_result(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    guint32 result;

    VNC_DEBUG("Checking auth result");
    result = vnc_connection_read_u32(conn);
    if (!result) {
        VNC_DEBUG("Success");
        return TRUE;
    }

    if (priv->minor >= 8) {
        guint32 len;
        char reason[1024];
        len = vnc_connection_read_u32(conn);
        if (len > (sizeof(reason)-1))
            return FALSE;
        vnc_connection_read(conn, reason, len);
        reason[len] = '\0';
        VNC_DEBUG("Fail %s", reason);
        if (!priv->coroutine_stop) {
            struct signal_data sigdata;
            sigdata.params.authReason = reason;
            vnc_connection_set_error(conn, "%s", reason);
            vnc_connection_emit_main_context(conn, VNC_AUTH_FAILURE, &sigdata);
        }
    } else {
        VNC_DEBUG("Fail auth no result");
        if (!priv->coroutine_stop) {
            struct signal_data sigdata;
            sigdata.params.authReason = "Unknown authentication failure";
            vnc_connection_set_error(conn, "%s", "Unknown authentication failure");
            vnc_connection_emit_main_context(conn, VNC_AUTH_FAILURE, &sigdata);
        }
    }
    return FALSE;
}

static gboolean vnc_connection_perform_auth_vnc(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    guint8 challenge[16];
    guint8 key[8];
    gsize keylen;

    VNC_DEBUG("Do Challenge");
    priv->want_cred_password = TRUE;
    priv->want_cred_username = FALSE;
    priv->want_cred_x509 = FALSE;
    if (!vnc_connection_gather_credentials(conn))
        return FALSE;

    if (!priv->cred_password)
        return FALSE;

    vnc_connection_read(conn, challenge, 16);

    memset(key, 0, sizeof(key));
    keylen = strlen(priv->cred_password);
    if (keylen > sizeof(key))
        keylen = sizeof(key);
    memcpy(key, priv->cred_password, keylen);

    deskey(key, EN0);
    des(challenge, challenge);
    des(challenge + 8, challenge + 8);

    vnc_connection_write(conn, challenge, 16);
    vnc_connection_flush(conn);
    return vnc_connection_check_auth_result(conn);
}

/*
 *   marscha@2006 - Martin Scharpf
 *   Encrypt bytes[length] in memory using key.
 *   Key has to be 8 bytes, length a multiple of 8 bytes.
 */
static void
vncEncryptBytes2(unsigned char *where, const int length, unsigned char *key)
{
    int i, j;
    deskey(key, EN0);
    for (i = 0; i< 8; i++)
        where[i] ^= key[i];
    des(where, where);
    for (i = 8; i < length; i += 8) {
        for (j = 0; j < 8; j++)
            where[i + j] ^= where[i + j - 8];
        des(where + i, where + i);
    }
}

static gboolean vnc_connection_perform_auth_mslogon(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    struct vnc_dh *dh;
    guchar gen[8], mod[8], resp[8], pub[8], key[8];
    gcry_mpi_t genmpi, modmpi, respmpi, pubmpi, keympi;
    guchar username[256], password[64];
    guint passwordLen, usernameLen;

    VNC_DEBUG("Do Challenge");
    priv->want_cred_password = TRUE;
    priv->want_cred_username = TRUE;
    priv->want_cred_x509 = FALSE;
    if (!vnc_connection_gather_credentials(conn))
        return FALSE;

    vnc_connection_read(conn, gen, sizeof(gen));
    vnc_connection_read(conn, mod, sizeof(mod));
    vnc_connection_read(conn, resp, sizeof(resp));

    genmpi = vnc_bytes_to_mpi(gen,sizeof(gen));
    modmpi = vnc_bytes_to_mpi(mod,sizeof(mod));
    respmpi = vnc_bytes_to_mpi(resp,sizeof(resp));

    dh = vnc_dh_new(genmpi, modmpi);

    pubmpi = vnc_dh_gen_secret(dh);
    vnc_mpi_to_bytes(pubmpi, pub, sizeof(pub));

    vnc_connection_write(conn, pub, sizeof(pub));

    keympi = vnc_dh_gen_key(dh, respmpi);
    vnc_mpi_to_bytes(keympi, key, sizeof(key));

    passwordLen = strlen(priv->cred_password);
    usernameLen = strlen(priv->cred_username);
    if (passwordLen > sizeof(password))
        passwordLen = sizeof(password);
    if (usernameLen > sizeof(username))
        usernameLen = sizeof(username);

    memset(password, 0, sizeof password);
    memset(username, 0, sizeof username);
    memcpy(password, priv->cred_password, passwordLen);
    memcpy(username, priv->cred_username, usernameLen);

    vncEncryptBytes2(username, sizeof(username), key);
    vncEncryptBytes2(password, sizeof(password), key);

    vnc_connection_write(conn, username, sizeof(username));
    vnc_connection_write(conn, password, sizeof(password));
    vnc_connection_flush(conn);

    gcry_mpi_release(genmpi);
    gcry_mpi_release(modmpi);
    gcry_mpi_release(respmpi);
    vnc_dh_free (dh);

    return vnc_connection_check_auth_result(conn);
}

static gboolean vnc_connection_perform_auth_ard(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    struct vnc_dh *dh;
    guchar gen[2], len[2];
    size_t keylen;
    guchar *mod, *resp, *pub, *key, *shared;
    gcry_mpi_t genmpi, modmpi, respmpi, pubmpi, keympi;
    guchar userpass[128], ciphertext[128];
    guint passwordLen, usernameLen;
    gcry_md_hd_t md5;
    gcry_cipher_hd_t aes;
    gcry_error_t error;

    VNC_DEBUG("Do Challenge");
    priv->want_cred_password = TRUE;
    priv->want_cred_username = TRUE;
    priv->want_cred_x509 = FALSE;
    if (!vnc_connection_gather_credentials(conn))
        return FALSE;

    vnc_connection_read(conn, gen, sizeof(gen));
    vnc_connection_read(conn, len, sizeof(len));

    keylen = 256*len[0] + len[1];
    mod = malloc(keylen);
    if (mod == NULL) {
        VNC_DEBUG("malloc failed\n");
        return FALSE;
    }
    resp = malloc(keylen);
    if (resp == NULL) {
        free(mod);
        VNC_DEBUG("malloc failed\n");
        return FALSE;
    }
    pub = malloc(keylen);
    if (pub == NULL) {
        free(resp);
        free(mod);
        VNC_DEBUG("malloc failed\n");
        return FALSE;
    }
    key = malloc(keylen);
    if (key == NULL) {
        free(pub);
        free(resp);
        free(mod);
        VNC_DEBUG("malloc failed\n");
        return FALSE;
    }

    vnc_connection_read(conn, mod, keylen);
    vnc_connection_read(conn, resp, keylen);

    genmpi = vnc_bytes_to_mpi(gen,sizeof(gen));
    modmpi = vnc_bytes_to_mpi(mod,keylen);
    respmpi = vnc_bytes_to_mpi(resp,keylen);

    dh = vnc_dh_new(genmpi, modmpi);

    pubmpi = vnc_dh_gen_secret(dh);
    vnc_mpi_to_bytes(pubmpi, pub, keylen);

    keympi = vnc_dh_gen_key(dh, respmpi);
    vnc_mpi_to_bytes(keympi, key, keylen);

    error=gcry_md_open(&md5, GCRY_MD_MD5, 0);
    if (gcry_err_code (error) != GPG_ERR_NO_ERROR) {
        VNC_DEBUG("gcry_md_open error: %s\n", gcry_strerror(error));
        free(pub);
        free(resp);
        free(mod);
        return FALSE;
    }
    gcry_md_write(md5, key, keylen);
    error=gcry_md_final(md5);
    if (gcry_err_code (error) != GPG_ERR_NO_ERROR) {
        VNC_DEBUG("gcry_md_final error: %s\n", gcry_strerror(error));
        free(pub);
        free(resp);
        free(mod);
        return FALSE;
    }
    shared = gcry_md_read(md5, GCRY_MD_MD5);

    passwordLen = strlen(priv->cred_password)+1;
    usernameLen = strlen(priv->cred_username)+1;
    if (passwordLen > sizeof(userpass)/2)
        passwordLen = sizeof(userpass)/2;
    if (usernameLen > sizeof(userpass)/2)
        usernameLen = sizeof(userpass)/2;

    gcry_randomize(userpass, sizeof(userpass), GCRY_STRONG_RANDOM);
    memcpy(userpass, priv->cred_username, usernameLen);
    memcpy(userpass+sizeof(userpass)/2, priv->cred_password, passwordLen);

    error=gcry_cipher_open(&aes, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_ECB, 0);
    if (gcry_err_code (error) != GPG_ERR_NO_ERROR) {
        VNC_DEBUG("gcry_cipher_open error: %s\n", gcry_strerror(error));
        free(pub);
        free(resp);
        free(mod);
        return FALSE;
    }
    error=gcry_cipher_setkey(aes, shared, 16);
    if (gcry_err_code (error) != GPG_ERR_NO_ERROR) {
        VNC_DEBUG("gcry_cipher_setkey error: %s\n", gcry_strerror(error));
        free(pub);
        free(resp);
        free(mod);
        return FALSE;
    }
    error=gcry_cipher_encrypt(aes, ciphertext, sizeof(ciphertext), userpass, sizeof(userpass));
    if (gcry_err_code (error) != GPG_ERR_NO_ERROR) {
        VNC_DEBUG("gcry_cipher_encrypt error: %s\n", gcry_strerror(error));
        free(pub);
        free(resp);
        free(mod);
        return FALSE;
    }

    vnc_connection_write(conn, ciphertext, sizeof(ciphertext));
    vnc_connection_write(conn, pub, keylen);
    vnc_connection_flush(conn);

    free(mod);
    free(resp);
    free(pub);
    free(key);
    gcry_md_close(md5);
    gcry_mpi_release(genmpi);
    gcry_mpi_release(modmpi);
    gcry_mpi_release(respmpi);
    vnc_dh_free (dh);

    return vnc_connection_check_auth_result(conn);
}


#ifdef HAVE_SASL
/*
 * NB, keep in sync with similar method in qemud/remote.c
 */
static char *vnc_connection_addr_to_string(GSocketAddress *addr)
{
    GInetSocketAddress *iaddr = G_INET_SOCKET_ADDRESS(addr);
    guint16 port;
    GInetAddress *host;
    gchar *hoststr;
    gchar *ret;

    host = g_inet_socket_address_get_address(iaddr);
    port = g_inet_socket_address_get_port(iaddr);
    hoststr = g_inet_address_to_string(host);

    ret = g_strdup_printf("%s;%hu", hoststr, port);
    g_free(hoststr);

    return ret;
}



static gboolean
vnc_connection_gather_sasl_credentials(VncConnection *conn,
                                       sasl_interact_t *interact)
{
    VncConnectionPrivate *priv = conn->priv;
    int ninteract;

    priv->want_cred_password = FALSE;
    priv->want_cred_username = FALSE;
    priv->want_cred_x509 = FALSE;

    for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
        switch (interact[ninteract].id) {
        case SASL_CB_AUTHNAME:
        case SASL_CB_USER:
            priv->want_cred_username = TRUE;
            break;

        case SASL_CB_PASS:
            priv->want_cred_password = TRUE;
            break;

        default:
            VNC_DEBUG("Unsupported credential %lu",
                      interact[ninteract].id);
            /* Unsupported */
            return FALSE;
        }
    }

    if ((priv->want_cred_password ||
         priv->want_cred_username) &&
        !vnc_connection_gather_credentials(conn)) {
        VNC_DEBUG("%s", "cannot gather sasl credentials");
        return FALSE;
    }

    for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
        switch (interact[ninteract].id) {
        case SASL_CB_AUTHNAME:
        case SASL_CB_USER:
            interact[ninteract].result = priv->cred_username;
            interact[ninteract].len = strlen(priv->cred_username);
            VNC_DEBUG("Gather Username %s", priv->cred_username);
            break;

        case SASL_CB_PASS:
            interact[ninteract].result =  priv->cred_password;
            interact[ninteract].len = strlen(priv->cred_password);
            //VNC_DEBUG("Gather Password %s", priv->cred_password);
            break;

        default:
            g_warn_if_reached();
        }
    }

    VNC_DEBUG("%s", "Filled SASL interact");

    return TRUE;
}



/*
 *
 * Init msg from server
 *
 *  u32 mechlist-length
 *  u8-array mechlist-string
 *
 * Start msg to server
 *
 *  u32 mechname-length
 *  u8-array mechname-string
 *  u32 clientout-length
 *  u8-array clientout-string
 *
 * Start msg from server
 *
 *  u32 serverin-length
 *  u8-array serverin-string
 *  u8 continue
 *
 * Step msg to server
 *
 *  u32 clientout-length
 *  u8-array clientout-string
 *
 * Step msg from server
 *
 *  u32 serverin-length
 *  u8-array serverin-string
 *  u8 continue
 */

#define SASL_MAX_MECHLIST_LEN 300
#define SASL_MAX_DATA_LEN (1024 * 1024)

/* Perform the SASL authentication process
 */
static gboolean vnc_connection_perform_auth_sasl(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    sasl_conn_t *saslconn = NULL;
    sasl_security_properties_t secprops;
    const char *clientout;
    char *serverin = NULL;
    unsigned int clientoutlen, serverinlen;
    int err, complete;
    char *localAddr = NULL, *remoteAddr = NULL;
    const void *val;
    sasl_ssf_t ssf;
    sasl_callback_t saslcb[] = {
        { .id = SASL_CB_AUTHNAME },
        //                { .id = SASL_CB_USER },
        { .id = SASL_CB_PASS },
        { .id = 0 },
    };
    sasl_interact_t *interact = NULL;
    guint32 mechlistlen;
    char *mechlist;
    const char *mechname;
    gboolean ret;
    GSocketAddress *addr;

    /* Sets up the SASL library as a whole */
    err = sasl_client_init(NULL);
    VNC_DEBUG("Client initialize SASL authentication %d", err);
    if (err != SASL_OK) {
        vnc_connection_set_error(conn,
                                 "failed to initialize SASL library: %d (%s)",
                                 err, sasl_errstring(err, NULL, NULL));
        goto error;
    }

    /* Get local address in form  IPADDR:PORT */
    addr = g_socket_get_local_address(priv->sock, NULL);
    if (!addr) {
        vnc_connection_set_error(conn, "%s", "failed to get local address");
        goto error;
    }
    if ((g_socket_address_get_family(addr) == G_SOCKET_FAMILY_IPV4 ||
         g_socket_address_get_family(addr) == G_SOCKET_FAMILY_IPV6) &&
        (localAddr = vnc_connection_addr_to_string(addr)) == NULL) {
        vnc_connection_set_error(conn, "%s", "Unable to format address as string");
        goto error;
    }

    /* Get remote address in form  IPADDR:PORT */
    addr = g_socket_get_remote_address(priv->sock, NULL);
    if (!addr) {
        vnc_connection_set_error(conn, "%s", "failed to get peer address");
        goto error;
    }
    if ((g_socket_address_get_family(addr) == G_SOCKET_FAMILY_IPV4 ||
         g_socket_address_get_family(addr) == G_SOCKET_FAMILY_IPV6) &&
        (remoteAddr = vnc_connection_addr_to_string(addr)) == NULL) {
        vnc_connection_set_error(conn, "%s", "Unable to format address as string");
        goto error;
    }

    VNC_DEBUG("Client SASL new host:'%s' local:'%s' remote:'%s'", priv->host, localAddr, remoteAddr);

    /* Setup a handle for being a client */
    err = sasl_client_new("vnc",
                          priv->host,
                          localAddr,
                          remoteAddr,
                          saslcb,
                          SASL_SUCCESS_DATA,
                          &saslconn);
    g_free(localAddr);
    g_free(remoteAddr);

    if (err != SASL_OK) {
        vnc_connection_set_error(conn,
                                 "Failed to create SASL client context: %d (%s)",
                                 err, sasl_errstring(err, NULL, NULL));
        goto error;
    }

    /* Initialize some connection props we care about */
    if (priv->tls_session) {
        gnutls_cipher_algorithm_t cipher;

        cipher = gnutls_cipher_get(priv->tls_session);
        if (!(ssf = (sasl_ssf_t)gnutls_cipher_get_key_size(cipher))) {
            vnc_connection_set_error(conn, "%s",
                                     "invalid cipher size for TLS session");
            goto error;
        }
        ssf *= 8; /* key size is bytes, sasl wants bits */

        VNC_DEBUG("Setting external SSF %d", (int)ssf);
        err = sasl_setprop(saslconn, SASL_SSF_EXTERNAL, &ssf);
        if (err != SASL_OK) {
            vnc_connection_set_error(conn,
                                     "cannot set external SSF %d (%s)",
                                     err, sasl_errstring(err, NULL, NULL));
            goto error;
        }
    }

    memset (&secprops, 0, sizeof secprops);
    /* If we've got TLS, we don't care about SSF */
    secprops.min_ssf = priv->tls_session ? 0 : 56; /* Equiv to DES supported by all Kerberos */
    secprops.max_ssf = priv->tls_session ? 0 : 100000; /* Very strong ! AES == 256 */
    secprops.maxbufsize = 100000;
    /* If we're not TLS, then forbid any anonymous or trivially crackable auth */
    secprops.security_flags = priv->tls_session ? 0 :
        SASL_SEC_NOANONYMOUS | SASL_SEC_NOPLAINTEXT;

    err = sasl_setprop(saslconn, SASL_SEC_PROPS, &secprops);
    if (err != SASL_OK) {
        vnc_connection_set_error(conn,
                                 "cannot set security props %d (%s)",
                                 err, sasl_errstring(err, NULL, NULL));
        goto error;
    }

    /* Get the supported mechanisms from the server */
    mechlistlen = vnc_connection_read_u32(conn);
    if (priv->coroutine_stop)
        goto error;
    if (mechlistlen > SASL_MAX_MECHLIST_LEN) {
        vnc_connection_set_error(conn,
                                 "mechlistlen %u too long",
                                 mechlistlen);
        goto error;
    }

    mechlist = g_malloc(mechlistlen+1);
    vnc_connection_read(conn, mechlist, mechlistlen);
    mechlist[mechlistlen] = '\0';
    if (priv->coroutine_stop) {
        g_free(mechlist);
        mechlist = NULL;
        goto error;
    }

 restart:
    /* Start the auth negotiation on the client end first */
    VNC_DEBUG("Client start negotiation mechlist '%s'", mechlist);
    err = sasl_client_start(saslconn,
                            mechlist,
                            &interact,
                            &clientout,
                            &clientoutlen,
                            &mechname);
    if (err != SASL_OK && err != SASL_CONTINUE && err != SASL_INTERACT) {
        vnc_connection_set_error(conn,
                                 "Failed to start SASL negotiation: %d (%s)",
                                 err, sasl_errdetail(saslconn));
        g_free(mechlist);
        mechlist = NULL;
        goto error;
    }

    /* Need to gather some credentials from the client */
    if (err == SASL_INTERACT) {
        if (!vnc_connection_gather_sasl_credentials(conn,
                                                    interact)) {
            vnc_connection_set_error(conn, "%s",
                                     "Failed to collect auth credentials");
            goto error;
        }
        goto restart;
    }

    VNC_DEBUG("Server start negotiation with mech %s. Data %u bytes %p '%s'",
              mechname, clientoutlen, clientout, clientout);

    if (clientoutlen > SASL_MAX_DATA_LEN) {
        vnc_connection_set_error(conn,
                                 "SASL negotiation data too long: %u bytes",
                                 clientoutlen);
        goto error;
    }

    /* Send back the chosen mechname */
    vnc_connection_write_u32(conn, strlen(mechname));
    vnc_connection_write(conn, mechname, strlen(mechname));

    /* NB, distinction of NULL vs "" is *critical* in SASL */
    if (clientout) {
        vnc_connection_write_u32(conn, clientoutlen + 1);
        vnc_connection_write(conn, clientout, clientoutlen + 1);
    } else {
        vnc_connection_write_u32(conn, 0);
    }
    vnc_connection_flush(conn);
    if (priv->coroutine_stop)
        goto error;


    VNC_DEBUG("%s", "Getting sever start negotiation reply");
    /* Read the 'START' message reply from server */
    serverinlen = vnc_connection_read_u32(conn);
    if (priv->coroutine_stop)
        goto error;
    if (serverinlen > SASL_MAX_DATA_LEN) {
        vnc_connection_set_error(conn,
                                 "SASL negotiation data too long: %u bytes",
                                 clientoutlen);
        goto error;
    }

    /* NB, distinction of NULL vs "" is *critical* in SASL */
    if (serverinlen) {
        serverin = g_malloc(serverinlen);
        vnc_connection_read(conn, serverin, serverinlen);
        serverin[serverinlen-1] = '\0';
        serverinlen--;
    } else {
        serverin = NULL;
    }
    complete = vnc_connection_read_u8(conn);
    if (priv->coroutine_stop)
        goto error;

    VNC_DEBUG("Client start result complete: %d. Data %u bytes %p '%s'",
              complete, serverinlen, serverin, serverin);

    /* Previous server call showed completion & sasl_client_start() told us
     * we are locally complete too */
    if (complete && err == SASL_OK)
        goto done;

    /* Loop-the-loop...
     * Even if the server has completed, the client must *always* do at least one step
     * in this loop to verify the server isn't lying about something. Mutual auth */
    for (;;) {
    restep:
        err = sasl_client_step(saslconn,
                               serverin,
                               serverinlen,
                               &interact,
                               &clientout,
                               &clientoutlen);
        if (err != SASL_OK && err != SASL_CONTINUE && err != SASL_INTERACT) {
            vnc_connection_set_error(conn,
                                     "Failed SASL step: %d (%s)",
                                     err, sasl_errdetail(saslconn));
            goto error;
        }

        /* Need to gather some credentials from the client */
        if (err == SASL_INTERACT) {
            if (!vnc_connection_gather_sasl_credentials(conn,
                                                        interact)) {
                vnc_connection_set_error(conn, "%s",
                                         "Failed to collect auth credentials");
                goto error;
            }
            goto restep;
        }

        if (serverin) {
            g_free(serverin);
            serverin = NULL;
        }

        VNC_DEBUG("Client step result %d. Data %u bytes %p '%s'",
                  err, clientoutlen, clientout, clientout);

        /* Previous server call showed completion & we're now locally complete too */
        if (complete && err == SASL_OK)
            break;

        /* Not done, prepare to talk with the server for another iteration */

        /* NB, distinction of NULL vs "" is *critical* in SASL */
        if (clientout) {
            vnc_connection_write_u32(conn, clientoutlen + 1);
            vnc_connection_write(conn, clientout, clientoutlen + 1);
        } else {
            vnc_connection_write_u32(conn, 0);
        }
        vnc_connection_flush(conn);
        if (priv->coroutine_stop)
            goto error;

        VNC_DEBUG("Server step with %u bytes %p", clientoutlen, clientout);

        serverinlen = vnc_connection_read_u32(conn);
        if (priv->coroutine_stop)
            goto error;
        if (serverinlen > SASL_MAX_DATA_LEN) {
            vnc_connection_set_error(conn,
                                     "SASL negotiation data too long: %u bytes",
                                     clientoutlen);
            goto error;
        }

        /* NB, distinction of NULL vs "" is *critical* in SASL */
        if (serverinlen) {
            serverin = g_malloc(serverinlen);
            vnc_connection_read(conn, serverin, serverinlen);
            serverin[serverinlen-1] = '\0';
            serverinlen--;
        } else {
            serverin = NULL;
        }
        complete = vnc_connection_read_u8(conn);
        if (priv->coroutine_stop)
            goto error;

        VNC_DEBUG("Client step result complete: %d. Data %u bytes %p '%s'",
                  complete, serverinlen, serverin, serverin);

        /* This server call shows complete, and earlier client step was OK */
        if (complete && err == SASL_OK) {
            g_free(serverin);
            serverin = NULL;
            break;
        }
    }

 done:
    /* Check for suitable SSF if non-TLS */
    if (!priv->tls_session) {
        err = sasl_getprop(saslconn, SASL_SSF, &val);
        if (err != SASL_OK) {
            vnc_connection_set_error(conn,
                                     "cannot query SASL ssf on connection %d (%s)",
                                     err, sasl_errstring(err, NULL, NULL));
            goto error;
        }
        ssf = *(const int *)val;
        VNC_DEBUG("SASL SSF value %d", (int)ssf);
        if (ssf < 56) { /* 56 == DES level, good for Kerberos */
            vnc_connection_set_error(conn,
                                     "negotiated SSF %d was not strong enough",
                                     (int)ssf);
            goto error;
        }
    }

    VNC_DEBUG("%s", "SASL authentication complete");
    ret = vnc_connection_check_auth_result(conn);
    /* This must come *after* check-auth-result, because the former
     * is defined to be sent unencrypted, and setting saslconn turns
     * on the SSF layer encryption processing */
    priv->saslconn = saslconn;
    return ret;

 error:
    if (saslconn)
        sasl_dispose(&saslconn);
    return FALSE;
}
#endif /* HAVE_SASL */


static gboolean vnc_connection_start_tls(VncConnection *conn, int anonTLS)
{
    VncConnectionPrivate *priv = conn->priv;
    const char *priority = anonTLS ? TLS_PRIORITY ":+ANON-DH" : TLS_PRIORITY;
    int ret;

    VNC_DEBUG("Do TLS handshake");
    if (vnc_connection_tls_initialize() < 0) {
        vnc_connection_set_error(conn, "%s", "Failed to init TLS");
        return FALSE;
    }
    if (priv->tls_session == NULL) {
        if (gnutls_init(&priv->tls_session, GNUTLS_CLIENT) < 0) {
            vnc_connection_set_error(conn, "%s", "Failed to allocate client session");
            return FALSE;
        }

        if (gnutls_priority_set_direct(priv->tls_session, priority, NULL) < 0) {
            gnutls_deinit(priv->tls_session);
            vnc_connection_set_error(conn, "%s", "Failed to set priority");
            return FALSE;
        }

        if (anonTLS) {
            gnutls_anon_client_credentials anon_cred = vnc_connection_tls_initialize_anon_cred();
            if (!anon_cred) {
                gnutls_deinit(priv->tls_session);
                vnc_connection_set_error(conn, "%s", "Failed to allocate credentials");
                return FALSE;
            }
            if (gnutls_credentials_set(priv->tls_session, GNUTLS_CRD_ANON, anon_cred) < 0) {
                gnutls_deinit(priv->tls_session);
                vnc_connection_set_error(conn, "%s", "Failed to initialize credentials");
                return FALSE;
            }
        } else {
            priv->want_cred_password = FALSE;
            priv->want_cred_username = FALSE;
            priv->want_cred_x509 = TRUE;
            if (!vnc_connection_gather_credentials(conn))
                return FALSE;

            gnutls_certificate_credentials_t x509_cred = vnc_connection_tls_initialize_cert_cred(conn);
            if (!x509_cred) {
                gnutls_deinit(priv->tls_session);
                vnc_connection_set_error(conn, "%s", "Failed to allocate credentials");
                return FALSE;
            }
            if (gnutls_credentials_set(priv->tls_session, GNUTLS_CRD_CERTIFICATE, x509_cred) < 0) {
                gnutls_deinit(priv->tls_session);
                vnc_connection_set_error(conn, "%s", "Failed to initialize credentials");
                return FALSE;
            }
        }

        gnutls_transport_set_ptr(priv->tls_session, (gnutls_transport_ptr_t)conn);
        gnutls_transport_set_push_function(priv->tls_session, vnc_connection_tls_push);
        gnutls_transport_set_pull_function(priv->tls_session, vnc_connection_tls_pull);
    }

 retry:
    if ((ret = gnutls_handshake(priv->tls_session)) < 0) {
        if (!gnutls_error_is_fatal(ret)) {
            VNC_DEBUG("Handshake was blocking");
            if (!gnutls_record_get_direction(priv->tls_session))
                g_io_wait(priv->sock, G_IO_IN);
            else
                g_io_wait(priv->sock, G_IO_OUT);
            goto retry;
        }
        gnutls_deinit(priv->tls_session);
        priv->tls_session = NULL;
        vnc_connection_set_error(conn, "Failed to complete handshake %s",
                                  gnutls_strerror(ret));
        return FALSE;
    }

    VNC_DEBUG("Handshake done");

    if (anonTLS) {
        return TRUE;
    } else {
        if (!vnc_connection_validate_certificate(conn)) {
            return FALSE;
        }
        return TRUE;
    }
}

static gboolean vnc_connection_has_auth_subtype(gpointer data)
{
    VncConnection *conn = data;
    VncConnectionPrivate *priv = conn->priv;

    if (priv->coroutine_stop)
        return TRUE;
    if (priv->auth_subtype == VNC_CONNECTION_AUTH_INVALID)
        return FALSE;
    return TRUE;
}

static void vnc_connection_choose_auth(VncConnection *conn,
                                       int signum,
                                       unsigned int ntypes,
                                       unsigned int *types)
{
    VncConnectionPrivate *priv = conn->priv;
    struct signal_data sigdata;
    GValueArray *authTypes;
    GValue authType;

    authTypes = g_value_array_new(0);

    for (int i = 0 ; i < ntypes ; i++) {
        memset(&authType, 0, sizeof(authType));

        if (signum == VNC_AUTH_CHOOSE_TYPE) {
            g_value_init(&authType, VNC_TYPE_CONNECTION_AUTH);
        } else {
            if (priv->auth_type == VNC_CONNECTION_AUTH_VENCRYPT)
                g_value_init(&authType, VNC_TYPE_CONNECTION_AUTH_VENCRYPT);
            else
                g_value_init(&authType, VNC_TYPE_CONNECTION_AUTH);
        }
        g_value_set_enum(&authType, types[i]);
        authTypes = g_value_array_append(authTypes, &authType);
    }

    sigdata.params.authCred = authTypes;
    vnc_connection_emit_main_context(conn, signum, &sigdata);
    g_value_array_free(authTypes);
}

static gboolean vnc_connection_perform_auth_tls(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    unsigned int nauth, i;
    unsigned int auth[20];

    if (!vnc_connection_start_tls(conn, 1)) {
        VNC_DEBUG("Could not start TLS");
        return FALSE;
    }
    VNC_DEBUG("Completed TLS setup");

    nauth = vnc_connection_read_u8(conn);
    if (vnc_connection_has_error(conn))
        return FALSE;

    VNC_DEBUG("Got %u subauths", nauth);
    if (nauth == 0) {
        VNC_DEBUG("No sub-auth types requested");
        return vnc_connection_check_auth_result(conn);
    }

    if (nauth > sizeof(auth)) {
        vnc_connection_set_error(conn, "Too many (%u) auth types", nauth);
        return FALSE;
    }
    for (i = 0 ; i < nauth ; i++) {
        auth[i] = vnc_connection_read_u8(conn);
    }

    for (i = 0 ; i < nauth ; i++) {
        VNC_DEBUG("Possible TLS sub-auth %u", auth[i]);
    }

    if (priv->coroutine_stop)
        return FALSE;
    vnc_connection_choose_auth(conn, VNC_AUTH_CHOOSE_SUBTYPE, nauth, auth);
    if (priv->coroutine_stop)
        return FALSE;

    VNC_DEBUG("Waiting for TLS auth subtype");
    g_condition_wait(vnc_connection_has_auth_subtype, conn);
    if (priv->coroutine_stop)
        return FALSE;

    VNC_DEBUG("Choose auth %u", priv->auth_subtype);

    vnc_connection_write_u8(conn, priv->auth_subtype);
    vnc_connection_flush(conn);

    switch (priv->auth_subtype) {
    case VNC_CONNECTION_AUTH_NONE:
        if (priv->minor == 8)
            return vnc_connection_check_auth_result(conn);
        return TRUE;
    case VNC_CONNECTION_AUTH_VNC:
        return vnc_connection_perform_auth_vnc(conn);
#ifdef HAVE_SASL
    case VNC_CONNECTION_AUTH_SASL:
        return vnc_connection_perform_auth_sasl(conn);
#endif
    default:
        vnc_connection_set_error(conn, "Auth subtype %u is not supported",
                                 priv->auth_subtype);
        return FALSE;
    }

    return TRUE;
}

static gboolean vnc_connection_perform_auth_vencrypt(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    int major, minor, status, anonTLS;
    unsigned int nauth, i;
    unsigned int auth[20];

    major = vnc_connection_read_u8(conn);
    minor = vnc_connection_read_u8(conn);

    if (major != 0 &&
        minor != 2) {
        vnc_connection_set_error(conn, "Unsupported VeNCrypt version %d %d", major, minor);
        return FALSE;
    }

    vnc_connection_write_u8(conn, major);
    vnc_connection_write_u8(conn, minor);
    vnc_connection_flush(conn);
    status = vnc_connection_read_u8(conn);
    if (status != 0) {
        vnc_connection_set_error(conn, "Server refused VeNCrypt version %d %d", major, minor);
        return FALSE;
    }

    nauth = vnc_connection_read_u8(conn);
    if (nauth > (sizeof(auth)/sizeof(auth[0]))) {
        vnc_connection_set_error(conn, "Too many (%u) auth types", nauth);
        return FALSE;
    }

    for (i = 0 ; i < nauth ; i++) {
        auth[i] = vnc_connection_read_u32(conn);
    }

    for (i = 0 ; i < nauth ; i++) {
        VNC_DEBUG("Possible VeNCrypt sub-auth %u", auth[i]);
    }

    if (priv->coroutine_stop)
        return FALSE;
    vnc_connection_choose_auth(conn, VNC_AUTH_CHOOSE_SUBTYPE, nauth, auth);
    if (priv->coroutine_stop)
        return FALSE;

    VNC_DEBUG("Waiting for VeNCrypt auth subtype");
    g_condition_wait(vnc_connection_has_auth_subtype, conn);
    if (priv->coroutine_stop)
        return FALSE;

    VNC_DEBUG("Choose auth %u", priv->auth_subtype);

    if (!vnc_connection_gather_credentials(conn))
        return FALSE;

#ifndef DEBUG
    if (priv->auth_subtype == VNC_CONNECTION_AUTH_VENCRYPT_PLAIN) {
        vnc_connection_set_error(conn, "%s",
                                 "Cowardly refusing to transmit plain text password");
        return FALSE;
    }
#endif

    vnc_connection_write_u32(conn, priv->auth_subtype);
    vnc_connection_flush(conn);
    status = vnc_connection_read_u8(conn);
    if (status != 1) {
        vnc_connection_set_error(conn,
                                 "Server refused VeNCrypt auth %u %d", priv->auth_subtype, status);
        return FALSE;
    }

    switch (priv->auth_subtype) {
    case VNC_CONNECTION_AUTH_VENCRYPT_TLSNONE:
    case VNC_CONNECTION_AUTH_VENCRYPT_TLSPLAIN:
    case VNC_CONNECTION_AUTH_VENCRYPT_TLSVNC:
    case VNC_CONNECTION_AUTH_VENCRYPT_TLSSASL:
        anonTLS = 1;
        break;
    default:
        anonTLS = 0;
    }

    if (!vnc_connection_start_tls(conn, anonTLS)) {
        return FALSE;
    }
    VNC_DEBUG("Completed TLS setup, do subauth %u", priv->auth_subtype);

    switch (priv->auth_subtype) {
        /* Plain certificate based auth */
    case VNC_CONNECTION_AUTH_VENCRYPT_TLSNONE:
    case VNC_CONNECTION_AUTH_VENCRYPT_X509NONE:
        VNC_DEBUG("Completing auth");
        return vnc_connection_check_auth_result(conn);

        /* Regular VNC layered over TLS */
    case VNC_CONNECTION_AUTH_VENCRYPT_TLSVNC:
    case VNC_CONNECTION_AUTH_VENCRYPT_X509VNC:
        VNC_DEBUG("Handing off to VNC auth");
        return vnc_connection_perform_auth_vnc(conn);

#ifdef HAVE_SASL
        /* SASL layered over TLS */
    case VNC_CONNECTION_AUTH_VENCRYPT_TLSSASL:
    case VNC_CONNECTION_AUTH_VENCRYPT_X509SASL:
        VNC_DEBUG("Handing off to SASL auth");
        return vnc_connection_perform_auth_sasl(conn);
#endif

    default:
        vnc_connection_set_error(conn, "Unknown auth subtype %u", priv->auth_subtype);
        return FALSE;
    }
}

static gboolean vnc_connection_has_auth_type(gpointer data)
{
    VncConnection *conn = data;
    VncConnectionPrivate *priv = conn->priv;

    if (priv->coroutine_stop)
        return TRUE;
    if (priv->auth_type == VNC_CONNECTION_AUTH_INVALID)
        return FALSE;
    return TRUE;
}

static gboolean vnc_connection_perform_auth(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    unsigned int nauth, i;
    unsigned int auth[10];

    if (priv->minor <= 6) {
        nauth = 1;
        auth[0] = vnc_connection_read_u32(conn);
    } else {
        nauth = vnc_connection_read_u8(conn);
        if (vnc_connection_has_error(conn))
            return FALSE;

        if (nauth == 0)
            return vnc_connection_check_auth_result(conn);

        if (nauth > sizeof(auth)) {
            vnc_connection_set_error(conn, "Too many auth types %u",
                                     nauth);
            return FALSE;
        }
        for (i = 0 ; i < nauth ; i++)
            auth[i] = vnc_connection_read_u8(conn);
    }

    for (i = 0 ; i < nauth ; i++) {
        VNC_DEBUG("Possible auth %u", auth[i]);
    }

    if (priv->coroutine_stop)
        return FALSE;
    vnc_connection_choose_auth(conn, VNC_AUTH_CHOOSE_TYPE, nauth, auth);
    if (priv->coroutine_stop)
        return FALSE;

    VNC_DEBUG("Waiting for auth type");
    g_condition_wait(vnc_connection_has_auth_type, conn);
    if (priv->coroutine_stop)
        return FALSE;

    VNC_DEBUG("Choose auth %u", priv->auth_type);
    if (!vnc_connection_gather_credentials(conn))
        return FALSE;

    if (priv->minor > 6) {
        vnc_connection_write_u8(conn, priv->auth_type);
        vnc_connection_flush(conn);
    }

    switch (priv->auth_type) {
    case VNC_CONNECTION_AUTH_NONE:
        if (priv->minor == 8)
            return vnc_connection_check_auth_result(conn);
        return TRUE;
    case VNC_CONNECTION_AUTH_VNC:
        return vnc_connection_perform_auth_vnc(conn);

    case VNC_CONNECTION_AUTH_TLS:
        if (priv->minor < 7) {
            vnc_connection_set_error(conn, "%s", "TLS auth requires protocol 3.8");
            return FALSE;
        }
        return vnc_connection_perform_auth_tls(conn);

    case VNC_CONNECTION_AUTH_VENCRYPT:
        return vnc_connection_perform_auth_vencrypt(conn);

#ifdef HAVE_SASL
    case VNC_CONNECTION_AUTH_SASL:
        return vnc_connection_perform_auth_sasl(conn);
#endif

    case VNC_CONNECTION_AUTH_MSLOGON:
        return vnc_connection_perform_auth_mslogon(conn);

    case VNC_CONNECTION_AUTH_ARD:
        return vnc_connection_perform_auth_ard(conn);

    default:
        {
            struct signal_data sigdata;
            sigdata.params.authUnsupported = priv->auth_type;
            vnc_connection_emit_main_context(conn, VNC_AUTH_UNSUPPORTED, &sigdata);
        }
        vnc_connection_set_error(conn, "Unsupported auth type %u", priv->auth_type);
        return FALSE;
    }

    return TRUE;
}

static void vnc_connection_finalize (GObject *object)
{
    VncConnection *conn = VNC_CONNECTION(object);
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Finalize VncConnection=%p", conn);

    if (vnc_connection_is_open(conn))
        vnc_connection_close(conn);

    if (priv->cursor)
        g_object_unref(G_OBJECT(priv->cursor));

    if (priv->fb)
        g_object_unref(G_OBJECT(priv->fb));

    if (priv->audio)
        g_object_unref(G_OBJECT(priv->audio));
    if (priv->audio_sample)
        g_object_unref(G_OBJECT(priv->audio_sample));
    if (priv->audio_timer)
        g_source_remove(priv->audio_timer);

    G_OBJECT_CLASS(vnc_connection_parent_class)->finalize (object);
}

static void vnc_connection_class_init(VncConnectionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = vnc_connection_finalize;
    object_class->get_property = vnc_connection_get_property;
    object_class->set_property = vnc_connection_set_property;

    g_object_class_install_property(object_class,
                                    PROP_FRAMEBUFFER,
                                    g_param_spec_object("framebuffer",
                                                        "The desktop framebuffer",
                                                        "The desktop framebuffer instance",
                                                        VNC_TYPE_FRAMEBUFFER,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_NICK |
                                                        G_PARAM_STATIC_BLURB));

    signals[VNC_CURSOR_CHANGED] =
        g_signal_new ("vnc-cursor-changed",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_cursor_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE,
                      1,
                      VNC_TYPE_CURSOR);

    signals[VNC_POINTER_MODE_CHANGED] =
        g_signal_new ("vnc-pointer-mode-changed",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_pointer_mode_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__BOOLEAN,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_BOOLEAN);

    signals[VNC_BELL] =
        g_signal_new ("vnc-bell",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_bell),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE,
                      0);

    signals[VNC_SERVER_CUT_TEXT] =
        g_signal_new ("vnc-server-cut-text",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_server_cut_text),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_STRING);

    signals[VNC_FRAMEBUFFER_UPDATE] =
        g_signal_new ("vnc-framebuffer-update",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_framebuffer_update),
                      NULL, NULL,
                      g_cclosure_user_marshal_VOID__INT_INT_INT_INT,
                      G_TYPE_NONE,
                      4,
                      G_TYPE_INT,
                      G_TYPE_INT,
                      G_TYPE_INT,
                      G_TYPE_INT);

    signals[VNC_DESKTOP_RESIZE] =
        g_signal_new ("vnc-desktop-resize",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_desktop_resize),
                      NULL, NULL,
                      g_cclosure_user_marshal_VOID__INT_INT,
                      G_TYPE_NONE,
                      2,
                      G_TYPE_INT,
                      G_TYPE_INT);

    signals[VNC_PIXEL_FORMAT_CHANGED] =
        g_signal_new ("vnc-pixel-format-changed",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_pixel_format_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_POINTER);

    signals[VNC_LED_STATE] =
        g_signal_new ("vnc-led-state",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_led_state),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__INT,
                      G_TYPE_NONE,
                      0);

    signals[VNC_AUTH_FAILURE] =
        g_signal_new ("vnc-auth-failure",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_failure),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_STRING);


    signals[VNC_AUTH_UNSUPPORTED] =
        g_signal_new ("vnc-auth-unsupported",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_unsupported),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__UINT,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_UINT);

    signals[VNC_AUTH_CREDENTIAL] =
        g_signal_new ("vnc-auth-credential",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_credential),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__BOXED,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_VALUE_ARRAY);

    signals[VNC_AUTH_CHOOSE_TYPE] =
        g_signal_new ("vnc-auth-choose-type",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_choose_type),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__BOXED,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_VALUE_ARRAY);

    signals[VNC_AUTH_CHOOSE_SUBTYPE] =
        g_signal_new ("vnc-auth-choose-subtype",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_choose_subtype),
                      NULL, NULL,
                      g_cclosure_user_marshal_VOID__UINT_BOXED,
                      G_TYPE_NONE,
                      2,
                      G_TYPE_UINT,
                      G_TYPE_VALUE_ARRAY);


    signals[VNC_CONNECTED] =
        g_signal_new ("vnc-connected",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_connected),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE,
                      0);
    signals[VNC_INITIALIZED] =
        g_signal_new ("vnc-initialized",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_initialized),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE,
                      0);
    signals[VNC_DISCONNECTED] =
        g_signal_new ("vnc-disconnected",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_disconnected),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE,
                      0);
    signals[VNC_ERROR] =
        g_signal_new ("vnc-error",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (VncConnectionClass, vnc_error),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_STRING);


    g_type_class_add_private(klass, sizeof(VncConnectionPrivate));
}


static void vnc_connection_init(VncConnection *conn)
{
    VncConnectionPrivate *priv;

    VNC_DEBUG("Init VncConnection=%p", conn);

    priv = conn->priv = VNC_CONNECTION_GET_PRIVATE(conn);

    memset(priv, 0, sizeof(*priv));

    priv->fd = -1;
    priv->auth_type = VNC_CONNECTION_AUTH_INVALID;
    priv->auth_subtype = VNC_CONNECTION_AUTH_INVALID;
}


/**
 * vnc_connection_new:
 *
 * Create a new connection object, which is initially
 * in the disconnected state.
 *
 * Returns: (transfer full): the new connection
 */
VncConnection *vnc_connection_new(void)
{
    return VNC_CONNECTION(g_object_new(VNC_TYPE_CONNECTION,
                                       NULL));
}

static void vnc_connection_close(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    int i;

    VNC_DEBUG("Close VncConnection=%p", conn);

    if (priv->tls_session) {
        gnutls_bye(priv->tls_session, GNUTLS_SHUT_RDWR);
        priv->tls_session = NULL;
    }
#ifdef HAVE_SASL
    if (priv->saslconn) {
        sasl_dispose (&priv->saslconn);
        priv->saslconn = NULL;
        priv->saslDecodedOffset = priv->saslDecodedLength = 0;
    }
#endif

    if (priv->sock) {
        g_object_unref(priv->sock);
        priv->sock = NULL;
    }
    if (priv->addr) {
        g_object_unref(priv->addr);
        priv->addr = NULL;
    }
    if (priv->fd != -1)
        priv->fd = -1;

    if (priv->host) {
        g_free(priv->host);
        priv->host = NULL;
    }

    if (priv->port) {
        g_free(priv->port);
        priv->port = NULL;
    }

    if (priv->name) {
        g_free(priv->name);
        priv->name = NULL;
    }

    if (priv->xmit_buffer) {
        g_free(priv->xmit_buffer);
        priv->xmit_buffer = NULL;
        priv->xmit_buffer_size = 0;
        priv->xmit_buffer_capacity = 0;
    }

    priv->read_offset = priv->read_size = 0;
    priv->write_offset = 0;
    priv->uncompressed_offset = 0;
    priv->uncompressed_size = 0;
    priv->compressed_length = 0;

    priv->width = priv->height = 0;
    priv->major = priv->minor = 0;

    if (priv->cred_username) {
        g_free(priv->cred_username);
        priv->cred_username = NULL;
    }
    if (priv->cred_password) {
        g_free(priv->cred_password);
        priv->cred_password = NULL;
    }

    if (priv->cred_x509_cacert) {
        g_free(priv->cred_x509_cacert);
        priv->cred_x509_cacert = NULL;
    }
    if (priv->cred_x509_cacrl) {
        g_free(priv->cred_x509_cacrl);
        priv->cred_x509_cacrl = NULL;
    }
    if (priv->cred_x509_cert) {
        g_free(priv->cred_x509_cert);
        priv->cred_x509_cert = NULL;
    }
    if (priv->cred_x509_key) {
        g_free(priv->cred_x509_key);
        priv->cred_x509_key = NULL;
    }

    priv->want_cred_x509 = priv->want_cred_username =
        priv->want_cred_password = FALSE;

    for (i = 0; i < 5; i++)
        inflateEnd(&priv->streams[i]);

    priv->auth_type = VNC_CONNECTION_AUTH_INVALID;
    priv->auth_subtype = VNC_CONNECTION_AUTH_INVALID;
    priv->sharedFlag = FALSE;

    memset(&priv->fmt, 0, sizeof(priv->fmt));

    g_free(priv->error);
    priv->error = NULL;
    priv->coroutine_stop = FALSE;
}


/**
 * vnc_connection_shutdown:
 * @conn: (transfer none): the connection object
 *
 * Initiate a shutdown of the current connection
 * by closing its socket
 *
 */
void vnc_connection_shutdown(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Shutdown VncConnection=%p", conn);

    if (priv->open_id) {
        g_source_remove(priv->open_id);
        priv->open_id = 0;
    }

    priv->fd = -1;
    priv->coroutine_stop = TRUE;
    VNC_DEBUG("Waking up coroutine to shutdown gracefully");
    g_io_wakeup(&priv->wait);

    /* Closing the socket triggers an I/O error in the
     * event loop resulting...eventually.. in a call
     * to vnc_connection_close for full cleanup
     */
    if (priv->sock)
        g_socket_close(priv->sock, NULL);
}


/**
 * vnc_connection_is_open:
 * @conn: (transfer none): the connection object
 *
 * Check if the connection is currently open
 *
 * Returns: TRUE if open, FALSE if closing/closed
 */
gboolean vnc_connection_is_open(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    if (priv->fd != -1)
        return TRUE;
    if (priv->sock != NULL)
        return TRUE;
    if (priv->host)
        return TRUE;
    if (priv->addr)
        return TRUE;
    return FALSE;
}


/**
 * vnc_connection_is_initialized:
 * @conn: (transfer none): the connection object
 *
 * Determine if the connection to the remote desktop is
 * fully initialized and thus receiving framebuffer
 * updates.
 *
 * Returns: TRUE if initialized, FALSE if closed or still negotiating
 */
gboolean vnc_connection_is_initialized(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    if (!vnc_connection_is_open(conn))
        return FALSE;
    if (priv->name)
        return TRUE;
    return FALSE;
}


static gboolean vnc_connection_before_version (VncConnection *conn, int major, int minor)
{
    VncConnectionPrivate *priv = conn->priv;

    return (priv->major < major) || (priv->major == major && priv->minor < minor);
}


static gboolean vnc_connection_after_version (VncConnection *conn, int major, int minor)
{
    return !vnc_connection_before_version (conn, major, minor+1);
}


static gboolean vnc_connection_initialize(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    int ret, i;
    char version[13];
    guint32 n_name;

    priv->absPointer = TRUE;

    vnc_connection_read(conn, version, 12);
    if (vnc_connection_has_error(conn)) {
        VNC_DEBUG("Error while reading server version");
        goto fail;
    }

    version[12] = 0;

    ret = sscanf(version, "RFB %03d.%03d\n", &priv->major, &priv->minor);
    if (ret != 2) {
        vnc_connection_set_error(conn, "%s",
                                 "Error while parsing server version");
        goto fail;
    }

    VNC_DEBUG("Server version: %d.%d", priv->major, priv->minor);

    if (vnc_connection_before_version(conn, 3, 3)) {
        vnc_connection_set_error(conn,
                                 "Server version is not supported (%d.%d)", priv->major, priv->minor);
        goto fail;
    } else if (vnc_connection_before_version(conn, 3, 7)) {
        priv->minor = 3;
    } else if (vnc_connection_after_version(conn, 3, 8)) {
        priv->major = 3;
        priv->minor = 8;
    }

    snprintf(version, 13, "RFB %03d.%03d\n", priv->major, priv->minor);
    vnc_connection_write(conn, version, 12);
    vnc_connection_flush(conn);
    VNC_DEBUG("Using version: %d.%d", priv->major, priv->minor);

    if (!vnc_connection_perform_auth(conn)) {
        VNC_DEBUG("Auth failed");
        goto fail;
    }

    vnc_connection_write_u8(conn, priv->sharedFlag);
    vnc_connection_flush(conn);
    priv->width = vnc_connection_read_u16(conn);
    priv->height = vnc_connection_read_u16(conn);

    if (vnc_connection_has_error(conn))
        return FALSE;

    VNC_DEBUG("Initial desktop size %dx%d", priv->width, priv->height);

    vnc_connection_read_pixel_format(conn, &priv->fmt);

    n_name = vnc_connection_read_u32(conn);
    if (n_name > 4096) {
        vnc_connection_set_error(conn, "Name length %u too long",
                                 n_name);
        goto fail;
    }

    priv->name = g_new(char, n_name + 1);

    vnc_connection_read(conn, priv->name, n_name);
    priv->name[n_name] = 0;
    VNC_DEBUG("Display name '%s'", priv->name);

    if (vnc_connection_has_error(conn))
        return FALSE;

    memset(&priv->strm, 0, sizeof(priv->strm));
    /* FIXME what level? */
    for (i = 0; i < 5; i++)
        inflateInit(&priv->streams[i]);
    priv->strm = NULL;

    return !vnc_connection_has_error(conn);

 fail:
    return !vnc_connection_has_error(conn);
}


static gboolean vnc_connection_open_fd_internal(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Connecting to FD %d", priv->fd);

    if (!(priv->sock = g_socket_new_from_fd(priv->fd, NULL))) {
        VNC_DEBUG("Failed to open socket from fd %d", priv->fd);
        return FALSE;
    }

    g_socket_set_blocking(priv->sock, FALSE);

    return !vnc_connection_has_error(conn);
}

static gboolean connect_timeout(gpointer data)
{
    struct wait_queue *wait = data;

    g_io_wakeup(wait);

    return FALSE;
}

static GSocket *vnc_connection_connect_socket(struct wait_queue *wait,
                                              GSocketAddress *sockaddr,
                                              GError **error)
{
    GSocket *sock = g_socket_new(g_socket_address_get_family(sockaddr),
                                 G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_DEFAULT,
                                 error);

    if (!sock)
        return NULL;

    guint timeout = g_timeout_add_seconds(10, connect_timeout, wait);

    g_socket_set_blocking(sock, FALSE);
    if (!g_socket_connect(sock, sockaddr, NULL, error)) {
        if (*error && (*error)->code == G_IO_ERROR_PENDING) {
            g_error_free(*error);
            *error = NULL;
            VNC_DEBUG("Socket pending");
            if (!g_io_wait_interruptable(wait, sock, G_IO_OUT|G_IO_ERR|G_IO_HUP)) {
                VNC_DEBUG("connect interrupted");
                timeout = 0;
                goto timeout;
            }

            if (!g_socket_check_connect_result(sock, error))
                goto error;
        } else
            goto error;
    }

    VNC_DEBUG("Finally connected");
    goto end;

error:
    VNC_DEBUG("Socket error: %s", *error ? (*error)->message : "unknown");

timeout:
    g_object_unref(sock);
    sock = NULL;

end:
    if (timeout != 0)
        g_source_remove(timeout);

    return sock;
}

static gboolean vnc_connection_open_addr_internal(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    GError *conn_error = NULL;
    GSocket *sock = NULL;

    VNC_DEBUG("Connecting with addr %p", priv->addr);

    sock = vnc_connection_connect_socket(&priv->wait, priv->addr, &conn_error);
    vnc_connection_set_error(conn, "Unable to connect: %s",
                             conn_error->message);
    g_clear_error(&conn_error);
    if (sock) {
        priv->sock = sock;
        return TRUE;
    }
    return FALSE;
}


static gboolean vnc_connection_open_host_internal(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;
    GSocketConnectable *addr;
    GSocketAddressEnumerator *enumerator;
    GSocketAddress *sockaddr;
    GError *conn_error = NULL;
    GSocket *sock = NULL;
    int port = atoi(priv->port);

    VNC_DEBUG("Resolving host %s %s", priv->host, priv->port);

    addr = g_network_address_new(priv->host, port);

    enumerator = g_socket_connectable_enumerate (addr);
    g_object_unref (addr);

    /* Try each sockaddr until we succeed. Record the first
     * connection error, but not any further ones (since they'll probably
     * be basically the same as the first).
     */
    while (!sock &&
           (sockaddr = g_socket_address_enumerator_next(enumerator, NULL, &conn_error))) {
        VNC_DEBUG("Trying one socket");
        g_clear_error(&conn_error);
        sock = vnc_connection_connect_socket(&priv->wait, sockaddr, &conn_error);
        g_object_unref(sockaddr);
    }
    g_object_unref(enumerator);
    if (!sock) {
        vnc_connection_set_error(conn, "Unable to connect: %s",
                                 conn_error->message);
    }
    g_clear_error(&conn_error);
    if (sock) {
        priv->sock = sock;
        return TRUE;
    }
    return FALSE;
}


/* we use an idle function to allow the coroutine to exit before we actually
 * unref the object since the coroutine's state is part of the object */
static gboolean vnc_connection_delayed_unref(gpointer data)
{
    VncConnection *conn = VNC_CONNECTION(data);
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Delayed unref VncConnection=%p", conn);

    g_assert(priv->coroutine.exited == TRUE);

    g_object_unref(G_OBJECT(data));

    return FALSE;
}

static void *vnc_connection_coroutine(void *opaque)
{
    VncConnection *conn = VNC_CONNECTION(opaque);
    VncConnectionPrivate *priv = conn->priv;
    int ret;
    struct signal_data s;

    VNC_DEBUG("Started background coroutine");

    if (priv->fd != -1) {
        if (!vnc_connection_open_fd_internal(conn))
            goto cleanup;
    } else if (priv->addr != NULL) {
        if (!vnc_connection_open_addr_internal(conn))
            goto cleanup;
    } else {
        if (!vnc_connection_open_host_internal(conn))
            goto cleanup;
    }

    vnc_connection_emit_main_context(conn, VNC_CONNECTED, &s);

    VNC_DEBUG("Protocol initialization");
    if (!vnc_connection_initialize(conn))
        goto cleanup;

    vnc_connection_emit_main_context(conn, VNC_INITIALIZED, &s);

    VNC_DEBUG("Running main loop");
    while ((ret = vnc_connection_server_message(conn)))
        ;

 cleanup:
    VNC_DEBUG("Doing final VNC cleanup");
    vnc_connection_close(conn);
    vnc_connection_emit_main_context(conn, VNC_DISCONNECTED, &s);
    g_idle_add(vnc_connection_delayed_unref, conn);
    /* Co-routine exits now - the VncDisplay object may no longer exist,
       so don't do anything else now unless you like SEGVs */
    return NULL;
}

static gboolean do_vnc_connection_open(gpointer data)
{
    VncConnection *conn = VNC_CONNECTION(data);
    VncConnectionPrivate *priv = conn->priv;
    struct coroutine *co;

    VNC_DEBUG("Open coroutine starting");
    priv->open_id = 0;

    co = &priv->coroutine;

    co->stack_size = 16 << 20;
    co->entry = vnc_connection_coroutine;
    co->release = NULL;

    coroutine_init(co);
    coroutine_yieldto(co, conn);

    return FALSE;
}


/**
 * vnc_connection_open_fd:
 * @conn: (transfer none): the connection object
 * @fd: file descriptor to use for the connection
 *
 * Open a connection using @fd as the transport. If @fd
 * refers to a TCP connection, it is recommended to use
 * vnc_connection_open_fd_with_hostname instead, to
 * provide the remote hostname. This allows use of
 * x509 based authentication which requires a hostname
 * to be available.
 *
 * Returns: TRUE if a connection was opened, FALSE if already open
 */
gboolean vnc_connection_open_fd(VncConnection *conn, int fd)
{
    return vnc_connection_open_fd_with_hostname(conn, fd, NULL);
}


/**
 * vnc_connection_open_fd_with_hostname:
 * @conn: (transfer none): the connection object
 * @fd: file descriptor to use for the connection
 * @hostname: (transfer none)(nullable): the host associated with the connection
 *
 * Open a connection using @fd as the transport. The
 * @hostname provided should reflect the name of the
 * host that the @fd provides a connection to. This
 * will be used by some authentication schemes, for
 * example x509 certificate validation against @hostname.
 *
 * Returns: TRUE if a connection was opened, FALSE if already open
 */
gboolean vnc_connection_open_fd_with_hostname(VncConnection *conn, int fd, const char *hostname)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Open fd=%d", fd);

    if (vnc_connection_is_open(conn))
        return FALSE;

    priv->fd = fd;
    priv->addr = NULL;
    priv->host = g_strdup(hostname ? hostname : "localhost");
    priv->port = g_strdup("");

    g_object_ref(G_OBJECT(conn)); /* Unref'd when co-routine exits */
    priv->open_id = g_idle_add(do_vnc_connection_open, conn);

    return TRUE;
}


/**
 * vnc_connection_open_host:
 * @conn: (transfer none): the connection object
 * @host: (transfer none): the host name or IP address
 * @port: (transfer none): the service name or port number
 *
 * Open a TCP connection to the remote desktop at @host
 * listening on @port.
 *
 * Returns: TRUE if a connection was opened, FALSE if already open
 */
gboolean vnc_connection_open_host(VncConnection *conn, const char *host, const char *port)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Open host=%s port=%s", host, port);

    if (vnc_connection_is_open(conn))
        return FALSE;

    priv->fd = -1;
    priv->addr = NULL;
    priv->host = g_strdup(host);
    priv->port = g_strdup(port);

    g_object_ref(G_OBJECT(conn)); /* Unref'd when co-routine exits */
    priv->open_id = g_idle_add(do_vnc_connection_open, conn);

    return TRUE;
}


/**
 * vnc_connection_open_addr:
 * @conn: (transfer none): the connection object
 * @addr: (transfer none): the socket address
 * @hostname: (transfer none)(nullable): the hostname
 *
 * Open a socket connection to server identified by @addr.
 * @addr may refer to either a TCP address (IPv4/6) or
 * a UNIX socket address. The @hostname provided should
 * reflect the name of the host that the @addr provides a
 * connection to, if it is not already available in @addr.
 * For example, if @addr points to a proxy server, then
 * @hostname can be used to provide the name of the final
 * endpoint. This will be used by some authentication
 * schemes, for example x509 certificate validation
 * against @hostname.
 *
 * Returns: TRUE if a connection was opened, FALSE if already open
 */
gboolean vnc_connection_open_addr(VncConnection *conn, GSocketAddress *addr, const char *hostname)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Open addr=%p", addr);

    if (vnc_connection_is_open(conn))
        return FALSE;

    priv->fd = -1;
    priv->addr = g_object_ref(addr);

    priv->host = g_strdup(hostname ? hostname : "localhost");
    if (G_IS_INET_SOCKET_ADDRESS(addr)) {
        guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(addr));
        priv->port = g_strdup_printf("%d", (int)port);
    } else {
        priv->port = g_strdup("");
    }

    g_object_ref(G_OBJECT(conn)); /* Unref'd when co-routine exits */
    priv->open_id = g_idle_add(do_vnc_connection_open, conn);

    return TRUE;
}


/**
 * vnc_connection_set_auth_type:
 * @conn: (transfer none): the connection object
 * @type: the requested auth type
 *
 * Set the authentication type to use to complete the
 * connection.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_auth_type(VncConnection *conn, unsigned int type)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Thinking about auth type %u", type);
    if (priv->auth_type != VNC_CONNECTION_AUTH_INVALID) {
        vnc_connection_set_error(conn, "%s", "Auth type has already been set");
        return !vnc_connection_has_error(conn);
    }
    if (type != VNC_CONNECTION_AUTH_NONE &&
        type != VNC_CONNECTION_AUTH_VNC &&
        type != VNC_CONNECTION_AUTH_MSLOGON &&
        type != VNC_CONNECTION_AUTH_ARD &&
        type != VNC_CONNECTION_AUTH_TLS &&
        type != VNC_CONNECTION_AUTH_VENCRYPT &&
        type != VNC_CONNECTION_AUTH_SASL) {
        vnc_connection_set_error(conn, "Auth type %u is not supported",
                                 type);
        g_signal_emit(conn, VNC_AUTH_UNSUPPORTED, 0, type);
        return !vnc_connection_has_error(conn);
    }
    VNC_DEBUG("Decided on auth type %u", type);
    priv->auth_type = type;
    priv->auth_subtype = VNC_CONNECTION_AUTH_INVALID;

    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_set_auth_subtype:
 * @conn: (transfer none): the connection object
 * @type: the auth sub-type
 *
 * If a multi-level authentication scheme was requested, this
 * identifies which auth type to use for the second phase.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_auth_subtype(VncConnection *conn, unsigned int type)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Requested auth subtype %u", type);
    if (priv->auth_type != VNC_CONNECTION_AUTH_VENCRYPT &&
        priv->auth_type != VNC_CONNECTION_AUTH_TLS) {
        vnc_connection_set_error(conn, "Auth type %u does not support subauth",
                                 priv->auth_type);
        return !vnc_connection_has_error(conn);
    }
    if (priv->auth_subtype != VNC_CONNECTION_AUTH_INVALID) {
        vnc_connection_set_error(conn, "%s", "Auth subtype has already been set");
        return !vnc_connection_has_error(conn);
    }
    priv->auth_subtype = type;

    return !vnc_connection_has_error(conn);
}


static int vnc_connection_best_path(char **buf,
                                    const char *basedir,
                                    const char *basefile,
                                    char **dirs,
                                    unsigned int ndirs)
{
    unsigned int i;
    gchar *tmp;
    for (i = 0 ; i < ndirs ; i++) {
        struct stat sb;
        tmp = g_strdup_printf("%s/%s/%s", dirs[i], basedir, basefile);
        if (stat(tmp, &sb) == 0) {
            *buf = tmp;
            return 0;
        }
        g_free(tmp);
    }
    VNC_DEBUG("Failed to find certificate %s/%s", basedir, basefile);
    return -1;
}



static gboolean vnc_connection_set_credential_x509(VncConnection *conn,
                                                   const gchar *name)
{
    VncConnectionPrivate *priv = conn->priv;
    char *sysdir = g_strdup_printf("%s/pki", SYSCONFDIR);
#ifndef WIN32
    struct passwd *pw;

    if (!(pw = getpwuid(getuid())))
        return TRUE;

    char *userdir = g_strdup_printf("%s/.pki", pw->pw_dir);
    char *dirs[] = { sysdir, userdir };
#else
    char *dirs[] = { sysdir };
#endif
    for (int i = 0 ; i < sizeof(dirs)/sizeof(dirs[0]) ; i++)
        VNC_DEBUG("Searching for certs in %s", dirs[i]);

    if (vnc_connection_best_path(&priv->cred_x509_cacert, "CA", "cacert.pem",
                                 dirs, sizeof(dirs)/sizeof(dirs[0])) < 0)
        return FALSE;

    /* Don't mind failures of CRL */
    vnc_connection_best_path(&priv->cred_x509_cacrl, "CA", "cacrl.pem",
                             dirs, sizeof(dirs)/sizeof(dirs[0]));

    /* Set client key & cert if we have them. Server will reject auth
     * if it decides it requires them*/
    vnc_connection_best_path(&priv->cred_x509_key, name, "private/clientkey.pem",
                             dirs, sizeof(dirs)/sizeof(dirs[0]));
    vnc_connection_best_path(&priv->cred_x509_cert, name, "clientcert.pem",
                             dirs, sizeof(dirs)/sizeof(dirs[0]));

    return TRUE;
}


/**
 * vnc_connection_set_credential:
 * @conn: (transfer none): the connection object
 * @type: the authentication credential type
 * @data: (transfer none): the value associated with the credential
 *
 * Sets the value of the authentication credential
 * @type to the string @data.
 *
 * @type is one of the VncConnectionCredential enum vlaues
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_credential(VncConnection *conn, int type, const gchar *data)
{
    VncConnectionPrivate *priv = conn->priv;

    VNC_DEBUG("Set credential %d %s", type, data);
    switch (type) {
    case VNC_CONNECTION_CREDENTIAL_PASSWORD:
        g_free(priv->cred_password);
        priv->cred_password = g_strdup(data);
        break;

    case VNC_CONNECTION_CREDENTIAL_USERNAME:
        g_free(priv->cred_username);
        priv->cred_username = g_strdup(data);
        break;

    case VNC_CONNECTION_CREDENTIAL_CLIENTNAME:
        g_free(priv->cred_x509_cacert);
        g_free(priv->cred_x509_cacrl);
        g_free(priv->cred_x509_key);
        g_free(priv->cred_x509_cert);
        return vnc_connection_set_credential_x509(conn, data);

    default:
        vnc_connection_set_error(conn, "Unknown credential type %d", type);
    }

    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_set_framebuffer:
 * @conn: (transfer none): the connection object
 * @fb: (transfer none): the framebuffer object
 *
 * Set the framebuffer object to which frame buffer updates
 * will be written.
 *
 * Returns: TRUE if the connection is ok, FALSE if it has an error
 */
gboolean vnc_connection_set_framebuffer(VncConnection *conn, VncFramebuffer *fb)
{
    VncConnectionPrivate *priv = conn->priv;
    const VncPixelFormat *remote;
    int i;

    VNC_DEBUG("Set framebuffer %p", fb);

    if (priv->fb)
        g_object_unref(G_OBJECT(priv->fb));
    priv->fb = fb;
    g_object_ref(G_OBJECT(priv->fb));

    remote = vnc_framebuffer_get_remote_format(priv->fb);

    priv->fbSwapRemote = remote->byte_order != G_BYTE_ORDER;

    i = priv->fmt.bits_per_pixel / 8;

    if (i == 4) i = 3;

    priv->rich_cursor_blt = vnc_connection_rich_cursor_blt_table[i - 1];
    priv->tight_compute_predicted = vnc_connection_tight_compute_predicted_table[i - 1];
    priv->tight_sum_pixel = vnc_connection_tight_sum_pixel_table[i - 1];

    return !vnc_connection_has_error(conn);
}


/**
 * vnc_connection_get_name:
 * @conn: (transfer none): the connection object
 *
 * Get the name of the remote display. A name will only
 * be available once the "vnc-initialized" signal has
 * been emitted
 *
 * Returns: (transfer none): the remote display name
 */
const char *vnc_connection_get_name(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->name;
}

/**
 * vnc_connection_get_width:
 * @conn: (transfer none): the connection object
 *
 * Get the width of the remote display. The width will
 * only be set once the "vnc-initialized" signal has
 * been emitted
 *
 * Returns: the desktop width
 */
int vnc_connection_get_width(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->width;
}

/**
 * vnc_connection_get_height:
 * @conn: (transfer none): the connection object
 *
 * Get the height of the remote display. The height will
 * only be set once the "vnc-initialized" signal has
 * been emitted
 *
 * Returns: the desktop height
 */
int vnc_connection_get_height(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->height;
}

/**
 * vnc_connection_get_ext_key_event:
 * @conn: (transfer none): the connection object
 *
 * Determine if the remote server supports the extended
 * keyboard event which transmits raw XT scancodes.
 * This will only be valid once the "vnc-initialized"
 * signal has been emitted
 *
 * Returns: TRUE if supported, FALSE otherwise
 */
gboolean vnc_connection_get_ext_key_event(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->has_ext_key_event;
}


/**
 * vnc_connection_get_cursor:
 * @conn: the VNC connection
 *
 * Get the cursor currently associated with the desktop,
 * if any.
 *
 * Returns: (transfer none): the cursor or NULL
 */
VncCursor *vnc_connection_get_cursor(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->cursor;
}


/**
 * vnc_connection_get_abs_pointer:
 * @conn: (transfer none): the connection object
 *
 * Determine if the remote server supports absolute pointer
 * motion events. This will only be valid once the
 * "vnc-initialized" signal has been emitted.
 *
 * Returns: TRUE if the server supports absolute pointer mode
 */
gboolean vnc_connection_get_abs_pointer(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->absPointer;
}

/**
 * vnc_connection_get_ledstate:
 * @conn: (transfer none): the connection object
 *
 * Get the current LED state bitmap. This is only
 * valid once the "vnc-initialized" signal has been
 * emitted.
 *
 * Returns: the LED state
 */
int vnc_connection_get_ledstate(VncConnection *conn)
{
    VncConnectionPrivate *priv = conn->priv;

    return priv->ledstate;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 */
