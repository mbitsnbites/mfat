//--------------------------------------------------------------------------------------------------
// Copyright (C) 2022 Marcus Geelnard
//
// Redistribution and use in source and binary forms, with or without modification, are permitted
// provided that the following conditions are met:
//
//   1. Redistributions of source code must retain the above copyright notice, this list of
//      conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright notice, this list of
//      conditions and the following disclaimer in the documentation and/or other materials provided
//      with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
// WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//--------------------------------------------------------------------------------------------------

#include "mfat.h"

#include <inttypes.h>
#include <string.h>  // memcpy, memset

//--------------------------------------------------------------------------------------------------
// Configuration.
//--------------------------------------------------------------------------------------------------

// Enable debugging?
#ifndef MFAT_ENABLE_DEBUG
#define MFAT_ENABLE_DEBUG 0
#endif

// Enable writing?
#ifndef MFAT_ENABLE_WRITE
#define MFAT_ENABLE_WRITE 1
#endif

// Enable MBR support?
#ifndef MFAT_ENABLE_MBR
#define MFAT_ENABLE_MBR 1
#endif

// Enable GPT support?
#ifndef MFAT_ENABLE_GPT
#define MFAT_ENABLE_GPT 1
#endif

// Number of cached blocks.
#ifndef MFAT_NUM_CACHED_BLOCKS
#define MFAT_NUM_CACHED_BLOCKS 2
#endif

// Maximum number of open file descriptors.
#ifndef MFAT_NUM_FDS
#define MFAT_NUM_FDS 4
#endif

// Maximum number of partitions to support.
#ifndef MFAT_NUM_PARTITIONS
#define MFAT_NUM_PARTITIONS 4
#endif

//--------------------------------------------------------------------------------------------------
// Debugging macros.
//--------------------------------------------------------------------------------------------------

#if MFAT_ENABLE_DEBUG
#include <stdio.h>
#define DBG(_str) (void)fprintf(stderr, "[MFAT] " _str "\n")
#define DBGF(_fmt, ...) (void)fprintf(stderr, "[MFAT] " _fmt "\n", __VA_ARGS__)
#else
#define DBG(_str)
#define DBGF(_fmt, ...)
#endif  //  MFAT_ENABLE_DEBUG

//--------------------------------------------------------------------------------------------------
// FAT definitions (used for encoding/decoding).
//--------------------------------------------------------------------------------------------------

// Known partition ID:s.
#define MFAT_PART_ID_NONE 0x00
#define MFAT_PART_ID_FAT16_LT32GB 0x04
#define MFAT_PART_ID_FAT16_GT32GB 0x06
#define MFAT_PART_ID_FAT32 0x0b
#define MFAT_PART_ID_FAT32_LBA 0x0c
#define MFAT_PART_ID_FAT16_GT32GB_LBA 0x0e

// File attribute flags.
#define MFAT_ATTR_READ_ONLY 0x01
#define MFAT_ATTR_HIDDEN 0x02
#define MFAT_ATTR_SYSTEM 0x04
#define MFAT_ATTR_VOLUME_ID 0x08
#define MFAT_ATTR_DIRECTORY 0x10
#define MFAT_ATTR_ARCHIVE 0x20
#define MFAT_ATTR_LONG_NAME 0x0f

//--------------------------------------------------------------------------------------------------
// Private types and constants.
//--------------------------------------------------------------------------------------------------

// A custom boolean type (stdbool is byte-sized which is unnecessarily performance costly).
typedef int mfat_bool_t;
#define false 0
#define true 1

// mfat_cached_block_t::state
#define MFAT_INVALID 0
#define MFAT_VALID 1
#define MFAT_DIRTY 2

// Different types of caches (we keep independent block types in different caches).
#define MFAT_CACHE_DATA 0
#define MFAT_CACHE_FAT 1
#define MFAT_NUM_CACHES 2

// mfat_partition_t::type
#define MFAT_TYPE_UNKNOWN 0
#define MFAT_TYPE_FAT_UNDECIDED 1  // Indicates a FAT partion of yet unknown type (FAT16 or FAT32).
#define MFAT_TYPE_FAT16 2
#define MFAT_TYPE_FAT32 3

// A collection of variables for keeping track of the current cluster & block position, e.g. during
// read/write operations.
typedef struct {
  uint32_t cluster_no;         ///< The cluster number.
  uint32_t block_in_cluster;   ///< The block offset within the cluster (0..blocks_per_cluster-1).
  uint32_t cluster_start_blk;  ///< Absolute block number of the first block of the cluster.
} mfat_cluster_pos_t;

typedef struct {
  uint32_t type;
  uint32_t first_block;
  uint32_t num_blocks;
  uint32_t blocks_per_cluster;
#if MFAT_ENABLE_WRITE
  uint32_t num_clusters;
#endif
  uint32_t blocks_per_fat;
  uint32_t num_fats;
  uint32_t num_reserved_blocks;
  uint32_t root_dir_block;      // Used for FAT16.
  uint32_t blocks_in_root_dir;  // Used for FAT16 (zero for FAT32).
  uint32_t root_dir_cluster;    // Used for FAT32.
  uint32_t first_data_block;
  mfat_bool_t boot;
} mfat_partition_t;

// Static information about a file, as specified in the file system.
typedef struct {
  int part_no;                // Partition that this file is located on.
  uint32_t size;              // Total size of file, in bytes.
  uint32_t first_cluster;     // Starting cluster for the file.
  uint32_t dir_entry_block;   // Block number for the directory entry of this file.
  uint32_t dir_entry_offset;  // Offset (in bytes) into the directory entry block.
} mfat_file_info_t;

typedef struct {
  mfat_bool_t open;          // Is the file open?
  int oflag;                 // Flags used when opening the file.
  uint32_t offset;           // Current byte offset relative to the file start (seek offset).
  uint32_t current_cluster;  // Current cluster (representing the current seek offset)
  mfat_file_info_t info;
} mfat_file_t;

typedef struct {
  int state;
  uint32_t blk_no;
  uint8_t buf[MFAT_BLOCK_SIZE];
} mfat_cached_block_t;

typedef struct {
  mfat_cached_block_t block[MFAT_NUM_CACHED_BLOCKS];
#if MFAT_NUM_CACHED_BLOCKS > 1
  // This is a priority queue: The last item in the queue is an index to the
  // least recently used cached block item.
  int pri[MFAT_NUM_CACHED_BLOCKS];
#endif
} mfat_cache_t;

typedef struct {
  mfat_bool_t initialized;
  int active_partition;
  mfat_read_block_fun_t read;
#if MFAT_ENABLE_WRITE
  mfat_write_block_fun_t write;
#endif
  void* custom;
  mfat_partition_t partition[MFAT_NUM_PARTITIONS];
  mfat_file_t file[MFAT_NUM_FDS];
  mfat_cache_t cache[MFAT_NUM_CACHES];
} mfat_ctx_t;

// Statically allocated state.
static mfat_ctx_t s_ctx;

//--------------------------------------------------------------------------------------------------
// Private functions.
//--------------------------------------------------------------------------------------------------

