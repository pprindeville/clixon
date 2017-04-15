/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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


#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <fcgi_stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <libgen.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_xml.h"
#include "clixon_yang.h"
#include "clixon_options.h"
#include "clixon_xml_db.h"

static struct xmldb_api *_xa_api = NULL;

/*! Load a specific plugin, call its init function and add it to plugins list
 * If init function fails (not found, wrong version, etc) print a log and dont
 * add it.
 * @param[in]  name      Filename (complete path) of plugin
 * @param[in]  filename  Actual filename with path
 * @param[out] plugin    Plugin data structure for invoking. Dealloc with free
 */
int 
xmldb_plugin_load(clicon_handle h,
		  char         *filename)
{
    int                      retval = -1;
    char                    *dlerrcode;
    plugin_init_t           *initfun;
    void                    *handle = NULL;
    char                    *error;

    dlerror();    /* Clear any existing error */
    if ((handle = dlopen(filename, RTLD_NOW|RTLD_GLOBAL)) == NULL) {
        error = (char*)dlerror();
	clicon_err(OE_PLUGIN, errno, "dlopen: %s\n", error ? error : "Unknown error");	
	goto done;
    }
    /* Try v1 */
    initfun = dlsym(handle, XMLDB_PLUGIN_INIT_FN);
    if ((dlerrcode = (char*)dlerror()) != NULL) {
	clicon_log(LOG_WARNING, "Error when loading init function %s: %s",
		   XMLDB_PLUGIN_INIT_FN, dlerrcode); 
	goto fail;
    }
    if ((_xa_api = initfun(XMLDB_API_VERSION)) == NULL) {
	clicon_log(LOG_WARNING, "%s: failed when running init function %s: %s", 
		   filename, XMLDB_PLUGIN_INIT_FN, errno?strerror(errno):"");
	goto fail;
    }
    if (_xa_api->xa_version != XMLDB_API_VERSION){
	clicon_log(LOG_WARNING, "%s: Unexpected plugin version number: %d", 
		   filename, _xa_api->xa_version);
	goto fail;
    }
    if (_xa_api->xa_magic != XMLDB_API_MAGIC){
	clicon_log(LOG_WARNING, "%s: Wrong plugin magic number: %x", 
		   filename, _xa_api->xa_magic);
	goto fail;
    }
    clicon_log(LOG_WARNING, "xmldb plugin %s loaded", filename);
    retval = 0;
 done:
    if (retval < 0 && handle)
	dlclose(handle);
    return retval;
 fail: /* plugin load failed, continue */
    retval = 0;
    goto done;
}

/*! XXX: fixme */
int
xmldb_plugin_unload(clicon_handle h)
{
    return 0;
}

/*! Connect to a datastore plugin
 * @retval  handle  Use this handle for other API calls
 * @retval  NULL    Error
 * @note You can do several connects, and have multiple connections to the same
 *       datastore. Note also that the xmldb handle is hidden in the clicon 
 *       handle, the clixon user does not need to handle it. Note also that
 *       typically only the backend invokes the datastore.
 * XXX what args does connect have?
 */
int
xmldb_connect(clicon_handle h)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_connect_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = _xa_api->xa_connect_fn()) == NULL)
	goto done;
    clicon_handle_xmldb_set(h, xh);

    retval = 0;
 done:
    return retval;
}

/*! Disconnect from a datastore plugin and deallocate handle
 * @param[in]  handle  Disconect and deallocate from this handle
 * @retval     0       OK
  */
int
xmldb_disconnect(clicon_handle h)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_disconnect_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Already disconnected from datastore plugin");
	goto done;
    }
    if (_xa_api->xa_disconnect_fn(xh) < 0)
	goto done;
    clicon_handle_xmldb_set(h, NULL);
    retval = 0;
 done:
    return retval;
}

/*! Get value of generic plugin option. Type of value is givenby context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name
 * @param[out] value   Pointer to Value of option
 * @retval     0       OK
 * @retval    -1       Error
 */
int
xmldb_getopt(clicon_handle h, 
	     char         *optname,
	     void        **value)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_getopt_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_getopt_fn(xh, optname, value);
 done:
    return retval;
}

/*! Set value of generic plugin option. Type of value is givenby context
 * @param[in]  xh      XMLDB handle
 * @param[in]  optname Option name
 * @param[in]  value   Value of option
 * @retval     0       OK
 * @retval    -1       Error
  */
int
xmldb_setopt(clicon_handle h, 
	     char         *optname,
	     void         *value)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_setopt_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_setopt_fn(xh, optname, value);
 done:
    return retval;
}

