/* this must be first: */
#include "postgres.h"

/* perl stuff */
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "ppport.h"

#include "spi_internal.h"



MODULE = SPI PREFIX = spi_

PROTOTYPES: ENABLE
VERSIONCHECK: DISABLE

void
spi_elog(level, message)
	int level
	char* message
	CODE:
	elog(level, message);


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
		int limit=0;
	CODE:
			if (items>2) Perl_croak(aTHX_ "Usage: spi_exec_query(query, limit) or spi_exec_query(query)");
			if (items == 2) limit = SvIV(ST(1));
			ret_hash=plperl_spi_exec(query, limit);
		RETVAL = newRV_noinc((SV*)ret_hash);
	OUTPUT:
		RETVAL
