/*-------------------------------------------------------------------------
 *
 * compression.c
 *
 * Shared code for compression methods and specifications.
 *
 * A compression specification specifies the parameters that should be used
 * when performing compression with a specific algorithm. The simplest
 * possible compression specification is an integer, which sets the
 * compression level.
 *
 * Otherwise, a compression specification is a comma-separated list of items,
 * each having the form keyword or keyword=value.
 *
 * Currently, the supported keywords are "level", "long", and "workers".
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/common/compression.c
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef USE_ZSTD
#include <zstd.h>
#endif
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "common/compression.h"

static int	expect_integer_value(char *keyword, char *value,
								 pg_compress_specification *result);
static bool expect_boolean_value(char *keyword, char *value,
								 pg_compress_specification *result);

/*
 * Look up a compression algorithm by name. Returns true and sets *algorithm
 * if the name is recognized. Otherwise returns false.
 */
bool
parse_compress_algorithm(char *name, pg_compress_algorithm *algorithm)
{
	if (strcmp(name, "none") == 0)
		*algorithm = PG_COMPRESSION_NONE;
	else if (strcmp(name, "gzip") == 0)
		*algorithm = PG_COMPRESSION_GZIP;
	else if (strcmp(name, "lz4") == 0)
		*algorithm = PG_COMPRESSION_LZ4;
	else if (strcmp(name, "zstd") == 0)
		*algorithm = PG_COMPRESSION_ZSTD;
	else
		return false;
	return true;
}

/*
 * Get the human-readable name corresponding to a particular compression
 * algorithm.
 */
