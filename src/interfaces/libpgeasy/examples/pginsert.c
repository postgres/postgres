/*
 * insert.c
 *
*/

#include <stdio.h>
#include <time.h>
#include <libpq-fe.h>
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

	if (argc != 2)
		halt("Usage:  %s database\n", argv[0]);

	connectdb(argv[1], NULL, NULL, NULL, NULL);

	on_error_continue();
	doquery("DROP TABLE testfetch");
	on_error_stop();

	doquery("\
		CREATE TABLE testfetch( \
			aint 	int4, \
			afloat 	float4, \
			adouble float8, \
			achar	char, \
			achar16	char16, \
			abpchar char(10), \
			avarchar varchar(50), \
			atext	text, \
			aabstime abstime) \
		");

	while (1)
	{
		sprintf(query, "INSERT INTO testfetch VALUES ( \
			%d, \
			2322.12, \
			'923121.0323'::float8, \
			'A', \
			'Betty', \
			'Charley', \
			'Doug', \
			'Ernie', \
			'now' )", row);
		doquery(query);

		doquery("BEGIN WORK");
		doquery("DECLARE c_testfetch BINARY CURSOR FOR \
					SELECT * FROM testfetch");

		doquery("FETCH ALL IN c_testfetch");

		while (fetch(
					 &aint,
					 &afloat,
					 &adouble,
					 achar,
					 achar16,
					 abpchar,
					 avarchar,
					 atext,
					 &aabstime) != END_OF_TUPLES)
			printf("int %d\nfloat %f\ndouble %f\nchar %s\nchar16 %s\n\
bpchar %s\nvarchar %s\ntext %s\nabstime %s",
				   aint,
				   afloat,
				   adouble,
				   achar,
				   achar16,
				   abpchar,
				   avarchar,
				   atext,
				   ctime(&aabstime));


		doquery("CLOSE c_testfetch");
		doquery("COMMIT WORK");
		printf("--- %-d rows inserted so far\n", row);

		row++;
	}

	disconnectdb();
	return 0;
}
