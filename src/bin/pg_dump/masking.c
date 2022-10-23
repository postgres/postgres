/*-------------------------------------------------------------------------
 *
 * masking.c
 *
 * Data masking tool for pg_dump
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/masking.c
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include "masking.h"
#include "common/logging.h"

#define REL_SIZE 65 /* Length of relation name - 63 bytes (symbols) + addition symbol for correct work with option --quote-all-identifiers*/
#define DEFAULT_NAME "default"
#define COL_WITH_FUNC_SIZE 3 * REL_SIZE + 3 /* schema_name.function name + '(' + column_name + ') */

char REL_SEP = '.'; /* Relation separator */

void concatFunctionAndColumn(char *col_with_func, char *schema_name, char *column_name, char *function_name);
extern void extractFuncNameIfPath(char *func_path, SimpleStringList *masking_func_query_path);
int extractFunctionNameFromQueryFile(char *filename, char *func_name);
char *getFullRelName(char *schema_name, char *table_name, char *column_name);
int getMapIndexByKey(MaskingMap *map, char *key);
bool isTerminal(char c);
bool isSpace(char c);
void printParsingError(struct MaskingDebugDetails *md, char *message, char current_symbol);
char readName(char *rel_name, char c, struct MaskingDebugDetails *md, FILE *fin, int size);
char readNextSymbol(struct MaskingDebugDetails *md, FILE *fin);
void removeQuotes(char *func_name);
char *readWord(FILE *fin, char *word);
void setMapValue(MaskingMap *map, char *key, char *value);
char skipMultiLineComment(char c, struct MaskingDebugDetails *md, FILE *fin);
char skipOneLineComment(char c, struct MaskingDebugDetails *md, FILE *fin);

/* Initialise masking map */
MaskingMap *
newMaskingMap()
{
    MaskingMap *map = malloc(sizeof(MaskingMap));
    map->size = 0;
    map->capacity = 8;
    map->data = malloc(sizeof(Pair) * map->capacity);
    memset(map->data, 0, sizeof(Pair) * map->capacity);
    return map;
}

int
getMapIndexByKey(MaskingMap *map, char *key)
{
    int index = 0;
    while (map->data[index] != NULL)
    {
        if (strcmp(map->data[index]->key, key) == 0)
        {
            return index;
        }
        index++;
    }
    return -1;
}

/*
 * Add value to map or rewrite, if key already exists
 */
void
setMapValue(MaskingMap *map, char *key, char *value)
{
    if (key != NULL)
    {
        int index = getMapIndexByKey(map, key);
        if (index != -1) /* Already have key in map */
        {
            free(map->data[index]->value);
            map->data[index]->value = malloc(strlen(value) + 1);
            strcpy(map->data[index]->value, value);
        }
        else
        {
            Pair *pair = malloc(sizeof(Pair));
            pair->key = malloc(strlen(key) + 1);
            pair->value = malloc(strlen(value) + 1);
            memset(pair->key, 0, strlen(key));
            memset(pair->value, 0, strlen(value));
            strcpy(pair->key, key);
            strcpy(pair->value, value);

            map->data[map->size] = malloc(sizeof(Pair));
            *map->data[map->size] = *pair;
            map->size++;
            free(pair);
        }
        if (map->size == map->capacity) /* Increase capacity */
        {
            map->capacity *= 1.5;
            map->data = realloc(map->data, sizeof(Pair) * map->capacity);
        }
    }
    free(key);
}

void
printParsingError(struct MaskingDebugDetails *md, char *message, char current_symbol)
{
    pg_log_error("Error position (symbol '%c'): line: %d pos: %d. %s\n", current_symbol, md->line_num, md->symbol_num,
           message);
}

bool
isTerminal(char c)
{
    return c == ':' || c == ',' || c == '{' || c == '}' || c == EOF;
}

bool
isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == EOF;
}

/* Read to the end of a comment */
char
skipOneLineComment(char c, struct MaskingDebugDetails *md, FILE *fin)
{
	while (md->is_comment)
	{
		c = readNextSymbol(md, fin);
		switch (c)
		{
			case '\n':
				md->is_comment = false; /* End of a one line comment */
				c = readNextSymbol(md, fin);
				break;
			case EOF:
			  	return c; /* Handling `EOF` outside the function */
			default:
			  	continue;
		}
	}
	return c;
}

