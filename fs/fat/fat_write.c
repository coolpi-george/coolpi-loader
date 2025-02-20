// SPDX-License-Identifier: GPL-2.0+
/*
 * fat_write.c
 *
 * R/W (V)FAT 12/16/32 filesystem implementation by Donggeun Kim
 */

#include <common.h>
#include <command.h>
#include <config.h>
#include <fat.h>
#include <asm/byteorder.h>
#include <part.h>
#include <linux/ctype.h>
#include <div64.h>
#include <linux/math64.h>
#include "fat.c"

static void uppercase(char *str, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		*str = toupper(*str);
		str++;
	}
}

static int total_sector;
static int disk_write(__u32 block, __u32 nr_blocks, void *buf)
{
	ulong ret;

	if (!cur_dev)
		return -1;

	if (cur_part_info.start + block + nr_blocks >
		cur_part_info.start + total_sector) {
		printf("error: overflow occurs\n");
		return -1;
	}

	ret = blk_dwrite(cur_dev, cur_part_info.start + block, nr_blocks, buf);
	if (nr_blocks && ret == 0)
		return -1;

	return ret;
}

/*
 * Set short name in directory entry
 */
static void set_name(dir_entry *dirent, const char *filename)
{
	char s_name[VFAT_MAXLEN_BYTES];
	char *period;
	int period_location, len, i, ext_num;

	if (filename == NULL)
		return;

	len = strlen(filename);
	if (len == 0)
		return;

	strcpy(s_name, filename);
	uppercase(s_name, len);

	period = strchr(s_name, '.');
	if (period == NULL) {
		period_location = len;
		ext_num = 0;
	} else {
		period_location = period - s_name;
		ext_num = len - period_location - 1;
	}

	/* Pad spaces when the length of file name is shorter than eight */
	if (period_location < 8) {
		memcpy(dirent->name, s_name, period_location);
		for (i = period_location; i < 8; i++)
			dirent->name[i] = ' ';
	} else if (period_location == 8) {
		memcpy(dirent->name, s_name, period_location);
	} else {
		memcpy(dirent->name, s_name, 6);
		dirent->name[6] = '~';
		dirent->name[7] = '1';
	}

	if (ext_num < 3) {
		memcpy(dirent->ext, s_name + period_location + 1, ext_num);
		for (i = ext_num; i < 3; i++)
			dirent->ext[i] = ' ';
	} else
		memcpy(dirent->ext, s_name + period_location + 1, 3);

	debug("name : %s\n", dirent->name);
	debug("ext : %s\n", dirent->ext);
}

/*
 * Write fat buffer into block device
 */
static int flush_dirty_fat_buffer(fsdata *mydata)
{
	int getsize = FATBUFBLOCKS;
	__u32 fatlength = mydata->fatlength;
	__u8 *bufptr = mydata->fatbuf;
	__u32 startblock = mydata->fatbufnum * FATBUFBLOCKS;

	debug("debug: evicting %d, dirty: %d\n", mydata->fatbufnum,
	      (int)mydata->fat_dirty);

	if ((!mydata->fat_dirty) || (mydata->fatbufnum == -1))
		return 0;

	/* Cap length if fatlength is not a multiple of FATBUFBLOCKS */
	if (startblock + getsize > fatlength)
		getsize = fatlength - startblock;

	startblock += mydata->fat_sect;

	/* Write FAT buf */
	if (disk_write(startblock, getsize, bufptr) < 0) {
		debug("error: writing FAT blocks\n");
		return -1;
	}

	if (mydata->fats == 2) {
		/* Update corresponding second FAT blocks */
		startblock += mydata->fatlength;
		if (disk_write(startblock, getsize, bufptr) < 0) {
			debug("error: writing second FAT blocks\n");
			return -1;
		}
	}
	mydata->fat_dirty = 0;

	return 0;
}

/*
 * Set the file name information from 'name' into 'slotptr',
 */