static inline uint32_t _mfat_min(uint32_t a, uint32_t b) {
  return (a < b) ? a : b;
}

static uint32_t _mfat_get_word(const uint8_t* buf) {
  return ((uint32_t)buf[0]) | (((uint32_t)buf[1]) << 8);
}

static uint32_t _mfat_get_dword(const uint8_t* buf) {
  return ((uint32_t)buf[0]) | (((uint32_t)buf[1]) << 8) | (((uint32_t)buf[2]) << 16) |
         (((uint32_t)buf[3]) << 24);
}

static mfat_bool_t _mfat_cmpbuf(const uint8_t* a, const uint8_t* b, const uint32_t nbyte) {
  for (uint32_t i = 0; i < nbyte; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

#if MFAT_ENABLE_MBR
static mfat_bool_t _mfat_is_fat_part_id(uint32_t id) {
  switch (id) {
    case MFAT_PART_ID_FAT16_LT32GB:
    case MFAT_PART_ID_FAT16_GT32GB:
    case MFAT_PART_ID_FAT16_GT32GB_LBA:
    case MFAT_PART_ID_FAT32:
    case MFAT_PART_ID_FAT32_LBA:
      return true;
    default:
      return false;
  }
}
#endif  // MFAT_ENABLE_MBR

#if MFAT_ENABLE_GPT
static mfat_bool_t _mfat_is_fat_part_guid(const uint8_t* guid) {
  // Windows Basic data partition GUID = EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
  static const uint8_t BDP_GUID[] = {0xa2,
                                     0xa0,
                                     0xd0,
                                     0xeb,
                                     0xe5,
                                     0xb9,
                                     0x33,
                                     0x44,
                                     0x87,
                                     0xc0,
                                     0x68,
                                     0xb6,
                                     0xb7,
                                     0x26,
                                     0x99,
                                     0xc7};
  if (_mfat_cmpbuf(guid, &BDP_GUID[0], sizeof(BDP_GUID))) {
    // TODO(m): These may also be NTFS partitions. How to detect?
    return true;
  }
  return false;
}
#endif  // MFAT_ENABLE_GPT

static mfat_bool_t _mfat_is_valid_bpb(const uint8_t* bpb_buf) {
  // Check that the BPB signature is there.
  if (bpb_buf[510] != 0x55U || bpb_buf[511] != 0xaaU) {
    DBGF("\t\tInvalid BPB signature: [%02x,%02x]", bpb_buf[510], bpb_buf[511]);
    return false;
  }

  // Check for a valid jump instruction (first three bytes).
  if (bpb_buf[0] != 0xe9 && !(bpb_buf[0] == 0xeb && bpb_buf[2] == 0x90)) {
    DBGF("\t\tInvalid BPB jump code: [%02x,%02x,%02x]", bpb_buf[0], bpb_buf[1], bpb_buf[2]);
    return false;
  }

  // Check for valid bytes per sector.
  uint32_t bps = _mfat_get_word(&bpb_buf[11]);
  if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
    DBGF("\t\tInvalid BPB bytes per sector: %" PRIu32, bps);
    return false;
  }

  return true;
}

static mfat_cached_block_t* _mfat_get_cached_block(uint32_t blk_no, int cache_type) {
  // Pick the relevant cache.
  mfat_cache_t* cache = &s_ctx.cache[cache_type];

#if MFAT_NUM_CACHED_BLOCKS > 1
  // By default, pick the last (least recently used) item in the pirority queue...
  int item_id = cache->pri[MFAT_NUM_CACHED_BLOCKS - 1];

  // ...but override it if we have a cache hit.
  for (int i = 0; i < MFAT_NUM_CACHED_BLOCKS; ++i) {
    mfat_cached_block_t* cb = &cache->block[i];
    if (cb->state != MFAT_INVALID && cb->blk_no == blk_no) {
      item_id = i;
      break;
    }
  }

  // Move the selected item to the front of the priority queue (i.e. it's the most recently used
  // cache item).
  {
    int prev_id = item_id;
    for (int i = 0; i < MFAT_NUM_CACHED_BLOCKS; ++i) {
      int this_id = cache->pri[i];
      cache->pri[i] = prev_id;
      if (this_id == item_id) {
        break;
      }
      prev_id = this_id;
    }
  }

  mfat_cached_block_t* cached_block = &cache->block[item_id];
#else
  mfat_cached_block_t* cached_block = &cache->block[0];
#endif

  // Reassign the cached block to the requested block number (if necessary).
  if (cached_block->blk_no != blk_no) {
#if MFAT_ENABLE_DEBUG
    if (cached_block->state != MFAT_INVALID) {
      DBGF("Cache %d: Evicting block %" PRIu32 " in favor of block %" PRIu32,
           cache_type,
           cached_block->blk_no,
           blk_no);
    }
#endif

#if MFAT_ENABLE_WRITE
    // Flush the block?
    if (cached_block->state == MFAT_DIRTY) {
      DBGF("Cache %d: Flushing evicted block %" PRIu32, cache_type, cached_block->blk_no);
      if (s_ctx.write((const char*)cached_block->buf, cached_block->blk_no, s_ctx.custom) == -1) {
        // FATAL: We can't recover from here... :-(
        DBGF("Cache %d: Failed to flush the block", cache_type);
        return NULL;
      }
    }
#endif

    // Set the new block ID.
    cached_block->blk_no = blk_no;

    // The contents of the buffer is now invalid.
    cached_block->state = MFAT_INVALID;
  }

  return cached_block;
}

static mfat_cached_block_t* _mfat_read_block(uint32_t block_no, int cache_type) {
  // First query the cache.
  mfat_cached_block_t* block = _mfat_get_cached_block(block_no, cache_type);
  if (block == NULL) {
    return NULL;
  }

  // If necessary, read the block from storage.
  if (block->state == MFAT_INVALID) {
    if (s_ctx.read((char*)block->buf, block_no, s_ctx.custom) == -1) {
      return NULL;
    }
    block->state = MFAT_VALID;
  }

  return block;
}

// Helper function for finding the next cluster in a cluster chain.
static mfat_bool_t _mfat_next_cluster(const mfat_partition_t* part, uint32_t* cluster) {
  const uint32_t fat_entry_size = (part->type == MFAT_TYPE_FAT32) ? 4 : 2;

  uint32_t fat_offset = fat_entry_size * (*cluster);
  uint32_t fat_block =
      part->first_block + part->num_reserved_blocks + (fat_offset / MFAT_BLOCK_SIZE);
  uint32_t fat_block_offset = fat_offset % MFAT_BLOCK_SIZE;

  // For FAT copy no. N (0..num_fats-1):
  // fat_block += N * part->blocks_per_fat

  // Read the FAT block into a cached buffer.
  mfat_cached_block_t* block = _mfat_read_block(fat_block, MFAT_CACHE_FAT);
  if (block == NULL) {
    DBGF("Failed to read the FAT block %" PRIu32, fat_block);
    return false;
  }
  uint8_t* buf = &block->buf[0];

  // Get the value for this cluster from the FAT.
  uint32_t next_cluster;
  if (part->type == MFAT_TYPE_FAT32) {
    // For FAT32 we mask off upper 4 bits, as the cluster number is 28 bits.
    next_cluster = _mfat_get_dword(&buf[fat_block_offset]) & 0x0fffffffU;
  } else {
    next_cluster = _mfat_get_word(&buf[fat_block_offset]);
    if (next_cluster >= 0xfff7U) {
      // Convert FAT16 special codes (BAD & EOC) to FAT32 codes.
      next_cluster |= 0x0fff0000U;
    }
  }

  // This is a sanity check (failure indicates a corrupt filesystem). We should really do this check
  // BEFORE accessing the cluster instead.
  // We do not expect to see:
  //  0x00000000 - free cluster
  //  0x0ffffff7 - BAD cluster
  if (next_cluster == 0U || next_cluster == 0x0ffffff7U) {
    DBGF("Unexpected next cluster: 0x%08" PRIx32, next_cluster);
    return false;
  }

  DBGF("Next cluster: %" PRIu32 " (0x%08" PRIx32 ")", next_cluster, next_cluster);

  // Return the next cluster number.
  *cluster = next_cluster;

  return true;
}

// Helper function for finding the first block of a cluster.
static uint32_t _mfat_first_block_of_cluster(const mfat_partition_t* part, uint32_t cluster) {
  return part->first_data_block + ((cluster - 2U) * part->blocks_per_cluster);
}

// Initialize a cluster pos object to the current cluster & offset (e.g. for a file).
static mfat_cluster_pos_t _mfat_cluster_pos_init(const mfat_partition_t* part,
                                                 const uint32_t cluster_no,
                                                 const uint32_t offset) {
  mfat_cluster_pos_t cpos;
  cpos.cluster_no = cluster_no;
  cpos.block_in_cluster = (offset % (part->blocks_per_cluster * MFAT_BLOCK_SIZE)) / MFAT_BLOCK_SIZE;
  cpos.cluster_start_blk = _mfat_first_block_of_cluster(part, cluster_no);
  return cpos;
}

// Initialize a cluster pos object to the current file offset of the specified file.
static mfat_cluster_pos_t _mfat_cluster_pos_init_from_file(const mfat_partition_t* part,
                                                           const mfat_file_t* f) {
  return _mfat_cluster_pos_init(part, f->current_cluster, f->offset);
}

// Advance a cluster pos by one block.
static mfat_bool_t _mfat_cluster_pos_advance(mfat_cluster_pos_t* cpos,
                                             const mfat_partition_t* part) {
  ++cpos->block_in_cluster;
  if (cpos->block_in_cluster == part->blocks_per_cluster) {
    if (!_mfat_next_cluster(part, &cpos->cluster_no)) {
      return false;
    }
    cpos->cluster_start_blk = _mfat_first_block_of_cluster(part, cpos->cluster_no);
    cpos->block_in_cluster = 0;
  }
  return true;
}

// Get the current absolute block of a cluster pos object.
static uint32_t _mfat_cluster_pos_blk_no(const mfat_cluster_pos_t* cpos) {
  return cpos->cluster_start_blk + cpos->block_in_cluster;
}

// Check for EOC (End of clusterchain). NOTE: EOC cluster is included in file.
static mfat_bool_t _mfat_is_eoc(const uint32_t cluster) {
  return cluster >= 0x0ffffff8U;
}

#if MFAT_ENABLE_GPT
static mfat_bool_t _mfat_decode_gpt(void) {
  // Read the primary GUID Partition Table (GPT) header, located at block 1.
  mfat_cached_block_t* block = _mfat_read_block(1U, MFAT_CACHE_DATA);
  if (block == NULL) {
    DBG("Failed to read the GPT");
    return false;
  }
  uint8_t* buf = &block->buf[0];

  // Is this in fact an GPT header?
  // TODO(m): We could do more validation (CRC etc).
  static const uint8_t gpt_sig[] = {0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54};
  if (!_mfat_cmpbuf(&buf[0], &gpt_sig[0], sizeof(gpt_sig))) {
    DBG("Not a valid GPT signature");
    return false;
  }

  // Get the start LBA of the the partition entries.
  // Note: This is actually a 64-bit value, but we don't support such large media anyway, and by
  // reading it out as a 32-bit little endian number we get the correct value IF the upper 32
  // bits are zero. This should be OK as the value should always be 0x0000000000000002 for the
  // primary copy, which we are interested in.
  uint32_t entries_block = _mfat_get_dword(&buf[72]);

  // Get the number of partition entries.
  uint32_t num_entries = _mfat_get_dword(&buf[80]);

  // Get the size of each partition entry, in bytes.
  uint32_t entry_size = _mfat_get_dword(&buf[84]);

  DBGF("GPT header: entries_block=%" PRIu32 ", num_entries=%" PRIu32 ", entry_size=%" PRIu32,
       entries_block,
       num_entries,
       entry_size);

  uint32_t entry_offs = 0;
  for (uint32_t i = 0; i < num_entries && i < MFAT_NUM_PARTITIONS; ++i) {
    // Read the next block of the partition entry array if necessary.
    if ((entry_offs % MFAT_BLOCK_SIZE) == 0) {
      block = _mfat_read_block(entries_block, MFAT_CACHE_DATA);
      if (block == NULL) {
        DBGF("Failed to read the GPT partition entry array at block %" PRIu32, entries_block);
        return false;
      }
      buf = &block->buf[0];

      ++entries_block;
      entry_offs = 0;
    }

    // Decode the partition entry.
    uint8_t* entry = &buf[entry_offs];
    mfat_partition_t* part = &s_ctx.partition[i];

    // Get the starting block of the partition (again, 64-bit value that we read as a 32-bit
    // value).
    part->first_block = _mfat_get_dword(&entry[32]);

    // Check if the partition is bootable (bit ).
    part->boot = ((entry[48] & 0x04) != 0);

    // Is this a potential FAT partition?
    if (_mfat_is_fat_part_guid(&entry[0])) {
      // The actual FAT type is determined later.
      part->type = MFAT_TYPE_FAT_UNDECIDED;
    }

    // Note: We print GUIDs in byte order, as we expect them in our signatures, NOT in GUID
    // mixed endian order.
    DBGF("GPT entry %" PRIu32 ": first_block = %" PRIu32 ", GUID = %08" PRIx32 "%08" PRIx32
         "%08" PRIx32 "%08" PRIx32 ", FAT = %s",
         i,
         part->first_block,
         _mfat_get_dword(&entry[12]),
         _mfat_get_dword(&entry[8]),
         _mfat_get_dword(&entry[4]),
         _mfat_get_dword(&entry[0]),
         part->type != MFAT_TYPE_UNKNOWN ? "Yes" : "No");

    entry_offs += entry_size;
  }

  return true;
}
#endif  // MFAT_ENABLE_GPT

#if MFAT_ENABLE_MBR
static mfat_bool_t _mfat_decode_mbr(void) {
  mfat_cached_block_t* block = _mfat_read_block(0U, MFAT_CACHE_DATA);
  if (block == NULL) {
    DBG("Failed to read the MBR");
    return false;
  }
  uint8_t* buf = &block->buf[0];

  // Is this an MBR block (can also be a partition boot record)?
  mfat_bool_t found_valid_mbr = false;
  if (buf[510] == 0x55U && buf[511] == 0xaaU) {
    // Parse each partition entry.
    for (int i = 0; i < 4 && i < MFAT_NUM_PARTITIONS; ++i) {
      uint8_t* entry = &buf[446U + 16U * (uint32_t)i];
      mfat_partition_t* part = &s_ctx.partition[i];

      // Current state of partition (00h=Inactive, 80h=Active).
      part->boot = ((entry[0] & 0x80U) != 0);

      // Is this a FAT partition.
      if (_mfat_is_fat_part_id(entry[4])) {
        // The actual FAT type is determined later.
        part->type = MFAT_TYPE_FAT_UNDECIDED;
        found_valid_mbr = true;
      }

      // Get the beginning of the partition (LBA).
      part->first_block = _mfat_get_dword(&entry[8]);
    }
  } else {
    DBGF("Not a valid MBR signature: [%02x,%02x]", buf[510], buf[511]);
  }

  return found_valid_mbr;
}
#endif  // MFAT_ENABLE_MBR

static void _mfat_decode_tableless(void) {
  // Some storage media are formatted without an MBR or GPT. If so, there is only a single volume
  // and the first block is the BPB (BIOS Parameter Block) of that "partition". We initially guess
  // that this is the case, and let the partition decoding logic assert if this was a good guess.
  DBG("Assuming that the storage medium does not have a partition table");

  // Clear all partitions (their values are potentially garbage).
  for (int i = 0; i < MFAT_NUM_PARTITIONS; ++i) {
    memset(&s_ctx.partition[i], 0, sizeof(mfat_partition_t));
  }

  // Guess that the first partition is FAT (we'll detect the actual type later).
  // Note: The "first_block" field is cleared to zero in the previous memset, so the first
  // partition starts at the first block, which is what we intend.
  s_ctx.partition[0].type = MFAT_TYPE_FAT_UNDECIDED;
}

static mfat_bool_t _mfat_decode_partition_tables(void) {
  mfat_bool_t found_partition_table = false;

#if MFAT_ENABLE_GPT
  // 1: Try to read the GUID Partition Table (GPT).
  if (!found_partition_table) {
    found_partition_table = _mfat_decode_gpt();
  }
#endif

#if MFAT_ENABLE_MBR
  // 2: Try to read the Master Boot Record (MBR).
  if (!found_partition_table) {
    found_partition_table = _mfat_decode_mbr();
  }
#endif

  // 3: Assume that the storage medium does not have a partition table at all.
  if (!found_partition_table) {
    _mfat_decode_tableless();
  }

  // Read and parse the BPB for each FAT partition.
  for (int i = 0; i < MFAT_NUM_PARTITIONS; ++i) {
    DBGF("Partition %d:", i);
    mfat_partition_t* part = &s_ctx.partition[i];

    // Skip unsupported partition types.
    if (part->type == MFAT_TYPE_UNKNOWN) {
      DBG("\t\tNot a FAT partition");
      continue;
    }

    // Load the BPB (the first block of the partition).
    mfat_cached_block_t* block = _mfat_read_block(part->first_block, MFAT_CACHE_DATA);
    if (block == NULL) {
      DBG("\t\tFailed to read the BPB");
      return false;
    }
    uint8_t* buf = &block->buf[0];

    if (!_mfat_is_valid_bpb(buf)) {
      DBG("\t\tPartition does not appear to have a valid BPB");
      part->type = MFAT_TYPE_UNKNOWN;
      continue;
    }

    // Decode useful metrics from the BPB (these are used in block number arithmetic etc).
    uint32_t bytes_per_block = _mfat_get_word(&buf[11]);
    part->blocks_per_cluster = buf[13];
    part->num_reserved_blocks = _mfat_get_word(&buf[14]);
    part->num_fats = buf[16];
    {
      uint32_t tot_sectors_16 = _mfat_get_word(&buf[19]);
      uint32_t tot_sectors_32 = _mfat_get_dword(&buf[32]);
      part->num_blocks = (tot_sectors_16 != 0) ? tot_sectors_16 : tot_sectors_32;
    }
    {
      uint32_t blocks_per_fat_16 = _mfat_get_word(&buf[22]);
      uint32_t blocks_per_fat_32 = _mfat_get_dword(&buf[36]);
      part->blocks_per_fat = (blocks_per_fat_16 != 0) ? blocks_per_fat_16 : blocks_per_fat_32;
    }
    {
      uint32_t num_root_entries = _mfat_get_word(&buf[17]);
      part->blocks_in_root_dir =
          ((num_root_entries * 32U) + (MFAT_BLOCK_SIZE - 1U)) / MFAT_BLOCK_SIZE;
    }

    // Derive useful metrics.
    part->first_data_block = part->first_block + part->num_reserved_blocks +
                             (part->num_fats * part->blocks_per_fat) + part->blocks_in_root_dir;

    // Check that the number of bytes per sector is 512.
    // TODO(m): We could add support for larger block sizes.
    if (bytes_per_block != MFAT_BLOCK_SIZE) {
      DBGF("\t\tUnsupported block size: %" PRIu32, bytes_per_block);
      part->type = MFAT_TYPE_UNKNOWN;
      continue;
    }

    // Up until now we have only been guessing the partition FAT type. Now we determine the *actual*
    // partition type (FAT12, FAT16 or FAT32), using the detection algorithm given in the FAT32 File
    // System Specification, p.14. The definition is based on the count of clusters as follows:
    //   FAT12: count of clusters < 4085
    //   FAT16: 4085 <= count of clusters < 65525
    //   FAT32: 65525 <= count of clusters
    {
      uint32_t root_ent_cnt = _mfat_get_word(&buf[17]);
      uint32_t root_dir_sectors = ((root_ent_cnt * 32) + (MFAT_BLOCK_SIZE - 1)) / MFAT_BLOCK_SIZE;

      uint32_t data_sectors =
          part->num_blocks -
          (part->num_reserved_blocks + (part->num_fats * part->blocks_per_fat) + root_dir_sectors);

      uint32_t count_of_clusters = data_sectors / part->blocks_per_cluster;

#if MFAT_ENABLE_WRITE
      // We need to know the actual number of clusters when writing files.
      part->num_clusters = count_of_clusters + 1;
#endif

      // We don't support FAT12.
      if (count_of_clusters < 4085) {
        part->type = MFAT_TYPE_UNKNOWN;
        DBG("\t\tFAT12 is not supported.");
        continue;
      }

      if (count_of_clusters < 65525) {
        part->type = MFAT_TYPE_FAT16;
      } else {
        part->type = MFAT_TYPE_FAT32;
      }
    }

    // Determine the starting block/cluster for the root directory.
    if (part->type == MFAT_TYPE_FAT16) {
      part->root_dir_block = part->first_data_block - part->blocks_in_root_dir;
    } else {
      part->root_dir_cluster = _mfat_get_dword(&buf[44]);
    }

#if MFAT_ENABLE_DEBUG
    // Print the partition information.
    DBGF("\t\ttype = %s", part->type == MFAT_TYPE_FAT16 ? "FAT16" : "FAT32");
    DBGF("\t\tboot = %s", part->boot ? "Yes" : "No");
    DBGF("\t\tfirst_block = %" PRIu32, part->first_block);
    DBGF("\t\tbytes_per_block = %" PRIu32, bytes_per_block);
    DBGF("\t\tnum_blocks = %" PRIu32, part->num_blocks);
    DBGF("\t\tblocks_per_cluster = %" PRIu32, part->blocks_per_cluster);
#if MFAT_ENABLE_WRITE
    DBGF("\t\tnum_clusters = %" PRIu32, part->num_clusters);
#endif
    DBGF("\t\tblocks_per_fat = %" PRIu32, part->blocks_per_fat);
    DBGF("\t\tnum_fats = %" PRIu32, part->num_fats);
    DBGF("\t\tnum_reserved_blocks = %" PRIu32, part->num_reserved_blocks);
    DBGF("\t\troot_dir_block = %" PRIu32, part->root_dir_block);
    DBGF("\t\troot_dir_cluster = %" PRIu32, part->root_dir_cluster);
    DBGF("\t\tblocks_in_root_dir = %" PRIu32, part->blocks_in_root_dir);
    DBGF("\t\tfirst_data_block = %" PRIu32, part->first_data_block);

    // Print the extended boot signature.
    const uint8_t* ex_boot_sig = (part->type == MFAT_TYPE_FAT32) ? &buf[66] : &buf[38];
    if (ex_boot_sig[0] == 0x29) {
      uint32_t vol_id = _mfat_get_dword(&ex_boot_sig[1]);

      char label[12];
      memcpy(&label[0], &ex_boot_sig[5], 11);
      label[11] = 0;

      char fs_type[9];
      memcpy(&fs_type[0], &ex_boot_sig[16], 8);
      fs_type[8] = 0;

      DBGF("\t\tvol_id = %04" PRIx32 ":%04" PRIx32, (vol_id >> 16), vol_id & 0xffffU);
      DBGF("\t\tlabel = \"%s\"", label);
      DBGF("\t\tfs_type = \"%s\"", fs_type);
    } else {
      DBGF("\t\t(Boot signature N/A - bpb[%d]=0x%02x)", (int)(ex_boot_sig - buf), ex_boot_sig[0]);
    }
#endif  // MFAT_ENABLE_DEBUG
  }

  return true;
}

static mfat_file_t* _mfat_get_file(int fd) {
  if (fd < 0 || fd >= MFAT_NUM_FDS) {
    DBGF("FD out of range: %d", fd);
    return NULL;
  }
  mfat_file_t* f = &s_ctx.file[fd];
  if (!f->open) {
    DBGF("File is not open: %d", fd);
    return NULL;
  }
  return f;
}

static void _mfat_dir_entry_to_stat(uint8_t* dir_entry, mfat_stat_t* stat) {
  // Decode file attributes.
  uint32_t attr = dir_entry[11];
  uint32_t st_mode =
      MFAT_S_IRUSR | MFAT_S_IRGRP | MFAT_S_IROTH | MFAT_S_IXUSR | MFAT_S_IXGRP | MFAT_S_IXOTH;
  if ((attr & MFAT_ATTR_READ_ONLY) == 0) {
    st_mode |= MFAT_S_IWUSR | MFAT_S_IWGRP | MFAT_S_IWOTH;
  }
  if ((attr & MFAT_ATTR_DIRECTORY) != 0) {
    st_mode |= MFAT_S_IFDIR;
  } else {
    st_mode |= MFAT_S_IFREG;
  }
  stat->st_mode = st_mode;

  // Decode time.
  uint32_t time = _mfat_get_word(&dir_entry[22]);
  stat->st_mtim.hour = time >> 11;
  stat->st_mtim.minute = (time >> 5) & 63;
  stat->st_mtim.second = (time & 31) * 2;

  // Decode date.
  uint32_t date = _mfat_get_word(&dir_entry[24]);
  stat->st_mtim.year = (date >> 9) + 1980U;
  stat->st_mtim.month = (date >> 5) & 15;
  stat->st_mtim.day = date & 31;

  // Get file size.
  stat->st_size = _mfat_get_dword(&dir_entry[28]);
}

static int _mfat_canonicalize_char(int c) {
  // Valid character?
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '%' || c == '-' ||
      c == '_' || c == '@' || c == '~' || c == '`' || c == '!' || c == '(' || c == ')' ||
      c == '{' || c == '}' || c == '^' || c == '#' || c == '&') {
    return c;
  }

  // Convert lower case to upper case.
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 'A';
  }

  // Invalid character.
  return '!';
}

