/* Module:			columninfo.c
 *
 * Description:		This module contains routines related to
 *					reading and storing the field information from a query.
 *
 * Classes:			ColumnInfoClass (Functions prefix: "CI_")
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#include "columninfo.h"
#include "connection.h"
#include "socket.h"
#include <stdlib.h>
#include <string.h>

ColumnInfoClass *
CI_Constructor()
{
	ColumnInfoClass *rv;

	rv = (ColumnInfoClass *) malloc(sizeof(ColumnInfoClass));

	if (rv)
	{
		rv->num_fields = 0;
		rv->name = NULL;
		rv->adtid = NULL;
		rv->adtsize = NULL;
		rv->display_size = NULL;
		rv->atttypmod = NULL;
	}

	return rv;
}

void
CI_Destructor(ColumnInfoClass *self)
{
	CI_free_memory(self);

	free(self);
}

/*	Read in field descriptions.
	If self is not null, then also store the information.
	If self is null, then just read, don't store.
*/
char
CI_read_fields(ColumnInfoClass *self, ConnectionClass *conn)
{
	Int2		lf;
	int			new_num_fields;
	Oid			new_adtid;
	Int2		new_adtsize;
	Int4		new_atttypmod = -1;
	char		new_field_name[MAX_MESSAGE_LEN + 1];
	SocketClass *sock;
	ConnInfo   *ci;

	sock = CC_get_socket(conn);
	ci = &conn->connInfo;

	/* at first read in the number of fields that are in the query */
	new_num_fields = (Int2) SOCK_get_int(sock, sizeof(Int2));

	mylog("num_fields = %d\n", new_num_fields);

	if (self)
	{							/* according to that allocate memory */
		CI_set_num_fields(self, new_num_fields);
	}

	/* now read in the descriptions */
	for (lf = 0; lf < new_num_fields; lf++)
	{
		SOCK_get_string(sock, new_field_name, MAX_MESSAGE_LEN);
		new_adtid = (Oid) SOCK_get_int(sock, 4);
		new_adtsize = (Int2) SOCK_get_int(sock, 2);

		/* If 6.4 protocol, then read the atttypmod field */
		if (PG_VERSION_GE(conn, 6.4))
		{
			mylog("READING ATTTYPMOD\n");
			new_atttypmod = (Int4) SOCK_get_int(sock, 4);

			/* Subtract the header length */
			new_atttypmod -= 4;
			if (new_atttypmod < 0)
				new_atttypmod = -1;
		}

		mylog("CI_read_fields: fieldname='%s', adtid=%d, adtsize=%d, atttypmod=%d\n", new_field_name, new_adtid, new_adtsize, new_atttypmod);

		if (self)
			CI_set_field_info(self, lf, new_field_name, new_adtid, new_adtsize, new_atttypmod);
	}

	return (SOCK_get_errcode(sock) == 0);
}



void
CI_free_memory(ColumnInfoClass *self)
{
	register Int2 lf;
	int			num_fields = self->num_fields;

	for (lf = 0; lf < num_fields; lf++)
	{
		if (self->name[lf])
			free(self->name[lf]);
	}

	/* Safe to call even if null */
	free(self->name);
	free(self->adtid);
	free(self->adtsize);
	free(self->display_size);

	free(self->atttypmod);
}

void
CI_set_num_fields(ColumnInfoClass *self, int new_num_fields)
{
	CI_free_memory(self);		/* always safe to call */

	self->num_fields = new_num_fields;

	self->name = (char **) malloc(sizeof(char *) * self->num_fields);
	self->adtid = (Oid *) malloc(sizeof(Oid) * self->num_fields);
	self->adtsize = (Int2 *) malloc(sizeof(Int2) * self->num_fields);
	self->display_size = (Int2 *) malloc(sizeof(Int2) * self->num_fields);
	self->atttypmod = (Int4 *) malloc(sizeof(Int4) * self->num_fields);
}

void
CI_set_field_info(ColumnInfoClass *self, int field_num, char *new_name,
				  Oid new_adtid, Int2 new_adtsize, Int4 new_atttypmod)
{
	/* check bounds */
	if ((field_num < 0) || (field_num >= self->num_fields))
		return;

	/* store the info */
	self->name[field_num] = strdup(new_name);
	self->adtid[field_num] = new_adtid;
	self->adtsize[field_num] = new_adtsize;
	self->atttypmod[field_num] = new_atttypmod;

	self->display_size[field_num] = 0;
}
