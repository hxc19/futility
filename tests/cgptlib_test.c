/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <string.h>

#include "../cgpt/cgpt.h"
#include "../cgpt/flash_ts.h"
#include "cgptlib_internal.h"
#include "cgptlib_test.h"
#include "crc32.h"
#include "crc32_test.h"
#include "gpt.h"
#include "mtdlib.h"
#include "mtdlib_unused.h"
#include "test_common.h"
#define _STUB_IMPLEMENTATION_
#include "utility.h"

/*
 * Testing partition layout (sector_bytes=512)
 *
 *     LBA   Size  Usage
 * ---------------------------------------------------------
 *       0      1  PMBR
 *       1      1  primary partition header
 *       2     32  primary partition entries (128B * 128)
 *      34    100  kernel A (index: 0)
 *     134    100  root A (index: 1)
 *     234    100  root B (index: 2)
 *     334    100  kernel B (index: 3)
 *     434     32  secondary partition entries
 *     466      1  secondary partition header
 *     467
 */
#define KERNEL_A 0
#define KERNEL_B 1
#define ROOTFS_A 2
#define ROOTFS_B 3
#define KERNEL_X 2 /* Overload ROOTFS_A, for some GetNext tests */
#define KERNEL_Y 3 /* Overload ROOTFS_B, for some GetNext tests */

#define DEFAULT_SECTOR_SIZE 512
#define MAX_SECTOR_SIZE 4096
#define DEFAULT_DRIVE_SECTORS 467
#define PARTITION_ENTRIES_SIZE TOTAL_ENTRIES_SIZE /* 16384 */

static const Guid guid_zero = {{{0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0}}}};
static const Guid guid_kernel = GPT_ENT_TYPE_CHROMEOS_KERNEL;
static const Guid guid_rootfs = GPT_ENT_TYPE_CHROMEOS_ROOTFS;

// cgpt_common.c requires these be defined if linked in.
const char *progname = "CGPT-TEST";
const char *command = "TEST";

// Ramdisk for flash ts testing.
static uint8_t *nand_drive = NULL;
static uint32_t nand_drive_sz;
static uint8_t *nand_bad_block_map = NULL;

/*
 * Copy a random-for-this-program-only Guid into the dest. The num parameter
 * completely determines the Guid.
 */
static void SetGuid(void *dest, uint32_t num)
{
	Guid g = {{{num,0xd450,0x44bc,0xa6,0x93,
		    {0xb8,0xac,0x75,0x5f,0xcd,0x48}}}};
	Memcpy(dest, &g, sizeof(Guid));
}

/*
 * Given a GptData pointer, first re-calculate entries CRC32 value, then reset
 * header CRC32 value to 0, and calculate header CRC32 value.  Both primary and
 * secondary are updated.
 */
static void RefreshCrc32(GptData *gpt)
{
	GptHeader *header, *header2;
	GptEntry *entries, *entries2;

	header = (GptHeader *)gpt->primary_header;
	entries = (GptEntry *)gpt->primary_entries;
	header2 = (GptHeader *)gpt->secondary_header;
	entries2 = (GptEntry *)gpt->secondary_entries;

	header->entries_crc32 =
		Crc32((uint8_t *)entries,
		      header->number_of_entries * header->size_of_entry);
	header->header_crc32 = 0;
	header->header_crc32 = Crc32((uint8_t *)header, header->size);
	header2->entries_crc32 =
		Crc32((uint8_t *)entries2,
		      header2->number_of_entries * header2->size_of_entry);
	header2->header_crc32 = 0;
	header2->header_crc32 = Crc32((uint8_t *)header2, header2->size);
}

static void ZeroHeaders(GptData *gpt)
{
	Memset(gpt->primary_header, 0, MAX_SECTOR_SIZE);
	Memset(gpt->secondary_header, 0, MAX_SECTOR_SIZE);
}

static void ZeroEntries(GptData *gpt)
{
	Memset(gpt->primary_entries, 0, PARTITION_ENTRIES_SIZE);
	Memset(gpt->secondary_entries, 0, PARTITION_ENTRIES_SIZE);
}

static void ZeroHeadersEntries(GptData *gpt)
{
	ZeroHeaders(gpt);
	ZeroEntries(gpt);
}

/*
 * Return a pointer to a static GptData instance (no free is required).
 * All fields are zero except 4 pointers linking to header and entries.
 * All content of headers and entries are zero.
 */
static GptData *GetEmptyGptData(void)
{
	static GptData gpt;
	static uint8_t primary_header[MAX_SECTOR_SIZE];
	static uint8_t primary_entries[PARTITION_ENTRIES_SIZE];
	static uint8_t secondary_header[MAX_SECTOR_SIZE];
	static uint8_t secondary_entries[PARTITION_ENTRIES_SIZE];

	Memset(&gpt, 0, sizeof(gpt));
	gpt.primary_header = primary_header;
	gpt.primary_entries = primary_entries;
	gpt.secondary_header = secondary_header;
	gpt.secondary_entries = secondary_entries;
	ZeroHeadersEntries(&gpt);

	/* Initialize GptData internal states. */
	gpt.current_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;

	return &gpt;
}

static MtdData *GetEmptyMtdData() {
	static MtdData mtd;
	Memset(&mtd, 0, sizeof(mtd));
	mtd.current_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;
	return &mtd;
}

/*
 * Fill in most of fields and creates the layout described in the top of this
 * file. Before calling this function, primary/secondary header/entries must
 * have been pointed to the buffer, say, a gpt returned from GetEmptyGptData().
 * This function returns a good (valid) copy of GPT layout described in top of
 * this file.
 */
static void BuildTestGptData(GptData *gpt)
{
	GptHeader *header, *header2;
	GptEntry *entries, *entries2;
	Guid chromeos_kernel = GPT_ENT_TYPE_CHROMEOS_KERNEL;
	Guid chromeos_rootfs = GPT_ENT_TYPE_CHROMEOS_ROOTFS;

	gpt->sector_bytes = DEFAULT_SECTOR_SIZE;
	gpt->drive_sectors = DEFAULT_DRIVE_SECTORS;
	gpt->current_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;
	gpt->valid_headers = MASK_BOTH;
	gpt->valid_entries = MASK_BOTH;
	gpt->modified = 0;

	/* Build primary */
	header = (GptHeader *)gpt->primary_header;
	entries = (GptEntry *)gpt->primary_entries;
	Memcpy(header->signature, GPT_HEADER_SIGNATURE,
	       sizeof(GPT_HEADER_SIGNATURE));
	header->revision = GPT_HEADER_REVISION;
	header->size = sizeof(GptHeader);
	header->reserved_zero = 0;
	header->my_lba = 1;
	header->alternate_lba = DEFAULT_DRIVE_SECTORS - 1;
	header->first_usable_lba = 34;
	header->last_usable_lba = DEFAULT_DRIVE_SECTORS - 1 - 32 - 1;  /* 433 */
	header->entries_lba = 2;
	  /* 512B / 128B * 32sectors = 128 entries */
	header->number_of_entries = 128;
	header->size_of_entry = 128;  /* bytes */
	Memcpy(&entries[0].type, &chromeos_kernel, sizeof(chromeos_kernel));
	SetGuid(&entries[0].unique, 0);
	entries[0].starting_lba = 34;
	entries[0].ending_lba = 133;
	Memcpy(&entries[1].type, &chromeos_rootfs, sizeof(chromeos_rootfs));
	SetGuid(&entries[1].unique, 1);
	entries[1].starting_lba = 134;
	entries[1].ending_lba = 232;
	Memcpy(&entries[2].type, &chromeos_rootfs, sizeof(chromeos_rootfs));
	SetGuid(&entries[2].unique, 2);
	entries[2].starting_lba = 234;
	entries[2].ending_lba = 331;
	Memcpy(&entries[3].type, &chromeos_kernel, sizeof(chromeos_kernel));
	SetGuid(&entries[3].unique, 3);
	entries[3].starting_lba = 334;
	entries[3].ending_lba = 430;

	/* Build secondary */
	header2 = (GptHeader *)gpt->secondary_header;
	entries2 = (GptEntry *)gpt->secondary_entries;
	Memcpy(header2, header, sizeof(GptHeader));
	Memcpy(entries2, entries, PARTITION_ENTRIES_SIZE);
	header2->my_lba = DEFAULT_DRIVE_SECTORS - 1;  /* 466 */
	header2->alternate_lba = 1;
	header2->entries_lba = DEFAULT_DRIVE_SECTORS - 1 - 32;  /* 434 */

	RefreshCrc32(gpt);
}

