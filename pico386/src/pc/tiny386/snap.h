// snap.h -- pico-386's machine snapshots: what each device's *_snap() writes or reads (not upstream).
//
// Every device keeps its state in a struct private to its .c file, so each file gets a *_snap()
// that moves its struct through a SnapIO. Most save the whole struct; on load the fields that
// point into this session (other devices, callbacks, host buffers) are kept, not taken from the
// file. A snapshot is only read back by a build whose struct sizes match (t386_snap_signature).
#ifndef TINY386_SNAP_H
#define TINY386_SNAP_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct SnapIO {
	FILE *f;
	int load;   /* 1: read into the structs, 0: write them out */
	int err;    /* set by the first short read or write; later calls do nothing */
	unsigned want, got;  /* the first snap_check that failed: this build's value, the file's */
	long at;             /* and where in the file it was */
} SnapIO;

static inline void snap_io(SnapIO *io, void *p, size_t n)
{
	if (io->err || !n)
		return;
	if (io->load ? fread(p, 1, n, io->f) != n : fwrite(p, 1, n, io->f) != n)
		io->err = 1;
}

/* Writes v; on load reads it back and fails the snapshot if it differs. Each struct is preceded
   by its size this way, so a build with another layout refuses the file instead of misreading it. */
static inline void snap_check(SnapIO *io, unsigned v)
{
	unsigned r = v;
	snap_io(io, &r, sizeof(r));
	if (io->load && !io->err && r != v) {
		io->err = 2;
		io->want = v;
		io->got = r;
		io->at = ftell(io->f) - (long)sizeof(r);
	}
}

/* A field to keep on load: SNAP_KEEP(Type, field), ended by {0, 0}. */
typedef struct SnapKeep { unsigned short off, len; } SnapKeep;
#define SNAP_KEEP(T, f) { (unsigned short)offsetof(T, f), (unsigned short)sizeof(((T *)0)->f) }

/* Moves the whole struct; on load the kept fields (256 bytes in all at most) survive. */
static inline void snap_struct(SnapIO *io, void *p, size_t n, const SnapKeep *keep)
{
	unsigned char save[256];
	size_t at = 0, i;
	snap_check(io, (unsigned)n);
	if (io->err)
		return;
	if (io->load && keep)
		for (i = 0; keep[i].len; at += keep[i].len, i++)
			memcpy(save + at, (char *)p + keep[i].off, keep[i].len);
	snap_io(io, p, n);
	if (io->load && keep)
		for (i = 0, at = 0; keep[i].len; at += keep[i].len, i++)
			memcpy((char *)p + keep[i].off, save + at, keep[i].len);
}

#endif
