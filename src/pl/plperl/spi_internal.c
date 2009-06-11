/*
 * $PostgreSQL: pgsql/src/pl/plperl/spi_internal.c,v 1.10 2009/06/11 14:49:14 momjian Exp $
 *
 *
 * This kludge is necessary because of the conflicting
 * definitions of 'DEBUG' between postgres and perl.
 * we'll live.
 */

#include "postgres.h"
/* Defined by Perl */
#undef _

/* perl stuff */
#include "plperl.h"

int
spi_DEBUG(void)
{
	return DEBUG2;
}

int
spi_LOG(void)
{
	return LOG;
}

int
spi_INFO(void)
{
	return INFO;
}

int
spi_NOTICE(void)
{
	return NOTICE;
}

int
spi_WARNING(void)
{
	return WARNING;
}

int
spi_ERROR(void)
{
	return ERROR;
}
