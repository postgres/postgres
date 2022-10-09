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
#include "fe_utils/simple_list.h"

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
  enum ParsingState parsing_state;
};

MaskingMap *newMaskingMap(void);
void cleanMap(MaskingMap *map);
void setMapValue(MaskingMap *map, char *key, char *value);
void printParsingError(struct MaskingDebugDetails *md, char *message, char current_symbol);
bool isTerminal(char c);
bool isSpace(char c);
char readNextSymbol(struct MaskingDebugDetails *md, FILE *fin);
char nameReader(char *rel_name, char c, struct MaskingDebugDetails *md, FILE *fin, int size);
int getMapIndexByKey(MaskingMap *map, char *key);
extern int readMaskingPatternFromFile(FILE *fin, MaskingMap *map, SimpleStringList *masking_func_query_path);
char *addFunctionToColumn(char *schema_name, char *table_name, char *column_name, MaskingMap *map);
char *getFullRelName(char *schema_name, char *table_name, char *column_name);
void concatFunctionAndColumn(char *col_with_func, char *schema_name, char *column_name, char *function_name);
char *readQueryForCreatingFunction(char *filename);
extern void extractFuncNameIfPath(char *func_path, SimpleStringList *masking_func_query_path);
void removeQuotes(char *func_name);
char *readWord(FILE *fin, char *word);
int extractFunctionNameFromQueryFile(char *filename, char *func_name);
char *default_functions(void);

#endif                            /* MASKING_H */