static void BuildTestMtdData(MtdData *mtd) {
	MtdDiskPartition *partitions;

	mtd->sector_bytes = DEFAULT_SECTOR_SIZE;
	mtd->drive_sectors = DEFAULT_DRIVE_SECTORS;
	mtd->current_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;
	mtd->modified = 0;
	Memset(&mtd->primary, 0, sizeof(mtd->primary));

	Memcpy(mtd->primary.signature, MTD_DRIVE_SIGNATURE,
		sizeof(mtd->primary.signature));
	mtd->primary.first_offset = 32 * DEFAULT_SECTOR_SIZE;
	mtd->primary.last_offset = DEFAULT_DRIVE_SECTORS * DEFAULT_SECTOR_SIZE - 1;
	mtd->primary.size = MTD_DRIVE_V1_SIZE;

	/* These values are not used directly by the library, but they are checked */
	mtd->flash_page_bytes = mtd->sector_bytes * 8;
	mtd->flash_block_bytes = mtd->flash_page_bytes * 8;
	mtd->fts_block_offset = 1;
	mtd->fts_block_size = 1;

	partitions = &mtd->primary.partitions[0];
	partitions[0].starting_offset = 34 * DEFAULT_SECTOR_SIZE;
	partitions[0].ending_offset = 134 * DEFAULT_SECTOR_SIZE - 1;
	partitions[0].flags =
		MTD_PARTITION_TYPE_CHROMEOS_KERNEL << MTD_ATTRIBUTE_TYPE_OFFSET;
	partitions[1].starting_offset = 134 * DEFAULT_SECTOR_SIZE;
	partitions[1].ending_offset = 233 * DEFAULT_SECTOR_SIZE - 1;
	partitions[1].flags =
		MTD_PARTITION_TYPE_CHROMEOS_ROOTFS << MTD_ATTRIBUTE_TYPE_OFFSET;
	partitions[2].starting_offset = 234 * DEFAULT_SECTOR_SIZE;
	partitions[2].ending_offset = 332 * DEFAULT_SECTOR_SIZE - 1;
	partitions[2].flags =
		MTD_PARTITION_TYPE_CHROMEOS_KERNEL << MTD_ATTRIBUTE_TYPE_OFFSET;
	partitions[3].starting_offset = 334 * DEFAULT_SECTOR_SIZE;
	partitions[3].ending_offset = 431 * DEFAULT_SECTOR_SIZE - 1;
	partitions[3].flags =
		MTD_PARTITION_TYPE_CHROMEOS_ROOTFS << MTD_ATTRIBUTE_TYPE_OFFSET;

	mtd->primary.crc32 = 0;
	mtd->primary.crc32 = Crc32(&mtd->primary, MTD_DRIVE_V1_SIZE);
}


/*
 * Test if the structures are the expected size; if this fails, struct packing
 * is not working properly.
 */
static int StructSizeTest(void)
{

	EXPECT(GUID_EXPECTED_SIZE == sizeof(Guid));
	EXPECT(GPTHEADER_EXPECTED_SIZE == sizeof(GptHeader));
	EXPECT(GPTENTRY_EXPECTED_SIZE == sizeof(GptEntry));
	EXPECT(MTDENTRY_EXPECTED_SIZE == sizeof(MtdDiskPartition));
	EXPECT(MTDLAYOUT_EXPECTED_SIZE == sizeof(MtdDiskLayout));
	return TEST_OK;
}


/* Test if the default structure returned by BuildTestGptData() is good. */
static int TestBuildTestGptData(void)
{
	GptData *gpt;

	gpt = GetEmptyGptData();
	BuildTestGptData(gpt);
	EXPECT(GPT_SUCCESS == GptInit(gpt));
	gpt->sector_bytes = 0;
	EXPECT(GPT_ERROR_INVALID_SECTOR_SIZE == GptInit(gpt));
	return TEST_OK;
}

static int TestBuildTestMtdData() {
	MtdData *mtd = GetEmptyMtdData();

	BuildTestMtdData(mtd);
	EXPECT(GPT_SUCCESS == MtdInit(mtd));
	return TEST_OK;
}

/*
 * Test if wrong sector_bytes or drive_sectors is detected by GptInit().
 * Currently we only support 512 bytes per sector.  In the future, we may
 * support other sizes.  A too small drive_sectors should be rejected by
 * GptInit().
 * For MtdInit(), additionally test various flash geometries to verify
 * that only valid ones are accepted.
 */
static int ParameterTests(void)
{
	GptData *gpt;
	MtdData *mtd;
	struct {
		uint32_t sector_bytes;
		uint64_t drive_sectors;
		int expected_retval;
	} cases[] = {
		{512, DEFAULT_DRIVE_SECTORS, GPT_SUCCESS},
		{520, DEFAULT_DRIVE_SECTORS, GPT_ERROR_INVALID_SECTOR_SIZE},
		{512, 0, GPT_ERROR_INVALID_SECTOR_NUMBER},
		{512, 66, GPT_ERROR_INVALID_SECTOR_NUMBER},
		{512, GPT_PMBR_SECTORS + GPT_HEADER_SECTORS * 2 +
		 GPT_ENTRIES_SECTORS * 2, GPT_SUCCESS},
		{4096, DEFAULT_DRIVE_SECTORS, GPT_ERROR_INVALID_SECTOR_SIZE},
	};
	struct {
		uint32_t sector_bytes;
		uint32_t drive_sectors;
		uint32_t flash_page_bytes;
		uint32_t flash_block_bytes;
		int expected_retval;
	} mtdcases[] = {
		{512, DEFAULT_DRIVE_SECTORS, 8*512,
			8*512, GPT_SUCCESS},
		{510, DEFAULT_DRIVE_SECTORS, 8*512,
			8*512, GPT_ERROR_INVALID_SECTOR_SIZE},
		{512, DEFAULT_DRIVE_SECTORS, 8*512,
			8*512, GPT_SUCCESS},
		{512, DEFAULT_DRIVE_SECTORS, 512,
			8*512, GPT_SUCCESS},
		{512, DEFAULT_DRIVE_SECTORS, 8*512,
			10*512, GPT_ERROR_INVALID_FLASH_GEOMETRY},
		{512, DEFAULT_DRIVE_SECTORS, 3*512,
			9*512, GPT_SUCCESS},
		{512, DEFAULT_DRIVE_SECTORS, 8*512,
			6*512, GPT_ERROR_INVALID_FLASH_GEOMETRY},
		{512, DEFAULT_DRIVE_SECTORS, 256,
			6*512, GPT_ERROR_INVALID_FLASH_GEOMETRY},
		{512, DEFAULT_DRIVE_SECTORS, 512,
			6*512 + 256, GPT_ERROR_INVALID_FLASH_GEOMETRY},
	};
	int i;

	gpt = GetEmptyGptData();
	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestGptData(gpt);
		gpt->sector_bytes = cases[i].sector_bytes;
		gpt->drive_sectors = cases[i].drive_sectors;
		EXPECT(cases[i].expected_retval == CheckParameters(gpt));
	}

	mtd = GetEmptyMtdData();
	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestMtdData(mtd);
		mtd->sector_bytes = mtdcases[i].sector_bytes;
		mtd->drive_sectors = mtdcases[i].drive_sectors;
		mtd->flash_block_bytes = mtdcases[i].flash_block_bytes;
		mtd->flash_page_bytes = mtdcases[i].flash_page_bytes;
		if(mtdcases[i].expected_retval != MtdCheckParameters(mtd)) {
			printf("i=%d\n",i);
		}
		EXPECT(mtdcases[i].expected_retval == MtdCheckParameters(mtd));
	}

	return TEST_OK;
}

/* Test if header CRC in two copies are calculated. */
static int HeaderCrcTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;

	BuildTestGptData(gpt);
	EXPECT(HeaderCrc(h1) == h1->header_crc32);

	/* CRC covers first byte of header */
	BuildTestGptData(gpt);
	gpt->primary_header[0] ^= 0xa5;
	EXPECT(HeaderCrc(h1) != h1->header_crc32);

	/* CRC covers last byte of header */
	BuildTestGptData(gpt);
	gpt->primary_header[h1->size - 1] ^= 0x5a;
	EXPECT(HeaderCrc(h1) != h1->header_crc32);

	/* CRC only covers header */
	BuildTestGptData(gpt);
	gpt->primary_header[h1->size] ^= 0x5a;
	EXPECT(HeaderCrc(h1) == h1->header_crc32);

	return TEST_OK;
}

/* Test if header-same comparison works. */
static int HeaderSameTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;
	GptHeader h3;

	EXPECT(0 == HeaderFieldsSame(h1, h2));

	Memcpy(&h3, h2, sizeof(h3));
	h3.signature[0] ^= 0xba;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.revision++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.size++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.reserved_zero++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.first_usable_lba++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.last_usable_lba++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.disk_uuid.u.raw[0] ^= 0xba;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.number_of_entries++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.size_of_entry++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	Memcpy(&h3, h2, sizeof(h3));
	h3.entries_crc32++;
	EXPECT(1 == HeaderFieldsSame(h1, &h3));

	return TEST_OK;
}

/* Test if signature ("EFI PART") is checked. */
static int SignatureTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;
	int i;

	EXPECT(1 == CheckHeader(NULL, 0, gpt->drive_sectors));

	for (i = 0; i < 8; ++i) {
		BuildTestGptData(gpt);
		h1->signature[i] ^= 0xff;
		h2->signature[i] ^= 0xff;
		RefreshCrc32(gpt);
		EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
		EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));
	}

	return TEST_OK;
}

/*
 * The revision we currently support is GPT_HEADER_REVISION.  If the revision
 * in header is not that, we expect the header is invalid.
 */
static int RevisionTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;
	int i;

	struct {
		uint32_t value_to_test;
		int expect_rv;
	} cases[] = {
		{0x01000000, 1},
		{0x00010000, 0},  /* GPT_HEADER_REVISION */
		{0x00000100, 1},
		{0x00000001, 1},
		{0x23010456, 1},
	};

	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestGptData(gpt);
		h1->revision = cases[i].value_to_test;
		h2->revision = cases[i].value_to_test;
		RefreshCrc32(gpt);

		EXPECT(CheckHeader(h1, 0, gpt->drive_sectors) ==
		       cases[i].expect_rv);
		EXPECT(CheckHeader(h2, 1, gpt->drive_sectors) ==
		       cases[i].expect_rv);
	}
	return TEST_OK;
}

