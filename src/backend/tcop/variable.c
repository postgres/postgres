#include "postgres.h"
#include "tcop/variable.h"

bool SetPGVariable(const char *name, const char *value)
	{
	elog(NOTICE, "Variable %s set to \"%s\"", name, value);

	return TRUE;
	}

const char *GetPGVariable(const char *varName)
	{
	return NULL;
	}
