exec sql include sqlca;

exec sql whenever sqlerror
do
				PrintAndStop();
exec sql whenever sqlwarning
do
				warn();

void		PrintAndStop(void)
{
	sqlprint();
	exit(-1);
}

void		warn(void)
{
	fprintf(stderr, "Warning: At least one column was truncated\n");
}