/*! Get content of database using xpath. return a set of matching sub-trees
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath.
 * @param[in]  h      Clicon handle
 * @param[in]  dbname Name of database to search in (filename including dir path
 * @param[in]  xpath  String with XPATH syntax. or NULL for all
 * @param[out] xtop   Single XML tree which xvec points to. Free with xml_free()
 * @param[out] xvec   Vector of xml trees. Free after use.
 * @param[out] xlen   Length of vector.
 * @retval     0      OK
 * @retval     -1     Error
 * @code
 *   cxobj   *xt;
 *   cxobj  **xvec;
 *   size_t   xlen;
 *   if (xmldb_get(xh, "running", "/interfaces/interface[name="eth"]", 
 *                 &xt, &xvec, &xlen) < 0)
 *      err;
 *   for (i=0; i<xlen; i++){
 *      xn = xv[i];
 *      ...
 *   }
 *   xml_free(xt);
 *   free(xvec);
 * @endcode
 * @note if xvec is given, then purge tree, if not return whole tree.
 * @see xpath_vec
 * @see xmldb_get
 */

int 
xmldb_get(clicon_handle h, 
	  char         *db, 
	  char         *xpath,
	  cxobj       **xtop, 
	  cxobj      ***xvec, 
	  size_t       *xlen)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_get_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_get_fn(xh, db, xpath, xtop, xvec, xlen);
 done:
    return retval;
}

/*! Modify database provided an xml tree and an operation
 *
 * @param[in]  h      CLICON handle
 * @param[in]  db     running or candidate
 * @param[in]  xt     xml-tree. Top-level symbol is dummy
 * @param[in]  op     OP_MERGE: just add it. 
 *                    OP_REPLACE: first delete whole database
 *                    OP_NONE: operation attribute in xml determines operation
 * @param[in]  api_path According to restconf (Sec 3.5.1.1 in [restconf-draft 13])
 * @retval     0      OK
 * @retval     -1     Error
 * The xml may contain the "operation" attribute which defines the operation.
 * @code
 *   cxobj     *xt;
 *   if (clicon_xml_parse_str("<a>17</a>", &xt) < 0)
 *     err;
 *   if (xmldb_put(xh, "running", OP_MERGE, NULL, xt) < 0)
 *     err;
 * @endcode
 * @see xmldb_put_xkey  for single key
 */
int 
xmldb_put(clicon_handle       h, 
	  char               *db, 
	  enum operation_type op, 
	  char               *api_path, 
	  cxobj              *xt)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_put_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_put_fn(xh, db, op, api_path, xt);
 done:
    return retval;
}

/*! Copy database from db1 to db2
 * @param[in]  h     Clicon handle
 * @param[in]  from  Source database copy
 * @param[in]  to    Destination database
 * @retval -1  Error
 * @retval  0  OK
  */
int 
xmldb_copy(clicon_handle h, 
	   char         *from, 
	   char         *to)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_copy_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_copy_fn(xh, from, to);
 done:
    return retval;
}

/*! Lock database
 * @param[in]  h    Clicon handle
 * @param[in]  db   Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
  */
int 
xmldb_lock(clicon_handle h, 
	   char         *db, 
	   int           pid)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_lock_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_lock_fn(xh, db, pid);
 done:
    return retval;
}

/*! Unlock database
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
 * Assume all sanity checks have been made
 */
int 
xmldb_unlock(clicon_handle h, 
	     char         *db, 
	     int           pid)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_unlock_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_unlock_fn(xh, db, pid);
 done:
    return retval;
}

/*! Unlock all databases locked by pid (eg process dies) 
 * @param[in]    h   Clicon handle
 * @param[in]    pid Process / Session id
 * @retval -1    Error
 * @retval  0   OK
 */
int
xmldb_unlock_all(clicon_handle h, 
		 int           pid)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_unlock_all_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval =_xa_api->xa_unlock_all_fn(xh, pid);
 done:
    return retval;
}

/*! Check if database is locked
 * @param[in]    h   Clicon handle
 * @param[in]    db  Database
 * @retval -1    Error
 * @retval   0   Not locked
 * @retval  >0   Id of locker
  */
int 
xmldb_islocked(clicon_handle h, 
	       char         *db)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_islocked_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval =_xa_api->xa_islocked_fn(xh, db);
 done:
    return retval;
}

/*! Check if db exists 
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  No it does not exist
 * @retval  1  Yes it exists
 */
int 
xmldb_exists(clicon_handle h, 
	     char         *db)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_exists_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_exists_fn(xh, db);
 done:
    return retval;
}

/*! Delete database. Remove file 
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  OK
 */
int 
xmldb_delete(clicon_handle h, 
	     char         *db)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_delete_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_delete_fn(xh, db);
 done:
    return retval;
}

/*! Initialize database. Open database for writing.
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @retval  0  OK
 * @retval -1  Error
 */
int 
xmldb_init(clicon_handle h, 
	   char         *db)
{
    int          retval = -1;
    xmldb_handle xh;

    if (_xa_api == NULL){
	clicon_err(OE_DB, 0, "No xmldb plugin");
	goto done;
    }
    if (_xa_api->xa_init_fn == NULL){
	clicon_err(OE_DB, 0, "No xmldb function");
	goto done;
    }
    if ((xh = clicon_handle_xmldb_get(h)) == NULL){
	clicon_err(OE_DB, 0, "Not connected to datastore plugin");
	goto done;
    }
    retval = _xa_api->xa_init_fn(xh, db);
 done:
    return retval;
}
