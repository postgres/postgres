
/* Module:          qresult.c
 *
 * Description:     This module contains functions related to 
 *                  managing result information (i.e, fetching rows from the backend,
 *                  managing the tuple cache, etc.) and retrieving it.
 *                  Depending on the situation, a QResultClass will hold either data
 *                  from the backend or a manually built result (see "qresult.h" to
 *                  see which functions/macros are for manual or backend results.
 *                  For manually built results, the QResultClass simply points to 
 *                  TupleList and ColumnInfo structures, which actually hold the data.
 *
 * Classes:         QResultClass (Functions prefix: "QR_")
 *
 * API functions:   none
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include "qresult.h"
#include "misc.h"
#include <stdio.h>
#include <string.h>

#ifndef TRUE
#define TRUE	(BOOL)1
#endif
#ifndef FALSE
#define FALSE	(BOOL)0
#endif

extern GLOBAL_VALUES globals;

/*  Used for building a Manual Result only */
/*	All info functions call this function to create the manual result set. */
void 
QR_set_num_fields(QResultClass *self, int new_num_fields)
{
	mylog("in QR_set_num_fields\n");

    CI_set_num_fields(self->fields, new_num_fields);
    if(self->manual_tuples) 
        TL_Destructor(self->manual_tuples);

    self->manual_tuples = TL_Constructor(new_num_fields);

	mylog("exit QR_set_num_fields\n");
}

void 
QR_set_position(QResultClass *self, int pos)
{
	self->tupleField = self->backend_tuples + ((self->base + pos) * self->num_fields);
}

void
QR_set_cache_size(QResultClass *self, int cache_size)
{
	self->cache_size = cache_size;
}

void 
QR_set_rowset_size(QResultClass *self, int rowset_size)
{
	self->rowset_size = rowset_size;
}

void
QR_inc_base(QResultClass *self, int base_inc)
{
	self->base += base_inc;
}

/************************************/
/* CLASS QResult                    */
/************************************/

QResultClass *
QR_Constructor(void)
{
QResultClass *rv;

	mylog("in QR_Constructor\n");
	rv = (QResultClass *) malloc(sizeof(QResultClass));

	if (rv != NULL) {
		rv->status = PGRES_EMPTY_QUERY;

		/* construct the column info */
		if ( ! (rv->fields = CI_Constructor())) {
			free(rv);
			return NULL;
		}
		rv->manual_tuples = NULL;	
		rv->backend_tuples = NULL;	
		rv->message = NULL;
		rv->command = NULL;
		rv->notice = NULL;
		rv->conn = NULL;
		rv->inTuples = FALSE;
		rv->fcount = 0;
		rv->fetch_count = 0;
		rv->base = 0;
		rv->currTuple = -1;
		rv->num_fields = 0;
		rv->tupleField = NULL;
		rv->cursor = NULL;

		rv->cache_size = globals.fetch_max;
		rv->rowset_size = 1;

	}

	mylog("exit QR_Constructor\n");
	return rv;
}

void
QR_Destructor(QResultClass *self)
{
	mylog("QResult: in DESTRUCTOR\n");

	/* manual result set tuples */
	if (self->manual_tuples)
		TL_Destructor(self->manual_tuples);

	//	If conn is defined, then we may have used "backend_tuples",
	//	so in case we need to, free it up.  Also, close the cursor.
	if (self->conn && self->conn->sock && CC_is_in_trans(self->conn))
		QR_close(self);			// close the cursor if there is one

	QR_free_memory(self);	// safe to call anyway

	//	Should have been freed in the close() but just in case...
	if (self->cursor)
		free(self->cursor);

	/*	Free up column info */
	if (self->fields)
		CI_Destructor(self->fields);

	/*	Free command info (this is from strdup()) */
	if (self->command)
		free(self->command);

	/*	Free notice info (this is from strdup()) */
	if (self->notice)
		free(self->notice);

	free(self);

	mylog("QResult: exit DESTRUCTOR\n");

}

void
QR_set_command(QResultClass *self, char *msg)
{
	if (self->command)
		free(self->command);

	self->command = msg ? strdup(msg) : NULL;
}

void 
QR_set_notice(QResultClass *self, char *msg)
{
	if (self->notice)
		free(self->notice);

	self->notice = msg ? strdup(msg) : NULL;
}

void 
QR_free_memory(QResultClass *self)
{
register int lf, row;
register TupleField *tuple = self->backend_tuples;
int fcount = self->fcount;
int num_fields = self->num_fields;

	mylog("QResult: free memory in, fcount=%d\n", fcount);

	if ( self->backend_tuples) {

		for (row = 0; row < fcount; row++) {
			mylog("row = %d, num_fields = %d\n", row, num_fields);
			for (lf=0; lf < num_fields; lf++) {
				if (tuple[lf].value != NULL) {
					mylog("free [lf=%d] %u\n", lf, tuple[lf].value);
					free(tuple[lf].value);
				}
			}
			tuple += num_fields;  // next row
		}

		free(self->backend_tuples);
		self->backend_tuples = NULL;
	}

	self->fcount = 0;

	mylog("QResult: free memory out\n");
}

