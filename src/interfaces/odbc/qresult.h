
/* File:			qresult.h
 *
 * Description:		See "qresult.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __QRESULT_H__
#define __QRESULT_H__

#include "connection.h"
#include "socket.h"
#include "columninfo.h"
#include "tuplelist.h"
#include "psqlodbc.h"
#include "tuple.h"

enum QueryResultCode_
{
	PGRES_EMPTY_QUERY = 0,
	PGRES_COMMAND_OK,			/* a query command that doesn't return */
	/* anything was executed properly by the backend */
	PGRES_TUPLES_OK,			/* a query command that returns tuples */
	/* was executed properly by the backend, PGresult */
	/* contains the resulttuples */
	PGRES_COPY_OUT,
	PGRES_COPY_IN,
	PGRES_BAD_RESPONSE,			/* an unexpected response was recv'd from
								 * the backend */
	PGRES_NONFATAL_ERROR,
	PGRES_FATAL_ERROR,
	PGRES_FIELDS_OK,			/* field information from a query was
								 * successful */
	PGRES_END_TUPLES,
	PGRES_INTERNAL_ERROR
};
typedef enum QueryResultCode_ QueryResultCode;


struct QResultClass_
{
	ColumnInfoClass *fields;	/* the Column information */
	TupleListClass *manual_tuples;		/* manual result tuple list */
	ConnectionClass *conn;		/* the connection this result is using
								 * (backend) */

	/* Stuff for declare/fetch tuples */
	int			fetch_count;	/* logical rows read so far */
	int			fcount;			/* actual rows read in the fetch */
	int			currTuple;
	int			base;

	int			num_fields;		/* number of fields in the result */
	int			cache_size;
	int			rowset_size;

	QueryResultCode status;

	char	   *message;
	char	   *cursor;			/* The name of the cursor for select
								 * statements */
	char	   *command;
	char	   *notice;

	TupleField *backend_tuples; /* data from the backend (the tuple cache) */
	TupleField *tupleField;		/* current backend tuple being retrieved */

	char		inTuples;		/* is a fetch of rows from the backend in
								 * progress? */
};

#define QR_get_fields(self)				(self->fields)


/*	These functions are for retrieving data from the qresult */
#define QR_get_value_manual(self, tupleno, fieldno) (TL_get_fieldval(self->manual_tuples, tupleno, fieldno))
#define QR_get_value_backend(self, fieldno)			(self->tupleField[fieldno].value)
#define QR_get_value_backend_row(self, tupleno, fieldno) ((self->backend_tuples + (tupleno * self->num_fields))[fieldno].value)

/*	These functions are used by both manual and backend results */
#define QR_NumResultCols(self)				(CI_get_num_fields(self->fields))
#define QR_get_fieldname(self, fieldno_)	(CI_get_fieldname(self->fields, fieldno_))
#define QR_get_fieldsize(self, fieldno_)	(CI_get_fieldsize(self->fields, fieldno_))
#define QR_get_display_size(self, fieldno_) (CI_get_display_size(self->fields, fieldno_))
#define QR_get_atttypmod(self, fieldno_)	(CI_get_atttypmod(self->fields, fieldno_))
#define QR_get_field_type(self, fieldno_)	(CI_get_oid(self->fields, fieldno_))

/*	These functions are used only for manual result sets */
#define QR_get_num_tuples(self)				(self->manual_tuples ? TL_get_num_tuples(self->manual_tuples) : self->fcount)
#define QR_add_tuple(self, new_tuple)		(TL_add_tuple(self->manual_tuples, new_tuple))
#define QR_set_field_info(self, field_num, name, adtid, adtsize)  (CI_set_field_info(self->fields, field_num, name, adtid, adtsize, -1))

/* status macros */
#define QR_command_successful(self)		( !(self->status == PGRES_BAD_RESPONSE || self->status == PGRES_NONFATAL_ERROR || self->status == PGRES_FATAL_ERROR))
#define QR_command_nonfatal(self)		( self->status == PGRES_NONFATAL_ERROR)
#define QR_end_tuples(self)				( self->status == PGRES_END_TUPLES)
#define QR_set_status(self, condition)	( self->status = condition )
#define QR_set_message(self, message_)	( self->message = message_)

#define QR_get_message(self)			(self->message)
#define QR_get_command(self)			(self->command)
#define QR_get_notice(self)				(self->notice)
#define QR_get_status(self)				(self->status)

/*	Core Functions */
QResultClass *QR_Constructor(void);
void		QR_Destructor(QResultClass * self);
char		QR_read_tuple(QResultClass * self, char binary);
int			QR_next_tuple(QResultClass * self);
int			QR_close(QResultClass * self);
char		QR_fetch_tuples(QResultClass * self, ConnectionClass * conn, char *cursor);
void		QR_free_memory(QResultClass * self);
void		QR_set_command(QResultClass * self, char *msg);
void		QR_set_notice(QResultClass * self, char *msg);

void		QR_set_num_fields(QResultClass * self, int new_num_fields); /* manual result only */

void		QR_inc_base(QResultClass * self, int base_inc);
void		QR_set_cache_size(QResultClass * self, int cache_size);
void		QR_set_rowset_size(QResultClass * self, int rowset_size);
void		QR_set_position(QResultClass * self, int pos);

#endif