static int SizeTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;
	int i;

	struct {
		uint32_t value_to_test;
		int expect_rv;
	} cases[] = {
		{91, 1},
		{92, 0},
		{93, 0},
		{511, 0},
		{512, 0},
		{513, 1},
	};

	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestGptData(gpt);
		h1->size = cases[i].value_to_test;
		h2->size = cases[i].value_to_test;
		RefreshCrc32(gpt);

		EXPECT(CheckHeader(h1, 0, gpt->drive_sectors) ==
		       cases[i].expect_rv);
		EXPECT(CheckHeader(h2, 1, gpt->drive_sectors) ==
		       cases[i].expect_rv);
	}
	return TEST_OK;
}

/* Test if CRC is checked. */
static int CrcFieldTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;

	BuildTestGptData(gpt);
	/* Modify a field that the header verification doesn't care about */
	h1->entries_crc32++;
	h2->entries_crc32++;
	EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));
	/* Refresh the CRC; should pass now */
	RefreshCrc32(gpt);
	EXPECT(0 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(0 == CheckHeader(h2, 1, gpt->drive_sectors));

	return TEST_OK;
}

/* Test if reserved fields are checked.  We'll try non-zero values to test. */
static int ReservedFieldsTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;

	BuildTestGptData(gpt);
	h1->reserved_zero ^= 0x12345678;  /* whatever random */
	h2->reserved_zero ^= 0x12345678;  /* whatever random */
	RefreshCrc32(gpt);
	EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));

#ifdef PADDING_CHECKED
	/* TODO: padding check is currently disabled */
	BuildTestGptData(gpt);
	h1->padding[12] ^= 0x34;  /* whatever random */
	h2->padding[56] ^= 0x78;  /* whatever random */
	RefreshCrc32(gpt);
	EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));
#endif

	return TEST_OK;
}

/*
 * Technically, any size which is 2^N where N > 6 should work, but our
 * library only supports one size.
 */
static int SizeOfPartitionEntryTest(void) {
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;
	int i;

	struct {
		uint32_t value_to_test;
		int expect_rv;
	} cases[] = {
		{127, 1},
		{128, 0},
		{129, 1},
		{256, 1},
		{512, 1},
	};

	/* Check size of entryes */
	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestGptData(gpt);
		h1->size_of_entry = cases[i].value_to_test;
		h2->size_of_entry = cases[i].value_to_test;
		h1->number_of_entries = TOTAL_ENTRIES_SIZE /
			cases[i].value_to_test;
		h2->number_of_entries = TOTAL_ENTRIES_SIZE /
			cases[i].value_to_test;
		RefreshCrc32(gpt);

		EXPECT(CheckHeader(h1, 0, gpt->drive_sectors) ==
		       cases[i].expect_rv);
		EXPECT(CheckHeader(h2, 1, gpt->drive_sectors) ==
		       cases[i].expect_rv);
	}

	return TEST_OK;
}

/*
 * Technically, any size which is 2^N where N > 6 should work, but our library
 * only supports one size.
 */
static int NumberOfPartitionEntriesTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;

	BuildTestGptData(gpt);
	h1->number_of_entries--;
	h2->number_of_entries /= 2;
	RefreshCrc32(gpt);
	EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));

	return TEST_OK;
}


/* Test if myLBA field is checked (1 for primary, last for secondary). */
static int MyLbaTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;

	/* myLBA depends on primary vs secondary flag */
	BuildTestGptData(gpt);
	EXPECT(1 == CheckHeader(h1, 1, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 0, gpt->drive_sectors));

	BuildTestGptData(gpt);
	h1->my_lba--;
	h2->my_lba--;
	RefreshCrc32(gpt);
	EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));

	BuildTestGptData(gpt);
	h1->my_lba = 2;
	h2->my_lba--;
	RefreshCrc32(gpt);
	EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));

	/* We should ignore the alternate_lba field entirely */
	BuildTestGptData(gpt);
	h1->alternate_lba++;
	h2->alternate_lba++;
	RefreshCrc32(gpt);
	EXPECT(0 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(0 == CheckHeader(h2, 1, gpt->drive_sectors));

	BuildTestGptData(gpt);
	h1->alternate_lba--;
	h2->alternate_lba--;
	RefreshCrc32(gpt);
	EXPECT(0 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(0 == CheckHeader(h2, 1, gpt->drive_sectors));

	BuildTestGptData(gpt);
	h1->entries_lba++;
	h2->entries_lba++;
	RefreshCrc32(gpt);
	/*
	 * We support a padding between primary GPT header and its entries. So
	 * this still passes.
	 */
	EXPECT(0 == CheckHeader(h1, 0, gpt->drive_sectors));
	/*
	 * But the secondary table should fail because it would overlap the
	 * header, which is now lying after its entry array.
	 */
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));

	BuildTestGptData(gpt);
	h1->entries_lba--;
	h2->entries_lba--;
	RefreshCrc32(gpt);
	EXPECT(1 == CheckHeader(h1, 0, gpt->drive_sectors));
	EXPECT(1 == CheckHeader(h2, 1, gpt->drive_sectors));

	return TEST_OK;
}

/* Test if FirstUsableLBA and LastUsableLBA are checked.
 * FirstUsableLBA must be after the end of the primary GPT table array.
 * LastUsableLBA must be before the start of the secondary GPT table array.
 * FirstUsableLBA <= LastUsableLBA. */
static int FirstUsableLbaAndLastUsableLbaTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptHeader *h2 = (GptHeader *)gpt->secondary_header;
	int i;

	struct {
		uint64_t primary_entries_lba;
		uint64_t primary_first_usable_lba;
		uint64_t primary_last_usable_lba;
		uint64_t secondary_first_usable_lba;
		uint64_t secondary_last_usable_lba;
		uint64_t secondary_entries_lba;
		int primary_rv;
		int secondary_rv;
	} cases[] = {
		{2,  34, 433,   34, 433, 434,  0, 0},
		{2,  34, 432,   34, 430, 434,  0, 0},
		{2,  33, 433,   33, 433, 434,  1, 1},
		{2,  34, 434,   34, 433, 434,  1, 0},
		{2,  34, 433,   34, 434, 434,  0, 1},
		{2,  35, 433,   35, 433, 434,  0, 0},
		{2, 433, 433,  433, 433, 434,  0, 0},
		{2, 434, 433,  434, 434, 434,  1, 1},
		{2, 433,  34,   34, 433, 434,  1, 0},
		{2,  34, 433,  433,  34, 434,  0, 1},
	};

	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestGptData(gpt);
		h1->entries_lba = cases[i].primary_entries_lba;
		h1->first_usable_lba = cases[i].primary_first_usable_lba;
		h1->last_usable_lba = cases[i].primary_last_usable_lba;
		h2->entries_lba = cases[i].secondary_entries_lba;
		h2->first_usable_lba = cases[i].secondary_first_usable_lba;
		h2->last_usable_lba = cases[i].secondary_last_usable_lba;
		RefreshCrc32(gpt);

		EXPECT(CheckHeader(h1, 0, gpt->drive_sectors) ==
		       cases[i].primary_rv);
		EXPECT(CheckHeader(h2, 1, gpt->drive_sectors) ==
		       cases[i].secondary_rv);
	}

	return TEST_OK;
}

/*
 * Test if PartitionEntryArrayCRC32 is checked.  PartitionEntryArrayCRC32 must
 * be calculated over SizeOfPartitionEntry * NumberOfPartitionEntries bytes.
 */
static int EntriesCrcTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptEntry *e1 = (GptEntry *)(gpt->primary_entries);
	GptEntry *e2 = (GptEntry *)(gpt->secondary_entries);

	/* Modify first byte of primary entries, and expect the CRC is wrong. */
	BuildTestGptData(gpt);
	EXPECT(0 == CheckEntries(e1, h1));
	EXPECT(0 == CheckEntries(e2, h1));
	gpt->primary_entries[0] ^= 0xa5;  /* just XOR a non-zero value */
	gpt->secondary_entries[TOTAL_ENTRIES_SIZE-1] ^= 0x5a;
	EXPECT(GPT_ERROR_CRC_CORRUPTED == CheckEntries(e1, h1));
	EXPECT(GPT_ERROR_CRC_CORRUPTED == CheckEntries(e2, h1));

	return TEST_OK;
}

/*
 * Test if partition geometry is checked.
 * All active (non-zero PartitionTypeGUID) partition entries should have:
 *   entry.StartingLBA >= header.FirstUsableLBA
 *   entry.EndingLBA <= header.LastUsableLBA
 *   entry.StartingLBA <= entry.EndingLBA
 */
