/* Glue file to use varbit before it is properly integrated with postgres */

#include "varbit.h"

bits8	   *varbit_in(char *s);
char	   *varbit_out(bits8 *s);

bits8 *
varbit_in(char *s)
{
	return varbitin(s, 0, -1);
}

/*char *
varbit_out (bits8 *s) {
  return zpbitout(s);
}
*/

char *
varbit_out(bits8 *s)
{
	return zpbitsout(s);
}
