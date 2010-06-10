/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Data structure definitions for verified boot, for on-disk / in-eeprom
 * data.
 */

#ifndef VBOOT_REFERENCE_VBOOT_STRUCT_H_
#define VBOOT_REFERENCE_VBOOT_STRUCT_H_

#include <stdint.h>


/* Public key data */
typedef struct VbPublicKey {
  uint64_t key_offset;     /* Offset of key data from start of this struct */
  uint64_t key_size;       /* Size of key data in bytes (NOT strength of key
                           * in bits) */
  uint64_t algorithm;      /* Signature algorithm used by the key */
  uint64_t key_version;    /* Key version */
} VbPublicKey;


/* Signature data (a secure hash, possibly signed) */
typedef struct VbSignature {
  uint64_t sig_offset;  /* Offset of signature data from start of this
                         * struct */
  uint64_t sig_size;    /* Size of signature data from start of this struct */
  uint64_t data_size;   /* Size of the data block which was signed in bytes */
} VbSignature;


#define KEY_BLOCK_MAGIC "CHROMEOS"
#define KEY_BLOCK_MAGIC_SIZE 8

#define KEY_BLOCK_HEADER_VERSION_MAJOR 2
#define KEY_BLOCK_HEADER_VERSION_MINOR 1

/* Flags for key_block_flags */
/* The following flags set where the key is valid */
#define KEY_BLOCK_FLAG_DEVELOPER_0  UINT64_C(0x01)  /* Developer switch off */
#define KEY_BLOCK_FLAG_DEVELOPER_1  UINT64_C(0x02)  /* Developer switch on */
#define KEY_BLOCK_FLAG_RECOVERY_0   UINT64_C(0x04)  /* Not recovery mode */
#define KEY_BLOCK_FLAG_RECOVERY_1   UINT64_C(0x08)  /* Recovery mode */

/* Key block, containing the public key used to sign some other chunk
 * of data. */
typedef struct VbKeyBlockHeader {
  uint8_t magic[KEY_BLOCK_MAGIC_SIZE];  /* Magic number */
  uint32_t header_version_major;     /* Version of this header format */
  uint32_t header_version_minor;     /* Version of this header format */
  uint64_t key_block_size;           /* Length of this entire key block,
                                      * including keys, signatures, and
                                      * padding, in bytes */
  VbSignature key_block_signature;   /* Signature for this key block
                                      * (header + data pointed to by data_key)
                                      * For use with signed data keys*/
  VbSignature key_block_checksum;    /* SHA-512 checksum for this key block
                                      * (header + data pointed to by data_key)
                                      * For use with unsigned data keys */
  uint64_t key_block_flags;          /* Flags for key (KEY_BLOCK_FLAG_*) */
  VbPublicKey data_key;              /* Key to verify the chunk of data */
} VbKeyBlockHeader;
/* This should be followed by:
 *   1) The data_key key data, pointed to by data_key.key_offset.
 *   2) The checksum data for (VBKeyBlockHeader + data_key data), pointed to
 *      by key_block_checksum.sig_offset.
 *   3) The signature data for (VBKeyBlockHeader + data_key data), pointed to
 *      by key_block_signature.sig_offset. */


#define FIRMWARE_PREAMBLE_HEADER_VERSION_MAJOR 2
#define FIRMWARE_PREAMBLE_HEADER_VERSION_MINOR 0

/* Preamble block for rewritable firmware */
typedef struct VbFirmwarePreambleHeader {
  uint64_t preamble_size;            /* Size of this preamble, including keys,
                                      * signatures, and padding, in bytes */
  VbSignature preamble_signature;    /* Signature for this preamble
                                      * (header + kernel subkey +
                                      * body signature) */
  uint32_t header_version_major;     /* Version of this header format */
  uint32_t header_version_minor;     /* Version of this header format */

  uint64_t firmware_version;         /* Firmware version */
  VbPublicKey kernel_subkey;         /* Key to verify kernel key block */
  VbSignature body_signature;        /* Signature for the firmware body */
} VbFirmwarePreambleHeader;
/* This should be followed by:
 *   1) The kernel_subkey key data, pointed to by kernel_subkey.key_offset.
 *   2) The signature data for the firmware body, pointed to by
 *      body_signature.sig_offset.
 *   3) The signature data for (VBFirmwarePreambleHeader + kernel_subkey data
 *      + body signature data), pointed to by
 *      preamble_signature.sig_offset. */


#define KERNEL_PREAMBLE_HEADER_VERSION_MAJOR 2
#define KERNEL_PREAMBLE_HEADER_VERSION_MINOR 0

/* Preamble block for kernel */
typedef struct VbKernelPreambleHeader {
  uint64_t preamble_size;            /* Size of this preamble, including keys,
                                      * signatures, and padding, in bytes */
  VbSignature preamble_signature;    /* Signature for this preamble
                                      * (header + body signature) */
  uint32_t header_version_major;     /* Version of this header format */
  uint32_t header_version_minor;     /* Version of this header format */

  uint64_t kernel_version;           /* Kernel version */
  uint64_t body_load_address;        /* Load address for kernel body */
  uint64_t bootloader_address;       /* Address of bootloader, after body is
                                      * loaded at body_load_address */
  uint64_t bootloader_size;          /* Size of bootloader in bytes */
  VbSignature body_signature;        /* Signature for the kernel body */
} VbKernelPreambleHeader;
/* This should be followed by:
 *   2) The signature data for the kernel body, pointed to by
 *      body_signature.sig_offset.
 *   3) The signature data for (VBFirmwarePreambleHeader + body signature
 *      data), pointed to by preamble_signature.sig_offset. */

#endif  /* VBOOT_REFERENCE_VBOOT_STRUCT_H_ */
