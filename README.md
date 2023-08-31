## ⚠️ This repository has moved to: https://gitlab.com/mbitsnbites/mfat

# MFAT

MFAT is a minimal I/O library for [FAT (File Allocation Table)](https://en.wikipedia.org/wiki/File_Allocation_Table) volumes.

The library has been designed for embedded systems, and its small code and memory footprint makes it suitable for inclusion in firmware/ROM and/or small OS kernels.

## Features

* Works with any storage medium that supports random access block I/O (SD cards, hard drives, raw disk image files, etc).
* Supports both FAT16 and FAT32.
* Supports multiple partitions (both [MBR](https://en.wikipedia.org/wiki/Master_boot_record) and [GPT](https://en.wikipedia.org/wiki/GUID_Partition_Table) partition tables are supported).
* Cached I/O (configurable cache size).
* Small memory footprint.
* No dynamic memory allocation (only static/BSS).
* Configurable to tune code and memory requirements.
* Completely dependency-free.
* Implemented in portable C99.
* Easy to integrate into your project (only two files are needed: mfat.h and mfat.c).
* Familiar and easy-to-use POSIX-like API.
* Liberal license ([BSD-2-Clause](https://opensource.org/licenses/BSD-2-Clause)).

## Limitations

The internal MFAT context is statically allocated, meaning:

* Only a single device may be mounted at any time.
* The API is not thread safe.

Also: **MFAT is still work-in-progress**.

* Writing is not supported yet.
* Long file names are not supported yet.

## POSIX compatibility

The API of this library is modelled after the [POSIX.1-2017](https://pubs.opengroup.org/onlinepubs/9699919799/) file I/O C API:s.

These are the MFAT functions that are inspired by POSIX functions:

| MFAT function | POSIX prototype |
| --- | --- |
| `mfat_close()` | [`close()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/close.html) |
| `mfat_closedir()` | [`closedir()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/closedir.html) |
| `mfat_fdopendir()` | [`fdopendir()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/fdopendir.html) |
| `mfat_fstat()` | [`fstat()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/fstat.html) |
| `mfat_lseek()` | [`lseek()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/lseek.html) |
| `mfat_open()` | [`open()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/open.html) |
| `mfat_opendir()` | [`opendir()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/opendir.html) |
| `mfat_read()` | [`read()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/read.html) |
| `mfat_readdir()` | [`readdir()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/readdir.html) |
| `mfat_stat()` | [`stat()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/stat.html) |
| `mfat_sync()` | [`sync()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/sync.html) |
| `mfat_write()` | [`write()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/write.html) |

Note that the library is not fully POSIX compliant. For instance:

* Names of functions, types, and macros differ (an mfat_/MFAT_ prefix is used).
* Some types are different (e.g. date/time types and sizes/names of integral types).
* Some semantics are slightly different.
* Some functionality is missing.
* errno is not supported.

While the library itself is not fully POSIX compliant, it is suitable as a low level I/O implementation layer for higher level libraries, such as [newlib](https://sourceware.org/newlib/).

It is also easy to modify existing programs that use POSIX I/O routines to use the MFAT library instead.

## FAT Documentation

[FAT.md](FAT.md)