static int str2slot(dir_slot *slotptr, const char *name, int *idx)
{
	int j, end_idx = 0;

	for (j = 0; j <= 8; j += 2) {
		if (name[*idx] == 0x00) {
			slotptr->name0_4[j] = 0;
			slotptr->name0_4[j + 1] = 0;
			end_idx++;
			goto name0_4;
		}
		slotptr->name0_4[j] = name[*idx];
		(*idx)++;
		end_idx++;
	}
	for (j = 0; j <= 10; j += 2) {
		if (name[*idx] == 0x00) {
			slotptr->name5_10[j] = 0;
			slotptr->name5_10[j + 1] = 0;
			end_idx++;
			goto name5_10;
		}
		slotptr->name5_10[j] = name[*idx];
		(*idx)++;
		end_idx++;
	}
	for (j = 0; j <= 2; j += 2) {
		if (name[*idx] == 0x00) {
			slotptr->name11_12[j] = 0;
			slotptr->name11_12[j + 1] = 0;
			end_idx++;
			goto name11_12;
		}
		slotptr->name11_12[j] = name[*idx];
		(*idx)++;
		end_idx++;
	}

	if (name[*idx] == 0x00)
		return 1;

	return 0;
/* Not used characters are filled with 0xff 0xff */
name0_4:
	for (; end_idx < 5; end_idx++) {
		slotptr->name0_4[end_idx * 2] = 0xff;
		slotptr->name0_4[end_idx * 2 + 1] = 0xff;
	}
	end_idx = 5;
name5_10:
	end_idx -= 5;
	for (; end_idx < 6; end_idx++) {
		slotptr->name5_10[end_idx * 2] = 0xff;
		slotptr->name5_10[end_idx * 2 + 1] = 0xff;
	}
	end_idx = 11;
name11_12:
	end_idx -= 11;
	for (; end_idx < 2; end_idx++) {
		slotptr->name11_12[end_idx * 2] = 0xff;
		slotptr->name11_12[end_idx * 2 + 1] = 0xff;
	}

	return 1;
}

static int flush_dir_table(fat_itr *itr);

/*
 * Fill dir_slot entries with appropriate name, id, and attr
 * 'itr' will point to a next entry
 */
static int
fill_dir_slot(fat_itr *itr, const char *l_name)
{
	__u8 temp_dir_slot_buffer[MAX_LFN_SLOT * sizeof(dir_slot)];
	dir_slot *slotptr = (dir_slot *)temp_dir_slot_buffer;
	__u8 counter = 0, checksum;
	int idx = 0, ret;

	/* Get short file name checksum value */
	checksum = mkcksum(itr->dent->name, itr->dent->ext);

	do {
		memset(slotptr, 0x00, sizeof(dir_slot));
		ret = str2slot(slotptr, l_name, &idx);
		slotptr->id = ++counter;
		slotptr->attr = ATTR_VFAT;
		slotptr->alias_checksum = checksum;
		slotptr++;
	} while (ret == 0);

	slotptr--;
	slotptr->id |= LAST_LONG_ENTRY_MASK;

	while (counter >= 1) {
		memcpy(itr->dent, slotptr, sizeof(dir_slot));
		slotptr--;
		counter--;
		if (!fat_itr_next(itr))
			if (!itr->dent && !itr->is_root && flush_dir_table(itr))
				return -1;
	}

	if (!itr->dent && !itr->is_root)
		/*
		 * don't care return value here because we have already
		 * finished completing an entry with name, only ending up
		 * no more entry left
		 */
		flush_dir_table(itr);

	return 0;
}

/*
 * Set the entry at index 'entry' in a FAT (12/16/32) table.
 */
