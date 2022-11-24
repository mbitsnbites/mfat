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

#include <mfat.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int blkread(char* ptr, unsigned block_no, void* custom) {
  int fd = *(int*)custom;
  if (lseek(fd, MFAT_BLOCK_SIZE * (size_t)block_no, SEEK_SET) == -1) {
    return -1;
  }
  size_t num_bytes = read(fd, ptr, MFAT_BLOCK_SIZE);
  return (num_bytes != MFAT_BLOCK_SIZE) && (num_bytes != 0) ? -1 : 0;
}

static int blkwrite(const char* ptr, unsigned block_no, void* custom) {
  int fd = *(int*)custom;
  if (lseek(fd, MFAT_BLOCK_SIZE * (size_t)block_no, SEEK_SET) == -1) {
    return -1;
  }
  size_t num_bytes = write(fd, ptr, MFAT_BLOCK_SIZE);
  return (num_bytes != MFAT_BLOCK_SIZE) && (num_bytes != 0) ? -1 : 0;
}

int main(int argc, char** argv) {
  // Get arguments.
  if (argc != 3) {
    printf("Usage: %s FATIMAGE DIR\n", argv[0]);
    return 1;
  }
  const char* img_path = argv[1];
  const char* dir_name = argv[2];

  // Open the FAT image file or device.
  int img_fd = open(img_path, O_RDONLY);
  if (img_fd == -1) {
    fprintf(stderr, "*** Failed to open the FAT image\n");
    return 1;
  }

  // Mount the image in MFAT.
  if (mfat_mount(blkread, blkwrite, &img_fd) == -1) {
    close(img_fd);
    fprintf(stderr, "*** Failed to init MFAT\n");
    return 1;
  }

  // Show the files in the directory.
  mfat_dir_t* dirp = mfat_opendir(dir_name);
  if (dirp != NULL) {
    mfat_dirent_t* dirent;
    while ((dirent = mfat_readdir(dirp)) != NULL) {
      // Construct full path.
      char path[256];
      snprintf(&path[0], 255, "%s/%s", dir_name, dirent->d_name);
      path[255] = 0;

      // Stat and print file info + name.
      mfat_stat_t stat;
      if (mfat_stat(path, &stat) != -1) {
        printf("%u-%02u-%02u %02u:%02u:%02u\t%s\t%u\t%s\n",
               stat.st_mtim.year,
               stat.st_mtim.month,
               stat.st_mtim.day,
               stat.st_mtim.hour,
               stat.st_mtim.minute,
               stat.st_mtim.second,
               MFAT_S_ISDIR(stat.st_mode) ? "<DIR>" : "",
               stat.st_size,
               dirent->d_name);
      } else {
        fprintf(stderr, "*** Failed to stat %s\n", path);
      }
    }
    mfat_closedir(dirp);
  } else {
    fprintf(stderr, "*** Failed to open dir %s\n", dir_name);
  }

  // Unmount and close down.
  mfat_unmount();
  close(img_fd);

  return 0;
}