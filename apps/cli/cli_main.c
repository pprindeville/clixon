/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
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

 * 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <pwd.h>
#include <libgen.h>
#include <wordexp.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_cli_api.h"

#include "cli_plugin.h"
#include "cli_autocli.h"
#include "cli_generate.h"
#include "cli_common.h"
#include "cli_handle.h"

/* Command line options to be passed to getopt(3) */
#define CLI_OPTS "+hD:f:E:l:F:1a:u:d:m:qp:GLy:c:U:o:"

/*! Check if there is a CLI history file and if so dump the CLI histiry to it
 * Just log if file does not exist or is not readable
 * @param[in]  h    CLICON handle
 */
static int
cli_history_load(clicon_handle h)
{
    int       retval = -1;
    int       lines;
    char     *filename;
    FILE     *f = NULL;
    wordexp_t result = {0,}; /* for tilde expansion */

    /* Get history size from clixon option, if not use cligen default. */
    if (clicon_option_exists(h, "CLICON_CLI_HIST_SIZE"))
	lines = clicon_option_int(h,"CLICON_CLI_HIST_SIZE");
    else
	lines = CLIGEN_HISTSIZE_DEFAULT;
    /* Re-init history with clixon lines (1st time was w cligen defaults) */
    if (cligen_hist_init(cli_cligen(h), lines) < 0)
	goto done;
    if ((filename = clicon_option_str(h,"CLICON_CLI_HIST_FILE")) == NULL)
	goto ok; /* ignore */
    if (wordexp(filename, &result, 0) < 0){
	clicon_err(OE_UNIX, errno, "wordexp");
	goto done;
    }
    if ((f = fopen(result.we_wordv[0], "r")) == NULL){
	clicon_log(LOG_DEBUG, "Warning: Could not open CLI history file for reading: %s: %s",
		   result.we_wordv[0], strerror(errno));
	goto ok;
    }
    if (cligen_hist_file_load(cli_cligen(h), f) < 0){
	clicon_err(OE_UNIX, errno, "cligen_hist_file_load");
	goto done;
    }
 ok:
    retval = 0;
 done:
    wordfree(&result);
    if (f)
	fclose(f);
    return retval;
}

/*! Start CLI history and load from file 
 * Just log if file does not exist or is not readable
 * @param[in]  h    CLICON handle
 */
static int
cli_history_save(clicon_handle h)
{
    int       retval = -1;
    char     *filename;
    FILE     *f = NULL;
    wordexp_t result = {0,}; /* for tilde expansion */

    if ((filename = clicon_option_str(h, "CLICON_CLI_HIST_FILE")) == NULL)
	goto ok; /* ignore */
    if (wordexp(filename, &result, 0) < 0){
	clicon_err(OE_UNIX, errno, "wordexp");
	goto done;
    }
    if ((f = fopen(result.we_wordv[0], "w+")) == NULL){
	clicon_log(LOG_DEBUG, "Warning: Could not open CLI history file for writing: %s: %s",
		   result.we_wordv[0], strerror(errno));
	goto ok;
    }
    if (cligen_hist_file_save(cli_cligen(h), f) < 0){
	clicon_err(OE_UNIX, errno, "cligen_hist_file_save");
	goto done;
    }
 ok:
    retval = 0;
 done:
    wordfree(&result);
    if (f)
	fclose(f);
    return retval;
}


/*! Clean and close all state of cli process (but dont exit). 
 * Cannot use h after this 
 * @param[in]  h  Clixon handle
 */
static int
cli_terminate(clicon_handle h)
{
    yang_stmt  *yspec;
    cvec       *nsctx;
    cxobj      *x;

    clicon_rpc_close_session(h);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	ys_free(yspec);
    if ((yspec = clicon_config_yang(h)) != NULL)
	ys_free(yspec);
    if ((nsctx = clicon_nsctx_global_get(h)) != NULL)
	cvec_free(nsctx);
    if ((x = clicon_conf_xml(h)) != NULL)
	xml_free(x);
    clicon_data_cvec_del(h, "cli-edit-cvv");;
    clicon_data_cvec_del(h, "cli-edit-filter");;
    xpath_optimize_exit();
    /* Delete all plugins, and RPC callbacks */
    clixon_plugin_module_exit(h);
    /* Delete CLI syntax et al */
    cli_plugin_finish(h);  

    cli_history_save(h);
    cli_handle_exit(h);
    clixon_err_exit();
    clicon_log_exit();
    return 0;
}

/*! Unlink pidfile and quit
*/
static void
cli_sig_term(int arg)
{
    clicon_log(LOG_NOTICE, "%s: %u Terminated (killed by sig %d)", 
	    __PROGRAM__, getpid(), arg);
    exit(1);
}

