#include "utils/elog.h"

/*
 * This kludge is necessary because of the conflicting
 * definitions of 'DEBUG' between postgres and perl.
 * we'll live.
 */

#include "eloglvl.h"

int
elog_DEBUG(void)
{
	return DEBUG;
}

int
elog_ERROR(void)
{
	return ERROR;
}

int
elog_NOIND(void)
{
	return NOIND;
}

int
elog_NOTICE(void)
{
	return NOTICE;
}