/// @brief Canonicalize a file name.
///
/// Convert the next path part (up until a directory separator or the end of the string) into a
/// valid 8.3 short file name as represented in a FAT directory entry (i.e. space padded and without
/// the dot). File names that do not fit in an 8.3 representation will be silently truncated.
///
/// Examples:
///   - "hello.txt"          -> "HELLO   TXT"
///   - "File.1"             -> "FILE    1  "
///   - "ALongFileName.json" -> "ALONGFILJSO"
///   - "bin/foo.exe"        -> "BIN        " (followed by "FOO     EXE" at the next invocation)
///   - "./foo.exe"          -> "FOO     EXE"
/// @param path A path to a file, possibly includuing directory separators (/ or \).
/// @param[out] fname The canonicalized file name.
/// @returns the index of the next path part, or -1 if this was the last path part.
static int _mfat_canonicalize_fname(const char* path, char name[12]) {
  int next_idx = 0;

  do {
    int pos = next_idx;
    int npos = 0;
    int c;

    // Extract the name part.
    while (true) {
      c = (int)(uint8_t)path[pos++];
      if (c == 0 || c == '.' || c == '/' || c == '\\') {
        break;
      }
      if (npos < 8) {
        name[npos++] = (char)_mfat_canonicalize_char(c);
      }
    }

    // Space-fill remaining characters of the name part.
    for (; npos < 8; ++npos) {
      name[npos] = ' ';
    }

    // Extract the extension part.
    if (c == '.') {
      while (true) {
        c = (int)(uint8_t)path[pos++];
        if (c == 0 || c == '/' || c == '\\') {
          break;
        }
        if (npos < 11) {
          name[npos++] = (char)_mfat_canonicalize_char(c);
        }
      }
    }

    // Space-fill remaining characters of the extension part.
    for (; npos < 11; ++npos) {
      name[npos] = ' ';
    }

    // Zero terminate the string.
    name[11] = 0;

    // Was this a directory part of the path (ignore trailing directory separators)?
    if ((c == '/' || c == '\\') && path[pos] != 0) {
      // Return the starting position of the next path part.
      next_idx = pos;
    } else {
      // Indicate that there are no more path parts by returning -1.
      next_idx = -1;
    }

    // Skip empty "/" and "./" directory references.
  } while (next_idx >= 0 &&
           _mfat_cmpbuf((const uint8_t*)&name[0], (const uint8_t*)"           ", 11));

  return next_idx;
}

