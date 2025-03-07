 /*
 *
  ***** BEGIN LICENSE BLOCK *****
 
# Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
# Copyright (C) 2017-2019 Olof Hagsand
# Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * Clixon Datastore (XMLDB)
 * Saves Clixon data as clear-text XML (or JSON)
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
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_string.h"
#include "clixon_file.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_netconf_lib.h"
#include "clixon_xml_bind.h"
#include "clixon_xml_default.h"
#include "clixon_xml_io.h"
#include "clixon_json.h"
#include "clixon_datastore.h"
#include "clixon_datastore_write.h"
#include "clixon_datastore_read.h"

/*! Translate from symbolic database name to actual filename in file-system
 *
 * @param[in]   th       text handle handle
 * @param[in]   db       Symbolic database name, eg "candidate", "running"
 * @param[out]  filename Filename. Unallocate after use with free()
 * @retval      0        OK
 * @retval     -1        Error
 * @note Could need a way to extend which databases exists, eg to register new.
 * The currently allowed databases are: 
 *   candidate, tmp, running, result
 * The filename reside in CLICON_XMLDB_DIR option
 */
int
xmldb_db2file(clixon_handle  h, 
              const char    *db,
              char         **filename)
{
    int   retval = -1;
    cbuf *cb = NULL;
    char *dir;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    if ((dir = clicon_xmldb_dir(h)) == NULL){
        clixon_err(OE_XML, errno, "dbdir not set");
        goto done;
    }
    cprintf(cb, "%s/%s_db", dir, db);
    if ((*filename = strdup4(cbuf_get(cb))) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Connect to a datastore plugin, allocate resources to be used in API calls
 *
 * @param[in]  h    Clixon handle
 * @retval     0    OK
 * @retval    -1    Error
 */
int
xmldb_connect(clixon_handle h)
{
    return 0;
}

/*! Disconnect from a datastore plugin and deallocate resources
 *
 * @param[in]  handle  Disconect and deallocate from this handle
 * @retval     0       OK
 * @retval    -1    Error
 */
int
xmldb_disconnect(clixon_handle h)
{
    int       retval = -1;
    char    **keys = NULL;
    size_t    klen;
    int       i;
    db_elmnt *de;
    
    if (clicon_hash_keys(clicon_db_elmnt(h), &keys, &klen) < 0)
        goto done;
    for(i = 0; i < klen; i++) 
        if ((de = clicon_hash_value(clicon_db_elmnt(h), keys[i], NULL)) != NULL){
            if (de->de_xml){
                xml_free(de->de_xml);
                de->de_xml = NULL;
            }
        }
    retval = 0;
 done:
    if (keys)
        free(keys);
    return retval;
}

/*! Copy database from db1 to db2
 *
 * @param[in]  h     Clixon handle
 * @param[in]  from  Source database
 * @param[in]  to    Destination database
 * @retval     0     OK
 * @retval    -1     Error
  */
int 
xmldb_copy(clixon_handle h, 
           const char   *from, 
           const char   *to)
{
    int                 retval = -1;
    char               *fromfile = NULL;
    char               *tofile = NULL;
    db_elmnt           *de1 = NULL; /* from */
    db_elmnt           *de2 = NULL; /* to */
    db_elmnt            de0 = {0,};
    cxobj              *x1 = NULL;  /* from */
    cxobj              *x2 = NULL;  /* to */

    clixon_debug(CLIXON_DBG_DATASTORE, "%s %s", from, to);
    /* XXX lock */
    /* Copy in-memory cache */
    /* 1. "to" xml tree in x1 */
    if ((de1 = clicon_db_elmnt_get(h, from)) != NULL)
        x1 = de1->de_xml;
    if ((de2 = clicon_db_elmnt_get(h, to)) != NULL)
        x2 = de2->de_xml;
    if (x1 == NULL && x2 == NULL){
        /* do nothing */
    }
    else if (x1 == NULL){  /* free x2 and set to NULL */
        xml_free(x2);
        x2 = NULL;
    }
    else  if (x2 == NULL){ /* create x2 and copy from x1 */
        if ((x2 = xml_new(xml_name(x1), NULL, CX_ELMNT)) == NULL)
            goto done;
        xml_flag_set(x2, XML_FLAG_TOP);
        if (xml_copy(x1, x2) < 0) 
            goto done;
    }
    else{ /* copy x1 to x2 */
        xml_free(x2);
        if ((x2 = xml_new(xml_name(x1), NULL, CX_ELMNT)) == NULL)
            goto done;
        xml_flag_set(x2, XML_FLAG_TOP);
        if (xml_copy(x1, x2) < 0) 
            goto done;
    }
    /* always set cache although not strictly necessary in case 1
     * above, but logic gets complicated due to differences with
     * de and de->de_xml */
    if (de2)
        de0 = *de2;
    de0.de_xml = x2; /* The new tree */
    clicon_db_elmnt_set(h, to, &de0);

    /* Copy the files themselves (above only in-memory cache) */
    if (xmldb_db2file(h, from, &fromfile) < 0)
        goto done;
    if (xmldb_db2file(h, to, &tofile) < 0)
        goto done;
    if (clicon_file_copy(fromfile, tofile) < 0)
        goto done;
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DATASTORE, "retval:%d", retval);
    if (fromfile)
        free(fromfile);
    if (tofile)
        free(tofile);
    return retval;
}

/*! Lock database
 *
 * @param[in]  h    Clixon handle
 * @param[in]  db   Database
 * @param[in]  id   Session id
 * @retval     0    OK
 * @retval    -1    Error
 */
int 
xmldb_lock(clixon_handle h, 
           const char   *db, 
           uint32_t      id)
{
    db_elmnt  *de = NULL;
    db_elmnt   de0 = {0,};

    if ((de = clicon_db_elmnt_get(h, db)) != NULL)
        de0 = *de;
    de0.de_id = id;
    gettimeofday(&de0.de_tv, NULL);
    clicon_db_elmnt_set(h, db, &de0);
    clixon_debug(CLIXON_DBG_DATASTORE, "%s: locked by %u",  db, id);
    return 0;
}

/*! Unlock database
 *
 * @param[in]  h   Clixon handle
 * @param[in]  db  Database
 * @retval     0   OK
 * @retval    -1   Error
 * Assume all sanity checks have been made
 */
int
xmldb_unlock(clixon_handle h,
             const char   *db)
{
    db_elmnt  *de = NULL;

    if ((de = clicon_db_elmnt_get(h, db)) != NULL){
        de->de_id = 0;
        memset(&de->de_tv, 0, sizeof(struct timeval));
        clicon_db_elmnt_set(h, db, de);
    }
    return 0;
}

/*! Unlock all databases locked by session-id (eg process dies) 
 *
 * @param[in]  h   Clixon handle
 * @param[in]  id  Session id
 * @retval     0   OK
 * @retval    -1   Error
 */
int
xmldb_unlock_all(clixon_handle h,
                 uint32_t      id)
{
    int       retval = -1;
    char    **keys = NULL;
    size_t    klen;
    int       i;
    db_elmnt *de;

    /* get all db:s */
    if (clicon_hash_keys(clicon_db_elmnt(h), &keys, &klen) < 0)
        goto done;
    /* Identify the ones locked by client id */
    for (i = 0; i < klen; i++) {
        if ((de = clicon_db_elmnt_get(h, keys[i])) != NULL &&
            de->de_id == id){
            de->de_id = 0;
            memset(&de->de_tv, 0, sizeof(struct timeval));
            clicon_db_elmnt_set(h, keys[i], de);
        }
    }
    retval = 0;
 done:
    if (keys)
        free(keys);
    return retval;
}

/*! Check if database is locked
 *
 * @param[in] h   Clixon handle
 * @param[in] db  Database
 * @retval    >0  Session id of locker
 * @retval    0   Not locked
 * @retval   -1   Error
 */
uint32_t
xmldb_islocked(clixon_handle h,
               const char   *db)
{
    db_elmnt  *de;

    if ((de = clicon_db_elmnt_get(h, db)) == NULL)
        return 0;
    return de->de_id;
}

/*! Get timestamp of when database was locked
 *
 * @param[in]  h   Clixon handle
 * @param[in]  db  Database
 * @param[out] tv  Timestamp
 * @retval     0   OK
 * @retval    -1   No timestamp / not locked
 */
int
xmldb_lock_timestamp(clixon_handle   h,
                     const char     *db,
                     struct timeval *tv)
{
    db_elmnt  *de;

    if ((de = clicon_db_elmnt_get(h, db)) == NULL)
        return -1;
    memcpy(tv, &de->de_tv, sizeof(*tv));
    return 0;
}

/*! Check if db exists or is empty
 *
 * @param[in]  h   Clixon handle
 * @param[in]  db  Database
 * @retval     1   Yes it exists
 * @retval     0   No it does not exist
 * @retval    -1   Error
 * @note  An empty datastore is treated as not existent so that a backend after dropping priviliges can re-create it
 */
int
xmldb_exists(clixon_handle h,
             const char   *db)
{
    int                 retval = -1;
    char               *filename = NULL;
    struct stat         sb;

    clixon_debug(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, "%s", db);
    if (xmldb_db2file(h, db, &filename) < 0)
        goto done;
    if (lstat(filename, &sb) < 0)
        retval = 0;
    else{
        if (sb.st_size == 0)
            retval = 0;
        else
            retval = 1;
    }
 done:
    clixon_debug(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, "retval:%d", retval);
    if (filename)
        free(filename);
    return retval;
}

/*! Clear database cache if any for mem/size optimization only, not file itself
 *
 * @param[in]  h   Clixon handle
 * @param[in]  db  Database
 * @retval     0   OK
 * @retval    -1   Error
 */
int
xmldb_clear(clixon_handle h,
            const char   *db)
{
    cxobj    *xt = NULL;
    db_elmnt *de = NULL;

    if ((de = clicon_db_elmnt_get(h, db)) != NULL){
        if ((xt = de->de_xml) != NULL){
            xml_free(xt);
            de->de_xml = NULL;
        }
    }
    return 0;
}

/*! Delete database, clear cache if any. Remove file 
 *
 * @param[in]  h   Clixon handle
 * @param[in]  db  Database
 * @retval     0   OK
 * @retval    -1   Error

 * @note  Datastore is not actually deleted so that a backend after dropping priviliges can re-create it
 */
int
xmldb_delete(clixon_handle h,
             const char   *db)
{
    int         retval = -1;
    char       *filename = NULL;
    struct stat sb;

    clixon_debug(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, "%s", db);
    if (xmldb_clear(h, db) < 0)
        goto done;
    if (xmldb_db2file(h, db, &filename) < 0)
        goto done;
    if (lstat(filename, &sb) == 0)
        if (truncate(filename, 0) < 0){
            clixon_err(OE_DB, errno, "truncate %s", filename);
            goto done;
        }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, "retval:%d", retval);
    if (filename)
        free(filename);
    return retval;
}

