#include <ecpgtype.h>
#include <ecpglib.h>

bool
ECPGget_desc_header(int lineno, char * desc_name, int *count)
{
	PGresult *ECPGresult = ECPGresultByDescriptor(lineno, desc_name);
	
	if (!ECPGresult)
		return false;

	*count = PQnfields(ECPGresult);
	ECPGlog("ECPGget_desc_header: found %d attributes.\n", *count);
	return true;
}	

static bool
get_int_item(int lineno, void *var, enum ECPGdtype vartype, int value)
{
	switch (vartype)
     	{
       		case ECPGt_short:
              		*(short *)var = value;
               		break;
		case ECPGt_int:
	       		*(int *)var = value;
	  		break;
  		case ECPGt_long:
  			*(long *)var = value;
	  		break;
  		case ECPGt_unsigned_short:
  			*(unsigned short *)var = value;
	  		break;
		case ECPGt_unsigned_int:
  			*(unsigned int *)var = value;
	  		break;
  		case ECPGt_unsigned_long:
  			*(unsigned long *)var = value;
	  		break;
  		case ECPGt_float:
  			*(float *)var = value;
	  		break;
  		case ECPGt_double:
  			*(double *)var = value;
	  		break;
  		default:
  			ECPGraise(lineno, ECPG_VAR_NOT_NUMERIC, NULL);
  			return (false);
	}
	
	return(true);
}

bool
ECPGget_desc(int lineno, char *desc_name, int index, ...)
{
	va_list         args;
	PGresult 	*ECPGresult = ECPGresultByDescriptor(lineno, desc_name);
	enum ECPGdtype	type;
	bool		DataButNoIndicator = false;
	
	va_start(args, index);
        if (!ECPGresult)
        	return (false);

       	if (PQntuples(ECPGresult) < 1)
       	{
       		ECPGraise(lineno, ECPG_NOT_FOUND, NULL);
       		return (false);
       	}
        
        if (index < 1 || index >PQnfields(ECPGresult))
        {
                ECPGraise(lineno, ECPG_INVALID_DESCRIPTOR_INDEX, NULL);
                return (false);
        }

	ECPGlog("ECPGget_desc: reading items for tuple %d\n", index);
    	--index;
    	
    	type =  va_arg(args, enum ECPGdtype);
    	
    	while (type != ECPGd_EODT)
    	{
    		char type_str[20];
    		long varcharsize;
    		long offset;
    		long arrsize;
    		enum ECPGttype vartype;
    		void *var;
    		
    		vartype = va_arg(args, enum ECPGttype);
    		var = va_arg(args, void *);
    		varcharsize = va_arg(args, long);
    		arrsize = va_arg(args, long);
		offset = va_arg(args, long);
		
    		switch (type)
    		{ 
	        	case (ECPGd_indicator):
	        		if (!get_int_item(lineno, var, vartype, -PQgetisnull(ECPGresult, 0, index)))
	        			return (false);
	                 	break;

	                case ECPGd_name:
	                 	strncpy((char *)var, PQfname(ECPGresult, index), varcharsize);
	                 	break;
	                 	
	                case ECPGd_nullable:
	                	if (!get_int_item(lineno, var, vartype, 1))
	        			return (false);
	                	break;
	                	
	                case ECPGd_key_member:
	                	if (!get_int_item(lineno, var, vartype, 0))
	        			return (false);
	                	break;
	                 
	                case ECPGd_scale:
	                	if (!get_int_item(lineno, var, vartype, (PQfmod(ECPGresult, index) - VARHDRSZ) & 0xffff))
	        			return (false);
		                break;
		                 
		        case ECPGd_precision:
		        	if (!get_int_item(lineno, var, vartype, PQfmod(ECPGresult, index) >> 16))
	        			return (false);
		                break;
		                 
		        case ECPGd_ret_length:
			case ECPGd_ret_octet:
				if (!get_int_item(lineno, var, vartype, PQgetlength(ECPGresult, 0, index)))
	        			return (false);
			 	break;
			 	
			case ECPGd_octet:
				if (!get_int_item(lineno, var, vartype, PQfsize(ECPGresult, index)))
	        			return (false);
	                        break;

			case ECPGd_length:
				if (!get_int_item(lineno, var, vartype, PQfmod(ECPGresult, index) - VARHDRSZ))
	        			return (false);
				break;
				
                        case ECPGd_type:
                        	if (!get_int_item(lineno, var, vartype, ECPGDynamicType(PQftype(ECPGresult, index))))
	        			return (false);
                        	break;
                        	
                        default:
                        	snprintf(type_str, sizeof(type_str), "%d", type);
                        	ECPGraise(lineno, ECPG_UNKNOWN_DESCRIPTOR_ITEM, type_str);
                        	return(false);
		}
		
		type =  va_arg(args, enum ECPGdtype);
	}
                        
        if (DataButNoIndicator && PQgetisnull(ECPGresult, 0, index))
        {
               	ECPGraise(lineno, ECPG_MISSING_INDICATOR, NULL);
               	return (false);
        }

	return (true);                	
}