/// @brief Find a file on the given partition.
///
/// If the directory (if part of the path) exists, but the file does not exist in the directory,
/// this function will find the first free directory entry slot and set @c exists to false. This is
/// useful for creating new files.
/// @param part_no The partition number.
/// @param path The absolute path to the file.
/// @param[out] info Information about the file.
/// @param[out] is_dir true if the file is a directory, false if it is a regular file.
/// @param[out] exists true if the file exists, false if it needs to be created.
/// @returns true if the file (or its potential slot) was found.
static mfat_bool_t _mfat_find_file(int part_no,
                                   const char* path,
                                   mfat_file_info_t* info,
                                   mfat_bool_t* is_dir,
                                   mfat_bool_t* exists) {
  mfat_partition_t* part = &s_ctx.partition[part_no];

  // Start with the root directory cluster/block.
  mfat_cluster_pos_t cpos;
  uint32_t blocks_left;
  if (part->type == MFAT_TYPE_FAT32) {
    cpos = _mfat_cluster_pos_init(part, part->root_dir_cluster, 0);
    blocks_left = 0xffffffffU;
  } else {
    // We use a fake/tweaked cluster pos for FAT16 root directories.
    cpos.cluster_no = 0U;
    cpos.cluster_start_blk = part->root_dir_block;
    cpos.block_in_cluster = 0U;
    blocks_left = part->blocks_in_root_dir;
  }

  // Try to find the given path.
  mfat_cached_block_t* block = NULL;
  uint8_t* file_entry = NULL;
  int path_pos = 0;
  while (path_pos >= 0) {
    // Extract a directory entry compatible file name.
    char fname[12];
    int name_pos = _mfat_canonicalize_fname(&path[path_pos], fname);
    mfat_bool_t is_parent_dir = (name_pos >= 0);
    path_pos = is_parent_dir ? path_pos + name_pos : -1;
    DBGF("Looking for %s: \"%s\"", is_parent_dir ? "parent dir" : "file", fname);

    // Use an "unlimited" block counter if we're doing a clusterchain lookup.
    if (cpos.cluster_no != 0U) {
      blocks_left = 0xffffffffU;
    }

    // Look up the file name in the directory.
    mfat_bool_t no_more_entries = false;
    for (; file_entry == NULL && !no_more_entries && blocks_left > 0U; ++blocks_left) {
      // Load the directory table block.
      block = _mfat_read_block(_mfat_cluster_pos_blk_no(&cpos), MFAT_CACHE_DATA);
      if (block == NULL) {
        DBGF("Unable to load directory block %" PRIu32, _mfat_cluster_pos_blk_no(&cpos));
        return false;
      }
      uint8_t* buf = &block->buf[0];

      // Loop over all the files in this directory block.
      uint8_t* found_entry = NULL;
      for (uint32_t offs = 0U; offs < 512U; offs += 32U) {
        uint8_t* entry = &buf[offs];

        // TODO(m): Look for the first 0xe5-entry too in case we want to create a new file.

        // Last entry in the directory structure?
        if (entry[0] == 0x00) {
          no_more_entries = true;
          break;
        }

        // Is this the file/dir that we are looking for?
        if (_mfat_cmpbuf(&entry[0], (const uint8_t*)&fname[0], 11)) {
          found_entry = entry;
          break;
        }
      }

      // Did we have a match.
      if (found_entry != NULL) {
        uint32_t attr = found_entry[11];

        // Descend into directory?
        if (is_parent_dir) {
          if ((attr & MFAT_ATTR_DIRECTORY) == 0U) {
            DBGF("Not a directory: %s", fname);
            return false;
          }

          // Decode the starting cluster of the child directory entry table.
          uint32_t chile_dir_cluster_no =
              (_mfat_get_word(&found_entry[20]) << 16) | _mfat_get_word(&found_entry[26]);
          cpos = _mfat_cluster_pos_init(part, chile_dir_cluster_no, 0);
          blocks_left = 0xffffffffU;
        } else {
          file_entry = found_entry;
        }

        break;
      }

      // Go to the next block in the directory.
      if (cpos.cluster_no != 0U) {
        if (!_mfat_cluster_pos_advance(&cpos, part)) {
          return false;
        }
      } else {
        cpos.block_in_cluster += 1;  // FAT16 style linear block access.
      }
    }

    // Break loop if we didn't find the file.
    if (no_more_entries) {
      break;
    }
  }

  // Could we neither find the file nor a new directory slot for the file?
  if (file_entry == NULL) {
    return false;
  }

  // Define the file properties.
  info->part_no = part_no;
  info->size = _mfat_get_dword(&file_entry[28]);
  info->first_cluster = (_mfat_get_word(&file_entry[20]) << 16) | _mfat_get_word(&file_entry[26]);
  info->dir_entry_block = block->blk_no;
  info->dir_entry_offset = file_entry - block->buf;

  // Does the file exist?
  if ((file_entry[0] != 0x00) && (file_entry[0] != 0xe5)) {
    *is_dir = (file_entry[11] & MFAT_ATTR_DIRECTORY) != 0U;
    *exists = true;
  } else {
    *is_dir = false;
    *exists = false;
  }

  return true;
}

