/*-------------------------------------------------------------------------
 * help_config.c
 *
 * Displays available options under grand unified configuration scheme
 *
 * The purpose of this option is to list, sort, and make searchable, all
 * runtime options available to Postgresql, by their description and grouping.
 *
 * Valid command-line options to this program:
 *
 *	none		: All available variables are sorted by group and name
 *				  and formatted nicely. ( for human consumption )
 *	<string>	: list all the variables whose name matches this string
 *	-g <string> : list all the variables whose group matches this string
 *	-l			: lists all currently defined groups and terminates
 *	-G			: no sort by groups (you get strict name order, instead)
 *	-m			: output the list in Machine friendly format, with a header row
 *	-M			: same as m, except no header
 *	-h			: help
 *
 * Options whose flag bits are set to GUC_NO_SHOW_ALL, GUC_NOT_IN_SAMPLE,
 * or GUC_DISALLOW_IN_FILE are not displayed, unless the user specifically
 * requests that variable by name
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/help_config.c,v 1.3 2003/07/28 19:31:32 tgl Exp $
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
enum outputFormat
{
	HUMAN_OUTPUT,
	MACHINE_OUTPUT
};

static const char * const GENERIC_FORMAT[] = {
	gettext_noop("Name       : %-20s \nContext    : %-20s \nGroup      : %-20s\n"),
	gettext_noop("%s\t%s\t%s\t")
};
static const char * const GENERIC_DESC[] = {
	gettext_noop("Description: %s\n%s\n"),
	gettext_noop("%s	%s\n")
};
static const char * const BOOL_FORMAT[] = {
	gettext_noop("Type       : BOOL\nReset Value: %-s \n"),
	gettext_noop("BOOL\t%s\t\t\t")
};
static const char * const INT_FORMAT[] = {
	gettext_noop("Type       : INT\nReset Value: %-20d \nMin Value  : %-20d \nMax Value  : %-20d \n"),
	gettext_noop("INT\t%d\t%d\t%d\t")
};
static const char * const REAL_FORMAT[] = {
	gettext_noop("Type       : REAL\nReset Value: %-20g \nMin Value  : %-20g \nMax Value  : %-20g \n"),
	gettext_noop("REAL\t%g\t%g\t%g\t")
};
static const char * const STRING_FORMAT[] = {
	gettext_noop("Type       : STRING\nReset Value: %-s \n"),
	gettext_noop("STRING\t%s\t\t\t")
};
static const char * const COLUMN_HEADER[] = {
	"",
	gettext_noop("NAME\tCONTEXT\tGROUP\tTYPE\tRESET_VALUE\tMIN\tMAX\tSHORT_DESCRIPTION\tLONG_DESCRIPTION\n")
};
static const char * const ROW_SEPARATOR[] = {
	"------------------------------------------------------------\n",
	""
};

/*
 * Variables loaded from the command line
 */
static char *nameString = NULL; /* The var name pattern to match */
static bool nameRegexBool = false;		/* Match the name pattern as a
										 * regex */
static char *groupString = NULL;	/* The var group pattern to match */
static bool groupRegexBool = false;		/* Match the group pattern as a
										 * regex */
static enum outputFormat outFormat = HUMAN_OUTPUT;
static bool suppressAllHeaders = false; /* MACHINE_OUTPUT output, no column
										 * headers */
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
}	mixedStruct;


/* function prototypes */
static bool varMatches(mixedStruct * structToTest);
static int	compareMixedStructs(const void *, const void *);
static mixedStruct **varsToDisplay(int *resultListSize);
static const char *usageErrMsg(void);
static void helpMessage(void);
static void listAllGroups(void);
static void printGenericHead(struct config_generic structToPrint);
static void printGenericFoot(struct config_generic structToPrint);
static void printMixedStruct(mixedStruct * structToPrint);
static bool displayStruct(mixedStruct * structToDisplay);


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

	while ((c = getopt(argc, argv, "g:rGmMlh")) != -1)
	{
		switch (c)
		{
			case 'g':
				groupString = optarg;
				break;
			case 'r':			/* not actually implemented yet */
				nameRegexBool = true;
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
				helpMessage();
				exit(0);

			default:
				fprintf(stderr, gettext("%s \n Try -h for further details\n"), usageErrMsg());
				exit(1);
		}
	}

	if (optind < argc)
		nameString = argv[optind];

	/* get the list of variables that match the user's specs. */
	varList = varsToDisplay(&resultListSize);

	/* sort them by group if desired */
	/* (without this, we get the original sort by name from guc.c) */
	if (groupResults)
		qsort(varList, resultListSize,
			  sizeof(mixedStruct *), compareMixedStructs);

	/* output the results */
	if (!suppressAllHeaders)
		printf(gettext(COLUMN_HEADER[outFormat]));

	for (i = 0; varList[i] != NULL; i++)
	{
		printf(gettext(ROW_SEPARATOR[outFormat]));
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
displayStruct(mixedStruct * structToDisplay)
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
varMatches(mixedStruct * structToTest)
{
	bool		matches = false;
	bool		specificSearch = false; /* This is true if the user
										 * searched for a variable in
										 * particular. */

	if (nameString != NULL && !nameRegexBool)
	{
		if (strstr(structToTest->generic.name, nameString) != NULL)
		{
			matches = true;
			specificSearch = true;
		}
	}

	if (nameString != NULL && nameRegexBool)
	{
		/* We do not support this option yet */
	}

	if (groupString != NULL && !groupRegexBool)
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

	if (groupString != NULL && groupRegexBool)
	{
		/* We do not support this option yet */
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
printMixedStruct(mixedStruct * structToPrint)
{
	printGenericHead(structToPrint->generic);

	switch (structToPrint->generic.vartype)
	{

		case PGC_BOOL:
			printf(gettext(BOOL_FORMAT[outFormat]),
				   (structToPrint->bool.reset_val == 0) ?
				   gettext("FALSE") : gettext("TRUE"));
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
			printf(gettext("Unrecognized variable type!\n"));
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

	printf(gettext("All currently defined groups\n"));
	printf(gettext("----------------------------\n"));
	for (i = 0; config_group_names[i] != NULL; i++)
		printf(gettext("%s\n"), gettext(config_group_names[i]));
}

static const char *
usageErrMsg(void)
{
	return gettext("Usage for --help-config option: [-h] [-g <group>] [-l] [-G] [-m] [-M] [string]\n");
}

static void
helpMessage(void)
{
	printf(gettext("Description:\n"
				   "--help-config displays all the runtime options available in PostgreSQL.\n"
				   "It groups them by category and sorts them by name. If available, it will\n"
				   "present a short description, default, max and min values as well as other\n"
				   "information about each option.\n\n"
				   "With no options specified, it will output all available runtime options\n"
				   "in human friendly format, grouped by category and sorted by name.\n\n"

				   "%s\n"

				   "General Options:\n"
			"  [string]	All options with names that match this string\n"
			   "  -g GROUP	All options in categories that match GROUP\n"
				   "  -l      	Prints list of all groups / subgroups\n"
				   "  -h      	Prints this help message\n"
				   "\nOutput Options:\n"
				   "  -G      	Do not group by category\n"
			"  -m      	Machine friendly format: tab separated fields\n"
				   "  -M      	Same as m, except header with column names is suppressed\n"),
		   usageErrMsg()
	);
}