/*! Setup signal handlers
 */
static void
cli_signal_init (clicon_handle h)
{
	cli_signal_block(h);
	set_signal(SIGTERM, cli_sig_term, NULL);
}

/*! Interactive CLI command loop
 * @param[in]  h    CLICON handle
 * @retval     0
 * @retval    -1
 * @see cligen_loop
 */
static int
cli_interactive(clicon_handle h)
{
    int           retval = -1;
    char         *cmd;
    char         *new_mode;
    cligen_result result;
    
    /* Loop through all commands */
    while(!cligen_exiting(cli_cligen(h))) {
	new_mode = cli_syntax_mode(h);
	cmd = NULL;
	if (clicon_cliread(h, &cmd) < 0)
	    goto done;
	if (cmd == NULL) { /* EOF */
	    cligen_exiting_set(cli_cligen(h), 1); 
	    continue;
	}
	if (clicon_parse(h, cmd, &new_mode, &result, NULL) < 0)
	    goto done;
	/* Why not check result? */
    }
    retval = 0;
 done:
    return retval;
}

/*! Create pre-5.5 tree-refs for backward compatibility
 * should probably be moved to clispec default 
 */
static int
autocli_trees_default(clicon_handle h)
{
    int         retval = -1;
    cbuf       *cb = NULL;
    int         mode = 0;
    parse_tree *pt = NULL; 
    pt_head    *ph;

    /* Create backward compatible tree: @datamodel */
    if ((ph = cligen_ph_add(cli_cligen(h), "datamodel")) == NULL)
	goto done;
    if ((pt = pt_new()) == NULL){
	clicon_err(OE_UNIX, errno, "pt_new");
	goto done;
    }
    if (cligen_parse_str(cli_cligen(h),
			 "@basemodel, @remove:act-prekey, @remove:act-list, @remove:act-leafconst, @remove:ac-state;",
			 "datamodel", pt, NULL) < 0)
	goto done;
    if (cligen_ph_parsetree_set(ph, pt) < 0)
	goto done;


    /* Create backward compatible tree: @datamodelshow */
    if ((ph = cligen_ph_add(cli_cligen(h), "datamodelshow")) == NULL)
	goto done;
    if ((pt = pt_new()) == NULL){
	clicon_err(OE_UNIX, errno, "pt_new");
	goto done;
    }
    if (cligen_parse_str(cli_cligen(h),
			 "@basemodel, @remove:act-leafvar, @remove:ac-state;",
			 "datamodelshow", pt, NULL) < 0)
	goto done;
    if (cligen_ph_parsetree_set(ph, pt) < 0)
	goto done;

    /* Create backward compatible tree: @datamodelstate */
    if ((ph = cligen_ph_add(cli_cligen(h), "datamodelstate")) == NULL)
	goto done;
    if ((pt = pt_new()) == NULL){
	clicon_err(OE_UNIX, errno, "pt_new");
	goto done;
    }
    if (cligen_parse_str(cli_cligen(h),
			 "@basemodel, @remove:act-leafvar;",
			 "datamodelstate", pt, NULL) < 0)
	goto done;
    if (cligen_ph_parsetree_set(ph, pt) < 0)
	goto done;

    /* Create new tree: @datamodelmode */
    if ((ph = cligen_ph_add(cli_cligen(h), "datamodelmode")) == NULL)
	goto done;
    if ((pt = pt_new()) == NULL){
	clicon_err(OE_UNIX, errno, "pt_new");
	goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "@basemodel, @remove:act-prekey, @remove:act-leafconst, @remove:ac-state");
    /* Check if container and list are allowed edit modes */
    mode = 0;
    if (autocli_edit_mode(h, "container", &mode) < 0)
	goto done;
    if (mode == 0)
	cprintf(cb, ", @remove:act-container");
    mode = 0;
    if (autocli_edit_mode(h, "listall", &mode) < 0)
	goto done;
    if (mode == 0)
	cprintf(cb, ", @remove:act-list");
    mode = 0;
    if (autocli_edit_mode(h, "list", &mode) < 0)
	goto done;
    if (mode == 0)
	cprintf(cb, ", @remove:act-lastkey");
    mode = 0;
    if (autocli_edit_mode(h, "leaf", &mode) < 0)
	goto done;
    if (mode == 0)
	cprintf(cb, ", @remove:ac-leaf");
    cprintf(cb, ";");
    if (cligen_parse_str(cli_cligen(h), cbuf_get(cb), "datamodelmode", pt, NULL) < 0)
	goto done;
    if (cligen_ph_parsetree_set(ph, pt) < 0)
	goto done;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Generate autocli, ie if enabled, generate clispec from YANG and add to cligen parse-trees
 *
 * Generate clispec (basemodel) from YANG dataspec and add to the set of cligen trees 
 * This tree is referenced from the main CLI spec (CLICON_CLISPEC_DIR) using the 
 * "tree reference" syntax.
 *
 * @param[in]  h        Clixon handle
 * @param[in]  printgen Print CLI syntax generated from dbspec
 * @retval     0        OK
 * @retval    -1        Error
 */
static int
autocli_start(clicon_handle h,
	      int           printgen)
{
    int           retval = -1;
    yang_stmt    *yspec;
    int           enable = 0;
    
    clicon_debug(1, "%s", __FUNCTION__);
    /* There is no single "enable-autocli" flag,
     * but set 
     *   <module-default>false</module-default> 
     * with no rules:
     *   <rule><operation>enable</operation>
     * is disable
     */
    if (autocli_module(h, NULL, &enable) < 0)
	goto done;
    if (!enable){
	clicon_debug(1, "%s Autocli not enabled (clixon-autocli)", __FUNCTION__);
	goto ok;
    }
    /* Init yang2cli */
    if (yang2cli_init(h) < 0)
	goto done;
    yspec = clicon_dbspec_yang(h);
    /* The actual generating call from yang to clispec for the complete yang spec */
    if (yang2cli_yspec(h, yspec, AUTOCLI_TREENAME, printgen) < 0)
	goto done;
    /* XXX Create pre-5.5 tree-refs for backward compatibility */    
    if (autocli_trees_default(h) < 0)
	goto done;
 ok:
    retval = 0;
 done:
    return retval;
}

static void
usage(clicon_handle h,
      char         *argv0)
{
    char *plgdir = clicon_cli_dir(h);

    fprintf(stderr, "usage:%s [options] [commands]\n"
	    "where commands is a CLI command or options passed to the main plugin\n" 
	    "where options are\n"
            "\t-h \t\tHelp\n"
    	    "\t-D <level> \tDebug level\n"
	    "\t-f <file> \tConfig-file (mandatory)\n"
	    "\t-E <dir>  \tExtra configuration file directory\n"
    	    "\t-F <file> \tRead commands from file (default stdin)\n"
	    "\t-1\t\tDo not enter interactive mode\n"
    	    "\t-a UNIX|IPv4|IPv6\tInternal backend socket family\n"
    	    "\t-u <path|addr>\tInternal socket domain path or IP addr (see -a)\n"
	    "\t-d <dir>\tSpecify plugin directory (default: %s)\n"
            "\t-m <mode>\tSpecify plugin syntax mode\n"
	    "\t-q \t\tQuiet mode, dont print greetings or prompt, terminate on ctrl-C\n"
	    "\t-p <dir>\tYang directory path (see CLICON_YANG_DIR)\n"
	    "\t-G \t\tPrint auo-cli CLI syntax generated from YANG\n"
	    "\t-L \t\tDebug print dynamic CLI syntax including completions and expansions\n"
	    "\t-l <s|e|o|f<file>> \tLog on (s)yslog, std(e)rr, std(o)ut or (f)ile (stderr is default)\n"
	    "\t-y <file>\tOverride yang spec file (dont include .yang suffix)\n"
	    "\t-c <file>\tSpecify cli spec file.\n"
	    "\t-U <user>\tOver-ride unix user with a pseudo user for NACM.\n"
	    "\t-o \"<option>=<value>\"\tGive configuration option overriding config file (see clixon-config.yang)\n",
	    argv0,
	    plgdir ? plgdir : "none"
	);
    exit(1);
}

/*
 */
int
main(int    argc,
     char **argv)
{
    int            retval = -1;
    int            c;    
    int            once;
    char	  *tmp;
    char	  *argv0 = argv[0];
    clicon_handle  h;
    int            printgen  = 0;
    int            logclisyntax  = 0;
    int            help = 0;
    int            logdst = CLICON_LOG_STDERR;
    char          *restarg = NULL; /* what remains after options */
    yang_stmt     *yspec;
    struct passwd *pw;
    char          *str;
    int            tabmode;
    cvec          *nsctx_global = NULL; /* Global namespace context */
    size_t         cligen_buflen;
    size_t         cligen_bufthreshold;
    int            dbg=0;
    int            nr;
    
    /* Defaults */
    once = 0;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Initiate CLICON handle. CLIgen is also initialized */
    if ((h = cli_handle_init()) == NULL)
	goto done;

    /* Set username to clicon handle. Use in all communication to backend 
     * Note, can be overridden by -U
     */
    if ((pw = getpwuid(getuid())) == NULL){
	clicon_err(OE_UNIX, errno, "getpwuid");
	goto done;
    }
    if (clicon_username_set(h, pw->pw_name) < 0)
	goto done;

    cligen_comment_set(cli_cligen(h), '#'); /* Default to handle #! clicon_cli scripts */
    cligen_lexicalorder_set(cli_cligen(h), 1);
    
    /*
     * First-step command-line options for help, debug, config-file and log, 
     */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, CLI_OPTS)) != -1)
	switch (c) {
	case 'h':
	    /* Defer the call to usage() to later. Reason is that for helpful
	       text messages, default dirs, etc, are not set until later.
	       But this means that we need to check if 'help' is set before 
	       exiting, and then call usage() before exit.
	    */
	    help = 1; 
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(h, argv[0]);
	    break;
	case 'f': /* config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	case 'E': /* extra config directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGDIR", optarg);
	    break;
	case 'l': /* Log destination: s|e|o|f */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	    break;
	}
    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst);
    clicon_debug_init(dbg, NULL); 
    yang_init(h);

    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0){
        if (help)
	    usage(h, argv[0]);
	goto done;
    }
    /* Now rest of options */   
    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv, CLI_OPTS)) != -1){
	switch (c) {
	case 'D' : /* debug */
	case 'f': /* config file */
	case 'E': /* extra config dir */
	case 'l': /* Log destination */
	    break; /* see above */
	case 'F': /* read commands from file */
	    if (freopen(optarg, "r", stdin) == NULL){
		fprintf(stderr, "freopen: %s\n", strerror(errno));
		return -1;
	    }
	    break; 
	case '1' : /* Quit after reading database once - dont wait for events */
	    once = 1;
	    break;
	case 'a': /* internal backend socket address family */
	    if (clicon_option_add(h, "CLICON_SOCK_FAMILY", optarg) < 0)
		goto done;
	    break;
	case 'u': /* internal backend socket unix domain path or ip host */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_option_add(h, "CLICON_SOCK", optarg) < 0)
		goto done;
	    break;
	case 'd':  /* Plugin directory: overrides configfile */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_option_add(h, "CLICON_CLI_DIR", optarg) < 0)
		goto done;
	    break;
	case 'm': /* CLI syntax mode */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_option_add(h, "CLICON_CLI_MODE", optarg) < 0)
		goto done;
	    break;
	case 'q' : /* Quiet mode */
	    clicon_quiet_mode_set(h, 1);
	    break;
	case 'p' : /* yang dir path */
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'G' : /* Print generated CLI syntax */
	    printgen++;
	    break;
	case 'L' : /* Debug print dynamic CLI syntax */
	    logclisyntax++;
	    break;
	case 'y' : /* Load yang absolute filename */
	    if (clicon_option_add(h, "CLICON_YANG_MAIN_FILE", optarg) < 0)
		goto done;
	    break;
	case 'c' : /* Overwrite clispec with absolute filename */
	    if (clicon_option_add(h, "CLICON_CLISPEC_FILE", optarg) < 0)
		goto done;
	    break;
	case 'U': /* Clixon 'pseudo' user */
	    if (clicon_username_set(h, optarg) < 0)
		goto done;
	    break;
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
	default:
	    usage(h, argv[0]);
	    break;
	}
    }
    argc -= optind;
    argv += optind;

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);

    /* Defer: Wait to the last minute to print help message */
    if (help)
	usage(h, argv[0]);

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    /* Init row numbers for raw terminals */
    if (clicon_option_exists(h, "CLICON_CLI_LINES_DEFAULT")){
	nr = clicon_option_int(h, "CLICON_CLI_LINES_DEFAULT");
	cligen_terminal_rows_set(cli_cligen(h), nr);
    }
    
    if (clicon_yang_regexp(h) == REGEXP_LIBXML2){
#ifdef HAVE_LIBXML2
	/* Enable XSD libxml2 regex engine */
	cligen_regex_xsd_set(cli_cligen(h), 1);
#else
	clicon_err(OE_FATAL, 0, "CLICON_YANG_REGEXP set to libxml2, but HAVE_LIBXML2 not set (Either change CLICON_YANG_REGEXP to posix, or run: configure --with-libxml2))");
	goto done;
#endif
    }

    /* CLIgen help string setting for long and multi-line strings */
    nr = clicon_option_int(h, "CLICON_CLI_HELPSTRING_TRUNCATE");
    cligen_helpstring_truncate_set(cli_cligen(h), nr);
    nr = clicon_option_int(h, "CLICON_CLI_HELPSTRING_LINES");
    cligen_helpstring_lines_set(cli_cligen(h), nr);
    
    /* Setup signal handlers */
    cli_signal_init(h);

    /* Backward compatible mode, do not include keys in cgv-arrays in callbacks.
       Should be 0 but default is 1 since all legacy apps use 1
       Test legacy before shifting default to 0
     */
    cligen_exclude_keys_set(cli_cligen(h), clicon_cli_varonly(h)); 

    /* Initialize plugin module by creating a handle holding plugin and callback lists */
    if (clixon_plugin_module_init(h) < 0)
	goto done;