static int set_fatent_value(fsdata *mydata, __u32 entry, __u32 entry_value)
{
	__u32 bufnum, offset, off16;
	__u16 val1, val2;

	switch (mydata->fatsize) {
	case 32:
		bufnum = entry / FAT32BUFSIZE;
		offset = entry - bufnum * FAT32BUFSIZE;
		break;
	case 16:
		bufnum = entry / FAT16BUFSIZE;
		offset = entry - bufnum * FAT16BUFSIZE;
		break;
	case 12:
		bufnum = entry / FAT12BUFSIZE;
		offset = entry - bufnum * FAT12BUFSIZE;
		break;
	default:
		/* Unsupported FAT size */
		return -1;
	}

	/* Read a new block of FAT entries into the cache. */
	if (bufnum != mydata->fatbufnum) {
		int getsize = FATBUFBLOCKS;
		__u8 *bufptr = mydata->fatbuf;
		__u32 fatlength = mydata->fatlength;
		__u32 startblock = bufnum * FATBUFBLOCKS;

		/* Cap length if fatlength is not a multiple of FATBUFBLOCKS */
		if (startblock + getsize > fatlength)
			getsize = fatlength - startblock;

		if (flush_dirty_fat_buffer(mydata) < 0)
			return -1;

		startblock += mydata->fat_sect;

		if (disk_read(startblock, getsize, bufptr) < 0) {
			debug("Error reading FAT blocks\n");
			return -1;
		}
		mydata->fatbufnum = bufnum;
	}

	/* Mark as dirty */
	mydata->fat_dirty = 1;

	/* Set the actual entry */
	switch (mydata->fatsize) {
	case 32:
		((__u32 *) mydata->fatbuf)[offset] = cpu_to_le32(entry_value);
		break;
	case 16:
		((__u16 *) mydata->fatbuf)[offset] = cpu_to_le16(entry_value);
		break;
	case 12:
		off16 = (offset * 3) / 4;

		switch (offset & 0x3) {
		case 0:
			val1 = cpu_to_le16(entry_value) & 0xfff;
			((__u16 *)mydata->fatbuf)[off16] &= ~0xfff;
			((__u16 *)mydata->fatbuf)[off16] |= val1;
			break;
		case 1:
			val1 = cpu_to_le16(entry_value) & 0xf;
			val2 = (cpu_to_le16(entry_value) >> 4) & 0xff;

			((__u16 *)mydata->fatbuf)[off16] &= ~0xf000;
			((__u16 *)mydata->fatbuf)[off16] |= (val1 << 12);

			((__u16 *)mydata->fatbuf)[off16 + 1] &= ~0xff;
			((__u16 *)mydata->fatbuf)[off16 + 1] |= val2;
			break;
		case 2:
			val1 = cpu_to_le16(entry_value) & 0xff;
			val2 = (cpu_to_le16(entry_value) >> 8) & 0xf;

			((__u16 *)mydata->fatbuf)[off16] &= ~0xff00;
			((__u16 *)mydata->fatbuf)[off16] |= (val1 << 8);

			((__u16 *)mydata->fatbuf)[off16 + 1] &= ~0xf;
			((__u16 *)mydata->fatbuf)[off16 + 1] |= val2;
			break;
		case 3:
			val1 = cpu_to_le16(entry_value) & 0xfff;
			((__u16 *)mydata->fatbuf)[off16] &= ~0xfff0;
			((__u16 *)mydata->fatbuf)[off16] |= (val1 << 4);
			break;
		default:
			break;
		}

		break;
	default:
		return -1;
	}

	return 0;
}

/*
 * Determine the next free cluster after 'entry' in a FAT (12/16/32) table
 * and link it to 'entry'. EOC marker is not set on returned entry.
 */
static __u32 determine_fatent(fsdata *mydata, __u32 entry)
{
	__u32 next_fat, next_entry = entry + 1;

	while (1) {
		next_fat = get_fatent(mydata, next_entry);
		if (next_fat == 0) {
			/* found free entry, link to entry */
			set_fatent_value(mydata, entry, next_entry);
			break;
		}
		next_entry++;
	}
	debug("FAT%d: entry: %08x, entry_value: %04x\n",
	       mydata->fatsize, entry, next_entry);

	return next_entry;
}

/*
 * Write at most 'size' bytes from 'buffer' into the specified cluster.
 * Return 0 on success, -1 otherwise.
 */
