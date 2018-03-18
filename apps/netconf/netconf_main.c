/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <libgen.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_netconf.h"
#include "netconf_lib.h"
#include "netconf_hello.h"
#include "netconf_plugin.h"
#include "netconf_rpc.h"

/* Command line options to be passed to getopt(3) */
#define NETCONF_OPTS "hDqf:d:Sy:"

/*! Process incoming packet 
 * @param[in]   h    Clicon handle
 * @param[in]   cb   Packet buffer
 */
static int
process_incoming_packet(clicon_handle h, 
			cbuf         *cb)
{
    char  *str;
    char  *str0;
    cxobj *xreq = NULL; /* Request (in) */
    int    isrpc = 0;   /* either hello or rpc */
    cbuf  *cbret = NULL;
    cxobj *xret = NULL; /* Return (out) */
    cxobj *xrpc;
    cxobj *xc;

    clicon_debug(1, "RECV");
    clicon_debug(2, "%s: RCV: \"%s\"", __FUNCTION__, cbuf_get(cb));
    if ((str0 = strdup(cbuf_get(cb))) == NULL){
	clicon_log(LOG_ERR, "%s: strdup: %s", __FUNCTION__, strerror(errno));
	return -1;
    }
    str = str0;
    /* Parse incoming XML message */
    if (xml_parse_string(str, NULL, &xreq) < 0){ 
	if ((cbret = cbuf_new()) == NULL){
	    if (netconf_operation_failed(cbret, "rpc", "internal error")< 0)
		goto done;
	    netconf_output(1, cbret, "rpc-error");
	}
	else
	    clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	free(str0);
	goto done;
    }
    free(str0);
    if ((xrpc=xpath_first(xreq, "//rpc")) != NULL)
        isrpc++;
    else
        if (xpath_first(xreq, "//hello") != NULL)
	    ;
        else{
            clicon_log(LOG_WARNING, "Invalid netconf msg: neither rpc or hello: dropped");
            goto done;
        }
    if (!isrpc){ /* hello */
	if (netconf_hello_dispatch(xreq) < 0)
	    goto done;
    }
    else  /* rpc */
	if (netconf_rpc_dispatch(h, xrpc, &xret) < 0){
	    goto done;
	}
	else{ /* there is a return message in xret */
	    cxobj *xa, *xa2;
	    assert(xret);

	    if ((cbret = cbuf_new()) != NULL){
		if ((xc = xml_child_i(xret,0))!=NULL){
		    xa=NULL;
		    while ((xa = xml_child_each(xrpc, xa, CX_ATTR)) != NULL){
			if ((xa2 = xml_dup(xa)) ==NULL)
			    goto done;
			if (xml_addsub(xc, xa2) < 0)
			    goto done;
		    }
		    add_preamble(cbret);

		    clicon_xml2cbuf(cbret, xml_child_i(xret,0), 0, 0);
		    add_postamble(cbret);
		    if (netconf_output(1, cbret, "rpc-reply") < 0){
			cbuf_free(cbret);
			goto done;
		    }
		}
	    }
	}
  done:
    if (xreq)
	xml_free(xreq);
    if (xret)
	xml_free(xret);
    if (cbret)
	cbuf_free(cbret);
    return 0;
}

/*! Get netconf message: detect end-of-msg 
 * @param[in]   s    Socket where input arrived. read from this.
 * @param[in]   arg  Clicon handle.
 * This routine continuously reads until no more data on s. There could
 * be risk of starvation, but the netconf client does little else than
 * read data so I do not see a danger of true starvation here.
 */
static int
netconf_input_cb(int   s, 
		 void *arg)
{
    int           retval = -1;
    clicon_handle h = arg;
    unsigned char buf[BUFSIZ];
    int           i;
    int           len;
    cbuf         *cb=NULL;
    int           xml_state = 0;
    int           poll;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "%s: cbuf_new", __FUNCTION__);
	return retval;
    }
    memset(buf, 0, sizeof(buf));
    while (1){
	if ((len = read(s, buf, sizeof(buf))) < 0){
	    if (errno == ECONNRESET)
		len = 0; /* emulate EOF */
	    else{
		clicon_log(LOG_ERR, "%s: read: %s", __FUNCTION__, strerror(errno));
		goto done;
	    }
	} /* read */
	if (len == 0){ 	/* EOF */
	    cc_closed++;
	    close(s);
	    retval = 0;
	    goto done;
	}

	for (i=0; i<len; i++){
	    if (buf[i] == 0)
		continue; /* Skip NULL chars (eg from terminals) */
	    cprintf(cb, "%c", buf[i]);
	    if (detect_endtag("]]>]]>",
			      buf[i],
			      &xml_state)) {
		/* OK, we have an xml string from a client */
		/* Remove trailer */
		*(((char*)cbuf_get(cb)) + cbuf_len(cb) - strlen("]]>]]>")) = '\0';
		if (process_incoming_packet(h, cb) < 0)
		    goto done;
		if (cc_closed)
		    break;
		cbuf_reset(cb);
	    }
	}
	/* poll==1 if more, poll==0 if none */
	if ((poll = event_poll(s)) < 0)
	    goto done;
	if (poll == 0)
	    break; /* No data to read */
    } /* while */
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (cc_closed) 
	retval = -1;
    return retval;
}

