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

//--------------------------------------------------------------------------------------------------
// MFAT - A minimal I/O library for FAT (File Allocation Table) volumes
//
// The API of this library is modelled after the POSIX.1-2017 file I/O C API:s (but it is not fully
// POSIX compliant).
//--------------------------------------------------------------------------------------------------

#ifndef MFAT_H_
#define MFAT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Block size.
#define MFAT_BLOCK_SIZE 512

// Flags for mfat_open().
#define MFAT_O_RDONLY 1
#define MFAT_O_WRONLY 2
#define MFAT_O_RDWR (MFAT_O_RDONLY | MFAT_O_WRONLY)
#define MFAT_O_APPEND 4
#define MFAT_O_CREAT 8
#define MFAT_O_DIRECTORY 16

// Whence values for mfat_lseek().
#define MFAT_SEEK_SET 0  ///< The offset is set to offset bytes.
#define MFAT_SEEK_CUR 1  ///< The offset is set to its current location plus offset bytes.
#define MFAT_SEEK_END 2  ///< The offset is set to the size of the file plus offset bytes.

// Values for mfat_stat_t.st_mode (OR:able bits).
#define MFAT_S_IFREG 0x8000  ///< Regular file.
#define MFAT_S_IFDIR 0x4000  ///< Directory.
#define MFAT_S_IRUSR 0x0100  ///< R for owner.
#define MFAT_S_IWUSR 0x0080  ///< W for owner.
#define MFAT_S_IXUSR 0x0040  ///< X for owner.
#define MFAT_S_IRGRP 0x0020  ///< R for group (same as MFAT_S_IRUSR).
#define MFAT_S_IWGRP 0x0010  ///< W for group (same as MFAT_S_IWUSR).
#define MFAT_S_IXGRP 0x0008  ///< X for group (same as MFAT_S_IXUSR).
#define MFAT_S_IROTH 0x0004  ///< R for other (same as MFAT_S_IRUSR).
#define MFAT_S_IWOTH 0x0002  ///< W for other (same as MFAT_S_IWUSR).
#define MFAT_S_IXOTH 0x0001  ///< X for other (same as MFAT_S_IXUSR).

// Macros for decoding the st_mode field of mfat_stat_t.
#define MFAT_S_ISREG(m) (((m)&MFAT_S_IFREG) != 0U)
#define MFAT_S_ISDIR(m) (((m)&MFAT_S_IFDIR) != 0U)

// The values of this struct are compatible with struct tm in <time.h>, so it is easy to convert the
// date/time to other representations using mktime(), for instance.
typedef struct {
  uint16_t year;   ///< Year (1980-2235).
  uint8_t month;   ///< Month of year (1-12).
  uint8_t day;     ///< Day of month (1-31).
  uint8_t hour;    ///< Hour of day (0-23).
  uint8_t minute;  ///< Minute of hour (0-59).
  uint8_t second;  ///< Second of minute (0-59).
} mfat_time_t;

typedef struct {
  uint32_t st_mode;     ///< File permission bits plus MFAT_S_IFREG or MFAT_S_IFDIR.
  uint32_t st_size;     ///< Size in bytes.
  mfat_time_t st_mtim;  ///< Modification time.
} mfat_stat_t;

/// @brief Block reader function pointer.
/// @param ptr Pointer to the buffer to read to.
/// @param block_no The block to read (relative to start of the storage medium).
/// @param custom The custom data pointer that was passed to mfat_init().
/// @returns zero (0) on success, or -1 on failure.
typedef int (*mfat_read_block_fun_t)(char* ptr, unsigned block_no, void* custom);

/// @brief Block writer function pointer.
/// @param ptr Pointer to the buffer to write from.
/// @param block_no The block to write (relative to start of the storage medium).
/// @param custom The custom data pointer that was passed to mfat_init().
/// @returns zero (0) on success, or -1 on failure.
typedef int (*mfat_write_block_fun_t)(const char* ptr, unsigned block_no, void* custom);

/// @brief Mount FAT volumes.
///
/// The provided read and write functions implement access to the storage medium, and the optional
/// custom data pointer can be used by these functions for keeping track of necessary state.
///
/// Before the function returns, it will identify and "mount" all FAT volumes on the storage medium.
/// If no FAT volumes are found, the function will return -1 (indicating failure).
///
/// This function needs to be called before calling any other library functions.
/// @param read_fun A block reader function pointer.
/// @param write_fun A block writer function pointer.
/// @param custom A custom data handle that is passed to the read/write functions (may be NULL).
/// @returns zero (0) on success, or -1 on failure.
int mfat_mount(mfat_read_block_fun_t read_fun, mfat_write_block_fun_t write_fun, void* custom);

/// @brief Unmount all FAT volumes.
///
/// Any pending write operations will be flushed to the storage medium.
void mfat_unmount(void);

/// @brief Select which partition to use.
/// @param partition_no The partition number.
/// @returns zero (0) on success, or -1 on failure.
int mfat_select_partition(int partition_no);

/// @brief Flush pending data updates to storage.
void mfat_sync(void);

/// @brief Obtain information about a open file.
/// @param fd The file descriptor.
/// @param stat Pointer to a stat structure into which information is placed concerning the file.
/// @returns zero (0) on success, or -1 on failure.
int mfat_fstat(int fd, mfat_stat_t* stat);

/// @brief Obtain information about a file.
/// @param path The path to the file.
/// @param stat Pointer to a stat structure into which information is placed concerning the file.
/// @returns zero (0) on success, or -1 on failure.
int mfat_stat(const char* path, mfat_stat_t* stat);

/// @brief Open a file.
/// @param path The path to the file.
/// @param oflag The open flags (OR of MFAT_O_* flags).
/// @returns a non-negative integer representing the lowest numbered unused file descriptor, or -1
/// on failure.
/// @note Be aware that valid file descriptors are in the range 0..N-1, where N is the maximum
/// number of file descriptors (defined at compile time). This is in contrast to most POSIX
/// systems where 0, 1 and 2 are usually reserved for stdin, stdout and stderr, respectively.
int mfat_open(const char* path, int oflag);

/// @brief Close a file descriptor.
/// @param fd The file descriptor.
/// @returns zero (0) on success, or -1 on failure.
int mfat_close(int fd);

/// @brief Read from a file.
/// @param fd The file descriptor.
/// @param buf Buffer to read data into.
/// @param nbyte Number of bytes to read.
/// @returns a non-negative integer indicating the number of bytes actually read if the operation
/// was succesful, zero (0) if the seek offset was at the end of the file when the function was
/// called, or -1 on failure.
int64_t mfat_read(int fd, void* buf, uint32_t nbyte);

/// @brief Write to a file.
/// @param fd The file descriptor.
/// @param buf Buffer that contains the data to write.
/// @param nbyte Number of bytes to write.
/// @returns a non-negative integer indicating the number of bytes actually written if the operation
/// was succesful, or -1 on failure.
int64_t mfat_write(int fd, const void* buf, uint32_t nbyte);

/// @brief Reposition read/write file offset.
/// @param fd The file descriptor.
/// @param offset The offset.
/// @param whence How the offset is interpreted.
/// @returns the resulting offset location as measured in bytes from the beginning of the file if
/// the operation was successful, or -1 on failure.
/// @note It is possible to query the current file position with mfat_lseek(fd, 0, MFAT_SEEK_CUR).
int64_t mfat_lseek(int fd, int64_t offset, int whence);

#ifdef __cplusplus
}
#endif

#endif  // MFAT_H_
