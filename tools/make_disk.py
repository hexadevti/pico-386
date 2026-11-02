# make_disk.py -- empty, formatted disk images for pico-386, made on a computer.
#
#   python tools/make_disk.py hdd 128 c.img       hard disk: MBR + one FAT16 partition, 1-504 MB
#   python tools/make_disk.py floppy 1440 a.img   floppy: FAT12, 360/720/1200/1440/2880 KB
#
# The hard disk is laid out exactly as the home menu's "Create a hard disk" lays it out
# (pc_disks.cpp makeHardDisk): 16 heads, 63 sectors, the partition at LBA 63, DOS FORMAT's cluster
# sizes, and boot code that says the disk has no DOS yet. A floppy's boot code says the same.
import struct
import sys

# pc_disks.cpp kMbrCode: finds the active partition and boots it
MBR_CODE = bytes([
    0xfa, 0x31, 0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0x8e, 0xd8, 0x8e, 0xc0, 0xfb, 0xfc, 0xbe, 0x00,
    0x7c, 0xbf, 0x00, 0x06, 0xb9, 0x00, 0x01, 0xf3, 0xa5, 0xea, 0x1e, 0x06, 0x00, 0x00, 0xbe, 0xbe,
    0x07, 0xb9, 0x04, 0x00, 0xf6, 0x04, 0x80, 0x75, 0x0a, 0x83, 0xc6, 0x10, 0xe2, 0xf6, 0xbe, 0x91,
    0x06, 0xeb, 0x3a, 0x89, 0xf5, 0x66, 0x8b, 0x44, 0x08, 0x66, 0xa3, 0x89, 0x06, 0xbe, 0x81, 0x06,
    0xb4, 0x42, 0xcd, 0x13, 0x73, 0x15, 0x8a, 0x76, 0x01, 0x8b, 0x4e, 0x02, 0xbb, 0x00, 0x7c, 0xb8,
    0x01, 0x02, 0xcd, 0x13, 0x73, 0x05, 0xbe, 0xa5, 0x06, 0xeb, 0x12, 0x81, 0x3e, 0xfe, 0x7d, 0x55,
    0xaa, 0x75, 0x07, 0x89, 0xee, 0xea, 0x00, 0x7c, 0x00, 0x00, 0xbe, 0xb5, 0x06, 0xac, 0x84, 0xc0,
    0x74, 0x09, 0xb4, 0x0e, 0xbb, 0x07, 0x00, 0xcd, 0x10, 0xeb, 0xf2, 0x31, 0xc0, 0xcd, 0x16, 0xcd,
    0x19, 0x10, 0x00, 0x01, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00]) + b"No active partition\0Disk read error\0Partition not bootable\0"

# pc_disks.cpp kVbrCode, at 3Eh: prints the message that follows it (at 7C61h), waits for a key and
# INT 19h. The message is ours to pick.
VBR_CODE = bytes([
    0xfa, 0x31, 0xc0, 0x8e, 0xd8, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0xfb, 0xfc, 0xbe, 0x61, 0x7c, 0xac,
    0x84, 0xc0, 0x74, 0x09, 0xb4, 0x0e, 0xbb, 0x07, 0x00, 0xcd, 0x10, 0xeb, 0xf2, 0x31, 0xc0, 0xcd,
    0x16, 0xcd, 0x19])
HDD_MSG = b"\r\nThis hard disk has no DOS yet.\r\nBoot a DOS floppy (A:) and install DOS on C:.\r\n" \
          b"Press a key to try again.\r\n\0"
FLOPPY_MSG = b"\r\nThis floppy has no DOS.\r\nPress a key to try again.\r\n\0"

HEADS, SPT = 16, 63

# size in KB: (sectors per track, heads, sectors per cluster, root entries, FAT sectors, media)
FLOPPIES = {
    360: (9, 2, 2, 112, 2, 0xFD),
    720: (9, 2, 2, 112, 3, 0xF9),
    1200: (15, 2, 1, 224, 7, 0xF9),
    1440: (18, 2, 1, 224, 9, 0xF0),
    2880: (36, 2, 2, 240, 9, 0xF0),
}


