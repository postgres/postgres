#include <ecpgtype.h>
#include <ecpglib.h>

bool
ECPGget_desc_header(int lineno, char * desc_name, int *count)
{
	PGresult *ECPGresult = ECPGresultByDescriptor(lineno, desc_name);
	
	if (!ECPGresult)
		return false;

	*count = PQnfields(ECPGresult);
	ECPGlog("ECPGget-desc_header: found %d sttributes.\n", *count);
	return true;
}	
