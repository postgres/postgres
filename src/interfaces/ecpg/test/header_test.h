exec sql include sqlca;

exec sql whenever sqlerror do print_and_stop();
exec sql whenever sqlwarning do warn();

void print_and_stop(void)
{
	sqlprint();
	exit(-1);
}

void warn(void)
{
	fprintf(stderr, "Warning: At least one column was truncated\n");
}
