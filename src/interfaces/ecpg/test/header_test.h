exec sql include sqlca;

exec sql whenever sqlerror do PrintAndStop(msg);
exec sql whenever sqlwarning do warn();

void PrintAndStop(msg)
{
	fprintf(stderr, "Error in statement '%s':\n", msg);
	sqlprint();
	exit(-1);
}

void warn(void)
{
	fprintf(stderr, "Warning: At least one column was truncated\n");
}
