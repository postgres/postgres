/*-------------------------------------------------------------------------
 * help_config.c
 *
 * Displays available options under grand unified configuration scheme
 *
 * Options whose flag bits are set to GUC_NO_SHOW_ALL, GUC_NOT_IN_SAMPLE,
 * or GUC_DISALLOW_IN_FILE are not displayed, unless the user specifically
 * requests that variable by name
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/help_config.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <unistd.h>

#include "utils/guc_tables.h"
#include "utils/help_config.h"


/*
 * This union allows us to mix the numerous different types of structs
 * that we are organizing.
 */
typedef union
{
	struct config_generic generic;
	struct config_bool _bool;
	struct config_real real;
	struct config_int integer;
	struct config_string string;
	struct config_enum _enum;
} mixedStruct;


static void printMixedStruct(mixedStruct *structToPrint);
static bool displayStruct(mixedStruct *structToDisplay);


void
GucInfoMain(void)
{
	struct config_generic **guc_vars;
	int			numOpts,
				i;

	/* Initialize the guc_variables[] array */
	build_guc_variables();

	guc_vars = get_guc_variables();
	numOpts = GetNumConfigOptions();

	for (i = 0; i < numOpts; i++)
	{
		mixedStruct *var = (mixedStruct *) guc_vars[i];

		if (displayStruct(var))
			printMixedStruct(var);
	}

	exit(0);
}


/*
 * This function will return true if the struct passed to it
 * should be displayed to the user.
 */
static bool
displayStruct(mixedStruct *structToDisplay)
{
	return !(structToDisplay->generic.flags & (GUC_NO_SHOW_ALL |
											   GUC_NOT_IN_SAMPLE |
											   GUC_DISALLOW_IN_FILE));
}


/*
 * This function prints out the generic struct passed to it. It will print out
 * a different format, depending on what the user wants to see.
 */
static void
printMixedStruct(mixedStruct *structToPrint)
{
	printf("%s\t%s\t%s\t",
		   structToPrint->generic.name,
		   GucContext_Names[structToPrint->generic.context],
		   _(config_group_names[structToPrint->generic.group]));

	switch (structToPrint->generic.vartype)
	{

		case PGC_BOOL:
			printf("BOOLEAN\t%s\t\t\t",
				   (structToPrint->_bool.reset_val == 0) ?
				   "FALSE" : "TRUE");
			break;

		case PGC_INT:
			printf("INTEGER\t%d\t%d\t%d\t",
				   structToPrint->integer.reset_val,
				   structToPrint->integer.min,
				   structToPrint->integer.max);
			break;

		case PGC_REAL:
			printf("REAL\t%g\t%g\t%g\t",
				   structToPrint->real.reset_val,
				   structToPrint->real.min,
				   structToPrint->real.max);
			break;

		case PGC_STRING:
			printf("STRING\t%s\t\t\t",
				   structToPrint->string.boot_val ? structToPrint->string.boot_val : "");
			break;

		case PGC_ENUM:
			printf("ENUM\t%s\t\t\t",
				   config_enum_lookup_by_value(&structToPrint->_enum,
											   structToPrint->_enum.boot_val));
			break;

		default:
			write_stderr("internal error: unrecognized run-time parameter type\n");
			break;
	}

	printf("%s\t%s\n",
		   (structToPrint->generic.short_desc == NULL) ? "" : _(structToPrint->generic.short_desc),
		   (structToPrint->generic.long_desc == NULL) ? "" : _(structToPrint->generic.long_desc));
}
