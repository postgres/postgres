/*
 * op function for ltree[] 
 * Teodor Sigaev <teodor@stack.net>
 */

#include "ltree.h"
#include <ctype.h>
#include "utils/array.h"

PG_FUNCTION_INFO_V1(_ltree_isparent);
PG_FUNCTION_INFO_V1(_ltree_r_isparent);
PG_FUNCTION_INFO_V1(_ltree_risparent);
PG_FUNCTION_INFO_V1(_ltree_r_risparent);
PG_FUNCTION_INFO_V1(_ltq_regex);
PG_FUNCTION_INFO_V1(_ltq_rregex);
PG_FUNCTION_INFO_V1(_ltxtq_exec);
PG_FUNCTION_INFO_V1(_ltxtq_rexec);

Datum _ltree_r_isparent(PG_FUNCTION_ARGS);
Datum _ltree_r_risparent(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(_ltree_extract_isparent);
PG_FUNCTION_INFO_V1(_ltree_extract_risparent);
PG_FUNCTION_INFO_V1(_ltq_extract_regex);
PG_FUNCTION_INFO_V1(_ltxtq_extract_exec);
Datum _ltree_extract_isparent(PG_FUNCTION_ARGS);
Datum _ltree_extract_risparent(PG_FUNCTION_ARGS);
Datum _ltq_extract_regex(PG_FUNCTION_ARGS);
Datum _ltxtq_extract_exec(PG_FUNCTION_ARGS);


typedef Datum (*PGCALL2)(PG_FUNCTION_ARGS);
#define NEXTVAL(x) ( (ltree*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )

static bool
array_iterator( ArrayType *la, PGCALL2 callback, void* param, ltree ** found) {
	int num=ArrayGetNItems( ARR_NDIM(la), ARR_DIMS(la));
	ltree	*item = (ltree*)ARR_DATA_PTR(la);

	if ( ARR_NDIM(la) !=1 )
		elog(ERROR,"Dimension of array != 1");

	if ( found )
		*found=NULL;
	while( num>0 ) {
		if ( DatumGetBool( DirectFunctionCall2( callback, 
			PointerGetDatum(item), PointerGetDatum(param) ) ) ) {

			if ( found )
				*found = item;
			return true;
		}
		num--;
		item = NEXTVAL(item); 
	}

	return false;
}

Datum
_ltree_isparent(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	ltree		*query = PG_GETARG_LTREE(1);
	bool res = array_iterator( la, ltree_isparent, (void*)query, NULL );
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_BOOL(res);
}

Datum
_ltree_r_isparent(PG_FUNCTION_ARGS) {
	PG_RETURN_DATUM( DirectFunctionCall2( _ltree_isparent,
		PG_GETARG_DATUM(1),
		PG_GETARG_DATUM(0)
	) );
}

Datum
_ltree_risparent(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	ltree		*query = PG_GETARG_LTREE(1);
	bool res = array_iterator( la, ltree_risparent, (void*)query, NULL );
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_BOOL(res);
}

Datum
_ltree_r_risparent(PG_FUNCTION_ARGS) {
	PG_RETURN_DATUM( DirectFunctionCall2( _ltree_risparent,
		PG_GETARG_DATUM(1),
		PG_GETARG_DATUM(0)
	) );
}

Datum
_ltq_regex(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	lquery		*query = PG_GETARG_LQUERY(1);
	bool res = array_iterator( la, ltq_regex, (void*)query, NULL );
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_BOOL(res);
}

Datum
_ltq_rregex(PG_FUNCTION_ARGS) {
	PG_RETURN_DATUM( DirectFunctionCall2( _ltq_regex,
		PG_GETARG_DATUM(1),
		PG_GETARG_DATUM(0)
	) );
}

Datum   
_ltxtq_exec(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	ltxtquery	*query = PG_GETARG_LTXTQUERY(1);
	bool res = array_iterator( la, ltxtq_exec, (void*)query, NULL );
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_BOOL(res);
}

Datum
_ltxtq_rexec(PG_FUNCTION_ARGS) {
	PG_RETURN_DATUM( DirectFunctionCall2( _ltxtq_exec,
		PG_GETARG_DATUM(1),
		PG_GETARG_DATUM(0)
	) );
}


Datum 
_ltree_extract_isparent(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	ltree		*query = PG_GETARG_LTREE(1);
	ltree		*found,*item;

	if ( !array_iterator( la, ltree_isparent, (void*)query, &found )  ) {
		PG_FREE_IF_COPY(la,0);
		PG_FREE_IF_COPY(query,1);
		PG_RETURN_NULL();
	}

	item = (ltree*)palloc( found->len );
	memcpy( item, found, found->len );	
	
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_POINTER(item);
}

Datum 
_ltree_extract_risparent(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	ltree		*query = PG_GETARG_LTREE(1);
	ltree		*found,*item;

	if ( !array_iterator( la, ltree_risparent, (void*)query, &found )  ) {
		PG_FREE_IF_COPY(la,0);
		PG_FREE_IF_COPY(query,1);
		PG_RETURN_NULL();
	}

	item = (ltree*)palloc( found->len );
	memcpy( item, found, found->len );	
	
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_POINTER(item);
}

Datum 
_ltq_extract_regex(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	lquery		*query = PG_GETARG_LQUERY(1);
	ltree		*found,*item;

	if ( !array_iterator( la, ltq_regex, (void*)query, &found )  ) {
		PG_FREE_IF_COPY(la,0);
		PG_FREE_IF_COPY(query,1);
		PG_RETURN_NULL();
	}

	item = (ltree*)palloc( found->len );
	memcpy( item, found, found->len );	
	
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_POINTER(item);
}

Datum 
_ltxtq_extract_exec(PG_FUNCTION_ARGS) {
	ArrayType	*la = (ArrayType *)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	ltxtquery	*query = PG_GETARG_LTXTQUERY(1);
	ltree		*found,*item;

	if ( !array_iterator( la, ltxtq_exec, (void*)query, &found )  ) {
		PG_FREE_IF_COPY(la,0);
		PG_FREE_IF_COPY(query,1);
		PG_RETURN_NULL();
	}

	item = (ltree*)palloc( found->len );
	memcpy( item, found, found->len );	
	
	PG_FREE_IF_COPY(la,0);
	PG_FREE_IF_COPY(query,1);
	PG_RETURN_POINTER(item);
}

