/*
 * pgnulltest.c
 *
*/

#define TEST_NON_NULLS

#include <stdio.h>
#include <time.h>
#include "libpq-fe.h"
#include "../halt.h"
#include "libpgeasy.h"

int
main(int argc, char **argv)
{
	char		query[4000];
	int			row = 1;
	int			aint;
	float		afloat;
	double		adouble;
	char		achar[11],
				achar16[17],
				abpchar[11],
				avarchar[51],
				atext[51];
	time_t		aabstime;
	int			aint_null,
				afloat_null,
				adouble_null,
				achar_null,
				achar16_null,
				abpchar_null,
				avarchar_null,
				atext_null,
				aabstime_null;
	char	    optstr[256];

	if (argc != 2)
		halt("Usage:  %s database\n", argv[0]);

	snprintf(optstr, 256, "dbname=%s", argv[1]);
	connectdb(optstr);

	on_error_continue();
	doquery("DROP TABLE testfetch");
	on_error_stop();

	doquery("\
        CREATE TABLE testfetch( \
            aint    int4, \
            afloat  float4, \
            adouble float8, \
            achar   char, \
            achar16 char16, \
            abpchar char(10), \
            avarchar varchar(50), \
            atext   text, \
            aabstime abstime) \
        ");

#ifdef TEST_NON_NULLS
	sprintf(query, "INSERT INTO testfetch VALUES ( \
            0, \
			0, \
			0, \
			'', \
			'', \
			'', \
			'', \
			'', \
			'');");
#else
	sprintf(query, "INSERT INTO testfetch VALUES ( \
            NULL, \
			NULL, \
			NULL, \
			NULL, \
			NULL, \
			NULL, \
			NULL, \
			NULL, \
			NULL);");
#endif
	doquery(query);

	doquery("BEGIN WORK");
	doquery("DECLARE c_testfetch BINARY CURSOR FOR \
                    SELECT * FROM testfetch");

	doquery("FETCH ALL IN c_testfetch");

	if (fetchwithnulls(
					   &aint,
					   &aint_null,
					   &afloat,
					   &afloat_null,
					   &adouble,
					   &adouble_null,
					   achar,
					   &achar_null,
					   achar16,
					   &achar16_null,
					   abpchar,
					   &abpchar_null,
					   avarchar,
					   &avarchar_null,
					   atext,
					   &atext_null,
					   &aabstime,
					   &aabstime_null) != END_OF_TUPLES)
		printf("int %d\nfloat %f\ndouble %f\nchar %s\nchar16 %s\n\
bpchar %s\nvarchar %s\ntext %s\nabstime %s\n",
			   aint,
			   afloat,
			   adouble,
			   achar,
			   achar16,
			   abpchar,
			   avarchar,
			   atext,
			   ctime(&aabstime));
	printf("NULL:\nint %d\nfloat %d\ndouble %d\nchar %d\nchar16 %d\n\
bpchar %d\nvarchar %d\ntext %d\nabstime %d\n",
		   aint_null,
		   afloat_null,
		   adouble_null,
		   achar_null,
		   achar16_null,
		   abpchar_null,
		   avarchar_null,
		   atext_null,
		   aabstime_null);


	doquery("CLOSE c_testfetch");
	doquery("COMMIT WORK");
	printf("--- %-d rows inserted so far\n", row);

	row++;

	disconnectdb();
	return 0;
}