#if MFAT_ENABLE_WRITE
static void _mfat_sync_impl() {
  for (int j = 0; j < MFAT_NUM_CACHES; ++j) {
    mfat_cache_t* cache = &s_ctx.cache[j];
    for (int i = 0; i < MFAT_NUM_CACHED_BLOCKS; ++i) {
      mfat_cached_block_t* cb = &cache->block[i];
      if (cb->state == MFAT_DIRTY) {
        DBGF("Cache: Flushing block %" PRIu32, cb->blk_no);
        s_ctx.write((const char*)cb->buf, cb->blk_no, s_ctx.custom);
        cb->state = MFAT_VALID;
      }
    }
  }
}
#endif

static int _mfat_fstat_impl(mfat_file_info_t* info, mfat_stat_t* stat) {
  // Read the directory entry block (should already be in the cache).
  mfat_cached_block_t* block = _mfat_read_block(info->dir_entry_block, MFAT_CACHE_DATA);
  if (block == NULL) {
    return -1;
  }

  // Extract the relevant information for this directory entry.
  uint8_t* dir_entry = &block->buf[info->dir_entry_offset];
  _mfat_dir_entry_to_stat(dir_entry, stat);

  return 0;
}

static int _mfat_stat_impl(const char* path, mfat_stat_t* stat) {
  // Find the file in the file system structure.
  mfat_bool_t is_dir;
  mfat_bool_t exists;
  mfat_file_info_t info;
  mfat_bool_t ok = _mfat_find_file(s_ctx.active_partition, path, &info, &is_dir, &exists);
  if (!ok || !exists) {
    DBGF("File not found: %s", path);
    return -1;
  }

  return _mfat_fstat_impl(&info, stat);
}