static int ValidEntryTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptEntry *e1 = (GptEntry *)(gpt->primary_entries);
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskLayout *mh = &mtd->primary;
	MtdDiskPartition *me = mh->partitions;

	/* error case: entry.StartingLBA < header.FirstUsableLBA */
	BuildTestGptData(gpt);
	e1[0].starting_lba = h1->first_usable_lba - 1;
	RefreshCrc32(gpt);
	EXPECT(GPT_ERROR_OUT_OF_REGION == CheckEntries(e1, h1));

	BuildTestMtdData(mtd);
	if (mh->first_offset > 0) {
		me[0].starting_offset = mh->first_offset - 1;
		mh->crc32 = MtdHeaderCrc(mh);
		EXPECT(GPT_ERROR_OUT_OF_REGION == MtdCheckEntries(me, mh));
	}

	/* error case: entry.EndingLBA > header.LastUsableLBA */
	BuildTestGptData(gpt);
	e1[2].ending_lba = h1->last_usable_lba + 1;
	RefreshCrc32(gpt);
	EXPECT(GPT_ERROR_OUT_OF_REGION == CheckEntries(e1, h1));

	BuildTestMtdData(mtd);
	me[0].ending_offset = mh->last_offset + 1;
	mh->crc32 = MtdHeaderCrc(mh);
	EXPECT(GPT_ERROR_OUT_OF_REGION == MtdCheckEntries(me, mh));

	/* error case: entry.StartingLBA > entry.EndingLBA */
	BuildTestGptData(gpt);
	e1[3].starting_lba = e1[3].ending_lba + 1;
	RefreshCrc32(gpt);
	EXPECT(GPT_ERROR_OUT_OF_REGION == CheckEntries(e1, h1));

	BuildTestMtdData(mtd);
	me[0].starting_offset = me[0].ending_offset + 1;
	mh->crc32 = MtdHeaderCrc(mh);
	EXPECT(GPT_ERROR_OUT_OF_REGION == MtdCheckEntries(me, mh));

	/* case: non active entry should be ignored. */
	BuildTestGptData(gpt);
	Memset(&e1[1].type, 0, sizeof(e1[1].type));
	e1[1].starting_lba = e1[1].ending_lba + 1;
	RefreshCrc32(gpt);
	EXPECT(0 == CheckEntries(e1, h1));

	BuildTestMtdData(mtd);
	me[0].flags = 0;
	me[0].starting_offset = me[0].ending_offset + 1;
	mh->crc32 = MtdHeaderCrc(mh);
	EXPECT(GPT_SUCCESS == MtdCheckEntries(me, mh));

	return TEST_OK;
}

/* Test if overlapped partition tables can be detected. */
static int OverlappedPartitionTest(void) {
	GptData *gpt = GetEmptyGptData();
	GptHeader *h = (GptHeader *)gpt->primary_header;
	GptEntry *e = (GptEntry *)gpt->primary_entries;
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskLayout *mh = &mtd->primary;
	MtdDiskPartition *me = mh->partitions;
	int i, j;

	struct {
		int overlapped;
		struct {
			int active;
			uint64_t starting_lba;
			uint64_t ending_lba;
		} entries[16];  /* enough for testing. */
	} cases[] = {
		{GPT_SUCCESS, {{0, 100, 199}}},
		{GPT_SUCCESS, {{1, 100, 199}}},
		{GPT_SUCCESS, {{1, 100, 150}, {1, 200, 250}, {1, 300, 350}}},
		{GPT_ERROR_START_LBA_OVERLAP,
		 {{1, 200, 299}, {1, 100, 199}, {1, 100, 100}}},
		{GPT_ERROR_END_LBA_OVERLAP,
		 {{1, 200, 299}, {1, 100, 199}, {1, 299, 299}}},
		{GPT_SUCCESS, {{1, 300, 399}, {1, 200, 299}, {1, 100, 199}}},
		{GPT_ERROR_END_LBA_OVERLAP,
		 {{1, 100, 199}, {1, 199, 299}, {1, 299, 399}}},
		{GPT_ERROR_START_LBA_OVERLAP,
		 {{1, 100, 199}, {1, 200, 299}, {1, 75, 399}}},
		{GPT_ERROR_START_LBA_OVERLAP,
		 {{1, 100, 199}, {1, 75, 250}, {1, 200, 299}}},
		{GPT_ERROR_END_LBA_OVERLAP,
		 {{1, 75, 150}, {1, 100, 199}, {1, 200, 299}}},
		{GPT_ERROR_START_LBA_OVERLAP,
		 {{1, 200, 299}, {1, 100, 199}, {1, 300, 399}, {1, 100, 399}}},
		{GPT_SUCCESS,
		 {{1, 200, 299}, {1, 100, 199}, {1, 300, 399}, {0, 100, 399}}},
		{GPT_ERROR_START_LBA_OVERLAP,
		 {{1, 200, 300}, {1, 100, 200}, {1, 100, 400}, {1, 300, 400}}},
		{GPT_ERROR_START_LBA_OVERLAP,
		 {{0, 200, 300}, {1, 100, 200}, {1, 100, 400}, {1, 300, 400}}},
		{GPT_SUCCESS,
		 {{1, 200, 300}, {1, 100, 199}, {0, 100, 400}, {0, 300, 400}}},
		{GPT_ERROR_END_LBA_OVERLAP,
		 {{1, 200, 299}, {1, 100, 199}, {1, 199, 199}}},
		{GPT_SUCCESS, {{1, 200, 299}, {0, 100, 199}, {1, 199, 199}}},
		{GPT_SUCCESS, {{1, 200, 299}, {1, 100, 199}, {0, 199, 199}}},
		{GPT_ERROR_START_LBA_OVERLAP,
		 {{1, 199, 199}, {1, 200, 200}, {1, 201, 201}, {1, 202, 202},
		  {1, 203, 203}, {1, 204, 204}, {1, 205, 205}, {1, 206, 206},
		  {1, 207, 207}, {1, 208, 208}, {1, 199, 199}}},
		{GPT_SUCCESS,
		 {{1, 199, 199}, {1, 200, 200}, {1, 201, 201}, {1, 202, 202},
		  {1, 203, 203}, {1, 204, 204}, {1, 205, 205}, {1, 206, 206},
		  {1, 207, 207}, {1, 208, 208}, {0, 199, 199}}},
	};

	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestGptData(gpt);
		BuildTestMtdData(mtd);
		Memset(mh->partitions, 0, sizeof(mh->partitions));
		ZeroEntries(gpt);
		for(j = 0; j < ARRAY_SIZE(cases[0].entries); ++j) {
			if (!cases[i].entries[j].starting_lba)
				break;

			if (cases[i].entries[j].active) {
				Memcpy(&e[j].type, &guid_kernel, sizeof(Guid));
				me[j].flags =
					MTD_PARTITION_TYPE_CHROMEOS_KERNEL << MTD_ATTRIBUTE_TYPE_OFFSET;
			}
			SetGuid(&e[j].unique, j);
			e[j].starting_lba = cases[i].entries[j].starting_lba;
			e[j].ending_lba = cases[i].entries[j].ending_lba;
			me[j].starting_offset = cases[i].entries[j].starting_lba *
			    DEFAULT_SECTOR_SIZE;
			me[j].ending_offset = cases[i].entries[j].ending_lba *
			    DEFAULT_SECTOR_SIZE;

		}
		RefreshCrc32(gpt);

		EXPECT(cases[i].overlapped == CheckEntries(e, h));
		EXPECT(cases[i].overlapped == MtdCheckEntries(me, mh));
	}
	return TEST_OK;
}