static int
set_cluster(fsdata *mydata, __u32 clustnum, __u8 *buffer,
	     unsigned long size)
{
	__u32 idx = 0;
	__u32 startsect;
	int ret;

	if (clustnum > 0)
		startsect = clust_to_sect(mydata, clustnum);
	else
		startsect = mydata->rootdir_sect;

	debug("clustnum: %d, startsect: %d\n", clustnum, startsect);

	if ((unsigned long)buffer & (ARCH_DMA_MINALIGN - 1)) {
		ALLOC_CACHE_ALIGN_BUFFER(__u8, tmpbuf, mydata->sect_size);

		printf("FAT: Misaligned buffer address (%p)\n", buffer);

		while (size >= mydata->sect_size) {
			memcpy(tmpbuf, buffer, mydata->sect_size);
			ret = disk_write(startsect++, 1, tmpbuf);
			if (ret != 1) {
				debug("Error writing data (got %d)\n", ret);
				return -1;
			}

			buffer += mydata->sect_size;
			size -= mydata->sect_size;
		}
	} else if (size >= mydata->sect_size) {
		idx = size / mydata->sect_size;
		ret = disk_write(startsect, idx, buffer);
		if (ret != idx) {
			debug("Error writing data (got %d)\n", ret);
			return -1;
		}

		startsect += idx;
		idx *= mydata->sect_size;
		buffer += idx;
		size -= idx;
	}

	if (size) {
		ALLOC_CACHE_ALIGN_BUFFER(__u8, tmpbuf, mydata->sect_size);

		memcpy(tmpbuf, buffer, size);
		ret = disk_write(startsect, 1, tmpbuf);
		if (ret != 1) {
			debug("Error writing data (got %d)\n", ret);
			return -1;
		}
	}

	return 0;
}

/*
 * Find the first empty cluster
 */
static int find_empty_cluster(fsdata *mydata)
{
	__u32 fat_val, entry = 3;

	while (1) {
		fat_val = get_fatent(mydata, entry);
		if (fat_val == 0)
			break;
		entry++;
	}

	return entry;
}

/*
 * Write directory entries in itr's buffer to block device
 */
static int flush_dir_table(fat_itr *itr)
{
	fsdata *mydata = itr->fsdata;
	int dir_newclust = 0;
	unsigned int bytesperclust = mydata->clust_size * mydata->sect_size;

	if (set_cluster(mydata, itr->clust, itr->block, bytesperclust) != 0) {
		printf("error: writing directory entry\n");
		return -1;
	}
	dir_newclust = find_empty_cluster(mydata);
	set_fatent_value(mydata, itr->clust, dir_newclust);
	if (mydata->fatsize == 32)
		set_fatent_value(mydata, dir_newclust, 0xffffff8);
	else if (mydata->fatsize == 16)
		set_fatent_value(mydata, dir_newclust, 0xfff8);
	else if (mydata->fatsize == 12)
		set_fatent_value(mydata, dir_newclust, 0xff8);

	itr->clust = dir_newclust;
	itr->next_clust = dir_newclust;

	if (flush_dirty_fat_buffer(mydata) < 0)
		return -1;

	memset(itr->block, 0x00, bytesperclust);

	itr->dent = (dir_entry *)itr->block;
	itr->last_cluster = 1;
	itr->remaining = bytesperclust / sizeof(dir_entry) - 1;

	return 0;
}

/*
 * Set empty cluster from 'entry' to the end of a file
 */
static int clear_fatent(fsdata *mydata, __u32 entry)
{
	__u32 fat_val;

	while (!CHECK_CLUST(entry, mydata->fatsize)) {
		fat_val = get_fatent(mydata, entry);
		if (fat_val != 0)
			set_fatent_value(mydata, entry, 0);
		else
			break;

		entry = fat_val;
	}

	/* Flush fat buffer */
	if (flush_dirty_fat_buffer(mydata) < 0)
		return -1;

	return 0;
}

