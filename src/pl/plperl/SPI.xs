/* this must be first: */
#include "postgres.h"
/* Defined by Perl */
#undef _

/* perl stuff */
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "ppport.h"

#include "spi_internal.h"


/*
 * Implementation of plperl's elog() function
 *
 * If the error level is less than ERROR, we'll just emit the message and
 * return.  When it is ERROR, elog() will longjmp, which we catch and
 * turn into a Perl croak().  Note we are assuming that elog() can't have
 * any internal failures that are so bad as to require a transaction abort.
 *
 * This is out-of-line to suppress "might be clobbered by longjmp" warnings.
 */
static void
do_spi_elog(int level, char *message)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		elog(level, "%s", message);
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


MODULE = SPI PREFIX = spi_

PROTOTYPES: ENABLE
VERSIONCHECK: DISABLE

void
spi_elog(level, message)
	int level
	char* message
	CODE:
		if (level > ERROR)		/* no PANIC allowed thanks */
			level = ERROR;
		if (level < DEBUG5)
			level = DEBUG5;
		do_spi_elog(level, message);

int
spi_DEBUG()

int
spi_LOG()

int
spi_INFO()

int
spi_NOTICE()

int
spi_WARNING()

int
spi_ERROR()

SV*
spi_spi_exec_query(query, ...)
	char* query;
	PREINIT:
		HV *ret_hash;
		int limit = 0;
	CODE:
		if (items > 2)
			croak("Usage: spi_exec_query(query, limit) or spi_exec_query(query)");
		if (items == 2)
			limit = SvIV(ST(1));
		ret_hash = plperl_spi_exec(query, limit);
		RETVAL = newRV_noinc((SV*) ret_hash);
	OUTPUT:
		RETVAL


BOOT:
    items = 0;  /* avoid 'unused variable' warning */