/* Test both sanity checking and repair. */
static int SanityCheckTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h1 = (GptHeader *)gpt->primary_header;
	GptEntry *e1 = (GptEntry *)gpt->primary_entries;
	uint8_t *tempptr;

	/* Unmodified test data is completely sane */
	BuildTestGptData(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	/* Repair doesn't damage it */
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT(0 == gpt->modified);

	/* Invalid sector size should fail */
	BuildTestGptData(gpt);
	gpt->sector_bytes = 1024;
	EXPECT(GPT_ERROR_INVALID_SECTOR_SIZE == GptSanityCheck(gpt));

	/* Modify headers */
	BuildTestGptData(gpt);
	gpt->primary_header[0]++;
	gpt->secondary_header[0]++;
	EXPECT(GPT_ERROR_INVALID_HEADERS == GptSanityCheck(gpt));
	EXPECT(0 == gpt->valid_headers);
	EXPECT(0 == gpt->valid_entries);
	/* Repair can't fix completely busted headers */
	GptRepair(gpt);
	EXPECT(GPT_ERROR_INVALID_HEADERS == GptSanityCheck(gpt));
	EXPECT(0 == gpt->valid_headers);
	EXPECT(0 == gpt->valid_entries);
	EXPECT(0 == gpt->modified);

	BuildTestGptData(gpt);
	gpt->primary_header[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_SECONDARY == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT(GPT_MODIFIED_HEADER1 == gpt->modified);

	BuildTestGptData(gpt);
	gpt->secondary_header[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_PRIMARY == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT(GPT_MODIFIED_HEADER2 == gpt->modified);

	/*
	 * Modify header1 and update its CRC.  Since header2 is now different
	 * than header1, it'll be the one considered invalid.
	 */
	BuildTestGptData(gpt);
	h1->size++;
	RefreshCrc32(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_PRIMARY == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT(GPT_MODIFIED_HEADER2 == gpt->modified);

	/* Modify entries */
	BuildTestGptData(gpt);
	gpt->primary_entries[0]++;
	gpt->secondary_entries[0]++;
	EXPECT(GPT_ERROR_INVALID_ENTRIES == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_NONE == gpt->valid_entries);
	/* Repair can't fix both copies of entries being bad, either. */
	GptRepair(gpt);
	EXPECT(GPT_ERROR_INVALID_ENTRIES == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_NONE == gpt->valid_entries);
	EXPECT(0 == gpt->modified);

	BuildTestGptData(gpt);
	gpt->primary_entries[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_SECONDARY == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT(GPT_MODIFIED_ENTRIES1 == gpt->modified);

	BuildTestGptData(gpt);
	gpt->secondary_entries[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_PRIMARY == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT(GPT_MODIFIED_ENTRIES2 == gpt->modified);

	/*
	 * Modify entries and recompute CRCs, then make both primary and
	 * secondary entry pointers use the secondary data.  The primary
	 * header will have the wrong entries CRC, so we should fall back
	 * to the secondary header.
	 */
	BuildTestGptData(gpt);
	e1->starting_lba++;
	RefreshCrc32(gpt);
	tempptr = gpt->primary_entries;
	gpt->primary_entries = gpt->secondary_entries;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_SECONDARY == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	gpt->primary_entries = tempptr;

	/* Modify both header and entries */
	BuildTestGptData(gpt);
	gpt->primary_header[0]++;
	gpt->primary_entries[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_SECONDARY == gpt->valid_headers);
	EXPECT(MASK_SECONDARY == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT((GPT_MODIFIED_HEADER1 | GPT_MODIFIED_ENTRIES1) == gpt->modified);

	BuildTestGptData(gpt);
	gpt->secondary_header[0]++;
	gpt->secondary_entries[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_PRIMARY == gpt->valid_headers);
	EXPECT(MASK_PRIMARY == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT((GPT_MODIFIED_HEADER2 | GPT_MODIFIED_ENTRIES2) == gpt->modified);

	/* Test cross-correction (h1+e2, h2+e1) */
	BuildTestGptData(gpt);
	gpt->primary_header[0]++;
	gpt->secondary_entries[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_SECONDARY == gpt->valid_headers);
	EXPECT(MASK_PRIMARY == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT((GPT_MODIFIED_HEADER1 | GPT_MODIFIED_ENTRIES2) == gpt->modified);

	BuildTestGptData(gpt);
	gpt->secondary_header[0]++;
	gpt->primary_entries[0]++;
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_PRIMARY == gpt->valid_headers);
	EXPECT(MASK_SECONDARY == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT((GPT_MODIFIED_HEADER2 | GPT_MODIFIED_ENTRIES1) == gpt->modified);

	/*
	 * Test mismatched pairs (h1+e1 valid, h2+e2 valid but different.  This
	 * simulates a partial update of the drive.
	 */
	BuildTestGptData(gpt);
	gpt->secondary_entries[0]++;
	RefreshCrc32(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_PRIMARY == gpt->valid_headers);
	EXPECT(MASK_PRIMARY == gpt->valid_entries);
	GptRepair(gpt);
	EXPECT(GPT_SUCCESS == GptSanityCheck(gpt));
	EXPECT(MASK_BOTH == gpt->valid_headers);
	EXPECT(MASK_BOTH == gpt->valid_entries);
	EXPECT((GPT_MODIFIED_HEADER2 | GPT_MODIFIED_ENTRIES2) == gpt->modified);

	return TEST_OK;
}

static int EntryAttributeGetSetTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e = (GptEntry *)(gpt->primary_entries);
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskPartition *m = &mtd->primary.partitions[0];

	e->attrs.whole = 0x0000000000000000ULL;
	SetEntrySuccessful(e, 1);
	EXPECT(0x0100000000000000ULL == e->attrs.whole);
	EXPECT(1 == GetEntrySuccessful(e));
	e->attrs.whole = 0xFFFFFFFFFFFFFFFFULL;
	SetEntrySuccessful(e, 0);
	EXPECT(0xFEFFFFFFFFFFFFFFULL == e->attrs.whole);
	EXPECT(0 == GetEntrySuccessful(e));

	m->flags = 0;
	MtdSetEntrySuccessful(m, 1);
	EXPECT(0x00000100 == m->flags);
	EXPECT(1 == MtdGetEntrySuccessful(m));
	m->flags = ~0;
	MtdSetEntrySuccessful(m, 0);
	EXPECT(0xFFFFFEFF == m->flags);
	EXPECT(0 == MtdGetEntrySuccessful(m));

	e->attrs.whole = 0x0000000000000000ULL;
	SetEntryTries(e, 15);
	EXPECT(15 == GetEntryTries(e));
	EXPECT(0x00F0000000000000ULL == e->attrs.whole);
	e->attrs.whole = 0xFFFFFFFFFFFFFFFFULL;
	SetEntryTries(e, 0);
	EXPECT(0xFF0FFFFFFFFFFFFFULL == e->attrs.whole);
	EXPECT(0 == GetEntryTries(e));

	m->flags = 0;
	MtdSetEntryTries(m, 15);
	EXPECT(0x000000F0 == m->flags);
	EXPECT(15 == MtdGetEntryTries(m));
	m->flags = ~0;
	MtdSetEntryTries(m, 0);
	EXPECT(0xFFFFFF0F == m->flags);
	EXPECT(0 == MtdGetEntryTries(m));

	e->attrs.whole = 0x0000000000000000ULL;
	SetEntryPriority(e, 15);
	EXPECT(0x000F000000000000ULL == e->attrs.whole);
	EXPECT(15 == GetEntryPriority(e));
	e->attrs.whole = 0xFFFFFFFFFFFFFFFFULL;
	SetEntryPriority(e, 0);
	EXPECT(0xFFF0FFFFFFFFFFFFULL == e->attrs.whole);
	EXPECT(0 == GetEntryPriority(e));

	m->flags = 0;
	MtdSetEntryPriority(m, 15);
	EXPECT(0x0000000F == m->flags);
	EXPECT(15 == MtdGetEntryPriority(m));
	m->flags = ~0;
	MtdSetEntryPriority(m, 0);
	EXPECT(0xFFFFFFF0 == m->flags);
	EXPECT(0 == MtdGetEntryPriority(m));

	e->attrs.whole = 0xFFFFFFFFFFFFFFFFULL;
	EXPECT(1 == GetEntrySuccessful(e));
	EXPECT(15 == GetEntryPriority(e));
	EXPECT(15 == GetEntryTries(e));

	e->attrs.whole = 0x0123000000000000ULL;
	EXPECT(1 == GetEntrySuccessful(e));
	EXPECT(2 == GetEntryTries(e));
	EXPECT(3 == GetEntryPriority(e));

	return TEST_OK;
}

static int EntryTypeTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e = (GptEntry *)(gpt->primary_entries);

	Memcpy(&e->type, &guid_zero, sizeof(Guid));
	EXPECT(1 == IsUnusedEntry(e));
	EXPECT(0 == IsKernelEntry(e));

	Memcpy(&e->type, &guid_kernel, sizeof(Guid));
	EXPECT(0 == IsUnusedEntry(e));
	EXPECT(1 == IsKernelEntry(e));

	Memcpy(&e->type, &guid_rootfs, sizeof(Guid));
	EXPECT(0 == IsUnusedEntry(e));
	EXPECT(0 == IsKernelEntry(e));

	return TEST_OK;
}

/* Make an entry unused by clearing its type. */
static void FreeEntry(GptEntry *e)
{
	Memset(&e->type, 0, sizeof(Guid));
}

static void MtdFreeEntry(MtdDiskPartition *e)
{
	MtdSetEntryType(e, MTD_PARTITION_TYPE_UNUSED);
}

/* Set up an entry. */
static void FillEntry(GptEntry *e, int is_kernel,
                      int priority, int successful, int tries)
{
	Memcpy(&e->type, (is_kernel ? &guid_kernel : &guid_zero), sizeof(Guid));
	SetEntryPriority(e, priority);
	SetEntrySuccessful(e, successful);
	SetEntryTries(e, tries);
}

static void MtdFillEntry(MtdDiskPartition *e, int is_kernel,
                      int priority, int successful, int tries)
{
	MtdSetEntryType(e, is_kernel ? MTD_PARTITION_TYPE_CHROMEOS_KERNEL :
									MTD_PARTITION_TYPE_CHROMEOS_FIRMWARE);
	MtdSetEntryPriority(e, priority);
	MtdSetEntrySuccessful(e, successful);
	MtdSetEntryTries(e, tries);
}

/*
 * Invalidate all kernel entries and expect GptNextKernelEntry() cannot find
 * any usable kernel entry.
 */
static int NoValidKernelEntryTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e1 = (GptEntry *)(gpt->primary_entries);

	BuildTestGptData(gpt);
	SetEntryPriority(e1 + KERNEL_A, 0);
	FreeEntry(e1 + KERNEL_B);
	RefreshCrc32(gpt);
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       GptNextKernelEntry(gpt, NULL, NULL));

	return TEST_OK;
}

static int MtdNoValidKernelEntryTest(void)
{
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskPartition *e1 = mtd->primary.partitions;

	BuildTestMtdData(mtd);
	MtdSetEntryPriority(e1 + KERNEL_A, 0);
	MtdFreeEntry(e1 + KERNEL_B);
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       MtdNextKernelEntry(mtd, NULL, NULL));

	return TEST_OK;
}

static int GetNextNormalTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e1 = (GptEntry *)(gpt->primary_entries);
	uint64_t start, size;

	/* Normal case - both kernels successful */
	BuildTestGptData(gpt);
	FillEntry(e1 + KERNEL_A, 1, 2, 1, 0);
	FillEntry(e1 + KERNEL_B, 1, 2, 1, 0);
	RefreshCrc32(gpt);
	GptInit(gpt);

	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_A == gpt->current_kernel);
	EXPECT(34 == start);
	EXPECT(100 == size);

	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_B == gpt->current_kernel);
	EXPECT(134 == start);
	EXPECT(99 == size);

	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       GptNextKernelEntry(gpt, &start, &size));
	EXPECT(-1 == gpt->current_kernel);

	/* Call as many times as you want; you won't get another kernel... */
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       GptNextKernelEntry(gpt, &start, &size));
	EXPECT(-1 == gpt->current_kernel);

	return TEST_OK;
}

