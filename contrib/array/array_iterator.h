#ifndef ARRAY_ITERATOR_H
#define ARRAY_ITERATOR_H

static int32 array_iterator(Oid proc, int and,
			   ArrayType *array, Datum value);

int32		array_texteq(ArrayType *array, void *value);
int32		array_all_texteq(ArrayType *array, void *value);
int32		array_textregexeq(ArrayType *array, void *value);
int32		array_all_textregexeq(ArrayType *array, void *value);

int32		array_bpchareq(ArrayType *array, void *value);
int32		array_all_bpchareq(ArrayType *array, void *value);
int32		array_bpcharregexeq(ArrayType *array, void *value);
int32		array_all_bpcharregexeq(ArrayType *array, void *value);

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

int32		array_oideq(ArrayType *array, Oid value);
int32		array_all_oidne(ArrayType *array, Oid value);

int32		array_ineteq(ArrayType *array, void *value);
int32		array_all_ineteq(ArrayType *array, void *value);
int32		array_inetne(ArrayType *array, void *value);
int32		array_all_inetne(ArrayType *array, void *value);

#endif
