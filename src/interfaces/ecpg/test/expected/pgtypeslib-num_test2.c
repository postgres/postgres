/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "num_test2.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <pgtypes_numeric.h>
#include <pgtypes_error.h>
#include <decimal.h>


#line 1 "./../regression.h"






#line 7 "num_test2.pgc"


char* nums[] = { "2E394", "-2", ".794", "3.44", "592.49E21", "-32.84e4",
				 "2E-394", ".1E-2", "+.0", "-592.49E-07", "+32.84e-4",
				 ".500001", "-.5000001",
				 "1234567890123456789012345678.91", /* 30 digits should fit
				                                       into decimal */
				 "1234567890123456789012345678.921", /* 31 digits should NOT
				                                        fit into decimal */
				 NULL};


static void
check_errno(void);

int
main(void)
{
	char *text="error\n";
	char *endptr;
	numeric *num, *nin;
	decimal *dec;
	long l;
	int i, q, r, k;
	double d;

	ECPGdebug(1, stderr);

	for (i = 0; nums[i]; i++)
	{
		num = PGTYPESnumeric_from_asc(nums[i], &endptr);
		check_errno();
		if (endptr != NULL)
			printf("endptr of %d is not NULL\n", i);
		if (*endptr != '\0')
			printf("*endptr of %d is not \\0\n", i);
		text = PGTYPESnumeric_to_asc(num, -1);
		check_errno();
		printf("num[%d,1]: %s\n", i, text); free(text);
		text = PGTYPESnumeric_to_asc(num, 0);
		check_errno();
		printf("num[%d,2]: %s\n", i, text); free(text);
		text = PGTYPESnumeric_to_asc(num, 1);
		check_errno();
		printf("num[%d,3]: %s\n", i, text); free(text);
		text = PGTYPESnumeric_to_asc(num, 2);
		check_errno();
		printf("num[%d,4]: %s\n", i, text); free(text);

		nin = PGTYPESnumeric_new();
		text = PGTYPESnumeric_to_asc(nin, 2);
		check_errno();
		printf("num[%d,5]: %s\n", i, text); free(text);

		r = PGTYPESnumeric_to_long(num, &l);
		check_errno();
		printf("num[%d,6]: %ld (r: %d)\n", i, r?0L:l, r);
		if (r == 0)
		{
			r = PGTYPESnumeric_from_long(l, nin);
			check_errno();
			text = PGTYPESnumeric_to_asc(nin, 2);
			q = PGTYPESnumeric_cmp(num, nin);
			printf("num[%d,7]: %s (r: %d - cmp: %d)\n", i, text, r, q);
			free(text);
		}

		r = PGTYPESnumeric_to_int(num, &k);
		check_errno();
		printf("num[%d,8]: %d (r: %d)\n", i, r?0:k, r);
		if (r == 0)
		{
			r = PGTYPESnumeric_from_int(k, nin);
			check_errno();
			text = PGTYPESnumeric_to_asc(nin, 2);
			q = PGTYPESnumeric_cmp(num, nin);
			printf("num[%d,9]: %s (r: %d - cmp: %d)\n", i, text, r, q);
			free(text);
		}

		r = PGTYPESnumeric_to_double(num, &d);
		check_errno();
		printf("num[%d,10]: %2.7f (r: %d)\n", i, r?0.0:d, r);
		if (r == 0)
		{
			r = PGTYPESnumeric_from_double(d, nin);
			check_errno();
			text = PGTYPESnumeric_to_asc(nin, 2);
			q = PGTYPESnumeric_cmp(num, nin);
			printf("num[%d,11]: %s (r: %d - cmp: %d)\n", i, text, r, q);
			free(text);
		}

		dec = PGTYPESdecimal_new();
		r = PGTYPESnumeric_to_decimal(num, dec);
		check_errno();
		/* we have no special routine for outputting decimal, it would
		 * convert to a numeric anyway */
		printf("num[%d,12]: - (r: %d)\n", i, r);
		if (r == 0)
		{
			r = PGTYPESnumeric_from_decimal(dec, nin);
			check_errno();
			text = PGTYPESnumeric_to_asc(nin, 2);
			q = PGTYPESnumeric_cmp(num, nin);
			printf("num[%d,13]: %s (r: %d - cmp: %d)\n", i, text, r, q);
			free(text);
		}

		PGTYPESdecimal_free(dec);
		PGTYPESnumeric_free(nin);
		printf("\n");
	}

	return (0);
}

static void
check_errno(void)
{
	switch(errno)
	{
		case 0:
			printf("(no errno set) - ");
			break;
		case PGTYPES_NUM_OVERFLOW:
			printf("(errno == PGTYPES_NUM_OVERFLOW) - ");
			break;
		case PGTYPES_NUM_BAD_NUMERIC:
			printf("(errno == PGTYPES_NUM_BAD_NUMERIC) - ");
			break;
		default:
			printf("(unknown errno (%d))\n", errno);
			printf("(libc: (%s)) ", strerror(errno));
			break;
	}

}
