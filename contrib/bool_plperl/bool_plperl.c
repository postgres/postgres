#include "postgres.h"

#include "fmgr.h"
#include "plperl.h"


PG_MODULE_MAGIC_EXT(
					.name = "bool_plperl",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(bool_to_plperl);

Datum
bool_to_plperl(PG_FUNCTION_ARGS)
{
	dTHX;
	bool		in = PG_GETARG_BOOL(0);

	return PointerGetDatum(in ? &PL_sv_yes : &PL_sv_no);
}


PG_FUNCTION_INFO_V1(plperl_to_bool);

Datum
plperl_to_bool(PG_FUNCTION_ARGS)
{
	dTHX;
	SV		   *in = (SV *) PG_GETARG_POINTER(0);

	PG_RETURN_BOOL(SvTRUE(in));
}