//	This function is called by send_query()
char
QR_fetch_tuples(QResultClass *self, ConnectionClass *conn, char *cursor)
{
int tuple_size;

	//	If called from send_query the first time (conn != NULL), 
	//	then set the inTuples state,
	//	and read the tuples.  If conn is NULL,
	//	it implies that we are being called from next_tuple(),
	//	like to get more rows so don't call next_tuple again!
	if (conn != NULL) {
		self->conn = conn;

		mylog("QR_fetch_tuples: cursor = '%s', self->cursor=%u\n",  (cursor==NULL)?"":cursor, self->cursor);

		if (self->cursor)
			free(self->cursor);

		if ( globals.use_declarefetch) {
			if (! cursor || cursor[0] == '\0') {
				self->status = PGRES_INTERNAL_ERROR;
				QR_set_message(self, "Internal Error -- no cursor for fetch");
				return FALSE;
			}
			self->cursor = strdup(cursor);
		}
 
		//	Read the field attributes.
		//	$$$$ Should do some error control HERE! $$$$
		if ( CI_read_fields(self->fields, self->conn)) {
			self->status = PGRES_FIELDS_OK;
			self->num_fields = CI_get_num_fields(self->fields);
		}
		else {
			self->status = PGRES_BAD_RESPONSE;
			QR_set_message(self, "Error reading field information");
			return FALSE;
		}

		mylog("QR_fetch_tuples: past CI_read_fields: num_fields = %d\n", self->num_fields);

		if (globals.use_declarefetch) 
			tuple_size = self->cache_size;
		else
			tuple_size = TUPLE_MALLOC_INC;

		/* allocate memory for the tuple cache */
		mylog("MALLOC: tuple_size = %d, size = %d\n", tuple_size, self->num_fields * sizeof(TupleField) * tuple_size);
		self->backend_tuples = (TupleField *) malloc(self->num_fields * sizeof(TupleField) * tuple_size);
		if ( ! self->backend_tuples) {
			self->status = PGRES_FATAL_ERROR; 
			QR_set_message(self, "Could not get memory for tuple cache.");
			return FALSE;
		}

		self->inTuples = TRUE;


		/*  Force a read to occur in next_tuple */
		self->fcount = tuple_size+1;
		self->fetch_count = tuple_size+1;
		self->base = 0;

		return QR_next_tuple(self);
	}
	else {

		//	Always have to read the field attributes.
		//	But we dont have to reallocate memory for them!

		if ( ! CI_read_fields(NULL, self->conn)) {
			self->status = PGRES_BAD_RESPONSE;
			QR_set_message(self, "Error reading field information");
			return FALSE;
		}
		return TRUE;
	}
}

//	Close the cursor and end the transaction (if no cursors left)
//	We only close cursor/end the transaction if a cursor was used.
int
QR_close(QResultClass *self)
{
QResultClass *res;

	if (globals.use_declarefetch && self->conn && self->cursor) {
		char buf[64];

		sprintf(buf, "close %s", self->cursor);
		mylog("QResult: closing cursor: '%s'\n", buf);

		res = CC_send_query(self->conn, buf, NULL);

		self->inTuples = FALSE;
		self->currTuple = -1;

		free(self->cursor);
		self->cursor = NULL;

		if (res == NULL) {
			self->status = PGRES_FATAL_ERROR;
			QR_set_message(self, "Error closing cursor.");
			return FALSE;
		}

		/*	End the transaction if there are no cursors left on this conn */
		if (CC_cursor_count(self->conn) == 0) {
			mylog("QResult: END transaction on conn=%u\n", self->conn);

			res = CC_send_query(self->conn, "END", NULL);

			CC_set_no_trans(self->conn);

			if (res == NULL) {
				self->status = PGRES_FATAL_ERROR;
				QR_set_message(self, "Error ending transaction.");
				return FALSE;
			}
		}

	}

	return TRUE;
}

