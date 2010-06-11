/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Utility for ChromeOS-specific GPT partitions, Please see corresponding .c
 * files for more details.
 */

#include "cgpt.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <stdarg.h>

#include "cgptlib_internal.h"
#include "crc32.h"


void Error(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  fprintf(stderr, "ERROR: %s %s: ", progname, command);
  vfprintf(stderr, format, ap);
  va_end(ap);
}


int CheckValid(const struct drive *drive) {
  if ((drive->gpt.valid_headers != MASK_BOTH) ||
      (drive->gpt.valid_entries != MASK_BOTH)) {
    fprintf(stderr, "\nWARNING: one of the GPT header/entries is invalid, "
           "please run '%s repair'\n", progname);
    return CGPT_FAILED;
  }
  return CGPT_OK;
}

/* Loads sectors from 'fd'.
 * *buf is pointed to an allocated memory when returned, and should be
 * freed by cgpt_close().
 *
 *   fd -- file descriptot.
 *   buf -- pointer to buffer pointer
 *   sector -- offset of starting sector (in sectors)
 *   sector_bytes -- bytes per sector
 *   sector_count -- number of sectors to load
 *
 * Returns CGPT_OK for successful. Aborts if any error occurs.
 */
static int Load(const int fd, uint8_t **buf,
                const uint64_t sector,
                const uint64_t sector_bytes,
                const uint64_t sector_count) {
  int count;  /* byte count to read */
  int nread;

  assert(buf);
  count = sector_bytes * sector_count;
  *buf = malloc(count);
  assert(*buf);

  if (-1 == lseek(fd, sector * sector_bytes, SEEK_SET))
    goto error_free;

  nread = read(fd, *buf, count);
  if (nread < count)
    goto error_free;

  return CGPT_OK;

error_free:
  free(*buf);
  *buf = 0;
  return CGPT_FAILED;
}


int ReadPMBR(struct drive *drive) {
  if (-1 == lseek(drive->fd, 0, SEEK_SET))
    return CGPT_FAILED;

  int nread = read(drive->fd, &drive->pmbr, sizeof(struct pmbr));
  if (nread != sizeof(struct pmbr))
    return CGPT_FAILED;

  return CGPT_OK;
}

int WritePMBR(struct drive *drive) {
  if (-1 == lseek(drive->fd, 0, SEEK_SET))
    return CGPT_FAILED;

  int nwrote = write(drive->fd, &drive->pmbr, sizeof(struct pmbr));
  if (nwrote != sizeof(struct pmbr))
    return CGPT_FAILED;

  return CGPT_OK;
}

/* Saves sectors to 'fd'.
 *
 *   fd -- file descriptot.
 *   buf -- pointer to buffer
 *   sector -- starting sector offset
 *   sector_bytes -- bytes per sector
 *   sector_count -- number of sector to save
 *
 * Returns CGPT_OK for successful, CGPT_FAILED for failed.
 */
static int Save(const int fd, const uint8_t *buf,
                const uint64_t sector,
                const uint64_t sector_bytes,
                const uint64_t sector_count) {
  int count;  /* byte count to write */
  int nwrote;

  assert(buf);
  count = sector_bytes * sector_count;

  if (-1 == lseek(fd, sector * sector_bytes, SEEK_SET))
    return CGPT_FAILED;

  nwrote = write(fd, buf, count);
  if (nwrote < count)
    return CGPT_FAILED;

  return CGPT_OK;
}


