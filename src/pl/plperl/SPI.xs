/**********************************************************************
 * PostgreSQL::InServer::SPI
 *
 * SPI interface for plperl.
 *
 *    src/pl/plperl/SPI.xs
 *
 **********************************************************************/

/* this must be first: */
#include "postgres.h"
#include "mb/pg_wchar.h"       /* for GetDatabaseEncoding */

/* Defined by Perl */
#undef _

/* perl stuff */
#include "plperl.h"
#include "plperl_helpers.h"


/*
 * Interface routine to catch ereports and punt them to Perl
 */
static void
do_plperl_return_next(SV *sv)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		plperl_return_next(sv);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Must reset elog.c's state */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Punt the error to Perl */
		croak("%s", edata->message);
	}
	PG_END_TRY();
}


MODULE = PostgreSQL::InServer::SPI PREFIX = spi_

PROTOTYPES: ENABLE
VERSIONCHECK: DISABLE

SV*
spi_spi_exec_query(sv, ...)
	SV* sv;
	PREINIT:
		HV *ret_hash;
		int limit = 0;
		char *query;
	CODE:
		if (items > 2)
			croak("Usage: spi_exec_query(query, limit) "
				  "or spi_exec_query(query)");
		if (items == 2)
			limit = SvIV(ST(1));
		query = sv2cstr(sv);
		ret_hash = plperl_spi_exec(query, limit);
		pfree(query);
		RETVAL = newRV_noinc((SV*) ret_hash);
	OUTPUT:
		RETVAL

void
spi_return_next(rv)
	SV *rv;
	CODE:
		do_plperl_return_next(rv);

SV *
spi_spi_query(sv)
	SV *sv;
	CODE:
		char* query = sv2cstr(sv);
		RETVAL = plperl_spi_query(query);
		pfree(query);
	OUTPUT:
		RETVAL

SV *
spi_spi_fetchrow(sv)
	SV* sv;
	CODE:
		char* cursor = sv2cstr(sv);
		RETVAL = plperl_spi_fetchrow(cursor);
		pfree(cursor);
	OUTPUT:
		RETVAL

SV*
spi_spi_prepare(sv, ...)
	SV* sv;
	CODE:
		int i;
		SV** argv;
		char* query = sv2cstr(sv);
		if (items < 1)
			Perl_croak(aTHX_ "Usage: spi_prepare(query, ...)");
		argv = ( SV**) palloc(( items - 1) * sizeof(SV*));
		for ( i = 1; i < items; i++)
			argv[i - 1] = ST(i);
		RETVAL = plperl_spi_prepare(query, items - 1, argv);
		pfree( argv);
		pfree(query);
	OUTPUT:
		RETVAL

SV*
spi_spi_exec_prepared(sv, ...)
	SV* sv;
	PREINIT:
		HV *ret_hash;
	CODE:
		HV *attr = NULL;
		int i, offset = 1, argc;
		SV ** argv;
		char *query = sv2cstr(sv);
		if ( items < 1)
			Perl_croak(aTHX_ "Usage: spi_exec_prepared(query, [\\%%attr,] "
					   "[\\@bind_values])");
		if ( items > 1 && SvROK( ST( 1)) && SvTYPE( SvRV( ST( 1))) == SVt_PVHV)
		{
			attr = ( HV*) SvRV(ST(1));
			offset++;
		}
		argc = items - offset;
		argv = ( SV**) palloc( argc * sizeof(SV*));
		for ( i = 0; offset < items; offset++, i++)
			argv[i] = ST(offset);
		ret_hash = plperl_spi_exec_prepared(query, attr, argc, argv);
		RETVAL = newRV_noinc((SV*)ret_hash);
		pfree( argv);
		pfree(query);
	OUTPUT:
		RETVAL

SV*
spi_spi_query_prepared(sv, ...)
	SV * sv;
	CODE:
		int i;
		SV ** argv;
		char *query = sv2cstr(sv);
		if ( items < 1)
			Perl_croak(aTHX_ "Usage: spi_query_prepared(query, "
					   "[\\@bind_values])");
		argv = ( SV**) palloc(( items - 1) * sizeof(SV*));
		for ( i = 1; i < items; i++)
			argv[i - 1] = ST(i);
		RETVAL = plperl_spi_query_prepared(query, items - 1, argv);
		pfree( argv);
		pfree(query);
	OUTPUT:
		RETVAL

void
spi_spi_freeplan(sv)
	SV *sv;
	CODE:
		char *query = sv2cstr(sv);
		plperl_spi_freeplan(query);
		pfree(query);

void
spi_spi_cursor_close(sv)
	SV *sv;
	CODE:
		char *cursor = sv2cstr(sv);
		plperl_spi_cursor_close(cursor);
		pfree(cursor);


BOOT:
    items = 0;  /* avoid 'unused variable' warning */