/* Read to the end of a comment */
char
skipMultiLineComment(char c, struct MaskingDebugDetails *md, FILE *fin)
{
	while (md->is_comment)
	{
		c = readNextSymbol(md, fin);
		switch (c)
		{
			case '*':
				c = readNextSymbol(md, fin);
				if (c == '/')
				{
					md->is_comment = false; /* End of a multi line comment */
					c = readNextSymbol(md, fin);
					break;
				}
				continue;
			case EOF:
			  return c; /* Handling `EOF` outside the function */
			default:
			  continue;
		}
	}
  return c;
}

/*
 * Read symbol and change place of cursor in MaskingDebugDetails
 * md->line_num - increasing when we meet '\n'
 * md->symbol_num - increasing after reading any symbol and reset
 * when we meet '\n'
 */
char
readNextSymbol(struct MaskingDebugDetails *md, FILE *fin)
{
    char c = fgetc(fin);
  	/* Count lines and columns */
    if (c == '\n')
    {
        md->line_num++;
        md->symbol_num = 1;
    }
    else
    {
        md->symbol_num++;
    }
  	/* Skip comment */
  	if (c == '/' && !md->is_comment) /* First slash */
	{
		char next_c = fgetc(fin);
		fseek(fin, -1, SEEK_CUR); /* Returning on 1 symbol back for correct line counting */
		if (next_c == '/')
		{
			md->is_comment = true;
			c = skipOneLineComment(c, md, fin);
		}
		else if (next_c == '*')
		{
			md->is_comment = true;
			c = skipMultiLineComment(c, md, fin);
		}
	}
    return c;
}

/* Read relation name */
char
readName(char *rel_name, char c, struct MaskingDebugDetails *md, FILE *fin, int size)
{
  	bool word_started = false;
  	bool word_finished = false;
    memset(rel_name, 0,  size);
    while (!isTerminal(c))
    {
        switch (c)
        {
            case ' ':
            case '\t':
            case '\n':
				if (word_started && !word_finished)
				{
				  	word_finished = true;
				}
				break; /* Skip space symbols */
            case EOF:
                return c; /* Handling `EOF` outside the function */

            default:
				if (word_finished)
				{
				  	printParsingError(md, "Syntax error. Relation name can't contain space symbols.", c);
				  	return c;
				}
				word_started = true;
				strncat(rel_name, &c, 1);
				break;
        }
        c = readNextSymbol(md, fin);
    }
    return c;
}

/* Concat schema name, table name and column name */
char *
getFullRelName(char *schema_name, char *table_name, char *column_name)
{
    /* Schema.Table.Column */
    return psprintf("%s%c%s%c%s", schema_name, REL_SEP, table_name, REL_SEP, column_name);
}

/**
 * Parsing file with masking pattern
 * ------------------------------------
 * Schema1
 * {
 *      Table1
 *      {
 *            column11 : function_name11
 *          , column12 : function_name12
 *          , column13 : function_name13
 *      }
 *
 *      Table2
 *      {
 *            column21 : function_name21
 *          , column22 : function_name22
 *          , column23 : "path_to_file_with_function/masking.sql"
 *      // Function 'masking.sql' will be stored in `masking_func_query_path` list, and it will be
 *      // created by script from the path 'path_to_file_with_function'. See more `pg_dump.c:createMaskingFunctions`
 *      }
 *  }
 *
 *
 * default // Functions inside this block will be used for all schemas
 * {
 *
 *      default // Functions inside this block will be used for all tables
 *      {
 *          default: for_all_columns, // This function will be used for all columns did not covered by exact functions
 *          column1: value1,
 *          column2: value2
 *      }
 *
 *      Table // Functions inside this block will be used for tables with name 'Table' in the all schemas
 *      {
 *          column : function_name
 *      }
 * }
 */