// Opens a block device or file, loads raw GPT data from it.
// 
// Returns CGPT_FAILED if any error happens.
// Returns CGPT_OK if success and information are stored in 'drive'. */
int DriveOpen(const char *drive_path, struct drive *drive) {
  struct stat stat;

  assert(drive_path);
  assert(drive);

  // Clear struct for proper error handling.
  memset(drive, 0, sizeof(struct drive));

  drive->fd = open(drive_path, O_RDWR | O_LARGEFILE);
  if (drive->fd == -1) {
    Error("Can't open %s: %s\n", drive_path, strerror(errno));
    return CGPT_FAILED;
  }

  if (fstat(drive->fd, &stat) == -1) {
    goto error_close;
  }
  if ((stat.st_mode & S_IFMT) != S_IFREG) {
    if (ioctl(drive->fd, BLKGETSIZE64, &drive->size) < 0) {
      Error("Can't read drive size from %s: %s\n", drive_path, strerror(errno));
      goto error_close;
    }
    if (ioctl(drive->fd, BLKSSZGET, &drive->gpt.sector_bytes) < 0) {
      Error("Can't read sector size from %s: %s\n",
            drive_path, strerror(errno));
      goto error_close;
    }
  } else {
    drive->gpt.sector_bytes = 512;  /* bytes */
    drive->size = stat.st_size;
  }
  if (drive->size % drive->gpt.sector_bytes) {
    Error("Media size (%llu) is not a multiple of sector size(%d)\n",
          (long long unsigned int)drive->size, drive->gpt.sector_bytes);
    goto error_close;
  }
  drive->gpt.drive_sectors = drive->size / drive->gpt.sector_bytes;

  // Read the data.
  if (CGPT_OK != Load(drive->fd, &drive->gpt.primary_header,
                      GPT_PMBR_SECTOR,
                      drive->gpt.sector_bytes, GPT_HEADER_SECTOR)) {
    goto error_close;
  }
  if (CGPT_OK != Load(drive->fd, &drive->gpt.secondary_header,
                      drive->gpt.drive_sectors - GPT_PMBR_SECTOR,
                      drive->gpt.sector_bytes, GPT_HEADER_SECTOR)) {
    goto error_close;
  }
  if (CGPT_OK != Load(drive->fd, &drive->gpt.primary_entries,
                      GPT_PMBR_SECTOR + GPT_HEADER_SECTOR,
                      drive->gpt.sector_bytes, GPT_ENTRIES_SECTORS)) {
        goto error_close;
  }
  if (CGPT_OK != Load(drive->fd, &drive->gpt.secondary_entries,
                      drive->gpt.drive_sectors - GPT_HEADER_SECTOR
                      - GPT_ENTRIES_SECTORS,
                      drive->gpt.sector_bytes, GPT_ENTRIES_SECTORS)) {
    goto error_close;
  }
    
  // We just load the data. Caller must validate it.
  return CGPT_OK;

error_close:
  (void) DriveClose(drive, 0);
  return CGPT_FAILED;
}


int DriveClose(struct drive *drive, int update_as_needed) {
  int errors = 0;

  if (update_as_needed) {
    if (drive->gpt.modified & GPT_MODIFIED_HEADER1) {
      if (CGPT_OK != Save(drive->fd, drive->gpt.primary_header,
                          GPT_PMBR_SECTOR,
                          drive->gpt.sector_bytes, GPT_HEADER_SECTOR)) {
        errors++;
        Error("Cannot write primary header: %s\n", strerror(errno));
      }
    }
      
    if (drive->gpt.modified & GPT_MODIFIED_HEADER2) {
      if(CGPT_OK != Save(drive->fd, drive->gpt.secondary_header,
                         drive->gpt.drive_sectors - GPT_PMBR_SECTOR,
                         drive->gpt.sector_bytes, GPT_HEADER_SECTOR)) {
        errors++;
        Error("Cannot write secondary header: %s\n", strerror(errno));
      }
    }
    if (drive->gpt.modified & GPT_MODIFIED_ENTRIES1) {
      if (CGPT_OK != Save(drive->fd, drive->gpt.primary_entries,
                          GPT_PMBR_SECTOR + GPT_HEADER_SECTOR,
                          drive->gpt.sector_bytes, GPT_ENTRIES_SECTORS)) {
        errors++;
        Error("Cannot write primary entries: %s\n", strerror(errno));
      }
    }
    if (drive->gpt.modified & GPT_MODIFIED_ENTRIES2) {
      if (CGPT_OK != Save(drive->fd, drive->gpt.secondary_entries,
                          drive->gpt.drive_sectors - GPT_HEADER_SECTOR
                          - GPT_ENTRIES_SECTORS,
                          drive->gpt.sector_bytes, GPT_ENTRIES_SECTORS)) {
        errors++;
        Error("Cannot write secondary entries: %s\n", strerror(errno));
      }
    }
  }

  close(drive->fd);

  if (drive->gpt.primary_header)
    free(drive->gpt.primary_header);
  drive->gpt.primary_header = 0;
  if (drive->gpt.primary_entries)
    free(drive->gpt.primary_entries);
  drive->gpt.primary_entries = 0;
  if (drive->gpt.secondary_header)
    free(drive->gpt.secondary_header);
  drive->gpt.secondary_header = 0;
  if (drive->gpt.secondary_entries)
    free(drive->gpt.secondary_entries);
  drive->gpt.secondary_entries = 0;

  return errors ? CGPT_FAILED : CGPT_OK;
}