/*! Create a database. Open database for writing.
 *
 * @param[in]  h   Clixon handle
 * @param[in]  db  Database
 * @retval     0   OK
 * @retval    -1   Error
 */
int
xmldb_create(clixon_handle h,
             const char   *db)
{
    int                 retval = -1;
    char               *filename = NULL;
    int                 fd = -1;
    db_elmnt           *de = NULL;
    cxobj              *xt = NULL;

    clixon_debug(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, "%s", db);
    if ((de = clicon_db_elmnt_get(h, db)) != NULL){
        if ((xt = de->de_xml) != NULL){
            xml_free(xt);
            de->de_xml = NULL;
        }
    }
    if (xmldb_db2file(h, db, &filename) < 0)
        goto done;
    if ((fd = open(filename, O_CREAT|O_WRONLY, S_IRWXU)) == -1) {
        clixon_err(OE_UNIX, errno, "open(%s)", filename);
        goto done;
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, "retval:%d", retval);
    if (filename)
        free(filename);
    if (fd != -1)
        close(fd);
    return retval;
}

/*! Create an XML database. If it exists already, delete it before creating
 *
 * Utility function.
 * @param[in]  h   Clixon handle
 * @param[in]  db  Symbolic database name, eg "candidate", "running"
 * @retval     0   OK
 * @retval    -1   Error
 */