static int GetNextPrioTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e1 = (GptEntry *)(gpt->primary_entries);
	uint64_t start, size;

	/* Priority 3, 4, 0, 4 - should boot order B, Y, A */
	BuildTestGptData(gpt);
	FillEntry(e1 + KERNEL_A, 1, 3, 1, 0);
	FillEntry(e1 + KERNEL_B, 1, 4, 1, 0);
	FillEntry(e1 + KERNEL_X, 1, 0, 1, 0);
	FillEntry(e1 + KERNEL_Y, 1, 4, 1, 0);
	RefreshCrc32(gpt);
	GptInit(gpt);

	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_B == gpt->current_kernel);
	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_Y == gpt->current_kernel);
	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_A == gpt->current_kernel);
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       GptNextKernelEntry(gpt, &start, &size));

	return TEST_OK;
}

static int GetNextTriesTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e1 = (GptEntry *)(gpt->primary_entries);
	uint64_t start, size;

	/* Tries=nonzero is attempted just like success, but tries=0 isn't */
	BuildTestGptData(gpt);
	FillEntry(e1 + KERNEL_A, 1, 2, 1, 0);
	FillEntry(e1 + KERNEL_B, 1, 3, 0, 0);
	FillEntry(e1 + KERNEL_X, 1, 4, 0, 1);
	FillEntry(e1 + KERNEL_Y, 1, 0, 0, 5);
	RefreshCrc32(gpt);
	GptInit(gpt);

	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_X == gpt->current_kernel);
	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_A == gpt->current_kernel);
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       GptNextKernelEntry(gpt, &start, &size));

	return TEST_OK;
}

static int MtdGetNextNormalTest(void)
{
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskPartition *e1 = mtd->primary.partitions;
	uint64_t start, size;

	/* Normal case - both kernels successful */
	BuildTestMtdData(mtd);
	MtdFillEntry(e1 + KERNEL_A, 1, 2, 1, 0);
	MtdFillEntry(e1 + KERNEL_B, 1, 2, 1, 0);
	mtd->primary.crc32 = MtdHeaderCrc(&mtd->primary);
	MtdInit(mtd);

	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_A == mtd->current_kernel);
	EXPECT(34 == start);
	EXPECT(100 == size);

	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_B == mtd->current_kernel);
	EXPECT(134 == start);
	EXPECT(99 == size);

	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(-1 == mtd->current_kernel);

	/* Call as many times as you want; you won't get another kernel... */
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(-1 == mtd->current_kernel);

	return TEST_OK;
}

static int MtdGetNextPrioTest(void)
{
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskPartition *e1 = mtd->primary.partitions;
	uint64_t start, size;

	/* Priority 3, 4, 0, 4 - should boot order B, Y, A */
	BuildTestMtdData(mtd);
	MtdFillEntry(e1 + KERNEL_A, 1, 3, 1, 0);
	MtdFillEntry(e1 + KERNEL_B, 1, 4, 1, 0);
	MtdFillEntry(e1 + KERNEL_X, 1, 0, 1, 0);
	MtdFillEntry(e1 + KERNEL_Y, 1, 4, 1, 0);
	mtd->primary.crc32 = MtdHeaderCrc(&mtd->primary);
	MtdInit(mtd);

	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_B == mtd->current_kernel);
	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_Y == mtd->current_kernel);
	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_A == mtd->current_kernel);
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       MtdNextKernelEntry(mtd, &start, &size));

	return TEST_OK;
}

static int MtdGetNextTriesTest(void)
{
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskPartition *e1 = mtd->primary.partitions;
	uint64_t start, size;

	/* Tries=nonzero is attempted just like success, but tries=0 isn't */
	BuildTestMtdData(mtd);
	MtdFillEntry(e1 + KERNEL_A, 1, 2, 1, 0);
	MtdFillEntry(e1 + KERNEL_B, 1, 3, 0, 0);
	MtdFillEntry(e1 + KERNEL_X, 1, 4, 0, 1);
	MtdFillEntry(e1 + KERNEL_Y, 1, 0, 0, 5);
	mtd->primary.crc32 = MtdHeaderCrc(&mtd->primary);
	MtdInit(mtd);

	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_X == mtd->current_kernel);
	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_A == mtd->current_kernel);
	EXPECT(GPT_ERROR_NO_VALID_KERNEL ==
	       MtdNextKernelEntry(mtd, &start, &size));

	return TEST_OK;
}

static int MtdUpdateTest() {
	MtdData *mtd = GetEmptyMtdData();
	MtdDiskPartition *e = &mtd->primary.partitions[0];
	uint64_t start, size;

	BuildTestMtdData(mtd);

	/* Tries=nonzero is attempted just like success, but tries=0 isn't */
	MtdFillEntry(e + KERNEL_A, 1, 4, 1, 0);
	MtdFillEntry(e + KERNEL_B, 1, 3, 0, 2);
	MtdFillEntry(e + KERNEL_X, 1, 2, 0, 2);
	mtd->primary.crc32 = MtdHeaderCrc(&mtd->primary);
	mtd->modified = 0;
	EXPECT(GPT_SUCCESS == MtdInit(mtd));

	/* Successful kernel */
	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_A == mtd->current_kernel);
	EXPECT(1 == MtdGetEntrySuccessful(e + KERNEL_A));
	EXPECT(4 == MtdGetEntryPriority(e + KERNEL_A));
	EXPECT(0 == MtdGetEntryTries(e + KERNEL_A));
	/* Trying successful kernel changes nothing */
	EXPECT(GPT_SUCCESS == MtdUpdateKernelEntry(mtd, GPT_UPDATE_ENTRY_TRY));
	EXPECT(1 == MtdGetEntrySuccessful(e + KERNEL_A));
	EXPECT(4 == MtdGetEntryPriority(e + KERNEL_A));
	EXPECT(0 == MtdGetEntryTries(e + KERNEL_A));
	EXPECT(0 == mtd->modified);
	/* Marking it bad also does not update it. */
	EXPECT(GPT_SUCCESS == MtdUpdateKernelEntry(mtd, GPT_UPDATE_ENTRY_BAD));
	EXPECT(1 == MtdGetEntrySuccessful(e + KERNEL_A));
	EXPECT(4 == MtdGetEntryPriority(e + KERNEL_A));
	EXPECT(0 == MtdGetEntryTries(e + KERNEL_A));
	EXPECT(0 == mtd->modified);

	/* Kernel with tries */
	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_B == mtd->current_kernel);
	EXPECT(0 == MtdGetEntrySuccessful(e + KERNEL_B));
	EXPECT(3 == MtdGetEntryPriority(e + KERNEL_B));
	EXPECT(2 == MtdGetEntryTries(e + KERNEL_B));
	/* Marking it bad clears it */
	EXPECT(GPT_SUCCESS == MtdUpdateKernelEntry(mtd, GPT_UPDATE_ENTRY_BAD));
	EXPECT(0 == MtdGetEntrySuccessful(e + KERNEL_B));
	EXPECT(0 == MtdGetEntryPriority(e + KERNEL_B));
	EXPECT(0 == MtdGetEntryTries(e + KERNEL_B));
	/* And that's caused the mtd to need updating */
	EXPECT(1 == mtd->modified);

	/* Another kernel with tries */
	EXPECT(GPT_SUCCESS == MtdNextKernelEntry(mtd, &start, &size));
	EXPECT(KERNEL_X == mtd->current_kernel);
	EXPECT(0 == MtdGetEntrySuccessful(e + KERNEL_X));
	EXPECT(2 == MtdGetEntryPriority(e + KERNEL_X));
	EXPECT(2 == MtdGetEntryTries(e + KERNEL_X));
	/* Trying it uses up a try */
	EXPECT(GPT_SUCCESS == MtdUpdateKernelEntry(mtd, GPT_UPDATE_ENTRY_TRY));
	EXPECT(0 == MtdGetEntrySuccessful(e + KERNEL_X));
	EXPECT(2 == MtdGetEntryPriority(e + KERNEL_X));
	EXPECT(1 == MtdGetEntryTries(e + KERNEL_X));
	/* Trying it again marks it inactive */
	EXPECT(GPT_SUCCESS == MtdUpdateKernelEntry(mtd, GPT_UPDATE_ENTRY_TRY));
	EXPECT(0 == MtdGetEntrySuccessful(e + KERNEL_X));
	EXPECT(0 == MtdGetEntryPriority(e + KERNEL_X));
	EXPECT(0 == MtdGetEntryTries(e + KERNEL_X));

	/* Can't update if entry isn't a kernel, or there isn't an entry */
	MtdSetEntryType(e + KERNEL_X, MTD_PARTITION_TYPE_UNUSED);
	EXPECT(GPT_ERROR_INVALID_UPDATE_TYPE ==
	       MtdUpdateKernelEntry(mtd, GPT_UPDATE_ENTRY_BAD));
	mtd->current_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;
	EXPECT(GPT_ERROR_INVALID_UPDATE_TYPE ==
	       MtdUpdateKernelEntry(mtd, GPT_UPDATE_ENTRY_BAD));

	return TEST_OK;
}