static int _mfat_open_impl(const char* path, int oflag) {
  // Find the next free fd.
  int fd;
  for (fd = 0; fd < MFAT_NUM_FDS; ++fd) {
    if (!s_ctx.file[fd].open) {
      break;
    }
  }
  if (fd >= MFAT_NUM_FDS) {
    DBG("No free FD:s left");
    return -1;
  }
  mfat_file_t* f = &s_ctx.file[fd];

  // Find the file in the file system structure.
  mfat_bool_t is_dir;
  mfat_bool_t exists;
  if (!_mfat_find_file(s_ctx.active_partition, path, &f->info, &is_dir, &exists)) {
    DBGF("File not found: %s", path);
    return -1;
  }

  // Check that we found the correct type (i.e. regular file).
  if (is_dir) {
    DBGF("Can not open the directory: %s", path);
    return -1;
  }

  // Handle non-existing files.
  if (!exists) {
#if MFAT_ENABLE_WRITE
    // Should we create the file?
    if ((oflag & MFAT_O_CREAT) != 0U) {
      DBG("Creating files is not yet implemented");
      return -1;
    }
#endif

    DBGF("File does not exist: %s", path);
    return -1;
  }

  // Initialize the file state.
  f->open = true;
  f->oflag = oflag;
  f->current_cluster = f->info.first_cluster;
  f->offset = 0U;

  DBGF("Opening file: first_cluster = %" PRIu32 " (block = %" PRIu32 "), size = %" PRIu32
       " bytes, dir_blk = %" PRIu32
       ", dir_offs "
       "= %" PRIu32,
       f->info.first_cluster,
       _mfat_first_block_of_cluster(&s_ctx.partition[f->info.part_no], f->info.first_cluster),
       f->info.size,
       f->info.dir_entry_block,
       f->info.dir_entry_offset);

  return fd;
}

