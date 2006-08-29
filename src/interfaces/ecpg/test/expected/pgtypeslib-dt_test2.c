/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "dt_test2.pgc"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pgtypes_date.h>
#include <pgtypes_timestamp.h>


#line 1 "regression.h"






#line 7 "dt_test2.pgc"


int
main(void)
{
	/* exec sql begin declare section */
		 
		 
		 
	
#line 13 "dt_test2.pgc"
 date  date1    ;
 
#line 14 "dt_test2.pgc"
 timestamp  ts1    ;
 
#line 15 "dt_test2.pgc"
 char * text    ;
/* exec sql end declare section */
#line 16 "dt_test2.pgc"


	ECPGdebug(1, stderr);

	ts1 = PGTYPEStimestamp_from_asc("2003-12-04 17:34:29", NULL);
	text = PGTYPEStimestamp_to_asc(ts1);

	printf("timestamp: %s\n", text);
	free(text);

	date1 = PGTYPESdate_from_timestamp(ts1);
	text = PGTYPESdate_to_asc(date1);
	printf("Date of timestamp: %s\n", text);
	free(text);

	return (0);
}

