/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */

#line 1 "charfuncs.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <sqltypes.h>

int main(void)
{
	char t1[] = "abc  def  ghi  ";
	          /* 123456789012345 */
	char buf[50];
	int k;

	printf("t1: _%s_\n", t1);
	rupshift(t1);
	printf("t1: _%s_\n", t1);

	k = 2;
	ldchar(t1, k, buf);
	printf("byleng(t1, %d): %d, ldchar: _%s_\n", k, byleng(t1, k), buf);
	k = 5;
	ldchar(t1, k, buf);
	printf("byleng(t1, %d): %d, ldchar: _%s_\n", k, byleng(t1, k), buf);
	k = 9;
	ldchar(t1, k, buf);
	printf("byleng(t1, %d): %d, ldchar: _%s_\n", k, byleng(t1, k), buf);
	k = 15;
	ldchar(t1, k, buf);
	printf("byleng(t1, %d): %d, ldchar: _%s_\n", k, byleng(t1, k), buf);


	return 0;
}
