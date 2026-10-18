/* Minimal ZIP reader with the API of Gilles Vollant's unzip 0.15 as used by the game (main 0x020211e8..0x02023ac0),
 * reading the archive through the NitroSDK FS shim and inflating with zlib.
 *
 * This is a compatible re-implementation of the subset the game calls, not the original source:
 *   unzOpen, unzClose, unzGetGlobalInfo, unzGoToFilePos, unzGetCurrentFileInfo, unzOpenCurrentFile,
 *   unzReadCurrentFile, unzCloseCurrentFile
 * plus Firebrand's additions:
 *   unzGetOffsetCentralDir (0x02022f34), unzDup (0x0202367c), unzFreeDup (0x02023620).
 * Stored and deflated entries are supported; no encryption, no multi-disk, no ZIP64. */
#ifndef TM_UNZIP_H
#define TM_UNZIP_H

#include "nitro.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UNZ_OK 0
#define UNZ_END_OF_LIST_OF_FILE (-100)
#define UNZ_EOF 0
#define UNZ_PARAMERROR (-102)
#define UNZ_BADZIPFILE (-103)
#define UNZ_INTERNALERROR (-104)
#define UNZ_CRCERROR (-105)
#define UNZ_ERRNO (-1)

typedef struct unz_s *unzFile;

typedef struct unz_global_info {
    u32 number_entry;
    u32 size_comment;
} unz_global_info;

typedef struct unz_file_info {
    u32 version;
    u32 version_needed;
    u32 flag;
    u32 compression_method;
    u32 dosDate;
    u32 crc;
    u32 compressed_size;
    u32 uncompressed_size;
    u32 size_filename;
    u32 size_file_extra;
    u32 size_file_comment;
    u32 disk_num_start;
    u32 internal_fa;
    u32 external_fa;
} unz_file_info;

/* 0x02023698: opens a ZIP file through FS_OpenFile. */
unzFile unzOpen(const char *path);
/* 0x0202364c */
int unzClose(unzFile file);
int unzGetGlobalInfo(unzFile file, unz_global_info *info);
/* 0x02022f34: start of the central directory (the game's vfs.bin offsets are relative to it). */
u32 unzGetOffsetCentralDir(unzFile file);
/* 0x02023348: selects the entry whose central directory record is at pos_in_central_dir (absolute offset)
 * and whose index is num_file. Same as minizip's unzGoToFilePos. */
int unzGoToFilePos(unzFile file, u32 pos_in_central_dir, u32 num_file);
/* 0x02023388 (via 0x020233b4) */
int unzGetCurrentFileInfo(unzFile file, unz_file_info *info, char *filename, u32 filename_size, void *extra,
                          u32 extra_size, char *comment, u32 comment_size);
/* 0x020230f4 */
int unzOpenCurrentFile(unzFile file);
/* 0x02022f8c: returns the number of bytes read, 0 at the end, < 0 on error. */
int unzReadCurrentFile(unzFile file, void *buf, u32 len);
/* 0x02022f38 */
int unzCloseCurrentFile(unzFile file);
/* 0x0202367c: copy of an open archive with its own file cursor and current entry (one per open VFS file).
 * The game copies a global state; the port passes the source handle explicitly. */
unzFile unzDup(unzFile src);
/* 0x02023620: releases a copy made by unzDup without closing the shared archive. */
int unzFreeDup(unzFile file);

#ifdef __cplusplus
}
#endif

#endif
