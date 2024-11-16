/*--------------------------------------------------------------------
 * guc.c
 *
 * Support for grand unified configuration scheme, including SET
 * command, configuration file, and command line options.
 *
 * This file contains the generic option processing infrastructure.
 * guc_funcs.c contains SQL-level functionality, including SET/SHOW
 * commands and various system-administration SQL functions.
 * guc_tables.c contains the arrays that define all the built-in
 * GUC variables.  Code that implements variable-specific behavior
 * is scattered around the system in check, assign, and show hooks.
 *
 * See src/backend/utils/misc/README for more information.
 *
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_parameter_acl.h"
#include "guc_internal.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "port/pg_bitutils.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/conffiles.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"


#define CONFIG_FILENAME "postgresql.conf"
#define HBA_FILENAME	"pg_hba.conf"
#define IDENT_FILENAME	"pg_ident.conf"

#ifdef EXEC_BACKEND
#define CONFIG_EXEC_PARAMS "global/config_exec_params"
#define CONFIG_EXEC_PARAMS_NEW "global/config_exec_params.new"
#endif

/*
 * Precision with which REAL type guc values are to be printed for GUC
 * serialization.
 */
#define REALTYPE_PRECISION 17

/*
 * Safe search path when executing code as the table owner, such as during
 * maintenance operations.
 */
#define GUC_SAFE_SEARCH_PATH "pg_catalog, pg_temp"

static int	GUC_check_errcode_value;

static List *reserved_class_prefix = NIL;

/* global variables for check hook support */
char	   *GUC_check_errmsg_string;
char	   *GUC_check_errdetail_string;
char	   *GUC_check_errhint_string;


/*
 * Unit conversion tables.
 *
 * There are two tables, one for memory units, and another for time units.
 * For each supported conversion from one unit to another, we have an entry
 * in the table.
 *
 * To keep things simple, and to avoid possible roundoff error,
 * conversions are never chained.  There needs to be a direct conversion
 * between all units (of the same type).
 *
 * The conversions for each base unit must be kept in order from greatest to
 * smallest human-friendly unit; convert_xxx_from_base_unit() rely on that.
 * (The order of the base-unit groups does not matter.)
 */
#define MAX_UNIT_LEN		3	/* length of longest recognized unit string */

typedef struct
{
	char		unit[MAX_UNIT_LEN + 1]; /* unit, as a string, like "kB" or
										 * "min" */
	int			base_unit;		/* GUC_UNIT_XXX */
	double		multiplier;		/* Factor for converting unit -> base_unit */
} unit_conversion;

/* Ensure that the constants in the tables don't overflow or underflow */
#if BLCKSZ < 1024 || BLCKSZ > (1024*1024)
#error BLCKSZ must be between 1KB and 1MB
#endif
#if XLOG_BLCKSZ < 1024 || XLOG_BLCKSZ > (1024*1024)
#error XLOG_BLCKSZ must be between 1KB and 1MB
#endif

static const char *const memory_units_hint = gettext_noop("Valid units for this parameter are \"B\", \"kB\", \"MB\", \"GB\", and \"TB\".");

static const unit_conversion memory_unit_conversion_table[] =
{
	{"TB", GUC_UNIT_BYTE, 1024.0 * 1024.0 * 1024.0 * 1024.0},
	{"GB", GUC_UNIT_BYTE, 1024.0 * 1024.0 * 1024.0},
	{"MB", GUC_UNIT_BYTE, 1024.0 * 1024.0},
	{"kB", GUC_UNIT_BYTE, 1024.0},
	{"B", GUC_UNIT_BYTE, 1.0},

	{"TB", GUC_UNIT_KB, 1024.0 * 1024.0 * 1024.0},
	{"GB", GUC_UNIT_KB, 1024.0 * 1024.0},
	{"MB", GUC_UNIT_KB, 1024.0},
	{"kB", GUC_UNIT_KB, 1.0},
	{"B", GUC_UNIT_KB, 1.0 / 1024.0},

	{"TB", GUC_UNIT_MB, 1024.0 * 1024.0},
	{"GB", GUC_UNIT_MB, 1024.0},
	{"MB", GUC_UNIT_MB, 1.0},
	{"kB", GUC_UNIT_MB, 1.0 / 1024.0},
	{"B", GUC_UNIT_MB, 1.0 / (1024.0 * 1024.0)},

	{"TB", GUC_UNIT_BLOCKS, (1024.0 * 1024.0 * 1024.0) / (BLCKSZ / 1024)},
	{"GB", GUC_UNIT_BLOCKS, (1024.0 * 1024.0) / (BLCKSZ / 1024)},
	{"MB", GUC_UNIT_BLOCKS, 1024.0 / (BLCKSZ / 1024)},
	{"kB", GUC_UNIT_BLOCKS, 1.0 / (BLCKSZ / 1024)},
	{"B", GUC_UNIT_BLOCKS, 1.0 / BLCKSZ},

	{"TB", GUC_UNIT_XBLOCKS, (1024.0 * 1024.0 * 1024.0) / (XLOG_BLCKSZ / 1024)},
	{"GB", GUC_UNIT_XBLOCKS, (1024.0 * 1024.0) / (XLOG_BLCKSZ / 1024)},
	{"MB", GUC_UNIT_XBLOCKS, 1024.0 / (XLOG_BLCKSZ / 1024)},
	{"kB", GUC_UNIT_XBLOCKS, 1.0 / (XLOG_BLCKSZ / 1024)},
	{"B", GUC_UNIT_XBLOCKS, 1.0 / XLOG_BLCKSZ},

	{""}						/* end of table marker */
};

static const char *const time_units_hint = gettext_noop("Valid units for this parameter are \"us\", \"ms\", \"s\", \"min\", \"h\", and \"d\".");

static const unit_conversion time_unit_conversion_table[] =
{
	{"d", GUC_UNIT_MS, 1000 * 60 * 60 * 24},
	{"h", GUC_UNIT_MS, 1000 * 60 * 60},
	{"min", GUC_UNIT_MS, 1000 * 60},
	{"s", GUC_UNIT_MS, 1000},
	{"ms", GUC_UNIT_MS, 1},
	{"us", GUC_UNIT_MS, 1.0 / 1000},

	{"d", GUC_UNIT_S, 60 * 60 * 24},
	{"h", GUC_UNIT_S, 60 * 60},
	{"min", GUC_UNIT_S, 60},
	{"s", GUC_UNIT_S, 1},
	{"ms", GUC_UNIT_S, 1.0 / 1000},
	{"us", GUC_UNIT_S, 1.0 / (1000 * 1000)},

	{"d", GUC_UNIT_MIN, 60 * 24},
	{"h", GUC_UNIT_MIN, 60},
	{"min", GUC_UNIT_MIN, 1},
	{"s", GUC_UNIT_MIN, 1.0 / 60},
	{"ms", GUC_UNIT_MIN, 1.0 / (1000 * 60)},
	{"us", GUC_UNIT_MIN, 1.0 / (1000 * 1000 * 60)},

	{""}						/* end of table marker */
};

/*
 * To allow continued support of obsolete names for GUC variables, we apply
 * the following mappings to any unrecognized name.  Note that an old name
 * should be mapped to a new one only if the new variable has very similar
 * semantics to the old.
 */
static const char *const map_old_guc_names[] = {
	"sort_mem", "work_mem",
	"vacuum_mem", "maintenance_work_mem",
	"ssl_ecdh_curve", "ssl_groups",
	NULL
};


/* Memory context holding all GUC-related data */
static MemoryContext GUCMemoryContext;

/*
 * We use a dynahash table to look up GUCs by name, or to iterate through
 * all the GUCs.  The gucname field is redundant with gucvar->name, but
 * dynahash makes it too painful to not store the hash key separately.
 */
typedef struct
{
	const char *gucname;		/* hash key */
	struct config_generic *gucvar;	/* -> GUC's defining structure */
} GUCHashEntry;

static HTAB *guc_hashtab;		/* entries are GUCHashEntrys */

/*
 * In addition to the hash table, variables having certain properties are
 * linked into these lists, so that we can find them without scanning the
 * whole hash table.  In most applications, only a small fraction of the
 * GUCs appear in these lists at any given time.  The usage of the stack
 * and report lists is stylized enough that they can be slists, but the
 * nondef list has to be a dlist to avoid O(N) deletes in common cases.
 */
static dlist_head guc_nondef_list;	/* list of variables that have source
									 * different from PGC_S_DEFAULT */
static slist_head guc_stack_list;	/* list of variables that have non-NULL
									 * stack */
static slist_head guc_report_list;	/* list of variables that have the
									 * GUC_NEEDS_REPORT bit set in status */

static bool reporting_enabled;	/* true to enable GUC_REPORT */

static int	GUCNestLevel = 0;	/* 1 when in main transaction */


static int	guc_var_compare(const void *a, const void *b);
static uint32 guc_name_hash(const void *key, Size keysize);
static int	guc_name_match(const void *key1, const void *key2, Size keysize);
static void InitializeGUCOptionsFromEnvironment(void);
static void InitializeOneGUCOption(struct config_generic *gconf);
static void RemoveGUCFromLists(struct config_generic *gconf);
static void set_guc_source(struct config_generic *gconf, GucSource newsource);
static void pg_timezone_abbrev_initialize(void);
static void push_old_value(struct config_generic *gconf, GucAction action);
static void ReportGUCOption(struct config_generic *record);
static void set_config_sourcefile(const char *name, char *sourcefile,
								  int sourceline);
static void reapply_stacked_values(struct config_generic *variable,
								   struct config_string *pHolder,
								   GucStack *stack,
								   const char *curvalue,
								   GucContext curscontext, GucSource cursource,
								   Oid cursrole);
static bool validate_option_array_item(const char *name, const char *value,
									   bool skipIfNoPermissions);
static void write_auto_conf_file(int fd, const char *filename, ConfigVariable *head);
static void replace_auto_config_value(ConfigVariable **head_p, ConfigVariable **tail_p,
									  const char *name, const char *value);
static bool valid_custom_variable_name(const char *name);
static bool assignable_custom_variable_name(const char *name, bool skip_errors,
											int elevel);
static void do_serialize(char **destptr, Size *maxbytes,
						 const char *fmt,...) pg_attribute_printf(3, 4);
static bool call_bool_check_hook(struct config_bool *conf, bool *newval,
								 void **extra, GucSource source, int elevel);
static bool call_int_check_hook(struct config_int *conf, int *newval,
								void **extra, GucSource source, int elevel);
static bool call_real_check_hook(struct config_real *conf, double *newval,
								 void **extra, GucSource source, int elevel);
static bool call_string_check_hook(struct config_string *conf, char **newval,
								   void **extra, GucSource source, int elevel);
static bool call_enum_check_hook(struct config_enum *conf, int *newval,
								 void **extra, GucSource source, int elevel);


/*
 * This function handles both actual config file (re)loads and execution of
 * show_all_file_settings() (i.e., the pg_file_settings view).  In the latter
 * case we don't apply any of the settings, but we make all the usual validity
 * checks, and we return the ConfigVariable list so that it can be printed out
 * by show_all_file_settings().
 */
ConfigVariable *
ProcessConfigFileInternal(GucContext context, bool applySettings, int elevel)
{
	bool		error = false;
	bool		applying = false;
	const char *ConfFileWithError;
	ConfigVariable *item,
			   *head,
			   *tail;
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;

	/* Parse the main config file into a list of option names and values */
	ConfFileWithError = ConfigFileName;
	head = tail = NULL;

	if (!ParseConfigFile(ConfigFileName, true,
						 NULL, 0, CONF_FILE_START_DEPTH, elevel,
						 &head, &tail))
	{
		/* Syntax error(s) detected in the file, so bail out */
		error = true;
		goto bail_out;
	}

	/*
	 * Parse the PG_AUTOCONF_FILENAME file, if present, after the main file to
	 * replace any parameters set by ALTER SYSTEM command.  Because this file
	 * is in the data directory, we can't read it until the DataDir has been
	 * set.
	 */
	if (DataDir)
	{
		if (!ParseConfigFile(PG_AUTOCONF_FILENAME, false,
							 NULL, 0, CONF_FILE_START_DEPTH, elevel,
							 &head, &tail))
		{
			/* Syntax error(s) detected in the file, so bail out */
			error = true;
			ConfFileWithError = PG_AUTOCONF_FILENAME;
			goto bail_out;
		}
	}
	else
	{
		/*
		 * If DataDir is not set, the PG_AUTOCONF_FILENAME file cannot be
		 * read.  In this case, we don't want to accept any settings but
		 * data_directory from postgresql.conf, because they might be
		 * overwritten with settings in the PG_AUTOCONF_FILENAME file which
		 * will be read later. OTOH, since data_directory isn't allowed in the
		 * PG_AUTOCONF_FILENAME file, it will never be overwritten later.
		 */
		ConfigVariable *newlist = NULL;

		/*
		 * Prune all items except the last "data_directory" from the list.
		 */
		for (item = head; item; item = item->next)
		{
			if (!item->ignore &&
				strcmp(item->name, "data_directory") == 0)
				newlist = item;
		}

		if (newlist)
			newlist->next = NULL;
		head = tail = newlist;

		/*
		 * Quick exit if data_directory is not present in file.
		 *
		 * We need not do any further processing, in particular we don't set
		 * PgReloadTime; that will be set soon by subsequent full loading of
		 * the config file.
		 */
		if (head == NULL)
			goto bail_out;
	}

	/*
	 * Mark all extant GUC variables as not present in the config file. We
	 * need this so that we can tell below which ones have been removed from
	 * the file since we last processed it.
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *gconf = hentry->gucvar;

		gconf->status &= ~GUC_IS_IN_FILE;
	}

	/*
	 * Check if all the supplied option names are valid, as an additional
	 * quasi-syntactic check on the validity of the config file.  It is
	 * important that the postmaster and all backends agree on the results of
	 * this phase, else we will have strange inconsistencies about which
	 * processes accept a config file update and which don't.  Hence, unknown
	 * custom variable names have to be accepted without complaint.  For the
	 * same reason, we don't attempt to validate the options' values here.
	 *
	 * In addition, the GUC_IS_IN_FILE flag is set on each existing GUC
	 * variable mentioned in the file; and we detect duplicate entries in the
	 * file and mark the earlier occurrences as ignorable.
	 */
	for (item = head; item; item = item->next)
	{
		struct config_generic *record;

		/* Ignore anything already marked as ignorable */
		if (item->ignore)
			continue;

		/*
		 * Try to find the variable; but do not create a custom placeholder if
		 * it's not there already.
		 */
		record = find_option(item->name, false, true, elevel);

		if (record)
		{
			/* If it's already marked, then this is a duplicate entry */
			if (record->status & GUC_IS_IN_FILE)
			{
				/*
				 * Mark the earlier occurrence(s) as dead/ignorable.  We could
				 * avoid the O(N^2) behavior here with some additional state,
				 * but it seems unlikely to be worth the trouble.
				 */
				ConfigVariable *pitem;

				for (pitem = head; pitem != item; pitem = pitem->next)
				{
					if (!pitem->ignore &&
						strcmp(pitem->name, item->name) == 0)
						pitem->ignore = true;
				}
			}
			/* Now mark it as present in file */
			record->status |= GUC_IS_IN_FILE;
		}
		else if (!valid_custom_variable_name(item->name))
		{
			/* Invalid non-custom variable, so complain */
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("unrecognized configuration parameter \"%s\" in file \"%s\" line %d",
							item->name,
							item->filename, item->sourceline)));
			item->errmsg = pstrdup("unrecognized configuration parameter");
			error = true;
			ConfFileWithError = item->filename;
		}
	}

	/*
	 * If we've detected any errors so far, we don't want to risk applying any
	 * changes.
	 */
	if (error)
		goto bail_out;

	/* Otherwise, set flag that we're beginning to apply changes */
	applying = true;

	/*
	 * Check for variables having been removed from the config file, and
	 * revert their reset values (and perhaps also effective values) to the
	 * boot-time defaults.  If such a variable can't be changed after startup,
	 * report that and continue.
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *gconf = hentry->gucvar;
		GucStack   *stack;

		if (gconf->reset_source != PGC_S_FILE ||
			(gconf->status & GUC_IS_IN_FILE))
			continue;
		if (gconf->context < PGC_SIGHUP)
		{
			/* The removal can't be effective without a restart */
			gconf->status |= GUC_PENDING_RESTART;
			ereport(elevel,
					(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
					 errmsg("parameter \"%s\" cannot be changed without restarting the server",
							gconf->name)));
			record_config_file_error(psprintf("parameter \"%s\" cannot be changed without restarting the server",
											  gconf->name),
									 NULL, 0,
									 &head, &tail);
			error = true;
			continue;
		}

		/* No more to do if we're just doing show_all_file_settings() */
		if (!applySettings)
			continue;

		/*
		 * Reset any "file" sources to "default", else set_config_option will
		 * not override those settings.
		 */
		if (gconf->reset_source == PGC_S_FILE)
			gconf->reset_source = PGC_S_DEFAULT;
		if (gconf->source == PGC_S_FILE)
			set_guc_source(gconf, PGC_S_DEFAULT);
		for (stack = gconf->stack; stack; stack = stack->prev)
		{
			if (stack->source == PGC_S_FILE)
				stack->source = PGC_S_DEFAULT;
		}

		/* Now we can re-apply the wired-in default (i.e., the boot_val) */
		if (set_config_option(gconf->name, NULL,
							  context, PGC_S_DEFAULT,
							  GUC_ACTION_SET, true, 0, false) > 0)
		{
			/* Log the change if appropriate */
			if (context == PGC_SIGHUP)
				ereport(elevel,
						(errmsg("parameter \"%s\" removed from configuration file, reset to default",
								gconf->name)));
		}
	}

	/*
	 * Restore any variables determined by environment variables or
	 * dynamically-computed defaults.  This is a no-op except in the case
	 * where one of these had been in the config file and is now removed.
	 *
	 * In particular, we *must not* do this during the postmaster's initial
	 * loading of the file, since the timezone functions in particular should
	 * be run only after initialization is complete.
	 *
	 * XXX this is an unmaintainable crock, because we have to know how to set
	 * (or at least what to call to set) every non-PGC_INTERNAL variable that
	 * could potentially have PGC_S_DYNAMIC_DEFAULT or PGC_S_ENV_VAR source.
	 */
	if (context == PGC_SIGHUP && applySettings)
	{
		InitializeGUCOptionsFromEnvironment();
		pg_timezone_abbrev_initialize();
		/* this selects SQL_ASCII in processes not connected to a database */
		SetConfigOption("client_encoding", GetDatabaseEncodingName(),
						PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);
	}

	/*
	 * Now apply the values from the config file.
	 */
	for (item = head; item; item = item->next)
	{
		char	   *pre_value = NULL;
		int			scres;

		/* Ignore anything marked as ignorable */
		if (item->ignore)
			continue;

		/* In SIGHUP cases in the postmaster, we want to report changes */
		if (context == PGC_SIGHUP && applySettings && !IsUnderPostmaster)
		{
			const char *preval = GetConfigOption(item->name, true, false);

			/* If option doesn't exist yet or is NULL, treat as empty string */
			if (!preval)
				preval = "";
			/* must dup, else might have dangling pointer below */
			pre_value = pstrdup(preval);
		}

		scres = set_config_option(item->name, item->value,
								  context, PGC_S_FILE,
								  GUC_ACTION_SET, applySettings, 0, false);
		if (scres > 0)
		{
			/* variable was updated, so log the change if appropriate */
			if (pre_value)
			{
				const char *post_value = GetConfigOption(item->name, true, false);

				if (!post_value)
					post_value = "";
				if (strcmp(pre_value, post_value) != 0)
					ereport(elevel,
							(errmsg("parameter \"%s\" changed to \"%s\"",
									item->name, item->value)));
			}
			item->applied = true;
		}
		else if (scres == 0)
		{
			error = true;
			item->errmsg = pstrdup("setting could not be applied");
			ConfFileWithError = item->filename;
		}
		else
		{
			/* no error, but variable's active value was not changed */
			item->applied = true;
		}

		/*
		 * We should update source location unless there was an error, since
		 * even if the active value didn't change, the reset value might have.
		 * (In the postmaster, there won't be a difference, but it does matter
		 * in backends.)
		 */
		if (scres != 0 && applySettings)
			set_config_sourcefile(item->name, item->filename,
								  item->sourceline);

		if (pre_value)
			pfree(pre_value);
	}

	/* Remember when we last successfully loaded the config file. */
	if (applySettings)
		PgReloadTime = GetCurrentTimestamp();

bail_out:
	if (error && applySettings)
	{
		/* During postmaster startup, any error is fatal */
		if (context == PGC_POSTMASTER)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors",
							ConfFileWithError)));
		else if (applying)
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors; unaffected changes were applied",
							ConfFileWithError)));
		else
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors; no changes were applied",
							ConfFileWithError)));
	}

	/* Successful or otherwise, return the collected data list */
	return head;
}


