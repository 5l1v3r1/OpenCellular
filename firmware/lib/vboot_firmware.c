/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * High-level firmware API for loading and verifying rewritable firmware.
 * (Firmware portion)
 */

#include "load_firmware_fw.h"
#include "rollback_index.h"
#include "utility.h"
#include "vboot_common.h"

/* Static variables for UpdateFirmwareBodyHash().  It's less than
 * optimal to have static variables in a library, but in UEFI the
 * caller is deep inside a different firmware stack and doesn't have a
 * good way to pass the params struct back to us. */
typedef struct VbLoadFirmwareInternal {
  DigestContext body_digest_context;
  uint64_t body_size_accum;
} VbLoadFirmwareInternal;


void UpdateFirmwareBodyHash(LoadFirmwareParams* params,
                             uint8_t* data, uint64_t size) {
  VbLoadFirmwareInternal* lfi =
      (VbLoadFirmwareInternal*)params->load_firmware_internal;

  DigestUpdate(&lfi->body_digest_context, data, size);
  lfi->body_size_accum += size;
}


int LoadFirmware(LoadFirmwareParams* params) {

  VbPublicKey* root_key = (VbPublicKey*)params->firmware_root_key_blob;
  VbLoadFirmwareInternal* lfi;

  uint16_t tpm_key_version = 0;
  uint16_t tpm_fw_version = 0;
  uint64_t lowest_key_version = 0xFFFF;
  uint64_t lowest_fw_version = 0xFFFF;
  int good_index = -1;
  int index;

  /* Clear output params in case we fail */
  params->firmware_index = 0;

  VBDEBUG(("LoadFirmware started...\n"));

  if (params->kernel_sign_key_size < sizeof(VbPublicKey)) {
    VBDEBUG(("Kernel sign key buffer too small\n"));
    return LOAD_FIRMWARE_RECOVERY;
  }

  /* Must have a root key */
  if (!root_key) {
    VBDEBUG(("No root key\n"));
    return LOAD_FIRMWARE_RECOVERY;
  }

  /* Initialize the TPM and read rollback indices. */
  if (0 != RollbackFirmwareSetup(
          (params->boot_flags & BOOT_FLAG_DEVELOPER ? 1 : 0),
          &tpm_key_version, &tpm_fw_version)) {
    VBDEBUG(("Unable to get stored versions.\n"));
    return LOAD_FIRMWARE_RECOVERY;
  }

  /* Allocate our internal data */
  lfi = (VbLoadFirmwareInternal*)Malloc(sizeof(VbLoadFirmwareInternal));
  if (!lfi)
    return LOAD_FIRMWARE_RECOVERY;
  params->load_firmware_internal = (uint8_t*)lfi;

  /* Loop over indices */
  for (index = 0; index < 2; index++) {
    VbKeyBlockHeader* key_block;
    uint64_t vblock_size;
    VbFirmwarePreambleHeader* preamble;
    RSAPublicKey* data_key;
    uint64_t key_version;
    uint8_t* body_digest;

    /* Verify the key block */
    if (0 == index) {
      key_block = (VbKeyBlockHeader*)params->verification_block_0;
      vblock_size = params->verification_size_0;
    } else {
      key_block = (VbKeyBlockHeader*)params->verification_block_1;
      vblock_size = params->verification_size_1;
    }
    if ((0 != KeyBlockVerify(key_block, vblock_size, root_key))) {
      VBDEBUG(("Key block verification failed.\n"));
      continue;
    }

    /* Check for rollback of key version. */
    key_version = key_block->data_key.key_version;
    if (key_version < tpm_key_version) {
      VBDEBUG(("Key rollback detected.\n"));
      continue;
    }

    /* Get the key for preamble/data verification from the key block. */
    data_key = PublicKeyToRSA(&key_block->data_key);
    if (!data_key) {
      VBDEBUG(("Unable to parse data key.\n"));
      continue;
    }

    /* Verify the preamble, which follows the key block. */
    preamble = (VbFirmwarePreambleHeader*)((uint8_t*)key_block +
                                           key_block->key_block_size);
    if ((0 != VerifyFirmwarePreamble2(preamble,
                                      vblock_size - key_block->key_block_size,
                                      data_key))) {
      VBDEBUG(("Preamble verfication failed.\n"));
      RSAPublicKeyFree(data_key);
      continue;
    }

    /* Check for rollback of firmware version. */
    if (key_version == tpm_key_version &&
        preamble->firmware_version < tpm_fw_version) {
      VBDEBUG(("Firmware version rollback detected.\n"));
      RSAPublicKeyFree(data_key);
      continue;
    }

    /* Check for lowest key version from a valid header. */
    if (lowest_key_version > key_version) {
      lowest_key_version = key_version;
      lowest_fw_version = preamble->firmware_version;
    }
    else if (lowest_key_version == key_version &&
             lowest_fw_version > preamble->firmware_version) {
      lowest_fw_version = preamble->firmware_version;
    }

    /* If we already have good firmware, no need to read another one;
     * we only needed to look at the versions to check for
     * rollback. */
    if (-1 != good_index)
      continue;

    /* Read the firmware data */
    DigestInit(&lfi->body_digest_context, data_key->algorithm);
    lfi->body_size_accum = 0;
    if (0 != GetFirmwareBody(params, index)) {
      VBDEBUG(("GetFirmwareBody() failed for index %d\n", index));
      RSAPublicKeyFree(data_key);
      continue;
    }
    if (lfi->body_size_accum != preamble->body_signature.data_size) {
      VBDEBUG(("Hash updated %d bytes but expected %d\n",
               (int)lfi->body_size_accum,
               (int)preamble->body_signature.data_size));
      RSAPublicKeyFree(data_key);
      continue;
    }

    /* Verify firmware data */
    body_digest = DigestFinal(&lfi->body_digest_context);
    if (0 != VerifyDigest(body_digest, &preamble->body_signature, data_key)) {
      VBDEBUG(("Firmware body verification failed.\n"));
      RSAPublicKeyFree(data_key);
      Free(body_digest);
      continue;
    }

    /* Done with the digest and data key, so can free them now */
    RSAPublicKeyFree(data_key);
    Free(body_digest);

    /* If we're still here, the firmware is valid. */
    VBDEBUG(("Firmware %d is valid.\n", index));
    if (-1 == good_index) {
      VbPublicKey *kdest = (VbPublicKey*)params->kernel_sign_key_blob;

      /* Copy the kernel sign key blob into the destination buffer */
      PublicKeyInit(kdest, (uint8_t*)(kdest + 1),
                    (params->kernel_sign_key_size - sizeof(VbPublicKey)));

      if (0 != PublicKeyCopy(kdest, &preamble->kernel_subkey)) {
        VBDEBUG(("Kernel subkey too big for buffer.\n"));
        continue;  /* The firmware signature was good, but the public
                    * key was bigger that the caller can handle. */
      }

      /* Save the key size we actually used */
      params->kernel_sign_key_size = kdest->key_offset + kdest->key_size;

      /* Save the good index, now that we're sure we can actually use
       * this firmware.  That's the one we'll boot. */
      good_index = index;
      params->firmware_index = index;

      /* If the good firmware's key version is the same as the tpm,
       * then the TPM doesn't need updating; we can stop now.
       * Otherwise, we'll check all the other headers to see if they
       * contain a newer key. */
      if (key_version == tpm_key_version &&
          preamble->firmware_version == tpm_fw_version)
        break;
    }
  }

  /* Free internal data */
  Free(lfi);
  params->load_firmware_internal = NULL;

  /* Handle finding good firmware */
  if (good_index >= 0) {

    /* Update TPM if necessary */
    if ((lowest_key_version > tpm_key_version) ||
        (lowest_key_version == tpm_key_version &&
         lowest_fw_version > tpm_fw_version)) {
      if (0 != RollbackFirmwareWrite((uint16_t)lowest_key_version,
                                     (uint16_t)lowest_fw_version)) {
        VBDEBUG(("Unable to write stored versions.\n"));
        return LOAD_FIRMWARE_RECOVERY;
      }
    }

    /* Lock firmware versions in TPM */
    if (0 != RollbackFirmwareLock()) {
      VBDEBUG(("Unable to lock firmware versions.\n"));
      return LOAD_FIRMWARE_RECOVERY;
    }

    /* Success */
    VBDEBUG(("Will boot firmware index %d\n", (int)params->firmware_index));
    return LOAD_FIRMWARE_SUCCESS;
  }

  /* If we're still here, no good firmware, so go to recovery mode. */
  VBDEBUG(("Alas, no good firmware.\n"));
  return LOAD_FIRMWARE_RECOVERY;
}
