/* quota.c -- program to report/reconstruct quotas
 *
 * Copyright (c) 1998-2003 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

/* $Id: quota.c,v 1.48.2.8 2004/12/23 18:14:38 ken3 Exp $ */


#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <syslog.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/stat.h>

#if HAVE_DIRENT_H
# include <dirent.h>
# define NAMLEN(dirent) strlen((dirent)->d_name)
#else
# define dirent direct
# define NAMLEN(dirent) (dirent)->d_namlen
# if HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# if HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# if HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

#include "assert.h"
#include "cyrusdb.h"
#include "global.h"
#include "exitcodes.h"
#include "imap_err.h"
#include "mailbox.h"
#include "xmalloc.h"
#include "mboxlist.h"
#include "mboxname.h"
#include "quota.h"
#include "convert_code.h"
#include "util.h"

extern int optind;
extern char *optarg;

/* current namespace */
static struct namespace quota_namespace;

/* config.c stuff */
const int config_need_data = CONFIG_NEED_PARTITION_DATA;

/* forward declarations */
void usage(void);
void reportquota(void);
int buildquotalist(char *domain, char **roots, int nroots);
int fixquota_mailbox(char *name,
		     int matchlen,
		     int maycreate,
		     void *rock);
int fixquota(char *domain, int ispartial);
int fixquota_fixroot(struct mailbox *mailbox,
		     char *root);
int fixquota_finish(int thisquota, struct txn **tid, unsigned long *count);

struct fix_rock {
    char *domain;
    struct txn **tid;
    unsigned long change_count;
};

struct quotaentry {
    struct quota quota;
    int refcount;
    int deleted;
    uquota_t newused;
};

#define QUOTAGROW 300

struct quotaentry *quota;
int quota_num = 0, quota_alloc = 0;

int firstquota;
int redofix;
int partial;

