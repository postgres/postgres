/*-------------------------------------------------------------------------
 *
 * toast_compression.c
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_compression.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "common/pg_lzcompress.h"
#include "utils/memutils.h"
#include "varatt.h"

/* GUC */
int			default_toast_compression = TOAST_PGLZ_COMPRESSION;
bool		use_extended_toast_header = true;	/* default: use new 20-byte format */

#define NO_COMPRESSION_SUPPORT(method) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method %s not supported", method), \
			 errdetail("This functionality requires the server to be built with %s support.", method)))

/*
 * Compress a varlena using PGLZ.
 *
 * Returns the compressed varlena, or NULL if compression fails.
 */
struct varlena *
pglz_compress_datum(const struct varlena *value)
{
	int32		valsize,
				len;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * No point in wasting a palloc cycle if value size is outside the allowed
	 * range for compression.
	 */
	if (valsize < PGLZ_strategy_default->min_input_size ||
		valsize > PGLZ_strategy_default->max_input_size)
		return NULL;

	/*
	 * Figure out the maximum possible size of the pglz output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 */
	tmp = (struct varlena *) palloc(PGLZ_MAX_OUTPUT(valsize) +
									VARHDRSZ_COMPRESSED);

	len = pglz_compress(VARDATA_ANY(value),
						valsize,
						(char *) tmp + VARHDRSZ_COMPRESSED,
						NULL);
	if (len < 0)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
}

/*
 * Decompress a varlena that was compressed using PGLZ.
 */
struct varlena *
pglz_decompress_datum(const struct varlena *value)
{
	struct varlena *result;
	int32		rawsize;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  VARDATA_COMPRESSED_GET_EXTSIZE(value), true);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * Decompress part of a varlena that was compressed using PGLZ.
 */
struct varlena *
pglz_decompress_datum_slice(const struct varlena *value,
							int32 slicelength)
{
	struct varlena *result;
	int32		rawsize;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  slicelength, false);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * Compress a varlena using LZ4.
 *
 * Returns the compressed varlena, or NULL if compression fails.
 */
struct varlena *
lz4_compress_datum(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_COMPRESSION_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		valsize;
	int32		len;
	int32		max_size;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * Figure out the maximum possible size of the LZ4 output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 */
	max_size = LZ4_compressBound(valsize);
	tmp = (struct varlena *) palloc(max_size + VARHDRSZ_COMPRESSED);

	len = LZ4_compress_default(VARDATA_ANY(value),
							   (char *) tmp + VARHDRSZ_COMPRESSED,
							   valsize, max_size);
	if (len <= 0)
		elog(ERROR, "lz4 compression failed");

	/* data is incompressible so just free the memory and return NULL */
	if (len > valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
#endif
}

/*
 * Decompress a varlena that was compressed using LZ4.
 */
struct varlena *
lz4_decompress_datum(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_COMPRESSION_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		rawsize;
	struct varlena *result;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe((char *) value + VARHDRSZ_COMPRESSED,
								  VARDATA(result),
								  VARSIZE(value) - VARHDRSZ_COMPRESSED,
								  VARDATA_COMPRESSED_GET_EXTSIZE(value));
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));


	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Decompress part of a varlena that was compressed using LZ4.
 */
struct varlena *
lz4_decompress_datum_slice(const struct varlena *value, int32 slicelength)
{
#ifndef USE_LZ4
	NO_COMPRESSION_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		rawsize;
	struct varlena *result;

	/* slice decompression not supported prior to 1.8.3 */
	if (LZ4_versionNumber() < 10803)
		return lz4_decompress_datum(value);

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe_partial((char *) value + VARHDRSZ_COMPRESSED,
										  VARDATA(result),
										  VARSIZE(value) - VARHDRSZ_COMPRESSED,
										  slicelength,
										  slicelength);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Extract compression ID from a varlena.
 *
 * Returns TOAST_INVALID_COMPRESSION_ID if the varlena is not compressed.
 *
 * For external data stored in extended format (VARTAG_ONDISK_EXTENDED),
 * the actual compression method is stored in va_data[0].  We map that
 * back to the appropriate ToastCompressionId for legacy compatibility.
 */
ToastCompressionId
toast_get_compression_id(struct varlena *attr)
{
	ToastCompressionId cmid = TOAST_INVALID_COMPRESSION_ID;
	vartag_external tag;

	/*
	 * If it is stored externally then fetch the compression method id from
	 * the external toast pointer.  If compressed inline, fetch it from the
	 * toast compression header.
	 */
	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		tag = VARTAG_EXTERNAL(attr);
		if (tag == VARTAG_ONDISK)
		{
			struct varatt_external toast_pointer;

			VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

			if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
				cmid = VARATT_EXTERNAL_GET_COMPRESS_METHOD(toast_pointer);
		}
		else
		{
			struct varatt_external_extended toast_pointer_ext;
			uint8		ext_method;

			Assert(tag == VARTAG_ONDISK_EXTENDED);
			VARATT_EXTERNAL_GET_POINTER_EXTENDED(toast_pointer_ext, attr);

			if (VARATT_EXTERNAL_IS_COMPRESSED_EXTENDED(toast_pointer_ext))
			{
				/*
				 * Extended format stores the actual method in va_data[0].
				 * Map it back to ToastCompressionId for reporting purposes.
				 */
				ext_method = VARATT_EXTERNAL_GET_EXT_COMPRESSION_METHOD(toast_pointer_ext);
				switch (ext_method)
				{
					case TOAST_PGLZ_EXT_METHOD:
						cmid = TOAST_PGLZ_COMPRESSION_ID;
						break;
					case TOAST_LZ4_EXT_METHOD:
						cmid = TOAST_LZ4_COMPRESSION_ID;
						break;
					case TOAST_ZSTD_EXT_METHOD:
						cmid = TOAST_EXTENDED_COMPRESSION_ID;
						break;
					case TOAST_UNCOMPRESSED_EXT_METHOD:
						/* Uncompressed data in extended format */
						cmid = TOAST_INVALID_COMPRESSION_ID;
						break;
					default:
						elog(ERROR, "invalid extended compression method %d",
							 ext_method);
				}
			}
		}
	}
	else if (VARATT_IS_COMPRESSED(attr))
		cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(attr);

	return cmid;
}