const char *
get_compress_algorithm_name(pg_compress_algorithm algorithm)
{
	switch (algorithm)
	{
		case PG_COMPRESSION_NONE:
			return "none";
		case PG_COMPRESSION_GZIP:
			return "gzip";
		case PG_COMPRESSION_LZ4:
			return "lz4";
		case PG_COMPRESSION_ZSTD:
			return "zstd";
			/* no default, to provoke compiler warnings if values are added */
	}
	Assert(false);
	return "???";				/* placate compiler */
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
 * The compression level is assigned by default if not directly specified
 * by the specification.
 *
 * Use validate_compress_specification() to find out whether a compression
 * specification is semantically sensible.
 */
void
parse_compress_specification(pg_compress_algorithm algorithm, char *specification,
							 pg_compress_specification *result)
{
	int			bare_level;
	char	   *bare_level_endp;

	/* Initial setup of result object. */
	result->algorithm = algorithm;
	result->options = 0;
	result->parse_error = NULL;

	/*
	 * Assign a default level depending on the compression method.  This may
	 * be enforced later.
	 */
	switch (result->algorithm)
	{
		case PG_COMPRESSION_NONE:
			result->level = 0;
			break;
		case PG_COMPRESSION_LZ4:
#ifdef USE_LZ4
			result->level = 0;	/* fast compression mode */
#else
			result->parse_error =
				psprintf(_("this build does not support compression with %s"),
						 "LZ4");
#endif
			break;
		case PG_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
			result->level = ZSTD_CLEVEL_DEFAULT;
#else
			result->parse_error =
				psprintf(_("this build does not support compression with %s"),
						 "ZSTD");
#endif
			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			result->level = Z_DEFAULT_COMPRESSION;
#else
			result->parse_error =
				psprintf(_("this build does not support compression with %s"),
						 "gzip");
#endif
			break;
	}

	/* If there is no specification, we're done already. */
	if (specification == NULL)
		return;

	/* As a special case, the specification can be a bare integer. */
	bare_level = strtol(specification, &bare_level_endp, 10);
	if (specification != bare_level_endp && *bare_level_endp == '\0')
	{
		result->level = bare_level;
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

			/*
			 * No need to set a flag in "options", there is a default level
			 * set at least thanks to the logic above.
			 */
		}
		else if (strcmp(keyword, "workers") == 0)
		{
			result->workers = expect_integer_value(keyword, value, result);
			result->options |= PG_COMPRESSION_OPTION_WORKERS;
		}
		else if (strcmp(keyword, "long") == 0)
		{
			result->long_distance = expect_boolean_value(keyword, value, result);
			result->options |= PG_COMPRESSION_OPTION_LONG_DISTANCE;
		}
		else
			result->parse_error =
				psprintf(_("unrecognized compression option: \"%s\""), keyword);

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
expect_integer_value(char *keyword, char *value, pg_compress_specification *result)
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
 * Parse 'value' as a boolean and return the result.
 *
 * If parsing fails, set result->parse_error to an appropriate message
 * and return -1.  The caller must check result->parse_error to determine if
 * the call was successful.
 *
 * Valid values are: yes, no, on, off, 1, 0.
 *
 * Inspired by ParseVariableBool().
 */
static bool
expect_boolean_value(char *keyword, char *value, pg_compress_specification *result)
{
	if (value == NULL)
		return true;

	if (pg_strcasecmp(value, "yes") == 0)
		return true;
	if (pg_strcasecmp(value, "on") == 0)
		return true;
	if (pg_strcasecmp(value, "1") == 0)
		return true;

	if (pg_strcasecmp(value, "no") == 0)
		return false;
	if (pg_strcasecmp(value, "off") == 0)
		return false;
	if (pg_strcasecmp(value, "0") == 0)
		return false;

	result->parse_error =
		psprintf(_("value for compression option \"%s\" must be a Boolean value"),
				 keyword);
	return false;
}

/*
 * Returns NULL if the compression specification string was syntactically
 * valid and semantically sensible.  Otherwise, returns an error message.
 *
 * Does not test whether this build of PostgreSQL supports the requested
 * compression method.
 */
char *
validate_compress_specification(pg_compress_specification *spec)
{
	int			min_level = 1;
	int			max_level = 1;
	int			default_level = 0;

	/* If it didn't even parse OK, it's definitely no good. */
	if (spec->parse_error != NULL)
		return spec->parse_error;

	/*
	 * Check that the algorithm expects a compression level and it is within
	 * the legal range for the algorithm.
	 */
	switch (spec->algorithm)
	{
		case PG_COMPRESSION_GZIP:
			max_level = 9;
#ifdef HAVE_LIBZ
			default_level = Z_DEFAULT_COMPRESSION;
#endif
			break;
		case PG_COMPRESSION_LZ4:
			max_level = 12;
			default_level = 0;	/* fast mode */
			break;
		case PG_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
			max_level = ZSTD_maxCLevel();
			min_level = ZSTD_minCLevel();
			default_level = ZSTD_CLEVEL_DEFAULT;
#endif
			break;
		case PG_COMPRESSION_NONE:
			if (spec->level != 0)
				return psprintf(_("compression algorithm \"%s\" does not accept a compression level"),
								get_compress_algorithm_name(spec->algorithm));
			break;
	}

	if ((spec->level < min_level || spec->level > max_level) &&
		spec->level != default_level)
		return psprintf(_("compression algorithm \"%s\" expects a compression level between %d and %d (default at %d)"),
						get_compress_algorithm_name(spec->algorithm),
						min_level, max_level, default_level);

	/*
	 * Of the compression algorithms that we currently support, only zstd
	 * allows parallel workers.
	 */
	if ((spec->options & PG_COMPRESSION_OPTION_WORKERS) != 0 &&
		(spec->algorithm != PG_COMPRESSION_ZSTD))
	{
		return psprintf(_("compression algorithm \"%s\" does not accept a worker count"),
						get_compress_algorithm_name(spec->algorithm));
	}

	/*
	 * Of the compression algorithms that we currently support, only zstd
	 * supports long-distance mode.
	 */
	if ((spec->options & PG_COMPRESSION_OPTION_LONG_DISTANCE) != 0 &&
		(spec->algorithm != PG_COMPRESSION_ZSTD))
	{
		return psprintf(_("compression algorithm \"%s\" does not support long-distance mode"),
						get_compress_algorithm_name(spec->algorithm));
	}

	return NULL;
}

#ifdef FRONTEND

/*
 * Basic parsing of a value specified through a command-line option, commonly
 * -Z/--compress.
 *
 * The parsing consists of a METHOD:DETAIL string fed later to
 * parse_compress_specification().  This only extracts METHOD and DETAIL.
 * If only an integer is found, the method is implied by the value specified.
 */
void
parse_compress_options(const char *option, char **algorithm, char **detail)
{
	char	   *sep;
	char	   *endp;
	long		result;

	/*
	 * Check whether the compression specification consists of a bare integer.
	 *
	 * For backward-compatibility, assume "none" if the integer found is zero
	 * and "gzip" otherwise.
	 */
	result = strtol(option, &endp, 10);
	if (*endp == '\0')
	{
		if (result == 0)
		{
			*algorithm = pstrdup("none");
			*detail = NULL;
		}
		else
		{
			*algorithm = pstrdup("gzip");
			*detail = pstrdup(option);
		}
		return;
	}

	/*
	 * Check whether there is a compression detail following the algorithm
	 * name.
	 */
	sep = strchr(option, ':');
	if (sep == NULL)
	{
		*algorithm = pstrdup(option);
		*detail = NULL;
	}
	else
	{
		char	   *alg;

		alg = palloc((sep - option) + 1);
		memcpy(alg, option, sep - option);
		alg[sep - option] = '\0';

		*algorithm = alg;
		*detail = pstrdup(sep + 1);
	}
}
#endif							/* FRONTEND */
