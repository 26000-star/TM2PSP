/* See unzip.h. ZIP structures: PKWARE APPNOTE (local header 0x04034b50, central record 0x02014b50, EOCD 0x06054b50). */
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "unzip.h"

#define READ_BUFFER_SIZE 16384
#define CENTRAL_RECORD_SIZE 46
#define LOCAL_HEADER_SIZE 30

typedef struct file_in_zip {
    z_stream stream;
    int stream_initialised;
    u8 buffer[READ_BUFFER_SIZE];
    u32 pos_in_zipfile;          /* next compressed byte to read */
    u32 rest_read_compressed;
    u32 rest_read_uncompressed;
    u32 compression_method;
    u32 crc32_wait;
    u32 crc32;
} file_in_zip;

struct unz_s {
    FSFile file;
    int owns_file;               /* FALSE for copies made by unzDup */
    unz_global_info gi;
    u32 offset_central_dir;
    u32 size_central_dir;
    u32 num_file;
    u32 pos_in_central_dir;
    int current_file_ok;
    unz_file_info cur_file_info;
    u32 cur_local_header_offset;
    file_in_zip *pfile;
};

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static int read_at(FSFile *f, u32 pos, void *dst, u32 len)
{
    if (!FS_SeekFile(f, (s32)pos, FS_SEEK_SET))
        return 0;
    return FS_ReadFile(f, dst, (s32)len) == (s32)len;
}

/* Finds the end-of-central-directory record in the last 64 KiB + 22 bytes. */
static int locate_eocd(FSFile *f, u32 *eocd_pos)
{
    u32 size = FS_GetLength(f);
    u32 back = size < 0x10000 + 22 ? size : 0x10000 + 22;
    u8 *buf = (u8 *)malloc(back);
    int found = 0;
    if (!buf)
        return 0;
    if (read_at(f, size - back, buf, back)) {
        for (u32 i = back - 22 + 1; i-- > 0;) {
            if (rd32(buf + i) == 0x06054b50) {
                *eocd_pos = size - back + i;
                found = 1;
                break;
            }
        }
    }
    free(buf);
    return found;
}

static int read_current_info(struct unz_s *s)
{
    u8 h[CENTRAL_RECORD_SIZE];
    unz_file_info *fi = &s->cur_file_info;
    if (!read_at(&s->file, s->pos_in_central_dir, h, sizeof h) || rd32(h) != 0x02014b50)
        return UNZ_BADZIPFILE;
    fi->version = rd16(h + 4);
    fi->version_needed = rd16(h + 6);
    fi->flag = rd16(h + 8);
    fi->compression_method = rd16(h + 10);
    fi->dosDate = rd32(h + 12);
    fi->crc = rd32(h + 16);
    fi->compressed_size = rd32(h + 20);
    fi->uncompressed_size = rd32(h + 24);
    fi->size_filename = rd16(h + 28);
    fi->size_file_extra = rd16(h + 30);
    fi->size_file_comment = rd16(h + 32);
    fi->disk_num_start = rd16(h + 34);
    fi->internal_fa = rd16(h + 36);
    fi->external_fa = rd32(h + 38);
    s->cur_local_header_offset = rd32(h + 42);
    return UNZ_OK;
}

/* @port main:02023698 lib */
unzFile unzOpen(const char *path)
{
    struct unz_s *s = (struct unz_s *)calloc(1, sizeof *s);
    u8 e[22];
    u32 eocd;
    if (!s)
        return NULL;
    FS_InitFile(&s->file);
    if (!FS_OpenFile(&s->file, path)) {
        free(s);
        return NULL;
    }
    s->owns_file = TRUE;
    if (!locate_eocd(&s->file, &eocd) || !read_at(&s->file, eocd, e, sizeof e) || rd16(e + 4) || rd16(e + 6)) {
        FS_CloseFile(&s->file);
        free(s);
        return NULL;
    }
    s->gi.number_entry = rd16(e + 10);
    s->gi.size_comment = rd16(e + 20);
    s->size_central_dir = rd32(e + 12);
    s->offset_central_dir = rd32(e + 16);
    s->num_file = 0;
    s->pos_in_central_dir = s->offset_central_dir;
    s->current_file_ok = read_current_info(s) == UNZ_OK;
    return s;
}

/* @port main:0202364c lib */
int unzClose(unzFile s)
{
    if (!s)
        return UNZ_PARAMERROR;
    if (s->pfile)
        unzCloseCurrentFile(s);
    if (s->owns_file)
        FS_CloseFile(&s->file);
    free(s);
    return UNZ_OK;
}

int unzGetGlobalInfo(unzFile s, unz_global_info *info)
{
    if (!s)
        return UNZ_PARAMERROR;
    *info = s->gi;
    return UNZ_OK;
}

/* @port main:02022f34 lib */
u32 unzGetOffsetCentralDir(unzFile s)
{
    return s->offset_central_dir;
}

/* @port main:02023348 lib */
int unzGoToFilePos(unzFile s, u32 pos_in_central_dir, u32 num_file)
{
    int err;
    if (!s)
        return UNZ_PARAMERROR;
    s->pos_in_central_dir = pos_in_central_dir;
    s->num_file = num_file;
    err = read_current_info(s);
    s->current_file_ok = err == UNZ_OK;
    return err;
}