static int GptUpdateTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e = (GptEntry *)(gpt->primary_entries);
	GptEntry *e2 = (GptEntry *)(gpt->secondary_entries);
	uint64_t start, size;

	/* Tries=nonzero is attempted just like success, but tries=0 isn't */
	BuildTestGptData(gpt);
	FillEntry(e + KERNEL_A, 1, 4, 1, 0);
	FillEntry(e + KERNEL_B, 1, 3, 0, 2);
	FillEntry(e + KERNEL_X, 1, 2, 0, 2);
	RefreshCrc32(gpt);
	GptInit(gpt);
	gpt->modified = 0;  /* Nothing modified yet */

	/* Successful kernel */
	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_A == gpt->current_kernel);
	EXPECT(1 == GetEntrySuccessful(e + KERNEL_A));
	EXPECT(4 == GetEntryPriority(e + KERNEL_A));
	EXPECT(0 == GetEntryTries(e + KERNEL_A));
	EXPECT(1 == GetEntrySuccessful(e2 + KERNEL_A));
	EXPECT(4 == GetEntryPriority(e2 + KERNEL_A));
	EXPECT(0 == GetEntryTries(e2 + KERNEL_A));
	/* Trying successful kernel changes nothing */
	EXPECT(GPT_SUCCESS == GptUpdateKernelEntry(gpt, GPT_UPDATE_ENTRY_TRY));
	EXPECT(1 == GetEntrySuccessful(e + KERNEL_A));
	EXPECT(4 == GetEntryPriority(e + KERNEL_A));
	EXPECT(0 == GetEntryTries(e + KERNEL_A));
	EXPECT(0 == gpt->modified);
	/* Marking it bad also does not update it. */
	EXPECT(GPT_SUCCESS == GptUpdateKernelEntry(gpt, GPT_UPDATE_ENTRY_BAD));
	EXPECT(1 == GetEntrySuccessful(e + KERNEL_A));
	EXPECT(4 == GetEntryPriority(e + KERNEL_A));
	EXPECT(0 == GetEntryTries(e + KERNEL_A));
	EXPECT(0 == gpt->modified);

	/* Kernel with tries */
	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_B == gpt->current_kernel);
	EXPECT(0 == GetEntrySuccessful(e + KERNEL_B));
	EXPECT(3 == GetEntryPriority(e + KERNEL_B));
	EXPECT(2 == GetEntryTries(e + KERNEL_B));
	/* Marking it bad clears it */
	EXPECT(GPT_SUCCESS == GptUpdateKernelEntry(gpt, GPT_UPDATE_ENTRY_BAD));
	EXPECT(0 == GetEntrySuccessful(e + KERNEL_B));
	EXPECT(0 == GetEntryPriority(e + KERNEL_B));
	EXPECT(0 == GetEntryTries(e + KERNEL_B));
	/* Which affects both copies of the partition entries */
	EXPECT(0 == GetEntrySuccessful(e2 + KERNEL_B));
	EXPECT(0 == GetEntryPriority(e2 + KERNEL_B));
	EXPECT(0 == GetEntryTries(e2 + KERNEL_B));
	/* And that's caused the GPT to need updating */
	EXPECT(0x0F == gpt->modified);

	/* Another kernel with tries */
	EXPECT(GPT_SUCCESS == GptNextKernelEntry(gpt, &start, &size));
	EXPECT(KERNEL_X == gpt->current_kernel);
	EXPECT(0 == GetEntrySuccessful(e + KERNEL_X));
	EXPECT(2 == GetEntryPriority(e + KERNEL_X));
	EXPECT(2 == GetEntryTries(e + KERNEL_X));
	/* Trying it uses up a try */
	EXPECT(GPT_SUCCESS == GptUpdateKernelEntry(gpt, GPT_UPDATE_ENTRY_TRY));
	EXPECT(0 == GetEntrySuccessful(e + KERNEL_X));
	EXPECT(2 == GetEntryPriority(e + KERNEL_X));
	EXPECT(1 == GetEntryTries(e + KERNEL_X));
	EXPECT(0 == GetEntrySuccessful(e2 + KERNEL_X));
	EXPECT(2 == GetEntryPriority(e2 + KERNEL_X));
	EXPECT(1 == GetEntryTries(e2 + KERNEL_X));
	/* Trying it again marks it inactive */
	EXPECT(GPT_SUCCESS == GptUpdateKernelEntry(gpt, GPT_UPDATE_ENTRY_TRY));
	EXPECT(0 == GetEntrySuccessful(e + KERNEL_X));
	EXPECT(0 == GetEntryPriority(e + KERNEL_X));
	EXPECT(0 == GetEntryTries(e + KERNEL_X));

	/* Can't update if entry isn't a kernel, or there isn't an entry */
	Memcpy(&e[KERNEL_X].type, &guid_rootfs, sizeof(guid_rootfs));
	EXPECT(GPT_ERROR_INVALID_UPDATE_TYPE ==
	       GptUpdateKernelEntry(gpt, GPT_UPDATE_ENTRY_BAD));
	gpt->current_kernel = CGPT_KERNEL_ENTRY_NOT_FOUND;
	EXPECT(GPT_ERROR_INVALID_UPDATE_TYPE ==
	       GptUpdateKernelEntry(gpt, GPT_UPDATE_ENTRY_BAD));


	return TEST_OK;
}

/*
 * Give an invalid kernel type, and expect GptUpdateKernelEntry() returns
 * GPT_ERROR_INVALID_UPDATE_TYPE.
 */
static int UpdateInvalidKernelTypeTest(void)
{
	GptData *gpt = GetEmptyGptData();

	BuildTestGptData(gpt);
	/* anything, but not CGPT_KERNEL_ENTRY_NOT_FOUND */
	gpt->current_kernel = 0;
	/* any invalid update_type value */
	EXPECT(GPT_ERROR_INVALID_UPDATE_TYPE ==
	       GptUpdateKernelEntry(gpt, 99));

	return TEST_OK;
}

static int MtdUpdateInvalidKernelTypeTest(void)
{
	MtdData *mtd = GetEmptyMtdData();

	BuildTestMtdData(mtd);
	/* anything, but not CGPT_KERNEL_ENTRY_NOT_FOUND */
	mtd->current_kernel = 0;
	/* any invalid update_type value */
	EXPECT(GPT_ERROR_INVALID_UPDATE_TYPE ==
	       MtdUpdateKernelEntry(mtd, 99));

	return TEST_OK;
}

/* Test duplicate UniqueGuids can be detected. */
static int DuplicateUniqueGuidTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptHeader *h = (GptHeader *)gpt->primary_header;
	GptEntry *e = (GptEntry *)gpt->primary_entries;
	int i, j;

	struct {
		int duplicate;
		struct {
			uint64_t starting_lba;
			uint64_t ending_lba;
			uint32_t type_guid;
			uint32_t unique_guid;
		} entries[16];   /* enough for testing. */
	} cases[] = {
		{GPT_SUCCESS, {{100, 109, 1, 1},
			       {110, 119, 2, 2},
			       {120, 129, 3, 3},
			       {130, 139, 4, 4},
			}},
		{GPT_SUCCESS, {{100, 109, 1, 1},
			       {110, 119, 1, 2},
			       {120, 129, 2, 3},
			       {130, 139, 2, 4},
			}},
		{GPT_ERROR_DUP_GUID, {{100, 109, 1, 1},
				      {110, 119, 2, 2},
				      {120, 129, 3, 1},
				      {130, 139, 4, 4},
			}},
		{GPT_ERROR_DUP_GUID, {{100, 109, 1, 1},
				      {110, 119, 1, 2},
				      {120, 129, 2, 3},
				      {130, 139, 2, 2},
			}},
	};

	for (i = 0; i < ARRAY_SIZE(cases); ++i) {
		BuildTestGptData(gpt);
		ZeroEntries(gpt);
		for(j = 0; j < ARRAY_SIZE(cases[0].entries); ++j) {
			if (!cases[i].entries[j].starting_lba)
				break;

			e[j].starting_lba = cases[i].entries[j].starting_lba;
			e[j].ending_lba = cases[i].entries[j].ending_lba;
			SetGuid(&e[j].type, cases[i].entries[j].type_guid);
			SetGuid(&e[j].unique, cases[i].entries[j].unique_guid);
		}
		RefreshCrc32(gpt);

		EXPECT(cases[i].duplicate == CheckEntries(e, h));
	}

	return TEST_OK;
}

/* Test getting the current kernel GUID */
static int GetKernelGuidTest(void)
{
	GptData *gpt = GetEmptyGptData();
	GptEntry *e = (GptEntry *)gpt->primary_entries;
	Guid g;

	BuildTestGptData(gpt);
	gpt->current_kernel = 0;
	GetCurrentKernelUniqueGuid(gpt, &g);
	EXPECT(!Memcmp(&g, &e[0].unique, sizeof(Guid)));
	gpt->current_kernel = 1;
	GetCurrentKernelUniqueGuid(gpt, &g);
	EXPECT(!Memcmp(&g, &e[1].unique, sizeof(Guid)));

	return TEST_OK;
}

/* Test getting GPT error text strings */
static int ErrorTextTest(void)
{
	int i;

	/* Known errors are not unknown */
	for (i = 0; i < GPT_ERROR_COUNT; i++) {
		EXPECT(GptErrorText(i));
		EXPECT(strcmp(GptErrorText(i), "Unknown"));
	}

	/* But other error values are */
	EXPECT(!strcmp(GptErrorText(GPT_ERROR_COUNT), "Unknown"));

	return TEST_OK;
}

