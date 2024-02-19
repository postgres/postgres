/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "intoasc.pgc"
#include <stdio.h>
#include <stdlib.h>

#include "pgtypes_interval.h"

/* exec sql begin declare section */
       
     

#line 7 "intoasc.pgc"
 char dirty_str [ 100 ] = "aaaaaaaaa_bbbbbbbb_ccccccccc_ddddddddd_" ;
 
#line 8 "intoasc.pgc"
 interval * interval_ptr ;
/* exec sql end declare section */
#line 9 "intoasc.pgc"


int main()
{
    interval_ptr = (interval *) malloc(sizeof(interval));
    interval_ptr->time = 100000000;
    interval_ptr->month = 240;

    printf("dirty_str contents before intoasc: %s\n", dirty_str);
    intoasc(interval_ptr, dirty_str);
    printf("dirty_str contents after intoasc: %s\n", dirty_str);
    return 0;
}
