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
 *
 *   dev.c  -- low level operations on device (storage device)
 *
 *     written by, Kern Sibbald, MM
 *
 *     NOTE!!!! None of these routines are reentrant. You must
 *        use dev->rLock() and dev->Unlock() at a higher level,
 *        or use the xxx_device() equivalents.  By moving the
 *        thread synchronization to a higher level, we permit
 *        the higher level routines to "seize" the device and
 *        to carry out operations without worrying about who
 *        set what lock (i.e. race conditions).
 *
 *     Note, this is the device dependent code, and may have
 *           to be modified for each system, but is meant to
 *           be as "generic" as possible.
 *
 *     The purpose of this code is to develop a SIMPLE Storage
 *     daemon. More complicated coding (double buffering, writer
 *     thread, ...) is left for a later version.
 *
 */

/*
 * Handling I/O errors and end of tape conditions are a bit tricky.
 * This is how it is currently done when writing.
 * On either an I/O error or end of tape,
 * we will stop writing on the physical device (no I/O recovery is
 * attempted at least in this daemon). The state flag will be sent
 * to include ST_EOT, which is ephemeral, and ST_WEOT, which is
 * persistent. Lots of routines clear ST_EOT, but ST_WEOT is
 * cleared only when the problem goes away.  Now when ST_WEOT
 * is set all calls to write_block_to_device() call the fix_up
 * routine. In addition, all threads are blocked
 * from writing on the tape by calling lock_dev(), and thread other
 * than the first thread to hit the EOT will block on a condition
 * variable. The first thread to hit the EOT will continue to
 * be able to read and write the tape (he sort of tunnels through
 * the locking mechanism -- see lock_dev() for details).
 *
 * Now presumably somewhere higher in the chain of command
 * (device.c), someone will notice the EOT condition and
 * get a new tape up, get the tape label read, and mark
 * the label for rewriting. Then this higher level routine
 * will write the unwritten buffer to the new volume.
 * Finally, he will release
 * any blocked threads by doing a broadcast on the condition
 * variable.  At that point, we should be totally back in
 * business with no lost data.
 */

#include "bacula.h"
#include "stored.h"

#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

/* Imported functions */
extern void set_os_device_parameters(DCR *dcr);
extern bool dev_get_os_pos(DEVICE *dev, struct mtget *mt_stat);
extern uint32_t status_dev(DEVICE *dev);

/* Forward referenced functions */
const char *mode_to_str(int mode);
DEVICE *m_init_dev(JCR *jcr, DEVRES *device);
/*
 * Device types for printing
 */
static const char *prt_dev_types[] = {
   "*none*",
   "file",
   "tape",
   "DVD",
   "FIFO",
   "Vtape",
   "FTP",
   "VTL"
};

/*
 * Allocate and initialize the DEVICE structure
 * Note, if dev is non-NULL, it is already allocated,
 * thus we neither allocate it nor free it. This allows
 * the caller to put the packet in shared memory.
 *
 *  Note, for a tape, the device->device_name is the device name
 *     (e.g. /dev/nst0), and for a file, the device name
 *     is the directory in which the file will be placed.
 *
 */
DEVICE *init_dev(JCR *jcr, DEVRES *device)
{
   generate_global_plugin_event(bsdGlobalEventDeviceInit, device);
   DEVICE *dev = m_init_dev(jcr, device);
   return dev;
}

