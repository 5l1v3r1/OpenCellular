/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cgptlib.h"
#include "cgptlib_internal.h"
#include "crc32.h"
#include "gpt.h"
#include "utility.h"

/* global types to compare against */
const Guid guid_unused = GPT_ENT_TYPE_UNUSED;
const Guid guid_chromeos_kernel = GPT_ENT_TYPE_CHROMEOS_KERNEL;


int GptInit(GptData *gpt) {
  int retval;

  gpt->modified = 0;
  gpt->current_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;
  gpt->current_priority = 999;

  retval = GptSanityCheck(gpt);
  if (GPT_SUCCESS != retval)
    return retval;

  GptRepair(gpt);
  return GPT_SUCCESS;
}


int GptNextKernelEntry(GptData* gpt, uint64_t* start_sector, uint64_t* size) {
  GptHeader* header = (GptHeader*)gpt->primary_header;
  GptEntry* entries = (GptEntry*)gpt->primary_entries;
  GptEntry* e;
  int new_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;
  int new_prio = 0;
  int i;

  /* If we already found a kernel, continue the scan at the current
   * kernel's prioity, in case there is another kernel with the same
   * priority. */
  if (gpt->current_kernel != CGPT_KERNEL_ENTRY_NOT_FOUND) {
    for (i = gpt->current_kernel + 1; i < header->number_of_entries; i++) {
      e = entries + i;
      if (!IsKernelEntry(e))
        continue;
      if (!(GetEntrySuccessful(e) || GetEntryTries(e)))
        continue;
      if (GetEntryPriority(e) == gpt->current_priority) {
        gpt->current_kernel = i;
        *start_sector = e->starting_lba;
        *size = e->ending_lba - e->starting_lba + 1;
        return GPT_SUCCESS;
      }
    }
  }

  /* We're still here, so scan for the remaining kernel with the
   * highest priority less than the previous attempt. */
  for (i = 0, e = entries; i < header->number_of_entries; i++, e++) {
    int current_prio = GetEntryPriority(e);
    if (!IsKernelEntry(e))
      continue;
    if (!(GetEntrySuccessful(e) || GetEntryTries(e)))
      continue;
    if (current_prio >= gpt->current_priority)
      continue;  /* Already returned this kernel in a previous call */
    if (current_prio > new_prio) {
      new_kernel = i;
      new_prio = current_prio;
    }
  }

  /* Save what we found.  Note that if we didn't find a new kernel,
   * new_prio will still be -1, so future calls to this function will
   * also fail. */
  gpt->current_kernel = new_kernel;
  gpt->current_priority = new_prio;

  if (CGPT_KERNEL_ENTRY_NOT_FOUND == new_kernel)
    return GPT_ERROR_NO_VALID_KERNEL;

  e = entries + new_kernel;
  *start_sector = e->starting_lba;
  *size = e->ending_lba - e->starting_lba + 1;
  return GPT_SUCCESS;
}


int GptUpdateKernelEntry(GptData* gpt, uint32_t update_type) {
  GptHeader* header = (GptHeader*)gpt->primary_header;
  GptEntry* entries = (GptEntry*)gpt->primary_entries;
  GptEntry* e = entries + gpt->current_kernel;
  uint64_t previous_attr = e->attributes;

  /* TODO: need a better return code for these errors? */
  if (gpt->current_kernel == CGPT_KERNEL_ENTRY_NOT_FOUND)
    return GPT_ERROR_INVALID_UPDATE_TYPE;
  if (!IsKernelEntry(e))
    return GPT_ERROR_INVALID_UPDATE_TYPE;

  switch (update_type) {
    case GPT_UPDATE_ENTRY_TRY: {
      /* Used up a try */
      int tries;
      if (GetEntrySuccessful(e))
        return GPT_SUCCESS;  /* Successfully booted this partition, so
                              * tries field is ignored. */
      tries = GetEntryTries(e);
      if (tries > 1) {
        /* Still have tries left */
        SetEntryTries(e, tries - 1);
        break;
      }
      /* Out of tries, so drop through and mark partition bad. */
    }
    case GPT_UPDATE_ENTRY_BAD: {
      /* Giving up on this partition entirely. */
      e->attributes &= ~(CGPT_ATTRIBUTE_SUCCESSFUL_MASK |
                         CGPT_ATTRIBUTE_TRIES_MASK |
                         CGPT_ATTRIBUTE_PRIORITY_MASK);
      break;
    }
    default:
      return GPT_ERROR_INVALID_UPDATE_TYPE;
  }

  /* If no change to attributes, we're done */
  if (e->attributes == previous_attr)
    return GPT_SUCCESS;

  /* Update the CRCs */
  header->entries_crc32 = Crc32((const uint8_t *)entries,
                                header->size_of_entry *
                                header->number_of_entries);
  header->header_crc32 = HeaderCrc(header);
  gpt->modified |= GPT_MODIFIED_HEADER1 | GPT_MODIFIED_ENTRIES1;

  /* Use the repair function to update the other copy of the GPT.
   * This is a tad inefficient, but is much faster than the disk I/O
   * to update the GPT on disk so it doesn't matter. */
  gpt->valid_headers = MASK_PRIMARY;
  gpt->valid_entries = MASK_PRIMARY;
  GptRepair(gpt);

  return GPT_SUCCESS;
}
