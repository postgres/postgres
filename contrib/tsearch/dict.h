#define TABLE_DICT_START	,{
#define TABLE_DICT_END		}

#include "dict/porter_english.dct"
#ifdef USE_LOCALE
#include "dict/russian_stemming.dct"
#endif

#undef TABLE_DICT_START
#undef TABLE_DICT_END