/*
 * Some infrastructure for GUC-related memory allocation
 *
 * These functions are generally modeled on libc's malloc/realloc/etc,
 * but any OOM issue is reported at the specified elevel.
 * (Thus, control returns only if that's less than ERROR.)
 */
void *
guc_malloc(int elevel, size_t size)
{
	void	   *data;

	data = MemoryContextAllocExtended(GUCMemoryContext, size,
									  MCXT_ALLOC_NO_OOM);
	if (unlikely(data == NULL))
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return data;
}

void *
guc_realloc(int elevel, void *old, size_t size)
{
	void	   *data;

	if (old != NULL)
	{
		/* This is to help catch old code that malloc's GUC data. */
		Assert(GetMemoryChunkContext(old) == GUCMemoryContext);
		data = repalloc_extended(old, size,
								 MCXT_ALLOC_NO_OOM);
	}
	else
	{
		/* Like realloc(3), but not like repalloc(), we allow old == NULL. */
		data = MemoryContextAllocExtended(GUCMemoryContext, size,
										  MCXT_ALLOC_NO_OOM);
	}
	if (unlikely(data == NULL))
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return data;
}

char *
guc_strdup(int elevel, const char *src)
{
	char	   *data;
	size_t		len = strlen(src) + 1;

	data = guc_malloc(elevel, len);
	if (likely(data != NULL))
		memcpy(data, src, len);
	return data;
}

void
guc_free(void *ptr)
{
	/*
	 * Historically, GUC-related code has relied heavily on the ability to do
	 * free(NULL), so we allow that here even though pfree() doesn't.
	 */
	if (ptr != NULL)
	{
		/* This is to help catch old code that malloc's GUC data. */
		Assert(GetMemoryChunkContext(ptr) == GUCMemoryContext);
		pfree(ptr);
	}
}


/*
 * Detect whether strval is referenced anywhere in a GUC string item
 */
static bool
string_field_used(struct config_string *conf, char *strval)
{
	GucStack   *stack;

	if (strval == *(conf->variable) ||
		strval == conf->reset_val ||
		strval == conf->boot_val)
		return true;
	for (stack = conf->gen.stack; stack; stack = stack->prev)
	{
		if (strval == stack->prior.val.stringval ||
			strval == stack->masked.val.stringval)
			return true;
	}
	return false;
}

/*
 * Support for assigning to a field of a string GUC item.  Free the prior
 * value if it's not referenced anywhere else in the item (including stacked
 * states).
 */
static void
set_string_field(struct config_string *conf, char **field, char *newval)
{
	char	   *oldval = *field;

	/* Do the assignment */
	*field = newval;

	/* Free old value if it's not NULL and isn't referenced anymore */
	if (oldval && !string_field_used(conf, oldval))
		guc_free(oldval);
}

/*
 * Detect whether an "extra" struct is referenced anywhere in a GUC item
 */
static bool
extra_field_used(struct config_generic *gconf, void *extra)
{
	GucStack   *stack;

	if (extra == gconf->extra)
		return true;
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			if (extra == ((struct config_bool *) gconf)->reset_extra)
				return true;
			break;
		case PGC_INT:
			if (extra == ((struct config_int *) gconf)->reset_extra)
				return true;
			break;
		case PGC_REAL:
			if (extra == ((struct config_real *) gconf)->reset_extra)
				return true;
			break;
		case PGC_STRING:
			if (extra == ((struct config_string *) gconf)->reset_extra)
				return true;
			break;
		case PGC_ENUM:
			if (extra == ((struct config_enum *) gconf)->reset_extra)
				return true;
			break;
	}
	for (stack = gconf->stack; stack; stack = stack->prev)
	{
		if (extra == stack->prior.extra ||
			extra == stack->masked.extra)
			return true;
	}

	return false;
}

/*
 * Support for assigning to an "extra" field of a GUC item.  Free the prior
 * value if it's not referenced anywhere else in the item (including stacked
 * states).
 */
static void
set_extra_field(struct config_generic *gconf, void **field, void *newval)
{
	void	   *oldval = *field;

	/* Do the assignment */
	*field = newval;

	/* Free old value if it's not NULL and isn't referenced anymore */
	if (oldval && !extra_field_used(gconf, oldval))
		guc_free(oldval);
}

/*
 * Support for copying a variable's active value into a stack entry.
 * The "extra" field associated with the active value is copied, too.
 *
 * NB: be sure stringval and extra fields of a new stack entry are
 * initialized to NULL before this is used, else we'll try to guc_free() them.
 */
static void
set_stack_value(struct config_generic *gconf, config_var_value *val)
{
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			val->val.boolval =
				*((struct config_bool *) gconf)->variable;
			break;
		case PGC_INT:
			val->val.intval =
				*((struct config_int *) gconf)->variable;
			break;
		case PGC_REAL:
			val->val.realval =
				*((struct config_real *) gconf)->variable;
			break;
		case PGC_STRING:
			set_string_field((struct config_string *) gconf,
							 &(val->val.stringval),
							 *((struct config_string *) gconf)->variable);
			break;
		case PGC_ENUM:
			val->val.enumval =
				*((struct config_enum *) gconf)->variable;
			break;
	}
	set_extra_field(gconf, &(val->extra), gconf->extra);
}

/*
 * Support for discarding a no-longer-needed value in a stack entry.
 * The "extra" field associated with the stack entry is cleared, too.
 */
static void
discard_stack_value(struct config_generic *gconf, config_var_value *val)
{
	switch (gconf->vartype)
	{
		case PGC_BOOL:
		case PGC_INT:
		case PGC_REAL:
		case PGC_ENUM:
			/* no need to do anything */
			break;
		case PGC_STRING:
			set_string_field((struct config_string *) gconf,
							 &(val->val.stringval),
							 NULL);
			break;
	}
	set_extra_field(gconf, &(val->extra), NULL);
}


/*
 * Fetch a palloc'd, sorted array of GUC struct pointers
 *
 * The array length is returned into *num_vars.
 */
struct config_generic **
get_guc_variables(int *num_vars)
{
	struct config_generic **result;
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;
	int			i;

	*num_vars = hash_get_num_entries(guc_hashtab);
	result = palloc(sizeof(struct config_generic *) * *num_vars);

	/* Extract pointers from the hash table */
	i = 0;
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
		result[i++] = hentry->gucvar;
	Assert(i == *num_vars);

	/* Sort by name */
	qsort(result, *num_vars,
		  sizeof(struct config_generic *), guc_var_compare);

	return result;
}


/*
 * Build the GUC hash table.  This is split out so that help_config.c can
 * extract all the variables without running all of InitializeGUCOptions.
 * It's not meant for use anyplace else.
 */
