
/* File:			columninfo.h
 *
 * Description:		See "columninfo.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __COLUMNINFO_H__
#define __COLUMNINFO_H__

#include "psqlodbc.h"

struct ColumnInfoClass_
{
	Int2		num_fields;
	char	  **name;			/* list of type names */
	Oid		   *adtid;			/* list of type ids */
	Int2	   *adtsize;		/* list type sizes */
	Int2	   *display_size;	/* the display size (longest row) */
	Int4	   *atttypmod;		/* the length of bpchar/varchar */
};

#define CI_get_num_fields(self)			(self->num_fields)
#define CI_get_oid(self, col)			(self->adtid[col])
#define CI_get_fieldname(self, col)		(self->name[col])
#define CI_get_fieldsize(self, col)		(self->adtsize[col])
#define CI_get_display_size(self, col)	(self->display_size[col])
#define CI_get_atttypmod(self, col)		(self->atttypmod[col])

ColumnInfoClass *CI_Constructor(void);
void		CI_Destructor(ColumnInfoClass * self);
void		CI_free_memory(ColumnInfoClass * self);
char		CI_read_fields(ColumnInfoClass * self, ConnectionClass * conn);

/* functions for setting up the fields from within the program, */
/* without reading from a socket */
void		CI_set_num_fields(ColumnInfoClass * self, int new_num_fields);
void CI_set_field_info(ColumnInfoClass * self, int field_num, char *new_name,
				  Oid new_adtid, Int2 new_adtsize, Int4 atttypmod);


#endif