//	This function is called by fetch_tuples() AND SQLFetch()
int
QR_next_tuple(QResultClass *self)
{
int id;
QResultClass *res;
SocketClass *sock;
/* Speed up access */
int fetch_count = self->fetch_count;
int fcount = self->fcount;
int fetch_size, offset= 0;
int end_tuple = self->rowset_size + self->base;
char corrected = FALSE;
TupleField *the_tuples = self->backend_tuples;
static char msgbuffer[MAX_MESSAGE_LEN+1];
char cmdbuffer[MAX_MESSAGE_LEN+1];	// QR_set_command() dups this string so dont need static
char fetch[128];
QueryInfo qi;

	if (fetch_count < fcount) {	/* return a row from cache */
		mylog("next_tuple: fetch_count < fcount: returning tuple %d, fcount = %d\n", fetch_count, fcount);
		self->tupleField = the_tuples + (fetch_count * self->num_fields); /* next row */
		self->fetch_count++;
		return TRUE;
	}
	else if (self->fcount < self->cache_size) {   /* last row from cache */
			//	We are done because we didn't even get CACHE_SIZE tuples
		  mylog("next_tuple: fcount < CACHE_SIZE: fcount = %d, fetch_count = %d\n", fcount, fetch_count);
		  self->tupleField = NULL;
		  self->status = PGRES_END_TUPLES;
		  return -1;	/* end of tuples */
	}
	else {	
		/*	See if we need to fetch another group of rows.
			We may be being called from send_query(), and
			if so, don't send another fetch, just fall through
			and read the tuples.
		*/
		self->tupleField = NULL;

		if ( ! self->inTuples) {

			if ( ! globals.use_declarefetch) {
				mylog("next_tuple: ALL_ROWS: done, fcount = %d, fetch_count = %d\n", fcount, fetch_count);
				self->tupleField = NULL;
				self->status = PGRES_END_TUPLES;
				return -1;	/* end of tuples */
			}

			if (self->base == fcount) {		/* not a correction */

				/*	Determine the optimum cache size.  */
				if (globals.fetch_max % self->rowset_size == 0)
					fetch_size = globals.fetch_max;
				else if (self->rowset_size < globals.fetch_max)
					fetch_size = (globals.fetch_max / self->rowset_size) * self->rowset_size;
				else
					fetch_size = self->rowset_size;

				self->cache_size = fetch_size;
				self->fetch_count = 1;		
			} 
			else {	/* need to correct */

				corrected = TRUE;

				fetch_size = end_tuple - fcount;

				self->cache_size += fetch_size;

				offset = self->fetch_count;
				self->fetch_count++;

			}


			self->backend_tuples = (TupleField *) realloc(self->backend_tuples, self->num_fields * sizeof(TupleField) * self->cache_size);
			if ( ! self->backend_tuples) {
				self->status = PGRES_FATAL_ERROR; 
				QR_set_message(self, "Out of memory while reading tuples.");
				return FALSE;
			}
			sprintf(fetch, "fetch %d in %s", fetch_size, self->cursor);

			mylog("next_tuple: sending actual fetch (%d) query '%s'\n", fetch_size, fetch);

			//	don't read ahead for the next tuple (self) !
			qi.row_size = self->cache_size;
			qi.result_in = self;
			qi.cursor = NULL;
			res = CC_send_query(self->conn, fetch, &qi);
			if (res == NULL) {
				self->status = PGRES_FATAL_ERROR;
				QR_set_message(self, "Error fetching next group.");
				return FALSE;
			}
			self->inTuples = TRUE;
		}
		else {
			mylog("next_tuple: inTuples = true, falling through: fcount = %d, fetch_count = %d\n", self->fcount, self->fetch_count);
			/*	This is a pre-fetch (fetching rows right after query
				but before any real SQLFetch() calls.  This is done
				so the field attributes are available.
			*/
			self->fetch_count = 0;
		}
	}

	if ( ! corrected) {
		self->base = 0;
		self->fcount = 0;
	}


	sock = CC_get_socket(self->conn);
	self->tupleField = NULL;

	for ( ; ;) {

		id = SOCK_get_char(sock);

		switch (id) {
		case 'T': /* Tuples within tuples cannot be handled */
			self->status = PGRES_BAD_RESPONSE;
			QR_set_message(self, "Tuples within tuples cannot be handled");
			return FALSE;
		case 'B': /* Tuples in binary format */
		case 'D': /* Tuples in ASCII format  */

			if ( ! globals.use_declarefetch && self->fcount > 0 && ! (self->fcount % TUPLE_MALLOC_INC)) {
				size_t old_size = self->fcount * self->num_fields * sizeof(TupleField);
				mylog("REALLOC: old_size = %d\n", old_size);

				self->backend_tuples = (TupleField *) realloc(self->backend_tuples, old_size + (self->num_fields * sizeof(TupleField) * TUPLE_MALLOC_INC));
				if ( ! self->backend_tuples) {
					self->status = PGRES_FATAL_ERROR; 
					QR_set_message(self, "Out of memory while reading tuples.");
					return FALSE;
				}
			}

			if ( ! QR_read_tuple(self, (char) (id == 0))) {
				self->status = PGRES_BAD_RESPONSE;
				QR_set_message(self, "Error reading the tuple");
				return FALSE;
			}
			
			self->fcount++;
			break;	// continue reading


		case 'C': /* End of tuple list */
			SOCK_get_string(sock, cmdbuffer, MAX_MESSAGE_LEN);
			QR_set_command(self, cmdbuffer);

			mylog("end of tuple list -- setting inUse to false: this = %u\n", self);

			self->inTuples = FALSE;
			if (self->fcount > 0) {

				qlog("    [ fetched %d rows ]\n", self->fcount);
				mylog("_next_tuple: 'C' fetch_max && fcount = %d\n", self->fcount);

				/*  set to first row */
				self->tupleField = self->backend_tuples + (offset * self->num_fields);
				return TRUE;
			} 
			else { //	We are surely done here (we read 0 tuples)
				qlog("    [ fetched 0 rows ]\n");
				mylog("_next_tuple: 'C': DONE (fcount == 0)\n");
				return -1;	/* end of tuples */
			}

		case 'E': /* Error */
			SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
			QR_set_message(self, msgbuffer);
			self->status = PGRES_FATAL_ERROR;

			if ( ! strncmp(msgbuffer, "FATAL", 5))
				CC_set_no_trans(self->conn);

			qlog("ERROR from backend in next_tuple: '%s'\n", msgbuffer);

			return FALSE;

		case 'N': /* Notice */
			SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
			QR_set_message(self, msgbuffer);
			self->status = PGRES_NONFATAL_ERROR;
			qlog("NOTICE from backend in next_tuple: '%s'\n", msgbuffer);
			continue;

		default: /* this should only happen if the backend dumped core */
			mylog("QR_next_tuple: Unexpected result from backend: id = '%c' (%d)\n", id, id);
			qlog("QR_next_tuple: Unexpected result from backend: id = '%c' (%d)\n", id, id);
			QR_set_message(self, "Unexpected result from backend. It probably crashed");
			self->status = PGRES_FATAL_ERROR;
			CC_set_no_trans(self->conn);
			return FALSE;
		}
	}
	return TRUE;
}

