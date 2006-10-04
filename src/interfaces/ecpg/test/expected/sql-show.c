/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "show.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "show.pgc"


int
main(int argc, char *argv[])
{
	/* exec sql begin declare section */


#line 9 "show.pgc"
	char		var[25];

/* exec sql end declare section */
#line 10 "show.pgc"


	ECPGdebug(1, stderr);
	{
		ECPGconnect(__LINE__, 0, "regress1", NULL, NULL, NULL, 0);
	}
#line 13 "show.pgc"


	/* exec sql whenever sql_warning  sqlprint ; */
#line 15 "show.pgc"

	/* exec sql whenever sqlerror  sqlprint ; */
#line 16 "show.pgc"


	{
		ECPGdo(__LINE__, 0, 1, NULL, "show search_path", ECPGt_EOIT,
			   ECPGt_char, (var), (long) 25, (long) 1, (25) * sizeof(char),
			   ECPGt_NO_INDICATOR, NULL, 0L, 0L, 0L, ECPGt_EORT);
#line 18 "show.pgc"

		if (sqlca.sqlwarn[0] == 'W')
			sqlprint();
#line 18 "show.pgc"

		if (sqlca.sqlcode < 0)
			sqlprint();
	}
#line 18 "show.pgc"

	printf("Var: Search path: %s\n", var);

	{
		ECPGdo(__LINE__, 0, 1, NULL, "show wal_buffers", ECPGt_EOIT,
			   ECPGt_char, (var), (long) 25, (long) 1, (25) * sizeof(char),
			   ECPGt_NO_INDICATOR, NULL, 0L, 0L, 0L, ECPGt_EORT);
#line 21 "show.pgc"

		if (sqlca.sqlwarn[0] == 'W')
			sqlprint();
#line 21 "show.pgc"

		if (sqlca.sqlcode < 0)
			sqlprint();
	}
#line 21 "show.pgc"

	printf("Var: WAL buffers: %s\n", var);

	{
		ECPGdo(__LINE__, 0, 1, NULL, "show standard_conforming_strings", ECPGt_EOIT,
			   ECPGt_char, (var), (long) 25, (long) 1, (25) * sizeof(char),
			   ECPGt_NO_INDICATOR, NULL, 0L, 0L, 0L, ECPGt_EORT);
#line 24 "show.pgc"

		if (sqlca.sqlwarn[0] == 'W')
			sqlprint();
#line 24 "show.pgc"

		if (sqlca.sqlcode < 0)
			sqlprint();
	}
#line 24 "show.pgc"

	printf("Var: Standard conforming strings: %s\n", var);

	{
		ECPGdo(__LINE__, 0, 1, NULL, "show time zone", ECPGt_EOIT,
			   ECPGt_char, (var), (long) 25, (long) 1, (25) * sizeof(char),
			   ECPGt_NO_INDICATOR, NULL, 0L, 0L, 0L, ECPGt_EORT);
#line 27 "show.pgc"

		if (sqlca.sqlwarn[0] == 'W')
			sqlprint();
#line 27 "show.pgc"

		if (sqlca.sqlcode < 0)
			sqlprint();
	}
#line 27 "show.pgc"

	printf("Time Zone: %s\n", var);

	{
		ECPGdo(__LINE__, 0, 1, NULL, "show transaction isolation level", ECPGt_EOIT,
			   ECPGt_char, (var), (long) 25, (long) 1, (25) * sizeof(char),
			   ECPGt_NO_INDICATOR, NULL, 0L, 0L, 0L, ECPGt_EORT);
#line 30 "show.pgc"

		if (sqlca.sqlwarn[0] == 'W')
			sqlprint();
#line 30 "show.pgc"

		if (sqlca.sqlcode < 0)
			sqlprint();
	}
#line 30 "show.pgc"

	printf("Transaction isolation level: %s\n", var);

	/* Do not ask for the user name, it may differ in a regression test */
	/* EXEC SQL SHOW SESSION AUTHORIZATION INTO :var; */

	{
		ECPGdisconnect(__LINE__, "ALL");
#line 36 "show.pgc"

		if (sqlca.sqlwarn[0] == 'W')
			sqlprint();
#line 36 "show.pgc"

		if (sqlca.sqlcode < 0)
			sqlprint();
	}
#line 36 "show.pgc"


	return 0;
}