void
build_guc_variables(void)
{
	int			size_vars;
	int			num_vars = 0;
	HASHCTL		hash_ctl;
	GUCHashEntry *hentry;
	bool		found;
	int			i;

	/*
	 * Create the memory context that will hold all GUC-related data.
	 */
	Assert(GUCMemoryContext == NULL);
	GUCMemoryContext = AllocSetContextCreate(TopMemoryContext,
											 "GUCMemoryContext",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Count all the built-in variables, and set their vartypes correctly.
	 */
	for (i = 0; ConfigureNamesBool[i].gen.name; i++)
	{
		struct config_bool *conf = &ConfigureNamesBool[i];

		/* Rather than requiring vartype to be filled in by hand, do this: */
		conf->gen.vartype = PGC_BOOL;
		num_vars++;
	}

	for (i = 0; ConfigureNamesInt[i].gen.name; i++)
	{
		struct config_int *conf = &ConfigureNamesInt[i];

		conf->gen.vartype = PGC_INT;
		num_vars++;
	}

	for (i = 0; ConfigureNamesReal[i].gen.name; i++)
	{
		struct config_real *conf = &ConfigureNamesReal[i];

		conf->gen.vartype = PGC_REAL;
		num_vars++;
	}

	for (i = 0; ConfigureNamesString[i].gen.name; i++)
	{
		struct config_string *conf = &ConfigureNamesString[i];

		conf->gen.vartype = PGC_STRING;
		num_vars++;
	}

	for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
	{
		struct config_enum *conf = &ConfigureNamesEnum[i];

		conf->gen.vartype = PGC_ENUM;
		num_vars++;
	}

	/*
	 * Create hash table with 20% slack
	 */
	size_vars = num_vars + num_vars / 4;

	hash_ctl.keysize = sizeof(char *);
	hash_ctl.entrysize = sizeof(GUCHashEntry);
	hash_ctl.hash = guc_name_hash;
	hash_ctl.match = guc_name_match;
	hash_ctl.hcxt = GUCMemoryContext;
	guc_hashtab = hash_create("GUC hash table",
							  size_vars,
							  &hash_ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	for (i = 0; ConfigureNamesBool[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesBool[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesInt[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesInt[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesReal[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesReal[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesString[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesString[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesEnum[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	Assert(num_vars == hash_get_num_entries(guc_hashtab));
}

/*
 * Add a new GUC variable to the hash of known variables. The
 * hash is expanded if needed.
 */
static bool
add_guc_variable(struct config_generic *var, int elevel)
{
	GUCHashEntry *hentry;
	bool		found;

	hentry = (GUCHashEntry *) hash_search(guc_hashtab,
										  &var->name,
										  HASH_ENTER_NULL,
										  &found);
	if (unlikely(hentry == NULL))
	{
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return false;			/* out of memory */
	}
	Assert(!found);
	hentry->gucvar = var;
	return true;
}

/*
 * Decide whether a proposed custom variable name is allowed.
 *
 * It must be two or more identifiers separated by dots, where the rules
 * for what is an identifier agree with scan.l.  (If you change this rule,
 * adjust the errdetail in assignable_custom_variable_name().)
 */
static bool
valid_custom_variable_name(const char *name)
{
	bool		saw_sep = false;
	bool		name_start = true;

	for (const char *p = name; *p; p++)
	{
		if (*p == GUC_QUALIFIER_SEPARATOR)
		{
			if (name_start)
				return false;	/* empty name component */
			saw_sep = true;
			name_start = true;
		}
		else if (strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						"abcdefghijklmnopqrstuvwxyz_", *p) != NULL ||
				 IS_HIGHBIT_SET(*p))
		{
			/* okay as first or non-first character */
			name_start = false;
		}
		else if (!name_start && strchr("0123456789$", *p) != NULL)
			 /* okay as non-first character */ ;
		else
			return false;
	}
	if (name_start)
		return false;			/* empty name component */
	/* OK if we found at least one separator */
	return saw_sep;
}

/*
 * Decide whether an unrecognized variable name is allowed to be SET.
 *
 * It must pass the syntactic rules of valid_custom_variable_name(),
 * and it must not be in any namespace already reserved by an extension.
 * (We make this separate from valid_custom_variable_name() because we don't
 * apply the reserved-namespace test when reading configuration files.)
 *
 * If valid, return true.  Otherwise, return false if skip_errors is true,
 * else throw a suitable error at the specified elevel (and return false
 * if that's less than ERROR).
 */
static bool
assignable_custom_variable_name(const char *name, bool skip_errors, int elevel)
{
	/* If there's no separator, it can't be a custom variable */
	const char *sep = strchr(name, GUC_QUALIFIER_SEPARATOR);

	if (sep != NULL)
	{
		size_t		classLen = sep - name;
		ListCell   *lc;

		/* The name must be syntactically acceptable ... */
		if (!valid_custom_variable_name(name))
		{
			if (!skip_errors)
				ereport(elevel,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("invalid configuration parameter name \"%s\"",
								name),
						 errdetail("Custom parameter names must be two or more simple identifiers separated by dots.")));
			return false;
		}
		/* ... and it must not match any previously-reserved prefix */
		foreach(lc, reserved_class_prefix)
		{
			const char *rcprefix = lfirst(lc);

			if (strlen(rcprefix) == classLen &&
				strncmp(name, rcprefix, classLen) == 0)
			{
				if (!skip_errors)
					ereport(elevel,
							(errcode(ERRCODE_INVALID_NAME),
							 errmsg("invalid configuration parameter name \"%s\"",
									name),
							 errdetail("\"%s\" is a reserved prefix.",
									   rcprefix)));
				return false;
			}
		}
		/* OK to create it */
		return true;
	}

	/* Unrecognized single-part name */
	if (!skip_errors)
		ereport(elevel,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"",
						name)));
	return false;
}

/*
 * Create and add a placeholder variable for a custom variable name.
 */
static struct config_generic *
add_placeholder_variable(const char *name, int elevel)
{
	size_t		sz = sizeof(struct config_string) + sizeof(char *);
	struct config_string *var;
	struct config_generic *gen;

	var = (struct config_string *) guc_malloc(elevel, sz);
	if (var == NULL)
		return NULL;
	memset(var, 0, sz);
	gen = &var->gen;

	gen->name = guc_strdup(elevel, name);
	if (gen->name == NULL)
	{
		guc_free(var);
		return NULL;
	}

	gen->context = PGC_USERSET;
	gen->group = CUSTOM_OPTIONS;
	gen->short_desc = "GUC placeholder variable";
	gen->flags = GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_CUSTOM_PLACEHOLDER;
	gen->vartype = PGC_STRING;

	/*
	 * The char* is allocated at the end of the struct since we have no
	 * 'static' place to point to.  Note that the current value, as well as
	 * the boot and reset values, start out NULL.
	 */
	var->variable = (char **) (var + 1);

	if (!add_guc_variable((struct config_generic *) var, elevel))
	{
		guc_free(unconstify(char *, gen->name));
		guc_free(var);
		return NULL;
	}

	return gen;
}

/*
 * Look up option "name".  If it exists, return a pointer to its record.
 * Otherwise, if create_placeholders is true and name is a valid-looking
 * custom variable name, we'll create and return a placeholder record.
 * Otherwise, if skip_errors is true, then we silently return NULL for
 * an unrecognized or invalid name.  Otherwise, the error is reported at
 * error level elevel (and we return NULL if that's less than ERROR).
 *
 * Note: internal errors, primarily out-of-memory, draw an elevel-level
 * report and NULL return regardless of skip_errors.  Hence, callers must
 * handle a NULL return whenever elevel < ERROR, but they should not need
 * to emit any additional error message.  (In practice, internal errors
 * can only happen when create_placeholders is true, so callers passing
 * false need not think terribly hard about this.)
 */
struct config_generic *
find_option(const char *name, bool create_placeholders, bool skip_errors,
			int elevel)
{
	GUCHashEntry *hentry;
	int			i;

	Assert(name);

	/* Look it up using the hash table. */
	hentry = (GUCHashEntry *) hash_search(guc_hashtab,
										  &name,
										  HASH_FIND,
										  NULL);
	if (hentry)
		return hentry->gucvar;

	/*
	 * See if the name is an obsolete name for a variable.  We assume that the
	 * set of supported old names is short enough that a brute-force search is
	 * the best way.
	 */
	for (i = 0; map_old_guc_names[i] != NULL; i += 2)
	{
		if (guc_name_compare(name, map_old_guc_names[i]) == 0)
			return find_option(map_old_guc_names[i + 1], false,
							   skip_errors, elevel);
	}

	if (create_placeholders)
	{
		/*
		 * Check if the name is valid, and if so, add a placeholder.
		 */
		if (assignable_custom_variable_name(name, skip_errors, elevel))
			return add_placeholder_variable(name, elevel);
		else
			return NULL;		/* error message, if any, already emitted */
	}

	/* Unknown name and we're not supposed to make a placeholder */
	if (!skip_errors)
		ereport(elevel,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"",
						name)));
	return NULL;
}


/*
 * comparator for qsorting an array of GUC pointers
 */
static int
guc_var_compare(const void *a, const void *b)
{
	const char *namea = **(const char **const *) a;
	const char *nameb = **(const char **const *) b;

	return guc_name_compare(namea, nameb);
}

/*
 * the bare comparison function for GUC names
 */
int
guc_name_compare(const char *namea, const char *nameb)
{
	/*
	 * The temptation to use strcasecmp() here must be resisted, because the
	 * hash mapping has to remain stable across setlocale() calls. So, build
	 * our own with a simple ASCII-only downcasing.
	 */
	while (*namea && *nameb)
	{
		char		cha = *namea++;
		char		chb = *nameb++;

		if (cha >= 'A' && cha <= 'Z')
			cha += 'a' - 'A';
		if (chb >= 'A' && chb <= 'Z')
			chb += 'a' - 'A';
		if (cha != chb)
			return cha - chb;
	}
	if (*namea)
		return 1;				/* a is longer */
	if (*nameb)
		return -1;				/* b is longer */
	return 0;
}

/*
 * Hash function that's compatible with guc_name_compare
 */
static uint32
guc_name_hash(const void *key, Size keysize)
{
	uint32		result = 0;
	const char *name = *(const char *const *) key;

	while (*name)
	{
		char		ch = *name++;

		/* Case-fold in the same way as guc_name_compare */
		if (ch >= 'A' && ch <= 'Z')
			ch += 'a' - 'A';

		/* Merge into hash ... not very bright, but it needn't be */
		result = pg_rotate_left32(result, 5);
		result ^= (uint32) ch;
	}
	return result;
}

/*
 * Dynahash match function to use in guc_hashtab
 */
static int
guc_name_match(const void *key1, const void *key2, Size keysize)
{
	const char *name1 = *(const char *const *) key1;
	const char *name2 = *(const char *const *) key2;

	return guc_name_compare(name1, name2);
}


/*
 * Convert a GUC name to the form that should be used in pg_parameter_acl.
 *
 * We need to canonicalize entries since, for example, case should not be
 * significant.  In addition, we apply the map_old_guc_names[] mapping so that
 * any obsolete names will be converted when stored in a new PG version.
 * Note however that this function does not verify legality of the name.
 *
 * The result is a palloc'd string.
 */
char *
convert_GUC_name_for_parameter_acl(const char *name)
{
	char	   *result;

	/* Apply old-GUC-name mapping. */
	for (int i = 0; map_old_guc_names[i] != NULL; i += 2)
	{
		if (guc_name_compare(name, map_old_guc_names[i]) == 0)
		{
			name = map_old_guc_names[i + 1];
			break;
		}
	}

	/* Apply case-folding that matches guc_name_compare(). */
	result = pstrdup(name);
	for (char *ptr = result; *ptr != '\0'; ptr++)
	{
		char		ch = *ptr;

		if (ch >= 'A' && ch <= 'Z')
		{
			ch += 'a' - 'A';
			*ptr = ch;
		}
	}

	return result;
}

/*
 * Check whether we should allow creation of a pg_parameter_acl entry
 * for the given name.  (This can be applied either before or after
 * canonicalizing it.)  Throws error if not.
 */
void
check_GUC_name_for_parameter_acl(const char *name)
{
	/* OK if the GUC exists. */
	if (find_option(name, false, true, DEBUG5) != NULL)
		return;
	/* Otherwise, it'd better be a valid custom GUC name. */
	(void) assignable_custom_variable_name(name, false, ERROR);
}

/*
 * Routine in charge of checking various states of a GUC.
 *
 * This performs two sanity checks.  First, it checks that the initial
 * value of a GUC is the same when declared and when loaded to prevent
 * anybody looking at the C declarations of these GUCs from being fooled by
 * mismatched values.  Second, it checks for incorrect flag combinations.
 *
 * The following validation rules apply for the values:
 * bool - can be false, otherwise must be same as the boot_val
 * int  - can be 0, otherwise must be same as the boot_val
 * real - can be 0.0, otherwise must be same as the boot_val
 * string - can be NULL, otherwise must be strcmp equal to the boot_val
 * enum - must be same as the boot_val
 */
#ifdef USE_ASSERT_CHECKING
static bool
check_GUC_init(struct config_generic *gconf)
{
	/* Checks on values */
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;

				if (*conf->variable && !conf->boot_val)
				{
					elog(LOG, "GUC (PGC_BOOL) %s, boot_val=%d, C-var=%d",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				if (*conf->variable != 0 && *conf->variable != conf->boot_val)
				{
					elog(LOG, "GUC (PGC_INT) %s, boot_val=%d, C-var=%d",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;

				if (*conf->variable != 0.0 && *conf->variable != conf->boot_val)
				{
					elog(LOG, "GUC (PGC_REAL) %s, boot_val=%g, C-var=%g",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				if (*conf->variable != NULL &&
					(conf->boot_val == NULL ||
					 strcmp(*conf->variable, conf->boot_val) != 0))
				{
					elog(LOG, "GUC (PGC_STRING) %s, boot_val=%s, C-var=%s",
						 conf->gen.name, conf->boot_val ? conf->boot_val : "<null>", *conf->variable);
					return false;
				}
				break;
			}
		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				if (*conf->variable != conf->boot_val)
				{
					elog(LOG, "GUC (PGC_ENUM) %s, boot_val=%d, C-var=%d",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
	}

	/* Flag combinations */

	/*
	 * GUC_NO_SHOW_ALL requires GUC_NOT_IN_SAMPLE, as a parameter not part of
	 * SHOW ALL should not be hidden in postgresql.conf.sample.
	 */
	if ((gconf->flags & GUC_NO_SHOW_ALL) &&
		!(gconf->flags & GUC_NOT_IN_SAMPLE))
	{
		elog(LOG, "GUC %s flags: NO_SHOW_ALL and !NOT_IN_SAMPLE",
			 gconf->name);
		return false;
	}

	return true;
}
#endif

/*
 * Initialize GUC options during program startup.
 *
 * Note that we cannot read the config file yet, since we have not yet
 * processed command-line switches.
 */
void
InitializeGUCOptions(void)
{
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;

	/*
	 * Before log_line_prefix could possibly receive a nonempty setting, make
	 * sure that timezone processing is minimally alive (see elog.c).
	 */
	pg_timezone_initialize();

	/*
	 * Create GUCMemoryContext and build hash table of all GUC variables.
	 */
	build_guc_variables();

	/*
	 * Load all variables with their compiled-in defaults, and initialize
	 * status fields as needed.
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		/* Check mapping between initial and default value */
		Assert(check_GUC_init(hentry->gucvar));

		InitializeOneGUCOption(hentry->gucvar);
	}

	reporting_enabled = false;

	/*
	 * Prevent any attempt to override the transaction modes from
	 * non-interactive sources.
	 */
	SetConfigOption("transaction_isolation", "read committed",
					PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("transaction_read_only", "no",
					PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("transaction_deferrable", "no",
					PGC_POSTMASTER, PGC_S_OVERRIDE);

	/*
	 * For historical reasons, some GUC parameters can receive defaults from
	 * environment variables.  Process those settings.
	 */
	InitializeGUCOptionsFromEnvironment();
}

/*
 * Assign any GUC values that can come from the server's environment.
 *
 * This is called from InitializeGUCOptions, and also from ProcessConfigFile
 * to deal with the possibility that a setting has been removed from
 * postgresql.conf and should now get a value from the environment.
 * (The latter is a kludge that should probably go away someday; if so,
 * fold this back into InitializeGUCOptions.)
 */
static void
InitializeGUCOptionsFromEnvironment(void)
{
	char	   *env;
	long		stack_rlimit;

	env = getenv("PGPORT");
	if (env != NULL)
		SetConfigOption("port", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGDATESTYLE");
	if (env != NULL)
		SetConfigOption("datestyle", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGCLIENTENCODING");
	if (env != NULL)
		SetConfigOption("client_encoding", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	/*
	 * rlimit isn't exactly an "environment variable", but it behaves about
	 * the same.  If we can identify the platform stack depth rlimit, increase
	 * default stack depth setting up to whatever is safe (but at most 2MB).
	 * Report the value's source as PGC_S_DYNAMIC_DEFAULT if it's 2MB, or as
	 * PGC_S_ENV_VAR if it's reflecting the rlimit limit.
	 */
	stack_rlimit = get_stack_depth_rlimit();
	if (stack_rlimit > 0)
	{
		long		new_limit = (stack_rlimit - STACK_DEPTH_SLOP) / 1024L;

		if (new_limit > 100)
		{
			GucSource	source;
			char		limbuf[16];

			if (new_limit < 2048)
				source = PGC_S_ENV_VAR;
			else
			{
				new_limit = 2048;
				source = PGC_S_DYNAMIC_DEFAULT;
			}
			snprintf(limbuf, sizeof(limbuf), "%ld", new_limit);
			SetConfigOption("max_stack_depth", limbuf,
							PGC_POSTMASTER, source);
		}
	}
}

/*
 * Initialize one GUC option variable to its compiled-in default.
 *
 * Note: the reason for calling check_hooks is not that we think the boot_val
 * might fail, but that the hooks might wish to compute an "extra" struct.
 */
static void
InitializeOneGUCOption(struct config_generic *gconf)
{
	gconf->status = 0;
	gconf->source = PGC_S_DEFAULT;
	gconf->reset_source = PGC_S_DEFAULT;
	gconf->scontext = PGC_INTERNAL;
	gconf->reset_scontext = PGC_INTERNAL;
	gconf->srole = BOOTSTRAP_SUPERUSERID;
	gconf->reset_srole = BOOTSTRAP_SUPERUSERID;
	gconf->stack = NULL;
	gconf->extra = NULL;
	gconf->last_reported = NULL;
	gconf->sourcefile = NULL;
	gconf->sourceline = 0;

	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;
				bool		newval = conf->boot_val;
				void	   *extra = NULL;

				if (!call_bool_check_hook(conf, &newval, &extra,
										  PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, (int) newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;
				int			newval = conf->boot_val;
				void	   *extra = NULL;

				Assert(newval >= conf->min);
				Assert(newval <= conf->max);
				if (!call_int_check_hook(conf, &newval, &extra,
										 PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;
				double		newval = conf->boot_val;
				void	   *extra = NULL;

				Assert(newval >= conf->min);
				Assert(newval <= conf->max);
				if (!call_real_check_hook(conf, &newval, &extra,
										  PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %g",
						 conf->gen.name, newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;
				char	   *newval;
				void	   *extra = NULL;

				/* non-NULL boot_val must always get strdup'd */
				if (conf->boot_val != NULL)
					newval = guc_strdup(FATAL, conf->boot_val);
				else
					newval = NULL;

				if (!call_string_check_hook(conf, &newval, &extra,
											PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to \"%s\"",
						 conf->gen.name, newval ? newval : "");
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;
				int			newval = conf->boot_val;
				void	   *extra = NULL;

				if (!call_enum_check_hook(conf, &newval, &extra,
										  PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
	}
}

/*
 * Summarily remove a GUC variable from any linked lists it's in.
 *
 * We use this in cases where the variable is about to be deleted or reset.
 * These aren't common operations, so it's okay if this is a bit slow.
 */
static void
RemoveGUCFromLists(struct config_generic *gconf)
{
	if (gconf->source != PGC_S_DEFAULT)
		dlist_delete(&gconf->nondef_link);
	if (gconf->stack != NULL)
		slist_delete(&guc_stack_list, &gconf->stack_link);
	if (gconf->status & GUC_NEEDS_REPORT)
		slist_delete(&guc_report_list, &gconf->report_link);
}


/*
 * Select the configuration files and data directory to be used, and
 * do the initial read of postgresql.conf.
 *
 * This is called after processing command-line switches.
 *		userDoption is the -D switch value if any (NULL if unspecified).
 *		progname is just for use in error messages.
 *
 * Returns true on success; on failure, prints a suitable error message
 * to stderr and returns false.
 */
bool
SelectConfigFiles(const char *userDoption, const char *progname)
{
	char	   *configdir;
	char	   *fname;
	bool		fname_is_malloced;
	struct stat stat_buf;
	struct config_string *data_directory_rec;

	/* configdir is -D option, or $PGDATA if no -D */
	if (userDoption)
		configdir = make_absolute_path(userDoption);
	else
		configdir = make_absolute_path(getenv("PGDATA"));

	if (configdir && stat(configdir, &stat_buf) != 0)
	{
		write_stderr("%s: could not access directory \"%s\": %m\n",
					 progname,
					 configdir);
		if (errno == ENOENT)
			write_stderr("Run initdb or pg_basebackup to initialize a PostgreSQL data directory.\n");
		return false;
	}

	/*
	 * Find the configuration file: if config_file was specified on the
	 * command line, use it, else use configdir/postgresql.conf.  In any case
	 * ensure the result is an absolute path, so that it will be interpreted
	 * the same way by future backends.
	 */
	if (ConfigFileName)
	{
		fname = make_absolute_path(ConfigFileName);
		fname_is_malloced = true;
	}
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(CONFIG_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, CONFIG_FILENAME);
		fname_is_malloced = false;
	}
	else
	{
		write_stderr("%s does not know where to find the server configuration file.\n"
					 "You must specify the --config-file or -D invocation "
					 "option or set the PGDATA environment variable.\n",
					 progname);
		return false;
	}

	/*
	 * Set the ConfigFileName GUC variable to its final value, ensuring that
	 * it can't be overridden later.
	 */
	SetConfigOption("config_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);

	if (fname_is_malloced)
		free(fname);
	else
		guc_free(fname);

	/*
	 * Now read the config file for the first time.
	 */
	if (stat(ConfigFileName, &stat_buf) != 0)
	{
		write_stderr("%s: could not access the server configuration file \"%s\": %m\n",
					 progname, ConfigFileName);
		free(configdir);
		return false;
	}

	/*
	 * Read the configuration file for the first time.  This time only the
	 * data_directory parameter is picked up to determine the data directory,
	 * so that we can read the PG_AUTOCONF_FILENAME file next time.
	 */
	ProcessConfigFile(PGC_POSTMASTER);

	/*
	 * If the data_directory GUC variable has been set, use that as DataDir;
	 * otherwise use configdir if set; else punt.
	 *
	 * Note: SetDataDir will copy and absolute-ize its argument, so we don't
	 * have to.
	 */
	data_directory_rec = (struct config_string *)
		find_option("data_directory", false, false, PANIC);
	if (*data_directory_rec->variable)
		SetDataDir(*data_directory_rec->variable);
	else if (configdir)
		SetDataDir(configdir);
	else
	{
		write_stderr("%s does not know where to find the database system data.\n"
					 "This can be specified as \"data_directory\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}

	/*
	 * Reflect the final DataDir value back into the data_directory GUC var.
	 * (If you are wondering why we don't just make them a single variable,
	 * it's because the EXEC_BACKEND case needs DataDir to be transmitted to
	 * child backends specially.  XXX is that still true?  Given that we now
	 * chdir to DataDir, EXEC_BACKEND can read the config file without knowing
	 * DataDir in advance.)
	 */
	SetConfigOption("data_directory", DataDir, PGC_POSTMASTER, PGC_S_OVERRIDE);

	/*
	 * Now read the config file a second time, allowing any settings in the
	 * PG_AUTOCONF_FILENAME file to take effect.  (This is pretty ugly, but
	 * since we have to determine the DataDir before we can find the autoconf
	 * file, the alternatives seem worse.)
	 */
	ProcessConfigFile(PGC_POSTMASTER);

	/*
	 * If timezone_abbreviations wasn't set in the configuration file, install
	 * the default value.  We do it this way because we can't safely install a
	 * "real" value until my_exec_path is set, which may not have happened
	 * when InitializeGUCOptions runs, so the bootstrap default value cannot
	 * be the real desired default.
	 */
	pg_timezone_abbrev_initialize();

	/*
	 * Figure out where pg_hba.conf is, and make sure the path is absolute.
	 */
	if (HbaFileName)
	{
		fname = make_absolute_path(HbaFileName);
		fname_is_malloced = true;
	}
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(HBA_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, HBA_FILENAME);
		fname_is_malloced = false;
	}
	else
	{
		write_stderr("%s does not know where to find the \"hba\" configuration file.\n"
					 "This can be specified as \"hba_file\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}
	SetConfigOption("hba_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);

	if (fname_is_malloced)
		free(fname);
	else
		guc_free(fname);

	/*
	 * Likewise for pg_ident.conf.
	 */
	if (IdentFileName)
	{
		fname = make_absolute_path(IdentFileName);
		fname_is_malloced = true;
	}
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(IDENT_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, IDENT_FILENAME);
		fname_is_malloced = false;
	}
	else
	{
		write_stderr("%s does not know where to find the \"ident\" configuration file.\n"
					 "This can be specified as \"ident_file\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}
	SetConfigOption("ident_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);

	if (fname_is_malloced)
		free(fname);
	else
		guc_free(fname);

	free(configdir);

	return true;
}

/*
 * pg_timezone_abbrev_initialize --- set default value if not done already
 *
 * This is called after initial loading of postgresql.conf.  If no
 * timezone_abbreviations setting was found therein, select default.
 * If a non-default value is already installed, nothing will happen.
 *
 * This can also be called from ProcessConfigFile to establish the default
 * value after a postgresql.conf entry for it is removed.
 */
static void
pg_timezone_abbrev_initialize(void)
{
	SetConfigOption("timezone_abbreviations", "Default",
					PGC_POSTMASTER, PGC_S_DYNAMIC_DEFAULT);
}


/*
 * Reset all options to their saved default values (implements RESET ALL)
 */
void
ResetAllOptions(void)
{
	dlist_mutable_iter iter;

	/* We need only consider GUCs not already at PGC_S_DEFAULT */
	dlist_foreach_modify(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		/* Don't reset non-SET-able values */
		if (gconf->context != PGC_SUSET &&
			gconf->context != PGC_USERSET)
			continue;
		/* Don't reset if special exclusion from RESET ALL */
		if (gconf->flags & GUC_NO_RESET_ALL)
			continue;
		/* No need to reset if wasn't SET */
		if (gconf->source <= PGC_S_OVERRIDE)
			continue;

		/* Save old value to support transaction abort */
		push_old_value(gconf, GUC_ACTION_SET);

		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					set_string_field(conf, conf->variable, conf->reset_val);
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_ENUM:
				{
					struct config_enum *conf = (struct config_enum *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
		}

		set_guc_source(gconf, gconf->reset_source);
		gconf->scontext = gconf->reset_scontext;
		gconf->srole = gconf->reset_srole;

		if ((gconf->flags & GUC_REPORT) && !(gconf->status & GUC_NEEDS_REPORT))
		{
			gconf->status |= GUC_NEEDS_REPORT;
			slist_push_head(&guc_report_list, &gconf->report_link);
		}
	}
}


/*
 * Apply a change to a GUC variable's "source" field.
 *
 * Use this rather than just assigning, to ensure that the variable's
 * membership in guc_nondef_list is updated correctly.
 */
static void
set_guc_source(struct config_generic *gconf, GucSource newsource)
{
	/* Adjust nondef list membership if appropriate for change */
	if (gconf->source == PGC_S_DEFAULT)
	{
		if (newsource != PGC_S_DEFAULT)
			dlist_push_tail(&guc_nondef_list, &gconf->nondef_link);
	}
	else
	{
		if (newsource == PGC_S_DEFAULT)
			dlist_delete(&gconf->nondef_link);
	}
	/* Now update the source field */
	gconf->source = newsource;
}


/*
 * push_old_value
 *		Push previous state during transactional assignment to a GUC variable.
 */
static void
push_old_value(struct config_generic *gconf, GucAction action)
{
	GucStack   *stack;

	/* If we're not inside a nest level, do nothing */
	if (GUCNestLevel == 0)
		return;

	/* Do we already have a stack entry of the current nest level? */
	stack = gconf->stack;
	if (stack && stack->nest_level >= GUCNestLevel)
	{
		/* Yes, so adjust its state if necessary */
		Assert(stack->nest_level == GUCNestLevel);
		switch (action)
		{
			case GUC_ACTION_SET:
				/* SET overrides any prior action at same nest level */
				if (stack->state == GUC_SET_LOCAL)
				{
					/* must discard old masked value */
					discard_stack_value(gconf, &stack->masked);
				}
				stack->state = GUC_SET;
				break;
			case GUC_ACTION_LOCAL:
				if (stack->state == GUC_SET)
				{
					/* SET followed by SET LOCAL, remember SET's value */
					stack->masked_scontext = gconf->scontext;
					stack->masked_srole = gconf->srole;
					set_stack_value(gconf, &stack->masked);
					stack->state = GUC_SET_LOCAL;
				}
				/* in all other cases, no change to stack entry */
				break;
			case GUC_ACTION_SAVE:
				/* Could only have a prior SAVE of same variable */
				Assert(stack->state == GUC_SAVE);
				break;
		}
		return;
	}

	/*
	 * Push a new stack entry
	 *
	 * We keep all the stack entries in TopTransactionContext for simplicity.
	 */
	stack = (GucStack *) MemoryContextAllocZero(TopTransactionContext,
												sizeof(GucStack));

	stack->prev = gconf->stack;
	stack->nest_level = GUCNestLevel;
	switch (action)
	{
		case GUC_ACTION_SET:
			stack->state = GUC_SET;
			break;
		case GUC_ACTION_LOCAL:
			stack->state = GUC_LOCAL;
			break;
		case GUC_ACTION_SAVE:
			stack->state = GUC_SAVE;
			break;
	}
	stack->source = gconf->source;
	stack->scontext = gconf->scontext;
	stack->srole = gconf->srole;
	set_stack_value(gconf, &stack->prior);

	if (gconf->stack == NULL)
		slist_push_head(&guc_stack_list, &gconf->stack_link);
	gconf->stack = stack;
}


/*
 * Do GUC processing at main transaction start.
 */
void
AtStart_GUC(void)
{
	/*
	 * The nest level should be 0 between transactions; if it isn't, somebody
	 * didn't call AtEOXact_GUC, or called it with the wrong nestLevel.  We
	 * throw a warning but make no other effort to clean up.
	 */
	if (GUCNestLevel != 0)
		elog(WARNING, "GUC nest level = %d at transaction start",
			 GUCNestLevel);
	GUCNestLevel = 1;
}

/*
 * Enter a new nesting level for GUC values.  This is called at subtransaction
 * start, and when entering a function that has proconfig settings, and in
 * some other places where we want to set GUC variables transiently.
 * NOTE we must not risk error here, else subtransaction start will be unhappy.
 */
int
NewGUCNestLevel(void)
{
	return ++GUCNestLevel;
}

/*
 * Set search_path to a fixed value for maintenance operations. No effect
 * during bootstrap, when the search_path is already set to a fixed value and
 * cannot be changed.
 */
void
RestrictSearchPath(void)
{
	if (!IsBootstrapProcessingMode())
		set_config_option("search_path", GUC_SAFE_SEARCH_PATH, PGC_USERSET,
						  PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
}

/*
 * Do GUC processing at transaction or subtransaction commit or abort, or
 * when exiting a function that has proconfig settings, or when undoing a
 * transient assignment to some GUC variables.  (The name is thus a bit of
 * a misnomer; perhaps it should be ExitGUCNestLevel or some such.)
 * During abort, we discard all GUC settings that were applied at nesting
 * levels >= nestLevel.  nestLevel == 1 corresponds to the main transaction.
 */
void
AtEOXact_GUC(bool isCommit, int nestLevel)
{
	slist_mutable_iter iter;

	/*
	 * Note: it's possible to get here with GUCNestLevel == nestLevel-1 during
	 * abort, if there is a failure during transaction start before
	 * AtStart_GUC is called.
	 */
	Assert(nestLevel > 0 &&
		   (nestLevel <= GUCNestLevel ||
			(nestLevel == GUCNestLevel + 1 && !isCommit)));

	/* We need only process GUCs having nonempty stacks */
	slist_foreach_modify(iter, &guc_stack_list)
	{
		struct config_generic *gconf = slist_container(struct config_generic,
													   stack_link, iter.cur);
		GucStack   *stack;

		/*
		 * Process and pop each stack entry within the nest level. To simplify
		 * fmgr_security_definer() and other places that use GUC_ACTION_SAVE,
		 * we allow failure exit from code that uses a local nest level to be
		 * recovered at the surrounding transaction or subtransaction abort;
		 * so there could be more than one stack entry to pop.
		 */
		while ((stack = gconf->stack) != NULL &&
			   stack->nest_level >= nestLevel)
		{
			GucStack   *prev = stack->prev;
			bool		restorePrior = false;
			bool		restoreMasked = false;
			bool		changed;

			/*
			 * In this next bit, if we don't set either restorePrior or
			 * restoreMasked, we must "discard" any unwanted fields of the
			 * stack entries to avoid leaking memory.  If we do set one of
			 * those flags, unused fields will be cleaned up after restoring.
			 */
			if (!isCommit)		/* if abort, always restore prior value */
				restorePrior = true;
			else if (stack->state == GUC_SAVE)
				restorePrior = true;
			else if (stack->nest_level == 1)
			{
				/* transaction commit */
				if (stack->state == GUC_SET_LOCAL)
					restoreMasked = true;
				else if (stack->state == GUC_SET)
				{
					/* we keep the current active value */
					discard_stack_value(gconf, &stack->prior);
				}
				else			/* must be GUC_LOCAL */
					restorePrior = true;
			}
			else if (prev == NULL ||
					 prev->nest_level < stack->nest_level - 1)
			{
				/* decrement entry's level and do not pop it */
				stack->nest_level--;
				continue;
			}
			else
			{
				/*
				 * We have to merge this stack entry into prev. See README for
				 * discussion of this bit.
				 */
				switch (stack->state)
				{
					case GUC_SAVE:
						Assert(false);	/* can't get here */
						break;

					case GUC_SET:
						/* next level always becomes SET */
						discard_stack_value(gconf, &stack->prior);
						if (prev->state == GUC_SET_LOCAL)
							discard_stack_value(gconf, &prev->masked);
						prev->state = GUC_SET;
						break;

					case GUC_LOCAL:
						if (prev->state == GUC_SET)
						{
							/* LOCAL migrates down */
							prev->masked_scontext = stack->scontext;
							prev->masked_srole = stack->srole;
							prev->masked = stack->prior;
							prev->state = GUC_SET_LOCAL;
						}
						else
						{
							/* else just forget this stack level */
							discard_stack_value(gconf, &stack->prior);
						}
						break;

					case GUC_SET_LOCAL:
						/* prior state at this level no longer wanted */
						discard_stack_value(gconf, &stack->prior);
						/* copy down the masked state */
						prev->masked_scontext = stack->masked_scontext;
						prev->masked_srole = stack->masked_srole;
						if (prev->state == GUC_SET_LOCAL)
							discard_stack_value(gconf, &prev->masked);
						prev->masked = stack->masked;
						prev->state = GUC_SET_LOCAL;
						break;
				}
			}

			changed = false;

			if (restorePrior || restoreMasked)
			{
				/* Perform appropriate restoration of the stacked value */
				config_var_value newvalue;
				GucSource	newsource;
				GucContext	newscontext;
				Oid			newsrole;

				if (restoreMasked)
				{
					newvalue = stack->masked;
					newsource = PGC_S_SESSION;
					newscontext = stack->masked_scontext;
					newsrole = stack->masked_srole;
				}
				else
				{
					newvalue = stack->prior;
					newsource = stack->source;
					newscontext = stack->scontext;
					newsrole = stack->srole;
				}

				switch (gconf->vartype)
				{
					case PGC_BOOL:
						{
							struct config_bool *conf = (struct config_bool *) gconf;
							bool		newval = newvalue.val.boolval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
					case PGC_INT:
						{
							struct config_int *conf = (struct config_int *) gconf;
							int			newval = newvalue.val.intval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
					case PGC_REAL:
						{
							struct config_real *conf = (struct config_real *) gconf;
							double		newval = newvalue.val.realval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
					case PGC_STRING:
						{
							struct config_string *conf = (struct config_string *) gconf;
							char	   *newval = newvalue.val.stringval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								set_string_field(conf, conf->variable, newval);
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}

							/*
							 * Release stacked values if not used anymore. We
							 * could use discard_stack_value() here, but since
							 * we have type-specific code anyway, might as
							 * well inline it.
							 */
							set_string_field(conf, &stack->prior.val.stringval, NULL);
							set_string_field(conf, &stack->masked.val.stringval, NULL);
							break;
						}
					case PGC_ENUM:
						{
							struct config_enum *conf = (struct config_enum *) gconf;
							int			newval = newvalue.val.enumval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
				}

				/*
				 * Release stacked extra values if not used anymore.
				 */
				set_extra_field(gconf, &(stack->prior.extra), NULL);
				set_extra_field(gconf, &(stack->masked.extra), NULL);

				/* And restore source information */
				set_guc_source(gconf, newsource);
				gconf->scontext = newscontext;
				gconf->srole = newsrole;
			}

			/*
			 * Pop the GUC's state stack; if it's now empty, remove the GUC
			 * from guc_stack_list.
			 */
			gconf->stack = prev;
			if (prev == NULL)
				slist_delete_current(&iter);
			pfree(stack);

			/* Report new value if we changed it */
			if (changed && (gconf->flags & GUC_REPORT) &&
				!(gconf->status & GUC_NEEDS_REPORT))
			{
				gconf->status |= GUC_NEEDS_REPORT;
				slist_push_head(&guc_report_list, &gconf->report_link);
			}
		}						/* end of stack-popping loop */
	}

	/* Update nesting level */
	GUCNestLevel = nestLevel - 1;
}


/*
 * Start up automatic reporting of changes to variables marked GUC_REPORT.
 * This is executed at completion of backend startup.
 */
void
BeginReportingGUCOptions(void)
{
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;

	/*
	 * Don't do anything unless talking to an interactive frontend.
	 */
	if (whereToSendOutput != DestRemote)
		return;

	reporting_enabled = true;

	/*
	 * Hack for in_hot_standby: set the GUC value true if appropriate.  This
	 * is kind of an ugly place to do it, but there's few better options.
	 *
	 * (This could be out of date by the time we actually send it, in which
	 * case the next ReportChangedGUCOptions call will send a duplicate
	 * report.)
	 */
	if (RecoveryInProgress())
		SetConfigOption("in_hot_standby", "true",
						PGC_INTERNAL, PGC_S_OVERRIDE);

	/* Transmit initial values of interesting variables */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *conf = hentry->gucvar;

		if (conf->flags & GUC_REPORT)
			ReportGUCOption(conf);
	}
}

/*
 * ReportChangedGUCOptions: report recently-changed GUC_REPORT variables
 *
 * This is called just before we wait for a new client query.
 *
 * By handling things this way, we ensure that a ParameterStatus message
 * is sent at most once per variable per query, even if the variable
 * changed multiple times within the query.  That's quite possible when
 * using features such as function SET clauses.  Function SET clauses
 * also tend to cause values to change intraquery but eventually revert
 * to their prevailing values; ReportGUCOption is responsible for avoiding
 * redundant reports in such cases.
 */
void
ReportChangedGUCOptions(void)
{
	slist_mutable_iter iter;

	/* Quick exit if not (yet) enabled */
	if (!reporting_enabled)
		return;

	/*
	 * Since in_hot_standby isn't actually changed by normal GUC actions, we
	 * need a hack to check whether a new value needs to be reported to the
	 * client.  For speed, we rely on the assumption that it can never
	 * transition from false to true.
	 */
	if (in_hot_standby_guc && !RecoveryInProgress())
		SetConfigOption("in_hot_standby", "false",
						PGC_INTERNAL, PGC_S_OVERRIDE);

	/* Transmit new values of interesting variables */
	slist_foreach_modify(iter, &guc_report_list)
	{
		struct config_generic *conf = slist_container(struct config_generic,
													  report_link, iter.cur);

		Assert((conf->flags & GUC_REPORT) && (conf->status & GUC_NEEDS_REPORT));
		ReportGUCOption(conf);
		conf->status &= ~GUC_NEEDS_REPORT;
		slist_delete_current(&iter);
	}
}

/*
 * ReportGUCOption: if appropriate, transmit option value to frontend
 *
 * We need not transmit the value if it's the same as what we last
 * transmitted.
 */
static void
ReportGUCOption(struct config_generic *record)
{
	char	   *val = ShowGUCOption(record, false);

	if (record->last_reported == NULL ||
		strcmp(val, record->last_reported) != 0)
	{
		StringInfoData msgbuf;

		pq_beginmessage(&msgbuf, PqMsg_ParameterStatus);
		pq_sendstring(&msgbuf, record->name);
		pq_sendstring(&msgbuf, val);
		pq_endmessage(&msgbuf);

		/*
		 * We need a long-lifespan copy.  If guc_strdup() fails due to OOM,
		 * we'll set last_reported to NULL and thereby possibly make a
		 * duplicate report later.
		 */
		guc_free(record->last_reported);
		record->last_reported = guc_strdup(LOG, val);
	}

	pfree(val);
}

/*
 * Convert a value from one of the human-friendly units ("kB", "min" etc.)
 * to the given base unit.  'value' and 'unit' are the input value and unit
 * to convert from (there can be trailing spaces in the unit string).
 * The converted value is stored in *base_value.
 * It's caller's responsibility to round off the converted value as necessary
 * and check for out-of-range.
 *
 * Returns true on success, false if the input unit is not recognized.
 */
static bool
convert_to_base_unit(double value, const char *unit,
					 int base_unit, double *base_value)
{
	char		unitstr[MAX_UNIT_LEN + 1];
	int			unitlen;
	const unit_conversion *table;
	int			i;

	/* extract unit string to compare to table entries */
	unitlen = 0;
	while (*unit != '\0' && !isspace((unsigned char) *unit) &&
		   unitlen < MAX_UNIT_LEN)
		unitstr[unitlen++] = *(unit++);
	unitstr[unitlen] = '\0';
	/* allow whitespace after unit */
	while (isspace((unsigned char) *unit))
		unit++;
	if (*unit != '\0')
		return false;			/* unit too long, or garbage after it */

	/* now search the appropriate table */
	if (base_unit & GUC_UNIT_MEMORY)
		table = memory_unit_conversion_table;
	else
		table = time_unit_conversion_table;

	for (i = 0; *table[i].unit; i++)
	{
		if (base_unit == table[i].base_unit &&
			strcmp(unitstr, table[i].unit) == 0)
		{
			double		cvalue = value * table[i].multiplier;

			/*
			 * If the user gave a fractional value such as "30.1GB", round it
			 * off to the nearest multiple of the next smaller unit, if there
			 * is one.
			 */
			if (*table[i + 1].unit &&
				base_unit == table[i + 1].base_unit)
				cvalue = rint(cvalue / table[i + 1].multiplier) *
					table[i + 1].multiplier;

			*base_value = cvalue;
			return true;
		}
	}
	return false;
}

/*
 * Convert an integer value in some base unit to a human-friendly unit.
 *
 * The output unit is chosen so that it's the greatest unit that can represent
 * the value without loss.  For example, if the base unit is GUC_UNIT_KB, 1024
 * is converted to 1 MB, but 1025 is represented as 1025 kB.
 */
static void
convert_int_from_base_unit(int64 base_value, int base_unit,
						   int64 *value, const char **unit)
{
	const unit_conversion *table;
	int			i;

	*unit = NULL;

	if (base_unit & GUC_UNIT_MEMORY)
		table = memory_unit_conversion_table;
	else
		table = time_unit_conversion_table;

	for (i = 0; *table[i].unit; i++)
	{
		if (base_unit == table[i].base_unit)
		{
			/*
			 * Accept the first conversion that divides the value evenly.  We
			 * assume that the conversions for each base unit are ordered from
			 * greatest unit to the smallest!
			 */
			if (table[i].multiplier <= 1.0 ||
				base_value % (int64) table[i].multiplier == 0)
			{
				*value = (int64) rint(base_value / table[i].multiplier);
				*unit = table[i].unit;
				break;
			}
		}
	}

	Assert(*unit != NULL);
}

/*
 * Convert a floating-point value in some base unit to a human-friendly unit.
 *
 * Same as above, except we have to do the math a bit differently, and
 * there's a possibility that we don't find any exact divisor.
 */
static void
convert_real_from_base_unit(double base_value, int base_unit,
							double *value, const char **unit)
{
	const unit_conversion *table;
	int			i;

	*unit = NULL;

	if (base_unit & GUC_UNIT_MEMORY)
		table = memory_unit_conversion_table;
	else
		table = time_unit_conversion_table;

	for (i = 0; *table[i].unit; i++)
	{
		if (base_unit == table[i].base_unit)
		{
			/*
			 * Accept the first conversion that divides the value evenly; or
			 * if there is none, use the smallest (last) target unit.
			 *
			 * What we actually care about here is whether snprintf with "%g"
			 * will print the value as an integer, so the obvious test of
			 * "*value == rint(*value)" is too strict; roundoff error might
			 * make us choose an unreasonably small unit.  As a compromise,
			 * accept a divisor that is within 1e-8 of producing an integer.
			 */
			*value = base_value / table[i].multiplier;
			*unit = table[i].unit;
			if (*value > 0 &&
				fabs((rint(*value) / *value) - 1.0) <= 1e-8)
				break;
		}
	}

	Assert(*unit != NULL);
}

/*
 * Return the name of a GUC's base unit (e.g. "ms") given its flags.
 * Return NULL if the GUC is unitless.
 */
const char *
get_config_unit_name(int flags)
{
	switch (flags & GUC_UNIT)
	{
		case 0:
			return NULL;		/* GUC has no units */
		case GUC_UNIT_BYTE:
			return "B";
		case GUC_UNIT_KB:
			return "kB";
		case GUC_UNIT_MB:
			return "MB";
		case GUC_UNIT_BLOCKS:
			{
				static char bbuf[8];

				/* initialize if first time through */
				if (bbuf[0] == '\0')
					snprintf(bbuf, sizeof(bbuf), "%dkB", BLCKSZ / 1024);
				return bbuf;
			}
		case GUC_UNIT_XBLOCKS:
			{
				static char xbuf[8];

				/* initialize if first time through */
				if (xbuf[0] == '\0')
					snprintf(xbuf, sizeof(xbuf), "%dkB", XLOG_BLCKSZ / 1024);
				return xbuf;
			}
		case GUC_UNIT_MS:
			return "ms";
		case GUC_UNIT_S:
			return "s";
		case GUC_UNIT_MIN:
			return "min";
		default:
			elog(ERROR, "unrecognized GUC units value: %d",
				 flags & GUC_UNIT);
			return NULL;
	}
}


/*
 * Try to parse value as an integer.  The accepted formats are the
 * usual decimal, octal, or hexadecimal formats, as well as floating-point
 * formats (which will be rounded to integer after any units conversion).
 * Optionally, the value can be followed by a unit name if "flags" indicates
 * a unit is allowed.
 *
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 * If not okay and hintmsg is not NULL, *hintmsg is set to a suitable
 * HINT message, or NULL if no hint provided.
 */
bool
parse_int(const char *value, int *result, int flags, const char **hintmsg)
{
	/*
	 * We assume here that double is wide enough to represent any integer
	 * value with adequate precision.
	 */
	double		val;
	char	   *endptr;

	/* To suppress compiler warnings, always set output params */
	if (result)
		*result = 0;
	if (hintmsg)
		*hintmsg = NULL;

	/*
	 * Try to parse as an integer (allowing octal or hex input).  If the
	 * conversion stops at a decimal point or 'e', or overflows, re-parse as
	 * float.  This should work fine as long as we have no unit names starting
	 * with 'e'.  If we ever do, the test could be extended to check for a
	 * sign or digit after 'e', but for now that's unnecessary.
	 */
	errno = 0;
	val = strtol(value, &endptr, 0);
	if (*endptr == '.' || *endptr == 'e' || *endptr == 'E' ||
		errno == ERANGE)
	{
		errno = 0;
		val = strtod(value, &endptr);
	}

	if (endptr == value || errno == ERANGE)
		return false;			/* no HINT for these cases */

	/* reject NaN (infinities will fail range check below) */
	if (isnan(val))
		return false;			/* treat same as syntax error; no HINT */

	/* allow whitespace between number and unit */
	while (isspace((unsigned char) *endptr))
		endptr++;

	/* Handle possible unit */
	if (*endptr != '\0')
	{
		if ((flags & GUC_UNIT) == 0)
			return false;		/* this setting does not accept a unit */

		if (!convert_to_base_unit(val,
								  endptr, (flags & GUC_UNIT),
								  &val))
		{
			/* invalid unit, or garbage after the unit; set hint and fail. */
			if (hintmsg)
			{
				if (flags & GUC_UNIT_MEMORY)
					*hintmsg = memory_units_hint;
				else
					*hintmsg = time_units_hint;
			}
			return false;
		}
	}

	/* Round to int, then check for overflow */
	val = rint(val);

	if (val > INT_MAX || val < INT_MIN)
	{
		if (hintmsg)
			*hintmsg = gettext_noop("Value exceeds integer range.");
		return false;
	}

	if (result)
		*result = (int) val;
	return true;
}

/*
 * Try to parse value as a floating point number in the usual format.
 * Optionally, the value can be followed by a unit name if "flags" indicates
 * a unit is allowed.
 *
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 * If not okay and hintmsg is not NULL, *hintmsg is set to a suitable
 * HINT message, or NULL if no hint provided.
 */
bool
parse_real(const char *value, double *result, int flags, const char **hintmsg)
{
	double		val;
	char	   *endptr;

	/* To suppress compiler warnings, always set output params */
	if (result)
		*result = 0;
	if (hintmsg)
		*hintmsg = NULL;

	errno = 0;
	val = strtod(value, &endptr);

	if (endptr == value || errno == ERANGE)
		return false;			/* no HINT for these cases */

	/* reject NaN (infinities will fail range checks later) */
	if (isnan(val))
		return false;			/* treat same as syntax error; no HINT */

	/* allow whitespace between number and unit */
	while (isspace((unsigned char) *endptr))
		endptr++;

	/* Handle possible unit */
	if (*endptr != '\0')
	{
		if ((flags & GUC_UNIT) == 0)
			return false;		/* this setting does not accept a unit */

		if (!convert_to_base_unit(val,
								  endptr, (flags & GUC_UNIT),
								  &val))
		{
			/* invalid unit, or garbage after the unit; set hint and fail. */
			if (hintmsg)
			{
				if (flags & GUC_UNIT_MEMORY)
					*hintmsg = memory_units_hint;
				else
					*hintmsg = time_units_hint;
			}
			return false;
		}
	}

	if (result)
		*result = val;
	return true;
}


/*
 * Lookup the name for an enum option with the selected value.
 * Should only ever be called with known-valid values, so throws
 * an elog(ERROR) if the enum option is not found.
 *
 * The returned string is a pointer to static data and not
 * allocated for modification.
 */
const char *
config_enum_lookup_by_value(struct config_enum *record, int val)
{
	const struct config_enum_entry *entry;

	for (entry = record->options; entry && entry->name; entry++)
	{
		if (entry->val == val)
			return entry->name;
	}

	elog(ERROR, "could not find enum option %d for %s",
		 val, record->gen.name);
	return NULL;				/* silence compiler */
}


/*
 * Lookup the value for an enum option with the selected name
 * (case-insensitive).
 * If the enum option is found, sets the retval value and returns
 * true. If it's not found, return false and retval is set to 0.
 */
bool
config_enum_lookup_by_name(struct config_enum *record, const char *value,
						   int *retval)
{
	const struct config_enum_entry *entry;

	for (entry = record->options; entry && entry->name; entry++)
	{
		if (pg_strcasecmp(value, entry->name) == 0)
		{
			*retval = entry->val;
			return true;
		}
	}

	*retval = 0;
	return false;
}


/*
 * Return a palloc'd string listing all the available options for an enum GUC
 * (excluding hidden ones), separated by the given separator.
 * If prefix is non-NULL, it is added before the first enum value.
 * If suffix is non-NULL, it is added to the end of the string.
 */
char *
config_enum_get_options(struct config_enum *record, const char *prefix,
						const char *suffix, const char *separator)
{
	const struct config_enum_entry *entry;
	StringInfoData retstr;
	int			seplen;

	initStringInfo(&retstr);
	appendStringInfoString(&retstr, prefix);

	seplen = strlen(separator);
	for (entry = record->options; entry && entry->name; entry++)
	{
		if (!entry->hidden)
		{
			appendStringInfoString(&retstr, entry->name);
			appendBinaryStringInfo(&retstr, separator, seplen);
		}
	}

	/*
	 * All the entries may have been hidden, leaving the string empty if no
	 * prefix was given. This indicates a broken GUC setup, since there is no
	 * use for an enum without any values, so we just check to make sure we
	 * don't write to invalid memory instead of actually trying to do
	 * something smart with it.
	 */
	if (retstr.len >= seplen)
	{
		/* Replace final separator */
		retstr.data[retstr.len - seplen] = '\0';
		retstr.len -= seplen;
	}

	appendStringInfoString(&retstr, suffix);

	return retstr.data;
}

/*
 * Parse and validate a proposed value for the specified configuration
 * parameter.
 *
 * This does built-in checks (such as range limits for an integer parameter)
 * and also calls any check hook the parameter may have.
 *
 * record: GUC variable's info record
 * value: proposed value, as a string
 * source: identifies source of value (check hooks may need this)
 * elevel: level to log any error reports at
 * newval: on success, converted parameter value is returned here
 * newextra: on success, receives any "extra" data returned by check hook
 *	(caller must initialize *newextra to NULL)
 *
 * Returns true if OK, false if not (or throws error, if elevel >= ERROR)
 */
static bool
parse_and_validate_value(struct config_generic *record,
						 const char *value,
						 GucSource source, int elevel,
						 union config_var_val *newval, void **newextra)
{
	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;

				if (!parse_bool(value, &newval->boolval))
				{
					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("parameter \"%s\" requires a Boolean value",
									conf->gen.name)));
					return false;
				}

				if (!call_bool_check_hook(conf, &newval->boolval, newextra,
										  source, elevel))
					return false;
			}
			break;
		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;
				const char *hintmsg;

				if (!parse_int(value, &newval->intval,
							   conf->gen.flags, &hintmsg))
				{
					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									conf->gen.name, value),
							 hintmsg ? errhint("%s", _(hintmsg)) : 0));
					return false;
				}

				if (newval->intval < conf->min || newval->intval > conf->max)
				{
					const char *unit = get_config_unit_name(conf->gen.flags);
					const char *unitspace;

					if (unit)
						unitspace = " ";
					else
						unit = unitspace = "";

					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("%d%s%s is outside the valid range for parameter \"%s\" (%d%s%s .. %d%s%s)",
									newval->intval, unitspace, unit,
									conf->gen.name,
									conf->min, unitspace, unit,
									conf->max, unitspace, unit)));
					return false;
				}

				if (!call_int_check_hook(conf, &newval->intval, newextra,
										 source, elevel))
					return false;
			}
			break;
		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;
				const char *hintmsg;

				if (!parse_real(value, &newval->realval,
								conf->gen.flags, &hintmsg))
				{
					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									conf->gen.name, value),
							 hintmsg ? errhint("%s", _(hintmsg)) : 0));
					return false;
				}

				if (newval->realval < conf->min || newval->realval > conf->max)
				{
					const char *unit = get_config_unit_name(conf->gen.flags);
					const char *unitspace;

					if (unit)
						unitspace = " ";
					else
						unit = unitspace = "";

					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("%g%s%s is outside the valid range for parameter \"%s\" (%g%s%s .. %g%s%s)",
									newval->realval, unitspace, unit,
									conf->gen.name,
									conf->min, unitspace, unit,
									conf->max, unitspace, unit)));
					return false;
				}

				if (!call_real_check_hook(conf, &newval->realval, newextra,
										  source, elevel))
					return false;
			}
			break;
		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;

				/*
				 * The value passed by the caller could be transient, so we
				 * always strdup it.
				 */
				newval->stringval = guc_strdup(elevel, value);
				if (newval->stringval == NULL)
					return false;

				/*
				 * The only built-in "parsing" check we have is to apply
				 * truncation if GUC_IS_NAME.
				 */
				if (conf->gen.flags & GUC_IS_NAME)
					truncate_identifier(newval->stringval,
										strlen(newval->stringval),
										true);

				if (!call_string_check_hook(conf, &newval->stringval, newextra,
											source, elevel))
				{
					guc_free(newval->stringval);
					newval->stringval = NULL;
					return false;
				}
			}
			break;
		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;

				if (!config_enum_lookup_by_name(conf, value, &newval->enumval))
				{
					char	   *hintmsg;

					hintmsg = config_enum_get_options(conf,
													  "Available values: ",
													  ".", ", ");

					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									conf->gen.name, value),
							 hintmsg ? errhint("%s", _(hintmsg)) : 0));

					if (hintmsg)
						pfree(hintmsg);
					return false;
				}

				if (!call_enum_check_hook(conf, &newval->enumval, newextra,
										  source, elevel))
					return false;
			}
			break;
	}

	return true;
}


/*
 * set_config_option: sets option `name' to given value.
 *
 * The value should be a string, which will be parsed and converted to
 * the appropriate data type.  The context and source parameters indicate
 * in which context this function is being called, so that it can apply the
 * access restrictions properly.
 *
 * If value is NULL, set the option to its default value (normally the
 * reset_val, but if source == PGC_S_DEFAULT we instead use the boot_val).
 *
 * action indicates whether to set the value globally in the session, locally
 * to the current top transaction, or just for the duration of a function call.
 *
 * If changeVal is false then don't really set the option but do all
 * the checks to see if it would work.
 *
 * elevel should normally be passed as zero, allowing this function to make
 * its standard choice of ereport level.  However some callers need to be
 * able to override that choice; they should pass the ereport level to use.
 *
 * is_reload should be true only when called from read_nondefault_variables()
 * or RestoreGUCState(), where we are trying to load some other process's
 * GUC settings into a new process.
 *
 * Return value:
 *	+1: the value is valid and was successfully applied.
 *	0:	the name or value is invalid, or it's invalid to try to set
 *		this GUC now; but elevel was less than ERROR (see below).
 *	-1: no error detected, but the value was not applied, either
 *		because changeVal is false or there is some overriding setting.
 *
 * If there is an error (non-existing option, invalid value, etc) then an
 * ereport(ERROR) is thrown *unless* this is called for a source for which
 * we don't want an ERROR (currently, those are defaults, the config file,
 * and per-database or per-user settings, as well as callers who specify
 * a less-than-ERROR elevel).  In those cases we write a suitable error
 * message via ereport() and return 0.
 *
 * See also SetConfigOption for an external interface.
 */
int
set_config_option(const char *name, const char *value,
				  GucContext context, GucSource source,
				  GucAction action, bool changeVal, int elevel,
				  bool is_reload)
{
	Oid			srole;

	/*
	 * Non-interactive sources should be treated as having all privileges,
	 * except for PGC_S_CLIENT.  Note in particular that this is true for
	 * pg_db_role_setting sources (PGC_S_GLOBAL etc): we assume a suitable
	 * privilege check was done when the pg_db_role_setting entry was made.
	 */
	if (source >= PGC_S_INTERACTIVE || source == PGC_S_CLIENT)
		srole = GetUserId();
	else
		srole = BOOTSTRAP_SUPERUSERID;

	return set_config_with_handle(name, NULL, value,
								  context, source, srole,
								  action, changeVal, elevel,
								  is_reload);
}

/*
 * set_config_option_ext: sets option `name' to given value.
 *
 * This API adds the ability to explicitly specify which role OID
 * is considered to be setting the value.  Most external callers can use
 * set_config_option() and let it determine that based on the GucSource,
 * but there are a few that are supplying a value that was determined
 * in some special way and need to override the decision.  Also, when
 * restoring a previously-assigned value, it's important to supply the
 * same role OID that set the value originally; so all guc.c callers
 * that are doing that type of thing need to call this directly.
 *
 * Generally, srole should be GetUserId() when the source is a SQL operation,
 * or BOOTSTRAP_SUPERUSERID if the source is a config file or similar.
 */
int
set_config_option_ext(const char *name, const char *value,
					  GucContext context, GucSource source, Oid srole,
					  GucAction action, bool changeVal, int elevel,
					  bool is_reload)
{
	return set_config_with_handle(name, NULL, value,
								  context, source, srole,
								  action, changeVal, elevel,
								  is_reload);
}


/*
 * set_config_with_handle: sets option `name' to given value.
 *
 * This API adds the ability to pass a 'handle' argument, which can be
 * obtained by the caller from get_config_handle().  NULL has no effect,
 * but a non-null value avoids the need to search the GUC tables.
 *
 * This should be used by callers which repeatedly set the same config
 * option(s), and want to avoid the overhead of a hash lookup each time.
 */
int
set_config_with_handle(const char *name, config_handle *handle,
					   const char *value,
					   GucContext context, GucSource source, Oid srole,
					   GucAction action, bool changeVal, int elevel,
					   bool is_reload)
{
	struct config_generic *record;
	union config_var_val newval_union;
	void	   *newextra = NULL;
	bool		prohibitValueChange = false;
	bool		makeDefault;

	if (elevel == 0)
	{
		if (source == PGC_S_DEFAULT || source == PGC_S_FILE)
		{
			/*
			 * To avoid cluttering the log, only the postmaster bleats loudly
			 * about problems with the config file.
			 */
			elevel = IsUnderPostmaster ? DEBUG3 : LOG;
		}
		else if (source == PGC_S_GLOBAL ||
				 source == PGC_S_DATABASE ||
				 source == PGC_S_USER ||
				 source == PGC_S_DATABASE_USER)
			elevel = WARNING;
		else
			elevel = ERROR;
	}

	/* if handle is specified, no need to look up option */
	if (!handle)
	{
		record = find_option(name, true, false, elevel);
		if (record == NULL)
			return 0;
	}
	else
		record = handle;

	/*
	 * GUC_ACTION_SAVE changes are acceptable during a parallel operation,
	 * because the current worker will also pop the change.  We're probably
	 * dealing with a function having a proconfig entry.  Only the function's
	 * body should observe the change, and peer workers do not share in the
	 * execution of a function call started by this worker.
	 *
	 * Also allow normal setting if the GUC is marked GUC_ALLOW_IN_PARALLEL.
	 *
	 * Other changes might need to affect other workers, so forbid them.
	 */
	if (IsInParallelMode() && changeVal && action != GUC_ACTION_SAVE &&
		(record->flags & GUC_ALLOW_IN_PARALLEL) == 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("parameter \"%s\" cannot be set during a parallel operation",
						record->name)));
		return 0;
	}

	/*
	 * Check if the option can be set at this time. See guc.h for the precise
	 * rules.
	 */
	switch (record->context)
	{
		case PGC_INTERNAL:
			if (context != PGC_INTERNAL)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed",
								record->name)));
				return 0;
			}
			break;
		case PGC_POSTMASTER:
			if (context == PGC_SIGHUP)
			{
				/*
				 * We are re-reading a PGC_POSTMASTER variable from
				 * postgresql.conf.  We can't change the setting, so we should
				 * give a warning if the DBA tries to change it.  However,
				 * because of variant formats, canonicalization by check
				 * hooks, etc, we can't just compare the given string directly
				 * to what's stored.  Set a flag to check below after we have
				 * the final storable value.
				 */
				prohibitValueChange = true;
			}
			else if (context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed without restarting the server",
								record->name)));
				return 0;
			}
			break;
		case PGC_SIGHUP:
			if (context != PGC_SIGHUP && context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed now",
								record->name)));
				return 0;
			}

			/*
			 * Hmm, the idea of the SIGHUP context is "ought to be global, but
			 * can be changed after postmaster start". But there's nothing
			 * that prevents a crafty administrator from sending SIGHUP
			 * signals to individual backends only.
			 */
			break;
		case PGC_SU_BACKEND:
			if (context == PGC_BACKEND)
			{
				/*
				 * Check whether the requesting user has been granted
				 * privilege to set this GUC.
				 */
				AclResult	aclresult;

				aclresult = pg_parameter_aclcheck(record->name, srole, ACL_SET);
				if (aclresult != ACLCHECK_OK)
				{
					/* No granted privilege */
					ereport(elevel,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to set parameter \"%s\"",
									record->name)));
					return 0;
				}
			}
			/* fall through to process the same as PGC_BACKEND */
			/* FALLTHROUGH */
		case PGC_BACKEND:
			if (context == PGC_SIGHUP)
			{
				/*
				 * If a PGC_BACKEND or PGC_SU_BACKEND parameter is changed in
				 * the config file, we want to accept the new value in the
				 * postmaster (whence it will propagate to
				 * subsequently-started backends), but ignore it in existing
				 * backends.  This is a tad klugy, but necessary because we
				 * don't re-read the config file during backend start.
				 *
				 * However, if changeVal is false then plow ahead anyway since
				 * we are trying to find out if the value is potentially good,
				 * not actually use it.
				 *
				 * In EXEC_BACKEND builds, this works differently: we load all
				 * non-default settings from the CONFIG_EXEC_PARAMS file
				 * during backend start.  In that case we must accept
				 * PGC_SIGHUP settings, so as to have the same value as if
				 * we'd forked from the postmaster.  This can also happen when
				 * using RestoreGUCState() within a background worker that
				 * needs to have the same settings as the user backend that
				 * started it. is_reload will be true when either situation
				 * applies.
				 */
				if (IsUnderPostmaster && changeVal && !is_reload)
					return -1;
			}
			else if (context != PGC_POSTMASTER &&
					 context != PGC_BACKEND &&
					 context != PGC_SU_BACKEND &&
					 source != PGC_S_CLIENT)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be set after connection start",
								record->name)));
				return 0;
			}
			break;
		case PGC_SUSET:
			if (context == PGC_USERSET || context == PGC_BACKEND)
			{
				/*
				 * Check whether the requesting user has been granted
				 * privilege to set this GUC.
				 */
				AclResult	aclresult;

				aclresult = pg_parameter_aclcheck(record->name, srole, ACL_SET);
				if (aclresult != ACLCHECK_OK)
				{
					/* No granted privilege */
					ereport(elevel,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to set parameter \"%s\"",
									record->name)));
					return 0;
				}
			}
			break;
		case PGC_USERSET:
			/* always okay */
			break;
	}

	/*
	 * Disallow changing GUC_NOT_WHILE_SEC_REST values if we are inside a
	 * security restriction context.  We can reject this regardless of the GUC
	 * context or source, mainly because sources that it might be reasonable
	 * to override for won't be seen while inside a function.
	 *
	 * Note: variables marked GUC_NOT_WHILE_SEC_REST should usually be marked
	 * GUC_NO_RESET_ALL as well, because ResetAllOptions() doesn't check this.
	 * An exception might be made if the reset value is assumed to be "safe".
	 *
	 * Note: this flag is currently used for "session_authorization" and
	 * "role".  We need to prohibit changing these inside a local userid
	 * context because when we exit it, GUC won't be notified, leaving things
	 * out of sync.  (This could be fixed by forcing a new GUC nesting level,
	 * but that would change behavior in possibly-undesirable ways.)  Also, we
	 * prohibit changing these in a security-restricted operation because
	 * otherwise RESET could be used to regain the session user's privileges.
	 */
	if (record->flags & GUC_NOT_WHILE_SEC_REST)
	{
		if (InLocalUserIdChange())
		{
			/*
			 * Phrasing of this error message is historical, but it's the most
			 * common case.
			 */
			ereport(elevel,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("cannot set parameter \"%s\" within security-definer function",
							record->name)));
			return 0;
		}
		if (InSecurityRestrictedOperation())
		{
			ereport(elevel,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("cannot set parameter \"%s\" within security-restricted operation",
							record->name)));
			return 0;
		}
	}

	/* Disallow resetting and saving GUC_NO_RESET values */
	if (record->flags & GUC_NO_RESET)
	{
		if (value == NULL)
		{
			ereport(elevel,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("parameter \"%s\" cannot be reset", record->name)));
			return 0;
		}
		if (action == GUC_ACTION_SAVE)
		{
			ereport(elevel,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("parameter \"%s\" cannot be set locally in functions",
							record->name)));
			return 0;
		}
	}

	/*
	 * Should we set reset/stacked values?	(If so, the behavior is not
	 * transactional.)	This is done either when we get a default value from
	 * the database's/user's/client's default settings or when we reset a
	 * value to its default.
	 */
	makeDefault = changeVal && (source <= PGC_S_OVERRIDE) &&
		((value != NULL) || source == PGC_S_DEFAULT);

	/*
	 * Ignore attempted set if overridden by previously processed setting.
	 * However, if changeVal is false then plow ahead anyway since we are
	 * trying to find out if the value is potentially good, not actually use
	 * it. Also keep going if makeDefault is true, since we may want to set
	 * the reset/stacked values even if we can't set the variable itself.
	 */
	if (record->source > source)
	{
		if (changeVal && !makeDefault)
		{
			elog(DEBUG3, "\"%s\": setting ignored because previous source is higher priority",
				 record->name);
			return -1;
		}
		changeVal = false;
	}

	/*
	 * Evaluate value and set variable.
	 */
	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;

