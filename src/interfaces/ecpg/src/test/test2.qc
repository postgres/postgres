exec sql include sqlca;

#define       SQLCODE    sqlca.sqlcode

void
db_error (char *msg)
{
	sqlca.sqlerrm.sqlerrmc[sqlca.sqlerrm.sqlerrml] = '\0';
	printf ("%s: db error %s\n", msg, sqlca.sqlerrm.sqlerrmc);
	exit (1);
}

int
main ()
{
exec sql begin declare section;
varchar text[8];
exec sql end declare section;

	exec sql connect 'mm';
	if (SQLCODE)
		db_error ("connect");

	exec sql declare cur cursor for 
		select text from test;
	if (SQLCODE) db_error ("declare");

	exec sql open cur;
	if (SQLCODE)
		db_error ("open");

	while (1) {
		exec sql fetch in cur into :text;
		if (SQLCODE)
			break;
		printf ("%8.8s\n", text.arr);
	}

	if (SQLCODE < 0)
		db_error ("fetch");

	exec sql close cur;
	if (SQLCODE) db_error ("close");
	exec sql commit;
	if (SQLCODE) db_error ("commit");

	return (0);
}
