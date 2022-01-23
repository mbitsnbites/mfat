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
    printf("Usage: %s FATIMAGE FILE\n", argv[0]);
    return 1;
  }
  const char* img_path = argv[1];
  const char* file_name = argv[2];

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

  // Print the contents of the file to stdout.
  int fd = mfat_open(file_name, MFAT_O_RDONLY);
  if (fd != -1) {
    uint8_t buf[100];
    while (1) {
      ssize_t bytes_read = mfat_read(fd, &buf[0], sizeof(buf));
      if (bytes_read > 0) {
        ssize_t bytes_written = write(STDOUT_FILENO, &buf[0], bytes_read);
        (void)bytes_written;  // Silence compiler warnings.
      } else {
        // 0 = EOF, -1 = Error
        if (bytes_read == -1) {
          fprintf(stderr, "*** Failed to read %s\n", file_name);
        }
        break;
      }
    }

    // Close the file.
    mfat_close(fd);
  } else {
    fprintf(stderr, "*** Failed to open %s\n", file_name);
  }

  // Unmount and close down.
  mfat_unmount();
  close(img_fd);

  return 0;
}