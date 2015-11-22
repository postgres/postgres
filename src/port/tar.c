#include "c.h"
#include "pgtar.h"
#include <sys/stat.h>

/*
 * Print a numeric field in a tar header.  The field starts at *s and is of
 * length len; val is the value to be written.
 *
 * Per POSIX, the way to write a number is in octal with leading zeroes and
 * one trailing space (or NUL, but we use space) at the end of the specified
 * field width.
 *
 * However, the given value may not fit in the available space in octal form.
 * If that's true, we use the GNU extension of writing \200 followed by the
 * number in base-256 form (ie, stored in binary MSB-first).  (Note: here we
 * support only non-negative numbers, so we don't worry about the GNU rules
 * for handling negative numbers.)
 */
static void
print_tar_number(char *s, int len, uint64 val)
{
	if (val < (((uint64) 1) << ((len - 1) * 3)))
	{
		/* Use octal with trailing space */
		s[--len] = ' ';
		while (len)
		{
			s[--len] = (val & 7) + '0';
			val >>= 3;
		}
	}
	else
	{
		/* Use base-256 with leading \200 */
		s[0] = '\200';
		while (len > 1)
		{
			s[--len] = (val & 255);
			val >>= 8;
		}
	}
}


/*
 * Read a numeric field in a tar header.  The field starts at *s and is of
 * length len.
 *
 * The POSIX-approved format for a number is octal, ending with a space or
 * NUL.  However, for values that don't fit, we recognize the GNU extension
 * of \200 followed by the number in base-256 form (ie, stored in binary
 * MSB-first).  (Note: here we support only non-negative numbers, so we don't
 * worry about the GNU rules for handling negative numbers.)
 */
uint64
read_tar_number(const char *s, int len)
{
	uint64		result = 0;

	if (*s == '\200')
	{
		/* base-256 */
		while (--len)
		{
			result <<= 8;
			result |= (unsigned char) (*++s);
		}
	}
	else
	{
		/* octal */
		while (len-- && *s >= '0' && *s <= '7')
		{
			result <<= 3;
			result |= (*s - '0');
			s++;
		}
	}
	return result;
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
 * must always have space for 512 characters, which is a requirement of
 * the tar format.
 */
enum tarError
tarCreateHeader(char *h, const char *filename, const char *linktarget,
				pgoff_t size, mode_t mode, uid_t uid, gid_t gid, time_t mtime)
{
	if (strlen(filename) > 99)
		return TAR_NAME_TOO_LONG;

	if (linktarget && strlen(linktarget) > 99)
		return TAR_SYMLINK_TOO_LONG;

	memset(h, 0, 512);			/* assume tar header size */

	/* Name 100 */
	strlcpy(&h[0], filename, 100);
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

	/* Mode 8 - this doesn't include the file type bits (S_IFMT)  */
	print_tar_number(&h[100], 8, (mode & 07777));

	/* User ID 8 */
	print_tar_number(&h[108], 8, uid);

	/* Group 8 */
	print_tar_number(&h[116], 8, gid);

	/* File size 12 */
	if (linktarget != NULL || S_ISDIR(mode))
		/* Symbolic link or directory has size zero */
		print_tar_number(&h[124], 12, 0);
	else
		print_tar_number(&h[124], 12, size);

	/* Mod Time 12 */
	print_tar_number(&h[136], 12, mtime);

	/* Checksum 8 cannot be calculated until we've filled all other fields */

	if (linktarget != NULL)
	{
		/* Type - Symbolic link */
		h[156] = '2';
		/* Link Name 100 */
		strlcpy(&h[157], linktarget, 100);
	}
	else if (S_ISDIR(mode))
	{
		/* Type - directory */
		h[156] = '5';
	}
	else
	{
		/* Type - regular file */
		h[156] = '0';
	}

	/* Magic 6 */
	strcpy(&h[257], "ustar");

	/* Version 2 */
	memcpy(&h[263], "00", 2);

	/* User 32 */
	/* XXX: Do we need to care about setting correct username? */
	strlcpy(&h[265], "postgres", 32);

	/* Group 32 */
	/* XXX: Do we need to care about setting correct group name? */
	strlcpy(&h[297], "postgres", 32);

	/* Major Dev 8 */
	print_tar_number(&h[329], 8, 0);

	/* Minor Dev 8 */
	print_tar_number(&h[337], 8, 0);

	/* Prefix 155 - not used, leave as nulls */

	/* Finally, compute and insert the checksum */
	print_tar_number(&h[148], 8, tarChecksum(h));

	return TAR_OK;
}
