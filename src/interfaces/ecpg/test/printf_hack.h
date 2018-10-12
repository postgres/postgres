/*
 * print_double(x) has the same effect as printf("%g", x), but is intended
 * to produce the same formatting across all platforms.
 */
static void
print_double(double x)
{
#ifdef WIN32
	/* Change Windows' 3-digit exponents to look like everyone else's */
	char		convert[128];
	int			vallen;

	sprintf(convert, "%g", x);
	vallen = strlen(convert);

	if (vallen >= 6 &&
		convert[vallen - 5] == 'e' &&
		convert[vallen - 3] == '0')
	{
		convert[vallen - 3] = convert[vallen - 2];
		convert[vallen - 2] = convert[vallen - 1];
		convert[vallen - 1] = '\0';
	}

	printf("%s", convert);
#else
	printf("%g", x);
#endif
}