static int _mfat_close_impl(mfat_file_t* f) {
#if MFAT_ENABLE_WRITE
  // For good measure, we flush pending writes when a file is closed (only do this when closing
  // files that are open with write permissions).
  if ((f->oflag & MFAT_O_WRONLY) != 0) {
    _mfat_sync_impl();
  }
#endif

  // The file is no longer open. This makes the fd available for future open() requests.
  f->open = false;

  return 0;
}

static int64_t _mfat_read_impl(mfat_file_t* f, uint8_t* buf, uint32_t nbyte) {
  // Is the file open with read permissions?
  if ((f->oflag & MFAT_O_RDONLY) == 0) {
    return -1;
  }

  // Determine actual size of the operation (clamp to the size of the file).
  if (nbyte > (f->info.size - f->offset)) {
    nbyte = f->info.size - f->offset;
    DBGF("read: Clamped read request to %" PRIu32 " bytes", nbyte);
  }

  // Early out if only zero bytes were requested (e.g. if we are at the EOF).
  if (nbyte == 0U) {
    return 0;
  }

  // Start out at the current file offset.
  mfat_partition_t* part = &s_ctx.partition[f->info.part_no];
  mfat_cluster_pos_t cpos = _mfat_cluster_pos_init_from_file(part, f);
  uint32_t bytes_read = 0U;

  // Align the head of the operation to a block boundary.
  uint32_t block_offset = f->offset % MFAT_BLOCK_SIZE;
  if (block_offset != 0U) {
    // Use the block cache to get a partial block.
    mfat_cached_block_t* block = _mfat_read_block(_mfat_cluster_pos_blk_no(&cpos), MFAT_CACHE_DATA);
    if (block == NULL) {
      DBG("Unable to read block");
      return -1;
    }

    // Copy the data from the cache to the target buffer.
    uint32_t tail_bytes_in_block = MFAT_BLOCK_SIZE - block_offset;
    uint32_t bytes_to_copy = _mfat_min(tail_bytes_in_block, nbyte);
    memcpy(buf, &block->buf[block_offset], bytes_to_copy);
    DBGF("read: Head read of %" PRIu32 " bytes", bytes_to_copy);

    buf += bytes_to_copy;
    bytes_read += bytes_to_copy;

    // Move to the next block if we have read all the bytes of the block.
    if (bytes_to_copy == tail_bytes_in_block) {
      if (!_mfat_cluster_pos_advance(&cpos, part)) {
        return -1;
      }
    }
  }

  // Read aligned blocks directly into the target buffer.
  while ((nbyte - bytes_read) >= MFAT_BLOCK_SIZE) {
    if (_mfat_is_eoc(cpos.cluster_no)) {
      DBG("Unexpected cluster access after EOC");
      return -1;
    }

    DBGF("read: Direct read of %d bytes", MFAT_BLOCK_SIZE);
    if (s_ctx.read((char*)buf, _mfat_cluster_pos_blk_no(&cpos), s_ctx.custom) == -1) {
      DBG("Unable to read block");
      return -1;
    }
    buf += MFAT_BLOCK_SIZE;
    bytes_read += MFAT_BLOCK_SIZE;

    // Move to the next block.
    if (!_mfat_cluster_pos_advance(&cpos, part)) {
      return -1;
    }
  }

  // Handle the tail of the operation (unaligned tail).
  if (bytes_read < nbyte) {
    if (_mfat_is_eoc(cpos.cluster_no)) {
      DBG("Unexpected cluster access after EOC");
      return -1;
    }

    // Use the block cache to get a partial block.
    mfat_cached_block_t* block = _mfat_read_block(_mfat_cluster_pos_blk_no(&cpos), MFAT_CACHE_DATA);
    if (block == NULL) {
      DBG("Unable to read block");
      return -1;
    }

    // Copy the data from the cache to the target buffer.
    uint32_t bytes_to_copy = nbyte - bytes_read;
    memcpy(buf, &block->buf[0], bytes_to_copy);
    DBGF("read: Tail read of %" PRIu32 " bytes", bytes_to_copy);

    bytes_read += bytes_to_copy;
  }

  // Update file state.
  f->current_cluster = cpos.cluster_no;
  f->offset += bytes_read;

  return bytes_read;
}

#if MFAT_ENABLE_WRITE
static int64_t _mfat_write_impl(mfat_file_t* f, const uint8_t* buf, uint32_t nbyte) {
  // Is the file open with write permissions?
  if ((f->oflag & MFAT_O_WRONLY) == 0) {
    return -1;
  }

  // TODO(m): Implement me!
  (void)buf;
  (void)nbyte;
  DBG("_mfat_write_impl() - not yet implemented");
  return -1;
}
#endif

