/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stddef.h>

#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_connection.h>
#include <http_log.h>

#include <nghttp2/nghttp2.h>

#include "h2_private.h"
#include "h2_resp_head.h"
#include "h2_session.h"
#include "h2_stream.h"
#include "h2_task.h"
#include "h2_ctx.h"
#include "h2_frame.h"
#include "h2_task_input.h"


static void set_state(h2_stream *stream, h2_stream_state_t state)
{
    if (stream->state != state) {
        h2_stream_state_t oldstate = stream->state;
        stream->state = state;
        if (stream->state_change_cb) {
            stream->state_change_cb(stream, oldstate, stream->state_change_ctx);
        }
    }
}


apr_status_t h2_stream_create(h2_stream **pstream,
                              int id, h2_session *session)
{
    h2_stream *stream = apr_pcalloc(session->c->pool, sizeof(h2_stream));
    stream->id = id;
    stream->state = H2_STREAM_ST_IDLE;
    stream->eoh = 0;
    stream->session = session;
    
    *pstream = stream;
    return APR_SUCCESS;
}

apr_status_t h2_stream_destroy(h2_stream *stream)
{
    if (stream->work) {
        h2_bucket_destroy(stream->work);
        stream->work = NULL;
    }
    if (stream->resp_head) {
        h2_resp_head_destroy(stream->resp_head);
        stream->resp_head = NULL;
    }
    stream->session = NULL;
    return APR_SUCCESS;
}

void h2_stream_abort(h2_stream *stream)
{
    if (!stream->aborted) {
        stream->aborted = 1;
    }
}

void h2_stream_set_state_change_cb(h2_stream *stream,
                                   h2_stream_state_change_cb cb,
                                   void *cb_ctx)
{
    stream->state_change_cb = cb;
    stream->state_change_ctx = cb_ctx;
}

static apr_status_t h2_stream_check_work(h2_stream *stream)
{
    if (!stream->work) {
        stream->work = h2_bucket_alloc(16 * 1024);
        if (!stream->work) {
            return APR_ENOMEM;
        }
    }
    return APR_SUCCESS;
}

apr_status_t h2_stream_push(h2_stream *stream)
{
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, stream->session->c,
                  "h2_stream(%d-%d): pushing request: %s %s for %s",
                  stream->session->id, (int)stream->id,
                  stream->method, stream->path, stream->authority);
    
    apr_status_t status = h2_bucket_queue_append(stream->session->data_in,
                                                 stream->work, stream->id);
    if (status == APR_SUCCESS) {
        stream->work = NULL;
    }
    return status;
}

apr_status_t h2_stream_end_headers(h2_stream *stream)
{
    apr_status_t status = h2_stream_check_work(stream);
    if (status != APR_SUCCESS) {
        return status;
    }
    stream->eoh = 1;

    if (!h2_bucket_has_free(stream->work, 2)) {
        status = h2_stream_push(stream);
    }
    
    if (status == APR_SUCCESS) {
        h2_bucket_cat(stream->work, "\r\n");
        status = h2_stream_push(stream);
    }
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, status, stream->session->c,
                  "h2_stream(%d-%d): headers done",
                  stream->session->id, stream->id);
    return status;
}

apr_status_t h2_stream_close_input(h2_stream *stream)
{
    apr_status_t status = APR_SUCCESS;
    switch (stream->state) {
        case H2_STREAM_ST_CLOSED_INPUT:
        case H2_STREAM_ST_CLOSED:
            break; /* ignore, idempotent */
        case H2_STREAM_ST_CLOSED_OUTPUT:
            /* both closed now */
            set_state(stream, H2_STREAM_ST_CLOSED);
            break;
        default:
            /* everything else we jump to here */
            set_state(stream, H2_STREAM_ST_CLOSED_INPUT);
            break;
    }
    if (stream->work) {
        status = h2_stream_push(stream);
    }
    if (status == APR_SUCCESS) {
        status = h2_bucket_queue_append_eos(stream->session->data_in,
                                            stream->id);
    }
    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, status, stream->session->c,
                  "h2_stream(%d-%d): got eos",
                  stream->session->id, stream->id);
    return status;
}