/* GUID conversion functions. Accepted format:
 *
 *   "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
 *
 * Returns CGPT_OK if parsing is successful; otherwise CGPT_FAILED.
 */
int StrToGuid(const char *str, Guid *guid) {
  uint32_t time_low;
  uint16_t time_mid;
  uint16_t time_high_and_version;
  unsigned int chunk[11];

  if (11 != sscanf(str, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                   chunk+0,
                   chunk+1,
                   chunk+2,
                   chunk+3,
                   chunk+4,
                   chunk+5,
                   chunk+6,
                   chunk+7,
                   chunk+8,
                   chunk+9,
                   chunk+10)) {                   
    printf("FAILED\n");
    return CGPT_FAILED;
  }

  time_low = chunk[0] & 0xffffffff;
  time_mid = chunk[1] & 0xffff;
  time_high_and_version = chunk[2] & 0xffff;

  guid->u.Uuid.time_low = htole32(time_low);
  guid->u.Uuid.time_mid = htole16(time_mid);
  guid->u.Uuid.time_high_and_version = htole16(time_high_and_version);

  guid->u.Uuid.clock_seq_high_and_reserved = chunk[3] & 0xff;
  guid->u.Uuid.clock_seq_low = chunk[4] & 0xff;
  guid->u.Uuid.node[0] = chunk[5] & 0xff;
  guid->u.Uuid.node[1] = chunk[6] & 0xff;
  guid->u.Uuid.node[2] = chunk[7] & 0xff;
  guid->u.Uuid.node[3] = chunk[8] & 0xff;
  guid->u.Uuid.node[4] = chunk[9] & 0xff;
  guid->u.Uuid.node[5] = chunk[10] & 0xff;

  return CGPT_OK;
}
void GuidToStr(const Guid *guid, char *str) {
  sprintf(str, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
          le32toh(guid->u.Uuid.time_low), le16toh(guid->u.Uuid.time_mid),
          le16toh(guid->u.Uuid.time_high_and_version),
          guid->u.Uuid.clock_seq_high_and_reserved, guid->u.Uuid.clock_seq_low,
          guid->u.Uuid.node[0], guid->u.Uuid.node[1], guid->u.Uuid.node[2],
          guid->u.Uuid.node[3], guid->u.Uuid.node[4], guid->u.Uuid.node[5]);
}

/* Convert UTF16 string to UTF8. Rewritten from gpt utility.
 * Caller must prepare enough space for UTF8. The rough estimation is:
 *
 *   utf8 length = bytecount(utf16) * 1.5
 */
