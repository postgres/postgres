/*-------------------------------------------------------------------------
 *
 * walmethods.h
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
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
}	WalCloseMethod;

typedef struct WalWriteMethod WalWriteMethod;
struct WalWriteMethod
{
	Walfile(*open_for_write) (const char *pathname, const char *temp_suffix, size_t pad_to_size);
	int			(*close) (Walfile f, WalCloseMethod method);
	bool		(*existsfile) (const char *pathname);
	ssize_t		(*get_file_size) (const char *pathname);

	ssize_t		(*write) (Walfile f, const void *buf, size_t count);
	off_t		(*get_current_pos) (Walfile f);
	int			(*sync) (Walfile f);
	bool		(*finish) (void);
	char	   *(*getlasterror) (void);
};

/*
 * Available WAL methods:
 *	- WalDirectoryMethod - write WAL to regular files in a standard pg_xlog
 *	- TarDirectoryMethod - write WAL to a tarfile corresponding to pg_xlog
 *						   (only implements the methods required for pg_basebackup,
 *						   not all those required for pg_receivexlog)
 */
WalWriteMethod *CreateWalDirectoryMethod(const char *basedir,
										 int compression, bool sync);
WalWriteMethod *CreateWalTarMethod(const char *tarbase, int compression, bool sync);

/* Cleanup routines for previously-created methods */
void FreeWalDirectoryMethod(void);
void FreeWalTarMethod(void);