int
xmldb_db_reset(clixon_handle h,
               const char   *db)
{
    if (xmldb_exists(h, db) == 1){
        if (xmldb_delete(h, db) != 0 && errno != ENOENT)
            return -1;
    }
    if (xmldb_create(h, db) < 0)
        return -1;
    return 0;
}

/*! Get datastore XML cache
 *
 * @param[in]  h    Clixon handle
 * @param[in]  db   Database name
 * @retval     xml  XML cached tree or NULL
 */
cxobj *
xmldb_cache_get(clixon_handle h,
                const char   *db)
{
    db_elmnt *de;

    if ((de = clicon_db_elmnt_get(h, db)) == NULL)
        return NULL;
    return de->de_xml;
}

/*! Get modified flag from datastore
 *
 * @param[in]  h     Clixon handle
 * @param[in]  db    Database name
 * @retval     1     Db is modified
 * @retval     0     Db is not modified
 * @retval    -1     Error (datastore does not exist)
 * @note This only makes sense for "candidate", see RFC 6241 Sec 7.5
 * @note This only works if db cache is used,...
 */
int
xmldb_modified_get(clixon_handle h,
                   const char   *db)
{
    db_elmnt *de;

    if ((de = clicon_db_elmnt_get(h, db)) == NULL){
        clixon_err(OE_CFG, EFAULT, "datastore %s does not exist", db);
        return -1;
    }
    return de->de_modified;
}

