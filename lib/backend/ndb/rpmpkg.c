#include "system.h"

#include <rpm/rpmlog.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

#include "rpmpkg.h"

#define RPMRC_FAIL 2
#define RPMRC_NOTFOUND 1
#define RPMRC_OK 0

#ifdef RPMPKG_LZO
static int rpmpkgLZOCompress(unsigned char **blobp, unsigned int *bloblp);
static int rpmpkgLZODecompress(unsigned char **blobp, unsigned int *bloblp);
#endif

static int rpmpkgVerifyblob(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned int blkoff, unsigned int blkcnt);

typedef struct pkgslot_s {
    unsigned int pkgidx;
    unsigned int blkoff;
    unsigned int blkcnt;
    unsigned int slotno;
} pkgslot;

typedef struct rpmpkgdb_s {
    int fd;			/* our file descriptor */
    int flags;
    int mode;

    int rdonly;

    unsigned int locked_shared;
    unsigned int locked_excl;

    int header_ok;		/* header data (e.g. generation) is valid */
    unsigned int generation;
    unsigned int slotnpages;
    unsigned int nextpkgidx;

    struct pkgslot_s *slots;
    unsigned int aslots;	/* allocated slots */
    unsigned int nslots;	/* used slots */

    unsigned int *slothash;
    unsigned int nslothash;

    unsigned int freeslot;	/* first free slot */
    int slotorder;

    char *filename;
    unsigned int fileblks;	/* file size in blks */
    int dofsync;
} * rpmpkgdb;

#define SLOTORDER_UNORDERED	0
#define SLOTORDER_BLKOFF	1


static inline unsigned int le2h(unsigned char *p) 
{
    return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24; 
}

static inline void h2le(unsigned int x, unsigned char *p) 
{
    p[0] = x;
    p[1] = x >> 8;  
    p[2] = x >> 16; 
    p[3] = x >> 24; 
}

/* adler 32 algorithm taken from RFC 1950 */
#define ADLER32_INIT 1
static unsigned int update_adler32(unsigned int adler, unsigned char *buf, unsigned int len)
{
    unsigned int s1 = adler & 0xffff;
    unsigned int s2 = (adler >> 16) & 0xffff;
    int n;

    for (; len >= 5552; len -= 5552) {
        for (n = 0; n < 5552; n++) {
            s1 += *buf++;
            s2 += s1; 
        }
        s1 %= 65521;
        s2 %= 65521;
    }   
    for (n = 0; n < len; n++) {
        s1 += *buf++;
        s2 += s1; 
    }   
    return ((s2 % 65521) << 16) + (s1 % 65521);
}

/*** Header management ***/

#define PKGDB_MAGIC	('R' | 'p' << 8 | 'm' << 16 | 'P' << 24)
#define PKGDB_VERSION		0

/* must be a multiple of SLOT_SIZE! */
#define PKGDB_HEADER_SIZE	32

#define PKGDB_OFFSET_MAGIC	0
#define PKGDB_OFFSET_VERSION	4
#define PKGDB_OFFSET_GENERATION	8
#define PKGDB_OFFSET_SLOTNPAGES 12
#define PKGDB_OFFSET_NEXTPKGIDX 16

static int rpmpkgReadHeader(rpmpkgdb pkgdb)
{
    unsigned int generation, slotnpages, nextpkgidx, version;
    unsigned char header[PKGDB_HEADER_SIZE];

    /* if we always head the write lock then our data matches */
    if (pkgdb->header_ok)
	return RPMRC_OK;
    if (pread(pkgdb->fd, header, PKGDB_HEADER_SIZE, 0) != PKGDB_HEADER_SIZE) {
	return RPMRC_FAIL;
    }
    if (le2h(header + PKGDB_OFFSET_MAGIC) != PKGDB_MAGIC) {
	return RPMRC_FAIL;
    }
    version = le2h(header + PKGDB_OFFSET_VERSION);
    if (version != PKGDB_VERSION) {
	rpmlog(RPMLOG_ERR, _("rpmpkg: Version mismatch. Expected version: %u. "
	    "Found version: %u\n"), PKGDB_VERSION, version);
	return RPMRC_FAIL;
    }
    generation = le2h(header + PKGDB_OFFSET_GENERATION);
    slotnpages = le2h(header + PKGDB_OFFSET_SLOTNPAGES);
    nextpkgidx = le2h(header + PKGDB_OFFSET_NEXTPKGIDX);
    /* free slots if our internal data no longer matches */
    if (pkgdb->slots && (pkgdb->generation != generation || pkgdb->slotnpages != slotnpages)) {
	free(pkgdb->slots);
	pkgdb->slots = 0;
	if (pkgdb->slothash) {
	    free(pkgdb->slothash);
	    pkgdb->slothash = 0;
	}
    }
    pkgdb->generation = generation;
    pkgdb->slotnpages = slotnpages;
    pkgdb->nextpkgidx = nextpkgidx;
    pkgdb->header_ok = 1;
    return RPMRC_OK;
}