#define SIZEOF_GPTENTRY_NAME 36  /* sizeof(GptEntry.name[]) */
void UTF16ToUTF8(const uint16_t *utf16, uint8_t *utf8)
{
  size_t s8idx, s16idx, s16len;
  uint32_t utfchar;
  unsigned int next_utf16;

  for (s16len = 0; s16len < SIZEOF_GPTENTRY_NAME && utf16[s16len]; ++s16len);

  *utf8 = s8idx = s16idx = 0;
  while (s16idx < s16len) {
    utfchar = le16toh(utf16[s16idx++]);
    if ((utfchar & 0xf800) == 0xd800) {
      next_utf16 = le16toh(utf16[s16idx]);
      if ((utfchar & 0x400) != 0 || (next_utf16 & 0xfc00) != 0xdc00)
        utfchar = 0xfffd;
      else
        s16idx++;
    }
    if (utfchar < 0x80) {
      utf8[s8idx++] = utfchar;
    } else if (utfchar < 0x800) {
      utf8[s8idx++] = 0xc0 | (utfchar >> 6);
      utf8[s8idx++] = 0x80 | (utfchar & 0x3f);
    } else if (utfchar < 0x10000) {
      utf8[s8idx++] = 0xe0 | (utfchar >> 12);
      utf8[s8idx++] = 0x80 | ((utfchar >> 6) & 0x3f);
      utf8[s8idx++] = 0x80 | (utfchar & 0x3f);
    } else if (utfchar < 0x200000) {
      utf8[s8idx++] = 0xf0 | (utfchar >> 18);
      utf8[s8idx++] = 0x80 | ((utfchar >> 12) & 0x3f);
      utf8[s8idx++] = 0x80 | ((utfchar >> 6) & 0x3f);
      utf8[s8idx++] = 0x80 | (utfchar & 0x3f);
    }
  }
  utf8[s8idx++] = 0;
}

/* Convert UTF8 string to UTF16. Rewritten from gpt utility.
 * Caller must prepare enough space for UTF16. The conservative estimation is:
 *
 *   utf16 bytecount = bytecount(utf8) / 3 * 4
 */
void UTF8ToUTF16(const uint8_t *utf8, uint16_t *utf16)
{
  size_t s16idx, s8idx, s8len;
  uint32_t utfchar;
  unsigned int c, utfbytes;

  for (s8len = 0; utf8[s8len]; ++s8len);

  s8idx = s16idx = 0;
  utfbytes = 0;
  do {
    c = utf8[s8idx++];
    if ((c & 0xc0) != 0x80) {
      /* Initial characters. */
      if (utfbytes != 0) {
        /* Incomplete encoding. */
        utf16[s16idx++] = 0xfffd;
      }
      if ((c & 0xf8) == 0xf0) {
        utfchar = c & 0x07;
        utfbytes = 3;
      } else if ((c & 0xf0) == 0xe0) {
        utfchar = c & 0x0f;
        utfbytes = 2;
      } else if ((c & 0xe0) == 0xc0) {
        utfchar = c & 0x1f;
        utfbytes = 1;
      } else {
        utfchar = c & 0x7f;
        utfbytes = 0;
      }
    } else {
      /* Followup characters. */
      if (utfbytes > 0) {
        utfchar = (utfchar << 6) + (c & 0x3f);
        utfbytes--;
      } else if (utfbytes == 0)
        utfbytes = -1;
        utfchar = 0xfffd;
    }
    if (utfbytes == 0) {
      if (utfchar >= 0x10000) {
        utf16[s16idx++] = htole16(0xd800 | ((utfchar>>10)-0x40));
        if (s16idx >= SIZEOF_GPTENTRY_NAME) break;
        utf16[s16idx++] = htole16(0xdc00 | (utfchar & 0x3ff));
      } else {
        utf16[s16idx++] = htole16(utfchar);
      }
    }
  } while (c != 0 && s16idx < SIZEOF_GPTENTRY_NAME);
  if (s16idx < SIZEOF_GPTENTRY_NAME)
    utf16[s16idx++] = 0;
}

struct {
  Guid type;
  char *name;
  char *description;
} supported_types[] = {
  {GPT_ENT_TYPE_CHROMEOS_KERNEL, "kernel", "ChromeOS kernel"},
  {GPT_ENT_TYPE_CHROMEOS_ROOTFS, "rootfs", "ChromeOS rootfs"},
  {GPT_ENT_TYPE_LINUX_DATA, "data", "Linux data"},
  {GPT_ENT_TYPE_CHROMEOS_RESERVED, "reserved", "ChromeOS reserved"},
  {GPT_ENT_TYPE_EFI, "efi", "EFI System Partition"},
  {GPT_ENT_TYPE_UNUSED, "unused", "Unused (nonexistent) partition"},
};

