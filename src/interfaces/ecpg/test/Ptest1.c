/* These two include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
/* exec sql begin declare section */

 /* VARSIZE */ struct varchar_uid
{
	int			len;
	char		arr[200];
}			uid;
struct varchar_name
{
	int			len;
	char		arr[200];
}			name;
short		value;

/* exec sql end declare section */


#include "sqlca.h"

#define		  DBCP(x,y)  strcpy(x.arr,y);x.len = strlen(x.arr)
#define		  LENFIX(x)  x.len=strlen(x.arr)
#define		  STRFIX(x)  x.arr[x.len]='\0'
#define		  SQLCODE	 sqlca.sqlcode

void
db_error(char *msg)
{
	sqlca.sqlerrm.sqlerrmc[sqlca.sqlerrm.sqlerrml] = '\0';
	printf("%s: db error %s\n", msg, sqlca.sqlerrm.sqlerrmc);
	exit(1);
}

int
main()
{
	strcpy(uid.arr, "test/test");
	LENFIX(uid);

	ECPGconnect("kom");
	if (SQLCODE)
		db_error("connect");

	strcpy(name.arr, "opt1");
	LENFIX(name);

	ECPGdo(__LINE__, "declare cur cursor for select name , value from pace_test ", ECPGt_EOIT, ECPGt_EORT);
	if (SQLCODE)
		db_error("declare");


	if (SQLCODE)
		db_error("open");

	while (1)
	{
		ECPGdo(__LINE__, "fetch in cur ", ECPGt_EOIT, ECPGt_varchar, &name, 200, 0, sizeof(struct varchar_name), ECPGt_short, &value, 0, 0, sizeof(short), ECPGt_EORT);
		if (SQLCODE)
			break;
		STRFIX(name);
		printf("%s\t%d\n", name.arr, value);
	}

	if (SQLCODE < 0)
		db_error("fetch");

	ECPGdo(__LINE__, "close cur ", ECPGt_EOIT, ECPGt_EORT);
	if (SQLCODE)
		db_error("close");
	ECPGcommit(__LINE__);
	if (SQLCODE)
		db_error("commit");

	return (0);
}
