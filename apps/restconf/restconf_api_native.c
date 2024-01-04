/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  
  * Concrete functions for openssl of the
  * Virtual clixon restconf API functions.
  * @see restconf_api.h for virtual API
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifdef HAVE_LIBNGHTTP2
#include <nghttp2/nghttp2.h>
#endif

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include "restconf_lib.h"
#include "restconf_api.h"  /* Virtual api */
#include "restconf_native.h"

/*! Add HTTP header field name and value to reply
 *
 * @param[in]  req   request handle
 * @param[in]  name  HTTP header field name
 * @param[in]  vfmt  HTTP header field value format string w variable parameter
 * @retval     0     OK
 * @retval    -1     Error
 * @see eg RFC 7230
 */
int
restconf_reply_header(void       *req0,
                      const char *name,
                      const char *vfmt,
                      ...)
{
    int                   retval = -1;
    restconf_stream_data *sd = (restconf_stream_data *)req0;
    restconf_conn        *rc;
    size_t                vlen;
    char                 *value = NULL;
    va_list               ap;

    clixon_debug(CLIXON_DBG_CLIENT, "%s %s", __FUNCTION__, name);
    if (sd == NULL || name == NULL || vfmt == NULL){
        clixon_err(OE_CFG, EINVAL, "sd, name or value is NULL");
        goto done;
    }
    if ((rc = sd->sd_conn) == NULL){
        clixon_err(OE_CFG, EINVAL, "rc is NULL");
        goto done;
    }
    /* First round: compute vlen and allocate value */
    va_start(ap, vfmt);
    vlen = vsnprintf(NULL, 0, vfmt, ap);
    va_end(ap);
    /* allocate value string exactly fitting */
    if ((value = malloc(vlen+1)) == NULL){
        clixon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    /* Second round: compute actual value */
    va_start(ap, vfmt);
    if (vsnprintf(value, vlen+1, vfmt, ap) < 0){
        clixon_err(OE_UNIX, errno, "vsnprintf");
        va_end(ap);
        goto done;
    }
    va_end(ap);
    if (cvec_add_string(sd->sd_outp_hdrs, (char*)name, value) < 0){
        clixon_err(OE_RESTCONF, errno, "cvec_add_string");
        goto done;
    }
    retval = 0;
 done:
    if (value)
        free(value);
    return retval;
}

/*! Send HTTP reply with potential message body
 *
 * @param[in]  req   http request handle
 * @param[in]  code  Status code
 * @param[in]  cb    Body as a cbuf if non-NULL. Note: is consumed
 * @param[in]  head  Only send headers, dont send body. 
 * @retval     0     OK
 * @retval    -1     Error
 * Prerequisites: status code set, headers given, body if wanted set
 */
int
restconf_reply_send(void  *req0,
                    int    code,
                    cbuf  *cb,
                    int    head)
{
    int                   retval = -1;
    restconf_stream_data *sd = (restconf_stream_data *)req0;

    clixon_debug(CLIXON_DBG_CLIENT, "%s code:%d", __FUNCTION__, code);
    if (sd == NULL){
        clixon_err(OE_CFG, EINVAL, "sd is NULL");
        goto done;
    }
    sd->sd_code = code;
    if (cb != NULL){
        if (cbuf_len(cb)){
            sd->sd_body_len = cbuf_len(cb);
            if (head){
                cbuf_free(cb);
            }
            else{
                sd->sd_body = cb;
                sd->sd_body_offset = 0;
            }
        }
        else{
            cbuf_free(cb);
            sd->sd_body_len = 0;
        }
    }
    else
        sd->sd_body_len = 0;
    retval = 0;
 done:
    return retval;
}

/*! Get input data from http request, eg such as curl -X PUT http://... <indata>
 *
 * @param[in]  req        Request handle
 * @note: reuses cbuf from stream-data
 */
cbuf *
restconf_get_indata(void *req0)
{
    restconf_stream_data *sd = (restconf_stream_data *)req0;
    cbuf                 *cb = NULL;

    if (sd == NULL){
        clixon_err(OE_CFG, EINVAL, "sd is NULL");
        goto done;
    }
    cb = sd->sd_indata;
 done:
    return cb;
}