static int rpmpkgWriteHeader(rpmpkgdb pkgdb)
{
    unsigned char header[PKGDB_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    h2le(PKGDB_MAGIC, header + PKGDB_OFFSET_MAGIC);
    h2le(PKGDB_VERSION, header + PKGDB_OFFSET_VERSION);
    h2le(pkgdb->generation, header + PKGDB_OFFSET_GENERATION);
    h2le(pkgdb->slotnpages, header + PKGDB_OFFSET_SLOTNPAGES);
    h2le(pkgdb->nextpkgidx, header + PKGDB_OFFSET_NEXTPKGIDX);
    if (pwrite(pkgdb->fd, header, sizeof(header), 0) != sizeof(header)) {
	return RPMRC_FAIL;
    }
    if (pkgdb->dofsync && fsync(pkgdb->fd))
	return RPMRC_FAIL;	/* write error */
    return RPMRC_OK;
}

/*** Slot management ***/

#define SLOT_MAGIC	('S' | 'l' << 8 | 'o' << 16 | 't' << 24)

#define SLOT_SIZE 16
#define BLK_SIZE  16
#define PAGE_SIZE 4096

/* the first slots (i.e. 32 bytes) are used for the header */
#define SLOT_START (PKGDB_HEADER_SIZE / SLOT_SIZE)

static inline unsigned int hashpkgidx(unsigned int h)
{
    h *= 0x5bd1e995;
    h ^= h >> 16;
    return h;
}

static int rpmpkgHashSlots(rpmpkgdb pkgdb)
{
    unsigned int nslots, num;
    unsigned int *hash;
    unsigned int h, hh, hmask;
    int i;
    pkgslot *slot;

    pkgdb->nslothash = 0;
    num = pkgdb->nslots;
    while (num & (num - 1))
	num = num & (num - 1);
    num *= 4;
    hash = pkgdb->slothash;
    if (!hash || pkgdb->nslothash != num) {
	free(pkgdb->slothash);
	hash = pkgdb->slothash = calloc(num, sizeof(unsigned int));
	if (!hash)
	    return RPMRC_FAIL;
	pkgdb->nslothash = num;
    } else {
	memset(hash, 0, num * sizeof(unsigned int));
    }
    hmask = num - 1;
    nslots = pkgdb->nslots;
    for (i = 0, slot = pkgdb->slots; i < nslots; i++, slot++) {
	for (h = hashpkgidx(slot->pkgidx) & hmask, hh = 7; hash[h] != 0; h = (h + hh++) & hmask)
	    ;
	hash[h] = i + 1;
    }
    pkgdb->slothash = hash;
    pkgdb->nslothash = num;
    return RPMRC_OK;
}

static int rpmpkgReadSlots(rpmpkgdb pkgdb)
{
    unsigned int slotnpages = pkgdb->slotnpages;
    struct stat stb;
    unsigned char pagebuf[PAGE_SIZE];
    unsigned int page;
    unsigned int i, minblkoff, fileblks, slotno, freeslot, o;
    pkgslot *slot;

    /* free old slot data */
    if (pkgdb->slots) {
	free(pkgdb->slots);
	pkgdb->slots = 0;
    }
    if (pkgdb->slothash) {
	free(pkgdb->slothash);
	pkgdb->slothash = 0;
    }
    pkgdb->nslots = 0;
    pkgdb->freeslot = 0;

    /* calculate current database size in blks */
    if (fstat(pkgdb->fd, &stb))
	return RPMRC_FAIL;
    if (stb.st_size % BLK_SIZE)
	return RPMRC_FAIL;	/* hmm */
    fileblks = stb.st_size / BLK_SIZE;

    /* read (and somewhat verify) all slots */
    pkgdb->aslots = slotnpages * (PAGE_SIZE / SLOT_SIZE);
    pkgdb->slots = calloc(pkgdb->aslots, sizeof(*pkgdb->slots));
    if (!pkgdb->slots) {
	return RPMRC_FAIL;
    }
    i = 0;
    slot = pkgdb->slots;
    minblkoff = slotnpages * (PAGE_SIZE / BLK_SIZE);
    slotno = SLOT_START;
    freeslot = 0;
    for (page = 0; page < slotnpages; page++) {
	if (pread(pkgdb->fd, pagebuf, PAGE_SIZE, page * PAGE_SIZE) != PAGE_SIZE)
	    return RPMRC_FAIL;
	for (o = page ? 0 : SLOT_START * SLOT_SIZE; o < PAGE_SIZE; o += SLOT_SIZE, slotno++) {
	    unsigned char *pp = pagebuf + o;
	    unsigned int blkoff, blkcnt, pkgidx;
	    if (le2h(pp) != SLOT_MAGIC) {
		return RPMRC_FAIL;
	    }
	    blkoff = le2h(pp + 8);
	    if (!blkoff) {
		if (!freeslot)
		    freeslot = slotno;
		continue;
	    }
	    pkgidx = le2h(pp + 4);
	    blkcnt = le2h(pp + 12);
	    slot->pkgidx = pkgidx;
	    slot->blkoff = blkoff;
	    slot->blkcnt = blkcnt;
	    slot->slotno = slotno;
	    if (slot->blkoff + slot->blkcnt > fileblks)
		return RPMRC_FAIL;	/* truncated database */
	    if (!slot->pkgidx || !slot->blkcnt || slot->blkoff < minblkoff)
		return RPMRC_FAIL;	/* bad entry */
	    i++;
	    slot++;
	}
    }
    pkgdb->nslots = i;
    pkgdb->slotorder = SLOTORDER_UNORDERED;	/* XXX: always order? */
    pkgdb->fileblks = fileblks;
    pkgdb->freeslot = freeslot;
    if (rpmpkgHashSlots(pkgdb)) {
	free(pkgdb->slots);
	pkgdb->slots = 0;
	return RPMRC_FAIL;
    }
    return RPMRC_OK;
}

static int orderslots_blkoff_cmp(const void *a, const void *b)
{
    unsigned int blkoffa = ((const pkgslot *)a)->blkoff;
    unsigned int blkoffb = ((const pkgslot *)b)->blkoff;
    return blkoffa > blkoffb ? 1 : blkoffa < blkoffb ? -1 : 0;
}

static void rpmpkgOrderSlots(rpmpkgdb pkgdb, int slotorder)
{
    if (pkgdb->slotorder == slotorder)
	return;
    if (slotorder == SLOTORDER_BLKOFF) {
	if (pkgdb->nslots > 1)
	    qsort(pkgdb->slots, pkgdb->nslots, sizeof(*pkgdb->slots), orderslots_blkoff_cmp);
    }
    pkgdb->slotorder = slotorder;
    rpmpkgHashSlots(pkgdb);
}

static inline pkgslot *rpmpkgFindSlot(rpmpkgdb pkgdb, unsigned int pkgidx)
{
    unsigned int i, h,  hh, hmask = pkgdb->nslothash - 1;
    unsigned int *hash = pkgdb->slothash;

    for (h = hashpkgidx(pkgidx) & hmask, hh = 7; (i = hash[h]) != 0; h = (h + hh++) & hmask)
	if (pkgdb->slots[i - 1].pkgidx == pkgidx)
	    return pkgdb->slots + (i - 1);
    return 0;
}

static int rpmpkgFindEmptyOffset(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned int blkcnt, unsigned *blkoffp, pkgslot **oldslotp, int dontprepend)
{
    unsigned int i, nslots = pkgdb->nslots;
    unsigned int bestblkoff = 0;
    unsigned int freecnt, bestfreecnt = 0;
    unsigned int lastblkend = pkgdb->slotnpages * (PAGE_SIZE / BLK_SIZE);
    pkgslot *slot, *oldslot = 0;

    if (pkgdb->slotorder != SLOTORDER_BLKOFF)
	rpmpkgOrderSlots(pkgdb, SLOTORDER_BLKOFF);

    if (dontprepend && nslots) {
	lastblkend = pkgdb->slots[0].blkoff;
    }
    /* best fit strategy */
    for (i = 0, slot = pkgdb->slots; i < nslots; i++, slot++) {
	if (slot->blkoff < lastblkend) {
	    return RPMRC_FAIL;		/* eek, slots overlap! */
	}
	if (slot->pkgidx == pkgidx) {
	    if (oldslot) {
		return RPMRC_FAIL;	/* eek, two slots with our pkgid ! */
	    }
	    oldslot = slot;
	}
	freecnt = slot->blkoff - lastblkend;
	if (freecnt >= blkcnt) {
	    if (!bestblkoff || bestfreecnt > freecnt) {
		bestblkoff = lastblkend;
		bestfreecnt = freecnt;
	    }
	}
	lastblkend = slot->blkoff + slot->blkcnt;
    }
    if (!bestblkoff) {
	bestblkoff = lastblkend;	/* append to end */
    }
    *oldslotp = oldslot;
    *blkoffp = bestblkoff;
    return RPMRC_OK;
}

static int rpmpkgNeighbourCheck(rpmpkgdb pkgdb, unsigned int blkoff, unsigned int blkcnt, unsigned int *newblkcnt)
{
    unsigned int i, nslots = pkgdb->nslots;
    unsigned int lastblkend = pkgdb->slotnpages * (PAGE_SIZE / BLK_SIZE);
    pkgslot *slot, *left = 0, *right = 0;

    if (pkgdb->slotorder != SLOTORDER_BLKOFF)
	rpmpkgOrderSlots(pkgdb, SLOTORDER_BLKOFF);
    if (blkoff < lastblkend)
	return RPMRC_FAIL;
    for (i = 0, slot = pkgdb->slots; i < nslots; i++, slot++) {
	if (slot->blkoff < lastblkend)
	    return RPMRC_FAIL;		/* eek, slots overlap! */
	if (slot->blkoff < blkoff)
	    left = slot;
	if (!right && slot->blkoff >= blkoff)
	    right = slot;
	lastblkend = slot->blkoff + slot->blkcnt;
    }
    if (left && left->blkoff + left->blkcnt != blkoff)
	return RPMRC_FAIL;	/* must always start right after the block */
    if (!left && blkoff != pkgdb->slotnpages * (PAGE_SIZE / BLK_SIZE))
	return RPMRC_FAIL;
    if (right && right->blkoff < blkoff + blkcnt)
	return RPMRC_FAIL;
    /* check if neighbour blobs are in good shape */
    if (left && rpmpkgVerifyblob(pkgdb, left->pkgidx, left->blkoff, left->blkcnt) != RPMRC_OK)
	return RPMRC_FAIL;
    if (right && rpmpkgVerifyblob(pkgdb, right->pkgidx, right->blkoff, right->blkcnt) != RPMRC_OK)
	return RPMRC_FAIL;
    *newblkcnt = right ? right->blkoff - blkoff : blkcnt;
    /* bounds are intect. free area. */
    return RPMRC_OK;
}

static int rpmpkgWriteslot(rpmpkgdb pkgdb, unsigned int slotno, unsigned int pkgidx, unsigned int blkoff, unsigned int blkcnt)
{
    unsigned char buf[SLOT_SIZE];
    /* sanity */
    if (slotno < SLOT_START)
	return RPMRC_FAIL;
    if (blkoff && slotno == pkgdb->freeslot)
	pkgdb->freeslot = 0;
    h2le(SLOT_MAGIC, buf);
    h2le(pkgidx, buf + 4);
    h2le(blkoff, buf + 8);
    h2le(blkcnt, buf + 12);
    if (pwrite(pkgdb->fd, buf, sizeof(buf), slotno * SLOT_SIZE) != sizeof(buf)) {
	return RPMRC_FAIL;
    }
    pkgdb->generation++;
    if (rpmpkgWriteHeader(pkgdb)) {
	return RPMRC_FAIL;
    }
   return RPMRC_OK;
}

static int rpmpkgWriteEmptySlotpage(rpmpkgdb pkgdb, int pageno)
{
    unsigned char page[PAGE_SIZE];
    int i, off = pageno ? 0 : SLOT_START * SLOT_SIZE;
    memset(page, 0, sizeof(page));
    for (i = 0; i < PAGE_SIZE / SLOT_SIZE; i++)
        h2le(SLOT_MAGIC, page + i * SLOT_SIZE);
    if (pwrite(pkgdb->fd, page, PAGE_SIZE - off, pageno * PAGE_SIZE + off) != PAGE_SIZE - off) {
	return RPMRC_FAIL;
    }
    if (pkgdb->dofsync && fsync(pkgdb->fd)) {
	return RPMRC_FAIL;	/* write error */
    }
    return RPMRC_OK;
}

/*** Blk primitives ***/

static int rpmpkgZeroBlks(rpmpkgdb pkgdb, unsigned int blkoff, unsigned int blkcnt)
{
    unsigned char buf[65536];
    unsigned int towrite;
    off_t fileoff;

    memset(buf, 0, sizeof(buf));
    fileoff = (off_t)blkoff * BLK_SIZE;
    for (towrite = blkcnt * BLK_SIZE; towrite; ) {
	unsigned int chunk = towrite > 65536 ? 65536 : towrite;
	if (pwrite(pkgdb->fd, buf, chunk, fileoff) != chunk) {
	    return RPMRC_FAIL;	/* write error */
	}
	fileoff += chunk;
	towrite -= chunk;
    }
    if (blkoff + blkcnt > pkgdb->fileblks)
	pkgdb->fileblks = blkoff + blkcnt;
    return RPMRC_OK;
}

static int rpmpkgValidateZeroCheck(rpmpkgdb pkgdb, unsigned int blkoff, unsigned int blkcnt)
{
    unsigned long long buf[(65536 / sizeof(unsigned long long)) + 1];
    off_t fileoff;
    off_t tocheck;
    int i;

    if (blkoff > pkgdb->fileblks)
	return RPMRC_FAIL;		/* huh? */
    fileoff = (off_t)blkoff * BLK_SIZE;
    tocheck = blkoff + blkcnt > pkgdb->fileblks ? pkgdb->fileblks - blkoff : blkcnt;
    tocheck *= BLK_SIZE;
    while (tocheck >= 65536) {
        if (pread(pkgdb->fd, (void *)buf, 65536, fileoff) != 65536)
	    return RPMRC_FAIL;		/* read error */
	for (i = 0; i < 65536 / sizeof(unsigned long long); i++)
	    if (buf[i])
		return RPMRC_FAIL;	/* not empty */
	fileoff += 65536;
	tocheck -= 65536;
    }
    if (tocheck) {
	int cnt = (int)tocheck / sizeof(unsigned long long);
	buf[cnt++] = 0;
        if (pread(pkgdb->fd, (void *)buf, tocheck, fileoff) != tocheck)
	    return RPMRC_FAIL;		/* read error */
	for (i = 0; i < cnt; i++)
	    if (buf[i])
		return RPMRC_FAIL;	/* not empty */
    }
    return RPMRC_OK;
}

static int rpmpkgValidateZero(rpmpkgdb pkgdb, unsigned int blkoff, unsigned int blkcnt)
{
    if (rpmpkgValidateZeroCheck(pkgdb, blkoff, blkcnt) == RPMRC_OK)
	return RPMRC_OK;
    rpmlog(RPMLOG_WARNING, _("rpmpkg: detected non-zero blob, trying auto repair\n"));
    /* auto-repair interrupted transactions */
    if (rpmpkgNeighbourCheck(pkgdb, blkoff, blkcnt, &blkcnt) != RPMRC_OK)
	return RPMRC_FAIL;
    if (rpmpkgZeroBlks(pkgdb, blkoff, blkcnt) != RPMRC_OK)
	return RPMRC_FAIL;
    return RPMRC_OK;
}


/*** Blob primitives ***/

/* head: magic + pkgidx + timestamp + bloblen */
/* tail: adler32 + bloblen + magic */

#define BLOBHEAD_MAGIC	('B' | 'l' << 8 | 'b' << 16 | 'S' << 24)
#define BLOBTAIL_MAGIC	('B' | 'l' << 8 | 'b' << 16 | 'E' << 24)

#define BLOBHEAD_SIZE	(4 + 4 + 4 + 4)
#define BLOBTAIL_SIZE	(4 + 4 + 4)

static int rpmpkgReadBlob(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned int blkoff, unsigned int blkcnt, unsigned char *blob, unsigned int *bloblp, unsigned int *tstampp)
{
    unsigned char buf[BLOBHEAD_SIZE > BLOBTAIL_SIZE ? BLOBHEAD_SIZE : BLOBTAIL_SIZE];
    unsigned int bloblen, toread, tstamp;
    off_t fileoff;
    unsigned int adl;
    int verifyadler = bloblp ? 0 : 1;

    /* sanity */
    if (blkcnt <  (BLOBHEAD_SIZE + BLOBTAIL_SIZE + BLK_SIZE - 1) / BLK_SIZE)
	return RPMRC_FAIL;	/* blkcnt too small */
    /* read header */
    fileoff = (off_t)blkoff * BLK_SIZE;
    if (pread(pkgdb->fd, buf, BLOBHEAD_SIZE, fileoff) != BLOBHEAD_SIZE)
	return RPMRC_FAIL;	/* read error */
    if (le2h(buf) != BLOBHEAD_MAGIC)
	return RPMRC_FAIL;	/* bad blob */
    if (le2h(buf + 4) != pkgidx)
	return RPMRC_FAIL;	/* bad blob */
    tstamp = le2h(buf + 8);
    bloblen = le2h(buf + 12);
    if (blkcnt != (BLOBHEAD_SIZE + bloblen + BLOBTAIL_SIZE + BLK_SIZE - 1) / BLK_SIZE)
	return RPMRC_FAIL;	/* bad blob */
    adl = ADLER32_INIT;
    if (verifyadler)
	adl = update_adler32(adl, buf, BLOBHEAD_SIZE);
    /* read in 64K chunks */
    fileoff += BLOBHEAD_SIZE;
    toread = blkcnt * BLK_SIZE - BLOBHEAD_SIZE;
    if (!bloblp)
	toread -= BLOBTAIL_SIZE;
    while (toread) {
	unsigned int chunk = toread > 65536 ? 65536 : toread;
        if (pread(pkgdb->fd, blob, chunk, fileoff) != chunk) {
	    return RPMRC_FAIL;	/* read error */
	}
	if (verifyadler) {
	    if (!bloblp)
		adl = update_adler32(adl, blob, chunk);
	    else if (toread > BLOBTAIL_SIZE)
		adl = update_adler32(adl, blob, toread - BLOBTAIL_SIZE > chunk ? chunk : toread - BLOBTAIL_SIZE);
	}
	if (bloblp)
	    blob += chunk;
	toread -= chunk;
	fileoff += chunk;
    }
    /* read trailer */
    if (bloblp) {
	memcpy(buf, blob - BLOBTAIL_SIZE, BLOBTAIL_SIZE);
    } else if (pread(pkgdb->fd, buf, BLOBTAIL_SIZE, fileoff) != BLOBTAIL_SIZE) {
	return RPMRC_FAIL;	/* read error */
    }
    if (verifyadler && le2h(buf) != adl) {
	return RPMRC_FAIL;	/* bad blob, adler32 mismatch */
    }
    if (le2h(buf + 4) != bloblen) {
	return RPMRC_FAIL;	/* bad blob, bloblen mismatch */
    }
    if (le2h(buf + 8) != BLOBTAIL_MAGIC) {
	return RPMRC_FAIL;	/* bad blob */
    }
    if (bloblp)
	*bloblp = bloblen;
    if (tstampp)
	*tstampp = tstamp;
    return RPMRC_OK;
}

static int rpmpkgVerifyblob(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned int blkoff, unsigned int blkcnt)
{
    unsigned char buf[65536];
    return rpmpkgReadBlob(pkgdb, pkgidx, blkoff, blkcnt, buf, 0, 0);
}

static int rpmpkgWriteBlob(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned int blkoff, unsigned int blkcnt, unsigned char *blob, unsigned int blobl, unsigned int now)
{
    unsigned char buf[(BLOBHEAD_SIZE > BLOBTAIL_SIZE ? BLOBHEAD_SIZE : BLOBTAIL_SIZE) + BLK_SIZE];
    unsigned int towrite, pad;
    unsigned int adl;
    off_t fileoff;

    /* sanity */
    if (blkcnt <  (BLOBHEAD_SIZE + BLOBTAIL_SIZE + BLK_SIZE - 1) / BLK_SIZE)
	return RPMRC_FAIL;	/* blkcnt too small */
    if (blkcnt != (BLOBHEAD_SIZE + blobl + BLOBTAIL_SIZE + BLK_SIZE - 1) / BLK_SIZE)
	return RPMRC_FAIL;	/* blkcnt mismatch */
    fileoff = (off_t)blkoff * BLK_SIZE;
    h2le(BLOBHEAD_MAGIC, buf);
    h2le(pkgidx, buf + 4);
    h2le(now, buf + 8);
    h2le(blobl, buf + 12);
    if (pwrite(pkgdb->fd, buf, BLOBHEAD_SIZE, fileoff) != BLOBHEAD_SIZE) {
	return RPMRC_FAIL;	/* write error */
    }
    adl = ADLER32_INIT;
    adl = update_adler32(adl, buf, BLOBHEAD_SIZE);
    /* write in 64K chunks */
    fileoff += BLOBHEAD_SIZE;
    for (towrite = blobl; towrite;) {
	unsigned int chunk = towrite > 65536 ? 65536 : towrite;
	if (pwrite(pkgdb->fd, blob, chunk, fileoff) != chunk) {
	    return RPMRC_FAIL;	/* write error */
	}
	adl = update_adler32(adl, blob, chunk);
	blob += chunk;
	towrite -= chunk;
	fileoff += chunk;
    }
    /* pad if needed */
    pad = blkcnt * BLK_SIZE - (BLOBHEAD_SIZE + blobl + BLOBTAIL_SIZE);
    if (pad) {
	memset(buf + (sizeof(buf) - BLOBTAIL_SIZE) - pad, 0, pad);
	adl = update_adler32(adl, buf + (sizeof(buf) - BLOBTAIL_SIZE) - pad, pad);
    }
    h2le(adl, buf + (sizeof(buf) - BLOBTAIL_SIZE));
    h2le(blobl, buf + (sizeof(buf) - BLOBTAIL_SIZE) + 4);
    h2le(BLOBTAIL_MAGIC, buf + (sizeof(buf) - BLOBTAIL_SIZE) + 8);
    if (pwrite(pkgdb->fd, buf + (sizeof(buf) - BLOBTAIL_SIZE) - pad, pad + BLOBTAIL_SIZE, fileoff) != pad + BLOBTAIL_SIZE) {
	return RPMRC_FAIL;	/* write error */
    }
    /* update file length */
    if (blkoff + blkcnt > pkgdb->fileblks)
	pkgdb->fileblks = blkoff + blkcnt;
    if (pkgdb->dofsync && fsync(pkgdb->fd)) {
	return RPMRC_FAIL;	/* write error */
    }
    return RPMRC_OK;
}

static int rpmpkgDelBlob(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned int blkoff, unsigned int blkcnt)
{
    if (rpmpkgVerifyblob(pkgdb, pkgidx, blkoff, blkcnt))
	return RPMRC_FAIL;
    if (rpmpkgZeroBlks(pkgdb, blkoff, blkcnt))
	return RPMRC_FAIL;
    if (pkgdb->dofsync && fsync(pkgdb->fd))
	return RPMRC_FAIL;	/* write error */
    return RPMRC_OK;
}


static int rpmpkgMoveBlob(rpmpkgdb pkgdb, pkgslot *slot, unsigned int newblkoff)
{
    unsigned int pkgidx = slot->pkgidx;
    unsigned int blkoff = slot->blkoff;
    unsigned int blkcnt = slot->blkcnt;
    unsigned char *blob;
    unsigned int tstamp, blobl;

    blob = malloc((size_t)blkcnt * BLK_SIZE);
    if (rpmpkgReadBlob(pkgdb, pkgidx, blkoff, blkcnt, blob, &blobl, &tstamp)) {
	free(blob);
	return RPMRC_FAIL;
    }
    if (rpmpkgWriteBlob(pkgdb, pkgidx, newblkoff, blkcnt, blob, blobl, tstamp)) {
	free(blob);
	return RPMRC_FAIL;
    }
    free(blob);
    if (rpmpkgWriteslot(pkgdb, slot->slotno, pkgidx, newblkoff, blkcnt)) {
	return RPMRC_FAIL;
    }
    if (rpmpkgDelBlob(pkgdb, pkgidx, blkoff, blkcnt)) {
	return RPMRC_FAIL;
    }
    slot->blkoff = newblkoff;
    pkgdb->slotorder = SLOTORDER_UNORDERED;
    return RPMRC_OK;
}

static int rpmpkgAddSlotPage(rpmpkgdb pkgdb)
{
    unsigned int cutoff;
    if (pkgdb->slotorder != SLOTORDER_BLKOFF)
	rpmpkgOrderSlots(pkgdb, SLOTORDER_BLKOFF);
    cutoff = (pkgdb->slotnpages + 1) * (PAGE_SIZE / BLK_SIZE);

    /* now move every blob before cutoff */
    while (pkgdb->nslots && pkgdb->slots[0].blkoff < cutoff) {
	unsigned int newblkoff;
        pkgslot *slot = pkgdb->slots, *oldslot;

	oldslot = 0;
	if (rpmpkgFindEmptyOffset(pkgdb, slot->pkgidx, slot->blkcnt, &newblkoff, &oldslot, 1)) {
	    return RPMRC_FAIL;
	}
	if (!oldslot || oldslot != slot) {
	    return RPMRC_FAIL;
	}
	if (rpmpkgMoveBlob(pkgdb, slot, newblkoff)) {
	    return RPMRC_FAIL;
	}
	rpmpkgOrderSlots(pkgdb, SLOTORDER_BLKOFF);
    }

    /* make sure our new page is empty */
    if (rpmpkgValidateZero(pkgdb, pkgdb->slotnpages * (PAGE_SIZE / BLK_SIZE), PAGE_SIZE / BLK_SIZE)) {
	return RPMRC_FAIL;
    }
    if (rpmpkgWriteEmptySlotpage(pkgdb, pkgdb->slotnpages)) {
	return RPMRC_FAIL;
    }

    /* announce free page */
    pkgdb->freeslot = pkgdb->slotnpages * (PAGE_SIZE / SLOT_SIZE);
    pkgdb->slotnpages++;
    pkgdb->generation++;
    if (rpmpkgWriteHeader(pkgdb)) {
	return RPMRC_FAIL;
    }
    return RPMRC_OK;
}

static inline int is_correct_db(rpmpkgdb pkgdb)
{
    struct stat stb1, stb2;
    if (stat(pkgdb->filename, &stb1))
	return 0;
    if (fstat(pkgdb->fd, &stb2))
	return 0;
    return stb1.st_dev == stb2.st_dev && stb1.st_ino == stb2.st_ino;
}

static inline int reopen_db(rpmpkgdb pkgdb)
{
    close(pkgdb->fd);
    if ((pkgdb->fd = open(pkgdb->filename, pkgdb->flags, pkgdb->mode)) == -1) {
	return RPMRC_FAIL;
    }
    return RPMRC_OK;
}

static int rpmpkgGetLock(rpmpkgdb pkgdb, int type)
{
    if (!pkgdb->fd)
	return RPMRC_FAIL;
    for (;;) {
	if (flock(pkgdb->fd, type)) {
	    return RPMRC_FAIL;
	}
	if (!is_correct_db(pkgdb)) {
	    if (reopen_db(pkgdb)) {
		return RPMRC_FAIL;
	    }
	    continue;
	}
	break;
    }
    return RPMRC_OK;
}

int rpmpkgLock(rpmpkgdb pkgdb, int excl)
{
    unsigned int *lockcntp = excl ? &pkgdb->locked_excl : &pkgdb->locked_shared;
    if (*lockcntp > 0 || (!excl && pkgdb->locked_excl)) {
	(*lockcntp)++;
	return RPMRC_OK;
    }
    pkgdb->header_ok = 0;
    if (rpmpkgGetLock(pkgdb, excl ? LOCK_EX : LOCK_SH)) {
	return RPMRC_FAIL;
    }
    (*lockcntp)++;
    return RPMRC_OK;
}

static int rpmpkgLockInternal(rpmpkgdb pkgdb, int excl)
{
    if (excl && pkgdb->rdonly)
	return RPMRC_FAIL;

    return  rpmpkgLock(pkgdb, excl);
}

int rpmpkgUnlock(rpmpkgdb pkgdb, int excl)
{
    unsigned int *lockcntp = excl ? &pkgdb->locked_excl : &pkgdb->locked_shared;
    if (*lockcntp == 0) {
	return RPMRC_FAIL;
    }
    if (*lockcntp > 1 || (!excl && pkgdb->locked_excl)) {
	(*lockcntp)--;
	return RPMRC_OK;
    }
    if (excl && pkgdb->locked_shared) {
	/* excl -> shared switch */
	if (rpmpkgGetLock(pkgdb, LOCK_SH)) {
	    return RPMRC_FAIL;
	}
	(*lockcntp)--;
	return RPMRC_OK;
    }
    flock(pkgdb->fd, LOCK_UN);
    (*lockcntp)--;
    pkgdb->header_ok = 0;
    return RPMRC_OK;
}

static int rpmpkgLockReadHeader(rpmpkgdb pkgdb, int excl)
{
    if (rpmpkgLockInternal(pkgdb, excl))
	return RPMRC_FAIL;
    if (rpmpkgReadHeader(pkgdb)) {
	rpmpkgUnlock(pkgdb, excl);
	return RPMRC_FAIL;
    }
    return RPMRC_OK;
}

static int rpmpkgInitInternal(rpmpkgdb pkgdb)
{
    struct stat stb;
    if (fstat(pkgdb->fd, &stb)) {
	return RPMRC_FAIL;
    }
    if (stb.st_size == 0) {
	if (rpmpkgWriteEmptySlotpage(pkgdb, 0)) {
	    return RPMRC_FAIL;
	}
	pkgdb->slotnpages = 1;
	if (!pkgdb->nextpkgidx)
	    pkgdb->nextpkgidx = 1;
	pkgdb->generation++;
	if (rpmpkgWriteHeader(pkgdb)) {
	    return RPMRC_FAIL;
	}
    }
    return RPMRC_OK;
}

static int rpmpkgInit(rpmpkgdb pkgdb)
{
    int rc;
    
    if (rpmpkgLockInternal(pkgdb, 1))
	return RPMRC_FAIL;
    rc = rpmpkgInitInternal(pkgdb);
    rpmpkgUnlock(pkgdb, 1);
    return rc;
}

int rpmpkgOpen(rpmpkgdb *pkgdbp, const char *filename, int flags, int mode)
{
    struct stat stb;
    rpmpkgdb pkgdb;

    *pkgdbp = 0;
    pkgdb = calloc(1, sizeof(*pkgdb));
    pkgdb->filename = strdup(filename);
    if (!pkgdb->filename) {
	free(pkgdb);
	return RPMRC_FAIL;
    }
    if ((flags & (O_RDONLY|O_RDWR)) == O_RDONLY)
	pkgdb->rdonly = 1;
    if ((pkgdb->fd = open(filename, flags, mode)) == -1) {
	free(pkgdb->filename);
	free(pkgdb);
        return RPMRC_FAIL;
    }
    if (flags & O_CREAT) {
	char *filenameCopy;
	DIR *pdir;

	if ((filenameCopy = strdup(pkgdb->filename)) == NULL) {
	    close(pkgdb->fd);
	    free(pkgdb->filename);
	    free(pkgdb);
	    return RPMRC_FAIL;
	}

	if ((pdir = opendir(dirname(filenameCopy))) == NULL) {
	    free(filenameCopy);
	    close(pkgdb->fd);
	    free(pkgdb->filename);
	    free(pkgdb);
	    return RPMRC_FAIL;
	}

	if (fsync(dirfd(pdir)) == -1) {
	    closedir(pdir);
	    free(filenameCopy);
	    close(pkgdb->fd);
	    free(pkgdb->filename);
	    free(pkgdb);
	    return RPMRC_FAIL;
	}
	closedir(pdir);
	free(filenameCopy);

    }
    if (fstat(pkgdb->fd, &stb)) {
	close(pkgdb->fd);
	free(pkgdb->filename);
	free(pkgdb);
        return RPMRC_FAIL;
    }
    if (stb.st_size == 0) {
	if (rpmpkgInit(pkgdb)) {
	    close(pkgdb->fd);
	    free(pkgdb->filename);
	    free(pkgdb);
	    return RPMRC_FAIL;
	}
    }
    pkgdb->flags = flags;
    pkgdb->mode = mode;
    pkgdb->dofsync = 1;
    *pkgdbp = pkgdb;
    return RPMRC_OK;
}

void rpmpkgClose(rpmpkgdb pkgdb)
{
    if (pkgdb->fd >= 0) {
	close(pkgdb->fd);
	pkgdb->fd = -1;
    }
    if (pkgdb->slots)
	free(pkgdb->slots);
    pkgdb->slots = 0;
    if (pkgdb->slothash)
	free(pkgdb->slothash);
    pkgdb->slothash = 0;
    free(pkgdb->filename);
    free(pkgdb);
}

void rpmpkgSetFsync(rpmpkgdb pkgdb, int dofsync)
{
    pkgdb->dofsync = dofsync;
}


static int rpmpkgGetInternal(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned char **blobp, unsigned int *bloblp)
{
    pkgslot *slot;
    unsigned char *blob;

    if (!pkgdb->slots && rpmpkgReadSlots(pkgdb)) {
	return RPMRC_FAIL;
    }
    slot = rpmpkgFindSlot(pkgdb, pkgidx);
    if (!slot) {
	return RPMRC_NOTFOUND;
    }
    blob = malloc((size_t)slot->blkcnt * BLK_SIZE);
    if (rpmpkgReadBlob(pkgdb, pkgidx, slot->blkoff, slot->blkcnt, blob, bloblp, (unsigned int *)0)) {
	free(blob);
	return RPMRC_FAIL;
    }
    *blobp = blob;
    return RPMRC_OK;
}

static int rpmpkgPutInternal(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned char *blob, unsigned int blobl)
{
    unsigned int blkcnt, blkoff, slotno;
    pkgslot *oldslot;

    /* we always read all slots when writing, just in case */
    if (rpmpkgReadSlots(pkgdb)) {
	return RPMRC_FAIL;
    }
    blkcnt = (BLOBHEAD_SIZE + blobl + BLOBTAIL_SIZE + BLK_SIZE - 1) / BLK_SIZE;
    /* find a nice place for the blob */
    if (rpmpkgFindEmptyOffset(pkgdb, pkgidx, blkcnt, &blkoff, &oldslot, 0)) {
	return RPMRC_FAIL;
    }
    /* create new slot page if we don't have a free slot and can't reuse an old one */
    if (!oldslot && !pkgdb->freeslot) {
	if (rpmpkgAddSlotPage(pkgdb)) {
	    return RPMRC_FAIL;
	}
	/* redo rpmpkgFindEmptyOffset to get another free area */
	if (rpmpkgFindEmptyOffset(pkgdb, pkgidx, blkcnt, &blkoff, &oldslot, 0)) {
	    return RPMRC_FAIL;
	}
    }
    /* make sure that we don't overwrite data */
    if (rpmpkgValidateZero(pkgdb, blkoff, blkcnt)) {
	return RPMRC_FAIL;
    }
    /* write new blob */
    if (rpmpkgWriteBlob(pkgdb, pkgidx, blkoff, blkcnt, blob, blobl, (unsigned int)time(0))) {
	return RPMRC_FAIL;
    }
    /* write slot */
    slotno = oldslot ? oldslot->slotno : pkgdb->freeslot;
    if (!slotno) {
	return RPMRC_FAIL;
    }
    if (rpmpkgWriteslot(pkgdb, slotno, pkgidx, blkoff, blkcnt)) {
	free(pkgdb->slots);
	pkgdb->slots = 0;
	return RPMRC_FAIL;
    }
    /* erase old blob */
    if (oldslot && oldslot->blkoff) {
	if (rpmpkgDelBlob(pkgdb, pkgidx, oldslot->blkoff, oldslot->blkcnt)) {
	    free(pkgdb->slots);
	    pkgdb->slots = 0;
	    return RPMRC_FAIL;
	}
    }
    if (oldslot) {
	/* just update the slot, no need to free the slot data */
	oldslot->blkoff = blkoff;
	oldslot->blkcnt = blkcnt;
	pkgdb->slotorder = SLOTORDER_UNORDERED;
    } else {
	free(pkgdb->slots);
	pkgdb->slots = 0;
    }
    return RPMRC_OK;
}

static int rpmpkgDelInternal(rpmpkgdb pkgdb, unsigned int pkgidx)
{
    pkgslot *slot;
    unsigned int blkoff, blkcnt;

    /* we always read all slots when writing, just in case */
    if (rpmpkgReadSlots(pkgdb)) {
	return RPMRC_FAIL;
    }
    rpmpkgOrderSlots(pkgdb, SLOTORDER_BLKOFF);
    slot = rpmpkgFindSlot(pkgdb, pkgidx);
    if (!slot) {
	return RPMRC_OK;
    }
    if (rpmpkgWriteslot(pkgdb, slot->slotno, 0, 0, 0)) {
	return RPMRC_FAIL;
    }
    if (rpmpkgDelBlob(pkgdb, pkgidx, slot->blkoff, slot->blkcnt)) {
	return RPMRC_FAIL;
    }
    if (pkgdb->nslots > 1 && slot->blkoff < pkgdb->fileblks / 2) {
	/* we freed a blob in the first half of our data. do some extra work */
	int i;
	if (slot == pkgdb->slots) {
	    blkoff = pkgdb->slotnpages * (PAGE_SIZE / BLK_SIZE);
	} else {
	    blkoff = slot[-1].blkoff + slot[-1].blkcnt;
	}
	if (slot < pkgdb->slots + pkgdb->nslots - 1) {
	    blkcnt = slot[1].blkoff - blkoff;
	} else {
	    blkcnt = slot->blkoff + slot->blkcnt - blkoff;
	}
	slot->blkoff = 0;
	slot->blkcnt = 0;
	slot = pkgdb->slots + pkgdb->nslots - 2;
	if (slot->blkcnt < slot[1].blkcnt)
	  slot++;	/* bigger slot first */
	for (i = 0; i < 2; i++, slot++) {
	    if (slot == pkgdb->slots + pkgdb->nslots)
		slot -= 2;
	    if (!slot->blkoff || slot->blkoff < blkoff)
		continue;
	    if (slot->blkoff < pkgdb->fileblks / 2)
		continue;
	    if (slot->blkcnt > blkcnt)
		continue;
	    rpmpkgMoveBlob(pkgdb, slot, blkoff);
	    blkoff += slot->blkcnt;
	    blkcnt -= slot->blkcnt;
	}
	rpmpkgOrderSlots(pkgdb, SLOTORDER_BLKOFF);
    } else {
	slot->blkoff = 0;
	slot->blkcnt = 0;
    }
    /* check if we can truncate the file */
    slot = pkgdb->slots + pkgdb->nslots - 1;
    if (!slot->blkoff && pkgdb->nslots > 1) {
	slot--;
    }
    if (slot->blkoff)
	blkoff = slot->blkoff + slot->blkcnt;
    else
	blkoff = pkgdb->slotnpages * (PAGE_SIZE / BLK_SIZE);
    if (blkoff < pkgdb->fileblks / 4 * 3) {
	/* truncate the file */
	if (!rpmpkgValidateZero(pkgdb, blkoff, pkgdb->fileblks - blkoff)) {
	    if (!ftruncate(pkgdb->fd, blkoff * BLK_SIZE)) {
		pkgdb->fileblks = blkoff;
	    }
	}
    }
    free(pkgdb->slots);
    pkgdb->slots = 0;
    return RPMRC_OK;
}

static int rpmpkgListInternal(rpmpkgdb pkgdb, unsigned int **pkgidxlistp, unsigned int *npkgidxlistp)
{
    unsigned int i, nslots, *pkgidxlist;
    pkgslot *slot;

    if (!pkgdb->slots && rpmpkgReadSlots(pkgdb)) {
	return RPMRC_FAIL;
    }
    if (!pkgidxlistp) {
	*npkgidxlistp = pkgdb->nslots;
	return RPMRC_OK;
    }
    rpmpkgOrderSlots(pkgdb, SLOTORDER_BLKOFF);
    nslots = pkgdb->nslots;
    pkgidxlist = calloc(nslots + 1, sizeof(unsigned int));
    for (i = 0, slot = pkgdb->slots; i < nslots; i++, slot++) {
	pkgidxlist[i] = slot->pkgidx;
    }
    *pkgidxlistp = pkgidxlist;
    *npkgidxlistp = nslots;
    return RPMRC_OK;
}

int rpmpkgGet(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned char **blobp, unsigned int *bloblp)
{
    int rc;

    *blobp = 0;
    *bloblp = 0;
    if (!pkgidx)
	return RPMRC_FAIL;
    if (rpmpkgLockReadHeader(pkgdb, 0))
	return RPMRC_FAIL;
    rc = rpmpkgGetInternal(pkgdb, pkgidx, blobp, bloblp);
    rpmpkgUnlock(pkgdb, 0);
#ifdef RPMPKG_LZO
    if (!rc)
	rc = rpmpkgLZODecompress(blobp, bloblp);
#endif
    return rc;
}

int rpmpkgPut(rpmpkgdb pkgdb, unsigned int pkgidx, unsigned char *blob, unsigned int blobl)
{
    int rc;

    if (!pkgidx) {
	return RPMRC_FAIL;
    }
    if (rpmpkgLockReadHeader(pkgdb, 1))
	return RPMRC_FAIL;
#ifdef RPMPKG_LZO
    if (rpmpkgLZOCompress(&blob, &blobl)) {
	rpmpkgUnlock(pkgdb, 1);
	return RPMRC_FAIL;
    }
#endif
    rc = rpmpkgPutInternal(pkgdb, pkgidx, blob, blobl);
#ifdef RPMPKG_LZO
    free(blob);
#endif
    rpmpkgUnlock(pkgdb, 1);
    return rc;
}

int rpmpkgDel(rpmpkgdb pkgdb, unsigned int pkgidx)
{
    int rc;

    if (!pkgidx) {
	return RPMRC_FAIL;
    }
    if (rpmpkgLockReadHeader(pkgdb, 1))
	return RPMRC_FAIL;
    rc = rpmpkgDelInternal(pkgdb, pkgidx);
    rpmpkgUnlock(pkgdb, 1);
    return rc;
}

int rpmpkgList(rpmpkgdb pkgdb, unsigned int **pkgidxlistp, unsigned int *npkgidxlistp)
{
    int rc;
    if (pkgidxlistp)
	*pkgidxlistp = 0;
    *npkgidxlistp = 0;
    if (rpmpkgLockReadHeader(pkgdb, 0))
	return RPMRC_FAIL;
    rc = rpmpkgListInternal(pkgdb, pkgidxlistp, npkgidxlistp);
    rpmpkgUnlock(pkgdb, 0);
    return rc;
}

int rpmpkgNextPkgIdx(rpmpkgdb pkgdb, unsigned int *pkgidxp)
{
    if (rpmpkgLockReadHeader(pkgdb, 1))
	return RPMRC_FAIL;
    *pkgidxp = pkgdb->nextpkgidx++;
    if (rpmpkgWriteHeader(pkgdb)) {
	rpmpkgUnlock(pkgdb, 1);
	return RPMRC_FAIL;
    }
    /* no fsync needed. also no need to increase the generation count,
     * as the header is always read in */
    rpmpkgUnlock(pkgdb, 1);
    return RPMRC_OK;
}

int rpmpkgGeneration(rpmpkgdb pkgdb, unsigned int *generationp)
{
    if (rpmpkgLockReadHeader(pkgdb, 0))
	return RPMRC_FAIL;
    *generationp = pkgdb->generation;
    rpmpkgUnlock(pkgdb, 0);
    return RPMRC_OK;
}

int rpmpkgStats(rpmpkgdb pkgdb)
{
    unsigned int usedblks = 0;
    int i;

    if (rpmpkgLockReadHeader(pkgdb, 0))
	return RPMRC_FAIL;
    if (rpmpkgReadSlots(pkgdb)) {
	rpmpkgUnlock(pkgdb, 0);
	return RPMRC_FAIL;
    }
    for (i = 0; i < pkgdb->nslots; i++)
	usedblks += pkgdb->slots[i].blkcnt;
    printf("--- Package DB Stats\n");
    printf("Filename: %s\n", pkgdb->filename);
    printf("Generation: %d\n", pkgdb->generation);
    printf("Slot pages: %d\n", pkgdb->slotnpages);
    printf("Used slots: %d\n", pkgdb->nslots);
    printf("Free slots: %d\n", pkgdb->slotnpages * (PAGE_SIZE / SLOT_SIZE) - pkgdb->nslots);
    printf("Blob area size: %d\n", (pkgdb->fileblks - pkgdb->slotnpages * (PAGE_SIZE / BLK_SIZE)) * BLK_SIZE);
    printf("Blob area used: %d\n", usedblks * BLK_SIZE);
    rpmpkgUnlock(pkgdb, 0);
    return RPMRC_OK;
}

#ifdef RPMPKG_LZO

#include "lzo/lzoconf.h"
#include "lzo/lzo1x.h"

#define BLOBLZO_MAGIC	('L' | 'Z' << 8 | 'O' << 16 | 'B' << 24)

static int rpmpkgLZOCompress(unsigned char **blobp, unsigned int *bloblp)
{
    unsigned char *blob = *blobp;
    unsigned int blobl = *bloblp;
    unsigned char *lzoblob, *workmem;
    unsigned int lzoblobl;
    lzo_uint blobl2;

    if (lzo_init() != LZO_E_OK) {
	return RPMRC_FAIL;
    }
    workmem = malloc(LZO1X_1_MEM_COMPRESS);
    if (!workmem) {
	return RPMRC_FAIL;
    }
    lzoblobl = 4 + 4 + blobl + blobl / 16 + 64 + 3;
    lzoblob = malloc(lzoblobl);
    if (!lzoblob) {
	free(workmem);
	return RPMRC_FAIL;
    }
    h2le(BLOBLZO_MAGIC, lzoblob);
    h2le(blobl, lzoblob + 4);
    if (lzo1x_1_compress(blob, blobl, lzoblob + 8, &blobl2, workmem) != LZO_E_OK) {
	free(workmem);
	free(lzoblob);
	return RPMRC_FAIL;
    }
    free(workmem);
    *blobp = lzoblob;
    *bloblp = 8 + blobl2;
    return RPMRC_OK;
}

static int rpmpkgLZODecompress(unsigned char **blobp, unsigned int *bloblp)
{
    unsigned char *lzoblob = *blobp;
    unsigned int lzoblobl = *bloblp;
    unsigned char *blob;
    unsigned int blobl;
    lzo_uint blobl2;

    if (!lzoblob || lzoblobl < 8)
	return RPMRC_FAIL;
    if (le2h(lzoblob) != BLOBLZO_MAGIC)
	return RPMRC_FAIL;
    if (lzo_init() != LZO_E_OK)
	return RPMRC_FAIL;
    blobl = le2h(lzoblob + 4);
    blob = malloc(blobl ? blobl : 1);
    if (!blob)
	return RPMRC_FAIL;
    if (lzo1x_decompress(lzoblob + 8, lzoblobl - 8, blob, &blobl2, 0) != LZO_E_OK || blobl2 != blobl) {
	free(blob);
	return RPMRC_FAIL;
    }
    free(lzoblob);
    *blobp = blob;
    *bloblp = blobl;
    return RPMRC_OK;
}

#endif