#define newval (newval_union.boolval)

				if (value)
				{
					if (!parse_and_validate_value(record, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_bool_check_hook(conf, &newval, &newextra,
											  source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* Release newextra, unless it's reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										conf->gen.name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* Save old value to support transaction abort */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.boolval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* Perhaps we didn't install newextra anywhere */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;

#define newval (newval_union.intval)

				if (value)
				{
					if (!parse_and_validate_value(record, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_int_check_hook(conf, &newval, &newextra,
											 source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* Release newextra, unless it's reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										conf->gen.name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* Save old value to support transaction abort */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.intval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* Perhaps we didn't install newextra anywhere */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;

#define newval (newval_union.realval)

				if (value)
				{
					if (!parse_and_validate_value(record, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_real_check_hook(conf, &newval, &newextra,
											  source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* Release newextra, unless it's reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										conf->gen.name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* Save old value to support transaction abort */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.realval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* Perhaps we didn't install newextra anywhere */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;
				GucContext	orig_context = context;
				GucSource	orig_source = source;
				Oid			orig_srole = srole;

#define newval (newval_union.stringval)

				if (value)
				{
					if (!parse_and_validate_value(record, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					/* non-NULL boot_val must always get strdup'd */
					if (conf->boot_val != NULL)
					{
						newval = guc_strdup(elevel, conf->boot_val);
						if (newval == NULL)
							return 0;
					}
					else
						newval = NULL;

					if (!call_string_check_hook(conf, &newval, &newextra,
												source, elevel))
					{
						guc_free(newval);
						return 0;
					}
				}
				else
				{
					/*
					 * strdup not needed, since reset_val is already under
					 * guc.c's control
					 */
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					bool		newval_different;

					/* newval shouldn't be NULL, so we're a bit sloppy here */
					newval_different = (*conf->variable == NULL ||
										newval == NULL ||
										strcmp(*conf->variable, newval) != 0);

					/* Release newval, unless it's reset_val */
					if (newval && !string_field_used(conf, newval))
						guc_free(newval);
					/* Release newextra, unless it's reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (newval_different)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										conf->gen.name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* Save old value to support transaction abort */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					set_string_field(conf, conf->variable, newval);
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;

					/*
					 * Ugly hack: during SET session_authorization, forcibly
					 * do SET ROLE NONE with the same context/source/etc, so
					 * that the effects will have identical lifespan.  This is
					 * required by the SQL spec, and it's not possible to do
					 * it within the variable's check hook or assign hook
					 * because our APIs for those don't pass enough info.
					 * However, don't do it if is_reload: in that case we
					 * expect that if "role" isn't supposed to be default, it
					 * has been or will be set by a separate reload action.
					 *
					 * Also, for the call from InitializeSessionUserId with
					 * source == PGC_S_OVERRIDE, use PGC_S_DYNAMIC_DEFAULT for
					 * "role"'s source, so that it's still possible to set
					 * "role" from pg_db_role_setting entries.  (See notes in
					 * InitializeSessionUserId before changing this.)
					 *
					 * A fine point: for RESET session_authorization, we do
					 * "RESET role" not "SET ROLE NONE" (by passing down NULL
					 * rather than "none" for the value).  This would have the
					 * same effects in typical cases, but if the reset value
					 * of "role" is not "none" it seems better to revert to
					 * that.
					 */
					if (!is_reload &&
						strcmp(conf->gen.name, "session_authorization") == 0)
						(void) set_config_with_handle("role", NULL,
													  value ? "none" : NULL,
													  orig_context,
													  (orig_source == PGC_S_OVERRIDE)
													  ? PGC_S_DYNAMIC_DEFAULT
													  : orig_source,
													  orig_srole,
													  action,
													  true,
													  elevel,
													  false);
				}

				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						set_string_field(conf, &conf->reset_val, newval);
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							set_string_field(conf, &stack->prior.val.stringval,
											 newval);
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* Perhaps we didn't install newval anywhere */
				if (newval && !string_field_used(conf, newval))
					guc_free(newval);
				/* Perhaps we didn't install newextra anywhere */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;

#define newval (newval_union.enumval)

				if (value)
				{
					if (!parse_and_validate_value(record, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_enum_check_hook(conf, &newval, &newextra,
											  source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* Release newextra, unless it's reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										conf->gen.name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* Save old value to support transaction abort */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.enumval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* Perhaps we didn't install newextra anywhere */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}
	}

	if (changeVal && (record->flags & GUC_REPORT) &&
		!(record->status & GUC_NEEDS_REPORT))
	{
		record->status |= GUC_NEEDS_REPORT;
		slist_push_head(&guc_report_list, &record->report_link);
	}

	return changeVal ? 1 : -1;
}


/*
 * Retrieve a config_handle for the given name, suitable for calling
 * set_config_with_handle(). Only return handle to permanent GUC.
 */
config_handle *
get_config_handle(const char *name)
{
	struct config_generic *gen = find_option(name, false, false, 0);

	if (gen && ((gen->flags & GUC_CUSTOM_PLACEHOLDER) == 0))
		return gen;

	return NULL;
}


/*
 * Set the fields for source file and line number the setting came from.
 */
static void
set_config_sourcefile(const char *name, char *sourcefile, int sourceline)
{
	struct config_generic *record;
	int			elevel;

	/*
	 * To avoid cluttering the log, only the postmaster bleats loudly about
	 * problems with the config file.
	 */
	elevel = IsUnderPostmaster ? DEBUG3 : LOG;

	record = find_option(name, true, false, elevel);
	/* should not happen */
	if (record == NULL)
		return;

	sourcefile = guc_strdup(elevel, sourcefile);
	guc_free(record->sourcefile);
	record->sourcefile = sourcefile;
	record->sourceline = sourceline;
}

/*
 * Set a config option to the given value.
 *
 * See also set_config_option; this is just the wrapper to be called from
 * outside GUC.  (This function should be used when possible, because its API
 * is more stable than set_config_option's.)
 *
 * Note: there is no support here for setting source file/line, as it
 * is currently not needed.
 */
void
SetConfigOption(const char *name, const char *value,
				GucContext context, GucSource source)
{
	(void) set_config_option(name, value, context, source,
							 GUC_ACTION_SET, true, 0, false);
}



/*
 * Fetch the current value of the option `name', as a string.
 *
 * If the option doesn't exist, return NULL if missing_ok is true,
 * otherwise throw an ereport and don't return.
 *
 * If restrict_privileged is true, we also enforce that only superusers and
 * members of the pg_read_all_settings role can see GUC_SUPERUSER_ONLY
 * variables.  This should only be passed as true in user-driven calls.
 *
 * The string is *not* allocated for modification and is really only
 * valid until the next call to configuration related functions.
 */
const char *
GetConfigOption(const char *name, bool missing_ok, bool restrict_privileged)
{
	struct config_generic *record;
	static char buffer[256];

	record = find_option(name, false, missing_ok, ERROR);
	if (record == NULL)
		return NULL;
	if (restrict_privileged &&
		!ConfigOptionIsVisible(record))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to examine \"%s\"", name),
				 errdetail("Only roles with privileges of the \"%s\" role may examine this parameter.",
						   "pg_read_all_settings")));

	switch (record->vartype)
	{
		case PGC_BOOL:
			return *((struct config_bool *) record)->variable ? "on" : "off";

		case PGC_INT:
			snprintf(buffer, sizeof(buffer), "%d",
					 *((struct config_int *) record)->variable);
			return buffer;

		case PGC_REAL:
			snprintf(buffer, sizeof(buffer), "%g",
					 *((struct config_real *) record)->variable);
			return buffer;

		case PGC_STRING:
			return *((struct config_string *) record)->variable ?
				*((struct config_string *) record)->variable : "";

		case PGC_ENUM:
			return config_enum_lookup_by_value((struct config_enum *) record,
											   *((struct config_enum *) record)->variable);
	}
	return NULL;
}

/*
 * Get the RESET value associated with the given option.
 *
 * Note: this is not re-entrant, due to use of static result buffer;
 * not to mention that a string variable could have its reset_val changed.
 * Beware of assuming the result value is good for very long.
 */
const char *
GetConfigOptionResetString(const char *name)
{
	struct config_generic *record;
	static char buffer[256];

	record = find_option(name, false, false, ERROR);
	Assert(record != NULL);
	if (!ConfigOptionIsVisible(record))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to examine \"%s\"", name),
				 errdetail("Only roles with privileges of the \"%s\" role may examine this parameter.",
						   "pg_read_all_settings")));

	switch (record->vartype)
	{
		case PGC_BOOL:
			return ((struct config_bool *) record)->reset_val ? "on" : "off";

		case PGC_INT:
			snprintf(buffer, sizeof(buffer), "%d",
					 ((struct config_int *) record)->reset_val);
			return buffer;

		case PGC_REAL:
			snprintf(buffer, sizeof(buffer), "%g",
					 ((struct config_real *) record)->reset_val);
			return buffer;

		case PGC_STRING:
			return ((struct config_string *) record)->reset_val ?
				((struct config_string *) record)->reset_val : "";

		case PGC_ENUM:
			return config_enum_lookup_by_value((struct config_enum *) record,
											   ((struct config_enum *) record)->reset_val);
	}
	return NULL;
}

/*
 * Get the GUC flags associated with the given option.
 *
 * If the option doesn't exist, return 0 if missing_ok is true,
 * otherwise throw an ereport and don't return.
 */
int
GetConfigOptionFlags(const char *name, bool missing_ok)
{
	struct config_generic *record;

	record = find_option(name, false, missing_ok, ERROR);
	if (record == NULL)
		return 0;
	return record->flags;
}


/*
 * Write updated configuration parameter values into a temporary file.
 * This function traverses the list of parameters and quotes the string
 * values before writing them.
 */
static void
write_auto_conf_file(int fd, const char *filename, ConfigVariable *head)
{
	StringInfoData buf;
	ConfigVariable *item;

	initStringInfo(&buf);

	/* Emit file header containing warning comment */
	appendStringInfoString(&buf, "# Do not edit this file manually!\n");
	appendStringInfoString(&buf, "# It will be overwritten by the ALTER SYSTEM command.\n");

	errno = 0;
	if (write(fd, buf.data, buf.len) != buf.len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", filename)));
	}

	/* Emit each parameter, properly quoting the value */
	for (item = head; item != NULL; item = item->next)
	{
		char	   *escaped;

		resetStringInfo(&buf);

		appendStringInfoString(&buf, item->name);
		appendStringInfoString(&buf, " = '");

		escaped = escape_single_quotes_ascii(item->value);
		if (!escaped)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		appendStringInfoString(&buf, escaped);
		free(escaped);

		appendStringInfoString(&buf, "'\n");

		errno = 0;
		if (write(fd, buf.data, buf.len) != buf.len)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", filename)));
		}
	}

	/* fsync before considering the write to be successful */
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", filename)));

	pfree(buf.data);
}

/*
 * Update the given list of configuration parameters, adding, replacing
 * or deleting the entry for item "name" (delete if "value" == NULL).
 */
static void
replace_auto_config_value(ConfigVariable **head_p, ConfigVariable **tail_p,
						  const char *name, const char *value)
{
	ConfigVariable *item,
			   *next,
			   *prev = NULL;

	/*
	 * Remove any existing match(es) for "name".  Normally there'd be at most
	 * one, but if external tools have modified the config file, there could
	 * be more.
	 */
	for (item = *head_p; item != NULL; item = next)
	{
		next = item->next;
		if (guc_name_compare(item->name, name) == 0)
		{
			/* found a match, delete it */
			if (prev)
				prev->next = next;
			else
				*head_p = next;
			if (next == NULL)
				*tail_p = prev;

			pfree(item->name);
			pfree(item->value);
			pfree(item->filename);
			pfree(item);
		}
		else
			prev = item;
	}

	/* Done if we're trying to delete it */
	if (value == NULL)
		return;

	/* OK, append a new entry */
	item = palloc(sizeof *item);
	item->name = pstrdup(name);
	item->value = pstrdup(value);
	item->errmsg = NULL;
	item->filename = pstrdup("");	/* new item has no location */
	item->sourceline = 0;
	item->ignore = false;
	item->applied = false;
	item->next = NULL;

	if (*head_p == NULL)
		*head_p = item;
	else
		(*tail_p)->next = item;
	*tail_p = item;
}


/*
 * Execute ALTER SYSTEM statement.
 *
 * Read the old PG_AUTOCONF_FILENAME file, merge in the new variable value,
 * and write out an updated file.  If the command is ALTER SYSTEM RESET ALL,
 * we can skip reading the old file and just write an empty file.
 *
 * An LWLock is used to serialize updates of the configuration file.
 *
 * In case of an error, we leave the original automatic
 * configuration file (PG_AUTOCONF_FILENAME) intact.
 */
void
AlterSystemSetConfigFile(AlterSystemStmt *altersysstmt)
{
	char	   *name;
	char	   *value;
	bool		resetall = false;
	ConfigVariable *head = NULL;
	ConfigVariable *tail = NULL;
	volatile int Tmpfd;
	char		AutoConfFileName[MAXPGPATH];
	char		AutoConfTmpFileName[MAXPGPATH];

	/*
	 * Extract statement arguments
	 */
	name = altersysstmt->setstmt->name;

	if (!AllowAlterSystem)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER SYSTEM is not allowed in this environment")));

	switch (altersysstmt->setstmt->kind)
	{
		case VAR_SET_VALUE:
			value = ExtractSetVariableArgs(altersysstmt->setstmt);
			break;

		case VAR_SET_DEFAULT:
		case VAR_RESET:
			value = NULL;
			break;

		case VAR_RESET_ALL:
			value = NULL;
			resetall = true;
			break;

		default:
			elog(ERROR, "unrecognized alter system stmt type: %d",
				 altersysstmt->setstmt->kind);
			break;
	}

	/*
	 * Check permission to run ALTER SYSTEM on the target variable
	 */
	if (!superuser())
	{
		if (resetall)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to perform ALTER SYSTEM RESET ALL")));
		else
		{
			AclResult	aclresult;

			aclresult = pg_parameter_aclcheck(name, GetUserId(),
											  ACL_ALTER_SYSTEM);
			if (aclresult != ACLCHECK_OK)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set parameter \"%s\"",
								name)));
		}
	}

	/*
	 * Unless it's RESET_ALL, validate the target variable and value
	 */
	if (!resetall)
	{
		struct config_generic *record;

		/* We don't want to create a placeholder if there's not one already */
		record = find_option(name, false, true, DEBUG5);
		if (record != NULL)
		{
			/*
			 * Don't allow parameters that can't be set in configuration files
			 * to be set in PG_AUTOCONF_FILENAME file.
			 */
			if ((record->context == PGC_INTERNAL) ||
				(record->flags & GUC_DISALLOW_IN_FILE) ||
				(record->flags & GUC_DISALLOW_IN_AUTO_FILE))
				ereport(ERROR,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed",
								name)));

			/*
			 * If a value is specified, verify that it's sane.
			 */
			if (value)
			{
				union config_var_val newval;
				void	   *newextra = NULL;

				if (!parse_and_validate_value(record, value,
											  PGC_S_FILE, ERROR,
											  &newval, &newextra))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									name, value)));

				if (record->vartype == PGC_STRING && newval.stringval != NULL)
					guc_free(newval.stringval);
				guc_free(newextra);
			}
		}
		else
		{
			/*
			 * Variable not known; check we'd be allowed to create it.  (We
			 * cannot validate the value, but that's fine.  A non-core GUC in
			 * the config file cannot cause postmaster start to fail, so we
			 * don't have to be too tense about possibly installing a bad
			 * value.)
			 */
			(void) assignable_custom_variable_name(name, false, ERROR);
		}

		/*
		 * We must also reject values containing newlines, because the grammar
		 * for config files doesn't support embedded newlines in string
		 * literals.
		 */
		if (value && strchr(value, '\n'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("parameter value for ALTER SYSTEM must not contain a newline")));
	}

	/*
	 * PG_AUTOCONF_FILENAME and its corresponding temporary file are always in
	 * the data directory, so we can reference them by simple relative paths.
	 */
	snprintf(AutoConfFileName, sizeof(AutoConfFileName), "%s",
			 PG_AUTOCONF_FILENAME);
	snprintf(AutoConfTmpFileName, sizeof(AutoConfTmpFileName), "%s.%s",
			 AutoConfFileName,
			 "tmp");

	/*
	 * Only one backend is allowed to operate on PG_AUTOCONF_FILENAME at a
	 * time.  Use AutoFileLock to ensure that.  We must hold the lock while
	 * reading the old file contents.
	 */
	LWLockAcquire(AutoFileLock, LW_EXCLUSIVE);

	/*
	 * If we're going to reset everything, then no need to open or parse the
	 * old file.  We'll just write out an empty list.
	 */
	if (!resetall)
	{
		struct stat st;

		if (stat(AutoConfFileName, &st) == 0)
		{
			/* open old file PG_AUTOCONF_FILENAME */
			FILE	   *infile;

			infile = AllocateFile(AutoConfFileName, "r");
			if (infile == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m",
								AutoConfFileName)));

			/* parse it */
			if (!ParseConfigFp(infile, AutoConfFileName, CONF_FILE_START_DEPTH,
							   LOG, &head, &tail))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not parse contents of file \"%s\"",
								AutoConfFileName)));

			FreeFile(infile);
		}

		/*
		 * Now, replace any existing entry with the new value, or add it if
		 * not present.
		 */
		replace_auto_config_value(&head, &tail, name, value);
	}

	/*
	 * Invoke the post-alter hook for setting this GUC variable.  GUCs
	 * typically do not have corresponding entries in pg_parameter_acl, so we
	 * call the hook using the name rather than a potentially-non-existent
	 * OID.  Nonetheless, we pass ParameterAclRelationId so that this call
	 * context can be distinguished from others.  (Note that "name" will be
	 * NULL in the RESET ALL case.)
	 *
	 * We do this here rather than at the end, because ALTER SYSTEM is not
	 * transactional.  If the hook aborts our transaction, it will be cleaner
	 * to do so before we touch any files.
	 */
	InvokeObjectPostAlterHookArgStr(ParameterAclRelationId, name,
									ACL_ALTER_SYSTEM,
									altersysstmt->setstmt->kind,
									false);

	/*
	 * To ensure crash safety, first write the new file data to a temp file,
	 * then atomically rename it into place.
	 *
	 * If there is a temp file left over due to a previous crash, it's okay to
	 * truncate and reuse it.
	 */
	Tmpfd = BasicOpenFile(AutoConfTmpFileName,
						  O_CREAT | O_RDWR | O_TRUNC);
	if (Tmpfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						AutoConfTmpFileName)));

	/*
	 * Use a TRY block to clean up the file if we fail.  Since we need a TRY
	 * block anyway, OK to use BasicOpenFile rather than OpenTransientFile.
	 */
	PG_TRY();
	{
		/* Write and sync the new contents to the temporary file */
		write_auto_conf_file(Tmpfd, AutoConfTmpFileName, head);

		/* Close before renaming; may be required on some platforms */
		close(Tmpfd);
		Tmpfd = -1;

		/*
		 * As the rename is atomic operation, if any problem occurs after this
		 * at worst it can lose the parameters set by last ALTER SYSTEM
		 * command.
		 */
		durable_rename(AutoConfTmpFileName, AutoConfFileName, ERROR);
	}
	PG_CATCH();
	{
		/* Close file first, else unlink might fail on some platforms */
		if (Tmpfd >= 0)
			close(Tmpfd);

		/* Unlink, but ignore any error */
		(void) unlink(AutoConfTmpFileName);

		PG_RE_THROW();
	}
	PG_END_TRY();

	FreeConfigVariables(head);

	LWLockRelease(AutoFileLock);
}


