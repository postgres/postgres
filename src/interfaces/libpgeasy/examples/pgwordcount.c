/*
 * wordcount.c
 *
*/

#include <stdio.h>
#include "libpq-fe.h"
#include "../halt.h"
#include "libpgeasy.h"

int
main(int argc, char **argv)
{
	char		query[4000];
	int			row = 0;
	int			count;
	char		line[4000];
	char	    optstr[256];

	if (argc != 2)
		halt("Usage:  %s database\n", argv[0]);

	snprintf(optstr, 256, "dbname=%s", argv[1]);
	connectdb(optstr);

	on_error_continue();
	doquery("DROP TABLE words");
	on_error_stop();

	doquery("\
		CREATE TABLE words( \
			matches	int4, \
			word	text ) \
		");
	doquery("\
		CREATE INDEX i_words_1 ON words USING btree ( \
			word text_ops )\
		");

	while (1)
	{
		if (scanf("%s", line) != 1)
			break;
		doquery("BEGIN WORK");
		sprintf(query, "\
				DECLARE c_words BINARY CURSOR FOR \
				SELECT count(*) \
				FROM words \
				WHERE word = '%s'", line);
		doquery(query);
		doquery("FETCH ALL IN c_words");

		while (fetch(&count) == END_OF_TUPLES)
			count = 0;
		doquery("CLOSE c_words");
		doquery("COMMIT WORK");

		if (count == 0)
			sprintf(query, "\
				INSERT INTO words \
				VALUES (1, '%s')", line);
		else
			sprintf(query, "\
				UPDATE words \
				SET matches = matches + 1 \
				WHERE word = '%s'", line);
		doquery(query);
		row++;
	}

	disconnectdb();
	return 0;
}
