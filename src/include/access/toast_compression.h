/*-------------------------------------------------------------------------
 *
 * toast_compression.h
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021, PostgreSQL Global Development Group
 *
 * src/include/access/toast_compression.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_COMPRESSION_H
#define TOAST_COMPRESSION_H

#include "utils/guc.h"

/* GUCs */
extern char *default_toast_compression;

/* default compression method if not specified. */
#define DEFAULT_TOAST_COMPRESSION	"pglz"

/*
 * Built-in compression method-id.  The toast compression header will store
 * this in the first 2 bits of the raw length.  These built-in compression
 * method-id are directly mapped to the built-in compression methods.
 */
typedef enum ToastCompressionId
{
	TOAST_PGLZ_COMPRESSION_ID = 0,
	TOAST_LZ4_COMPRESSION_ID = 1,
	TOAST_INVALID_COMPRESSION_ID = 2
} ToastCompressionId;

/*
 * Built-in compression methods.  pg_attribute will store this in the
 * attcompression column.
 */
#define TOAST_PGLZ_COMPRESSION			'p'
#define TOAST_LZ4_COMPRESSION			'l'

#define InvalidCompressionMethod	'\0'
#define CompressionMethodIsValid(cm)  ((bool) ((cm) != InvalidCompressionMethod))

#define NO_LZ4_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("unsupported LZ4 compression method"), \
			 errdetail("This functionality requires the server to be built with lz4 support."), \
			 errhint("You need to rebuild PostgreSQL using --with-lz4.")))

#define IsStorageCompressible(storage) ((storage) != TYPSTORAGE_PLAIN && \
										(storage) != TYPSTORAGE_EXTERNAL)

/*
 * GetCompressionMethodName - Get compression method name
 */
static inline const char *
GetCompressionMethodName(char method)
{
	switch (method)
	{
		case TOAST_PGLZ_COMPRESSION:
			return "pglz";
		case TOAST_LZ4_COMPRESSION:
			return "lz4";
		default:
			elog(ERROR, "invalid compression method %c", method);
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * CompressionNameToMethod - Get compression method from compression name
 *
 * Search in the available built-in methods.  If the compression not found
 * in the built-in methods then return InvalidCompressionMethod.
 */
static inline char
CompressionNameToMethod(char *compression)
{
	if (strcmp(compression, "pglz") == 0)
		return TOAST_PGLZ_COMPRESSION;
	else if (strcmp(compression, "lz4") == 0)
	{
#ifndef USE_LZ4
		NO_LZ4_SUPPORT();
#endif
		return TOAST_LZ4_COMPRESSION;
	}

	return InvalidCompressionMethod;
}

/*
 * GetDefaultToastCompression -- get the default toast compression method
 *
 * This exists to hide the use of the default_toast_compression GUC variable.
 */
static inline char
GetDefaultToastCompression(void)
{
	return CompressionNameToMethod(default_toast_compression);
}

/* pglz compression/decompression routines */
extern struct varlena *pglz_compress_datum(const struct varlena *value);
extern struct varlena *pglz_decompress_datum(const struct varlena *value);
extern struct varlena *pglz_decompress_datum_slice(const struct varlena *value,
												   int32 slicelength);

/* lz4 compression/decompression routines */
extern struct varlena *lz4_compress_datum(const struct varlena *value);
extern struct varlena *lz4_decompress_datum(const struct varlena *value);
extern struct varlena *lz4_decompress_datum_slice(const struct varlena *value,
												  int32 slicelength);
extern ToastCompressionId toast_get_compression_id(struct varlena *attr);
extern bool check_default_toast_compression(char **newval, void **extra,
											GucSource source);

#endif							/* TOAST_COMPRESSION_H */
