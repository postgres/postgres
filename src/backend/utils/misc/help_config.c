/*-------------------------------------------------------------------------
 * help_config.c
 *
 * Displays available options under grand unified configuration scheme
 *
 * The purpose of this option is to list, sort, and make searchable, all
 * runtime options available to PostgreSQL, by their description and grouping.
 *
 * Options whose flag bits are set to GUC_NO_SHOW_ALL, GUC_NOT_IN_SAMPLE,
 * or GUC_DISALLOW_IN_FILE are not displayed, unless the user specifically
 * requests that variable by name
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/help_config.c,v 1.7 2003/09/27 09:29:31 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <unistd.h>

#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/help_config.h"

extern int	optind;
extern char *optarg;


/*
 * The following char constructs provide the different formats the variables
 * can be outputted in.
 */
#define HUMAN_OUTPUT 0
#define MACHINE_OUTPUT 1

static const char *const GENERIC_FORMAT[] = {
	gettext_noop("Name:          %-20s\nContext:       %-20s\nGroup:         %-20s\n"),
	"%s\t%s\t%s\t"
};
static const char *const GENERIC_DESC[] = {
	gettext_noop("Description:   %s\n%s\n"),
	"%s\t%s\n"
};
static const char *const BOOL_FORMAT[] = {
	gettext_noop("Type:          Boolean\nDefault value: %-s\n"),
	"BOOL\t%s\t\t\t"
};
static const char *const INT_FORMAT[] = {
	gettext_noop("Type:          integer\nDefault value: %-20d\nMin value:     %-20d\nMax value:     %-20d\n"),
	"INT\t%d\t%d\t%d\t"
};
static const char *const REAL_FORMAT[] = {
	gettext_noop("Type:          real\nDefault value: %-20g\nMin value:     %-20g\nMax value:     %-20g\n"),
	"REAL\t%g\t%g\t%g\t"
};
static const char *const STRING_FORMAT[] = {
	gettext_noop("Type:          string\nDefault value: %-s\n"),
	"STRING\t%s\t\t\t"
};
static const char *const ROW_SEPARATOR[] = {
	"------------------------------------------------------------\n",
	""
};

/*
 * Variables loaded from the command line
 */
static char *nameString = NULL; /* The var name pattern to match */
static char *groupString = NULL;	/* The var group pattern to match */
static int outFormat = HUMAN_OUTPUT;
static bool suppressAllHeaders = false; /* MACHINE_OUTPUT output, no
										 * column headers */
static bool groupResults = true;	/* sort result list by groups */


/*
 * This union allows us to mix the numerous different types of structs
 * that we are organizing.
 */
typedef union
{
	struct config_generic generic;
	struct config_bool bool;
	struct config_real real;
	struct config_int integer;
	struct config_string string;
} mixedStruct;


/* function prototypes */
static bool varMatches(mixedStruct *structToTest);
static int	compareMixedStructs(const void *, const void *);
static mixedStruct **varsToDisplay(int *resultListSize);
static void helpMessage(const char *progname);
static void listAllGroups(void);
static void printGenericHead(struct config_generic structToPrint);
static void printGenericFoot(struct config_generic structToPrint);
static void printMixedStruct(mixedStruct *structToPrint);
static bool displayStruct(mixedStruct *structToDisplay);


/*
 * Reads in the the command line options and sets the state of the program
 * accordingly. Initializes the result list and sorts it.
 */