int main(int argc,char **argv)
{
    int opt;
    int fflag = 0;
    int r, code = 0;
    char *alt_config = NULL, *domain = NULL;

    if (geteuid() == 0) fatal("must run as the Cyrus user", EC_USAGE);

    while ((opt = getopt(argc, argv, "C:d:f")) != EOF) {
	switch (opt) {
	case 'C': /* alt config file */
	    alt_config = optarg;
	    break;

	case 'd':
	    domain = optarg;
	    break;

	case 'f':
	    fflag = 1;
	    break;

	default:
	    usage();
	}
    }

    cyrus_init(alt_config, "quota", 0);

    /* Set namespace -- force standard (internal) */
    if ((r = mboxname_init_namespace(&quota_namespace, 1)) != 0) {
	syslog(LOG_ERR, error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }

    quotadb_init(0);
    quotadb_open(NULL);

    if (!r) r = buildquotalist(domain, argv+optind, argc-optind);

    if (!r && fflag) {
	mboxlist_init(0);
	r = fixquota(domain, argc-optind);
	mboxlist_done();
    }

    quotadb_close();
    quotadb_done();

    if (!r) reportquota();

    if (r) {
	com_err("quota", r, (r == IMAP_IOERROR) ? error_message(errno) : NULL);
	code = convert_code(r);
    }

    cyrus_done();

    return code;
}

void usage(void)
{
    fprintf(stderr, "usage: quota [-C <alt_config>] [-d <domain>] [-f] [prefix]...\n");
    exit(EC_USAGE);
}    

struct find_rock {
    char **roots;
    int nroots;
};

static int find_p(void *rockp,
		  const char *key, int keylen,
		  const char *data __attribute__((unused)),
		  int datalen __attribute__((unused)))
{
    struct find_rock *frock = (struct find_rock *) rockp;
    int i;

    /* If restricting our list, see if this quota root matches */
    if (frock->nroots) {
	const char *p;

	/* skip over domain */
	if (config_virtdomains && (p = strchr(key, '!'))) {
	    keylen -= (++p - key);
	    key = p;
	}

 	for (i = 0; i < frock->nroots; i++) {
	    if (keylen >= strlen(frock->roots[i]) &&
		!strncmp(key, frock->roots[i], strlen(frock->roots[i]))) {
		break;
	    }
	}
	if (i == frock->nroots) return 0;
    }

    return 1;
}

/*
 * Add a quota root to the list in 'quota'
 */
static int find_cb(void *rockp __attribute__((unused)),
		   const char *key, int keylen,
		   const char *data, int datalen __attribute__((unused)))
{
    if (!data) return 0;

    if (quota_num == quota_alloc) {
	quota_alloc += QUOTAGROW;
	quota = (struct quotaentry *)
	  xrealloc((char *)quota, quota_alloc * sizeof(struct quotaentry));
    }
    memset(&quota[quota_num], 0, sizeof(struct quotaentry));
    quota[quota_num].quota.root = xstrndup(key, keylen);
    sscanf(data, UQUOTA_T_FMT " %d",
	   &quota[quota_num].quota.used, &quota[quota_num].quota.limit);
  
    quota_num++;

    return 0;
}

/*
 * Build the list of quota roots in 'quota'
 */
int buildquotalist(char *domain, char **roots, int nroots)
{
    int i;
    char buf[MAX_MAILBOX_NAME+1];
    struct find_rock frock;

    frock.roots = roots;
    frock.nroots = nroots;

    /* Translate separator in mailboxnames.
     *
     * We do this directly instead of using the mboxname_tointernal()
     * function pointer because we know that we are using the internal
     * namespace and so we don't have to allocate a buffer for the
     * translated name.
     */
    for (i = 0; i < nroots; i++) {
	mboxname_hiersep_tointernal(&quota_namespace, roots[i], 0);
    }

    buf[0] = '\0';
    if (domain) snprintf(buf, sizeof(buf), "%s!", domain);

    /* if we have exactly one root specified, narrow the search */
    if (nroots == 1) strlcat(buf, roots[0], sizeof(buf));
	    
    config_quota_db->foreach(qdb, buf, strlen(buf),
			     &find_p, &find_cb, &frock, NULL);

    return 0;
}

/*
 * Account for mailbox 'name' when fixing the quota roots
 */
int fixquota_mailbox(char *name,
		     int matchlen __attribute__((unused)),
		     int maycreate __attribute__((unused)),
		     void* rock)
{
    int r;
    struct mailbox mailbox;
    int i, len, thisquota, thisquotalen;
    struct fix_rock *frock = (struct fix_rock *) rock;
    char *p, *domain = frock->domain;

    /* make sure the domains match */
    if (domain &&
	(!(p = strchr(name, '!')) || (p - name) != strlen(domain) ||
	 strncmp(name, domain, p - name))) {
	return 0;
    }

    while (firstquota < quota_num &&
	   strncmp(name, quota[firstquota].quota.root,
		       strlen(quota[firstquota].quota.root)) > 0) {
	r = fixquota_finish(firstquota++, frock->tid, &frock->change_count);
	if (r) return r;
    }

    thisquota = -1;
    thisquotalen = 0;
    for (i = firstquota;
	 i < quota_num && strcmp(name, quota[i].quota.root) >= 0; i++) {
	len = strlen(quota[i].quota.root);
	if (!strncmp(name, quota[i].quota.root, len) &&
	    (!name[len] || name[len] == '.' ||
	     (domain && name[len-1] == '!'))) {
	    quota[i].refcount++;
	    if (len > thisquotalen) {
		thisquota = i;
		thisquotalen = len;
	    }
	}
    }

    if (partial && thisquota == -1) return 0;

    r = mailbox_open_header(name, 0, &mailbox);
    if (!r) {
	if (thisquota == -1) {
	    if (mailbox.quota.root) {
		r = fixquota_fixroot(&mailbox, (char *)0);
	    }
	}
	else {
	    if (!mailbox.quota.root ||
		strcmp(mailbox.quota.root, quota[thisquota].quota.root) != 0) {
		r = fixquota_fixroot(&mailbox, quota[thisquota].quota.root);
	    }
	    if (!r) r = mailbox_open_index(&mailbox);
   
	    if (!r) quota[thisquota].newused += mailbox.quota_mailbox_used;
	}

	mailbox_close(&mailbox);
    }

    if (r) {
	/* mailbox error of some type, commit what we have */
	quota_commit(frock->tid);
	*(frock->tid) = NULL;
    }

    return r;
}
	
int fixquota_fixroot(struct mailbox *mailbox,
		     char *root)
{
    int r;

    redofix = 1;

    r = mailbox_lock_header(mailbox);
    if (r) return r;

    printf("%s: quota root %s --> %s\n", mailbox->name,
	   mailbox->quota.root ? mailbox->quota.root : "(none)",
	   root ? root : "(none)");

    if (mailbox->quota.root) free(mailbox->quota.root);
    if (root) {
	mailbox->quota.root = xstrdup(root);
    }
    else {
	mailbox->quota.root = 0;
    }

    r = mailbox_write_header(mailbox);
    (void) mailbox_unlock_header(mailbox);
    return r;
}

/*
 * Finish fixing up a quota root
 */
int fixquota_finish(int thisquota, struct txn **tid, unsigned long *count)
{
    int r;

    if (!quota[thisquota].refcount) {
	if (!quota[thisquota].deleted++) {
	    printf("%s: removed\n", quota[thisquota].quota.root);
	    r = quota_delete(&quota[thisquota].quota, tid);
	    if (r) return r;
	    (*count)++;
	    free(quota[thisquota].quota.root);
	    quota[thisquota].quota.root = NULL;
	}
	return 0;
    }

    if (quota[thisquota].quota.used != quota[thisquota].newused) {
	/* re-read the quota with the record locked */
	r = quota_read(&quota[thisquota].quota, tid, 1);
	if (r) return r;
	(*count)++;
    }
    if (quota[thisquota].quota.used != quota[thisquota].newused) {
	printf("%s: usage was " UQUOTA_T_FMT ", now " UQUOTA_T_FMT "\n",
	       quota[thisquota].quota.root,
	       quota[thisquota].quota.used, quota[thisquota].newused);
	quota[thisquota].quota.used = quota[thisquota].newused;
	r = quota_write(&quota[thisquota].quota, tid);
	if (r) return r;
	(*count)++;
    }

    /* commit the transaction every 100 changes */
    if (*count && !(*count % 100)) {
	quota_commit(tid);
	*tid = NULL;
    }

    return 0;
}


/*
 * Fix all the quota roots
 */
int fixquota(char *domain, int ispartial)
{
    int r = 0;
    static char pattern[2] = "*";
    struct fix_rock frock;
    struct txn *tid = NULL;

    /*
     * Lock mailbox list to prevent mailbox creation/deletion
     * during the fix
     */
    mboxlist_open(NULL);

    frock.domain = domain;
    frock.tid = &tid;
    frock.change_count = 0;

    redofix = 1;
    while (!r && redofix) {
	redofix = 0;
	firstquota = 0;
	partial = ispartial;

	r = (*quota_namespace.mboxlist_findall)(&quota_namespace, pattern, 1,
						0, 0, fixquota_mailbox, &frock);
	while (!r && firstquota < quota_num) {
	    r = fixquota_finish(firstquota++, &tid, &frock.change_count);
	}
    }

    if (!r && tid) quota_commit(&tid);
    
    mboxlist_close();

    return 0;
}
    
/*
 * Print out the quota report
 */
void
reportquota(void)
{
    int i;
    char buf[MAX_MAILBOX_PATH+1];

    printf("   Quota   %% Used     Used Root\n");

    for (i = 0; i < quota_num; i++) {
	if (quota[i].deleted) continue;
	if (quota[i].quota.limit > 0) {
	    printf(" %7d " QUOTA_REPORT_FMT , quota[i].quota.limit,
		   ((quota[i].quota.used / QUOTA_UNITS) * 100) / quota[i].quota.limit);
	}
	else if (quota[i].quota.limit == 0) {
	    printf("       0        ");
	}
	else {
	    printf("                ");
	}
	/* Convert internal name to external */
	(*quota_namespace.mboxname_toexternal)(&quota_namespace,
					       quota[i].quota.root,
					       "cyrus", buf);
	printf(" " QUOTA_REPORT_FMT " %s\n", quota[i].quota.used / QUOTA_UNITS, buf);
    }
}