/*
 * Common code for DefineCustomXXXVariable subroutines: allocate the
 * new variable's config struct and fill in generic fields.
 */
static struct config_generic *
init_custom_variable(const char *name,
					 const char *short_desc,
					 const char *long_desc,
					 GucContext context,
					 int flags,
					 enum config_type type,
					 size_t sz)
{
	struct config_generic *gen;

	/*
	 * Only allow custom PGC_POSTMASTER variables to be created during shared
	 * library preload; any later than that, we can't ensure that the value
	 * doesn't change after startup.  This is a fatal elog if it happens; just
	 * erroring out isn't safe because we don't know what the calling loadable
	 * module might already have hooked into.
	 */
	if (context == PGC_POSTMASTER &&
		!process_shared_preload_libraries_in_progress)
		elog(FATAL, "cannot create PGC_POSTMASTER variables after startup");

	/*
	 * We can't support custom GUC_LIST_QUOTE variables, because the wrong
	 * things would happen if such a variable were set or pg_dump'd when the
	 * defining extension isn't loaded.  Again, treat this as fatal because
	 * the loadable module may be partly initialized already.
	 */
	if (flags & GUC_LIST_QUOTE)
		elog(FATAL, "extensions cannot define GUC_LIST_QUOTE variables");

	/*
	 * Before pljava commit 398f3b876ed402bdaec8bc804f29e2be95c75139
	 * (2015-12-15), two of that module's PGC_USERSET variables facilitated
	 * trivial escalation to superuser privileges.  Restrict the variables to
	 * protect sites that have yet to upgrade pljava.
	 */
	if (context == PGC_USERSET &&
		(strcmp(name, "pljava.classpath") == 0 ||
		 strcmp(name, "pljava.vmoptions") == 0))
		context = PGC_SUSET;

	gen = (struct config_generic *) guc_malloc(ERROR, sz);
	memset(gen, 0, sz);

	gen->name = guc_strdup(ERROR, name);
	gen->context = context;
	gen->group = CUSTOM_OPTIONS;
	gen->short_desc = short_desc;
	gen->long_desc = long_desc;
	gen->flags = flags;
	gen->vartype = type;

	return gen;
}