int
readMaskingPatternFromFile(FILE *fin, MaskingMap *map, SimpleStringList *masking_func_query_path)
{
    int exit_status;
    int brace_counter;
    int close_brace_counter;
    char *schema_name;
    char *table_name;
    char *column_name;
    char *func_name;
    bool skip_reading;
    char c;

    struct MaskingDebugDetails md;
    md.line_num = 1;
    md.symbol_num = 0;
  	md.is_comment = false;
    md.parsing_state = SCHEMA_NAME;
    exit_status = EXIT_SUCCESS;

    schema_name = malloc(REL_SIZE);
    table_name = malloc(REL_SIZE);
    column_name = malloc(REL_SIZE);
    func_name = malloc(PATH_MAX + 1); /* We can get function name or path to file with a creating function query */

    brace_counter = 0;
    close_brace_counter = 0;
    skip_reading = false;

    c = ' ';
    while (c != EOF)
    {
        if (skip_reading)
        {
            skip_reading = false;
        }
        else if (!isTerminal(c))
        {
            c = readNextSymbol(&md, fin);
        }
        switch (md.parsing_state)
        {
            case SCHEMA_NAME:
                c = readName(schema_name, c, &md, fin, REL_SIZE);
                md.parsing_state = WAIT_OPEN_BRACE;
                memset(table_name, 0, REL_SIZE);
                break;

            case TABLE_NAME:
                c = readName(table_name, c, &md, fin, REL_SIZE);
                md.parsing_state = WAIT_OPEN_BRACE;
                break;

            case COLUMN_NAME:
                c = readName(column_name, c, &md, fin, REL_SIZE);
                md.parsing_state = WAIT_COLON;
                break;

            case FUNCTION_NAME:
                c = readName(func_name, c, &md, fin, PATH_MAX);
                extractFuncNameIfPath(func_name, masking_func_query_path);
                setMapValue(map, getFullRelName(schema_name, table_name, column_name), func_name);
                md.parsing_state = WAIT_COMMA;
                break;

            case WAIT_COLON:
                if (isSpace(c))
                    break;
                if (c != ':')
                {
                    printParsingError(&md, "Waiting symbol ':'", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                md.parsing_state = FUNCTION_NAME;
                c = readNextSymbol(&md, fin);
                skip_reading = true;
                break;

            case WAIT_OPEN_BRACE:
                if (isSpace(c))
                    break;
                if (c == '}' && brace_counter > 0)
                {
                    md.parsing_state = WAIT_CLOSE_BRACE;
                    break;
                }
                if (c != '{')
                {
                    printParsingError(&md, "Waiting symbol '{'", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                if (table_name[0] != '\0') /* we have already read table_name */
                {
                    md.parsing_state = COLUMN_NAME;
                }
                else
                {
                    md.parsing_state = TABLE_NAME;
                }
                c = readNextSymbol(&md, fin);
                skip_reading = true;
                brace_counter++;
                break;

            case WAIT_CLOSE_BRACE:
                if (isSpace(c))
                    break;
                if (c != '}')
                {
                    printParsingError(&md, "Waiting symbol '}'", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                md.parsing_state = TABLE_NAME;
                c = readNextSymbol(&md, fin);
                brace_counter--;
                break;

            case WAIT_COMMA:
                if (isSpace(c))
                    break;
                if (c == '}')
                {
                    c = readNextSymbol(&md, fin);
                    skip_reading = true;
                    close_brace_counter++;
                    break;
                }
                if (c != ',' && !isTerminal(c)) /* Schema_name or Table_name */
                {
                    if (close_brace_counter == 1)
                    {
                        md.parsing_state = TABLE_NAME;
                    }
                    else if (close_brace_counter == 2)
                    {
                        md.parsing_state = SCHEMA_NAME;
                    }
                    else
                    {
                        printParsingError(&md, "Too many symbols '}'", c);
                        exit_status = EXIT_FAILURE;
                        goto clear_resources;
                    }
                    skip_reading = true;
                    close_brace_counter = 0;
                    break;
                }
                else if (c != ',')
                {
                    printParsingError(&md, "Waiting symbol ','", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                md.parsing_state = COLUMN_NAME;
                c = readNextSymbol(&md, fin);
                skip_reading = true;
                break;
        }
    }
    clear_resources:
    free(schema_name);
    free(table_name);
    free(column_name);
    free(func_name);
    return exit_status;
}

/* Creating string in format `schema_name.function name(column_name)` */
void
concatFunctionAndColumn(char *col_with_func, char *schema_name, char *column_name, char *function_name)
{
    /* Default function */
    if (strcmp(function_name, DEFAULT_NAME)==0)
	{
	  strcpy(col_with_func, psprintf("_masking_function.%s", function_name));
	}
	/* Function name already contains schema name. If not, then add the same schema */
	else if (strrchr(function_name, '.') != NULL)
    {
        strcpy(col_with_func, function_name);
    }
    else
    {
	  	strcpy(col_with_func, psprintf("%s.%s", schema_name, function_name));
    }
    strcpy(col_with_func, psprintf("%s(%s)", col_with_func, column_name));
}

/*
 * Wrapping columns with functions
 * schema_name.function_name(schema_name.table_name.column_name)
 */
char *
addFunctionToColumn(char *schema_name, char *table_name, char *column_name, MaskingMap *map)
{
    char *col_with_func;
    /* Try to find for exact schema, table and column */
    int index = getMapIndexByKey(map, getFullRelName(schema_name, table_name, column_name));
    if (index == -1) /* If didn't find, try to find function, that used for all schemas */
    {
        /* Try to find for exact table and column [default.table.column] */
        index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, table_name, column_name));
        if (index == -1) /* If didn't find, try to find function, that used for all schemas and all tables */
        {
            /* Try to find for exact column [default.default.column] */
            index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, DEFAULT_NAME, column_name));
            if (index == -1) /* If didn't find, try to find function, that used for all schemas and all tables and all columns */
            {
                /* Try to find function that used for all columns in all schemas and tables [default.default.default] */
                index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, DEFAULT_NAME, DEFAULT_NAME));
            }
        }
    }
    col_with_func = malloc(COL_WITH_FUNC_SIZE);
    memset(col_with_func, 0, COL_WITH_FUNC_SIZE);
    if (index != -1)
    {
        char *function_name = map->data[index]->value;
        concatFunctionAndColumn(col_with_func, schema_name, column_name, function_name);
    }
    return col_with_func;
}

/* Remove the first and the last symbol in func_name */
void
removeQuotes(char *func_name)
{
    char *new_func_name = malloc(PATH_MAX + 1);
    strncpy(new_func_name, func_name + 1, strlen(func_name) - 2);
    memset(func_name, 0, PATH_MAX);
    strcpy(func_name, new_func_name);
    free(new_func_name);
}

/* Read a word from a query */
char *
readWord(FILE *fin, char *word)
{
    char c;
    memset(word, 0, strlen(word));
    do
    {
        c = tolower(getc(fin));
        if (isSpace(c) || c == '(') /* Space or open brace before function arguments */
        {
            if (word[0] == '\0') /* Spaces before the word */
                continue;
            else
                break; /* Spaces after the word */
        }
        else
        {
            strncat(word, &c, 1);
        }
    } while (c != EOF);
    return word;
}

/**
 * Extract function name from query. During extracting we also check
 * the query, but only the start of it. We expecting the pattern:
 * `create [or replace] function {func_name}`
 * If something is wrong we will not use function and leave
 * the column without transforming.
 *
 * We don't check the full script because we are guessing that this script will be
 * run by users who has access to run them and will not harm theirs own data
 */
int
extractFunctionNameFromQueryFile(char *filename, char *func_name)
{
    FILE *fin;
    char *word;

    memset(func_name, 0, REL_SIZE);
    fin = NULL;
    if (filename[0] != '\0')
    {
        fin = fopen(filename, "r");
    }
    if (fin == NULL)
    {
        pg_log_warning("Problem with file \'%s\"", filename);
    }
    else
    {
        word = malloc(REL_SIZE);
        memset(word, 0, REL_SIZE);
        if (strcmp(readWord(fin, word), "create") == 0) /* reading 'create' */
        {
            if (strcmp(readWord(fin, word), "or") == 0) /* reading 'or' | 'function' */
            {
                if (strcmp(readWord(fin, word), "replace") != 0) /* reading 'replace' */
                {
                    pg_log_warning("Keyword 'replace' was expected, but found '%s'. Check query for creating a function '%s'.\n",
                           word, filename);
                    goto free_resources;
                }
                else
                {
                    readWord(fin, word); /* reading 'function' */
                }
            }
        }
        else
        {
            pg_log_warning("Keyword 'create' was expected, but found '%s'. Check query for creating a function '%s'.\n", word,
                   filename);
            goto free_resources;
        }
        if (strcmp(word, "function") == 0)
        {
            strcpy(func_name, readWord(fin, word));
        }
        else
        {
            pg_log_warning("Keyword 'function' was expected, but found '%s'. Check query for creating a function '%s'.\n", word,
                   filename);
            goto free_resources;
        }
        free_resources:
        free(word);
        fclose(fin);
    }
    return func_name[0] != '\0'; /* If we got a function name, then - return 0, else - return 1 */
}

/**
 * If there is a path (the first symbol is a quote '"'), then store this path in masking_func_query_path
 * and write to the first argument (func_path) name of the function from the query in the file
 * If there is not a path - do nothing
*/
void
extractFuncNameIfPath(char *func_path, SimpleStringList *masking_func_query_path)
{
    char *func_name;
    if (func_path[0] == '"')
    {
        func_name = malloc(REL_SIZE);
        removeQuotes(func_path);
        if (extractFunctionNameFromQueryFile(func_path, func_name) != 0) /* Read function name from query and store in func_name */
        {
            if (!simple_string_list_member(masking_func_query_path, func_path))
            {
                simple_string_list_append(masking_func_query_path, func_path);
            }
            strcpy(func_path, func_name); /* Store func_name in func_path to throw it to upper function */
        }
        free(func_name);
    }
}

/* Read whole script from the file `filename` */
char *
readQueryForCreatingFunction(char *filename)
{
    FILE *fin;
    char *query;
    long fsize;
    query = malloc(sizeof(char));
    memset(query, 0, sizeof(char));
    fin = fopen(filename, "r");
    if (fin != NULL)
    {
        fseek(fin, 0L, SEEK_END);
        fsize = ftell(fin);
        fseek(fin, 0L, SEEK_SET);

        query = (char *) calloc(fsize + 1, sizeof(char));

        fsize = (int) fread(query, sizeof(char), fsize, fin);
        if (fsize==0)
        {
            pg_log_error("File is empty `%s`", filename);
        }
        fclose(fin);
    }
    return query;
}

void
maskingColumns(char *schema_name, char *table_name, char* column_list, MaskingMap *masking_map, PQExpBuffer *q)
{
    char *current_column_name = strtok(column_list, " ,()");
    char *masked_query = malloc(COL_WITH_FUNC_SIZE);
    char *func_with_column = malloc(COL_WITH_FUNC_SIZE);

    while (current_column_name != NULL)
    {
        func_with_column = addFunctionToColumn(schema_name, table_name, current_column_name, masking_map);
        if (func_with_column[0] != '\0')
        {
            strcpy(masked_query, func_with_column);
        }
        else
        {
            strcpy(masked_query, current_column_name);
        }
        current_column_name = strtok(NULL, " ,()");
        if (current_column_name != NULL)
            strcat(masked_query, ",");
        appendPQExpBufferStr(*q, masked_query);
    }
    free(masked_query);
}

/**
 * getMaskingPatternFromFile
 *
 * Parse the specified masking file with description of what we need to mask into masking_map
 */
int
getMaskingPatternFromFile(const char *filename, MaskingMap *masking_map, SimpleStringList *masking_func_query_path)
{
  FILE *fin;
  int exit_result;
  if (filename[0]=='\0')
  {
	pg_log_error("--masking filename shouldn't be empty");
	return 1;
  }

  fin = fopen(filename, "r");

  if (fin == NULL)
  {
	pg_log_error("--masking couldn't open file `%s`", filename);
	return 1;
  }

  exit_result = readMaskingPatternFromFile(fin, masking_map, masking_func_query_path);
  fclose(fin);
  return exit_result;
}

/**
 * Default masking function
 * Full masking according to the data types. Returns 'XXXX' for string data types [text, varchar, character].
 * Returns 0 for numeric data types [int, numeric, real, smallint, bigint]
 * Returns '1900-01-01' for date and '1900-01-01 00:00:00' for timestamp
 */
char *
default_functions()
{
  return "CREATE SCHEMA IF NOT EXISTS _masking_function;\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in text, out text)\n"
		 "    AS $$ SELECT 'XXXX' $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in real, out real)\n"
		 "    AS $$ SELECT 0 $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in date, out date)\n"
		 "    AS $$ SELECT DATE '1900-01-01' $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in timestamp, out timestamp)\n"
		 "    AS $$ SELECT TIMESTAMP '1900-01-01 00:00:00' $$\n"
		 "    LANGUAGE SQL;\n"
		 "   \n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in timestamptz, out timestamptz)\n"
		 "    AS $$ SELECT TIMESTAMPTZ '1900-01-01 00:00:00-00' $$\n"
		 "    LANGUAGE SQL;\n"
		 "   \n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in time, out time)\n"
		 "    AS $$ SELECT TIME '00:00:00' $$\n"
		 "    LANGUAGE SQL;\n"
		 "   \n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in timetz, out timetz)\n"
		 "    AS $$ SELECT TIMETZ '00:00:00-00' $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in interval, out interval)\n"
		 "    AS $$ SELECT INTERVAL '1 year 2 months 3 days' $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in box, out box)\n"
		 "    AS $$ SELECT box(circle '((0,0),2.0)') $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in circle, out circle)\n"
		 "    AS $$ SELECT circle(point '(0,0)', 0) $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in path, out path)\n"
		 "    AS $$ SELECT '[ ( 0 , 1 ) , ( 1 , 2 ) ]'::path $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in point, out point)\n"
		 "    AS $$ SELECT '(0, 0)'::point $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in polygon , out polygon)\n"
		 "    AS $$ SELECT '( ( 0 , 0 ) , ( 0 , 0 ) )'::polygon $$\n"
		 "    LANGUAGE SQL;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in bytea, out bytea)\n"
		 "    AS $$ SELECT '\\000'::bytea $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in inet, out inet)\n"
		 "    AS $$ SELECT '0.0.0.0'::inet $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in cidr, out cidr)\n"
		 "    AS $$ SELECT '0.0.0.0'::cidr $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in macaddr, out macaddr)\n"
		 "    AS $$ SELECT macaddr '0:0:0:0:0:ab' $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in json, out json)\n"
		 "    AS $$ SELECT '{\"a\":\"foo\", \"b\":\"bar\"}'::json $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in jsonb, out jsonb)\n"
		 "    AS $$ SELECT '{\"a\":1, \"b\":2}'::jsonb $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in line, out line)\n"
		 "    AS $$ SELECT '{1,2,3}'::line $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in lseg, out lseg)\n"
		 "    AS $$ SELECT '((0,0),(0,0))'::lseg $$\n"
		 "    LANGUAGE sql;\n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in bit, out bit)\n"
		 "    AS $$ SELECT '0'::bit $$\n"
		 "    LANGUAGE sql; \n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in boolean, out boolean)\n"
		 "    AS $$ SELECT true $$\n"
		 "    LANGUAGE sql;   \n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in money, out money)\n"
		 "    AS $$ SELECT 0 $$\n"
		 "    LANGUAGE sql;   \n"
		 "\n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in pg_lsn, out pg_lsn)\n"
		 "    AS $$ SELECT '0/0'::pg_lsn $$\n"
		 "    LANGUAGE sql;  \n"
		 "   \n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in uuid, out uuid)\n"
		 "    AS $$ SELECT '00000000-0000-0000-0000-000000000000'::uuid $$\n"
		 "    LANGUAGE sql;  \n"
		 "   \n"
		 "CREATE OR REPLACE FUNCTION _masking_function.default(in tsvector, out tsvector)\n"
		 "    AS $$ SELECT 'a:1'::tsvector $$\n"
		 "    LANGUAGE sql;     ";
}