/*
 * send_hello
 * args: s file descriptor to write on (eg 1 - stdout)
 */
static int
send_hello(int s)
{
    cbuf *xf;
    int retval = -1;
    
    if ((xf = cbuf_new()) == NULL){
	clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	goto done;
    }
    if (netconf_create_hello(xf, getpid()) < 0)
	goto done;
    if (netconf_output(s, xf, "hello") < 0)
	goto done;
    retval = 0;
  done:
    if (xf)
	cbuf_free(xf);
    return retval;
}

static int
netconf_terminate(clicon_handle h)
{
    yang_spec      *yspec;

    clicon_rpc_close_session(h);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	yspec_free(yspec);
    if ((yspec = clicon_netconf_yang(h)) != NULL)
	yspec_free(yspec);
    event_exit();
    clicon_handle_exit(h);
    return 0;
}


/*! Usage help routine
 * @param[in]  h      Clicon handle
 * @param[in]  argv0  command line
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
            "\t-D\t\tDebug\n"
            "\t-q\t\tQuiet: dont send hello prompt\n"
    	    "\t-f <file>\tConfiguration file (mandatory)\n"
	    "\t-d <dir>\tSpecify netconf plugin directory dir (default: %s)\n"
	    "\t-S\t\tLog on syslog\n"
	    "\t-y <file>\tOverride yang spec file (dont include .yang suffix)\n",
	    argv0,
	    clicon_netconf_dir(h)
	    );
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    char             c;
    char            *tmp;
    char            *argv0 = argv[0];
    int              quiet = 0;
    clicon_handle    h;
    int              use_syslog;

    /* Defaults */
    use_syslog = 0;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, CLICON_LOG_STDERR); 
    /* Create handle */
    if ((h = clicon_handle_init()) == NULL)
	return -1;

    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	    usage(h, argv[0]);
	    break;
	case 'D' : /* debug */
	    debug = 1;
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'S': /* Log on syslog */
	     use_syslog = 1;
	     break;
	}
    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, 
		    use_syslog?CLICON_LOG_SYSLOG:CLICON_LOG_STDERR); 
    clicon_debug_init(debug, NULL); 

    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
	return -1;

    /* Now rest of options */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f': /* config file */
	case 'S': /* Log on syslog */
	    break; /* see above */
	case 'q':  /* quiet: dont write hello */
	    quiet++;
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_NETCONF_DIR", optarg);
	    break;
	case 'y' :{ /* Overwrite yang module or absolute filename */
	    clicon_option_str_set(h, "CLICON_YANG_MODULE_MAIN", optarg);
	    break;
	}
	default:
	    usage(h, argv[0]);
	    break;
	}
    argc -= optind;
    argv += optind;

    /* Parse yang database spec file */
    if (yang_spec_main(h) == NULL)
	goto done;

    /* Parse netconf yang spec file  */
    if (yang_spec_netconf(h) == NULL)
	goto done;

    /* Initialize plugins group */
    if (netconf_plugin_load(h) < 0)
	goto done;

    /* Call start function is all plugins before we go interactive */
    tmp = *(argv-1);
    *(argv-1) = argv0;
    netconf_plugin_start(h, argc+1, argv-1);
    *(argv-1) = tmp;

    if (!quiet)
	send_hello(1);
    if (event_reg_fd(0, netconf_input_cb, h, "netconf socket") < 0)
	goto done;
    if (debug)
	clicon_option_dump(h, debug);
    if (event_loop() < 0)
	goto done;
  done:
    netconf_plugin_unload(h);
    netconf_terminate(h);
    clicon_log_init(__PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clicon_log(LOG_NOTICE, "%s: %u Terminated\n", __PROGRAM__, getpid());
    return 0;
}