DEVICE *m_init_dev(JCR *jcr, DEVRES *device)
{
   struct stat statp;
   int errstat;
   DCR *dcr = NULL;
   DEVICE *dev = NULL;
   uint32_t max_bs;

   /* If no device type specified, try to guess */
   if (!device->dev_type) {
      /* Check that device is available */
      if (stat(device->device_name, &statp) < 0) {
         berrno be;
         Jmsg2(jcr, M_ERROR, 0, _("Unable to stat device %s: ERR=%s\n"),
            device->device_name, be.bstrerror());
         return NULL;
      }
      if (S_ISDIR(statp.st_mode)) {
         device->dev_type = B_FILE_DEV;
      } else if (S_ISCHR(statp.st_mode)) {
         device->dev_type = B_TAPE_DEV;
      } else if (S_ISFIFO(statp.st_mode)) {
         device->dev_type = B_FIFO_DEV;
#ifdef USE_VTAPE
      /* must set DeviceType = Vtape
       * in normal mode, autodetection is disabled
       */
      } else if (S_ISREG(statp.st_mode)) {
         device->dev_type = B_VTAPE_DEV;
#endif
      } else if (!(device->cap_bits & CAP_REQMOUNT)) {
         Jmsg2(jcr, M_ERROR, 0, _("%s is an unknown device type. Must be tape or directory\n"
               " or have RequiresMount=yes for DVD. st_mode=%x\n"),
            device->device_name, statp.st_mode);
         return NULL;
      } else {
         device->dev_type = B_DVD_DEV;
      }
      if (strcmp(device->device_name, "/dev/null") == 0) {
         device->dev_type = B_NULL_DEV;
      }
   }
   switch (device->dev_type) {
   case B_DVD_DEV:
      Jmsg0(jcr, M_FATAL, 0, _("DVD support is now deprecated.\n"));
      return NULL;
   case B_VTAPE_DEV:
      dev = New(vtape);
      break;
#ifdef USE_FTP
   case B_FTP_DEV:
      dev = New(ftp_device);
      break;
#endif
   case B_TAPE_DEV:
      dev = New(tape_dev);
      break;
   case B_FILE_DEV:
   case B_FIFO_DEV:
   case B_NULL_DEV:
      dev = New(file_dev);
      break;
   default:
      return NULL;
   }
   dev->clear_slot();         /* unknown */

   /* Copy user supplied device parameters from Resource */
   dev->dev_name = get_memory(strlen(device->device_name)+1);
   pm_strcpy(dev->dev_name, device->device_name);
   dev->prt_name = get_memory(strlen(device->device_name) + strlen(device->hdr.name) + 20);
   /* We edit "Resource-name" (physical-name) */
   Mmsg(dev->prt_name, "\"%s\" (%s)", device->hdr.name, device->device_name);
   Dmsg1(400, "Allocate dev=%s\n", dev->print_name());
   dev->capabilities = device->cap_bits;
   dev->min_free_space = device->min_free_space;
   dev->min_block_size = device->min_block_size;
   dev->max_block_size = device->max_block_size;
   dev->max_volume_size = device->max_volume_size;
   dev->max_file_size = device->max_file_size;
   dev->max_concurrent_jobs = device->max_concurrent_jobs;
   dev->volume_capacity = device->volume_capacity;
   dev->max_rewind_wait = device->max_rewind_wait;
   dev->max_open_wait = device->max_open_wait;
   dev->vol_poll_interval = device->vol_poll_interval;
   dev->max_spool_size = device->max_spool_size;
   dev->drive_index = device->drive_index;
   dev->enabled = device->enabled;
   dev->autoselect = device->autoselect;
   dev->read_only = device->read_only;
   dev->dev_type = device->dev_type;
   dev->device = device;
   if (dev->is_tape()) { /* No parts on tapes */
      dev->max_part_size = 0;
   } else {
      dev->max_part_size = device->max_part_size;
   }
   /* Sanity check */
   if (dev->vol_poll_interval && dev->vol_poll_interval < 60) {
      dev->vol_poll_interval = 60;
   }

   if (!device->dev) {
      /* The first time we create a DEVICE from the DEVRES, we keep a pointer
       * to the DEVICE accessible from the DEVRES.
       */
      device->dev = dev;
   }

   if (dev->is_fifo()) {
      dev->capabilities |= CAP_STREAM; /* set stream device */
   }

   /* If the device requires mount :
    * - Check that the mount point is available
    * - Check that (un)mount commands are defined
    */
   if (dev->is_file() && dev->requires_mount()) {
      if (!device->mount_point || stat(device->mount_point, &statp) < 0) {
         berrno be;
         dev->dev_errno = errno;
         Jmsg2(jcr, M_ERROR_TERM, 0, _("Unable to stat mount point %s: ERR=%s\n"),
            device->mount_point, be.bstrerror());
      }

      if (!device->mount_command || !device->unmount_command) {
         Jmsg0(jcr, M_ERROR_TERM, 0, _("Mount and unmount commands must defined for a device which requires mount.\n"));
      }
   } 

   /* Keep the device ID in the DEVICE struct to identify the hardware */
   if (dev->is_file() && stat(dev->archive_name(), &statp) == 0) {
         dev->devno = statp.st_dev;
   } 

   /* Sanity check */
   if (dev->max_block_size == 0) {
      max_bs = DEFAULT_BLOCK_SIZE;
   } else {
      max_bs = dev->max_block_size;
   }
   if (dev->min_block_size > max_bs) {
      Jmsg(jcr, M_ERROR_TERM, 0, _("Min block size > max on device %s\n"),
           dev->print_name());
   }
   if (dev->max_block_size > 4096000) {
      Jmsg3(jcr, M_ERROR, 0, _("Block size %u on device %s is too large, using default %u\n"),
         dev->max_block_size, dev->print_name(), DEFAULT_BLOCK_SIZE);
      dev->max_block_size = 0;
   }
   if (dev->max_block_size % TAPE_BSIZE != 0) {
      Jmsg3(jcr, M_WARNING, 0, _("Max block size %u not multiple of device %s block size=%d.\n"),
         dev->max_block_size, dev->print_name(), TAPE_BSIZE);
   }
   if (dev->max_volume_size != 0 && dev->max_volume_size < (dev->max_block_size << 4)) {
      Jmsg(jcr, M_ERROR_TERM, 0, _("Max Vol Size < 8 * Max Block Size for device %s\n"),
           dev->print_name());
   }

   dev->errmsg = get_pool_memory(PM_EMSG);
   *dev->errmsg = 0;

   if ((errstat = dev->init_mutex()) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init mutex: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
   if ((errstat = pthread_cond_init(&dev->wait, NULL)) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init cond variable: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
   if ((errstat = pthread_cond_init(&dev->wait_next_vol, NULL)) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init cond variable: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
   if ((errstat = pthread_mutex_init(&dev->spool_mutex, NULL)) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init spool mutex: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
   if ((errstat = dev->init_acquire_mutex()) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init acquire mutex: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
   if ((errstat = dev->init_read_acquire_mutex()) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init read acquire mutex: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
   if ((errstat = dev->init_volcat_mutex()) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init volcat mutex: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
   if ((errstat = dev->init_dcrs_mutex()) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init dcrs mutex: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }

   dev->set_mutex_priorities();

#ifdef xxx
   if ((errstat = rwl_init(&dev->lock)) != 0) {
      berrno be;
      dev->dev_errno = errstat;
      Mmsg1(dev->errmsg, _("Unable to init mutex: ERR=%s\n"), be.bstrerror(errstat));
      Jmsg0(jcr, M_ERROR_TERM, 0, dev->errmsg);
   }
#endif

   dev->clear_opened();
   dev->attached_dcrs = New(dlist(dcr, &dcr->dev_link));
   Dmsg2(100, "init_dev: tape=%d dev_name=%s\n", dev->is_tape(), dev->dev_name);
   dev->initiated = true;

   return dev;
}

/*
 * Open the device with the operating system and
 * initialize buffer pointers.
 *
 * Returns:  true on success
 *           false on error
 *
 * Note, for a tape, the VolName is the name we give to the
 *    volume (not really used here), but for a file, the
 *    VolName represents the name of the file to be created/opened.
 *    In the case of a file, the full name is the device name
 *    (archive_name) with the VolName concatenated.
 */
bool DEVICE::open(DCR *dcr, int omode)
{
   int preserve = 0;
   if (is_open()) {
      if (openmode == omode) {
         return true;
      } else {
         Dmsg1(200, "Close fd=%d for mode change in open().\n", m_fd);
         d_close(m_fd);
         clear_opened();
         preserve = state & (ST_LABEL|ST_APPEND|ST_READ);
      }
   }
   if (dcr) {
      dcr->setVolCatName(dcr->VolumeName);
      VolCatInfo = dcr->VolCatInfo;    /* structure assign */
   }

   state &= ~(ST_NOSPACE|ST_LABEL|ST_APPEND|ST_READ|ST_EOT|ST_WEOT|ST_EOF);
   label_type = B_BACULA_LABEL;

   if (is_tape() || is_fifo()) {
      open_tape_device(dcr, omode);
   } else if (is_ftp()) {
      open_device(dcr, omode);
   } else {
      Dmsg1(100, "call open_file_device mode=%s\n", mode_to_str(omode));
      open_file_device(dcr, omode);
   }
   state |= preserve;                 /* reset any important state info */
   Dmsg2(100, "preserve=0x%x fd=%d\n", preserve, m_fd);

   Dmsg7(100, "open dev: fd=%d dev=%p dcr=%p vol=%s type=%d dev_name=%s mode=%s\n",
         m_fd, getVolCatName(), this, dcr, dev_type, print_name(), mode_to_str(omode));
   return m_fd >= 0;
}

void DEVICE::set_mode(int new_mode)
{
   switch (new_mode) {
   case CREATE_READ_WRITE:
      mode = O_CREAT | O_RDWR | O_BINARY;
      break;
   case OPEN_READ_WRITE:
      mode = O_RDWR | O_BINARY;
      break;
   case OPEN_READ_ONLY:
      mode = O_RDONLY | O_BINARY;
      break;
   case OPEN_WRITE_ONLY:
      mode = O_WRONLY | O_BINARY;
      break;
   default:
      Emsg0(M_ABORT, 0, _("Illegal mode given to open dev.\n"));
   }
}


void DEVICE::open_device(DCR *dcr, int omode)
{
   /* do nothing waiting to split open_file/tape_device */
}


/*
 * Called to indicate that we have just read an
 *  EOF from the device.
 */
void DEVICE::set_ateof()
{
   set_eof();
   if (is_tape()) {
      file++;
   }
   file_addr = 0;
   file_size = 0;
   block_num = 0;
}

/*
 * Called to indicate we are now at the end of the tape, and
 *   writing is not possible.
 */
void DEVICE::set_ateot()
{
   /* Make tape effectively read-only */
   Dmsg0(200, "==== Set AtEof\n");
   state |= (ST_EOF|ST_EOT|ST_WEOT);
   clear_append();
}


/*
 * Set the position of the device -- only for files and DVD
 *   For other devices, there is no generic way to do it.
 *  Returns: true  on succes
 *           false on error
 */
bool DEVICE::update_pos(DCR *dcr)
{
   boffset_t pos;
   bool ok = true;

   if (!is_open()) {
      dev_errno = EBADF;
      Mmsg0(errmsg, _("Bad device call. Device not open\n"));
      Emsg1(M_FATAL, 0, "%s", errmsg);
      return false;
   }

   if (is_file()) {
      file = 0;
      file_addr = 0;
      pos = lseek(dcr, (boffset_t)0, SEEK_CUR);
      if (pos < 0) {
         berrno be;
         dev_errno = errno;
         Pmsg1(000, _("Seek error: ERR=%s\n"), be.bstrerror());
         Mmsg2(errmsg, _("lseek error on %s. ERR=%s.\n"),
               print_name(), be.bstrerror());
         ok = false;
      } else {
         file_addr = pos;
         block_num = (uint32_t)pos;
         file = (uint32_t)(pos >> 32);
      }
   }
   return ok;
}

void DEVICE::set_slot(int32_t slot)
{
   m_slot = slot;
   if (vol) vol->clear_slot();
}

void DEVICE::clear_slot()
{
   m_slot = -1;
   if (vol) vol->set_slot(-1);
}

const char *DEVICE::print_type() const
{
   return prt_dev_types[device->dev_type];
}

/*
 * Set to unload the current volume in the drive
 */
void DEVICE::set_unload()
{
   if (!m_unload && VolHdr.VolumeName[0] != 0) {
       m_unload = true;
       memcpy(UnloadVolName, VolHdr.VolumeName, sizeof(UnloadVolName));
       notify_newvol_in_attached_dcrs(NULL);
   }
}

/*
 * Clear volume header
 */
void DEVICE::clear_volhdr()
{
   Dmsg1(100, "Clear volhdr vol=%s\n", VolHdr.VolumeName);
   memset(&VolHdr, 0, sizeof(VolHdr));
   setVolCatInfo(false);
}

void DEVICE::set_nospace()
{
   state |= ST_NOSPACE;
}

void DEVICE::clear_nospace()
{
   state &= ~ST_NOSPACE;
}

/* Put device in append mode */
void DEVICE::set_append()
{
   state &= ~(ST_NOSPACE|ST_READ|ST_EOT|ST_EOF|ST_WEOT);  /* remove EOF/EOT flags */
   state |= ST_APPEND;
}

/* Clear append mode */
void DEVICE::clear_append()
{
   state &= ~ST_APPEND;
}

/* Put device in read mode */
void DEVICE::set_read()
{
   state &= ~(ST_APPEND|ST_EOT|ST_EOF|ST_WEOT);  /* remove EOF/EOT flags */
   state |= ST_READ;
}

/* Clear read mode */
void DEVICE::clear_read()
{
   state &= ~ST_READ;
}

/*
 * Get freespace using OS calls
 * TODO: See if it's working with mount commands
 */
bool DEVICE::get_os_device_freespace()
{
   int64_t freespace, totalspace;

   if (!is_file()) {
      return true;
   }
   if (fs_get_free_space(dev_name, &freespace, &totalspace) == 0) {
      set_freespace(freespace,  totalspace, 0, true);
      Mmsg(errmsg, "");
      return true;

   } else {
      set_freespace(0, 0, 0, false); /* No valid freespace */
   }
   return false;
}

/* Update the free space on the device */
bool DEVICE::update_freespace()
{
   POOL_MEM ocmd(PM_FNAME);
   POOLMEM* results;
   char* icmd;
   char* p;
   uint64_t free, total;
   char ed1[50];
   bool ok = false;
   int status;
   berrno be;

   if (!is_file()) {
      Mmsg(errmsg, "");
      return true;
   }

   /* The device must be mounted in order for freespace to work */
   if (requires_mount()) {
      mount(1);
   }

   if (get_os_device_freespace()) {
      Dmsg4(20, "get_os_device_freespace: free_space=%s freespace_ok=%d free_space_errno=%d have_media=%d\n",
         edit_uint64(free_space, ed1), !!is_freespace_ok(), free_space_errno, !!have_media());
      return true;
   }

   icmd = device->free_space_command;

   if (!icmd) {
      set_freespace(0, 0, 0, false);
      Dmsg2(20, "ERROR: update_free_space_dev: free_space=%s, free_space_errno=%d (!icmd)\n",
            edit_uint64(free_space, ed1), free_space_errno);
      Mmsg(errmsg, _("No FreeSpace command defined.\n"));
      return false;
   }

   edit_mount_codes(ocmd, icmd);

   Dmsg1(20, "update_freespace: cmd=%s\n", ocmd.c_str());

   results = get_pool_memory(PM_MESSAGE);

   Dmsg1(20, "Run freespace prog=%s\n", ocmd.c_str());
   status = run_program_full_output(ocmd.c_str(), max_open_wait/2, results);
   Dmsg2(20, "Freespace status=%d result=%s\n", status, results);
   /* Should report "1223232 12323232\n"  "free  total\n" */
   if (status == 0) {
      free = str_to_int64(results) * 1024;
      p = results;

      if (skip_nonspaces(&p)) {
         total = str_to_int64(p) * 1024;

      } else {
         total = 0;
      }

      Dmsg1(400, "Free space program run: Freespace=%s\n", results);
      if (free >= 0) {
         set_freespace(free, total, 0, true); /* have valid freespace */
         Mmsg(errmsg, "");
         ok = true;
      }
   } else {
      set_freespace(0, 0, EPIPE, false); /* no valid freespace */
      Mmsg2(errmsg, _("Cannot run free space command. Results=%s ERR=%s\n"),
            results, be.bstrerror(status));

      dev_errno = free_space_errno;
      Dmsg4(20, "Cannot get free space on device %s. free_space=%s, "
         "free_space_errno=%d ERR=%s\n",
            print_name(), edit_uint64(free_space, ed1),
            free_space_errno, errmsg);
   }
   free_pool_memory(results);
   Dmsg4(20, "leave update_freespace: free_space=%s freespace_ok=%d free_space_errno=%d have_media=%d\n",
      edit_uint64(free_space, ed1), !!is_freespace_ok(), free_space_errno, !!have_media());
   return ok;
}

void DEVICE::updateVolCatBytes(uint64_t bytes)
{
   DEVICE *dev = this;
   Lock_VolCatInfo();
   dev->VolCatInfo.VolCatAmetaBytes += bytes;
   Dmsg1(200, "updateVolBytes ameta=%lld\n",
      dev->VolCatInfo.VolCatAmetaBytes);
   dev->VolCatInfo.VolCatBytes += bytes;
   setVolCatInfo(false);
   Unlock_VolCatInfo();
}

void DEVICE::updateVolCatBlocks(uint32_t blocks)
{
   DEVICE *dev = this;
   Lock_VolCatInfo();
   dev->VolCatInfo.VolCatAmetaBlocks += blocks;
   dev->VolCatInfo.VolCatBlocks += blocks;
   setVolCatInfo(false);
   Unlock_VolCatInfo();
}


void DEVICE::updateVolCatWrites(uint32_t writes)
{
   DEVICE *dev = this;
   Lock_VolCatInfo();
   dev->VolCatInfo.VolCatAmetaWrites += writes;
   dev->VolCatInfo.VolCatWrites += writes;
   setVolCatInfo(false);
   Unlock_VolCatInfo();
}

void DEVICE::updateVolCatReads(uint32_t reads)
{
   DEVICE *dev = this;
   Lock_VolCatInfo();
   dev->VolCatInfo.VolCatAmetaReads += reads;
   dev->VolCatInfo.VolCatReads += reads;
   setVolCatInfo(false);
   Unlock_VolCatInfo();
}

void DEVICE::updateVolCatReadBytes(uint64_t bytes)
{
   DEVICE *dev = this;
   Lock_VolCatInfo();
   dev->VolCatInfo.VolCatAmetaRBytes += bytes;
   dev->VolCatInfo.VolCatRBytes += bytes;
   setVolCatInfo(false);
   Unlock_VolCatInfo();
}

/*
 * Close the device
 */
bool DEVICE::close()
{
   bool ok = true;

   Dmsg4(40, "close_dev vol=%s fd=%d dev=%p dev=%s\n",
      VolHdr.VolumeName, m_fd, this, print_name());
   offline_or_rewind();

   if (!is_open()) {
      Dmsg2(200, "device %s already closed vol=%s\n", print_name(),
         VolHdr.VolumeName);
      return true;                    /* already closed */
   }

   switch (dev_type) {
   case B_VTL_DEV:
   case B_VTAPE_DEV:
   case B_TAPE_DEV:
      unlock_door();
      /* Fall through wanted */
   default:
      if (d_close(m_fd) != 0) {
         berrno be;
         dev_errno = errno;
         Mmsg2(errmsg, _("Error closing device %s. ERR=%s.\n"),
               print_name(), be.bstrerror());
         ok = false;
      }
      break; 
   }

   unmount(1);                       /* do unmount if required */
 
   /* Clean up device packet so it can be reused */
   clear_opened();

   /*
    * Be careful not to clear items needed by the DVD driver
    *    when it is closing a single part.
    */
   state &= ~(ST_LABEL|ST_READ|ST_APPEND|ST_EOT|ST_WEOT|ST_EOF|
              ST_NOSPACE|ST_MOUNTED|ST_MEDIA|ST_SHORT);
   label_type = B_BACULA_LABEL;
   file = block_num = 0;
   file_size = 0;
   file_addr = 0;
   EndFile = EndBlock = 0;
   openmode = 0;
   clear_volhdr();
   memset(&VolCatInfo, 0, sizeof(VolCatInfo));
   if (tid) {
      stop_thread_timer(tid);
      tid = 0;
   }
   return ok;
}

/*
 * This call closes the device, but it is used in DVD handling
 *  where we close one part and then open the next part. The
 *  difference between close_part() and close() is that close_part()
 *  saves the state information of the device (e.g. the Volume lable,
 *  the Volume Catalog record, ...  This permits opening and closing
 *  the Volume parts multiple times without losing track of what the
 *  main Volume parameters are.
 */
void DEVICE::close_part(DCR * /*dcr*/)
{
   VOLUME_LABEL saveVolHdr;
   VOLUME_CAT_INFO saveVolCatInfo;     /* Volume Catalog Information */


   saveVolHdr = VolHdr;               /* structure assignment */
   saveVolCatInfo = VolCatInfo;       /* structure assignment */
   close();                           /* close current part */
   VolHdr = saveVolHdr;               /* structure assignment */
   VolCatInfo = saveVolCatInfo;       /* structure assignment */
}

/* 
 * If timeout, wait until the mount command returns 0.
 * If !timeout, try to mount the device only once.
 */
bool DEVICE::mount(int timeout)
{
   Dmsg0(190, "Enter mount\n");
   if (!is_mounted() && device->mount_command) {
      return mount_file(1, timeout);
   } 
   return true;
}

/* 
 * Unmount the device 
 * If timeout, wait until the unmount command returns 0.
 * If !timeout, try to unmount the device only once.
 */
bool DEVICE::unmount(int timeout)
{
   Dmsg0(100, "Enter unmount\n");
   if (is_mounted() && requires_mount() && device->unmount_command) {
      return mount_file(0, timeout);
   } 
   return true;
}


/*
 * Edit codes into (Un)MountCommand, Write(First)PartCommand
 *  %% = %
 *  %a = archive device name
 *  %e = erase (set if cannot mount and first part)
 *  %n = part number
 *  %m = mount point
 *  %v = last part name
 *
 *  omsg = edited output message
 *  imsg = input string containing edit codes (%x)
 *
 */
void DEVICE::edit_mount_codes(POOL_MEM &omsg, const char *imsg)
{
   const char *p;
   const char *str;
   char add[20];

   POOL_MEM archive_name(PM_FNAME);

   omsg.c_str()[0] = 0;
   Dmsg1(800, "edit_mount_codes: %s\n", imsg);
   for (p=imsg; *p; p++) {
      if (*p == '%') {
         switch (*++p) {
         case '%':
            str = "%";
            break;
         case 'a':
            str = dev_name;
            break;
         case 'e':
            if (num_dvd_parts == 0) {
               if (truncating || blank_dvd) {
                  str = "2";
               } else {
                  str = "1";
               }
            } else {
               str = "0";
            }
            break;
         case 'n':
            bsnprintf(add, sizeof(add), "%d", part);
            str = add;
            break;
         case 'm':
            str = device->mount_point;
            break;
         default:
            add[0] = '%';
            add[1] = *p;
            add[2] = 0;
            str = add;
            break;
         }
      } else {
         add[0] = *p;
         add[1] = 0;
         str = add;
      }
      Dmsg1(1900, "add_str %s\n", str);
      pm_strcat(omsg, (char *)str);
      Dmsg1(1800, "omsg=%s\n", omsg.c_str());
   }
}

/* return the last timer interval (ms)
 * or 0 if something goes wrong
 */
btime_t DEVICE::get_timer_count()
{
   btime_t temp = last_timer;
   last_timer = get_current_btime();
   temp = last_timer - temp;   /* get elapsed time */
   return (temp>0)?temp:0;     /* take care of skewed clock */
}

/* read from fd */
ssize_t DEVICE::read(void *buf, size_t len)
{
   ssize_t read_len ;

   get_timer_count();

   read_len = d_read(m_fd, buf, len);

   last_tick = get_timer_count();

   DevReadTime += last_tick;
   VolCatInfo.VolReadTime += last_tick;

   if (read_len > 0) {          /* skip error */
      DevReadBytes += read_len;
   }

   return read_len;
}

/* write to fd */
ssize_t DEVICE::write(const void *buf, size_t len)
{
   ssize_t write_len ;

   get_timer_count();

   write_len = d_write(m_fd, buf, len);

   last_tick = get_timer_count();

   DevWriteTime += last_tick;
   VolCatInfo.VolWriteTime += last_tick;

   if (write_len > 0) {         /* skip error */
      DevWriteBytes += write_len;
   }

   return write_len;
}

/* Return the resource name for the device */
const char *DEVICE::name() const
{
   return device->hdr.name;
}

uint32_t DEVICE::get_file()
{
   if (is_dvd() || is_tape()) {
      return file;
   } else {
      uint64_t bytes = VolCatInfo.VolCatAdataBytes + VolCatInfo.VolCatAmetaBytes;
      return (uint32_t)(bytes >> 32);
   }
}

uint32_t DEVICE::get_block_num()
{
   if (is_dvd() || is_tape()) {
      return block_num;
   } else {
      return  VolCatInfo.VolCatAdataBlocks + VolCatInfo.VolCatAmetaBlocks;
   }
}

/*
 * Walk through all attached jcrs indicating the volume has changed
 *   Note: If you have the new VolumeName, it is passed here,
 *     otherwise pass a NULL.
 */
void
DEVICE::notify_newvol_in_attached_dcrs(const char *newVolumeName)
{
   Dmsg2(140, "Notify dcrs of vol change. oldVolume=%s NewVolume=%s\n",
      getVolCatName(), newVolumeName?"*None*":newVolumeName);
   Lock_dcrs();
   DCR *mdcr;
   foreach_dlist(mdcr, attached_dcrs) {
      if (mdcr->jcr->JobId == 0) {
         continue;                 /* ignore console */
      }
      mdcr->NewVol = true;
      mdcr->NewFile = true;
      if (newVolumeName && mdcr->VolumeName != newVolumeName) {
         bstrncpy(mdcr->VolumeName, newVolumeName, sizeof(mdcr->VolumeName));
         Dmsg2(140, "Set NewVol=%s in JobId=%d\n", mdcr->VolumeName, mdcr->jcr->JobId);
      }
   }
   Unlock_dcrs();
}

/*
 * Walk through all attached jcrs indicating the File has changed
 */
void
DEVICE::notify_newfile_in_attached_dcrs()
{
   Dmsg1(140, "Notify dcrs of file change. Volume=%s\n", getVolCatName());
   Lock_dcrs();
   DCR *mdcr;
   foreach_dlist(mdcr, attached_dcrs) {
      if (mdcr->jcr->JobId == 0) {
         continue;                 /* ignore console */
      }
      Dmsg1(140, "Notify JobI=%d\n", mdcr->jcr->JobId);
      mdcr->NewFile = true;
   }
   Unlock_dcrs();
}



/*
 * Free memory allocated for the device
 */
void DEVICE::term(void)
{
   DEVICE *dev = NULL;
   Dmsg1(900, "term dev: %s\n", print_name());
   close();
   if (dev_name) {
      free_memory(dev_name);
      dev_name = NULL;
   }
   if (prt_name) {
      free_memory(prt_name);
      prt_name = NULL;
   }
   if (errmsg) {
      free_pool_memory(errmsg);
      errmsg = NULL;
   }
   pthread_mutex_destroy(&m_mutex);
   pthread_cond_destroy(&wait);
   pthread_cond_destroy(&wait_next_vol);
   pthread_mutex_destroy(&spool_mutex);
   pthread_mutex_destroy(&freespace_mutex);
   if (attached_dcrs) {
      delete attached_dcrs;
      attached_dcrs = NULL;
   }
   /* We let the DEVRES pointer if not our device */
   if (device && device->dev == this) {
      device->dev = NULL;
   }
   delete this;
   if (dev) {
      dev->term();
   }
}

/* Get freespace values */
void DEVICE::get_freespace(uint64_t *freeval, uint64_t *totalval)
{
   get_os_device_freespace();
   P(freespace_mutex);
   if (is_freespace_ok()) {
      *freeval = free_space;
      *totalval = total_space;
   } else {
      *freeval = *totalval = 0;
   }
   V(freespace_mutex);
}

/* Set freespace values */
void DEVICE::set_freespace(uint64_t freeval, uint64_t totalval, int errnoval, bool valid)
{
   P(freespace_mutex);
   free_space = freeval;
   total_space = totalval;
   free_space_errno = errnoval;
   if (valid) {
      set_freespace_ok();
   } else {
      clear_freespace_ok();
   }
   V(freespace_mutex);
}

/*
 * This routine initializes the device wait timers
 */
void init_device_wait_timers(DCR *dcr)
{
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;

   /* ******FIXME******* put these on config variables */
   dev->min_wait = 60 * 60;
   dev->max_wait = 24 * 60 * 60;
   dev->max_num_wait = 9;              /* 5 waits =~ 1 day, then 1 day at a time */
   dev->wait_sec = dev->min_wait;
   dev->rem_wait_sec = dev->wait_sec;
   dev->num_wait = 0;
   dev->poll = false;

   jcr->min_wait = 60 * 60;
   jcr->max_wait = 24 * 60 * 60;
   jcr->max_num_wait = 9;              /* 5 waits =~ 1 day, then 1 day at a time */
   jcr->wait_sec = jcr->min_wait;
   jcr->rem_wait_sec = jcr->wait_sec;
   jcr->num_wait = 0;

}

void init_jcr_device_wait_timers(JCR *jcr)
{
   /* ******FIXME******* put these on config variables */
   jcr->min_wait = 60 * 60;
   jcr->max_wait = 24 * 60 * 60;
   jcr->max_num_wait = 9;              /* 5 waits =~ 1 day, then 1 day at a time */
   jcr->wait_sec = jcr->min_wait;
   jcr->rem_wait_sec = jcr->wait_sec;
   jcr->num_wait = 0;
}


/*
 * The dev timers are used for waiting on a particular device
 *
 * Returns: true if time doubled
 *          false if max time expired
 */
bool double_dev_wait_time(DEVICE *dev)
{
   dev->wait_sec *= 2;               /* double wait time */
   if (dev->wait_sec > dev->max_wait) {   /* but not longer than maxtime */
      dev->wait_sec = dev->max_wait;
   }
   dev->num_wait++;
   dev->rem_wait_sec = dev->wait_sec;
   if (dev->num_wait >= dev->max_num_wait) {
      return false;
   }
   return true;
}

static const char *modes[] = {
   "CREATE_READ_WRITE",
   "OPEN_READ_WRITE",
   "OPEN_READ_ONLY",
   "OPEN_WRITE_ONLY"
};


const char *mode_to_str(int mode)
{
   static char buf[100];
   if (mode < 1 || mode > 4) {
      bsnprintf(buf, sizeof(buf), "BAD mode=%d", mode);
      return buf;
    }
   return modes[mode-1];
}
