#include "c.h"
#include "pgtar.h"
#include <sys/stat.h>

/*
 * Utility routine to print possibly larger than 32 bit integers in a
 * portable fashion.  Filled with zeros.
 */
static void
print_val(char *s, uint64 val, unsigned int base, size_t len)
{
	int			i;

	for (i = len; i > 0; i--)
	{
		int			digit = val % base;

		s[i - 1] = '0' + digit;
		val = val / base;
	}
}


/*
 * Calculate the tar checksum for a header. The header is assumed to always
 * be 512 bytes, per the tar standard.
 */
int
tarChecksum(char *header)
{
	int			i,
				sum;

	/*
	 * Per POSIX, the checksum is the simple sum of all bytes in the header,
	 * treating the bytes as unsigned, and treating the checksum field (at
	 * offset 148) as though it contained 8 spaces.
	 */
	sum = 8 * ' ';				/* presumed value for checksum field */
	for (i = 0; i < 512; i++)
		if (i < 148 || i >= 156)
			sum += 0xFF & header[i];
	return sum;
}


/*
 * Fill in the buffer pointed to by h with a tar format header. This buffer
 * must always have space for 512 characters, which is a requirement by
 * the tar format.
 */
void
tarCreateHeader(char *h, const char *filename, const char *linktarget,
				size_t size, mode_t mode, uid_t uid, gid_t gid, time_t mtime)
{
	/*
	 * Note: most of the fields in a tar header are not supposed to be
	 * null-terminated.  We use sprintf, which will write a null after the
	 * required bytes; that null goes into the first byte of the next field.
	 * This is okay as long as we fill the fields in order.
	 */
	memset(h, 0, 512);			/* assume tar header size */

	/* Name 100 */
	sprintf(&h[0], "%.99s", filename);
	if (linktarget != NULL || S_ISDIR(mode))
	{
		/*
		 * We only support symbolic links to directories, and this is
		 * indicated in the tar format by adding a slash at the end of the
		 * name, the same as for regular directories.
		 */
		int			flen = strlen(filename);

		flen = Min(flen, 99);
		h[flen] = '/';
		h[flen + 1] = '\0';
	}

	/* Mode 8 */
	sprintf(&h[100], "%07o ", (int) mode);

	/* User ID 8 */
	sprintf(&h[108], "%07o ", (int) uid);

	/* Group 8 */
	sprintf(&h[116], "%07o ", (int) gid);

	/* File size 12 - 11 digits, 1 space; use print_val for 64 bit support */
	if (linktarget != NULL || S_ISDIR(mode))
		/* Symbolic link or directory has size zero */
		print_val(&h[124], 0, 8, 11);
	else
		print_val(&h[124], size, 8, 11);
	sprintf(&h[135], " ");

	/* Mod Time 12 */
	sprintf(&h[136], "%011o ", (int) mtime);

	/* Checksum 8 cannot be calculated until we've filled all other fields */

	if (linktarget != NULL)
	{
		/* Type - Symbolic link */
		sprintf(&h[156], "2");
		/* Link Name 100 */
		sprintf(&h[157], "%.99s", linktarget);
	}
	else if (S_ISDIR(mode))
		/* Type - directory */
		sprintf(&h[156], "5");
	else
		/* Type - regular file */
		sprintf(&h[156], "0");

	/* Magic 6 */
	sprintf(&h[257], "ustar");

	/* Version 2 */
	sprintf(&h[263], "00");

	/* User 32 */
	/* XXX: Do we need to care about setting correct username? */
	sprintf(&h[265], "%.31s", "postgres");

	/* Group 32 */
	/* XXX: Do we need to care about setting correct group name? */
	sprintf(&h[297], "%.31s", "postgres");

	/* Major Dev 8 */
	sprintf(&h[329], "%07o ", 0);

	/* Minor Dev 8 */
	sprintf(&h[337], "%07o ", 0);

	/* Prefix 155 - not used, leave as nulls */

	/*
	 * We mustn't overwrite the next field while inserting the checksum.
	 * Fortunately, the checksum can't exceed 6 octal digits, so we just write
	 * 6 digits, a space, and a null, which is legal per POSIX.
	 */
	sprintf(&h[148], "%06o ", tarChecksum(h));
}