/*
 * Write at most 'maxsize' bytes from 'buffer' into
 * the file associated with 'dentptr'
 * Update the number of bytes written in *gotsize and return 0
 * or return -1 on fatal errors.
 */
static int
set_contents(fsdata *mydata, dir_entry *dentptr, __u8 *buffer,
	      loff_t maxsize, loff_t *gotsize)
{
	loff_t filesize = FAT2CPU32(dentptr->size);
	unsigned int bytesperclust = mydata->clust_size * mydata->sect_size;
	__u32 curclust = START(dentptr);
	__u32 endclust = 0, newclust = 0;
	loff_t actsize;

	*gotsize = 0;
	debug("Filesize: %llu bytes\n", filesize);

	if (maxsize > 0 && filesize > maxsize)
		filesize = maxsize;

	debug("%llu bytes\n", filesize);

	if (!curclust) {
		if (filesize) {
			debug("error: nonempty clusterless file!\n");
			return -1;
		}
		return 0;
	}

	actsize = bytesperclust;
	endclust = curclust;
	do {
		/* search for consecutive clusters */
		while (actsize < filesize) {
			newclust = determine_fatent(mydata, endclust);

			if ((newclust - 1) != endclust)
				goto getit;

			if (CHECK_CLUST(newclust, mydata->fatsize)) {
				debug("newclust: 0x%x\n", newclust);
				debug("Invalid FAT entry\n");
				return 0;
			}
			endclust = newclust;
			actsize += bytesperclust;
		}

		/* set remaining bytes */
		actsize = filesize;
		if (set_cluster(mydata, curclust, buffer, (int)actsize) != 0) {
			debug("error: writing cluster\n");
			return -1;
		}
		*gotsize += actsize;

		/* Mark end of file in FAT */
		if (mydata->fatsize == 12)
			newclust = 0xfff;
		else if (mydata->fatsize == 16)
			newclust = 0xffff;
		else if (mydata->fatsize == 32)
			newclust = 0xfffffff;
		set_fatent_value(mydata, endclust, newclust);

		return 0;
getit:
		if (set_cluster(mydata, curclust, buffer, (int)actsize) != 0) {
			debug("error: writing cluster\n");
			return -1;
		}
		*gotsize += actsize;
		filesize -= actsize;
		buffer += actsize;

		if (CHECK_CLUST(newclust, mydata->fatsize)) {
			debug("newclust: 0x%x\n", newclust);
			debug("Invalid FAT entry\n");
			return 0;
		}
		actsize = bytesperclust;
		curclust = endclust = newclust;
	} while (1);
}

/*
 * Set start cluster in directory entry
 */
static void set_start_cluster(const fsdata *mydata, dir_entry *dentptr,
				__u32 start_cluster)
{
	if (mydata->fatsize == 32)
		dentptr->starthi =
			cpu_to_le16((start_cluster & 0xffff0000) >> 16);
	dentptr->start = cpu_to_le16(start_cluster & 0xffff);
}

/*
 * Fill dir_entry
 */
static void fill_dentry(fsdata *mydata, dir_entry *dentptr,
	const char *filename, __u32 start_cluster, __u32 size, __u8 attr)
{
	set_start_cluster(mydata, dentptr, start_cluster);
	dentptr->size = cpu_to_le32(size);

	dentptr->attr = attr;

	set_name(dentptr, filename);
}

/*
 * Check whether adding a file makes the file system to
 * exceed the size of the block device
 * Return -1 when overflow occurs, otherwise return 0
 */
static int check_overflow(fsdata *mydata, __u32 clustnum, loff_t size)
{
	__u32 startsect, sect_num, offset;

	if (clustnum > 0) {
		startsect = clust_to_sect(mydata, clustnum);
	} else {
		startsect = mydata->rootdir_sect;
	}

	sect_num = div_u64_rem(size, mydata->sect_size, &offset);

	if (offset != 0)
		sect_num++;

	if (startsect + sect_num > total_sector)
		return -1;
	return 0;
}