/*
 * Zstandard (zstd) compression/decompression for TOAST (extended methods).
 *
 * These routines use the same basic shape as the pglz and LZ4 helpers,
 * but are only available when PostgreSQL is built with USE_ZSTD.
 */

/*
 * Compress a varlena using ZSTD.
 *
 * Returns the compressed varlena, or NULL if compression fails or does
 * not save space.
 */
static struct varlena *
zstd_compress_datum_internal(const struct varlena *value, int level)
{
#ifndef USE_ZSTD
	NO_COMPRESSION_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	Size		valsize;
	Size		max_size;
	Size		out_size;
	struct varlena *tmp;
	size_t		rc;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * Compute an upper bound for the compressed size and allocate enough
	 * space for the compressed payload plus the varlena header.
	 */
	max_size = ZSTD_compressBound(valsize);
	if (max_size > (Size) (MaxAllocSize - VARHDRSZ_COMPRESSED))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("compressed data would exceed maximum allocation size")));

	tmp = (struct varlena *) palloc(max_size + VARHDRSZ_COMPRESSED);

	rc = ZSTD_compress((char *) tmp + VARHDRSZ_COMPRESSED, max_size,
					   VARDATA_ANY(value), valsize, level);
	if (ZSTD_isError(rc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("zstd compression failed: %s",
								 ZSTD_getErrorName(rc))));

	out_size = (Size) rc;

	/*
	 * If the compressed representation is not smaller than the original
	 * payload, give up and return NULL so that callers can fall back to
	 * storing the datum uncompressed or with a different method.
	 */
	if (out_size >= valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, out_size + VARHDRSZ_COMPRESSED);

	return tmp;
#endif							/* USE_ZSTD */
}

struct varlena *
zstd_compress_datum(const struct varlena *value)
{
#ifndef USE_ZSTD
	NO_COMPRESSION_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	return zstd_compress_datum_internal(value, ZSTD_CLEVEL_DEFAULT);
#endif
}

/*
 * Decompress a varlena that was compressed using ZSTD.
 */
struct varlena *
zstd_decompress_datum(const struct varlena *value)
{
#ifndef USE_ZSTD
	NO_COMPRESSION_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	struct varlena *result;
	Size		rawsize;
	size_t		rc;

	/* allocate memory for the uncompressed data */
	rawsize = VARDATA_COMPRESSED_GET_EXTSIZE(value);
	result = (struct varlena *) palloc(rawsize + VARHDRSZ);

	rc = ZSTD_decompress(VARDATA(result), rawsize,
						 (char *) value + VARHDRSZ_COMPRESSED,
						 VARSIZE(value) - VARHDRSZ_COMPRESSED);
	if (ZSTD_isError(rc) || rc != rawsize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt or truncated")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif							/* USE_ZSTD */
}

/*
 * Decompress part of a varlena that was compressed using ZSTD.
 *
 * At least initially we don't try to be clever with streaming slice
 * decompression here; instead we just decompress the full datum and
 * let higher layers perform the slicing.  Callers should prefer the
 * regular zstd_decompress_datum() when they know they need the whole
 * value anyway.
 */
struct varlena *
zstd_decompress_datum_slice(const struct varlena *value, int32 slicelength)
{
	/* For now, just fall back to full decompression. */
	(void) slicelength;
	return zstd_decompress_datum(value);
}

/*
 * CompressionNameToMethod - Get compression method from compression name
 *
 * Search in the available built-in methods.  If the compression not found
 * in the built-in methods then return InvalidCompressionMethod.
 */
char
CompressionNameToMethod(const char *compression)
{
	if (strcmp(compression, "pglz") == 0)
		return TOAST_PGLZ_COMPRESSION;
	else if (strcmp(compression, "lz4") == 0)
	{
#ifndef USE_LZ4
		NO_COMPRESSION_SUPPORT("lz4");
#endif
		return TOAST_LZ4_COMPRESSION;
	}
	else if (strcmp(compression, "zstd") == 0)
	{
#ifndef USE_ZSTD
		NO_COMPRESSION_SUPPORT("zstd");
#endif
		return TOAST_ZSTD_COMPRESSION;
	}

	return InvalidCompressionMethod;
}

/*
 * GetCompressionMethodName - Get compression method name
 */
const char *
GetCompressionMethodName(char method)
{
	switch (method)
	{
		case TOAST_PGLZ_COMPRESSION:
			return "pglz";
		case TOAST_LZ4_COMPRESSION:
			return "lz4";
		case TOAST_ZSTD_COMPRESSION:
			return "zstd";
		default:
			elog(ERROR, "invalid compression method %c", method);
			return NULL;		/* keep compiler quiet */
	}
}