int
GucInfoMain(int argc, char *argv[])
{
	mixedStruct **varList;
	int			resultListSize;
	int			c;
	int			i;

	while ((c = getopt(argc - 1, argv + 1, "g:GmMlh")) != -1)
	{
		switch (c)
		{
			case 'g':
				groupString = optarg;
				break;
			case 'G':
				groupResults = false;
				break;
			case 'm':
				outFormat = MACHINE_OUTPUT;
				break;
			case 'M':
				outFormat = MACHINE_OUTPUT;
				suppressAllHeaders = true;
				break;
			case 'l':
				listAllGroups();
				exit(0);
			case 'h':
				helpMessage(argv[0]);
				exit(0);

			default:
				fprintf(stderr, gettext("Try \"%s --help-config -h\" for more information.\n"), argv[0]);
				exit(1);
		}
	}

	if (optind < argc - 1)
		nameString = argv[optind + 1];

	/* get the list of variables that match the user's specs. */
	varList = varsToDisplay(&resultListSize);

	/* sort them by group if desired */
	/* (without this, we get the original sort by name from guc.c) */
	if (groupResults)
		qsort(varList, resultListSize,
			  sizeof(mixedStruct *), compareMixedStructs);

	/* output the results */
	if (outFormat == MACHINE_OUTPUT && !suppressAllHeaders)
		printf("NAME\tCONTEXT\tGROUP\tTYPE\tDEFAULT_VALUE\tMIN\tMAX\tSHORT_DESCRIPTION\tLONG_DESCRIPTION\n");

	for (i = 0; varList[i] != NULL; i++)
	{
		printf(ROW_SEPARATOR[outFormat]);
		printMixedStruct(varList[i]);
	}

	return 0;
}


/*
 * This function is used to compare two mixedStruct types. It compares based
 * on the value of the 'group' field, and then the name of the variable.
 * Each void* is expected to be a pointer to a pointer to a struct.
 * (This is because it is used by qsort to sort an array of struct pointers)
 *
 * Returns an integer less than, equal to, or greater than zero if the first
 * argument (struct1) is considered to be respectively less than, equal to,
 * or greater than the second (struct2). The comparison is made frist on the
 * value of struct{1,2}.generic.group and then struct{1,2}.generic.name. The
 * groups will display in the order they are defined in enum config_group
 */
static int
compareMixedStructs(const void *struct1, const void *struct2)
{
	mixedStruct *structVar1 = *(mixedStruct **) struct1;
	mixedStruct *structVar2 = *(mixedStruct **) struct2;

	if (structVar1->generic.group > structVar2->generic.group)
		return 1;
	else if (structVar1->generic.group < structVar2->generic.group)
		return -1;
	else
		return strcmp(structVar1->generic.name, structVar2->generic.name);
}


/*
 * This function returns a complete list of all the variables to display,
 * according to what the user wants to see.
 */
static mixedStruct **
varsToDisplay(int *resultListSize)
{
	mixedStruct **resultList;
	int			arrayIndex;
	int			i;

	/* Initialize the guc_variables[] array */
	build_guc_variables();

	/* Extract just the ones we want to display */
	resultList = malloc((num_guc_variables + 1) * sizeof(mixedStruct *));
	arrayIndex = 0;

	for (i = 0; i < num_guc_variables; i++)
	{
		mixedStruct *var = (mixedStruct *) guc_variables[i];

		if (varMatches(var))
			resultList[arrayIndex++] = var;
	}

	/* add an end marker */
	resultList[arrayIndex] = NULL;

	*resultListSize = arrayIndex;
	return resultList;
}


/*
 * This function will return true if the struct passed to it
 * should be displayed to the user.
 *
 * The criteria to determine if the struct should not be displayed is:
 *	+ It's flag bits are set to GUC_NO_SHOW_ALL
 *	+ It's flag bits are set to GUC_NOT_IN_SAMPLE
 *	+ It's flag bits are set to GUC_DISALLOW_IN_FILE
 */
static bool
displayStruct(mixedStruct *structToDisplay)
{
	if (structToDisplay->generic.flags & (GUC_NO_SHOW_ALL |
										  GUC_NOT_IN_SAMPLE |
										  GUC_DISALLOW_IN_FILE))
		return false;
	else
		return true;
}


/*
 * Used to determine if a variable matches the user's specifications (stored in
 * global variables). Returns true if this particular variable information should
 * be returned to the user.
 */
