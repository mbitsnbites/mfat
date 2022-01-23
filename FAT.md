# FAT documentation

This document summarizes some aspects of the FAT file system, and acts as a reference for the implementation.

The following sources of information were used during the development of MFAT:

* [FAT32 File System Specification, FAT: General Overview of On-Disk Format](https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/fatgen103.doc)
* [Wikipedia: Master boot record](https://en.wikipedia.org/wiki/Master_boot_record)
* [Wikipedia: GUID Partition Table](https://en.wikipedia.org/wiki/GUID_Partition_Table)
* [Application Note for Fat16 Interface for MSP430](https://teslabs.com/openplayer/docs/docs/prognotes/appnote_fat16.pdf)
* [FAT32 Structure Information - MBR, FAT32 Boot Sector Introduction](https://www.easeus.com/resource/fat32-disk-structure.htm)

## Terminology

* Block - One block of data. Usually 512 bytes. Possibly 1024, 2048 or 4096 bytes.
* Sector - Term used interchangeably with "block".
* Cluster - An integer multiple of blocks. Files are divided into clusters. One cluster belongs to one file.

## Master Boot Record (MBR)

The Master Boot Record, if present, is the first 512-byte block of the storage medium.

**Note**: Some storage media may be formatted without an MBR. In this case, there is only a single FAT partition, and the first block is the BIOS Parameter Block.

The layout of the MBR is as follows:

| Offset | Size | Description |
| --- | --- | --- |
| 0 | 446 | Boot code (usually 8086 code, unused) |
| 446 | 16 | Partition entry 0 (see below) |
| 462 | 16 | Partition entry 1 (--"--) |
| 478 | 16 | Partition entry 2 (--"--) |
| 494 | 16 | Partition entry 3 (--"--) |
| 510 | 2 | MBR signature: `[0x55, 0xaa]` |

The layout of each Partition entry is as follows:

| Offset | Size | Description |
| --- | --- | --- |
| 0 | 1 | Status (bit 7 = 1 indicates "active"/bootable) |
| 1 | 3 | CHS addr. of first block in partition (unused) |
| 4 | 1 | Partition type |
| 5 | 3 | CHS addr. of last block in partition (unused) |
| 8 | 4 | LBA of first block in partition (location of the BPB) |
| 12 | 4 | Number of blocks in partition |

## BIOS Parameter Block (BPB)

The BIOS Parameter Block, which is sometimes also refered to as the "Boot Record", is the first block of the partition.

There are essentially two different version of the BPB: One for FAT16 and one for FAT32. The first 36 bytes have the same layout for FAT16 and FAT32:

| Offset | Size | Description |
| --- | --- | --- |
| 0 | 3 | Jump code (`[0xEB,0x??,0x90]` or `[0xE9,0x??,0x??]`) |
| 3 | 8 | OEM name (unused) |
| 11 | 2 | Bytes per sector (512, 1024, 2048 or 4096) |
| 13 | 1 | Sectors per cluster (1, 2, 4, 8, 16, 32, 64 or 128) |
| 14 | 2 | Reserved sectors |
| 16 | 1 | Number of copies of FAT (set to 2) |
| 17 | 2 | Number of root directory entries (valid for FAT16, zero for FAT32) |
| 19 | 2 | Number of sectors in partition smaller than 32MB (valid for FAT16, zero for FAT32) |
| 21 | 1 | Media descriptor (0xf8 for hard disks) |
| 22 | 2 | Sectors per FAT (valid for FAT16, zero for FAT32) |
| 24 | 2 | Sectors per track |
| 26 | 2 | Number of heads |
| 28 | 4 | Number of hidden sectors in partition |
| 32 | 4 | Number of sectors in partition (valid for FAT32, may be zero for FAT16) |

### FAT16 BPB (offset 36-511)

| Offset | Size | Description |
| --- | --- | --- |
| 36 | 1 | Logical drive number of partition |
| 37 | 1 | Reserved (set to zero)) |
| 38 | 1 | Extended boot signature: If 0x29 the following three fields are present |
| 39 | 4 | Volume ID (serial number) of partition |
| 43 | 11 | Volume label of partition |
| 54 | 8 | FAT name ("FAT16") |
| 62 | 448 | Executable code (unused) |
| 510 | 2 | Executable marker: `[0x55, 0xaa]` |

### FAT32 BPB (offset 36-511)

| Offset | Size | Description |
| --- | --- | --- |
| 36 | 4 | Number of sectors per FAT |
| 40 | 2 | ExFlags (see below) |
| 42 | 2 | Version of FAT32 drive: `[major, minor]` |
| 44 | 4 | Cluster number of the start of the root directory |
| 48 | 2 | Sector number (relative to start of partition) of the FileSystem Information Sector (see below) |
| 50 | 2 | Sector number (relative to start of partition) of the Backup Boot Sector |
| 52 | 12 | Reserved (set to zero) |
| 64 | 1 | Logical drive number of partition |
| 65 | 1 | Reserved (set to zero) |
| 66 | 1 | Extended boot signature: If 0x29 the following three fields are present |
| 67 | 4 | Volume ID (serial number) of partition |
| 71 | 11 | Volume label of partition |
| 82 | 8 | FAT name ("FAT32") |
| 90 | 420 | Executable code (unused) |
| 510 | 2 | Executable marker: `[0x55, 0xaa]` |

ExFlags:

| Bits | Description |
| --- | --- |
| 0-3 | Zero-based number of active FAT (only valid if mirroring is disabled) |
| 4 | Reserved |
| 7 | Disable FAT mirroring if 1: The FAT information is only written to the copy indicated by bits 0-3 |

## Directory Entry Structure

| Offset | Size | Description |
| --- | --- | --- |
| 0 | 11 | Short name |
| 11 | 1 | File attributes (see below) |
| 12 | 1 | Reserved (set to zero) |
| 13 | 1 | Creation time stamp, milliseconds (optional) |
| 14 | 6 | Reserved (?) |
| 20 | 2 | High word of cluster number (zero for FAT16) |
| 22 | 2 | Time of last write |
| 24 | 2 | Date of last write |
| 26 | 2 | Low word of cluster number |
| 28 | 4 | File size in bytes |

File attributes:

| Name | Value |
| --- | --- |
| ATTR_READ_ONLY | 0x01 |
| ATTR_HIDDEN | 0x02 |
| ATTR_SYSTEM | 0x04 |
| ATTR_VOLUME_ID | 0x08 |
| ATTR_DIRECTORY | 0x10 |
| ATTR_ARCHIVE | 0x20 |
| ATTR_LONG_NAME | 0x0f |
