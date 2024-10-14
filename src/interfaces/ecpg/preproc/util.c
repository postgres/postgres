/* src/interfaces/ecpg/preproc/util.c */

#include "postgres_fe.h"

#include <unistd.h>

#include "preproc_extern.h"

static void vmmerror(int error_code, enum errortype type, const char *error, va_list ap) pg_attribute_printf(3, 0);


/*
 * Handle preprocessor errors and warnings
 */
static void
vmmerror(int error_code, enum errortype type, const char *error, va_list ap)
{
	/* localize the error message string */
	error = _(error);

	fprintf(stderr, "%s:%d: ", input_filename, base_yylineno);

	switch (type)
	{
		case ET_WARNING:
			fprintf(stderr, _("WARNING: "));
			break;
		case ET_ERROR:
			fprintf(stderr, _("ERROR: "));
			break;
	}

	vfprintf(stderr, error, ap);

	fprintf(stderr, "\n");

	/* If appropriate, set error code to be inspected by ecpg.c */
	switch (type)
	{
		case ET_WARNING:
			break;
		case ET_ERROR:
			ret_value = error_code;
			break;
	}
}

/* Report an error or warning */
void
mmerror(int error_code, enum errortype type, const char *error,...)
{
	va_list		ap;

	va_start(ap, error);
	vmmerror(error_code, type, error, ap);
	va_end(ap);
}

/* Report an error and abandon execution */
void
mmfatal(int error_code, const char *error,...)
{
	va_list		ap;

	va_start(ap, error);
	vmmerror(error_code, ET_ERROR, error, ap);
	va_end(ap);

	if (base_yyin)
		fclose(base_yyin);
	if (base_yyout)
		fclose(base_yyout);

	if (strcmp(output_filename, "-") != 0 && unlink(output_filename) != 0)
		fprintf(stderr, _("could not remove output file \"%s\"\n"), output_filename);
	exit(error_code);
}

/*
 * Basic memory management support
 */

/* malloc + error check */
void *
mm_alloc(size_t size)
{
	void	   *ptr = malloc(size);

	if (ptr == NULL)
		mmfatal(OUT_OF_MEMORY, "out of memory");

	return ptr;
}

/* strdup + error check */
char *
mm_strdup(const char *string)
{
	char	   *new = strdup(string);

	if (new == NULL)
		mmfatal(OUT_OF_MEMORY, "out of memory");

	return new;
}

/*
 * String concatenation
 */

/*
 * Concatenate 2 strings, inserting a space between them unless either is empty
 *
 * The input strings are freed.
 */
char *
cat2_str(char *str1, char *str2)
{
	char	   *res_str = (char *) mm_alloc(strlen(str1) + strlen(str2) + 2);

	strcpy(res_str, str1);
	if (strlen(str1) != 0 && strlen(str2) != 0)
		strcat(res_str, " ");
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return res_str;
}

/*
 * Concatenate N strings, inserting spaces between them unless they are empty
 *
 * The input strings are freed.
 */
char *
cat_str(int count,...)
{
	va_list		args;
	int			i;
	char	   *res_str;

	va_start(args, count);

	res_str = va_arg(args, char *);

	/* now add all other strings */
	for (i = 1; i < count; i++)
		res_str = cat2_str(res_str, va_arg(args, char *));

	va_end(args);

	return res_str;
}

/*
 * Concatenate 2 strings, with no space between
 *
 * The input strings are freed.
 */
char *
make2_str(char *str1, char *str2)
{
	char	   *res_str = (char *) mm_alloc(strlen(str1) + strlen(str2) + 1);

	strcpy(res_str, str1);
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return res_str;
}

/*
 * Concatenate 3 strings, with no space between
 *
 * The input strings are freed.
 */
char *
make3_str(char *str1, char *str2, char *str3)
{
	char	   *res_str = (char *) mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + 1);

	strcpy(res_str, str1);
	strcat(res_str, str2);
	strcat(res_str, str3);
	free(str1);
	free(str2);
	free(str3);
	return res_str;
}