/* Resolves human-readable GPT type.
 * Returns CGPT_OK if found.
 * Returns CGPT_FAILED if no known type found. */
int ResolveType(const Guid *type, char *buf) {
  int i;
  for (i = 0; i < ARRAY_COUNT(supported_types); ++i) {
    if (!memcmp(type, &supported_types[i].type, sizeof(Guid))) {
      strcpy(buf, supported_types[i].description);
      return CGPT_OK;
    }
  }
  return CGPT_FAILED;
}

int SupportedType(const char *name, Guid *type) {
  int i;
  for (i = 0; i < ARRAY_COUNT(supported_types); ++i) {
    if (!strcmp(name, supported_types[i].name)) {
      memcpy(type, &supported_types[i].type, sizeof(Guid));
      return CGPT_OK;
    }
  }
  return CGPT_FAILED;
}

void PrintTypes(void) {
  int i;
  printf("The partition type may also be given as one of these aliases:\n\n");
  for (i = 0; i < ARRAY_COUNT(supported_types); ++i) {
    printf("    %-10s  %s\n", supported_types[i].name,
                          supported_types[i].description);
  }
  printf("\n");
}

uint32_t GetNumberOfEntries(const GptData *gpt) {
  GptHeader *header = 0;
  if (gpt->valid_headers & MASK_PRIMARY)
    header = (GptHeader*)gpt->primary_header;
  else if (gpt->valid_headers & MASK_SECONDARY)
    header = (GptHeader*)gpt->secondary_header;
  else
    return 0;
  return header->number_of_entries;
}

static uint32_t GetSizeOfEntries(const GptData *gpt) {
  GptHeader *header = 0;
  if (gpt->valid_headers & MASK_PRIMARY)
    header = (GptHeader*)gpt->primary_header;
  else if (gpt->valid_headers & MASK_SECONDARY)
    header = (GptHeader*)gpt->secondary_header;
  else
    return 0;
  return header->number_of_entries;
}

GptEntry *GetEntry(GptData *gpt, int secondary, int entry_index) {
  uint8_t *entries;
  int stride = GetSizeOfEntries(gpt);
  if (!stride)
    return 0;

  if (secondary == PRIMARY) {
    entries = gpt->primary_entries;
  } else {
    entries = gpt->secondary_entries;
  }

  return (GptEntry*)(&entries[stride * entry_index]);
}

void SetPriority(GptData *gpt, int secondary, int entry_index, int priority) {
  GptEntry *entry;
  entry = GetEntry(gpt, secondary, entry_index);

  assert(priority >= 0 && priority <= CGPT_ATTRIBUTE_MAX_PRIORITY);
  entry->attributes &= ~CGPT_ATTRIBUTE_PRIORITY_MASK;
  entry->attributes |= (uint64_t)priority << CGPT_ATTRIBUTE_PRIORITY_OFFSET;
}

int GetPriority(GptData *gpt, int secondary, int entry_index) {
  GptEntry *entry;
  entry = GetEntry(gpt, secondary, entry_index);
  return (entry->attributes & CGPT_ATTRIBUTE_PRIORITY_MASK) >>
         CGPT_ATTRIBUTE_PRIORITY_OFFSET;
}

void SetTries(GptData *gpt, int secondary, int entry_index, int tries) {
  GptEntry *entry;
  entry = GetEntry(gpt, secondary, entry_index);

  assert(tries >= 0 && tries <= CGPT_ATTRIBUTE_MAX_TRIES);
  entry->attributes &= ~CGPT_ATTRIBUTE_TRIES_MASK;
  entry->attributes |= (uint64_t)tries << CGPT_ATTRIBUTE_TRIES_OFFSET;
}

int GetTries(GptData *gpt, int secondary, int entry_index) {
  GptEntry *entry;
  entry = GetEntry(gpt, secondary, entry_index);
  return (entry->attributes & CGPT_ATTRIBUTE_TRIES_MASK) >>
         CGPT_ATTRIBUTE_TRIES_OFFSET;
}