/*! Get empty flag from datastore (the datastore was empty ON LOAD)
 *
 * @param[in]  h     Clixon handle
 * @param[in]  db    Database name
 * @retval     1     Db was empty on load
 * @retval     0     Db was not empty on load
 * @retval    -1     Error (datastore does not exist)
 */
int
xmldb_empty_get(clixon_handle h,
                const char   *db)
{
    db_elmnt *de;

    if ((de = clicon_db_elmnt_get(h, db)) == NULL){
        clixon_err(OE_CFG, EFAULT, "datastore %s does not exist", db);
        return -1;
    }
    return de->de_empty;
}

/*! Set modified flag from datastore
 *
 * @param[in]  h     Clixon handle
 * @param[in]  db    Database name
 * @param[in]  value 0 or 1
 * @retval     0     OK
 * @retval    -1     Error (datastore does not exist)
 * @note This only makes sense for "candidate", see RFC 6241 Sec 7.5
 * @note This only works if db cache is used,...
 */
int
xmldb_modified_set(clixon_handle h,
                   const char   *db,
                   int           value)
{
    db_elmnt *de;

    if ((de = clicon_db_elmnt_get(h, db)) == NULL){
        clixon_err(OE_CFG, EFAULT, "datastore %s does not exist", db);
        return -1;
    }
    de->de_modified = value;
    return 0;
}

/* Print the datastore meta-info to file
 */
int
xmldb_print(clixon_handle h,
            FILE         *f)
{
    int       retval = -1;
    db_elmnt *de = NULL;
    char    **keys = NULL;
    size_t    klen;
    int       i;

    if (clicon_hash_keys(clicon_db_elmnt(h), &keys, &klen) < 0)
        goto done;
    for (i = 0; i < klen; i++){
        /* XXX name */
        if ((de = clicon_db_elmnt_get(h, keys[i])) == NULL)
            continue;
        fprintf(f, "Datastore:  %s\n", keys[i]);
        fprintf(f, "  Session:  %u\n", de->de_id);
        fprintf(f, "  XML:      %p\n", de->de_xml);
        fprintf(f, "  Modified: %d\n", de->de_modified);
        fprintf(f, "  Empty:    %d\n", de->de_empty);
    }
    retval = 0;
 done:
    return retval;
}

/*! Rename an XML database
 *
 * @param[in]  h        Clixon handle
 * @param[in]  db       Database name
 * @param[in]  newdb    New Database name; if NULL, then same as old
 * @param[in]  suffix   Suffix to append to new database name
 * @retval     0        OK
 * @retval    -1        Error
 * @note if newdb and suffix are null, OK is returned as it is a no-op
 */
int
xmldb_rename(clixon_handle h,
             const char    *db,
             const char    *newdb,
             const char    *suffix)
{
    int    retval = -1;
    char  *old;
    char  *fname = NULL;
    cbuf  *cb = NULL;

    if ((xmldb_db2file(h, db, &old)) < 0)
        goto done;
    if (newdb == NULL && suffix == NULL)        // no-op
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s", newdb == NULL ? old : newdb);
    if (suffix)
        cprintf(cb, "%s", suffix);
    fname = cbuf_get(cb);
    if ((rename(old, fname)) < 0) {
        clixon_err(OE_UNIX, errno, "rename: %s", strerror(errno));
        goto done;
    };
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (old)
        free(old);
    return retval;
}

