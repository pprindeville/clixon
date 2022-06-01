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
  
  * Restconf method implementation for post: operation(rpc) and data
  * From RFC 8040 Section 4.4.  POST
 */


#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/wait.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "restconf_lib.h"
#include "restconf_handle.h"
#include "restconf_api.h"
#include "restconf_err.h"
#include "restconf_methods_post.h"

/*! Print location header from 
 * @param[in]  req    Generic Www handle
 * @param[in]  xobj   If set (eg POST) add to api-path
 * $https  “on” if connection operates in SSL mode, or an empty string otherwise 
 * @note ports are ignored
 */
static int
http_location_header(clicon_handle h,
		     void         *req,
		     cxobj        *xobj)
{
    int   retval = -1;
    char *https;
    char *host;
    char *request_uri;
    cbuf *cb = NULL;

    https = restconf_param_get(h, "HTTPS");
    host = restconf_param_get(h, "HTTP_HOST");
    if ((request_uri = restconf_uripath(h)) == NULL)
	goto done;
    if (xobj != NULL){
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, 0, "cbuf_new");
	    goto done;
	}
	if (xml2api_path_1(xobj, cb) < 0)
	    goto done;
	if (restconf_reply_header(req, "Location", "http%s://%s%s%s",
				  https?"s":"",
				  host,
				  request_uri,
				  cbuf_get(cb)) < 0)
	    goto done;
    }
    else
	if (restconf_reply_header(req, "Location", "http%s://%s%s",
				  https?"s":"",
				  host,
				  request_uri) < 0)
	    goto done;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    if (request_uri)
	free(request_uri);
    return retval;
}

/*! Generic REST POST  method 
 * @param[in]  h        Clixon handle
 * @param[in]  req      Generic Www handle
 * @param[in]  api_path According to restconf (Sec 3.5.3.1 in rfc8040)
 * @param[in]  pcvec    Vector of path ie DOCUMENT_URI element
 * @param[in]  pi       Offset, where to start pcvec
 * @param[in]  qvec     Vector of query string (QUERY_STRING)
 * @param[in]  data     Stream input data
 * @param[in]  pretty   Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @param[in]  ds       0 if "data" resource, 1 if rfc8527 "ds" resource
 * @retval     0         OK
 * @retval    -1         Error
 * restconf POST is mapped to edit-config create. 
 * @see RFC8040 Sec 4.4.1

 POST:
   target resource type is datastore --> create a top-level resource
   target resource type is  data resource --> create child resource

   The message-body MUST contain exactly one instance of the
   expected data resource.  The data model for the child tree is the
   subtree, as defined by YANG for the child resource.

   If the POST method succeeds, a "201 Created" status-line is returned
   and there is no response message-body.  A "Location" header
   identifying the child resource that was created MUST be present in
   the response in this case.

   If the data resource already exists, then the POST request MUST fail
   and a "409 Conflict" status-line MUST be returned.

 * @see RFC8040 Section 4.4
 * @see api_data_put
 */
