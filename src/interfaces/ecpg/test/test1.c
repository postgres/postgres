exec sql begin declare section;
VARCHAR uid[200 /* VARSIZE */];
varchar name[200];
short value;
exec sql end declare section;

exec sql include sqlca;

#define       DBCP(x,y)  strcpy(x.arr,y);x.len = strlen(x.arr)
#define       LENFIX(x)  x.len=strlen(x.arr)
#define       STRFIX(x)  x.arr[x.len]='\0'
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
	strcpy (uid.arr, "test/test");
	LENFIX (uid);

	exec sql connect 'kom';
	if (SQLCODE)
		db_error ("connect");

	strcpy (name.arr, "opt1");
	LENFIX (name);

	exec sql declare cur cursor for 
		select name, value from pace_test;
	if (SQLCODE) db_error ("declare");

	exec sql open cur;
	if (SQLCODE)
		db_error ("open");

	while (1) {
		exec sql fetch in cur into :name, :value;
		if (SQLCODE)
			break;
		STRFIX (name);
		printf ("%s\t%d\n", name.arr, value);
	}

	if (SQLCODE < 0)
		db_error ("fetch");

	exec sql close cur;
	if (SQLCODE) db_error ("close");
	exec sql commit;
	if (SQLCODE) db_error ("commit");

	return (0);
}
