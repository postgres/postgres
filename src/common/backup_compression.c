/*-------------------------------------------------------------------------
 *
 * backup_compression.c
 *
 * Shared code for backup compression methods and specifications.
 *
 * A compression specification specifies the parameters that should be used
 * when performing compression with a specific algorithm. The simplest
 * possible compression specification is an integer, which sets the
 * compression level.
 *
 * Otherwise, a compression specification is a comma-separated list of items,
 * each having the form keyword or keyword=value.
 *
 * Currently, the only supported keyword is "level".
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/common/backup_compression.c
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/backup_compression.h"

static int	expect_integer_value(char *keyword, char *value,
								 bc_specification *result);

/*
 * Look up a compression algorithm by name. Returns true and sets *algorithm
 * if the name is recognized. Otherwise returns false.
 */
bool
parse_bc_algorithm(char *name, bc_algorithm *algorithm)
{
	if (strcmp(name, "none") == 0)
		*algorithm = BACKUP_COMPRESSION_NONE;
	else if (strcmp(name, "gzip") == 0)
		*algorithm = BACKUP_COMPRESSION_GZIP;
	else if (strcmp(name, "lz4") == 0)
		*algorithm = BACKUP_COMPRESSION_LZ4;
	else if (strcmp(name, "zstd") == 0)
		*algorithm = BACKUP_COMPRESSION_ZSTD;
	else
		return false;
	return true;
}

/*
 * Get the human-readable name corresponding to a particular compression
 * algorithm.
 */
const char *
get_bc_algorithm_name(bc_algorithm algorithm)
{
	switch (algorithm)
	{
		case BACKUP_COMPRESSION_NONE:
			return "none";
		case BACKUP_COMPRESSION_GZIP:
			return "gzip";
		case BACKUP_COMPRESSION_LZ4:
			return "lz4";
		case BACKUP_COMPRESSION_ZSTD:
			return "zstd";
			/* no default, to provoke compiler warnings if values are added */
	}
	Assert(false);
	return "???";	/* placate compiler */
}

/*
 * Parse a compression specification for a specified algorithm.
 *
 * See the file header comments for a brief description of what a compression
 * specification is expected to look like.
 *
 * On return, all fields of the result object will be initialized.
 * In particular, result->parse_error will be NULL if no errors occurred
 * during parsing, and will otherwise contain an appropriate error message.
 * The caller may free this error message string using pfree, if desired.
 * Note, however, even if there's no parse error, the string might not make
 * sense: e.g. for gzip, level=12 is not sensible, but it does parse OK.
 *
 * Use validate_bc_specification() to find out whether a compression
 * specification is semantically sensible.
 */