/* @port main:02023388 lib */
int unzGetCurrentFileInfo(unzFile s, unz_file_info *info, char *filename, u32 filename_size, void *extra,
                          u32 extra_size, char *comment, u32 comment_size)
{
    u32 p, n;
    if (!s)
        return UNZ_PARAMERROR;
    if (!s->current_file_ok)
        return UNZ_END_OF_LIST_OF_FILE;
    if (info)
        *info = s->cur_file_info;
    p = s->pos_in_central_dir + CENTRAL_RECORD_SIZE;
    if (filename && filename_size) {
        n = s->cur_file_info.size_filename < filename_size - 1 ? s->cur_file_info.size_filename : filename_size - 1;
        if (!read_at(&s->file, p, filename, n))
            return UNZ_ERRNO;
        filename[n] = 0;
    }
    p += s->cur_file_info.size_filename;
    if (extra && extra_size) {
        n = s->cur_file_info.size_file_extra < extra_size ? s->cur_file_info.size_file_extra : extra_size;
        if (!read_at(&s->file, p, extra, n))
            return UNZ_ERRNO;
    }
    p += s->cur_file_info.size_file_extra;
    if (comment && comment_size) {
        n = s->cur_file_info.size_file_comment < comment_size - 1 ? s->cur_file_info.size_file_comment : comment_size - 1;
        if (!read_at(&s->file, p, comment, n))
            return UNZ_ERRNO;
        comment[n] = 0;
    }
    return UNZ_OK;
}

/* @port main:020230f4 lib */
int unzOpenCurrentFile(unzFile s)
{
    u8 h[LOCAL_HEADER_SIZE];
    file_in_zip *f;
    if (!s || !s->current_file_ok)
        return UNZ_PARAMERROR;
    if (s->pfile)
        unzCloseCurrentFile(s);
    if (!read_at(&s->file, s->cur_local_header_offset, h, sizeof h) || rd32(h) != 0x04034b50)
        return UNZ_BADZIPFILE;
    if (s->cur_file_info.compression_method != 0 && s->cur_file_info.compression_method != Z_DEFLATED)
        return UNZ_BADZIPFILE;
    f = (file_in_zip *)calloc(1, sizeof *f);
    if (!f)
        return UNZ_INTERNALERROR;
    f->compression_method = s->cur_file_info.compression_method;
    f->crc32_wait = s->cur_file_info.crc;
    f->crc32 = (u32)crc32(0L, Z_NULL, 0);
    f->pos_in_zipfile = s->cur_local_header_offset + LOCAL_HEADER_SIZE + rd16(h + 26) + rd16(h + 28);
    f->rest_read_compressed = s->cur_file_info.compressed_size;
    f->rest_read_uncompressed = s->cur_file_info.uncompressed_size;
    if (f->compression_method == Z_DEFLATED) {
        if (inflateInit2(&f->stream, -MAX_WBITS) != Z_OK) {
            free(f);
            return UNZ_INTERNALERROR;
        }
        f->stream_initialised = 1;
    }
    s->pfile = f;
    return UNZ_OK;
}

/* @port main:02022f8c lib */
int unzReadCurrentFile(unzFile s, void *buf, u32 len)
{
    file_in_zip *f;
    u32 done = 0;
    if (!s || !(f = s->pfile))
        return UNZ_PARAMERROR;
    if (len > f->rest_read_uncompressed)
        len = f->rest_read_uncompressed;
    f->stream.next_out = (Bytef *)buf;
    f->stream.avail_out = len;
    while (f->stream.avail_out > 0) {
        if (f->stream.avail_in == 0 && f->rest_read_compressed > 0) {
            u32 n = f->rest_read_compressed < READ_BUFFER_SIZE ? f->rest_read_compressed : READ_BUFFER_SIZE;
            if (!read_at(&s->file, f->pos_in_zipfile, f->buffer, n))
                return UNZ_ERRNO;
            f->pos_in_zipfile += n;
            f->rest_read_compressed -= n;
            f->stream.next_in = f->buffer;
            f->stream.avail_in = n;
        }
        if (f->compression_method == 0) {
            u32 n = f->stream.avail_in < f->stream.avail_out ? f->stream.avail_in : f->stream.avail_out;
            if (n == 0)
                break;
            memcpy(f->stream.next_out, f->stream.next_in, n);
            f->crc32 = (u32)crc32(f->crc32, f->stream.next_out, n);
            f->stream.next_in += n;
            f->stream.avail_in -= n;
            f->stream.next_out += n;
            f->stream.avail_out -= n;
            f->rest_read_uncompressed -= n;
            done += n;
        } else {
            Bytef *before = f->stream.next_out;
            int err = inflate(&f->stream, Z_SYNC_FLUSH);
            u32 n = (u32)(f->stream.next_out - before);
            f->crc32 = (u32)crc32(f->crc32, before, n);
            f->rest_read_uncompressed -= n;
            done += n;
            if (err == Z_STREAM_END)
                break;
            if (err != Z_OK)
                return UNZ_BADZIPFILE;
            if (n == 0 && f->stream.avail_in == 0 && f->rest_read_compressed == 0)
                break;
        }
    }
    return (int)done;
}

/* @port main:02022f38 lib */
int unzCloseCurrentFile(unzFile s)
{
    file_in_zip *f;
    int err = UNZ_OK;
    if (!s || !(f = s->pfile))
        return UNZ_PARAMERROR;
    if (f->rest_read_uncompressed == 0 && f->crc32 != f->crc32_wait)
        err = UNZ_CRCERROR;
    if (f->stream_initialised)
        inflateEnd(&f->stream);
    free(f);
    s->pfile = NULL;
    return err;
}

/* @port main:0202367c lib */
unzFile unzDup(unzFile src)
{
    struct unz_s *s = (struct unz_s *)malloc(sizeof *s);
    if (!s)
        return NULL;
    *s = *src;
    s->owns_file = FALSE;
    s->pfile = NULL;
    return s;
}

/* @port main:02023620 lib */
int unzFreeDup(unzFile s)
{
    if (!s)
        return UNZ_PARAMERROR;
    if (s->pfile)
        unzCloseCurrentFile(s);
    free(s);
    return UNZ_OK;
}
