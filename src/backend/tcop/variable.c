#include "postgres.h"
#include "tcop/variable.h"

bool SetPGVariable(const char *varName, const char *value)
	{
	return TRUE;
	}

const char *GetPGVariable(const char *varName)
	{
	return NULL;
	}
