/* These two include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include "sqlca.h"

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
/* exec sql begin declare section */

 struct varchar_text { int len; char arr[8]; } text;
/* exec sql end declare section */


	ECPGconnect("mm");
	if (SQLCODE)
		db_error ("connect");

	ECPGdo(__LINE__, "declare cur cursor for select text from test ", ECPGt_EOIT, ECPGt_EORT );
	if (SQLCODE) db_error ("declare");

	
	if (SQLCODE)
		db_error ("open");

	while (1) {
		ECPGdo(__LINE__, "fetch in cur ", ECPGt_EOIT, ECPGt_varchar,&text,8,0,sizeof(struct varchar_text), ECPGt_EORT );
		if (SQLCODE)
			break;
		printf ("%8.8s\n", text.arr);
	}

	if (SQLCODE < 0)
		db_error ("fetch");

	ECPGdo(__LINE__, "close cur ", ECPGt_EOIT, ECPGt_EORT );
	if (SQLCODE) db_error ("close");
	ECPGcommit(__LINE__);
	if (SQLCODE) db_error ("commit");

	return (0);
}
