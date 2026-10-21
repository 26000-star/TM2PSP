"""Reads the file system of a Nintendo DS ROM (NitroFS): the name table (FNT) and the allocation table (FAT).

Header fields used: game code at 0x0C, FNT offset/size at 0x40/0x44, FAT offset/size at 0x48/0x4C. The FNT starts
with one 8-byte entry per directory (sub-table offset u32, first file id u16, parent id u16); a sub-table lists the
directory's entries in file id order: a length byte (0 ends it; bit 7 set = sub-directory, followed by its u16 id
0xF000 + n), then the name. FAT entry i is the start and end offsets of file i in the ROM.

Usage: python -P tools/nitrofs.py <rom.nds> [path ...]   lists the files, or prints the size of the given ones
"""
import struct
import sys


class NitroFS:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0x50)
        if len(head) < 0x50:
            raise ValueError("%s: too short for a DS ROM" % path)
        self.game_code = head[0x0C:0x10].decode("ascii", "replace")
        fnt_off, fnt_size, fat_off, fat_size = struct.unpack_from("<IIII", head, 0x40)
        self.f.seek(fnt_off)
        fnt = self.f.read(fnt_size)
        self.f.seek(fat_off)
        fat = self.f.read(fat_size)
        if len(fnt) != fnt_size or len(fat) != fat_size or fnt_size < 8:
            raise ValueError("%s: truncated or not a DS ROM" % path)
        self.fat = [struct.unpack_from("<II", fat, i) for i in range(0, fat_size - 7, 8)]
        self.files = {}  # "dir/name" -> file id
        self._walk(fnt, 0xF000, "")

    def _walk(self, fnt, dir_id, prefix):
        sub, file_id, _ = struct.unpack_from("<IHH", fnt, (dir_id & 0xFFF) * 8)
        p = sub
        while fnt[p]:
            n = fnt[p] & 0x7F
            name = fnt[p + 1:p + 1 + n].decode("latin-1")
            p += 1 + n
            if fnt[p - 1 - n] & 0x80:
                child = struct.unpack_from("<H", fnt, p)[0]
                p += 2
                self._walk(fnt, child, prefix + name + "/")
            else:
                self.files[prefix + name] = file_id
                file_id += 1

    def read(self, path):
        start, end = self.fat[self.files[path]]
        self.f.seek(start)
        data = self.f.read(end - start)
        if len(data) != end - start:
            raise ValueError("%s: the ROM is truncated" % path)
        return data


def main():
    fs = NitroFS(sys.argv[1])
    for path in sys.argv[2:] or sorted(fs.files):
        start, end = fs.fat[fs.files[path]]
        print("%10d  %s" % (end - start, path))


if __name__ == "__main__":
    main()
