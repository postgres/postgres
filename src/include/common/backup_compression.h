/*-------------------------------------------------------------------------
 *
 * backup_compression.h
 *
 * Shared definitions for backup compression methods and specifications.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/common/backup_compression.h
 *-------------------------------------------------------------------------
 */

#ifndef BACKUP_COMPRESSION_H
#define BACKUP_COMPRESSION_H

typedef enum bc_algorithm
{
	BACKUP_COMPRESSION_NONE,
	BACKUP_COMPRESSION_GZIP,
	BACKUP_COMPRESSION_LZ4,
	BACKUP_COMPRESSION_ZSTD
} bc_algorithm;

#define	BACKUP_COMPRESSION_OPTION_LEVEL			(1 << 0)
#define BACKUP_COMPRESSION_OPTION_WORKERS		(1 << 1)

typedef struct bc_specification
{
	bc_algorithm algorithm;
	unsigned	options;		/* OR of BACKUP_COMPRESSION_OPTION constants */
	int			level;
	int			workers;
	char	   *parse_error;	/* NULL if parsing was OK, else message */
} bc_specification;

extern bool parse_bc_algorithm(char *name, bc_algorithm *algorithm);
extern const char *get_bc_algorithm_name(bc_algorithm algorithm);

extern void parse_bc_specification(bc_algorithm algorithm,
								   char *specification,
								   bc_specification *result);

extern char *validate_bc_specification(bc_specification *);

#endif
