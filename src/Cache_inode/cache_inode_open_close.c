/**
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 *
 * @file    cache_inode_open_close.c
 * @brief   Manage opening and closing files and caching file
 *          descriptors
 *
 * This file manages the opening and closing files and the file
 * descriptor cache, in conjunction with the lru_thread in
 * cache_inode_lru.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif                          /* _SOLARIS */

#include "fsal.h"

#include "LRU_List.h"
#include "log.h"
#include "HashData.h"
#include "HashTable.h"
#include "cache_inode.h"
#include "cache_inode_lru.h"
#include "stuff_alloc.h"

#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <strings.h>
#include <assert.h>

/**
 * @brief Check if a file is available to write
 *
 * This function checks whether the given file is currently open in a
 * mode supporting write operations.
 *
 * @param entry [in] Entry for the file to check
 *
 * @return TRUE if the file is open for writes
 */

bool_t
is_open_for_write(cache_entry_t *entry)
{
     return
          (entry &&
           (entry->type == REGULAR_FILE)/*  && */
/*            ((entry->object.file.open_fd.openflags == FSAL_O_RDWR) || */
/*             (entry->object.file.open_fd.openflags == FSAL_O_WRONLY)) */);
}

/**
 * @brief Check if a file is available to read
 *
 * This function checks whether the given file is currently open in a
 * mode supporting read operations.
 *
 * @param entry [in] Entry for the file to check
 *
 * @return TRUE if the file is opened for reads
 */

bool_t
is_open_for_read(cache_entry_t *entry)
{
     return
          (entry &&
           (entry->type == REGULAR_FILE)/*  && */
/*            ((entry->object.file.open_fd.openflags == FSAL_O_RDWR) || */
/*             (entry->object.file.open_fd.openflags == FSAL_O_RDONLY)) */);
}

/**
 *
 * @brief Opens a file descriptor
 *
 * This function opens a file descriptor on a given cache entry.
 *
 * @param entry [in] Cache entry representing the file to open
 * @param client [in,out] Per-thread resource management data
 * @param openflags [in] The tyep of access for which to open
 * @param context [in] FSAL operation context
 * @param flags [in] Flags indicating lock status
 * @param status [out] Operation status
 *
 * @return CACHE_INODE_SUCCESS if successful, errors otherwise
 */

cache_inode_status_t
cache_inode_open(cache_entry_t *entry,
                 cache_inode_client_t *client,
                 fsal_openflags_t openflags,
		 struct user_cred *creds,
                 uint32_t flags,
                 cache_inode_status_t *status)
{
     /* Error return from FSAL */
     fsal_status_t fsal_status = {0, 0};
     fsal_accessflags_t access_type;

     if ((entry == NULL) || (client == NULL) ||
         (creds == NULL) || (status == NULL)) {
          *status = CACHE_INODE_INVALID_ARGUMENT;
          goto out;
     }

     if (entry->type != REGULAR_FILE) {
          *status = CACHE_INODE_BAD_TYPE;
          goto out;
     }

     if (!cache_inode_lru_fds_available()) {
          /* This seems the best idea, let the client try again later
             after the reap. */
          *status = CACHE_INODE_DELAY;
          goto out;
     }

     access_type = (openflags == FSAL_O_RDWR) ? FSAL_R_OK : FSAL_W_OK;

     if (!(flags & CACHE_INODE_FLAG_CONTENT_HAVE)) {
          pthread_rwlock_wrlock(&entry->content_lock);
     }

     /* access check but based on fsal_open_flags_t, not fsal_access_flags_t
      * this may be checked above but here is a last stop check.
      * Execute access not considered here.  Could fail execute opens.
      * FIXME: sort out access checks in callers.
      */
     fsal_status = entry->obj_handle->ops->test_access(entry->obj_handle,
						       creds, access_type);
     if(FSAL_IS_ERROR(fsal_status)) {
	 *status = cache_inode_error_convert(fsal_status);

	 LogDebug(COMPONENT_CACHE_INODE,
		  "returning %d(%s) from access check",
		  *status, cache_inode_err_str(*status));
	 goto unlock;
     }
/* @TODO all this logic is in the fsal as is the fd itself.
 * Do we have to test this or can we depend on the fsal?
 */
     /* Open file need to be closed, unless it is already open as read/write */
/*      if ((entry->object.file.open_fd.openflags != FSAL_O_RDWR) && */
/*          (entry->object.file.open_fd.openflags != 0) && */
/*          (entry->object.file.open_fd.openflags != openflags)) { */
/*           fsal_status = FSAL_close(&(entry->object.file.open_fd.fd)); */
/*           if (FSAL_IS_ERROR(fsal_status) && */
/*               (fsal_status.major != ERR_FSAL_NOT_OPENED)) { */
/*                *status = cache_inode_error_convert(fsal_status); */
/*                if (fsal_status.major == ERR_FSAL_STALE) { */
/*                     cache_inode_kill_entry(entry, client); */
/*                } */

/*                LogDebug(COMPONENT_CACHE_INODE, */
/*                         "cache_inode_open: returning %d(%s) from FSAL_close", */
/*                         *status, cache_inode_err_str(*status)); */

/*                goto unlock; */
/*           } */

/*           if (!FSAL_IS_ERROR(fsal_status)) */
/*               atomic_dec_uint(&open_fd_count); */

/*           /\* Force re-openning *\/ */
/*           entry->object.file.open_fd.openflags = FSAL_O_CLOSED; */
/*      } */

/*      if ((entry->object.file.open_fd.openflags == FSAL_O_CLOSED)) { */
	  fsal_status = entry->obj_handle->ops->open(entry->obj_handle,
						     openflags);
          if (FSAL_IS_ERROR(fsal_status)) {
               *status = cache_inode_error_convert(fsal_status);
               LogDebug(COMPONENT_CACHE_INODE,
                        "cache_inode_open: returning %d(%s) from FSAL_open",
                        *status, cache_inode_err_str(*status));
               if (fsal_status.major == ERR_FSAL_STALE) {
                    cache_inode_kill_entry(entry, client);
               }
               goto unlock;
          }

/*           entry->object.file.open_fd.openflags = openflags; */
          /* This is temporary code, until Jim Lieb makes FSALs cache
             their own file descriptors.  Under that regime, the LRU
             thread will interrogate FSALs for their FD use. */
          atomic_inc_uint(&open_fd_count);

          LogDebug(COMPONENT_CACHE_INODE,
                   "cache_inode_open: pentry %p: openflags = %d, "
                   "open_fd_count = %d", entry, openflags,
                   open_fd_count);
/*      } */

     *status = CACHE_INODE_SUCCESS;

unlock:

     if (!(flags & CACHE_INODE_FLAG_CONTENT_HOLD)) {
          pthread_rwlock_unlock(&entry->content_lock);
     }

out:

     return *status;

} /* cache_inode_open */