static bool
varMatches(mixedStruct *structToTest)
{
	bool		matches = false;
	bool		specificSearch = false; /* This is true if the user
										 * searched for a variable in
										 * particular. */

	if (nameString != NULL)
	{
		if (strstr(structToTest->generic.name, nameString) != NULL)
		{
			matches = true;
			specificSearch = true;
		}
	}

	if (groupString != NULL)
	{
		if (strstr(config_group_names[structToTest->generic.group], groupString) != NULL)
		{
			if (nameString != NULL)
				matches = (matches && true);
			else
				matches = true;
		}
		else
			matches = false;
	}

	/* return all variables */
	if (nameString == NULL && groupString == NULL)
		matches = true;

	if (specificSearch)
		return matches;
	else
		return matches && displayStruct(structToTest);
}


/*
 * This function prints out the generic struct passed to it. It will print out
 * a different format, depending on what the user wants to see.
 */
static void
printMixedStruct(mixedStruct *structToPrint)
{
	printGenericHead(structToPrint->generic);

	switch (structToPrint->generic.vartype)
	{

		case PGC_BOOL:
			if (outFormat == HUMAN_OUTPUT)
				printf(gettext(BOOL_FORMAT[outFormat]),
					   (structToPrint->bool.reset_val == 0) ?
					   gettext("false") : gettext("true"));
			else
				printf(gettext(BOOL_FORMAT[outFormat]),
					   (structToPrint->bool.reset_val == 0) ?
					   "FALSE" : "TRUE");
			break;

		case PGC_INT:
			printf(gettext(INT_FORMAT[outFormat]),
				   structToPrint->integer.reset_val,
				   structToPrint->integer.min,
				   structToPrint->integer.max);
			break;

		case PGC_REAL:
			printf(gettext(REAL_FORMAT[outFormat]),
				   structToPrint->real.reset_val,
				   structToPrint->real.min,
				   structToPrint->real.max);
			break;

		case PGC_STRING:
			printf(gettext(STRING_FORMAT[outFormat]),
				   structToPrint->string.boot_val);
			break;

		default:
			printf("Internal error: unrecognized run-time parameter type\n");
			break;
	}

	printGenericFoot(structToPrint->generic);
}

static void
printGenericHead(struct config_generic structToPrint)
{
	printf(gettext(GENERIC_FORMAT[outFormat]),
		   structToPrint.name,
		   GucContext_Names[structToPrint.context],
		   gettext(config_group_names[structToPrint.group]));
}

static void
printGenericFoot(struct config_generic sPrint)
{
	printf(gettext(GENERIC_DESC[outFormat]),
		   (sPrint.short_desc == NULL) ? "" : gettext(sPrint.short_desc),
		   (sPrint.long_desc == NULL) ? "" : gettext(sPrint.long_desc));
}

static void
listAllGroups(void)
{
	int			i;

	for (i = 0; config_group_names[i] != NULL; i++)
		printf("%s\n", gettext(config_group_names[i]));
}

static void
helpMessage(const char *progname)
{
	printf(gettext("%s --help-config displays information about the\n"
				   "run-time configuration parameters available in the PostgreSQL server.\n\n"),
		   progname);
	printf(gettext("Usage:\n  %s --help-config [OPTION]... [NAME]\n\n"), progname);
	printf(gettext("General Options:\n"));
	printf(gettext("  NAME      output information about parameters matching this name\n"));
	printf(gettext("  -g GROUP  output information about parameters matching this group\n"));
	printf(gettext("  -l        list available parameter groups\n"));
	printf(gettext("  -h        show this help, then exit\n"));
	printf(gettext("\nOutput Options:\n"));
	printf(gettext("  -G  do not group by category\n"));
	printf(gettext("  -m  machine-friendly format: tab separated fields\n"));
	printf(gettext("  -M  same as -m, but header with column names is suppressed\n"));
	printf(gettext("\n"
				   "If no parameter name is specified, all parameters are shown.  By default,\n"
				   "parameters are grouped by category, sorted by name, and output in a human-\n"
				   "friendly format.  Available information about run-time parameters includes\n"
				   "a short description, default value, maximum and minimum values.\n"));
}
