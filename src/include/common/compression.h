/*-------------------------------------------------------------------------
 *
 * compression.h
 *
 * Shared definitions for compression methods and specifications.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/common/compression.h
 *-------------------------------------------------------------------------
 */

#ifndef PG_COMPRESSION_H
#define PG_COMPRESSION_H

typedef enum pg_compress_algorithm
{
	PG_COMPRESSION_NONE,
	PG_COMPRESSION_GZIP,
	PG_COMPRESSION_LZ4,
	PG_COMPRESSION_ZSTD
} pg_compress_algorithm;

#define PG_COMPRESSION_OPTION_WORKERS		(1 << 0)

typedef struct pg_compress_specification
{
	pg_compress_algorithm algorithm;
	unsigned	options;		/* OR of PG_COMPRESSION_OPTION constants */
	int			level;
	int			workers;
	char	   *parse_error;	/* NULL if parsing was OK, else message */
} pg_compress_specification;

extern bool parse_compress_algorithm(char *name, pg_compress_algorithm *algorithm);
extern const char *get_compress_algorithm_name(pg_compress_algorithm algorithm);

extern void parse_compress_specification(pg_compress_algorithm algorithm,
										 char *specification,
										 pg_compress_specification *result);

extern char *validate_compress_specification(pg_compress_specification *);

#endif
