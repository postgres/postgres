/*-------------------------------------------------------------------------
 *
 * pseudotypes.c
 *	  Functions for the system pseudo-types.
 *
 * A pseudo-type isn't really a type and never has any operations, but
 * we do need to supply input and output functions to satisfy the links
 * in the pseudo-type's entry in pg_type.  In most cases the functions
 * just throw an error if invoked.  (XXX the error messages here cover
 * the most common case, but might be confusing in some contexts.  Can
 * we do better?)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pseudotypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rangetypes.h"
#include "utils/multirangetypes.h"


/*
 * These macros generate input and output functions for a pseudo-type that
 * will reject all input and output attempts.  (But for some types, only
 * the input function need be dummy.)
 */
#define PSEUDOTYPE_DUMMY_INPUT_FUNC(typname) \
Datum \
typname##_in(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot accept a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable

#define PSEUDOTYPE_DUMMY_IO_FUNCS(typname) \
PSEUDOTYPE_DUMMY_INPUT_FUNC(typname); \
\
Datum \
typname##_out(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot display a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable

/*
 * Likewise for binary send/receive functions.  We don't bother with these
 * at all for many pseudotypes, but some have them.  (By convention, if
 * a type has a send function it should have a receive function, even if
 * that's only dummy.)
 */
#define PSEUDOTYPE_DUMMY_RECEIVE_FUNC(typname) \
Datum \
typname##_recv(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot accept a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable

#define PSEUDOTYPE_DUMMY_BINARY_IO_FUNCS(typname) \
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(typname); \
\
Datum \
typname##_send(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot display a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable


/*
 * cstring
 *
 * cstring is marked as a pseudo-type because we don't want people using it
 * in tables.  But it's really a perfectly functional type, so provide
 * a full set of working I/O functions for it.  Among other things, this
 * allows manual invocation of datatype I/O functions, along the lines of
 * "SELECT foo_in('blah')" or "SELECT foo_out(some-foo-value)".
 */
Datum
cstring_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_CSTRING(pstrdup(str));
}

Datum
cstring_out(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_CSTRING(pstrdup(str));
}

Datum
cstring_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	PG_RETURN_CSTRING(str);
}

Datum
cstring_send(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, str, strlen(str));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * anyarray
 *
 * We need to allow output of anyarray so that, e.g., pg_statistic columns
 * can be printed.  Input has to be disallowed, however.
 *
 * XXX anyarray_recv could actually be made to work, since the incoming
 * array data would contain the element type OID.  It seems unlikely that
 * it'd be sufficiently type-safe, though.
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anyarray);
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(anyarray);

Datum
anyarray_out(PG_FUNCTION_ARGS)
{
	return array_out(fcinfo);
}

Datum
anyarray_send(PG_FUNCTION_ARGS)
{
	return array_send(fcinfo);
}

/*
 * anycompatiblearray
 *
 * We may as well allow output, since we do for anyarray.
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anycompatiblearray);
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(anycompatiblearray);

Datum
anycompatiblearray_out(PG_FUNCTION_ARGS)
{
	return array_out(fcinfo);
}

Datum
anycompatiblearray_send(PG_FUNCTION_ARGS)
{
	return array_send(fcinfo);
}

/*
 * anyenum
 *
 * We may as well allow output, since enum_out will in fact work.
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anyenum);

Datum
anyenum_out(PG_FUNCTION_ARGS)
{
	return enum_out(fcinfo);
}

/*
 * anyrange
 *
 * We may as well allow output, since range_out will in fact work.
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anyrange);

Datum
anyrange_out(PG_FUNCTION_ARGS)
{
	return range_out(fcinfo);
}

/*
 * anycompatiblerange
 *
 * We may as well allow output, since range_out will in fact work.
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anycompatiblerange);

Datum
anycompatiblerange_out(PG_FUNCTION_ARGS)
{
	return range_out(fcinfo);
}

/*
 * anycompatiblemultirange
 *
 * We may as well allow output, since multirange_out will in fact work.
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anycompatiblemultirange);

Datum
anycompatiblemultirange_out(PG_FUNCTION_ARGS)
{
	return multirange_out(fcinfo);
}

/*
 * anymultirange_in		- input routine for pseudo-type ANYMULTIRANGE.
 */
Datum
anymultirange_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "anymultirange")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * anymultirange_out		- output routine for pseudo-type ANYMULTIRANGE.
 *
 * We may as well allow this, since multirange_out will in fact work.
 */
Datum
anymultirange_out(PG_FUNCTION_ARGS)
{
	return multirange_out(fcinfo);
}

/*
 * void
 *
 * We support void_in so that PL functions can return VOID without any
 * special hack in the PL handler.  Whatever value the PL thinks it's
 * returning will just be ignored.  Conversely, void_out and void_send
 * are needed so that "SELECT function_returning_void(...)" works.
 */
Datum
void_in(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();			/* you were expecting something different? */
}

Datum
void_out(PG_FUNCTION_ARGS)
{
	PG_RETURN_CSTRING(pstrdup(""));
}

Datum
void_recv(PG_FUNCTION_ARGS)
{
	/*
	 * Note that since we consume no bytes, an attempt to send anything but an
	 * empty string will result in an "invalid message format" error.
	 */
	PG_RETURN_VOID();
}

Datum
void_send(PG_FUNCTION_ARGS)
{
	StringInfoData buf;

	/* send an empty string */
	pq_begintypsend(&buf);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * shell
 *
 * shell_in and shell_out are entered in pg_type for "shell" types
 * (those not yet filled in).  They should be unreachable, but we
 * set them up just in case some code path tries to do I/O without
 * having checked pg_type.typisdefined anywhere along the way.
 */
Datum
shell_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of a shell type")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

Datum
shell_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot display a value of a shell type")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}


/*
 * pg_node_tree
 *
 * pg_node_tree isn't really a pseudotype --- it's real enough to be a table
 * column --- but it presently has no operations of its own, and disallows
 * input too, so its I/O functions seem to fit here as much as anywhere.
 *
 * We must disallow input of pg_node_tree values because the SQL functions
 * that operate on the type are not secure against malformed input.
 * We do want to allow output, though.
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(pg_node_tree);
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(pg_node_tree);

Datum
pg_node_tree_out(PG_FUNCTION_ARGS)
{
	return textout(fcinfo);
}

Datum
pg_node_tree_send(PG_FUNCTION_ARGS)
{
	return textsend(fcinfo);
}

/*
 * pg_ddl_command
 *
 * Like pg_node_tree, pg_ddl_command isn't really a pseudotype; it's here
 * for the same reasons as that one.
 *
 * We don't have any good way to output this type directly, so punt
 * for output as well as input.
 */
PSEUDOTYPE_DUMMY_IO_FUNCS(pg_ddl_command);
PSEUDOTYPE_DUMMY_BINARY_IO_FUNCS(pg_ddl_command);


/*
 * Dummy I/O functions for various other pseudotypes.
 */
PSEUDOTYPE_DUMMY_IO_FUNCS(any);
PSEUDOTYPE_DUMMY_IO_FUNCS(trigger);
PSEUDOTYPE_DUMMY_IO_FUNCS(event_trigger);
PSEUDOTYPE_DUMMY_IO_FUNCS(language_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(fdw_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(table_am_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(index_am_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(tsm_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(internal);
PSEUDOTYPE_DUMMY_IO_FUNCS(anyelement);
PSEUDOTYPE_DUMMY_IO_FUNCS(anynonarray);
PSEUDOTYPE_DUMMY_IO_FUNCS(anycompatible);
PSEUDOTYPE_DUMMY_IO_FUNCS(anycompatiblenonarray);
