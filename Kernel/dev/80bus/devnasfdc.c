/*
 *	Driver for the GM809/829/849/849A floppy controllers
 *	(The SASI/SCSI has its own driver)
 */

#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <nascom.h>
#include <devnasfd.h>

__sfr __at 0xE0 nasfd_cmd;
__sfr __at 0xE0 nasfd_status;
__sfr __at 0xE1 nasfd_track;
__sfr __at 0xE2 nasfd_sector;
__sfr __at 0xE3 nasfd_data;
__sfr __at 0xE4 nasfd_drive;
__sfr __at 0xE5 nasfd_wait;

#define SIDE		1
#define DDENS		2

static uint8_t nasfd_cursel;
uint8_t nasfd_steprate;
static uint8_t drive_last = 0xFF, flags_last;

/* IBM3740 skew table */
static uint8_t skew_3740[] = {
	1, 7, 13, 19, 25, 5, 11, 17, 23, 3, 9, 15, 21, 2, 8, 14, 20, 26, 6, 12, 18, 24, 4, 10, 16, 22
};

/* Skewed at format level */
static uint8_t skew_hard[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19.20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};

struct nasfd nasfd_drives[MAX_NASFD];

/*
 *	Calculate the drive control value and if need be update it
 */
static uint8_t nasfd_select(uint8_t drive, uint8_t flags)
{
	uint8_t ret = 1;

	if (drive_last == drive && flags_last == flags)
		return 0;

	if (drive_last != drive) {
		if (drive_last != 0xFF)
			nasfd_drives[drive_last].track = nasfd_track;
		drive_last = drive;
		nasfd_track = nasfd_drives[drive].track;
		nasfd_steprate = nasfd_drives[drive].steprate;
		ret = 2;
	}

	flags_last = flags;

	drive = 1 << drive;
	if (flags & SIDE)
		drive |= 0x10;
	if (flags & DDENS)
		drive |= 0x40;
	nasfd_cursel = drive;
	nasfd_ctrl = drive;
	return ret;
}

/*
 *	We only support normal block I/O for the moment. We do need to
 *	add swapping!
 */

static int nasfd_transfer(uint8_t minor, bool is_read, uint8_t rawflag)
{
	int ct = 0;
	int tries;
	uint8_t err = 0;
	uint8_t side, sector, track;
	irqflags_t irqflags;
	struct nasfd *fd = nasfd_drives + minor;

	if (rawflag == 2)
		goto bad2;

	/* Translate everything into physical sectors. d_blkoff does the work
	   for raw I/O we do it for normal block I/O */
	if (rawflag) {
		io_page = udata.u_page;
		if (d_blkoff(fd->bs))
			return -1;
	} else {
		io_page = 0;
		udata.u_nblock <<= fd->bs;
		udata.u_block <<= fd->bs;
	}

	/* Loop through each logical sector translating it into a head/track/sector
	   and then attempting to do the I/O a few times */
	while (ct < udata.u_nblock) {
		side = 0;
		sector = fd->skewtab[udata.u_block % fd->spt];
		track = udata.u_block / fd->spt;
		if (sector > fd->ds) {
			sector -= fd->ds;
			side = SIDE;
		}
		/* Set the drive parameters, also pokes the motor */
		nasfd_select(minor, fd->dens | side);
		/* TODO - any delays ?? */

		/* Make multiple attempts to get the data. If it keeps failing try
		   restoring the head and seeking in order to re-align */
		for (tries = 0; tries < 5; tries++) {
			/* Try to get the requested track */
			if (nasfd_track != track) {
				if ((err = nasfd_seek(track))) {
					nasfd_restore();
					continue;
				}
			}
			/* The timing on these is too tight to do with interrupts on */
			irqflags = di();
			if (is_read)
				err = nasfd_ioread(udata.u_dptr);
			else
				err = nasfd_iowrite(udata.u_dptr);
			irqrestore(irqflags);

			/* It worked - exit then inner retry loop and move on */
			if (err == 0)
				break;
			/* Force a head seek */
			if (tries > 1)
				nasfd_restore();
		}
		if (tries == 5)
			goto bad;
		/* Move on a sector */
		udata.u_block++;
		udata.u_dptr += fd->ss;
		ct++;
	}

	/* Data read in bytes */
	return udata.u_nblock << (9 - fd->bs);
bad:
	kprintf("fd%d: error %x\n", minor, err);
bad2:
	udata.u_error = EIO;
	return -1;
}

uint8_t nasfd_density(uint8_t minor, uint8_t flags)
{
	/* Try double density */
	nasfd_select(minor, DDENS | flags);
	if (nasfd_restore_test() == 0)
		return DDENS | flags;
	/* Try single density */
	nasfd_select(minor, flags);
	if (nasfd_restore_test() == 0)
		return flags;
	return 255;
}

int nasfd_open(uint8_t minor, uint16_t flag)
{
	uint8_t den;
	struct nasfd *d = nasfd_drives + minor;

	flag;
	if (((nasfd_type & 0x80) && minor > 4) || minor > MAX_NASFD) {
		udata.u_error = ENODEV;
		return -1;
	}
	if ((den = nasfd_density(minor, d->dens)) == 255 && !(flag & O_NDELAY)) {
		udata.u_error = -EIO;
		return -1;
	}
	/* FIXME: how to detect double sided ? */
	d->dens = den;

	/* Default media types need to add switching ioctls yet */
	/* Once we also have the media geometry info in the superblock
	   it'll get a *lot* easier */
	/* Also need to add soft skewing ioctl */
	d->bs = 0;
	d->ss = 512;
	d->ds = 0;
	memcpy(d->skewtab, skew_hard, MAX_SKEW);
	switch (den) {
	case 0:
		/* 18 spt 128bps double sided */
		d->spt = 36;
		d->bs = 2;
		break;
	case DDENS:
	case 255:
		/* IBM PC style 5.25" double density is 18/9 but Nascom like many
		   other systems use 20/10 */
		d->spt = 20;
		break;
	}
	d->ss >>= d->bs;
	if (!d->ds)
		d->ds = d->spt / 2;
	return 0;
}

int nasfd_read(uint8_t minor, uint8_t rawflag, uint8_t flag)
{
	flag;
	return nasfd_transfer(minor, true, rawflag);
}

int nasfd_write(uint8_t minor, uint8_t rawflag, uint8_t flag)
{
	flag;
	rawflag;
	minor;
	return nasfd_transfer(minor, false, rawflag);
}


/* TODO discard routine to init this lot */