
/* File:            columninfo.h
 *
 * Description:     See "columninfo.c"
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifndef __COLUMNINFO_H__
#define __COLUMNINFO_H__

#include "psqlodbc.h"

struct ColumnInfoClass_ {
	Int2	num_fields;
	char	**name;				/* list of type names */
	Oid		*adtid;				/* list of type ids */
	Int2	*adtsize;			/* list type sizes */
};

#define CI_get_num_fields(self)    (self->num_fields)
#define CI_get_oid(self, col)		(self->adtid[col])


ColumnInfoClass *CI_Constructor();
void CI_Destructor(ColumnInfoClass *self);
char CI_read_fields(ColumnInfoClass *self, SocketClass *sock);

/* functions for setting up the fields from within the program, */
/* without reading from a socket */
void CI_set_num_fields(ColumnInfoClass *self, int new_num_fields);
void CI_set_field_info(ColumnInfoClass *self, int field_num, char *new_name, 
                       Oid new_adtid, Int2 new_adtsize);

char *CI_get_fieldname(ColumnInfoClass *self, Int2 which);
Int2 CI_get_fieldsize(ColumnInfoClass *self, Int2 which);
void CI_free_memory(ColumnInfoClass *self);

#endif