apr_status_t h2_stream_add_header(h2_stream *stream,
                                  const char *name, size_t nlen,
                                  const char *value, size_t vlen)
{
    apr_status_t status = APR_SUCCESS;
    
    if (nlen <= 0) {
        return status;
    }
    
    if (name[0] == ':') {
        /* pseudo header, see ch. 8.1.2.3, always should come first */
        if (stream->work) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, stream->session->c,
                          "h2_stream(%d-%d): pseudo header after request start",
                          stream->session->id, stream->id);
            return APR_EGENERAL;
        }
        
        if (vlen <= 0) {
            char buffer[32];
            memset(buffer, 0, 32);
            strncpy(buffer, name, (nlen > 31)? 31 : nlen);
            ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, stream->session->c,
                          "h2_stream(%d-%d): pseudo header without value %s",
                          stream->session->id, stream->id, buffer);
            status = APR_EGENERAL;
        }
        else if (H2_HEADER_METHOD_LEN == nlen
                 && !strncmp(H2_HEADER_METHOD, name, nlen)) {
            stream->method = apr_pstrndup(stream->session->c->pool, value, vlen);
        }
        else if (H2_HEADER_SCHEME_LEN == nlen
                 && !strncmp(H2_HEADER_SCHEME, name, nlen)) {
            stream->scheme = apr_pstrndup(stream->session->c->pool, value, vlen);
        }
        else if (H2_HEADER_PATH_LEN == nlen
                 && !strncmp(H2_HEADER_PATH, name, nlen)) {
            stream->path = apr_pstrndup(stream->session->c->pool, value, vlen);
        }
        else if (H2_HEADER_AUTH_LEN == nlen
                 && !strncmp(H2_HEADER_AUTH, name, nlen)) {
            stream->authority = apr_pstrndup(stream->session->c->pool, value, vlen);
        }
        else {
            char buffer[32];
            memset(buffer, 0, 32);
            strncpy(buffer, name, (nlen > 31)? 31 : nlen);
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, stream->session->c,
                          "h2_stream(%d-%d): ignoring unknown pseudo header %s",
                          stream->session->id, stream->id, buffer);
        }
    }
    else {
        /* non-pseudo header, append to work bucket of stream */
        if (stream->work == NULL) {
            /* the first bucket of request data we generate for this stream.
             * we should have all mandatory pseudo headers now.
             */
            if (!stream->method) {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, stream->session->c,
                              "h2_stream(%d-%d): header start but :method missing",
                              stream->session->id, stream->id);
                return APR_EGENERAL;
            }
            if (!stream->path) {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, stream->session->c,
                              "h2_stream(%d-%d): header start but :path missing",
                              stream->session->id, stream->id);
                return APR_EGENERAL;
            }
            
            status = h2_stream_check_work(stream);
            if (status != APR_SUCCESS) {
                return status;
            }
            status = h2_frame_req_add_start(stream->work,
                                            stream->method, stream->path);
            if (status == APR_SUCCESS && stream->authority) {
                status = h2_frame_req_add_header(stream->work,
                                                 "Host", 4,
                                                 stream->authority,
                                                 strlen(stream->authority));
            }
        }
        
        if (status == APR_SUCCESS) {
            status = h2_frame_req_add_header(stream->work,
                                             name, nlen, value, vlen);
            if (status == APR_ENAMETOOLONG && stream->work->data_len > 0) {
                /* header did not fit into bucket, push bucket to input and
                 * get a new one */
                status = h2_stream_push(stream);
                if (status == APR_SUCCESS) {
                    status = h2_frame_req_add_header(stream->work,
                                                     name, nlen, value, vlen);
                    /* if this still does not work, we fail */
                }
            }
        }
    }
    
    return status;
}

apr_status_t h2_stream_add_data(h2_stream *stream,
                                const char *data, size_t len)
{
    apr_status_t status = h2_stream_check_work(stream);
    if (status != APR_SUCCESS) {
        return status;
    }
    
    while (len > 0) {
        apr_size_t written = h2_bucket_append(stream->work, data, len);
        if (written < len) {
            len -= written;
            data += written;
            apr_status_t status = h2_stream_push(stream);
            if (status != APR_SUCCESS) {
                return status;
            }
        }
        else {
            len = 0;
        }
    }
    return APR_SUCCESS;
}


