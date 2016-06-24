/*
   Bacula(R) - The Network Backup Solution

   Copyright (C) 2000-2015 Kern Sibbald
   Copyright (C) 2004-2014 Free Software Foundation Europe e.V.

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
 * Properties we use for getting and setting ACLs.
 */

#ifndef __BACL_H_
#define __BACL_H_

/* Global JCR data */
struct acl_ctx_t  {
   uint32_t nr_errors;
   uint32_t flags;              /* See BACL_FLAG_* */
   uint32_t current_dev;
   uint32_t content_length;
   POOLMEM *content;
};

/*
 * We support the following types of ACLs
 */
typedef enum {
   BACL_TYPE_NONE        = 0,
   BACL_TYPE_ACCESS      = 1,
   BACL_TYPE_DEFAULT     = 2,
   BACL_TYPE_DEFAULT_DIR = 3,
   BACL_TYPE_EXTENDED    = 4,
   BACL_TYPE_NFS4        = 5
} bacl_type;

#define BACL_FLAG_SAVE_NATIVE     0x01
#define BACL_FLAG_SAVE_AFS        0x02
#define BACL_FLAG_RESTORE_NATIVE  0x04
#define BACL_FLAG_RESTORE_AFS     0x08

/*
 * Ensure we have none
 */
#ifndef ACL_TYPE_NONE
#define ACL_TYPE_NONE 0x0
#endif
 
#ifdef HAVE_IRIX_OS
#define BACL_ENOTSUP  ENOSYS
#else
#define BACL_ENOTSUP  EOPNOTSUPP
#endif /* HAVE_IRIX_OS */

#endif /* __BACL_H_ */
