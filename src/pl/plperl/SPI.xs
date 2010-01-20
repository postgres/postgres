/**********************************************************************
 * PostgreSQL::InServer::SPI
 *
 * SPI interface for plperl.  
 *
 *    $PostgreSQL: pgsql/src/pl/plperl/SPI.xs,v 1.21 2010/01/20 01:08:21 adunstan Exp $
 *
 **********************************************************************/

/* this must be first: */
#include "postgres.h"
/* Defined by Perl */
#undef _

/* perl stuff */
#include "plperl.h"


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
spi_spi_exec_query(query, ...)
	char* query;
	PREINIT:
		HV *ret_hash;
		int limit = 0;
	CODE:
		if (items > 2)
			croak("Usage: spi_exec_query(query, limit) "
				  "or spi_exec_query(query)");
		if (items == 2)
			limit = SvIV(ST(1));
		ret_hash = plperl_spi_exec(query, limit);
		RETVAL = newRV_noinc((SV*) ret_hash);
	OUTPUT:
		RETVAL

void
spi_return_next(rv)
	SV *rv;
	CODE:
		do_plperl_return_next(rv);

SV *
spi_spi_query(query)
	char *query;
	CODE:
		RETVAL = plperl_spi_query(query);
	OUTPUT:
		RETVAL

SV *
spi_spi_fetchrow(cursor)
	char *cursor;
	CODE:
		RETVAL = plperl_spi_fetchrow(cursor);
	OUTPUT:
		RETVAL

SV*
spi_spi_prepare(query, ...)
	char* query;
	CODE:
		int i;
		SV** argv;
		if (items < 1) 
			Perl_croak(aTHX_ "Usage: spi_prepare(query, ...)");
		argv = ( SV**) palloc(( items - 1) * sizeof(SV*));
		for ( i = 1; i < items; i++) 
			argv[i - 1] = ST(i);
		RETVAL = plperl_spi_prepare(query, items - 1, argv);
		pfree( argv);
	OUTPUT:
		RETVAL

SV*
spi_spi_exec_prepared(query, ...)
	char * query;
	PREINIT:
		HV *ret_hash;
	CODE:
		HV *attr = NULL;
		int i, offset = 1, argc;
		SV ** argv;
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
	OUTPUT:
		RETVAL

SV*
spi_spi_query_prepared(query, ...)
	char * query;
	CODE:
		int i;
		SV ** argv;
		if ( items < 1) 
			Perl_croak(aTHX_ "Usage: spi_query_prepared(query, "
					   "[\\@bind_values])");
		argv = ( SV**) palloc(( items - 1) * sizeof(SV*));
		for ( i = 1; i < items; i++) 
			argv[i - 1] = ST(i);
		RETVAL = plperl_spi_query_prepared(query, items - 1, argv);
		pfree( argv);
	OUTPUT:
		RETVAL

void
spi_spi_freeplan(query)
	char *query;
	CODE:
		plperl_spi_freeplan(query);

void
spi_spi_cursor_close(cursor)
	char *cursor;
	CODE:
		plperl_spi_cursor_close(cursor);


BOOT:
    items = 0;  /* avoid 'unused variable' warning */