void SetSuccessful(GptData *gpt, int secondary, int entry_index, int success) {
  GptEntry *entry;
  entry = GetEntry(gpt, secondary, entry_index);

  assert(success >= 0 && success <= CGPT_ATTRIBUTE_MAX_SUCCESSFUL);
  entry->attributes &= ~CGPT_ATTRIBUTE_SUCCESSFUL_MASK;
  entry->attributes |= (uint64_t)success << CGPT_ATTRIBUTE_SUCCESSFUL_OFFSET;
}

int GetSuccessful(GptData *gpt, int secondary, int entry_index) {
  GptEntry *entry;
  entry = GetEntry(gpt, secondary, entry_index);
  return (entry->attributes & CGPT_ATTRIBUTE_SUCCESSFUL_MASK) >>
         CGPT_ATTRIBUTE_SUCCESSFUL_OFFSET;
}


#define TOSTRING(A) #A
const char *GptError(int errnum) {
  const char *error_string[] = {
    TOSTRING(GPT_SUCCESS),
    TOSTRING(GPT_ERROR_NO_VALID_KERNEL),
    TOSTRING(GPT_ERROR_INVALID_HEADERS),
    TOSTRING(GPT_ERROR_INVALID_ENTRIES),
    TOSTRING(GPT_ERROR_INVALID_SECTOR_SIZE),
    TOSTRING(GPT_ERROR_INVALID_SECTOR_NUMBER),
    TOSTRING(GPT_ERROR_INVALID_UPDATE_TYPE)
  };
  if (errnum < 0 || errnum >= ARRAY_COUNT(error_string))
    return "<illegal value>";
  return error_string[errnum];
}

/*  Update CRC value if necessary.  */
void UpdateCrc(GptData *gpt) {
  GptHeader *primary_header, *secondary_header;

  primary_header = (GptHeader*)gpt->primary_header;
  secondary_header = (GptHeader*)gpt->secondary_header;

  if (gpt->modified & GPT_MODIFIED_ENTRIES1) {
    primary_header->entries_crc32 =
        Crc32(gpt->primary_entries, TOTAL_ENTRIES_SIZE);
  }
  if (gpt->modified & GPT_MODIFIED_ENTRIES2) {
    secondary_header->entries_crc32 =
        Crc32(gpt->secondary_entries, TOTAL_ENTRIES_SIZE);
  }
  if (gpt->modified & GPT_MODIFIED_HEADER1) {
    primary_header->header_crc32 = 0;
    primary_header->header_crc32 = Crc32(
        (const uint8_t *)primary_header, primary_header->size);
  }
  if (gpt->modified & GPT_MODIFIED_HEADER2) {
    secondary_header->header_crc32 = 0;
    secondary_header->header_crc32 = Crc32(
        (const uint8_t *)secondary_header, secondary_header->size);
  }
}
/* Two headers are NOT bitwise identical. For example, my_lba pointers to header
 * itself so that my_lba in primary and secondary is definitely different.
 * Only the following fields should be identical.
 *
 *   first_usable_lba
 *   last_usable_lba
 *   number_of_entries
 *   size_of_entry
 *   disk_uuid
 *
 * If any of above field are not matched, overwrite secondary with primary since
 * we always trust primary.
 * If any one of header is invalid, copy from another. */
int IsSynonymous(const GptHeader* a, const GptHeader* b) {
  if ((a->first_usable_lba == b->first_usable_lba) &&
      (a->last_usable_lba == b->last_usable_lba) &&
      (a->number_of_entries == b->number_of_entries) &&
      (a->size_of_entry == b->size_of_entry) &&
      (!memcmp(&a->disk_uuid, &b->disk_uuid, sizeof(Guid))))
    return 1;
  return 0;
}

/* Primary entries and secondary entries should be bitwise identical.
 * If two entries tables are valid, compare them. If not the same,
 * overwrites secondary with primary (primary always has higher priority),
 * and marks secondary as modified.
 * If only one is valid, overwrites invalid one.
 * If all are invalid, does nothing.
 * This function returns bit masks for GptData.modified field.
 * Note that CRC is NOT re-computed in this function.
 */