/*
 * Common code for DefineCustomXXXVariable subroutines: insert the new
 * variable into the GUC variable hash, replacing any placeholder.
 */
static void
define_custom_variable(struct config_generic *variable)
{
	const char *name = variable->name;
	GUCHashEntry *hentry;
	struct config_string *pHolder;

	/* Check mapping between initial and default value */
	Assert(check_GUC_init(variable));

	/*
	 * See if there's a placeholder by the same name.
	 */
	hentry = (GUCHashEntry *) hash_search(guc_hashtab,
										  &name,
										  HASH_FIND,
										  NULL);
	if (hentry == NULL)
	{
		/*
		 * No placeholder to replace, so we can just add it ... but first,
		 * make sure it's initialized to its default value.
		 */
		InitializeOneGUCOption(variable);
		add_guc_variable(variable, ERROR);
		return;
	}

	/*
	 * This better be a placeholder
	 */
	if ((hentry->gucvar->flags & GUC_CUSTOM_PLACEHOLDER) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("attempt to redefine parameter \"%s\"", name)));

	Assert(hentry->gucvar->vartype == PGC_STRING);
	pHolder = (struct config_string *) hentry->gucvar;

	/*
	 * First, set the variable to its default value.  We must do this even
	 * though we intend to immediately apply a new value, since it's possible
	 * that the new value is invalid.
	 */
	InitializeOneGUCOption(variable);

	/*
	 * Replace the placeholder in the hash table.  We aren't changing the name
	 * (at least up to case-folding), so the hash value is unchanged.
	 */
	hentry->gucname = name;
	hentry->gucvar = variable;

	/*
	 * Remove the placeholder from any lists it's in, too.
	 */
	RemoveGUCFromLists(&pHolder->gen);

	/*
	 * Assign the string value(s) stored in the placeholder to the real
	 * variable.  Essentially, we need to duplicate all the active and stacked
	 * values, but with appropriate validation and datatype adjustment.
	 *
	 * If an assignment fails, we report a WARNING and keep going.  We don't
	 * want to throw ERROR for bad values, because it'd bollix the add-on
	 * module that's presumably halfway through getting loaded.  In such cases
	 * the default or previous state will become active instead.
	 */

	/* First, apply the reset value if any */
	if (pHolder->reset_val)
		(void) set_config_option_ext(name, pHolder->reset_val,
									 pHolder->gen.reset_scontext,
									 pHolder->gen.reset_source,
									 pHolder->gen.reset_srole,
									 GUC_ACTION_SET, true, WARNING, false);
	/* That should not have resulted in stacking anything */
	Assert(variable->stack == NULL);

	/* Now, apply current and stacked values, in the order they were stacked */
	reapply_stacked_values(variable, pHolder, pHolder->gen.stack,
						   *(pHolder->variable),
						   pHolder->gen.scontext, pHolder->gen.source,
						   pHolder->gen.srole);

	/* Also copy over any saved source-location information */
	if (pHolder->gen.sourcefile)
		set_config_sourcefile(name, pHolder->gen.sourcefile,
							  pHolder->gen.sourceline);

	/*
	 * Free up as much as we conveniently can of the placeholder structure.
	 * (This neglects any stack items, so it's possible for some memory to be
	 * leaked.  Since this can only happen once per session per variable, it
	 * doesn't seem worth spending much code on.)
	 */
	set_string_field(pHolder, pHolder->variable, NULL);
	set_string_field(pHolder, &pHolder->reset_val, NULL);

	guc_free(pHolder);
}

/*
 * Recursive subroutine for define_custom_variable: reapply non-reset values
 *
 * We recurse so that the values are applied in the same order as originally.
 * At each recursion level, apply the upper-level value (passed in) in the
 * fashion implied by the stack entry.
 */
static void
reapply_stacked_values(struct config_generic *variable,
					   struct config_string *pHolder,
					   GucStack *stack,
					   const char *curvalue,
					   GucContext curscontext, GucSource cursource,
					   Oid cursrole)
{
	const char *name = variable->name;
	GucStack   *oldvarstack = variable->stack;

	if (stack != NULL)
	{
		/* First, recurse, so that stack items are processed bottom to top */
		reapply_stacked_values(variable, pHolder, stack->prev,
							   stack->prior.val.stringval,
							   stack->scontext, stack->source, stack->srole);

		/* See how to apply the passed-in value */
		switch (stack->state)
		{
			case GUC_SAVE:
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_SAVE, true,
											 WARNING, false);
				break;

			case GUC_SET:
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_SET, true,
											 WARNING, false);
				break;

			case GUC_LOCAL:
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_LOCAL, true,
											 WARNING, false);
				break;

			case GUC_SET_LOCAL:
				/* first, apply the masked value as SET */
				(void) set_config_option_ext(name, stack->masked.val.stringval,
											 stack->masked_scontext,
											 PGC_S_SESSION,
											 stack->masked_srole,
											 GUC_ACTION_SET, true,
											 WARNING, false);
				/* then apply the current value as LOCAL */
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_LOCAL, true,
											 WARNING, false);
				break;
		}

		/* If we successfully made a stack entry, adjust its nest level */
		if (variable->stack != oldvarstack)
			variable->stack->nest_level = stack->nest_level;
	}
	else
	{
		/*
		 * We are at the end of the stack.  If the active/previous value is
		 * different from the reset value, it must represent a previously
		 * committed session value.  Apply it, and then drop the stack entry
		 * that set_config_option will have created under the impression that
		 * this is to be just a transactional assignment.  (We leak the stack
		 * entry.)
		 */
		if (curvalue != pHolder->reset_val ||
			curscontext != pHolder->gen.reset_scontext ||
			cursource != pHolder->gen.reset_source ||
			cursrole != pHolder->gen.reset_srole)
		{
			(void) set_config_option_ext(name, curvalue,
										 curscontext, cursource, cursrole,
										 GUC_ACTION_SET, true, WARNING, false);
			if (variable->stack != NULL)
			{
				slist_delete(&guc_stack_list, &variable->stack_link);
				variable->stack = NULL;
			}
		}
	}
}