int
api_data_post(clicon_handle h,
	      void         *req,
	      char         *api_path, 
	      int           pi,
	      cvec         *qvec, 
	      char         *data,
	      int           pretty,
	      restconf_media media_in,
	      restconf_media media_out,
	      ietf_ds_t     ds)
{
    int            retval = -1;
    enum operation_type op = OP_CREATE;
    cxobj         *xdata = NULL; /* The actual data object to modify */
    int            i;
    cbuf          *cbx = NULL;
    cxobj         *xtop = NULL; /* top of api-path */
    cxobj         *xbot = NULL; /* bottom of api-path */
    yang_stmt     *ybot = NULL; /* yang of xbot */
    yang_stmt     *ymoddata = NULL; /* yang module of data (-d) */
    yang_stmt     *yspec;
    yang_stmt     *ydata;
    cxobj         *xa;
    cxobj         *xret = NULL;
    cxobj         *xretcom = NULL; /* return from commit */
    cxobj         *xretdis = NULL; /* return from discard-changes */
    cxobj         *xerr = NULL; /* malloced must be freed */
    cxobj         *xe;            /* dont free */
    cxobj         *x;            
    char          *username;
    int            ret;
    int            nrchildren0 = 0;
    yang_bind      yb;
    
    clicon_debug(1, "%s api_path:\"%s\"", __FUNCTION__, api_path);
    clicon_debug(1, "%s data:\"%s\"", __FUNCTION__, data);
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    for (i=0; i<pi; i++)
	api_path = index(api_path+1, '/');
    /* Create config top-of-tree */
    if ((xtop = xml_new(NETCONF_INPUT_CONFIG, NULL, CX_ELMNT)) == NULL)
	goto done;
    /* Translate api_path to xtop/xbot */
    xbot = xtop;
    if (api_path){
	/* Translate api-path to xml, side-effect: validate the api-path, note: strict=1 */
	if ((ret = api_path2xml(api_path, yspec, xtop, YC_DATANODE, 1, &xbot, &ybot, &xerr)) < 0)
	    goto done;
	if (ret == 0){ /* validation failed */
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    /* 4.4.1: The message-body MUST contain exactly one instance of the
     * expected data resource.  (tested again below)
     */
    if (data == NULL || strlen(data) == 0){
	if (netconf_malformed_message_xml(&xerr, "The message-body of POST MUST contain exactly one instance of the expected data resource") < 0)
	    goto done;
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }

    /* Record how many children before parse (after check nr should be +1) */
    nrchildren0 = 0;
    x = NULL;
    while ((x = xml_child_each(xbot, x, CX_ELMNT)) != NULL){
	nrchildren0++;
	xml_flag_set(x, XML_FLAG_MARK);
    }
    if (xml_spec(xbot)==NULL)
	yb = YB_MODULE;
    else
	yb = YB_PARENT;
    /* Parse input data as json or xml into xml 
     * If xbot is top-level (api_path=null) it does not have a spec therefore look for 
     * top-level (yspec) otherwise assume parent (xbot) is populated.
     */
    switch (media_in){
    case YANG_DATA_XML:
	if ((ret = clixon_xml_parse_string(data, yb, yspec, &xbot, &xerr)) < 0){
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
	if (ret == 0){
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
	break;
    case YANG_DATA_JSON:	
	if ((ret = clixon_json_parse_string(data, 1, yb, yspec, &xbot, &xerr)) < 0){
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
	if (ret == 0){
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
	break;
    default:
	restconf_unsupported_media(h, req, pretty, media_out);
	goto ok;
	break;
    } /* switch media_in */

    /* RFC 8040 4.4.1: The message-body MUST contain exactly one instance of the
     * expected data resource. 
     */
    clicon_debug(1, "%s nrchildren0: %d", __FUNCTION__, nrchildren0);
    if (xml_child_nr_type(xbot, CX_ELMNT) - nrchildren0 != 1){
	if (netconf_malformed_message_xml(&xerr, "The message-body MUST contain exactly one instance of the expected data resource") < 0)
	    goto done;
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Find the actual (new) object, the single unmarked one */
    x = NULL;
    while ((x = xml_child_each(xbot, x, CX_ELMNT)) != NULL){
	if (xml_flag(x, XML_FLAG_MARK)){ 
	    xml_flag_reset(x, XML_FLAG_MARK);
	    continue;
	}
	xdata = x;
    }

    /* Add operation (create/replace) as attribute */
    if ((xa = xml_new("operation", xdata, CX_ATTR)) == NULL)
	goto done;
    if (xml_value_set(xa, xml_operation2str(op)) < 0)
	goto done;
    if (xml_namespace_change(xa, NETCONF_BASE_NAMESPACE, NETCONF_BASE_PREFIX) < 0)
	goto done;

    if (ys_module_by_xml(yspec, xdata, &ymoddata) < 0)
	goto done;
    /* ybot is parent of spec(parent(data))) */
    if (ymoddata && (ydata = xml_spec(xdata)) != NULL){
	yang_stmt *ymod = NULL;
	if (ys_real_module(ydata, &ymod) < 0)
	    goto done;
	if (ymod != ymoddata){
	     if (netconf_malformed_message_xml(&xerr, "Data is not prefixed with matching namespace") < 0)
		 goto done;
	     if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		 goto done;
	     goto ok;
	}
	/* If URI points out an object, then data's parent should be that object 
	 */
	if (ybot && yang_parent_get(ydata) != ybot){
	     if (netconf_malformed_message_xml(&xerr, "Data is not prefixed with matching namespace") < 0)
		 goto done;
	     if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		 goto done;
	     goto ok;
	}
    }
    /* If restconf insert/point attributes are present, translate to netconf */
    if (restconf_insert_attributes(xdata, qvec) < 0)
	goto done;
#if 1
    if (clicon_debug_get())
	clicon_log_xml(LOG_DEBUG, xdata, "%s xdata:", __FUNCTION__);
#endif

    /* Create text buffer for transfer to backend */
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, 0, "cbuf_new");
	goto done;
    }
    /* For internal XML protocol: add username attribute for access control
     */
    username = clicon_username_get(h);
    cprintf(cbx, "<rpc xmlns=\"%s\" username=\"%s\" xmlns:%s=\"%s\" %s>",
	    NETCONF_BASE_NAMESPACE,
	    username?username:"",
	    NETCONF_BASE_PREFIX,
	    NETCONF_BASE_NAMESPACE,
	    NETCONF_MESSAGE_ID_ATTR); /* bind nc to netconf namespace */

    cprintf(cbx, "<edit-config");
    /* RFC8040 Sec 1.4:
     * If this is a "data" request and the NETCONF server supports :startup,
     * the RESTCONF server MUST automatically update the non-volatile startup
     * configuration datastore, after the "running" datastore has been altered
     * as a consequence of a RESTCONF edit operation.
     */
    if ((IETF_DS_NONE == ds) &&
	if_feature(yspec, "ietf-netconf", "startup") &&
	!clicon_option_bool(h, "CLICON_RESTCONF_STARTUP_DONTUPDATE")){
	cprintf(cbx, " copystartup=\"true\"");
    }
    cprintf(cbx, " autocommit=\"true\"");
    cprintf(cbx, "><target><candidate /></target>");
    cprintf(cbx, "<default-operation>none</default-operation>");
    if (clixon_xml2cbuf(cbx, xtop, 0, 0, -1, 0) < 0)
	goto done;
    cprintf(cbx, "</edit-config></rpc>");
    clicon_debug(1, "%s xml: %s api_path:%s",__FUNCTION__, cbuf_get(cbx), api_path);
    if (clicon_rpc_netconf(h, cbuf_get(cbx), &xret, NULL) < 0)
	goto done;
    if ((xe = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	if (api_return_err(h, req, xe, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if (http_location_header(h, req, xdata) < 0)
	goto done;
    if (restconf_reply_send(req, 201, NULL, 0) < 0)
	goto done;	
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xret)
	xml_free(xret);
    if (xerr)
	xml_free(xerr);
    if (xretcom)
	xml_free(xretcom);
    if (xretdis)
	xml_free(xretdis);
    if (xtop)
	xml_free(xtop);
     if (cbx)
	cbuf_free(cbx); 
   return retval;
} /* api_data_post */

/*! Handle input data to api_operations_post 
 * @param[in]  h      Clixon handle
 * @param[in]  req    Generic Www handle
 * @param[in]  data   Stream input data
 * @param[in]  yspec  Yang top-level specification 
 * @param[in]  yrpc   Yang rpc spec
 * @param[in]  xrpc   XML pointer to rpc method
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @retval     1      OK
 * @retval     0      Fail, Error message sent
 * @retval    -1      Fatal error, clicon_err called
 *
 * RFC8040 3.6.1
 *  If the "rpc" or "action" statement has an "input" section, then
 *  instances of these input parameters are encoded in the module
 *  namespace where the "rpc" or "action" statement is defined, in an XML
 *  element or JSON object named "input", which is in the module
 *  namespace where the "rpc" or "action" statement is defined.
 * (Any other input is assumed as error.)
 */
static int
api_operations_post_input(clicon_handle h,
			  void         *req,
			  char         *data,
			  yang_stmt    *yspec,
			  yang_stmt    *yrpc,
			  cxobj        *xrpc,
			  int           pretty,
			  restconf_media media_out)
{
    int        retval = -1;
    cxobj     *xdata = NULL;
    cxobj     *xerr = NULL; /* malloced must be freed */
    cxobj     *xinput;
    cxobj     *x;
    cbuf      *cbret = NULL;
    int        ret;
    restconf_media media_in;

    clicon_debug(1, "%s %s", __FUNCTION__, data);
    if ((cbret = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, 0, "cbuf_new");
	goto done;
    }
    /* Parse input data as json or xml into xml */
    media_in = restconf_content_type(h);
    switch (media_in){
    case YANG_DATA_XML:
	/* XXX: Here data is on the form: <input xmlns="urn:example:clixon"/> and has no proper yang binding 
	 * support */
	if ((ret = clixon_xml_parse_string(data, YB_NONE, yspec, &xdata, &xerr)) < 0){
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto fail;
	}
	if (ret == 0){
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto fail;
	}
	break;
    case YANG_DATA_JSON:
	/* XXX: Here data is on the form: {"clixon-example:input":null} and has no proper yang binding 
	 * support */
	if ((ret = clixon_json_parse_string(data, 1, YB_NONE, yspec, &xdata, &xerr)) < 0){
	    if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
		goto done;
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto fail;
	}
	if (ret == 0){
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto fail;
	}
	break;
    default:
	restconf_unsupported_media(h, req, pretty, media_out);
	goto fail;
	break;
    } /* switch media_in */
    xml_name_set(xdata, NETCONF_OUTPUT_DATA);
    /* Here xdata is: 
     * <data><input xmlns="urn:example:clixon">...</input></data>
     */
#if 1
    if (clicon_debug_get())
	clicon_log_xml(LOG_DEBUG, xdata, "%s xdata:", __FUNCTION__);
#endif
    /* Validate that exactly only <input> tag */
    if ((xinput = xml_child_i_type(xdata, 0, CX_ELMNT)) == NULL ||
	strcmp(xml_name(xinput),"input") != 0 ||
	xml_child_nr_type(xdata, CX_ELMNT) != 1){

	if (xml_child_nr_type(xdata, CX_ELMNT) == 0){
	    if (netconf_malformed_message_xml(&xerr, "restconf RPC does not have input statement") < 0)
		goto done;
	}
	else
	    if (netconf_malformed_message_xml(&xerr, "restconf RPC has malformed input statement (multiple or not called input)") < 0)
		goto done;	
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto fail;
    }
    //    clicon_debug(1, "%s input validation passed", __FUNCTION__);
    /* Add all input under <rpc>path */
    x = NULL;
    while ((x = xml_child_i_type(xinput, 0, CX_ELMNT)) != NULL)
	if (xml_addsub(xrpc, x) < 0) 	
	    goto done;
    /* Here xrpc is:  <myfn xmlns="uri"><x>42</x></myfn>
     */
    // ok:
    retval = 1;
 done:
    clicon_debug(1, "%s retval: %d", __FUNCTION__, retval);
    if (cbret)
	cbuf_free(cbret);
    if (xerr)
	xml_free(xerr);
    if (xdata)
	xml_free(xdata);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Handle output data to api_operations_post 
 * @param[in]  h        Clixon handle
 * @param[in]  req      Generic Www handle
 * @param[in]  xret     XML reply messages from backend/handler
 * @param[in]  yspec    Yang top-level specification 
 * @param[in]  youtput  Yang rpc output specification
 * @param[in]  pretty Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @param[out] xoutputp Restconf JSON/XML output
 * @retval     1        OK
 * @retval     0        Fail, Error message sent
 * @retval    -1        Fatal error, clicon_err called
 * xret should like: <top><rpc-reply><x xmlns="uri">0</x></rpc-reply></top>
 */
static int
api_operations_post_output(clicon_handle h,
			   void         *req,
			   cxobj        *xret,
			   yang_stmt    *yspec,
			   yang_stmt    *youtput,
			   char         *namespace,
			   int           pretty,
			   restconf_media media_out,
			   cxobj       **xoutputp)
    
{
    int        retval = -1;
    cxobj     *xoutput = NULL;
    cxobj     *xerr = NULL; /* assumed malloced, will be freed */
    cxobj     *xa;          /* xml attribute (xmlns) */
    cxobj     *x;
    cxobj     *xok;
    int        isempty;
    
    clicon_debug(1, "%s", __FUNCTION__);
    /* Validate that exactly only <rpc-reply> tag with exactly one element child */
    if ((xoutput = xml_child_i_type(xret, 0, CX_ELMNT)) == NULL ||
	strcmp(xml_name(xoutput),"rpc-reply") != 0
	/* See https://github.com/clicon/clixon/issues/158 
	 * This is an internal error, they should not be double but the error should not be detected
	 * here, it should be detected in the backend plugin caller.
	 ||	xml_child_nr_type(xrpc, CX_ELMNT) != 1 XXX backend can have multiple callbacks
	*/
	){
	if (netconf_malformed_message_xml(&xerr, "restconf RPC does not have single input") < 0)
	    goto done;	
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto fail;
    }
    /* xoutput should now look: <rpc-reply><x xmlns="uri">0</x></rpc-reply> */
    /* 9. Translate to restconf RPC data */
    xml_name_set(xoutput, "output");
    /* xoutput should now look: <output><x xmlns="uri">0</x></output> */
#if 1
    if (clicon_debug_get())
	clicon_log_xml(LOG_DEBUG, xoutput, "%s xoutput:", __FUNCTION__);
#endif
    /* Remove original netconf default namespace. Somewhat unsure what "output" belongs to? */
    if ((xa = xml_find_type(xoutput, NULL, "xmlns", CX_ATTR)) != NULL)
	if (xml_purge(xa) < 0)
	    goto done;

    /* Sanity check of outgoing XML 
     * For now, skip outgoing checks.
     * (1) Does not handle <ok/> properly
     * (2) Uncertain how validation errors should be logged/handled
     */
    if (youtput != NULL){
	xml_spec_set(xoutput, youtput); /* needed for xml_bind_yang */
#ifdef notyet
	if ((ret = xml_bind_yang(xoutput, YB_MODULE, yspec, &xerr)) < 0)
	    goto done;
	if (ret > 0 && (ret = xml_yang_validate_all(xoutput, &xerr)) < 0)
	    goto done;
	if (ret == 1 &&
	    (ret = xml_yang_validate_add(h, xoutput, &xerr)) < 0)
	    goto done;
	if (ret == 0){ /* validation failed */
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
		goto done;
	    goto fail;
	}
#endif
    }
    /* Special case, no yang output (single <ok/> - or empty body?)
     * RFC 7950 7.14.4
     * If the RPC operation invocation succeeded and no output parameters
     * are returned, the <rpc-reply> contains a single <ok/> element
     * RFC 8040 3.6.2
     * If the "rpc" statement has no "output" section, the response message
     * MUST NOT include a message-body and MUST send a "204 No Content"
     * status-line instead.
     */
    isempty = xml_child_nr_type(xoutput, CX_ELMNT) == 0 ||
	(xml_child_nr_type(xoutput, CX_ELMNT) == 1 &&
	 (xok = xml_child_i_type(xoutput, 0, CX_ELMNT)) != NULL &&
	 strcmp(xml_name(xok),"ok")==0);
    if (isempty) {
	/* Internal error - invalid output from rpc handler */
	if (restconf_reply_send(req, 204, NULL, 0) < 0)
	    goto done;	
	goto fail;
    }
    /* Clear namespace of parameters */
    x = NULL;
    while ((x = xml_child_each(xoutput, x, CX_ELMNT)) != NULL) {
	if ((xa = xml_find_type(x, NULL, "xmlns", CX_ATTR)) != NULL)
	    if (xml_purge(xa) < 0)
		goto done;
    }
    /* Set namespace on output */
    if (xmlns_set(xoutput, NULL, namespace) < 0)
	goto done;
    *xoutputp = xoutput;
    retval = 1;
 done:
    clicon_debug(1, "%s retval: %d", __FUNCTION__, retval);
    if (xerr)
	xml_free(xerr);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! REST operation POST method 
 * @param[in]  h        Clixon handle
 * @param[in]  req      Generic Www handle
 * @param[in]  api_path According to restconf (Sec 3.5.3.1 in rfc8040)
 * @param[in]  qvec     Vector of query string (QUERY_STRING)
 * @param[in]  data     Stream input data
 * @param[in]  pretty   Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * See RFC 8040 Sec 3.6 / 4.4.2
 * @note We map post to edit-config create. 
 *      POST {+restconf}/operations/<operation>
 * 1. Initialize
 * 2. Get rpc module and name from uri (oppath) and find yang spec
 * 3. Build xml tree with user and rpc: <rpc username="foo"><myfn xmlns="uri"/>
 * 4. Parse input data (arguments):
 *             JSON: {"example:input":{"x":0}}
 *             XML:  <input xmlns="uri"><x>0</x></input>
 * 5. Translate input args to Netconf RPC, add to xml tree:
 *             <rpc username="foo"><myfn xmlns="uri"><x>42</x></myfn></rpc>
 * 6. Validate outgoing RPC and fill in default values
 *  <rpc username="foo"><myfn xmlns="uri"><x>42</x><y>99</y></myfn></rpc>
 * 7. Send to RPC handler, either local or backend
 * 8. Receive reply from local/backend handler as Netconf RPC
 *       <rpc-reply><x xmlns="uri">0</x></rpc-reply>
 * 9. Translate to restconf RPC data:
 *             JSON: {"example:output":{"x":0}}
 *             XML:  <output xmlns="uri"><x>0</x></input>
 * 10. Validate and send reply to originator
 */
int
api_operations_post(clicon_handle h,
		    void         *req,
		    char         *api_path, 
		    int           pi,
		    cvec         *qvec, 
		    char         *data,
		    int           pretty,
		    restconf_media media_out)
{
    int        retval = -1;
    int        i;
    char      *oppath = api_path;
    yang_stmt *yspec;
    yang_stmt *youtput = NULL;
    yang_stmt *yrpc = NULL;
    cxobj     *xret = NULL;
    cxobj     *xerr = NULL; /* malloced must be freed */
    cxobj     *xtop = NULL; /* xpath root */
    cxobj     *xbot = NULL;
    yang_stmt *y = NULL;
    cxobj     *xoutput = NULL;
    cxobj     *xe;
    char      *username;
    cbuf      *cbret = NULL;
    int        ret = 0;
    char      *prefix = NULL;
    char      *id = NULL;
    yang_stmt *ys = NULL;
    char      *namespace = NULL;
    int        nr = 0;
    
    clicon_debug(1, "%s json:\"%s\" path:\"%s\"", __FUNCTION__, data, api_path);
    /* 1. Initialize */
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if ((cbret = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, 0, "cbuf_new");
	goto done;
    }
    for (i=0; i<pi; i++)
	oppath = index(oppath+1, '/');
    if (oppath == NULL || strcmp(oppath,"/")==0){
	if (netconf_operation_failed_xml(&xerr, "protocol", "Operation name expected") < 0)
	    goto done;
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* 2. Get rpc module and name from uri (oppath) and find yang spec 
     *       POST {+restconf}/operations/<operation>
     *
     * The <operation> field identifies the module name and rpc identifier
     * string for the desired operation.
     */
    if (nodeid_split(oppath+1, &prefix, &id) < 0) /* +1 skip / */
	goto done;
    if ((ys = yang_find(yspec, Y_MODULE, prefix)) == NULL){
	if (netconf_operation_failed_xml(&xerr, "protocol", "yang module not found") < 0)
	    goto done;
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if ((yrpc = yang_find(ys, Y_RPC, id)) == NULL){
	if (netconf_missing_element_xml(&xerr, "application", id, "RPC not defined") < 0)
	    goto done;
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* 3. Build xml tree with user and rpc: 
     * <rpc username="foo"><myfn xmlns="uri"/>
     */
    if ((username = clicon_username_get(h)) != NULL){
	if (clixon_xml_parse_va(YB_NONE, NULL, &xtop, NULL, "<rpc xmlns=\"%s\" username=\"%s\" %s/>",
				NETCONF_BASE_NAMESPACE, username, NETCONF_MESSAGE_ID_ATTR) < 0)
	    goto done;
    }
    else
	if (clixon_xml_parse_va(YB_NONE, NULL, &xtop, NULL, "<rpc xmlns=\"%s\" %s/>",
				NETCONF_BASE_NAMESPACE, NETCONF_MESSAGE_ID_ATTR) < 0)
	    goto done;
    if (xml_rootchild(xtop, 0, &xtop) < 0)
	goto done;
    xbot = xtop;
    if ((ret = api_path2xml(oppath, yspec, xtop, YC_SCHEMANODE, 1, &xbot, &y, &xerr)) < 0)
	goto done;
    if (ret == 0){ /* validation failed */
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Here xtop is: <rpc username="foo"><myfn xmlns="uri"/></rpc> 
     * xbot is <myfn xmlns="uri"/>
     * 4. Parse input data (arguments):
     *             JSON: {"example:input":{"x":0}}
     *             XML:  <input xmlns="uri"><x>0</x></input>
     */
    namespace = xml_find_type_value(xbot, NULL, "xmlns", CX_ATTR);
    clicon_debug(1, "%s : 4. Parse input data: %s", __FUNCTION__, data);
    if (data && strlen(data)){
	if ((ret = api_operations_post_input(h, req, data, yspec, yrpc, xbot,
					     pretty, media_out)) < 0)
	    goto done;
	if (ret == 0)
	    goto ok;
    }
    /* Here xtop is: 
      <rpc username="foo"><myfn xmlns="uri"><x>42</x></myfn></rpc> */
#if 1
    if (clicon_debug_get())
	clicon_log_xml(LOG_DEBUG, xtop, "%s 5. Translate input args:", __FUNCTION__);
#endif
    /* 6. Validate outgoing RPC and fill in defaults */
    if ((ret = xml_bind_yang_rpc(xtop, yspec, &xerr)) < 0) /*  */
	goto done;
    if (ret == 0){
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if ((ret = xml_yang_validate_rpc(h, xtop, &xerr)) < 0)
	goto done;
    if (ret == 0){
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Here xtop is (default values):
     * <rpc username="foo"><myfn xmlns="uri"><x>42</x><y>99</y></myfn></rpc>
    */
#if 0
    if (clicon_debug_get())
	clicon_log_xml(LOG_DEBUG, xtop, "%s 6. Validate and defaults:", __FUNCTION__);
#endif
    /* 7. Send to RPC handler, either local or backend
     * Note (1) xtop is <rpc><method> xbot is <method>
     *      (2) local handler wants <method> and backend wants <rpc><method>
     */
    /* Look for local (client-side) restconf plugins. 
     * -1:Error, 0:OK local, 1:OK backend 
     */
    if ((ret = rpc_callback_call(h, xbot, req, &nr, cbret)) < 0)
	goto done;
    if (ret == 0){
	if (clixon_xml_parse_string(cbuf_get(cbret), YB_NONE, NULL, &xe, NULL) < 0)
	    goto done;
	if (api_return_err(h, req, xe, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    else if (nr > 0){ /* Handled locally */
	if (clixon_xml_parse_string(cbuf_get(cbret), YB_NONE, NULL, &xret, NULL) < 0)
	    goto done;
	/* Local error: return it and quit */
	if ((xe = xpath_first(xret, NULL, "rpc-reply/rpc-error")) != NULL){
	    if (api_return_err(h, req, xe, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    else {    /* Send to backend */
	if (clicon_rpc_netconf_xml(h, xtop, &xret, NULL) < 0)
	    goto done;
	if ((xe = xpath_first(xret, NULL, "rpc-reply/rpc-error")) != NULL){
	    if (api_return_err(h, req, xe, pretty, media_out, 0) < 0)
		goto done;
	    goto ok;
	}
    }
    /* 8. Receive reply from local/backend handler as Netconf RPC
     *       <rpc-reply><x xmlns="uri">0</x></rpc-reply>
     */
#if 1
    if (clicon_debug_get())
	clicon_log_xml(LOG_DEBUG, xret, "%s Receive reply:", __FUNCTION__);
#endif
    youtput = yang_find(yrpc, Y_OUTPUT, NULL);
    if ((ret = api_operations_post_output(h, req, xret, yspec, youtput, namespace,
					  pretty, media_out, &xoutput)) < 0)
	goto done;
    if (ret == 0)
	goto ok;
    /* xoutput should now look: <output xmlns="uri"><x>0</x></output> */
    if (restconf_reply_header(req, "Content-Type", "%s", restconf_media_int2str(media_out)) < 0)
	goto done;
    cbuf_reset(cbret);
    switch (media_out){
    case YANG_DATA_XML:
	if (clixon_xml2cbuf(cbret, xoutput, 0, pretty, -1, 0) < 0)
	    goto done;
	/* xoutput should now look: <output xmlns="uri"><x>0</x></output> */
	break;
    case YANG_DATA_JSON:
	if (clixon_json2cbuf(cbret, xoutput, pretty, 0) < 0)
	    goto done;
	/* xoutput should now look: {"example:output": {"x":0,"y":42}} */
	break;
    default:
	break;
    }
    if (restconf_reply_send(req, 200, cbret, 0) < 0)
	goto done;
    cbret = NULL;
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (prefix)
	free(prefix);
    if (id)
	free(id);
    if (xtop)
	xml_free(xtop);
    if (xret)
	xml_free(xret);
    if (xerr)
	xml_free(xerr);
    if (cbret)
	cbuf_free(cbret);
   return retval;
}