uint8_t RepairEntries(GptData *gpt, const uint32_t valid_entries) {
  if (valid_entries == MASK_BOTH) {
    if (memcmp(gpt->primary_entries, gpt->secondary_entries,
               TOTAL_ENTRIES_SIZE)) {
      memcpy(gpt->secondary_entries, gpt->primary_entries, TOTAL_ENTRIES_SIZE);
      return GPT_MODIFIED_ENTRIES2;
    }
  } else if (valid_entries == MASK_PRIMARY) {
    memcpy(gpt->secondary_entries, gpt->primary_entries, TOTAL_ENTRIES_SIZE);
    return GPT_MODIFIED_ENTRIES2;
  } else if (valid_entries == MASK_SECONDARY) {
    memcpy(gpt->primary_entries, gpt->secondary_entries, TOTAL_ENTRIES_SIZE);
    return GPT_MODIFIED_ENTRIES1;
  }

  return 0;
}

/* The above five fields are shared between primary and secondary headers.
 * We can recover one header from another through copying those fields. */
void CopySynonymousParts(GptHeader* target, const GptHeader* source) {
  target->first_usable_lba = source->first_usable_lba;
  target->last_usable_lba = source->last_usable_lba;
  target->number_of_entries = source->number_of_entries;
  target->size_of_entry = source->size_of_entry;
  memcpy(&target->disk_uuid, &source->disk_uuid, sizeof(Guid));
}

/* This function repairs primary and secondary headers if possible.
 * If both headers are valid (CRC32 is correct) but
 *   a) indicate inconsistent usable LBA ranges,
 *   b) inconsistent partition entry size and number,
 *   c) inconsistent disk_uuid,
 * we will use the primary header to overwrite secondary header.
 * If primary is invalid (CRC32 is wrong), then we repair it from secondary.
 * If secondary is invalid (CRC32 is wrong), then we repair it from primary.
 * This function returns the bitmasks for modified header.
 * Note that CRC value is NOT re-computed in this function. UpdateCrc() will
 * do it later.
 */
uint8_t RepairHeader(GptData *gpt, const uint32_t valid_headers) {
  GptHeader *primary_header, *secondary_header;

  primary_header = (GptHeader*)gpt->primary_header;
  secondary_header = (GptHeader*)gpt->secondary_header;

  if (valid_headers == MASK_BOTH) {
    if (!IsSynonymous(primary_header, secondary_header)) {
      CopySynonymousParts(secondary_header, primary_header);
      return GPT_MODIFIED_HEADER2;
    }
  } else if (valid_headers == MASK_PRIMARY) {
    memcpy(secondary_header, primary_header, primary_header->size);
    secondary_header->my_lba = gpt->drive_sectors - 1;  /* the last sector */
    secondary_header->alternate_lba = primary_header->my_lba;
    secondary_header->entries_lba = secondary_header->my_lba -
        GPT_ENTRIES_SECTORS;
    return GPT_MODIFIED_HEADER2;
  } else if (valid_headers == MASK_SECONDARY) {
    memcpy(primary_header, secondary_header, secondary_header->size);
    primary_header->my_lba = GPT_PMBR_SECTOR;  /* the second sector on drive */
    primary_header->alternate_lba = secondary_header->my_lba;
    primary_header->entries_lba = primary_header->my_lba + GPT_HEADER_SECTOR;
    return GPT_MODIFIED_HEADER1;
  }

  return 0;
}


int IsZero(const Guid *gp) {
  return (0 == memcmp(gp, &guid_unused, sizeof(Guid)));
}

void PMBRToStr(struct pmbr *pmbr, char *str) {
  char buf[256];
  if (IsZero(&pmbr->boot_guid)) {
    sprintf(str, "PMBR");
  } else {
    GuidToStr(&pmbr->boot_guid, buf);
    sprintf(str, "PMBR (Boot GUID: %s)", buf);
  }
}

