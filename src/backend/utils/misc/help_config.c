/*-------------------------------------------------------------------------
 * help_config.c
 *
 * Displays available options under grand unified configuration scheme
 *
 * Options whose flag bits are set to GUC_NO_SHOW_ALL, GUC_NOT_IN_SAMPLE,
 * or GUC_DISALLOW_IN_FILE are not displayed, unless the user specifically
 * requests that variable by name
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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


static void printMixedStruct(const struct config_generic *structToPrint);
static bool displayStruct(const struct config_generic *structToDisplay);


void
GucInfoMain(void)
{
	struct config_generic **guc_vars;
	int			numOpts;

	/* Initialize the GUC hash table */
	build_guc_variables();

	guc_vars = get_guc_variables(&numOpts);

	for (int i = 0; i < numOpts; i++)
	{
		const struct config_generic *var = guc_vars[i];

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
displayStruct(const struct config_generic *structToDisplay)
{
	return !(structToDisplay->flags & (GUC_NO_SHOW_ALL |
									   GUC_NOT_IN_SAMPLE |
									   GUC_DISALLOW_IN_FILE));
}


/*
 * This function prints out the generic struct passed to it. It will print out
 * a different format, depending on what the user wants to see.
 */
static void
printMixedStruct(const struct config_generic *structToPrint)
{
	printf("%s\t%s\t%s\t",
		   structToPrint->name,
		   GucContext_Names[structToPrint->context],
		   _(config_group_names[structToPrint->group]));

	switch (structToPrint->vartype)
	{

		case PGC_BOOL:
			printf("BOOLEAN\t%s\t\t\t",
				   (structToPrint->_bool.reset_val == 0) ?
				   "FALSE" : "TRUE");
			break;

		case PGC_INT:
			printf("INTEGER\t%d\t%d\t%d\t",
				   structToPrint->_int.reset_val,
				   structToPrint->_int.min,
				   structToPrint->_int.max);
			break;

		case PGC_REAL:
			printf("REAL\t%g\t%g\t%g\t",
				   structToPrint->_real.reset_val,
				   structToPrint->_real.min,
				   structToPrint->_real.max);
			break;

		case PGC_STRING:
			printf("STRING\t%s\t\t\t",
				   structToPrint->_string.boot_val ? structToPrint->_string.boot_val : "");
			break;

		case PGC_ENUM:
			printf("ENUM\t%s\t\t\t",
				   config_enum_lookup_by_value(structToPrint,
											   structToPrint->_enum.boot_val));
			break;

		default:
			write_stderr("internal error: unrecognized run-time parameter type\n");
			break;
	}

	printf("%s\t%s\n",
		   (structToPrint->short_desc == NULL) ? "" : _(structToPrint->short_desc),
		   (structToPrint->long_desc == NULL) ? "" : _(structToPrint->long_desc));
}
