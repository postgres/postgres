#ifndef ARRAY_ITERATOR_H
#define ARRAY_ITERATOR_H

static int32 array_iterator(Oid elemtype, Oid proc, int and,
			   ArrayType *array, Datum value);
int32		array_texteq(ArrayType *array, char *value);
int32		array_all_texteq(ArrayType *array, char *value);
int32		array_textregexeq(ArrayType *array, char *value);
int32		array_all_textregexeq(ArrayType *array, char *value);
int32		array_char16eq(ArrayType *array, char *value);
int32		array_all_char16eq(ArrayType *array, char *value);
int32		array_char16regexeq(ArrayType *array, char *value);
int32		array_all_char16regexeq(ArrayType *array, char *value);
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

#endif
