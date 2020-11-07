/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "strings.pgc"

#line 1 "regression.h"






#line 3 "strings.pgc"
/* exec sql begin declare section */
#line 1 "strings.h"
	   
		   
		   
		   
		   
		   
		   
		   

#line 5 "strings.pgc"

#line 1 "strings.h"
 char * s1 , * s2 , * s3 , * s4 , * s5 , * s6 , * s7 , * s8 ;
/* exec sql end declare section */
#line 5 "strings.pgc"


int main(void)
{
  ECPGdebug(1, stderr);

  { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); }
#line 11 "strings.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set standard_conforming_strings to on", ECPGt_EOIT, ECPGt_EORT);}
#line 13 "strings.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select 'abc''d\\ef' , N'abc''d\\ef' as foo , E'abc''d\\\\ef' as \"foo\"\"bar\" , U&'d\\0061t\\0061' as U&\"foo\"\"bar\" , U&'d!+000061t!+000061' UESCAPE '!' , $foo$abc$def$foo$", ECPGt_EOIT, 
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

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select b'0010' , x'019ABcd'", ECPGt_EOIT, 
	ECPGt_char,&(s7),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(s8),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 26 "strings.pgc"


  printf("%s %s\n", s7, s8);

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 30 "strings.pgc"

  return 0;
}