void
parse_bc_specification(bc_algorithm algorithm, char *specification,
					   bc_specification *result)
{
	int			bare_level;
	char	   *bare_level_endp;

	/* Initial setup of result object. */
	result->algorithm = algorithm;
	result->options = 0;
	result->level = -1;
	result->parse_error = NULL;

	/* If there is no specification, we're done already. */
	if (specification == NULL)
		return;

	/* As a special case, the specification can be a bare integer. */
	bare_level = strtol(specification, &bare_level_endp, 10);
	if (specification != bare_level_endp && *bare_level_endp == '\0')
	{
		result->level = bare_level;
		result->options |= BACKUP_COMPRESSION_OPTION_LEVEL;
		return;
	}

	/* Look for comma-separated keyword or keyword=value entries. */
	while (1)
	{
		char	   *kwstart;
		char	   *kwend;
		char	   *vstart;
		char	   *vend;
		int			kwlen;
		int			vlen;
		bool		has_value;
		char	   *keyword;
		char	   *value;

		/* Figure start, end, and length of next keyword and any value. */
		kwstart = kwend = specification;
		while (*kwend != '\0' && *kwend != ',' && *kwend != '=')
			++kwend;
		kwlen = kwend - kwstart;
		if (*kwend != '=')
		{
			vstart = vend = NULL;
			vlen = 0;
			has_value = false;
		}
		else
		{
			vstart = vend = kwend + 1;
			while (*vend != '\0' && *vend != ',')
				++vend;
			vlen = vend - vstart;
			has_value = true;
		}

		/* Reject empty keyword. */
		if (kwlen == 0)
		{
			result->parse_error =
				pstrdup(_("found empty string where a compression option was expected"));
			break;
		}

		/* Extract keyword and value as separate C strings. */
		keyword = palloc(kwlen + 1);
		memcpy(keyword, kwstart, kwlen);
		keyword[kwlen] = '\0';
		if (!has_value)
			value = NULL;
		else
		{
			value = palloc(vlen + 1);
			memcpy(value, vstart, vlen);
			value[vlen] = '\0';
		}

		/* Handle whatever keyword we found. */
		if (strcmp(keyword, "level") == 0)
		{
			result->level = expect_integer_value(keyword, value, result);
			result->options |= BACKUP_COMPRESSION_OPTION_LEVEL;
		}
		else if (strcmp(keyword, "workers") == 0)
		{
			result->workers = expect_integer_value(keyword, value, result);
			result->options |= BACKUP_COMPRESSION_OPTION_WORKERS;
		}
		else
			result->parse_error =
				psprintf(_("unknown compression option \"%s\""), keyword);

		/* Release memory, just to be tidy. */
		pfree(keyword);
		if (value != NULL)
			pfree(value);

		/*
		 * If we got an error or have reached the end of the string, stop.
		 *
		 * If there is no value, then the end of the keyword might have been
		 * the end of the string. If there is a value, then the end of the
		 * keyword cannot have been the end of the string, but the end of the
		 * value might have been.
		 */
		if (result->parse_error != NULL ||
			(vend == NULL ? *kwend == '\0' : *vend == '\0'))
			break;

		/* Advance to next entry and loop around. */
		specification = vend == NULL ? kwend + 1 : vend + 1;
	}
}

/*
 * Parse 'value' as an integer and return the result.
 *
 * If parsing fails, set result->parse_error to an appropriate message
 * and return -1.
 */
static int
expect_integer_value(char *keyword, char *value, bc_specification *result)
{
	int			ivalue;
	char	   *ivalue_endp;

	if (value == NULL)
	{
		result->parse_error =
			psprintf(_("compression option \"%s\" requires a value"),
					 keyword);
		return -1;
	}

	ivalue = strtol(value, &ivalue_endp, 10);
	if (ivalue_endp == value || *ivalue_endp != '\0')
	{
		result->parse_error =
			psprintf(_("value for compression option \"%s\" must be an integer"),
					 keyword);
		return -1;
	}
	return ivalue;
}

/*
 * Returns NULL if the compression specification string was syntactically
 * valid and semantically sensible.  Otherwise, returns an error message.
 *
 * Does not test whether this build of PostgreSQL supports the requested
 * compression method.
 */
char *
validate_bc_specification(bc_specification *spec)
{
	/* If it didn't even parse OK, it's definitely no good. */
	if (spec->parse_error != NULL)
		return spec->parse_error;

	/*
	 * If a compression level was specified, check that the algorithm expects
	 * a compression level and that the level is within the legal range for
	 * the algorithm.
	 */
	if ((spec->options & BACKUP_COMPRESSION_OPTION_LEVEL) != 0)
	{
		int			min_level = 1;
		int			max_level;

		if (spec->algorithm == BACKUP_COMPRESSION_GZIP)
			max_level = 9;
		else if (spec->algorithm == BACKUP_COMPRESSION_LZ4)
			max_level = 12;
		else if (spec->algorithm == BACKUP_COMPRESSION_ZSTD)
			max_level = 22;
		else
			return psprintf(_("compression algorithm \"%s\" does not accept a compression level"),
							get_bc_algorithm_name(spec->algorithm));

		if (spec->level < min_level || spec->level > max_level)
			return psprintf(_("compression algorithm \"%s\" expects a compression level between %d and %d"),
							get_bc_algorithm_name(spec->algorithm),
							min_level, max_level);
	}

	/*
	 * Of the compression algorithms that we currently support, only zstd
	 * allows parallel workers.
	 */
	if ((spec->options & BACKUP_COMPRESSION_OPTION_WORKERS) != 0 &&
		(spec->algorithm != BACKUP_COMPRESSION_ZSTD))
	{
		return psprintf(_("compression algorithm \"%s\" does not accept a worker count"),
						get_bc_algorithm_name(spec->algorithm));
	}

	return NULL;
}