/*
 * Find a directory entry based on filename or start cluster number
 * If the directory entry is not found,
 * the new position for writing a directory entry will be returned
 */
static dir_entry *find_directory_entry(fat_itr *itr, char *filename)
{
	int match = 0;

	while (fat_itr_next(itr)) {
		/* check both long and short name: */
		if (!strcasecmp(filename, itr->name))
			match = 1;
		else if (itr->name != itr->s_name &&
			 !strcasecmp(filename, itr->s_name))
			match = 1;

		if (!match)
			continue;

		if (itr->dent->name[0] == '\0')
			return NULL;
		else
			return itr->dent;
	}

	if (!itr->dent && !itr->is_root && flush_dir_table(itr))
		/* indicate that allocating dent failed */
		itr->dent = NULL;

	return NULL;
}

static int split_filename(char *filename, char **dirname, char **basename)
{
	char *p, *last_slash, *last_slash_cont;

again:
	p = filename;
	last_slash = NULL;
	last_slash_cont = NULL;
	while (*p) {
		if (ISDIRDELIM(*p)) {
			last_slash = p;
			last_slash_cont = p;
			/* continuous slashes */
			while (ISDIRDELIM(*p))
				last_slash_cont = p++;
			if (!*p)
				break;
		}
		p++;
	}

	if (last_slash) {
		if (last_slash_cont == (filename + strlen(filename) - 1)) {
			/* remove trailing slashes */
			*last_slash = '\0';
			goto again;
		}

		if (last_slash == filename) {
			/* avoid ""(null) directory */
			*dirname = "/";
		} else {
			*last_slash = '\0';
			*dirname = filename;
		}

		*last_slash_cont = '\0';
		*basename = last_slash_cont + 1;
	} else {
		*dirname = "/"; /* root by default */
		*basename = filename;
	}

	return 0;
}

static int normalize_longname(char *l_filename, const char *filename)
{
	const char *p, legal[] = "!#$%&\'()-.@^`_{}~";
	char c;
	int name_len;

	/* Check that the filename is valid */
	for (p = filename; p < filename + strlen(filename); p++) {
		c = *p;

		if (('0' <= c) && (c <= '9'))
			continue;
		if (('A' <= c) && (c <= 'Z'))
			continue;
		if (('a' <= c) && (c <= 'z'))
			continue;
		if (strchr(legal, c))
			continue;
		/* extended code */
		if ((0x80 <= c) && (c <= 0xff))
			continue;

		return -1;
	}

	/* Normalize it */
	name_len = strlen(filename);
	if (name_len >= VFAT_MAXLEN_BYTES)
		/* should return an error? */
		name_len = VFAT_MAXLEN_BYTES - 1;

	memcpy(l_filename, filename, name_len);
	l_filename[name_len] = 0; /* terminate the string */
	downcase(l_filename, INT_MAX);

	return 0;
}

