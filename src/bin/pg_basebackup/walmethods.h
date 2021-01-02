/*-------------------------------------------------------------------------
 *
 * walmethods.h
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/walmethods.h
 *-------------------------------------------------------------------------
 */


typedef void *Walfile;

typedef enum
{
	CLOSE_NORMAL,
	CLOSE_UNLINK,
	CLOSE_NO_RENAME
} WalCloseMethod;

/*
 * A WalWriteMethod structure represents the different methods used
 * to write the streaming WAL as it's received.
 *
 * All methods that have a failure return indicator will set state
 * allowing the getlasterror() method to return a suitable message.
 * Commonly, errno is this state (or part of it); so callers must take
 * care not to clobber errno between a failed method call and use of
 * getlasterror() to retrieve the message.
 */
typedef struct WalWriteMethod WalWriteMethod;
struct WalWriteMethod
{
	/*
	 * Open a target file. Returns Walfile, or NULL if open failed. If a temp
	 * suffix is specified, a file with that name will be opened, and then
	 * automatically renamed in close(). If pad_to_size is specified, the file
	 * will be padded with NUL up to that size, if supported by the Walmethod.
	 */
	Walfile		(*open_for_write) (const char *pathname, const char *temp_suffix, size_t pad_to_size);

	/*
	 * Close an open Walfile, using one or more methods for handling automatic
	 * unlinking etc. Returns 0 on success, other values for error.
	 */
	int			(*close) (Walfile f, WalCloseMethod method);

	/* Check if a file exist */
	bool		(*existsfile) (const char *pathname);

	/* Return the size of a file, or -1 on failure. */
	ssize_t		(*get_file_size) (const char *pathname);

	/*
	 * Write count number of bytes to the file, and return the number of bytes
	 * actually written or -1 for error.
	 */
	ssize_t		(*write) (Walfile f, const void *buf, size_t count);

	/* Return the current position in a file or -1 on error */
	off_t		(*get_current_pos) (Walfile f);

	/*
	 * fsync the contents of the specified file. Returns 0 on success.
	 */
	int			(*sync) (Walfile f);

	/*
	 * Clean up the Walmethod, closing any shared resources. For methods like
	 * tar, this includes writing updated headers. Returns true if the
	 * close/write/sync of shared resources succeeded, otherwise returns false
	 * (but the resources are still closed).
	 */
	bool		(*finish) (void);

	/* Return a text for the last error in this Walfile */
	const char *(*getlasterror) (void);
};

/*
 * Available WAL methods:
 *	- WalDirectoryMethod - write WAL to regular files in a standard pg_wal
 *	- WalTarMethod       - write WAL to a tarfile corresponding to pg_wal
 *						   (only implements the methods required for pg_basebackup,
 *						   not all those required for pg_receivewal)
 */
WalWriteMethod *CreateWalDirectoryMethod(const char *basedir,
										 int compression, bool sync);
WalWriteMethod *CreateWalTarMethod(const char *tarbase, int compression, bool sync);

/* Cleanup routines for previously-created methods */
void		FreeWalDirectoryMethod(void);
void		FreeWalTarMethod(void);
