
#include "postgres.h"

int
main()
{
#ifdef USE_LOCALE
	printf("PostgreSQL compiled with locale support\n");
	return 0;
#else
	printf("PostgreSQL compiled without locale support\n");
	return 1;
#endif
}