/*
 * Functions for extensions to call to define their custom GUC variables.
 */
void
DefineCustomBoolVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 bool *valueAddr,
						 bool bootValue,
						 GucContext context,
						 int flags,
						 GucBoolCheckHook check_hook,
						 GucBoolAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_bool *var;

	var = (struct config_bool *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_BOOL, sizeof(struct config_bool));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomIntVariable(const char *name,
						const char *short_desc,
						const char *long_desc,
						int *valueAddr,
						int bootValue,
						int minValue,
						int maxValue,
						GucContext context,
						int flags,
						GucIntCheckHook check_hook,
						GucIntAssignHook assign_hook,
						GucShowHook show_hook)
{
	struct config_int *var;

	var = (struct config_int *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_INT, sizeof(struct config_int));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->min = minValue;
	var->max = maxValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomRealVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 double *valueAddr,
						 double bootValue,
						 double minValue,
						 double maxValue,
						 GucContext context,
						 int flags,
						 GucRealCheckHook check_hook,
						 GucRealAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_real *var;

	var = (struct config_real *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_REAL, sizeof(struct config_real));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->min = minValue;
	var->max = maxValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomStringVariable(const char *name,
						   const char *short_desc,
						   const char *long_desc,
						   char **valueAddr,
						   const char *bootValue,
						   GucContext context,
						   int flags,
						   GucStringCheckHook check_hook,
						   GucStringAssignHook assign_hook,
						   GucShowHook show_hook)
{
	struct config_string *var;

	var = (struct config_string *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_STRING, sizeof(struct config_string));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomEnumVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 int *valueAddr,
						 int bootValue,
						 const struct config_enum_entry *options,
						 GucContext context,
						 int flags,
						 GucEnumCheckHook check_hook,
						 GucEnumAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_enum *var;

	var = (struct config_enum *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_ENUM, sizeof(struct config_enum));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->options = options;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

/*
 * Mark the given GUC prefix as "reserved".
 *
 * This deletes any existing placeholders matching the prefix,
 * and then prevents new ones from being created.
 * Extensions should call this after they've defined all of their custom
 * GUCs, to help catch misspelled config-file entries.
 */
void
MarkGUCPrefixReserved(const char *className)
{
	int			classLen = strlen(className);
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;
	MemoryContext oldcontext;

	/*
	 * Check for existing placeholders.  We must actually remove invalid
	 * placeholders, else future parallel worker startups will fail.  (We
	 * don't bother trying to free associated memory, since this shouldn't
	 * happen often.)
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *var = hentry->gucvar;

		if ((var->flags & GUC_CUSTOM_PLACEHOLDER) != 0 &&
			strncmp(className, var->name, classLen) == 0 &&
			var->name[classLen] == GUC_QUALIFIER_SEPARATOR)
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("invalid configuration parameter name \"%s\", removing it",
							var->name),
					 errdetail("\"%s\" is now a reserved prefix.",
							   className)));
			/* Remove it from the hash table */
			hash_search(guc_hashtab,
						&var->name,
						HASH_REMOVE,
						NULL);
			/* Remove it from any lists it's in, too */
			RemoveGUCFromLists(var);
		}
	}

	/* And remember the name so we can prevent future mistakes. */
	oldcontext = MemoryContextSwitchTo(GUCMemoryContext);
	reserved_class_prefix = lappend(reserved_class_prefix, pstrdup(className));
	MemoryContextSwitchTo(oldcontext);
}


/*
 * Return an array of modified GUC options to show in EXPLAIN.
 *
 * We only report options related to query planning (marked with GUC_EXPLAIN),
 * with values different from their built-in defaults.
 */
struct config_generic **
get_explain_guc_options(int *num)
{
	struct config_generic **result;
	dlist_iter	iter;

	*num = 0;

	/*
	 * While only a fraction of all the GUC variables are marked GUC_EXPLAIN,
	 * it doesn't seem worth dynamically resizing this array.
	 */
	result = palloc(sizeof(struct config_generic *) * hash_get_num_entries(guc_hashtab));

	/* We need only consider GUCs with source not PGC_S_DEFAULT */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *conf = dlist_container(struct config_generic,
													  nondef_link, iter.cur);
		bool		modified;

		/* return only parameters marked for inclusion in explain */
		if (!(conf->flags & GUC_EXPLAIN))
			continue;

		/* return only options visible to the current user */
		if (!ConfigOptionIsVisible(conf))
			continue;

		/* return only options that are different from their boot values */
		modified = false;

		switch (conf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *lconf = (struct config_bool *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			case PGC_INT:
				{
					struct config_int *lconf = (struct config_int *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			case PGC_REAL:
				{
					struct config_real *lconf = (struct config_real *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			case PGC_STRING:
				{
					struct config_string *lconf = (struct config_string *) conf;

					if (lconf->boot_val == NULL &&
						*lconf->variable == NULL)
						modified = false;
					else if (lconf->boot_val == NULL ||
							 *lconf->variable == NULL)
						modified = true;
					else
						modified = (strcmp(lconf->boot_val, *(lconf->variable)) != 0);
				}
				break;

			case PGC_ENUM:
				{
					struct config_enum *lconf = (struct config_enum *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			default:
				elog(ERROR, "unexpected GUC type: %d", conf->vartype);
		}

		if (!modified)
			continue;

		/* OK, report it */
		result[*num] = conf;
		*num = *num + 1;
	}

	return result;
}

/*
 * Return GUC variable value by name; optionally return canonical form of
 * name.  If the GUC is unset, then throw an error unless missing_ok is true,
 * in which case return NULL.  Return value is palloc'd (but *varname isn't).
 */
char *
GetConfigOptionByName(const char *name, const char **varname, bool missing_ok)
{
	struct config_generic *record;

	record = find_option(name, false, missing_ok, ERROR);
	if (record == NULL)
	{
		if (varname)
			*varname = NULL;
		return NULL;
	}

	if (!ConfigOptionIsVisible(record))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to examine \"%s\"", name),
				 errdetail("Only roles with privileges of the \"%s\" role may examine this parameter.",
						   "pg_read_all_settings")));

	if (varname)
		*varname = record->name;

	return ShowGUCOption(record, true);
}

/*
 * ShowGUCOption: get string value of variable
 *
 * We express a numeric value in appropriate units if it has units and
 * use_units is true; else you just get the raw number.
 * The result string is palloc'd.
 */
char *
ShowGUCOption(struct config_generic *record, bool use_units)
{
	char		buffer[256];
	const char *val;

	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
					val = *conf->variable ? "on" : "off";
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
				{
					/*
					 * Use int64 arithmetic to avoid overflows in units
					 * conversion.
					 */
					int64		result = *conf->variable;
					const char *unit;

					if (use_units && result > 0 && (record->flags & GUC_UNIT))
						convert_int_from_base_unit(result,
												   record->flags & GUC_UNIT,
												   &result, &unit);
					else
						unit = "";

					snprintf(buffer, sizeof(buffer), INT64_FORMAT "%s",
							 result, unit);
					val = buffer;
				}
			}
			break;

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
				{
					double		result = *conf->variable;
					const char *unit;

					if (use_units && result > 0 && (record->flags & GUC_UNIT))
						convert_real_from_base_unit(result,
													record->flags & GUC_UNIT,
													&result, &unit);
					else
						unit = "";

					snprintf(buffer, sizeof(buffer), "%g%s",
							 result, unit);
					val = buffer;
				}
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else if (*conf->variable && **conf->variable)
					val = *conf->variable;
				else
					val = "";
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
					val = config_enum_lookup_by_value(conf, *conf->variable);
			}
			break;

		default:
			/* just to keep compiler quiet */
			val = "???";
			break;
	}

	return pstrdup(val);
}


#ifdef EXEC_BACKEND

/*
 *	These routines dump out all non-default GUC options into a binary
 *	file that is read by all exec'ed backends.  The format is:
 *
 *		variable name, string, null terminated
 *		variable value, string, null terminated
 *		variable sourcefile, string, null terminated (empty if none)
 *		variable sourceline, integer
 *		variable source, integer
 *		variable scontext, integer
*		variable srole, OID
 */
static void
write_one_nondefault_variable(FILE *fp, struct config_generic *gconf)
{
	Assert(gconf->source != PGC_S_DEFAULT);

	fprintf(fp, "%s", gconf->name);
	fputc(0, fp);

	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;

				if (*conf->variable)
					fprintf(fp, "true");
				else
					fprintf(fp, "false");
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				fprintf(fp, "%d", *conf->variable);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;

				fprintf(fp, "%.17g", *conf->variable);
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				if (*conf->variable)
					fprintf(fp, "%s", *conf->variable);
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				fprintf(fp, "%s",
						config_enum_lookup_by_value(conf, *conf->variable));
			}
			break;
	}

	fputc(0, fp);

	if (gconf->sourcefile)
		fprintf(fp, "%s", gconf->sourcefile);
	fputc(0, fp);

	fwrite(&gconf->sourceline, 1, sizeof(gconf->sourceline), fp);
	fwrite(&gconf->source, 1, sizeof(gconf->source), fp);
	fwrite(&gconf->scontext, 1, sizeof(gconf->scontext), fp);
	fwrite(&gconf->srole, 1, sizeof(gconf->srole), fp);
}

void
write_nondefault_variables(GucContext context)
{
	int			elevel;
	FILE	   *fp;
	dlist_iter	iter;

	Assert(context == PGC_POSTMASTER || context == PGC_SIGHUP);

	elevel = (context == PGC_SIGHUP) ? LOG : ERROR;

	/*
	 * Open file
	 */
	fp = AllocateFile(CONFIG_EXEC_PARAMS_NEW, "w");
	if (!fp)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						CONFIG_EXEC_PARAMS_NEW)));
		return;
	}

	/* We need only consider GUCs with source not PGC_S_DEFAULT */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		write_one_nondefault_variable(fp, gconf);
	}

	if (FreeFile(fp))
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						CONFIG_EXEC_PARAMS_NEW)));
		return;
	}

	/*
	 * Put new file in place.  This could delay on Win32, but we don't hold
	 * any exclusive locks.
	 */
	rename(CONFIG_EXEC_PARAMS_NEW, CONFIG_EXEC_PARAMS);
}


/*
 *	Read string, including null byte from file
 *
 *	Return NULL on EOF and nothing read
 */
static char *
read_string_with_null(FILE *fp)
{
	int			i = 0,
				ch,
				maxlen = 256;
	char	   *str = NULL;

	do
	{
		if ((ch = fgetc(fp)) == EOF)
		{
			if (i == 0)
				return NULL;
			else
				elog(FATAL, "invalid format of exec config params file");
		}
		if (i == 0)
			str = guc_malloc(FATAL, maxlen);
		else if (i == maxlen)
			str = guc_realloc(FATAL, str, maxlen *= 2);
		str[i++] = ch;
	} while (ch != 0);

	return str;
}


/*
 *	This routine loads a previous postmaster dump of its non-default
 *	settings.
 */
void
read_nondefault_variables(void)
{
	FILE	   *fp;
	char	   *varname,
			   *varvalue,
			   *varsourcefile;
	int			varsourceline;
	GucSource	varsource;
	GucContext	varscontext;
	Oid			varsrole;

	/*
	 * Open file
	 */
	fp = AllocateFile(CONFIG_EXEC_PARAMS, "r");
	if (!fp)
	{
		/* File not found is fine */
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": %m",
							CONFIG_EXEC_PARAMS)));
		return;
	}

	for (;;)
	{
		if ((varname = read_string_with_null(fp)) == NULL)
			break;

		if (find_option(varname, true, false, FATAL) == NULL)
			elog(FATAL, "failed to locate variable \"%s\" in exec config params file", varname);

		if ((varvalue = read_string_with_null(fp)) == NULL)
			elog(FATAL, "invalid format of exec config params file");
		if ((varsourcefile = read_string_with_null(fp)) == NULL)
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsourceline, 1, sizeof(varsourceline), fp) != sizeof(varsourceline))
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsource, 1, sizeof(varsource), fp) != sizeof(varsource))
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varscontext, 1, sizeof(varscontext), fp) != sizeof(varscontext))
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsrole, 1, sizeof(varsrole), fp) != sizeof(varsrole))
			elog(FATAL, "invalid format of exec config params file");

		(void) set_config_option_ext(varname, varvalue,
									 varscontext, varsource, varsrole,
									 GUC_ACTION_SET, true, 0, true);
		if (varsourcefile[0])
			set_config_sourcefile(varname, varsourcefile, varsourceline);

		guc_free(varname);
		guc_free(varvalue);
		guc_free(varsourcefile);
	}

	FreeFile(fp);
}
#endif							/* EXEC_BACKEND */

/*
 * can_skip_gucvar:
 * Decide whether SerializeGUCState can skip sending this GUC variable,
 * or whether RestoreGUCState can skip resetting this GUC to default.
 *
 * It is somewhat magical and fragile that the same test works for both cases.
 * Realize in particular that we are very likely selecting different sets of
 * GUCs on the leader and worker sides!  Be sure you've understood the
 * comments here and in RestoreGUCState thoroughly before changing this.
 */
static bool
can_skip_gucvar(struct config_generic *gconf)
{
	/*
	 * We can skip GUCs that are guaranteed to have the same values in leaders
	 * and workers.  (Note it is critical that the leader and worker have the
	 * same idea of which GUCs fall into this category.  It's okay to consider
	 * context and name for this purpose, since those are unchanging
	 * properties of a GUC.)
	 *
	 * PGC_POSTMASTER variables always have the same value in every child of a
	 * particular postmaster, so the worker will certainly have the right
	 * value already.  Likewise, PGC_INTERNAL variables are set by special
	 * mechanisms (if indeed they aren't compile-time constants).  So we may
	 * always skip these.
	 *
	 * For all other GUCs, we skip if the GUC has its compiled-in default
	 * value (i.e., source == PGC_S_DEFAULT).  On the leader side, this means
	 * we don't send GUCs that have their default values, which typically
	 * saves lots of work.  On the worker side, this means we don't need to
	 * reset the GUC to default because it already has that value.  See
	 * comments in RestoreGUCState for more info.
	 */
	return gconf->context == PGC_POSTMASTER ||
		gconf->context == PGC_INTERNAL ||
		gconf->source == PGC_S_DEFAULT;
}

/*
 * estimate_variable_size:
 *		Compute space needed for dumping the given GUC variable.
 *
 * It's OK to overestimate, but not to underestimate.
 */
static Size
estimate_variable_size(struct config_generic *gconf)
{
	Size		size;
	Size		valsize = 0;

	/* Skippable GUCs consume zero space. */
	if (can_skip_gucvar(gconf))
		return 0;

	/* Name, plus trailing zero byte. */
	size = strlen(gconf->name) + 1;

	/* Get the maximum display length of the GUC value. */
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				valsize = 5;	/* max(strlen('true'), strlen('false')) */
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				/*
				 * Instead of getting the exact display length, use max
				 * length.  Also reduce the max length for typical ranges of
				 * small values.  Maximum value is 2147483647, i.e. 10 chars.
				 * Include one byte for sign.
				 */
				if (abs(*conf->variable) < 1000)
					valsize = 3 + 1;
				else
					valsize = 10 + 1;
			}
			break;

		case PGC_REAL:
			{
				/*
				 * We are going to print it with %e with REALTYPE_PRECISION
				 * fractional digits.  Account for sign, leading digit,
				 * decimal point, and exponent with up to 3 digits.  E.g.
				 * -3.99329042340000021e+110
				 */
				valsize = 1 + 1 + 1 + REALTYPE_PRECISION + 5;
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				/*
				 * If the value is NULL, we transmit it as an empty string.
				 * Although this is not physically the same value, GUC
				 * generally treats a NULL the same as empty string.
				 */
				if (*conf->variable)
					valsize = strlen(*conf->variable);
				else
					valsize = 0;
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				valsize = strlen(config_enum_lookup_by_value(conf, *conf->variable));
			}
			break;
	}

	/* Allow space for terminating zero-byte for value */
	size = add_size(size, valsize + 1);

	if (gconf->sourcefile)
		size = add_size(size, strlen(gconf->sourcefile));

	/* Allow space for terminating zero-byte for sourcefile */
	size = add_size(size, 1);

	/* Include line whenever file is nonempty. */
	if (gconf->sourcefile && gconf->sourcefile[0])
		size = add_size(size, sizeof(gconf->sourceline));

	size = add_size(size, sizeof(gconf->source));
	size = add_size(size, sizeof(gconf->scontext));
	size = add_size(size, sizeof(gconf->srole));

	return size;
}

/*
 * EstimateGUCStateSpace:
 * Returns the size needed to store the GUC state for the current process
 */
Size
EstimateGUCStateSpace(void)
{
	Size		size;
	dlist_iter	iter;

	/* Add space reqd for saving the data size of the guc state */
	size = sizeof(Size);

	/*
	 * Add up the space needed for each GUC variable.
	 *
	 * We need only process non-default GUCs.
	 */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		size = add_size(size, estimate_variable_size(gconf));
	}

	return size;
}

/*
 * do_serialize:
 * Copies the formatted string into the destination.  Moves ahead the
 * destination pointer, and decrements the maxbytes by that many bytes. If
 * maxbytes is not sufficient to copy the string, error out.
 */
static void
do_serialize(char **destptr, Size *maxbytes, const char *fmt,...)
{
	va_list		vargs;
	int			n;

	if (*maxbytes <= 0)
		elog(ERROR, "not enough space to serialize GUC state");

	va_start(vargs, fmt);
	n = vsnprintf(*destptr, *maxbytes, fmt, vargs);
	va_end(vargs);

	if (n < 0)
	{
		/* Shouldn't happen. Better show errno description. */
		elog(ERROR, "vsnprintf failed: %m with format string \"%s\"", fmt);
	}
	if (n >= *maxbytes)
	{
		/* This shouldn't happen either, really. */
		elog(ERROR, "not enough space to serialize GUC state");
	}

	/* Shift the destptr ahead of the null terminator */
	*destptr += n + 1;
	*maxbytes -= n + 1;
}

/* Binary copy version of do_serialize() */
static void
do_serialize_binary(char **destptr, Size *maxbytes, void *val, Size valsize)
{
	if (valsize > *maxbytes)
		elog(ERROR, "not enough space to serialize GUC state");

	memcpy(*destptr, val, valsize);
	*destptr += valsize;
	*maxbytes -= valsize;
}

/*
 * serialize_variable:
 * Dumps name, value and other information of a GUC variable into destptr.
 */
static void
serialize_variable(char **destptr, Size *maxbytes,
				   struct config_generic *gconf)
{
	/* Ignore skippable GUCs. */
	if (can_skip_gucvar(gconf))
		return;

	do_serialize(destptr, maxbytes, "%s", gconf->name);

	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;

				do_serialize(destptr, maxbytes,
							 (*conf->variable ? "true" : "false"));
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				do_serialize(destptr, maxbytes, "%d", *conf->variable);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;

				do_serialize(destptr, maxbytes, "%.*e",
							 REALTYPE_PRECISION, *conf->variable);
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				/* NULL becomes empty string, see estimate_variable_size() */
				do_serialize(destptr, maxbytes, "%s",
							 *conf->variable ? *conf->variable : "");
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				do_serialize(destptr, maxbytes, "%s",
							 config_enum_lookup_by_value(conf, *conf->variable));
			}
			break;
	}

	do_serialize(destptr, maxbytes, "%s",
				 (gconf->sourcefile ? gconf->sourcefile : ""));

	if (gconf->sourcefile && gconf->sourcefile[0])
		do_serialize_binary(destptr, maxbytes, &gconf->sourceline,
							sizeof(gconf->sourceline));

	do_serialize_binary(destptr, maxbytes, &gconf->source,
						sizeof(gconf->source));
	do_serialize_binary(destptr, maxbytes, &gconf->scontext,
						sizeof(gconf->scontext));
	do_serialize_binary(destptr, maxbytes, &gconf->srole,
						sizeof(gconf->srole));
}

/*
 * SerializeGUCState:
 * Dumps the complete GUC state onto the memory location at start_address.
 */