char
QR_read_tuple(QResultClass *self, char binary)
{
Int2 field_lf;
TupleField *this_tuplefield;
char bmp, bitmap[MAX_FIELDS];        /* Max. len of the bitmap */
Int2 bitmaplen;                       /* len of the bitmap in bytes */
Int2 bitmap_pos;
Int2 bitcnt;
Int4 len;
char *buffer;
int num_fields = self->num_fields;	// speed up access
SocketClass *sock = CC_get_socket(self->conn);
ColumnInfoClass *flds;


	/* set the current row to read the fields into */
	this_tuplefield = self->backend_tuples + (self->fcount * num_fields);

	bitmaplen = (Int2) num_fields / BYTELEN;
	if ((num_fields % BYTELEN) > 0)
		bitmaplen++;

	/*
		At first the server sends a bitmap that indicates which
		database fields are null
	*/
	SOCK_get_n_char(sock, bitmap, bitmaplen);

	bitmap_pos = 0;
	bitcnt = 0;
	bmp = bitmap[bitmap_pos];

	for(field_lf = 0; field_lf < num_fields; field_lf++) {
		/* Check if the current field is NULL */
		if(!(bmp & 0200)) {
			/* YES, it is NULL ! */
			this_tuplefield[field_lf].len = 0;
			this_tuplefield[field_lf].value = 0;
		} else {
			/*
			NO, the field is not null. so get at first the
			length of the field (four bytes)
			*/
			len = SOCK_get_int(sock, VARHDRSZ);
			if (!binary)
				len -= VARHDRSZ;

			buffer = (char *)malloc(len+1);
			SOCK_get_n_char(sock, buffer, len);
			buffer[len] = '\0';

			mylog("qresult: len=%d, buffer='%s'\n", len, buffer);

			this_tuplefield[field_lf].len = len;
			this_tuplefield[field_lf].value = buffer;

			/*	This can be used to set the longest length of the column for any
				row in the tuple cache.  It would not be accurate for varchar and
				text fields to use this since a tuple cache is only 100 rows.
				Bpchar can be handled since the strlen of all rows is fixed,
				assuming there are not 100 nulls in a row!
			*/

			flds = self->fields;
			if (flds->display_size[field_lf] < len)
				flds->display_size[field_lf] = len;
		}
		/*
		Now adjust for the next bit to be scanned in the
		next loop.
		*/
		bitcnt++;
		if (BYTELEN == bitcnt) {
			bitmap_pos++;
			bmp = bitmap[bitmap_pos];
			bitcnt = 0;
		} else
			bmp <<= 1;
	}
	self->currTuple++;
	return TRUE;
}