/*! Given a datastore, populate its cache with yang binding and default values
 *
 * @param[in]  h      Clixon handle
 * @param[in]  db     Name of database to search in (filename including dir path
 * @retval     1      OK
 * @retval     0      YANG assigment and default assignment not made
 * @retval    -1      General error, check specific clicon_errno, clicon_suberrno
 */
int
xmldb_populate(clixon_handle h,
               const char   *db)
{
    int        retval = -1;
    cxobj     *x;
    yang_stmt *yspec;
    int        ret;

    if ((x = xmldb_cache_get(h, db)) == NULL){
        clixon_err(OE_XML, 0, "XML cache not found");
        goto done;
    }
    yspec = clicon_dbspec_yang(h);
    if ((ret = xml_bind_yang(h, x, YB_MODULE, yspec, NULL)) < 0)
        goto done;
    if (ret == 1){
        /* Add default global values (to make xpath below include defaults) */
        if (xml_global_defaults(h, x, NULL, "/", yspec, 0) < 0)
            goto done;
        /* Add default recursive values */
        if (xml_default_recurse(x, 0, 0) < 0)
            goto done;
    }
    retval = ret;
 done:
    return retval;
}

/* Dump a datastore to file, add modstate
 *
 * @param[in]  h   Clixon handle
 * @param[in]  db  Name of database to search in (filename including dir path
 * @param[in]  xt  Top of XML tree
 * @param[in]  wdef     With-defaults parameter
 * @retval     0   OK
 * @retval    -1   Error
 */
int
xmldb_dump(clixon_handle     h,
           FILE             *f,
           cxobj            *xt,
           withdefaults_type wdef)
{
    int    retval = -1;
    cxobj *xm;
    cxobj *xmodst = NULL;
    char  *format;
    int    pretty;

    /* Add modstate first */
    if ((xm = clicon_modst_cache_get(h, 1)) != NULL){
        if ((xmodst = xml_dup(xm)) == NULL)
            goto done;
        if (xml_child_insert_pos(xt, xmodst, 0) < 0)
            goto done;
        xml_parent_set(xmodst, xt);
    }
    if ((format = clicon_option_str(h, "CLICON_XMLDB_FORMAT")) == NULL){
        clixon_err(OE_CFG, ENOENT, "No CLICON_XMLDB_FORMAT");
        goto done;
    }
    pretty = clicon_option_bool(h, "CLICON_XMLDB_PRETTY");
    if (strcmp(format,"json")==0){
        if (clixon_json2file(f, xt, pretty, fprintf, 0, 0) < 0)
            goto done;
    }
    else if (clixon_xml2file1(f, xt, 0, pretty, NULL, fprintf, 0, 0, wdef) < 0)
        goto done;
    /* Remove modules state after writing to file */
    if (xmodst && xml_purge(xmodst) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Given a datastore, write the cache to file
 *
 * Also add mod-state if applicable
 * @param[in]  h   Clixon handle
 * @param[in]  db  Name of database to search in (filename including dir path
 * @retval     0   OK
 * @retval    -1   Error
 */
int
xmldb_write_cache2file(clixon_handle h,
                       const char   *db)
{
    int         retval = -1;
    cxobj      *xt;
    FILE       *f = NULL;
    char       *dbfile = NULL;

    if (xmldb_db2file(h, db, &dbfile) < 0)
        goto done;
    if (dbfile==NULL){
        clixon_err(OE_XML, 0, "dbfile NULL");
        goto done;
    }
    if ((xt = xmldb_cache_get(h, db)) == NULL){
        clixon_err(OE_XML, 0, "XML cache not found");
        goto done;
    }
    if ((f = fopen(dbfile, "w")) == NULL){
        clixon_err(OE_CFG, errno, "Creating file %s", dbfile);
        goto done;
    }
    if (xmldb_dump(h, f, xt, WITHDEFAULTS_EXPLICIT) < 0)
        goto done;
    if (f) {
        fclose(f);
        f = NULL;
    }
    retval = 0;
 done:
    if (dbfile)
        free(dbfile);
    if (f)
        fclose(f);
    return retval;
}