static int do_fat_write(const char *filename, void *buffer, loff_t size,
			loff_t *actwrite)
{
	dir_entry *retdent;
	__u32 start_cluster;
	fsdata datablock = { .fatbuf = NULL, };
	fsdata *mydata = &datablock;
	fat_itr *itr = NULL;
	int ret = -1;
	char *filename_copy, *parent, *basename;
	char l_filename[VFAT_MAXLEN_BYTES];

	filename_copy = strdup(filename);
	if (!filename_copy)
		return -ENOMEM;

	split_filename(filename_copy, &parent, &basename);
	if (!strlen(basename)) {
		ret = -EINVAL;
		goto exit;
	}

	filename = basename;
	if (normalize_longname(l_filename, filename)) {
		printf("FAT: illegal filename (%s)\n", filename);
		ret = -EINVAL;
		goto exit;
	}

	itr = malloc_cache_aligned(sizeof(fat_itr));
	if (!itr) {
		ret = -ENOMEM;
		goto exit;
	}

	ret = fat_itr_root(itr, &datablock);
	if (ret)
		goto exit;

	total_sector = datablock.total_sect;

	ret = fat_itr_resolve(itr, parent, TYPE_DIR);
	if (ret) {
		printf("%s: doesn't exist (%d)\n", parent, ret);
		goto exit;
	}

	retdent = find_directory_entry(itr, l_filename);

	if (retdent) {
		if (fat_itr_isdir(itr)) {
			ret = -EISDIR;
			goto exit;
		}

		/* Update file size and start_cluster in a directory entry */
		retdent->size = cpu_to_le32(size);
		start_cluster = START(retdent);

		if (start_cluster) {
			if (size) {
				ret = check_overflow(mydata, start_cluster,
							size);
				if (ret) {
					printf("Error: %llu overflow\n", size);
					ret = -ENOSPC;
					goto exit;
				}
			}

			ret = clear_fatent(mydata, start_cluster);
			if (ret) {
				printf("Error: clearing FAT entries\n");
				ret = -EIO;
				goto exit;
			}

			if (!size)
				set_start_cluster(mydata, retdent, 0);
		} else if (size) {
			ret = start_cluster = find_empty_cluster(mydata);
			if (ret < 0) {
				printf("Error: finding empty cluster\n");
				ret = -ENOSPC;
				goto exit;
			}

			ret = check_overflow(mydata, start_cluster, size);
			if (ret) {
				printf("Error: %llu overflow\n", size);
				ret = -ENOSPC;
				goto exit;
			}

			set_start_cluster(mydata, retdent, start_cluster);
		}
	} else {
		/* Create a new file */

		if (itr->is_root) {
			/* root dir cannot have "." or ".." */
			if (!strcmp(l_filename, ".") ||
			    !strcmp(l_filename, "..")) {
				ret = -EINVAL;
				goto exit;
			}
		}

		if (!itr->dent) {
			printf("Error: allocating new dir entry\n");
			ret = -EIO;
			goto exit;
		}

		memset(itr->dent, 0, sizeof(*itr->dent));

		/* Set short name to set alias checksum field in dir_slot */
		set_name(itr->dent, filename);
		if (fill_dir_slot(itr, filename)) {
			ret = -EIO;
			goto exit;
		}

		if (size) {
			ret = start_cluster = find_empty_cluster(mydata);
			if (ret < 0) {
				printf("Error: finding empty cluster\n");
				ret = -ENOSPC;
				goto exit;
			}

			ret = check_overflow(mydata, start_cluster, size);
			if (ret) {
				printf("Error: %llu overflow\n", size);
				ret = -ENOSPC;
				goto exit;
			}
		} else {
			start_cluster = 0;
		}

		/* Set attribute as archive for regular file */
		fill_dentry(itr->fsdata, itr->dent, filename,
			    start_cluster, size, 0x20);

		retdent = itr->dent;
	}

	ret = set_contents(mydata, retdent, buffer, size, actwrite);
	if (ret < 0) {
		printf("Error: writing contents\n");
		ret = -EIO;
		goto exit;
	}
	debug("attempt to write 0x%llx bytes\n", *actwrite);

	/* Flush fat buffer */
	ret = flush_dirty_fat_buffer(mydata);
	if (ret) {
		printf("Error: flush fat buffer\n");
		ret = -EIO;
		goto exit;
	}

	/* Write directory table to device */
	ret = set_cluster(mydata, itr->clust, itr->block,
			  mydata->clust_size * mydata->sect_size);
	if (ret) {
		printf("Error: writing directory entry\n");
		ret = -EIO;
	}

exit:
	free(filename_copy);
	free(mydata->fatbuf);
	free(itr);
	return ret;
}

int file_fat_write(const char *filename, void *buffer, loff_t offset,
		   loff_t maxsize, loff_t *actwrite)
{
	if (offset != 0) {
		printf("Error: non zero offset is currently not supported.\n");
		return -EINVAL;
	}

	printf("writing %s\n", filename);
	return do_fat_write(filename, buffer, maxsize, actwrite);
}