#ifndef CLIXON_STATIC_PLUGINS
    {
	char *dir;
	/* Load cli .so plugins before yangs are loaded (eg extension callbacks) and 
	 * before CLI is loaded by cli_syntax_load below */
	if ((dir = clicon_cli_dir(h)) != NULL &&
	    clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
	    goto done;
    }
#endif
    
    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;
    /* In case ietf-yang-metadata is loaded by application, handle annotation extension */
    if (yang_metadata_init(h) < 0)
	goto done;
    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);
    /* Create top-level and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);	
    
    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }

    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;

     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;

    /* Add netconf yang spec, used as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    
    /* Here all modules are loaded 
     * Compute and set canonical namespace context
     */
    if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	goto done;
    if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	goto done;

    /* Create autocli from YANG */
    if (autocli_start(h, printgen) < 0)
	goto done;

    /* Initialize cli syntax. 
     * Plugins have already been loaded by clixon_plugins_load above */
    if (cli_syntax_load(h) < 0)
	goto done;

    /* Set syntax mode if specified from command-line or config-file. */
    if (clicon_option_exists(h, "CLICON_CLI_MODE"))
	if ((tmp = clicon_cli_mode(h)) != NULL)
	    if (cli_set_syntax_mode(h, tmp) == 0) {
		fprintf(stderr, "FATAL: Failed to set syntax mode '%s'\n", tmp);
		goto done;
	    }

    if (!cli_syntax_mode(h)){
	fprintf(stderr, "FATAL: No cli mode set (use -m or CLICON_CLI_MODE)\n");
	goto done;
    }
    if (cligen_ph_find(cli_cligen(h), cli_syntax_mode(h)) == NULL)
	clicon_log(LOG_WARNING, "No such cli mode: %s (Specify cli mode with CLICON_CLI_MODE in config file or -m <mode> on command line", cli_syntax_mode(h));
    /* CLIgen tab mode, ie how <tab>s behave */
    if ((tabmode = clicon_cli_tab_mode(h)) < 0){
	fprintf(stderr, "FATAL: CLICON_CLI_TAB_MODE not set\n");
	goto done;
    }
    cligen_tabmode_set(cli_cligen(h), tabmode);

    if (logclisyntax)
	cli_logsyntax_set(h, logclisyntax);

    if (dbg)
	clicon_option_dump(h, dbg);
    
    /* Join rest of argv to a single command */
    restarg = clicon_strjoin(argc, argv, " ");

    /* If several cligen object variables match same preference, select first */
    cligen_preference_mode_set(cli_cligen(h), 1);

    /* Call start function in all plugins before we go interactive 
     */
    if (clixon_plugin_start_all(h) < 0)
	goto done;

    cligen_line_scrolling_set(cli_cligen(h), clicon_option_int(h,"CLICON_CLI_LINESCROLLING"));
    /*! Start CLI history and load from file */
    if (cli_history_load(h) < 0)
	goto done;
    /* Experimental utf8 mode */
    cligen_utf8_set(cli_cligen(h), clicon_option_int(h,"CLICON_CLI_UTF8"));
    /* Launch interfactive event loop, unless -1 */
    if (restarg != NULL && strlen(restarg)){
	char         *mode = cli_syntax_mode(h);
	cligen_result result;            /* match result */
	int           evalresult = 0;    /* if result == 1, calback result */

	if (clicon_parse(h, restarg, &mode, &result, &evalresult) < 0)
	    goto done;
	if (result != 1) /* Not unique match */
	    goto done;
	if (evalresult < 0)
	    goto done;
    }


    /* Go into event-loop unless -1 command-line */
    if (!once){
	retval = cli_interactive(h);
    }
    else
	retval = 0;
  done:
    if (restarg)
	free(restarg);
    // Gets in your face if we log on stderr
    clicon_log_init(__PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clicon_log(LOG_NOTICE, "%s: %u Terminated", __PROGRAM__, getpid());
    if (h)
	cli_terminate(h);
    return retval;
}