/**
 * @brief Close a file
 *
 * This function calls down to the FSAL to close the file.
 *
 * @param entry [in] Cache entry to close
 * @param client [in] Per-client resource management structure
 * @param flags [in] Flags for lock management
 * @param status [out] Operation status
 *
 * @return CACHE_INODE_SUCCESS or errors on failure
 */

cache_inode_status_t
cache_inode_close(cache_entry_t *entry,
                  cache_inode_client_t *client,
                  uint32_t flags,
                  cache_inode_status_t *status)
{
     /* Error return from the FSAL */
     fsal_status_t fsal_status;

     if ((entry == NULL) || (client == NULL) || (status == NULL)) {
          *status = CACHE_INODE_INVALID_ARGUMENT;
          goto out;
     }

     if (entry->type != REGULAR_FILE) {
          *status = CACHE_INODE_BAD_TYPE;
          goto out;
     }

     if (!(flags & CACHE_INODE_FLAG_CONTENT_HAVE)) {
          pthread_rwlock_wrlock(&entry->content_lock);
     }

     /* If nothing is opened, do nothing */
/*      if (entry->object.file.open_fd.openflags == FSAL_O_CLOSED) { */
/*           if (!(flags & CACHE_INODE_FLAG_CONTENT_HOLD)) { */
/*                pthread_rwlock_unlock(&entry->content_lock); */
/*           } */
/*           *status = CACHE_INODE_SUCCESS; */
/*           return *status; */
/*      } */

     /* If state is held in the file, do not close it.  This should
        be refined.  (A non return_on_close layout should not prevent
        the file from closing.)  The caller should hold the state
        lock. */
     if (cache_inode_file_holds_state(entry)) {
          *status = CACHE_INODE_SUCCESS;
          goto unlock;
     }

     if (!cache_inode_lru_caching_fds() ||
         (flags & CACHE_INODE_FLAG_REALLYCLOSE)) {
          LogDebug(COMPONENT_CACHE_INODE,
                   "cache_inode_close: entry %p", entry);
	  fsal_status = entry->obj_handle->ops->close(entry->obj_handle);

/*           entry->object.file.open_fd.openflags = FSAL_O_CLOSED; */
          if (FSAL_IS_ERROR(fsal_status) &&
              (fsal_status.major != ERR_FSAL_NOT_OPENED)) {
               *status = cache_inode_error_convert(fsal_status);
               if (fsal_status.major == ERR_FSAL_STALE) {
                    cache_inode_kill_entry(entry, client);
               }
               LogCrit(COMPONENT_CACHE_INODE,
                       "cache_inode_close: returning %d(%s) from FSAL_close",
                       *status, cache_inode_err_str(*status));
               goto unlock;
          }
          if (!FSAL_IS_ERROR(fsal_status))
              atomic_dec_uint(&open_fd_count);
     }

     *status = CACHE_INODE_SUCCESS;

unlock:

     if (!(flags & CACHE_INODE_FLAG_CONTENT_HOLD)) {
          pthread_rwlock_unlock(&entry->content_lock);
     }

out:

     return *status;
}
