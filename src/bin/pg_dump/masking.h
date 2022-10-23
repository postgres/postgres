/*-------------------------------------------------------------------------
 *
 * masking.h
 *
 *	Data masking tool for pg_dump
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/masking.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MASKING_H
#define MASKING_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include "fe_utils/option_utils.h"
#include "fe_utils/simple_list.h"
#include "dumputils.h"

typedef struct _pair
{
  char *key;
  char *value;
} Pair;

typedef struct MaskingMap
{
  Pair **data;
  int size;
  int capacity;
} MaskingMap;

enum
ParsingState
{
  SCHEMA_NAME,
  TABLE_NAME,
  COLUMN_NAME,
  FUNCTION_NAME,
  WAIT_COLON,
  WAIT_OPEN_BRACE,
  WAIT_CLOSE_BRACE,
  WAIT_COMMA
};

struct
MaskingDebugDetails
{
  int line_num;
  int symbol_num;
  bool is_comment;
  enum ParsingState parsing_state;
};

static SimpleStringList masking_func_query_path = {NULL, NULL}; /* List of path to query with masking functions, that must be created before starting dump */
static MaskingMap *masking_map; /* Map of columns and functions for data masking */

char *addFunctionToColumn(char *schema_name, char *table_name, char *column_name, MaskingMap *map);
char *default_functions(void);
int getMaskingPatternFromFile(const char *filename, MaskingMap *masking_map, SimpleStringList *masking_func_query_path);
void maskingColumns(char *schema_name, char *table_name, char* column_list, MaskingMap *masking_map, PQExpBuffer *q);
MaskingMap *newMaskingMap(void);
extern int readMaskingPatternFromFile(FILE *fin, MaskingMap *map, SimpleStringList *masking_func_query_path);
char *readQueryForCreatingFunction(char *filename);

#endif                            /* MASKING_H */
