#ifndef ARRAY_ITERATOR_H
#define ARRAY_ITERATOR_H

static int32 array_iterator(Oid elemtype, Oid proc, int and,
							ArrayType *array, Datum value);

int32		array_texteq(ArrayType *array, char *value);
int32		array_all_texteq(ArrayType *array, char *value);
int32		array_textregexeq(ArrayType *array, char *value);
int32		array_all_textregexeq(ArrayType *array, char *value);

int32		array_varchareq(ArrayType *array, char *value);
int32		array_all_varchareq(ArrayType *array, char *value);
int32		array_varcharregexeq(ArrayType *array, char *value);
int32		array_all_varcharregexeq(ArrayType *array, char *value);

int32		array_bpchareq(ArrayType *array, char *value);
int32		array_all_bpchareq(ArrayType *array, char *value);
int32		array_bpcharregexeq(ArrayType *array, char *value);
int32		array_all_bpcharregexeq(ArrayType *array, char *value);

int32		array_int4eq(ArrayType *array, int4 value);
int32		array_all_int4eq(ArrayType *array, int4 value);
int32		array_int4ne(ArrayType *array, int4 value);
int32		array_all_int4ne(ArrayType *array, int4 value);
int32		array_int4gt(ArrayType *array, int4 value);
int32		array_all_int4gt(ArrayType *array, int4 value);
int32		array_int4ge(ArrayType *array, int4 value);
int32		array_all_int4ge(ArrayType *array, int4 value);
int32		array_int4lt(ArrayType *array, int4 value);
int32		array_all_int4lt(ArrayType *array, int4 value);
int32		array_int4le(ArrayType *array, int4 value);
int32		array_all_int4le(ArrayType *array, int4 value);

int32       array_oideq(ArrayType *array, Oid value);
int32       array_all_oidne(ArrayType *array, Oid value);
#endif

/*
 * Local Variables:
 *  tab-width: 4
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 */
