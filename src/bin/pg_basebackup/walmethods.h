/*-------------------------------------------------------------------------
 *
 * walmethods.h
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/walmethods.h
 *-------------------------------------------------------------------------
 */

#include "common/compression.h"

struct WalWriteMethod;
typedef struct WalWriteMethod WalWriteMethod;

typedef struct
{
	WalWriteMethod *wwmethod;
	pgoff_t		currpos;
	char	   *pathname;

	/*
	 * MORE DATA FOLLOWS AT END OF STRUCT
	 *
	 * Each WalWriteMethod is expected to embed this as the first member of a
	 * larger struct with method-specific fields following.
	 */
} Walfile;

typedef enum
{
	CLOSE_NORMAL,
	CLOSE_UNLINK,
	CLOSE_NO_RENAME,
} WalCloseMethod;

/*
 * Table of callbacks for a WalWriteMethod.
 */
typedef struct WalWriteMethodOps
{
	/*
	 * Open a target file. Returns Walfile, or NULL if open failed. If a temp
	 * suffix is specified, a file with that name will be opened, and then
	 * automatically renamed in close(). If pad_to_size is specified, the file
	 * will be padded with NUL up to that size, if supported by the Walmethod.
	 */
	Walfile    *(*open_for_write) (WalWriteMethod *wwmethod, const char *pathname, const char *temp_suffix, size_t pad_to_size);

	/*
	 * Close an open Walfile, using one or more methods for handling automatic
	 * unlinking etc. Returns 0 on success, other values for error.
	 */
	int			(*close) (Walfile *f, WalCloseMethod method);

	/* Check if a file exist */
	bool		(*existsfile) (WalWriteMethod *wwmethod, const char *pathname);

	/* Return the size of a file, or -1 on failure. */
	ssize_t		(*get_file_size) (WalWriteMethod *wwmethod, const char *pathname);

	/*
	 * Return the name of the current file to work on in pg_malloc()'d string,
	 * without the base directory.  This is useful for logging.
	 */
	char	   *(*get_file_name) (WalWriteMethod *wwmethod, const char *pathname, const char *temp_suffix);

	/*
	 * Write count number of bytes to the file, and return the number of bytes
	 * actually written or -1 for error.
	 */
	ssize_t		(*write) (Walfile *f, const void *buf, size_t count);

	/*
	 * fsync the contents of the specified file. Returns 0 on success.
	 */
	int			(*sync) (Walfile *f);

	/*
	 * Clean up the Walmethod, closing any shared resources. For methods like
	 * tar, this includes writing updated headers. Returns true if the
	 * close/write/sync of shared resources succeeded, otherwise returns false
	 * (but the resources are still closed).
	 */
	bool		(*finish) (WalWriteMethod *wwmethod);

	/*
	 * Free subsidiary data associated with the WalWriteMethod, and the
	 * WalWriteMethod itself.
	 */
	void		(*free) (WalWriteMethod *wwmethod);
} WalWriteMethodOps;

/*
 * A WalWriteMethod structure represents a way of writing streaming WAL as
 * it's received.
 *
 * All methods that have a failure return indicator will set lasterrstring
 * or lasterrno (the former takes precedence) so that the caller can signal
 * a suitable error.
 */
struct WalWriteMethod
{
	const WalWriteMethodOps *ops;
	pg_compress_algorithm compression_algorithm;
	int			compression_level;
	bool		sync;
	const char *lasterrstring;	/* if set, takes precedence over lasterrno */
	int			lasterrno;

	/*
	 * MORE DATA FOLLOWS AT END OF STRUCT
	 *
	 * Each WalWriteMethod is expected to embed this as the first member of a
	 * larger struct with method-specific fields following.
	 */
};

/*
 * Available WAL methods:
 *	- WalDirectoryMethod - write WAL to regular files in a standard pg_wal
 *	- WalTarMethod       - write WAL to a tarfile corresponding to pg_wal
 *						   (only implements the methods required for pg_basebackup,
 *						   not all those required for pg_receivewal)
 */
WalWriteMethod *CreateWalDirectoryMethod(const char *basedir,
										 pg_compress_algorithm compression_algorithm,
										 int compression_level, bool sync);
WalWriteMethod *CreateWalTarMethod(const char *tarbase,
								   pg_compress_algorithm compression_algorithm,
								   int compression_level, bool sync);

const char *GetLastWalMethodError(WalWriteMethod *wwmethod);