int nand_read_page(const nand_geom *nand, int page, void *buf, int size) {
  uint32_t ofs = page * nand->szofpg;
  uint32_t sz = size;
  if (ofs + sz > nand_drive_sz) {
    return -1;
  }
  Memcpy(buf, nand_drive + ofs, sz);
  return 0;
}

int nand_write_page(const nand_geom *nand, int page,
                    const void *buf, int size) {
  uint32_t ofs = page * nand->szofpg;
  uint32_t sz = size;
  uint32_t i;
  if (ofs + sz > nand_drive_sz) {
    return -1;
  }
  for (i = 0; i < sz; i++) {
    if (nand_drive[ofs + i] != 0xff) {
      return -1;
    }
  }
  Memcpy(nand_drive + ofs, buf, sz);
  return 0;
}

int nand_erase_block(const nand_geom *nand, int block) {
  uint32_t ofs = block * nand->szofblk;
  uint32_t sz = nand->szofblk;
  if (ofs + sz > nand_drive_sz) {
    return -1;
  }
  if (!--nand_bad_block_map[block]) {
    return -1;
  }
  Memset(nand_drive + ofs, 0xFF, sz);
  return 0;
}

int nand_is_bad_block(const nand_geom *nand, int block) {
  return nand_bad_block_map[block] == 0;
}


static void nand_make_ramdisk() {
  if (nand_drive) {
    free(nand_drive);
  }
  if (nand_bad_block_map) {
    free(nand_bad_block_map);
  }
  nand_drive_sz = 1024 * 1024 * 16;
  nand_drive = (uint8_t *)malloc(nand_drive_sz);
  nand_bad_block_map = (uint8_t *)malloc(nand_drive_sz / 512);
  Memset(nand_drive, 0xff, nand_drive_sz);
  Memset(nand_bad_block_map, 0xff, nand_drive_sz / 512);
}

static int MtdFtsTest() {
  int MtdLoad(struct drive *drive, int sector_bytes);
  int MtdSave(struct drive *drive);
  int FlashGet(const char *key, uint8_t *data, uint32_t *bufsz);
  int FlashSet(const char *key, const uint8_t *data, uint32_t bufsz);

  int i, j, err;

  struct {
    int result;
    unsigned int offset, size, block_size_bytes, page_size_bytes;
  } cases[] = {
    { 0, 1, 2, 1024 * 1024, 1024 * 4 },
    { 0, 1, 2, 1024 * 1024, 1024 * 16 },

    /* Failure cases, non-power-of-2 */
    { -ENODEV, 1, 2, 5000000, 1024 * 16 },
    { -ENODEV, 1, 2, 1024 * 1024, 65535 },

    /* Page > block */
    { -ENODEV, 1, 2, 1024 * 16, 1024 * 1024 },
  };


  /* Check if the FTS store works */
  for (i = 0; i < ARRAY_SIZE(cases); i++) {
    nand_make_ramdisk();
    EXPECT(cases[i].result == flash_ts_init(cases[i].offset, cases[i].size,
                                            cases[i].page_size_bytes,
                                            cases[i].block_size_bytes, 512, 0));

    if (cases[i].result == 0) {
      /* We should have a working FTS store now */
      char buffer[64];
      uint8_t blob[256], blob_read[256];
      uint32_t sz = sizeof(blob_read);
      struct drive drive;

      /* Test the low level API */
      EXPECT(0 == flash_ts_set("some_key", "some value"));
      flash_ts_get("some_key", buffer, sizeof(buffer));
      EXPECT(0 == strcmp(buffer, "some value"));

      /* Check overwrite */
      EXPECT(0 == flash_ts_set("some_key", "some other value"));
      flash_ts_get("some_key", buffer, sizeof(buffer));
      EXPECT(0 == strcmp(buffer, "some other value"));

      /* Check delete */
      EXPECT(0 == flash_ts_set("some_key", ""));

      /* Verify that re-initialization pulls the right record. */
      flash_ts_init(cases[i].offset, cases[i].size, cases[i].page_size_bytes,
                    cases[i].block_size_bytes, 512, 0);
      flash_ts_get("some_key", buffer, sizeof(buffer));
      EXPECT(0 == strcmp(buffer, ""));

      /* Fill up the disk, eating all erase cycles */
      for (j = 0; j < nand_drive_sz / 512; j++) {
        nand_bad_block_map[j] = 2;
      }
      for (j = 0; j < 999999; j++) {
        char str[32];
        sprintf(str, "%d", j);
        err = flash_ts_set("some_new_key", str);
        if (err) {
          EXPECT(err == -ENOMEM);
          break;
        }

        /* Make sure we can figure out where the latest is. */
        flash_ts_init(cases[i].offset, cases[i].size, cases[i].page_size_bytes,
                      cases[i].block_size_bytes, 512, 0);
        flash_ts_get("some_new_key", buffer, sizeof(buffer));
        EXPECT(0 == strcmp(buffer, str));
      }
      EXPECT(j < 999999);

      /* We need our drive back. */
      nand_make_ramdisk();
      flash_ts_init(cases[i].offset, cases[i].size, cases[i].page_size_bytes,
                    cases[i].block_size_bytes, 512, 0);


      for (j = 0; j < 256; j++) {
        blob[j] = j;
      }

      /* Hex conversion / blob storage */
      EXPECT(0 == FlashSet("some_blob", blob, sizeof(blob)));
      EXPECT(0 == FlashGet("some_blob", blob_read, &sz));
      EXPECT(sz == sizeof(blob_read));
      EXPECT(0 == Memcmp(blob, blob_read, sizeof(blob)));

      BuildTestMtdData(&drive.mtd);
      drive.mtd.flash_block_bytes = cases[i].block_size_bytes;
      drive.mtd.flash_page_bytes = cases[i].page_size_bytes;
      drive.mtd.fts_block_offset = cases[i].offset;
      drive.mtd.fts_block_size = cases[i].size;
      drive.mtd.sector_bytes = 512;
      drive.mtd.drive_sectors = nand_drive_sz / 512;

      /* MTD-level API */
      EXPECT(0 == MtdSave(&drive));
      Memset(&drive.mtd.primary, 0, sizeof(drive.mtd.primary));
      EXPECT(0 == MtdLoad(&drive, 512));
    }
  }

  return TEST_OK;
}

int main(int argc, char *argv[])
{
	int i;
	int error_count = 0;
	struct {
		char *name;
		test_func fp;
		int retval;
	} test_cases[] = {
		{ TEST_CASE(StructSizeTest), },
		{ TEST_CASE(TestBuildTestGptData), },
		{ TEST_CASE(TestBuildTestMtdData), },
		{ TEST_CASE(ParameterTests), },
		{ TEST_CASE(HeaderCrcTest), },
		{ TEST_CASE(HeaderSameTest), },
		{ TEST_CASE(SignatureTest), },
		{ TEST_CASE(RevisionTest), },
		{ TEST_CASE(SizeTest), },
		{ TEST_CASE(CrcFieldTest), },
		{ TEST_CASE(ReservedFieldsTest), },
		{ TEST_CASE(SizeOfPartitionEntryTest), },
		{ TEST_CASE(NumberOfPartitionEntriesTest), },
		{ TEST_CASE(MyLbaTest), },
		{ TEST_CASE(FirstUsableLbaAndLastUsableLbaTest), },
		{ TEST_CASE(EntriesCrcTest), },
		{ TEST_CASE(ValidEntryTest), },
		{ TEST_CASE(OverlappedPartitionTest), },
		{ TEST_CASE(SanityCheckTest), },
		{ TEST_CASE(NoValidKernelEntryTest), },
		{ TEST_CASE(MtdNoValidKernelEntryTest), },
		{ TEST_CASE(EntryAttributeGetSetTest), },
		{ TEST_CASE(EntryTypeTest), },
		{ TEST_CASE(GetNextNormalTest), },
		{ TEST_CASE(GetNextPrioTest), },
		{ TEST_CASE(GetNextTriesTest), },
		{ TEST_CASE(MtdGetNextNormalTest), },
		{ TEST_CASE(MtdGetNextPrioTest), },
		{ TEST_CASE(MtdGetNextTriesTest), },
		{ TEST_CASE(GptUpdateTest), },
		{ TEST_CASE(MtdUpdateTest), },
		{ TEST_CASE(UpdateInvalidKernelTypeTest), },
		{ TEST_CASE(MtdUpdateInvalidKernelTypeTest), },
		{ TEST_CASE(DuplicateUniqueGuidTest), },
		{ TEST_CASE(TestCrc32TestVectors), },
		{ TEST_CASE(GetKernelGuidTest), },
		{ TEST_CASE(ErrorTextTest), },
		{ TEST_CASE(MtdFtsTest), },
	};

	for (i = 0; i < sizeof(test_cases)/sizeof(test_cases[0]); ++i) {
		printf("Running %s() ...\n", test_cases[i].name);
		test_cases[i].retval = test_cases[i].fp();
		if (test_cases[i].retval) {
			printf(COL_RED "[ERROR]\n\n" COL_STOP);
			++error_count;
		} else {
			printf(COL_GREEN "[PASS]\n\n" COL_STOP);
		}
	}

	if (error_count) {
		printf("\n------------------------------------------------\n");
		printf(COL_RED "The following %d test cases are failed:\n"
		       COL_STOP, error_count);
		for (i = 0; i < sizeof(test_cases)/sizeof(test_cases[0]); ++i) {
			if (test_cases[i].retval)
				printf("  %s()\n", test_cases[i].name);
		}
	}

	return error_count ? 1 : 0;
}