void
SerializeGUCState(Size maxsize, char *start_address)
{
	char	   *curptr;
	Size		actual_size;
	Size		bytes_left;
	dlist_iter	iter;

	/* Reserve space for saving the actual size of the guc state */
	Assert(maxsize > sizeof(actual_size));
	curptr = start_address + sizeof(actual_size);
	bytes_left = maxsize - sizeof(actual_size);

	/* We need only consider GUCs with source not PGC_S_DEFAULT */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		serialize_variable(&curptr, &bytes_left, gconf);
	}

	/* Store actual size without assuming alignment of start_address. */
	actual_size = maxsize - bytes_left - sizeof(actual_size);
	memcpy(start_address, &actual_size, sizeof(actual_size));
}

/*
 * read_gucstate:
 * Actually it does not read anything, just returns the srcptr. But it does
 * move the srcptr past the terminating zero byte, so that the caller is ready
 * to read the next string.
 */
static char *
read_gucstate(char **srcptr, char *srcend)
{
	char	   *retptr = *srcptr;
	char	   *ptr;

	if (*srcptr >= srcend)
		elog(ERROR, "incomplete GUC state");

	/* The string variables are all null terminated */
	for (ptr = *srcptr; ptr < srcend && *ptr != '\0'; ptr++)
		;

	if (ptr >= srcend)
		elog(ERROR, "could not find null terminator in GUC state");

	/* Set the new position to the byte following the terminating NUL */
	*srcptr = ptr + 1;

	return retptr;
}

/* Binary read version of read_gucstate(). Copies into dest */
static void
read_gucstate_binary(char **srcptr, char *srcend, void *dest, Size size)
{
	if (*srcptr + size > srcend)
		elog(ERROR, "incomplete GUC state");

	memcpy(dest, *srcptr, size);
	*srcptr += size;
}

/*
 * Callback used to add a context message when reporting errors that occur
 * while trying to restore GUCs in parallel workers.
 */
static void
guc_restore_error_context_callback(void *arg)
{
	char	  **error_context_name_and_value = (char **) arg;

	if (error_context_name_and_value)
		errcontext("while setting parameter \"%s\" to \"%s\"",
				   error_context_name_and_value[0],
				   error_context_name_and_value[1]);
}

/*
 * RestoreGUCState:
 * Reads the GUC state at the specified address and sets this process's
 * GUCs to match.
 *
 * Note that this provides the worker with only a very shallow view of the
 * leader's GUC state: we'll know about the currently active values, but not
 * about stacked or reset values.  That's fine since the worker is just
 * executing one part of a query, within which the active values won't change
 * and the stacked values are invisible.
 */
void
RestoreGUCState(void *gucstate)
{
	char	   *varname,
			   *varvalue,
			   *varsourcefile;
	int			varsourceline;
	GucSource	varsource;
	GucContext	varscontext;
	Oid			varsrole;
	char	   *srcptr = (char *) gucstate;
	char	   *srcend;
	Size		len;
	dlist_mutable_iter iter;
	ErrorContextCallback error_context_callback;

	/*
	 * First, ensure that all potentially-shippable GUCs are reset to their
	 * default values.  We must not touch those GUCs that the leader will
	 * never ship, while there is no need to touch those that are shippable
	 * but already have their default values.  Thus, this ends up being the
	 * same test that SerializeGUCState uses, even though the sets of
	 * variables involved may well be different since the leader's set of
	 * variables-not-at-default-values can differ from the set that are
	 * not-default in this freshly started worker.
	 *
	 * Once we have set all the potentially-shippable GUCs to default values,
	 * restoring the GUCs that the leader sent (because they had non-default
	 * values over there) leads us to exactly the set of GUC values that the
	 * leader has.  This is true even though the worker may have initially
	 * absorbed postgresql.conf settings that the leader hasn't yet seen, or
	 * ALTER USER/DATABASE SET settings that were established after the leader
	 * started.
	 *
	 * Note that ensuring all the potential target GUCs are at PGC_S_DEFAULT
	 * also ensures that set_config_option won't refuse to set them because of
	 * source-priority comparisons.
	 */
	dlist_foreach_modify(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		/* Do nothing if non-shippable or if already at PGC_S_DEFAULT. */
		if (can_skip_gucvar(gconf))
			continue;

		/*
		 * We can use InitializeOneGUCOption to reset the GUC to default, but
		 * first we must free any existing subsidiary data to avoid leaking
		 * memory.  The stack must be empty, but we have to clean up all other
		 * fields.  Beware that there might be duplicate value or "extra"
		 * pointers.  We also have to be sure to take it out of any lists it's
		 * in.
		 */
		Assert(gconf->stack == NULL);
		guc_free(gconf->extra);
		guc_free(gconf->last_reported);
		guc_free(gconf->sourcefile);
		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;

					guc_free(*conf->variable);
					if (conf->reset_val && conf->reset_val != *conf->variable)
						guc_free(conf->reset_val);
					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_ENUM:
				{
					struct config_enum *conf = (struct config_enum *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
		}
		/* Remove it from any lists it's in. */
		RemoveGUCFromLists(gconf);
		/* Now we can reset the struct to PGS_S_DEFAULT state. */
		InitializeOneGUCOption(gconf);
	}

	/* First item is the length of the subsequent data */
	memcpy(&len, gucstate, sizeof(len));

	srcptr += sizeof(len);
	srcend = srcptr + len;

	/* If the GUC value check fails, we want errors to show useful context. */
	error_context_callback.callback = guc_restore_error_context_callback;
	error_context_callback.previous = error_context_stack;
	error_context_callback.arg = NULL;
	error_context_stack = &error_context_callback;

	/* Restore all the listed GUCs. */
	while (srcptr < srcend)
	{
		int			result;
		char	   *error_context_name_and_value[2];

		varname = read_gucstate(&srcptr, srcend);
		varvalue = read_gucstate(&srcptr, srcend);
		varsourcefile = read_gucstate(&srcptr, srcend);
		if (varsourcefile[0])
			read_gucstate_binary(&srcptr, srcend,
								 &varsourceline, sizeof(varsourceline));
		else
			varsourceline = 0;
		read_gucstate_binary(&srcptr, srcend,
							 &varsource, sizeof(varsource));
		read_gucstate_binary(&srcptr, srcend,
							 &varscontext, sizeof(varscontext));
		read_gucstate_binary(&srcptr, srcend,
							 &varsrole, sizeof(varsrole));

		error_context_name_and_value[0] = varname;
		error_context_name_and_value[1] = varvalue;
		error_context_callback.arg = &error_context_name_and_value[0];
		result = set_config_option_ext(varname, varvalue,
									   varscontext, varsource, varsrole,
									   GUC_ACTION_SET, true, ERROR, true);
		if (result <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("parameter \"%s\" could not be set", varname)));
		if (varsourcefile[0])
			set_config_sourcefile(varname, varsourcefile, varsourceline);
		error_context_callback.arg = NULL;
	}

	error_context_stack = error_context_callback.previous;
}

/*
 * A little "long argument" simulation, although not quite GNU
 * compliant. Takes a string of the form "some-option=some value" and
 * returns name = "some_option" and value = "some value" in palloc'ed
 * storage. Note that '-' is converted to '_' in the option name. If
 * there is no '=' in the input string then value will be NULL.
 */
void
ParseLongOption(const char *string, char **name, char **value)
{
	size_t		equal_pos;
	char	   *cp;

	Assert(string);
	Assert(name);
	Assert(value);

	equal_pos = strcspn(string, "=");

	if (string[equal_pos] == '=')
	{
		*name = palloc(equal_pos + 1);
		strlcpy(*name, string, equal_pos + 1);

		*value = pstrdup(&string[equal_pos + 1]);
	}
	else
	{
		/* no equal sign in string */
		*name = pstrdup(string);
		*value = NULL;
	}

	for (cp = *name; *cp; cp++)
		if (*cp == '-')
			*cp = '_';
}


/*
 * Transform array of GUC settings into lists of names and values. The lists
 * are faster to process in cases where the settings must be applied
 * repeatedly (e.g. for each function invocation).
 */
void
TransformGUCArray(ArrayType *array, List **names, List **values)
{
	int			i;

	Assert(array != NULL);
	Assert(ARR_ELEMTYPE(array) == TEXTOID);
	Assert(ARR_NDIM(array) == 1);
	Assert(ARR_LBOUND(array)[0] == 1);

	*names = NIL;
	*values = NIL;
	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		bool		isnull;
		char	   *s;
		char	   *name;
		char	   *value;

		d = array_ref(array, 1, &i,
					  -1 /* varlenarray */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  TYPALIGN_INT /* TEXT's typalign */ ,
					  &isnull);

		if (isnull)
			continue;

		s = TextDatumGetCString(d);

		ParseLongOption(s, &name, &value);
		if (!value)
		{
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("could not parse setting for parameter \"%s\"",
							name)));
			pfree(name);
			continue;
		}

		*names = lappend(*names, name);
		*values = lappend(*values, value);

		pfree(s);
	}
}


/*
 * Handle options fetched from pg_db_role_setting.setconfig,
 * pg_proc.proconfig, etc.  Caller must specify proper context/source/action.
 *
 * The array parameter must be an array of TEXT (it must not be NULL).
 */
void
ProcessGUCArray(ArrayType *array,
				GucContext context, GucSource source, GucAction action)
{
	List	   *gucNames;
	List	   *gucValues;
	ListCell   *lc1;
	ListCell   *lc2;

	TransformGUCArray(array, &gucNames, &gucValues);
	forboth(lc1, gucNames, lc2, gucValues)
	{
		char	   *name = lfirst(lc1);
		char	   *value = lfirst(lc2);

		(void) set_config_option(name, value,
								 context, source,
								 action, true, 0, false);

		pfree(name);
		pfree(value);
	}

	list_free(gucNames);
	list_free(gucValues);
}


/*
 * Add an entry to an option array.  The array parameter may be NULL
 * to indicate the current table entry is NULL.
 */
ArrayType *
GUCArrayAdd(ArrayType *array, const char *name, const char *value)
{
	struct config_generic *record;
	Datum		datum;
	char	   *newval;
	ArrayType  *a;

	Assert(name);
	Assert(value);

	/* test if the option is valid and we're allowed to set it */
	(void) validate_option_array_item(name, value, false);

	/* normalize name (converts obsolete GUC names to modern spellings) */
	record = find_option(name, false, true, WARNING);
	if (record)
		name = record->name;

	/* build new item for array */
	newval = psprintf("%s=%s", name, value);
	datum = CStringGetTextDatum(newval);

	if (array)
	{
		int			index;
		bool		isnull;
		int			i;

		Assert(ARR_ELEMTYPE(array) == TEXTOID);
		Assert(ARR_NDIM(array) == 1);
		Assert(ARR_LBOUND(array)[0] == 1);

		index = ARR_DIMS(array)[0] + 1; /* add after end */

		for (i = 1; i <= ARR_DIMS(array)[0]; i++)
		{
			Datum		d;
			char	   *current;

			d = array_ref(array, 1, &i,
						  -1 /* varlenarray */ ,
						  -1 /* TEXT's typlen */ ,
						  false /* TEXT's typbyval */ ,
						  TYPALIGN_INT /* TEXT's typalign */ ,
						  &isnull);
			if (isnull)
				continue;
			current = TextDatumGetCString(d);

			/* check for match up through and including '=' */
			if (strncmp(current, newval, strlen(name) + 1) == 0)
			{
				index = i;
				break;
			}
		}

		a = array_set(array, 1, &index,
					  datum,
					  false,
					  -1 /* varlena array */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  TYPALIGN_INT /* TEXT's typalign */ );
	}
	else
		a = construct_array_builtin(&datum, 1, TEXTOID);

	return a;
}


/*
 * Delete an entry from an option array.  The array parameter may be NULL
 * to indicate the current table entry is NULL.  Also, if the return value
 * is NULL then a null should be stored.
 */
ArrayType *
GUCArrayDelete(ArrayType *array, const char *name)
{
	struct config_generic *record;
	ArrayType  *newarray;
	int			i;
	int			index;

	Assert(name);

	/* test if the option is valid and we're allowed to set it */
	(void) validate_option_array_item(name, NULL, false);

	/* normalize name (converts obsolete GUC names to modern spellings) */
	record = find_option(name, false, true, WARNING);
	if (record)
		name = record->name;

	/* if array is currently null, then surely nothing to delete */
	if (!array)
		return NULL;

	newarray = NULL;
	index = 1;

	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		char	   *val;
		bool		isnull;

		d = array_ref(array, 1, &i,
					  -1 /* varlenarray */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  TYPALIGN_INT /* TEXT's typalign */ ,
					  &isnull);
		if (isnull)
			continue;
		val = TextDatumGetCString(d);

		/* ignore entry if it's what we want to delete */
		if (strncmp(val, name, strlen(name)) == 0
			&& val[strlen(name)] == '=')
			continue;

		/* else add it to the output array */
		if (newarray)
			newarray = array_set(newarray, 1, &index,
								 d,
								 false,
								 -1 /* varlenarray */ ,
								 -1 /* TEXT's typlen */ ,
								 false /* TEXT's typbyval */ ,
								 TYPALIGN_INT /* TEXT's typalign */ );
		else
			newarray = construct_array_builtin(&d, 1, TEXTOID);

		index++;
	}

	return newarray;
}


/*
 * Given a GUC array, delete all settings from it that our permission
 * level allows: if superuser, delete them all; if regular user, only
 * those that are PGC_USERSET or we have permission to set
 */
ArrayType *
GUCArrayReset(ArrayType *array)
{
	ArrayType  *newarray;
	int			i;
	int			index;

	/* if array is currently null, nothing to do */
	if (!array)
		return NULL;

	/* if we're superuser, we can delete everything, so just do it */
	if (superuser())
		return NULL;

	newarray = NULL;
	index = 1;

	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		char	   *val;
		char	   *eqsgn;
		bool		isnull;

		d = array_ref(array, 1, &i,
					  -1 /* varlenarray */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  TYPALIGN_INT /* TEXT's typalign */ ,
					  &isnull);
		if (isnull)
			continue;
		val = TextDatumGetCString(d);

		eqsgn = strchr(val, '=');
		*eqsgn = '\0';

		/* skip if we have permission to delete it */
		if (validate_option_array_item(val, NULL, true))
			continue;

		/* else add it to the output array */
		if (newarray)
			newarray = array_set(newarray, 1, &index,
								 d,
								 false,
								 -1 /* varlenarray */ ,
								 -1 /* TEXT's typlen */ ,
								 false /* TEXT's typbyval */ ,
								 TYPALIGN_INT /* TEXT's typalign */ );
		else
			newarray = construct_array_builtin(&d, 1, TEXTOID);

		index++;
		pfree(val);
	}

	return newarray;
}

/*
 * Validate a proposed option setting for GUCArrayAdd/Delete/Reset.
 *
 * name is the option name.  value is the proposed value for the Add case,
 * or NULL for the Delete/Reset cases.  If skipIfNoPermissions is true, it's
 * not an error to have no permissions to set the option.
 *
 * Returns true if OK, false if skipIfNoPermissions is true and user does not
 * have permission to change this option (all other error cases result in an
 * error being thrown).
 */
static bool
validate_option_array_item(const char *name, const char *value,
						   bool skipIfNoPermissions)

{
	struct config_generic *gconf;

	/*
	 * There are three cases to consider:
	 *
	 * name is a known GUC variable.  Check the value normally, check
	 * permissions normally (i.e., allow if variable is USERSET, or if it's
	 * SUSET and user is superuser or holds ACL_SET permissions).
	 *
	 * name is not known, but exists or can be created as a placeholder (i.e.,
	 * it has a valid custom name).  We allow this case if you're a superuser,
	 * otherwise not.  Superusers are assumed to know what they're doing. We
	 * can't allow it for other users, because when the placeholder is
	 * resolved it might turn out to be a SUSET variable.  (With currently
	 * available infrastructure, we can actually handle such cases within the
	 * current session --- but once an entry is made in pg_db_role_setting,
	 * it's assumed to be fully validated.)
	 *
	 * name is not known and can't be created as a placeholder.  Throw error,
	 * unless skipIfNoPermissions is true, in which case return false.
	 */
	gconf = find_option(name, true, skipIfNoPermissions, ERROR);
	if (!gconf)
	{
		/* not known, failed to make a placeholder */
		return false;
	}

	if (gconf->flags & GUC_CUSTOM_PLACEHOLDER)
	{
		/*
		 * We cannot do any meaningful check on the value, so only permissions
		 * are useful to check.
		 */
		if (superuser() ||
			pg_parameter_aclcheck(name, GetUserId(), ACL_SET) == ACLCHECK_OK)
			return true;
		if (skipIfNoPermissions)
			return false;
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to set parameter \"%s\"", name)));
	}

	/* manual permissions check so we can avoid an error being thrown */
	if (gconf->context == PGC_USERSET)
		 /* ok */ ;
	else if (gconf->context == PGC_SUSET &&
			 (superuser() ||
			  pg_parameter_aclcheck(name, GetUserId(), ACL_SET) == ACLCHECK_OK))
		 /* ok */ ;
	else if (skipIfNoPermissions)
		return false;
	/* if a permissions error should be thrown, let set_config_option do it */

	/* test for permissions and valid option value */
	(void) set_config_option(name, value,
							 superuser() ? PGC_SUSET : PGC_USERSET,
							 PGC_S_TEST, GUC_ACTION_SET, false, 0, false);

	return true;
}


/*
 * Called by check_hooks that want to override the normal
 * ERRCODE_INVALID_PARAMETER_VALUE SQLSTATE for check hook failures.
 *
 * Note that GUC_check_errmsg() etc are just macros that result in a direct
 * assignment to the associated variables.  That is ugly, but forced by the
 * limitations of C's macro mechanisms.
 */
void
GUC_check_errcode(int sqlerrcode)
{
	GUC_check_errcode_value = sqlerrcode;
}


/*
 * Convenience functions to manage calling a variable's check_hook.
 * These mostly take care of the protocol for letting check hooks supply
 * portions of the error report on failure.
 */

static bool
call_bool_check_hook(struct config_bool *conf, bool *newval, void **extra,
					 GucSource source, int elevel)
{
	/* Quick success if no hook */
	if (!conf->check_hook)
		return true;

	/* Reset variables that might be set by hook */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": %d",
						conf->gen.name, (int) *newval),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* Flush any strings created in ErrorContext */
		FlushErrorState();
		return false;
	}

	return true;
}

static bool
call_int_check_hook(struct config_int *conf, int *newval, void **extra,
					GucSource source, int elevel)
{
	/* Quick success if no hook */
	if (!conf->check_hook)
		return true;

	/* Reset variables that might be set by hook */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": %d",
						conf->gen.name, *newval),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* Flush any strings created in ErrorContext */
		FlushErrorState();
		return false;
	}

	return true;
}

static bool
call_real_check_hook(struct config_real *conf, double *newval, void **extra,
					 GucSource source, int elevel)
{
	/* Quick success if no hook */
	if (!conf->check_hook)
		return true;

	/* Reset variables that might be set by hook */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": %g",
						conf->gen.name, *newval),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* Flush any strings created in ErrorContext */
		FlushErrorState();
		return false;
	}

	return true;
}

static bool
call_string_check_hook(struct config_string *conf, char **newval, void **extra,
					   GucSource source, int elevel)
{
	volatile bool result = true;

	/* Quick success if no hook */
	if (!conf->check_hook)
		return true;

	/*
	 * If elevel is ERROR, or if the check_hook itself throws an elog
	 * (undesirable, but not always avoidable), make sure we don't leak the
	 * already-malloc'd newval string.
	 */
	PG_TRY();
	{
		/* Reset variables that might be set by hook */
		GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
		GUC_check_errmsg_string = NULL;
		GUC_check_errdetail_string = NULL;
		GUC_check_errhint_string = NULL;

		if (!conf->check_hook(newval, extra, source))
		{
			ereport(elevel,
					(errcode(GUC_check_errcode_value),
					 GUC_check_errmsg_string ?
					 errmsg_internal("%s", GUC_check_errmsg_string) :
					 errmsg("invalid value for parameter \"%s\": \"%s\"",
							conf->gen.name, *newval ? *newval : ""),
					 GUC_check_errdetail_string ?
					 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
					 GUC_check_errhint_string ?
					 errhint("%s", GUC_check_errhint_string) : 0));
			/* Flush any strings created in ErrorContext */
			FlushErrorState();
			result = false;
		}
	}
	PG_CATCH();
	{
		guc_free(*newval);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}

static bool
call_enum_check_hook(struct config_enum *conf, int *newval, void **extra,
					 GucSource source, int elevel)
{
	/* Quick success if no hook */
	if (!conf->check_hook)
		return true;

	/* Reset variables that might be set by hook */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": \"%s\"",
						conf->gen.name,
						config_enum_lookup_by_value(conf, *newval)),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* Flush any strings created in ErrorContext */
		FlushErrorState();
		return false;
	}

	return true;
}
