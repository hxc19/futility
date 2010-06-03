/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Data structure and API definitions for a verified boot kernel image.
 * (Firmware Portion)
 */

#ifndef VBOOT_REFERENCE_KERNEL_IMAGE_FW_H_
#define VBOOT_REFERENCE_KERNEL_IMAGE_FW_H_

#include <stdint.h>

#include "cryptolib.h"

#define KERNEL_MAGIC "CHROMEOS"
#define KERNEL_MAGIC_SIZE 8

#define DEV_MODE_ENABLED 1
#define DEV_MODE_DISABLED 0

typedef struct KernelImage {
  uint8_t magic[KERNEL_MAGIC_SIZE];
  /* Key header */
  uint16_t header_version;  /* Header version. */
  uint16_t header_len;  /* Length of the header. */
  uint16_t firmware_sign_algorithm;  /* Signature algorithm used by the firmware
                                      * signing key (used to sign this kernel
                                      * header. */
  uint16_t kernel_sign_algorithm;  /* Signature algorithm used by the kernel
                                    * signing key. */
  uint16_t kernel_key_version;  /* Key Version# for preventing rollbacks. */
  uint8_t* kernel_sign_key;  /* Pre-processed public half of signing key. */
  /* TODO(gauravsh): Do we need a choice of digest algorithms for the header
   * checksum? */
  uint8_t header_checksum[SHA512_DIGEST_SIZE];  /* SHA-512 Crytographic hash of
                                                 * the concatenation of the
                                                 * header fields, i.e.
                                                 * [header_len,
                                                 * firmware_sign_algorithm,
                                                 * sign_algorithm, sign_key,
                                                 * key_version] */
  /* End of kernel key header. */
  uint8_t* kernel_key_signature;   /* Signature of the header above. */

  /* Kernel preamble */
  uint16_t kernel_version;  /* Kernel Version# for preventing rollbacks. */
  uint64_t kernel_len;  /* Length of the actual kernel image. */
  uint64_t bootloader_offset;  /* Offset of bootloader in kernel_data. */
  uint64_t bootloader_size;  /* Size of bootloader in bytes. */
  uint64_t padded_header_size;  /* start of kernel_data in disk partition */
  uint8_t* kernel_signature;  /* Signature on [kernel_data] below.
                               * NOTE: This is only considered valid
                               * if preamble_signature successfully verifies. */
  /* end of preamble */
  uint8_t* preamble_signature;  /* signature on preamble, (includes
                                   [kernel_signature]) */
  uint8_t* kernel_data;  /* Actual kernel data. */

} KernelImage;

/* Error Codes for VerifyFirmware. */
#define VERIFY_KERNEL_SUCCESS 0
#define VERIFY_KERNEL_INVALID_IMAGE 1
#define VERIFY_KERNEL_KEY_SIGNATURE_FAILED 2
#define VERIFY_KERNEL_INVALID_ALGORITHM 3
#define VERIFY_KERNEL_PREAMBLE_SIGNATURE_FAILED 4
#define VERIFY_KERNEL_SIGNATURE_FAILED 5
#define VERIFY_KERNEL_WRONG_MAGIC 6
#define VERIFY_KERNEL_MAX 7  /* Generic catch-all. */

extern char* kVerifyKernelErrors[VERIFY_KERNEL_MAX];

/* Returns the length of the verified boot kernel preamble based on
 * kernel signing algorithm [algorithm]. */
uint64_t GetKernelPreambleLen(int algorithm);

/* Returns the length of the Kernel Verified Boot header excluding
 * [kernel_data].
 *
 * This is always non-zero, so a return value of 0 signifies an error.
 */
uint64_t GetVBlockHeaderSize(const uint8_t* vkernel_blob);

/* Checks for the sanity of the kernel key header at [kernel_header_blob].
 * If [dev_mode] is enabled, also checks the kernel key signature using the
 * pre-processed public firmware signing  key [firmware_sign_key_blob].
 *
 * On success, puts firmware signature algorithm in [firmware_algorithm],
 * kernel signature algorithm in [kernel_algorithm], kernel header
 * length in [header_len], and return 0.
 * Else, return error code on failure.
 */
int VerifyKernelKeyHeader(const uint8_t* firmware_sign_key_blob,
                          const uint8_t* kernel_header_blob,
                          const int dev_mode,
                          int* firmware_algorithm,
                          int* kernel_algorithm,
                          int* header_len);

/* Checks the kernel preamble signature at [kernel_preamble_blob]
 * using the signing key [kernel_sign_key].
 *
 * On success, put kernel length into [kernel_len], and return 0.
 * Else, return error code on failure.
 */
int VerifyKernelPreamble(RSAPublicKey* kernel_sign_key,
                         const uint8_t* kernel_preamble_blob,
                         int algorithm,
                         uint64_t* kernel_len);

/* Checks [kernel_signature] on the kernel data at location [kernel_data]. The
 * signature is assumed to be generated using algorithm [algorithm].
 * The length of the kernel data is [kernel_len].
 *
 * Return 0 on success, error code on failure.
 */
int VerifyKernelData(RSAPublicKey* kernel_sign_key,
                     const uint8_t* kernel_signature,
                     const uint8_t* kernel_data,
                     uint64_t kernel_len,
                     int algorithm);

/* Verifies the kernel key header and preamble at [kernel_header_blob]
 * using the firmware public key [firmware_key_blob]. If [dev_mode] is 1
 * (active), then key header verification is skipped.
 *
 * On success, fills in the fields of image with the kernel header and
 * preamble fields.
 *
 * Note that pointers in the image point directly into the input
 * kernel_header_blob.  image->kernel_data is set to NULL, since it's not
 * part of the header and preamble data itself.
 *
 * On success, the signing key to use for kernel data verification is
 * returned in [kernel_sign_key], This must be free-d explicitly by
 * the caller after use.  On failure, the signing key is set to NULL.
 *
 * Returns 0 on success, error code on failure.
 */
int VerifyKernelHeader(const uint8_t* firmware_key_blob,
                       const uint8_t* kernel_header_blob,
                       uint64_t kernel_header_blob_len,
                       const int dev_mode,
                       KernelImage* image,
                       RSAPublicKey** kernel_sign_key);

/* Performs a chained verify of the kernel blob [kernel_blob]. If
 * [dev_mode] is 0 [inactive], then the pre-processed public signing key
 * [root_key_blob] is used to verify the signature of the signing key,
 * else the check is skipped.
 * Returns 0 on success, error code on failure.
 *
 * NOTE: The length of the kernel blob is derived from reading the fields
 * in the first few bytes of the buffer. This might look risky but in firmware
 * land, the start address of the kernel_blob will always be fixed depending
 * on the memory map on the particular platform. In addition, the signature on
 * length itself is checked early in the verification process for extra safety.
 */
int VerifyKernel(const uint8_t* signing_key_blob,
                 const uint8_t* kernel_blob,
                 const int dev_mode);

/* Returns the logical version of a kernel blob which is calculated as
 * (kernel_key_version << 16 | kernel_version). */
uint32_t GetLogicalKernelVersion(uint8_t* kernel_blob);

#endif  /* VBOOT_REFERENCE_KERNEL_IMAGE_FW_H_ */
