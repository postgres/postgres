/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "strings.pgc"
#include <stdlib.h>


#line 1 "regression.h"






#line 3 "strings.pgc"


/* exec sql begin declare section */
      

#line 6 "strings.pgc"
 char * s1 , * s2 , * s3 , * s4 , * s5 , * s6 ;
/* exec sql end declare section */
#line 7 "strings.pgc"


int main(void)
{
  ECPGdebug(1, stderr);

  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 13 "strings.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select 'abcdef' , N'abcdef' as foo , E'abc\\bdef' as \"foo\" , U&'d\\0061t\\0061' as U&\"foo\" , U&'d!+000061t!+000061' uescape '!' , $foo$abc$def$foo$", ECPGt_EOIT, 
	ECPGt_char,&(s1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(s2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(s3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(s4),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(s5),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(s6),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 21 "strings.pgc"


  printf("%s %s %s %s %s %s\n", s1, s2, s3, s4, s5, s6);

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 25 "strings.pgc"

  exit (0);
}
