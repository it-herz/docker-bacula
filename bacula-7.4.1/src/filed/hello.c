/*
   Bacula(R) - The Network Backup Solution

   Copyright (C) 2000-2016 Kern Sibbald

   The original author of Bacula is Kern Sibbald, with contributions
   from many others, a complete list can be found in the file AUTHORS.

   You may use this file and others of this release according to the
   license defined in the LICENSE file, which includes the Affero General
   Public License, v3.0 ("AGPLv3") and some additional permissions and
   terms pursuant to its AGPLv3 Section 7.

   This notice must be preserved when any source code is 
   conveyed and/or propagated.

   Bacula(R) is a registered trademark of Kern Sibbald.
*/
/*
 * Authenticate Director who is attempting to connect.
 *
 *   Kern Sibbald, October 2000
 *
 */

#include "bacula.h"
#include "filed.h"

extern CLIENT *me;                 /* my resource */

const int dbglvl = 50;

/* FD_VERSION history
 *   None prior to 10Mar08
 *   1 10Mar08
 *   2 13Mar09 - added the ability to restore from multiple storages
 *   3 03Sep10 - added the restore object command
 *   4 25Nov10 - added bandwidth command 5.1
 *   5 24Nov11 - added new restore object command format (pluginname)
 *   6 15Feb12 - added Component selection information list
 *   7 19Feb12 - added Expected files to restore
 *   8 22Mar13 - added restore options + version for SD
 *   9 06Aug13 - skipped
 *  10 01Jan14 - added SD Calls Client
 *  11 O4May14 - skipped
 *  12 22Jun14 - skipped
 * 213 04Feb15 - added snapshot protocol with the DIR
 */

#define FD_VERSION 213 /* FD version */

static char hello_sd[]  = "Hello Bacula SD: Start Job %s %d\n";
static char hello_dir[] = "2000 OK Hello %d\n";
static char sorry_dir[] = "2999 Authentication failed.\n";

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*********************************************************************
 *
 * Validate hello from the Director
 *
 * Returns: true  if Hello is good.
 *          false if Hello is bad.
 */
bool validate_dir_hello(JCR *jcr)
{
   POOLMEM *dirname;
   DIRRES *director = NULL;
   int dir_version = 0;
   BSOCK *dir = jcr->dir_bsock;
   bool auth_success = false;

   if (dir->msglen < 25 || dir->msglen > 500) {
      Dmsg2(dbglvl, "Bad Hello command from Director at %s. Len=%d.\n",
            dir->who(), dir->msglen);
      Jmsg2(jcr, M_FATAL, 0, _("Bad Hello command from Director at %s. Len=%d.\n"),
            dir->who(), dir->msglen);
      return false;
   }
   dirname = get_pool_memory(PM_MESSAGE);
   dirname = check_pool_memory_size(dirname, dir->msglen);

   if (sscanf(dir->msg, "Hello Director %127s calling %d", dirname, &dir_version) != 2 &&
       sscanf(dir->msg, "Hello Director %127s calling", dirname) != 1) {
      char addr[64];
      char *who = dir->get_peer(addr, sizeof(addr)) ? dir->who() : addr;
      dir->msg[100] = 0;
      Dmsg2(dbglvl, "Bad Hello command from Director at %s: %s\n",
            dir->who(), dir->msg);
      Jmsg2(jcr, M_FATAL, 0, _("Bad Hello command from Director at %s: %s\n"),
            who, dir->msg);
      goto auth_fatal;
   }
   unbash_spaces(dirname);
   foreach_res(director, R_DIRECTOR) {
      if (strcmp(director->hdr.name, dirname) == 0)
         break;
   }
   if (!director) {
      char addr[64];
      char *who = dir->get_peer(addr, sizeof(addr)) ? dir->who() : addr;
      Jmsg2(jcr, M_FATAL, 0, _("Connection from unknown Director %s at %s rejected.\n"),
            dirname, who);
      goto auth_fatal;
   }
   auth_success = true;

auth_fatal:
   free_pool_memory(dirname);
   jcr->director = director;
   /* Single thread all failures to avoid DOS */
   if (!auth_success) {
      P(mutex);
      bmicrosleep(6, 0);
      V(mutex);
   }
   return auth_success;
}

/*
 * Note, we handle the initial connection request here.
 *   We only get the jobname and the SD version, then we
 *   return, authentication will be done when the Director
 *   sends the storage command -- as is usually the case.
 *   This should be called only once by the SD.
 */
void *handle_storage_connection(BSOCK *sd)
{
   char job_name[500];
   char tbuf[150];
   int sd_version = 0;
   JCR *jcr;

   if (sscanf(sd->msg, "Hello FD: Bacula Storage calling Start Job %127s %d",
       job_name, &sd_version) != 2) {
      Jmsg(NULL, M_FATAL, 0, _("SD connect failed: Bad Hello command\n"));
      return NULL;
   }
   Dmsg1(110, "Got a SD connection at %s\n", bstrftimes(tbuf, sizeof(tbuf),
         (utime_t)time(NULL)));
   Dmsg1(50, "%s", sd->msg);

   if (!(jcr=get_jcr_by_full_name(job_name))) {
      Jmsg1(NULL, M_FATAL, 0, _("SD connect failed: Job name not found: %s\n"), job_name);
      Dmsg1(3, "**** Job \"%s\" not found.\n", job_name);
      sd->destroy();
      return NULL;
   }
   set_jcr_in_tsd(jcr);
   Dmsg1(150, "Found Job %s\n", job_name);

   jcr->lock_auth();
   /* We already have a socket connected, just discard it */
   if (jcr->sd_calls_client_bsock) {
      Qmsg1(jcr, M_WARNING, 0, _("SD \"%s\" tried to connect two times.\n"), sd->who());
      free_bsock(sd);
      /* will exit just after the unlock() */

   } else {
      /* If we have a previous socket in store_bsock, we are in multi restore mode */
      jcr->sd_calls_client_bsock = sd;
      sd->set_jcr(jcr);
   }
   jcr->unlock_auth();

   if (!sd) {                   /* freed by free_bsock(), connection already done */
      free_jcr(jcr);
      return NULL;
   }

   if (!jcr->max_bandwidth) {
      if (jcr->director->max_bandwidth_per_job) {
         jcr->max_bandwidth = jcr->director->max_bandwidth_per_job;

      } else if (me->max_bandwidth_per_job) {
         jcr->max_bandwidth = me->max_bandwidth_per_job;
      }
   }
   sd->set_bwlimit(jcr->max_bandwidth);

   pthread_cond_signal(&jcr->job_start_wait); /* wake waiting job */
   free_jcr(jcr);
   return NULL;
}


/*
 * Send Hello OK to DIR
 */
bool send_hello_ok(BSOCK *bs)
{
   return bs->fsend(hello_dir, FD_VERSION);
}

bool send_sorry(BSOCK *bs)
{
   return bs->fsend(sorry_dir);
}

/*
 * Send Hello to SD
 */
bool send_hello_sd(JCR *jcr, char *Job)
{
   bool rtn;
   BSOCK *sd = jcr->store_bsock;

   bash_spaces(Job);
   rtn = sd->fsend(hello_sd, Job, FD_VERSION);
   unbash_spaces(Job);
   Dmsg1(100, "Send to SD: %s\n", sd->msg);
   return rtn;
}