def chs(lba):
    c, h, s = lba // (HEADS * SPT), lba // SPT % HEADS, lba % SPT + 1
    return bytes([h, s | ((c >> 2) & 0xC0), c & 0xFF])


def boot_sector(bpb, msg):
    s = bytearray(512)
    s[0:3] = b"\xEB\x3C\x90"
    s[3:11] = b"PICO386 "
    s[11:62] = bpb
    code = VBR_CODE + msg
    s[62:62 + len(code)] = code
    s[510:512] = b"\x55\xAA"
    return s


def bpb(spc, rsvd, nfats, root, total, media, fatsz, spt, heads, hidden, drive, serial, fstype):
    return (struct.pack("<HBHBHHBHHHII", 512, spc, rsvd, nfats, root,
                        total if total < 65536 else 0, media, fatsz, spt, heads, hidden,
                        0 if total < 65536 else total)
            + struct.pack("<BBBI", drive, 0, 0x29, serial) + b"NO NAME    " + fstype)


def make_hdd(path, mb):
    if not 1 <= mb <= 504:
        sys.exit("hdd: 1 to 504 MB")
    cyl = mb * 2048 // (HEADS * SPT)
    total = cyl * HEADS * SPT
    start = SPT
    part = total - start
    spc = 2 if part <= 32680 else 4 if part <= 262144 else 8 if part <= 524288 else 16
    rsvd, root, nfats = 1, 512, 2
    tmp2 = 256 * spc + nfats
    fatsz = (part - rsvd - root * 32 // 512 + tmp2 - 1) // tmp2
    sig = (mb * 2654435761) & 0xFFFFFFFF

    mbr = bytearray(512)
    mbr[:len(MBR_CODE)] = MBR_CODE
    mbr[0x1B8:0x1BC] = struct.pack("<I", sig)
    mbr[0x1BE:0x1CE] = (b"\x80" + chs(start) + bytes([0x04 if part < 65536 else 0x06])
                        + chs(total - 1) + struct.pack("<II", start, part))
    mbr[510:512] = b"\x55\xAA"
    vbr = boot_sector(bpb(spc, rsvd, nfats, root, part, 0xF8, fatsz, SPT, HEADS, start, 0x80,
                          sig ^ 0x5A5A1234, b"FAT16   "), HDD_MSG)
    with open(path, "wb") as f:
        f.write(mbr)
        f.seek(start * 512)
        f.write(vbr)
        for i in range(nfats):
            f.seek((start + rsvd + i * fatsz) * 512)
            f.write(b"\xF8\xFF\xFF\xFF")
        f.truncate(total * 512)
    print(f"{path}: {total} sectors ({total * 512 // 1048576} MB), FAT16, "
          f"{spc}-sector clusters")


def make_floppy(path, kb):
    if kb not in FLOPPIES:
        sys.exit("floppy: " + ", ".join(map(str, FLOPPIES)) + " KB")
    spt, heads, spc, root, fatsz, media = FLOPPIES[kb]
    total = kb * 2
    img = bytearray(total * 512)
    img[0:512] = boot_sector(bpb(spc, 1, 2, root, total, media, fatsz, spt, heads, 0, 0x00,
                                 (kb * 2654435761) & 0xFFFFFFFF, b"FAT12   "), FLOPPY_MSG)
    for i in range(2):
        o = (1 + i * fatsz) * 512
        img[o:o + 3] = bytes([media, 0xFF, 0xFF])
    with open(path, "wb") as f:
        f.write(img)
    print(f"{path}: {kb} KB floppy, FAT12")


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("hdd", "floppy"):
        sys.exit("usage: make_disk.py hdd MB path | make_disk.py floppy KB path")
    (make_hdd if sys.argv[1] == "hdd" else make_floppy)(sys.argv[3], int(sys.argv[2]))