static int64_t _mfat_lseek_impl(mfat_file_t* f, int64_t offset, int whence) {
  // Calculate the new requested file offset (the arithmetic is done in 64-bit precision in order to
  // properly handle overflow).
  int64_t target_offset_64;
  switch (whence) {
    case MFAT_SEEK_SET:
      target_offset_64 = offset;
      break;
    case MFAT_SEEK_END:
      target_offset_64 = ((int64_t)f->info.size) + offset;
      break;
    case MFAT_SEEK_CUR:
      target_offset_64 = ((int64_t)f->offset) + offset;
      break;
    default:
      DBGF("Invalid whence: %d", whence);
      return -1;
  }

  // Sanity check the new offset.
  if (target_offset_64 < 0) {
    DBG("Seeking to a negative offset is not allowed");
    return -1;
  }
  if (target_offset_64 > (int64_t)f->info.size) {
    // TODO(m): POSIX lseek() allows seeking beyond the end of the file. We currently don't.
    DBG("Seeking beyond the end of the file is not allowed");
    return -1;
  }

  // This type conversion is safe, since we have verified that the value fits in a 32 bit unsigned
  // value.
  uint32_t target_offset = (uint32_t)target_offset_64;

  // Get partition info.
  mfat_partition_t* part = &s_ctx.partition[f->info.part_no];
  uint32_t bytes_per_cluster = part->blocks_per_cluster * MFAT_BLOCK_SIZE;

  // Define the starting point for the cluster search.
  uint32_t current_cluster = f->current_cluster;
  uint32_t cluster_offset = f->offset - (f->offset % bytes_per_cluster);
  if (target_offset < cluster_offset) {
    // For reverse seeking we need to start from the beginning of the file since FAT uses singly
    // linked lists.
    current_cluster = f->info.first_cluster;
    cluster_offset = 0;
  }

  // Skip along clusters until we find the cluster that contains the requested offset.
  while ((target_offset - cluster_offset) >= bytes_per_cluster) {
    if (_mfat_is_eoc(current_cluster)) {
      DBG("Unexpected cluster access after EOC");
      return -1;
    }

    // Look up the next cluster.
    if (!_mfat_next_cluster(part, &current_cluster)) {
      return -1;
    }
    cluster_offset += bytes_per_cluster;
  }

  // Update the current offset in the file descriptor.
  f->offset = target_offset;
  f->current_cluster = current_cluster;

  return (int64_t)target_offset;
}

//--------------------------------------------------------------------------------------------------
// Public API functions.
//--------------------------------------------------------------------------------------------------

int mfat_mount(mfat_read_block_fun_t read_fun, mfat_write_block_fun_t write_fun, void* custom) {
#if MFAT_ENABLE_WRITE
  if (read_fun == NULL || write_fun == NULL) {
#else
  if (read_fun == NULL) {
#endif
    DBG("Bad function pointers");
    return -1;
  }

  // Clear the context state.
  memset(&s_ctx, 0, sizeof(mfat_ctx_t));
  s_ctx.read = read_fun;
#if MFAT_ENABLE_WRITE
  s_ctx.write = write_fun;
#else
  (void)write_fun;
#endif
  s_ctx.custom = custom;
  s_ctx.active_partition = -1;

#if MFAT_NUM_CACHED_BLOCKS > 1
  // Initialize the block cache priority queues.
  for (int j = 0; j < MFAT_NUM_CACHES; ++j) {
    mfat_cache_t* cache = &s_ctx.cache[j];
    for (int i = 0; i < MFAT_NUM_CACHED_BLOCKS; ++i) {
      // Assign initial items to the priority queue.
      cache->pri[i] = i;
    }
  }
#endif

  // Read the partition tables.
  if (!_mfat_decode_partition_tables()) {
    return -1;
  }

  // Find the first bootable partition. If no bootable partition is found, pick the first supported
  // partition.
  int first_boot_partition = -1;
  for (int i = 0; i < MFAT_NUM_PARTITIONS; ++i) {
    if (s_ctx.partition[i].type != MFAT_TYPE_UNKNOWN) {
      if (s_ctx.partition[i].boot && first_boot_partition < 0) {
        first_boot_partition = i;
        s_ctx.active_partition = i;
      } else if (s_ctx.active_partition < 0) {
        s_ctx.active_partition = i;
      }
    }
  }
  if (s_ctx.active_partition < 0) {
    return -1;
  }
  DBGF("Selected partition %d", s_ctx.active_partition);

  // MFAT is now initizlied.
  DBG("Successfully initialized");
  s_ctx.initialized = true;

  return 0;
}

void mfat_unmount(void) {
#if MFAT_ENABLE_WRITE
  // Flush any pending writes.
  _mfat_sync_impl();
#endif
  s_ctx.initialized = false;
}

int mfat_select_partition(int partition_no) {
  if (!s_ctx.initialized) {
    DBG("Not initialized");
    return -1;
  }
  if (partition_no < 0 || partition_no >= MFAT_NUM_PARTITIONS) {
    DBGF("Bad partition number: %d", partition_no);
    return -1;
  }
  if (s_ctx.partition[partition_no].type == MFAT_TYPE_UNKNOWN) {
    DBG("Unsupported partition type");
    return -1;
  }

  s_ctx.active_partition = partition_no;

  return 0;
}

void mfat_sync(void) {
#if MFAT_ENABLE_WRITE
  if (!s_ctx.initialized) {
    DBG("Not initialized");
    return;
  }

  _mfat_sync_impl();
#endif
}

int mfat_fstat(int fd, mfat_stat_t* stat) {
  if (!s_ctx.initialized) {
    DBG("Not initialized");
    return -1;
  }

  mfat_file_t* f = _mfat_get_file(fd);
  if (f == NULL) {
    return -1;
  }

  return _mfat_fstat_impl(&f->info, stat);
}

int mfat_stat(const char* path, mfat_stat_t* stat) {
  if (!s_ctx.initialized || s_ctx.active_partition < 0) {
    DBG("Not initialized");
    return -1;
  }
  if (path == NULL || stat == NULL) {
    return -1;
  }

  return _mfat_stat_impl(path, stat);
}

int mfat_open(const char* path, int oflag) {
  if (!s_ctx.initialized || s_ctx.active_partition < 0) {
    DBG("Not initialized");
    return -1;
  }
  if (path == NULL || ((oflag & MFAT_O_RDWR) == 0)) {
    return -1;
  }

  return _mfat_open_impl(path, oflag);
}

int mfat_close(int fd) {
  if (!s_ctx.initialized) {
    DBG("Not initialized");
    return -1;
  }

  mfat_file_t* f = _mfat_get_file(fd);
  if (f == NULL) {
    return -1;
  }

  return _mfat_close_impl(f);
}

int64_t mfat_read(int fd, void* buf, uint32_t nbyte) {
  if (!s_ctx.initialized) {
    DBG("Not initialized");
    return -1;
  }

  mfat_file_t* f = _mfat_get_file(fd);
  if (f == NULL) {
    return -1;
  }

  return _mfat_read_impl(f, (uint8_t*)buf, nbyte);
}

int64_t mfat_write(int fd, const void* buf, uint32_t nbyte) {
#if MFAT_ENABLE_WRITE
  if (!s_ctx.initialized) {
    DBG("Not initialized");
    return -1;
  }

  mfat_file_t* f = _mfat_get_file(fd);
  if (f == NULL) {
    return -1;
  }

  return _mfat_write_impl(f, (const uint8_t*)buf, nbyte);
#else
  return -1;
#endif
}

int64_t mfat_lseek(int fd, int64_t offset, int whence) {
  if (!s_ctx.initialized) {
    DBG("Not initialized");
    return -1;
  }

  mfat_file_t* f = _mfat_get_file(fd);
  if (f == NULL) {
    return -1;
  }

  return _mfat_lseek_impl(f, offset, whence);
}
