/*---------
 * Module:			qresult.c
 *
 * Description:		This module contains functions related to
 *					managing result information (i.e, fetching rows
 *					from the backend, managing the tuple cache, etc.)
 *					and retrieving it.	Depending on the situation, a
 *					QResultClass will hold either data from the backend
 *					or a manually built result (see "qresult.h" to
 *					see which functions/macros are for manual or backend
 *					results.  For manually built results, the
 *					QResultClass simply points to TupleList and
 *					ColumnInfo structures, which actually hold the data.
 *
 * Classes:			QResultClass (Functions prefix: "QR_")
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *---------
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


/*
 *	Used for building a Manual Result only
 *	All info functions call this function to create the manual result set.
 */
void
QR_set_num_fields(QResultClass *self, int new_num_fields)
{
	mylog("in QR_set_num_fields\n");

	CI_set_num_fields(self->fields, new_num_fields);
	if (self->manual_tuples)
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


/*
 * CLASS QResult
 */
QResultClass *
QR_Constructor()
{
	QResultClass *rv;

	mylog("in QR_Constructor\n");
	rv = (QResultClass *) malloc(sizeof(QResultClass));

	if (rv != NULL)
	{
		rv->status = PGRES_EMPTY_QUERY;

		/* construct the column info */
		if (!(rv->fields = CI_Constructor()))
		{
			free(rv);
			return NULL;
		}
		rv->manual_tuples = NULL;
		rv->backend_tuples = NULL;
		rv->message = NULL;
		rv->command = NULL;
		rv->notice = NULL;
		rv->conn = NULL;
		rv->next = NULL;
		rv->inTuples = FALSE;
		rv->count_backend_allocated = 0;
		rv->count_keyset_allocated = 0;
		rv->num_total_rows = 0;
		rv->num_backend_rows = 0;
		rv->fetch_count = 0;
		rv->base = 0;
		rv->recent_processed_row_count = -1;
		rv->currTuple = -1;
		rv->num_fields = 0;
		rv->tupleField = NULL;
		rv->cursor = NULL;
		rv->aborted = FALSE;

		rv->cache_size = 0;
		rv->rowset_size = 1;
		rv->haskeyset = 0;
		rv->keyset = NULL;
		rv->rb_alloc = 0;
		rv->rb_count = 0;
		rv->rollback = NULL;
		rv->dl_alloc = 0;
		rv->dl_count = 0;
		rv->deleted = NULL;
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

	/*
	 * If conn is defined, then we may have used "backend_tuples", so in
	 * case we need to, free it up.  Also, close the cursor.
	 */
	if (self->conn && self->conn->sock && CC_is_in_trans(self->conn))
		QR_close(self);			/* close the cursor if there is one */

	QR_free_memory(self);		/* safe to call anyway */

	/* Should have been freed in the close() but just in case... */
	if (self->cursor)
		free(self->cursor);

	/* Free up column info */
	if (self->fields)
		CI_Destructor(self->fields);

	/* Free command info (this is from strdup()) */
	if (self->command)
		free(self->command);

	/* Free notice info (this is from strdup()) */
	if (self->notice)
		free(self->notice);
	/* Destruct the result object in the chain */
	if (self->next)
		QR_Destructor(self->next);

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
	register int lf,
				row;
	register TupleField *tuple = self->backend_tuples;
	int			num_backend_rows = self->num_backend_rows;
	int			num_fields = self->num_fields;

	mylog("QResult: free memory in, fcount=%d\n", num_backend_rows);

	if (self->backend_tuples)
	{
		for (row = 0; row < num_backend_rows; row++)
		{
			mylog("row = %d, num_fields = %d\n", row, num_fields);
			for (lf = 0; lf < num_fields; lf++)
			{
				if (tuple[lf].value != NULL)
				{
					mylog("free [lf=%d] %u\n", lf, tuple[lf].value);
					free(tuple[lf].value);
				}
			}
			tuple += num_fields;	/* next row */
		}

		free(self->backend_tuples);
		self->count_backend_allocated = 0;
		self->backend_tuples = NULL;
	}
	if (self->keyset)
	{
		free(self->keyset);
		self->keyset = NULL;
		self->count_keyset_allocated = 0;
	}
	if (self->rollback)
	{
		free(self->rollback);
		self->rb_alloc = 0;
		self->rb_count = 0;
		self->rollback = NULL;
	}
	if (self->deleted)
	{
		free(self->deleted);
		self->dl_alloc = 0;
		self->dl_count = 0;
		self->deleted = NULL;
	}

	self->num_total_rows = 0;
	self->num_backend_rows = 0;

	mylog("QResult: free memory out\n");
}


/*	This function is called by send_query() */
char
QR_fetch_tuples(QResultClass *self, ConnectionClass *conn, char *cursor)
{
	int			tuple_size;

	/*
	 * If called from send_query the first time (conn != NULL), then set
	 * the inTuples state, and read the tuples.  If conn is NULL, it
	 * implies that we are being called from next_tuple(), like to get
	 * more rows so don't call next_tuple again!
	 */
	if (conn != NULL)
	{
		ConnInfo   *ci = &(conn->connInfo);
		BOOL		fetch_cursor = (ci->drivers.use_declarefetch && cursor && cursor[0]);

		self->conn = conn;

		mylog("QR_fetch_tuples: cursor = '%s', self->cursor=%u\n", (cursor == NULL) ? "" : cursor, self->cursor);

		if (self->cursor)
			free(self->cursor);
		self->cursor = NULL;

		if (fetch_cursor)
		{
			if (!cursor || cursor[0] == '\0')
			{
				self->status = PGRES_INTERNAL_ERROR;
				QR_set_message(self, "Internal Error -- no cursor for fetch");
				return FALSE;
			}
			self->cursor = strdup(cursor);
		}

		/*
		 * Read the field attributes.
		 *
		 * $$$$ Should do some error control HERE! $$$$
		 */
		if (CI_read_fields(self->fields, self->conn))
		{
			self->status = PGRES_FIELDS_OK;
			self->num_fields = CI_get_num_fields(self->fields);
			if (self->haskeyset)
				self->num_fields -= 2;
		}
		else
		{
			self->status = PGRES_BAD_RESPONSE;
			QR_set_message(self, "Error reading field information");
			return FALSE;
		}

		mylog("QR_fetch_tuples: past CI_read_fields: num_fields = %d\n", self->num_fields);

		if (fetch_cursor)
		{
			if (self->cache_size <= 0)
				self->cache_size = ci->drivers.fetch_max;
			tuple_size = self->cache_size;
		}
		else
			tuple_size = TUPLE_MALLOC_INC;

		/* allocate memory for the tuple cache */
		mylog("MALLOC: tuple_size = %d, size = %d\n", tuple_size, self->num_fields * sizeof(TupleField) * tuple_size);
		self->count_backend_allocated = self->count_keyset_allocated = 0;
		if (self->num_fields > 0)
		{
			self->backend_tuples = (TupleField *) malloc(self->num_fields * sizeof(TupleField) * tuple_size);
			if (!self->backend_tuples)
			{
				self->status = PGRES_FATAL_ERROR;
				QR_set_message(self, "Could not get memory for tuple cache.");
				return FALSE;
			}
			self->count_backend_allocated = tuple_size;
		}
		if (self->haskeyset)
		{
			if (self->keyset = (KeySet *) calloc(sizeof(KeySet), tuple_size), !self->keyset)
			{
				self->status = PGRES_FATAL_ERROR;
				QR_set_message(self, "Could not get memory for tuple cache.");
				return FALSE;
			}
			self->count_keyset_allocated = tuple_size;
		}

		self->inTuples = TRUE;

		/* Force a read to occur in next_tuple */
		self->num_total_rows = tuple_size + 1;
		self->num_backend_rows = tuple_size + 1;
		self->fetch_count = tuple_size + 1;
		self->base = 0;

		return QR_next_tuple(self);
	}
	else
	{
		/*
		 * Always have to read the field attributes. But we dont have to
		 * reallocate memory for them!
		 */

		if (!CI_read_fields(NULL, self->conn))
		{
			self->status = PGRES_BAD_RESPONSE;
			QR_set_message(self, "Error reading field information");
			return FALSE;
		}
		return TRUE;
	}
}


/*
 *	Close the cursor and end the transaction (if no cursors left)
 *	We only close cursor/end the transaction if a cursor was used.
 */
int
QR_close(QResultClass *self)
{
	QResultClass *res;

	if (self->conn && self->cursor && self->conn->connInfo.drivers.use_declarefetch)
	{
		char		buf[64];

		sprintf(buf, "close %s", self->cursor);
		mylog("QResult: closing cursor: '%s'\n", buf);

		res = CC_send_query(self->conn, buf, NULL, CLEAR_RESULT_ON_ABORT);

		self->inTuples = FALSE;
		self->currTuple = -1;

		free(self->cursor);
		self->cursor = NULL;

		if (res == NULL)
		{
			self->status = PGRES_FATAL_ERROR;
			QR_set_message(self, "Error closing cursor.");
			return FALSE;
		}
		QR_Destructor(res);

		/* End the transaction if there are no cursors left on this conn */
		if (CC_is_in_autocommit(self->conn) && CC_cursor_count(self->conn) == 0)
		{
			mylog("QResult: END transaction on conn=%u\n", self->conn);

			if (!CC_commit(self->conn))
			{
				self->status = PGRES_FATAL_ERROR;
				QR_set_message(self, "Error ending transaction.");
				return FALSE;
			}
		}
	}

	return TRUE;
}


/*	This function is called by fetch_tuples() AND SQLFetch() */
int
QR_next_tuple(QResultClass *self)
{
	int			id;
	QResultClass *res;
	SocketClass *sock;

	/* Speed up access */
	int			fetch_count = self->fetch_count;
	int			num_backend_rows = self->num_backend_rows;
	int			fetch_size,
				offset = 0;
	int			end_tuple = self->rowset_size + self->base;
	char		corrected = FALSE;
	TupleField *the_tuples = self->backend_tuples;

	/* ERROR_MSG_LENGTH is sufficient */
	static char msgbuffer[ERROR_MSG_LENGTH + 1];

	/* QR_set_command() dups this string so doesn't need static */
	char		cmdbuffer[ERROR_MSG_LENGTH + 1];
	char		fetch[128];
	QueryInfo	qi;
	ConnInfo   *ci = NULL;
	UDWORD		abort_opt;

	if (fetch_count < num_backend_rows)
	{
		/* return a row from cache */
		mylog("next_tuple: fetch_count < fcount: returning tuple %d, fcount = %d\n", fetch_count, num_backend_rows);
		self->tupleField = the_tuples + (fetch_count * self->num_fields);		/* next row */
		self->fetch_count++;
		return TRUE;
	}
	else if (self->num_backend_rows < self->cache_size)
	{
		/* last row from cache */
		/* We are done because we didn't even get CACHE_SIZE tuples */
		mylog("next_tuple: fcount < CACHE_SIZE: fcount = %d, fetch_count = %d\n", num_backend_rows, fetch_count);
		self->tupleField = NULL;
		self->status = PGRES_END_TUPLES;
		/* end of tuples */
		return -1;
	}
	else
	{
		/*
		 * See if we need to fetch another group of rows. We may be being
		 * called from send_query(), and if so, don't send another fetch,
		 * just fall through and read the tuples.
		 */
		self->tupleField = NULL;

		if (!self->inTuples)
		{
			ci = &(self->conn->connInfo);
			if (!self->cursor || !ci->drivers.use_declarefetch)
			{
				mylog("next_tuple: ALL_ROWS: done, fcount = %d, fetch_count = %d\n", self->num_total_rows, fetch_count);
				self->tupleField = NULL;
				self->status = PGRES_END_TUPLES;
				return -1;		/* end of tuples */
			}

			if (self->base == num_backend_rows)
			{
				/* not a correction */
				/* Determine the optimum cache size.  */
				if (ci->drivers.fetch_max % self->rowset_size == 0)
					fetch_size = ci->drivers.fetch_max;
				else if (self->rowset_size < ci->drivers.fetch_max)
					fetch_size = (ci->drivers.fetch_max / self->rowset_size) * self->rowset_size;
				else
					fetch_size = self->rowset_size;

				self->cache_size = fetch_size;
				self->fetch_count = 1;
			}
			else
			{
				/* need to correct */
				corrected = TRUE;

				fetch_size = end_tuple - num_backend_rows;

				self->cache_size += fetch_size;

				offset = self->fetch_count;
				self->fetch_count++;
			}

			if (!self->backend_tuples || self->cache_size > self->count_backend_allocated)
			{
				self->count_backend_allocated = 0;
				if (self->num_fields > 0)
				{
					self->backend_tuples = (TupleField *) realloc(self->backend_tuples,
						self->num_fields * sizeof(TupleField) * self->cache_size);
					if (!self->backend_tuples)
					{
						self->status = PGRES_FATAL_ERROR;
						QR_set_message(self, "Out of memory while reading tuples.");
						return FALSE;
					}
					self->count_backend_allocated = self->cache_size;
				}
			}
			if (self->haskeyset && (!self->keyset || self->cache_size > self->count_keyset_allocated))
			{
				self->count_keyset_allocated = 0;
				self->keyset = (KeySet *) realloc(self->keyset, sizeof(KeySet) * self->cache_size); 
				self->count_keyset_allocated = self->cache_size;
			}
			sprintf(fetch, "fetch %d in %s", fetch_size, self->cursor);

			mylog("next_tuple: sending actual fetch (%d) query '%s'\n", fetch_size, fetch);

			/* don't read ahead for the next tuple (self) ! */
			qi.row_size = self->cache_size;
			qi.result_in = self;
			qi.cursor = NULL;
			res = CC_send_query(self->conn, fetch, &qi, CLEAR_RESULT_ON_ABORT);
			if (res == NULL)
			{
				self->status = PGRES_FATAL_ERROR;
				QR_set_message(self, "Error fetching next group.");
				return FALSE;
			}
			self->inTuples = TRUE;
		}
		else
		{
			mylog("next_tuple: inTuples = true, falling through: fcount = %d, fetch_count = %d\n", self->num_backend_rows, self->fetch_count);

			/*
			 * This is a pre-fetch (fetching rows right after query but
			 * before any real SQLFetch() calls.  This is done so the
			 * field attributes are available.
			 */
			self->fetch_count = 0;
		}
	}

	if (!corrected)
	{
		self->base = 0;
		self->num_total_rows = 0; /* right ? */
		self->num_backend_rows = 0;
	}

	sock = CC_get_socket(self->conn);
	self->tupleField = NULL;
	ci = &(self->conn->connInfo);

	for (;;)
	{
		id = SOCK_get_char(sock);

		switch (id)
		{

			case 'T':			/* Tuples within tuples cannot be handled */
				self->status = PGRES_BAD_RESPONSE;
				QR_set_message(self, "Tuples within tuples cannot be handled");
				return FALSE;
			case 'B':			/* Tuples in binary format */
			case 'D':			/* Tuples in ASCII format  */

				if (!self->cursor || !ci->drivers.use_declarefetch)
				{ 
					if (self->num_fields > 0 &&
					    self->num_total_rows >= self->count_backend_allocated)
					{
						int	tuple_size = self->count_backend_allocated;

						mylog("REALLOC: old_count = %d, size = %d\n", tuple_size, self->num_fields * sizeof(TupleField) * tuple_size);
						tuple_size *= 2;
						self->backend_tuples = (TupleField *) realloc(self->backend_tuples,
					 		tuple_size * self->num_fields * sizeof(TupleField));
						if (!self->backend_tuples)
						{
							self->status = PGRES_FATAL_ERROR;
							QR_set_message(self, "Out of memory while reading tuples.");
							return FALSE;
						}
						self->count_backend_allocated = tuple_size;
					}
					if (self->haskeyset &&
				 	    self->num_total_rows >= self->count_keyset_allocated)
					{
						int	tuple_size = self->count_keyset_allocated;
						tuple_size *= 2;
						self->keyset = (KeySet *) realloc(self->keyset, sizeof(KeySet) * tuple_size);
						self->count_keyset_allocated = tuple_size;
					}
				}

				if (!QR_read_tuple(self, (char) (id == 0)))
				{
					self->status = PGRES_BAD_RESPONSE;
					QR_set_message(self, "Error reading the tuple");
					return FALSE;
				}
				self->num_total_rows++;
				if (self->num_fields > 0)
					self->num_backend_rows++;
				break;			/* continue reading */

			case 'C':			/* End of tuple list */
				SOCK_get_string(sock, cmdbuffer, ERROR_MSG_LENGTH);
				QR_set_command(self, cmdbuffer);

				mylog("end of tuple list -- setting inUse to false: this = %u\n", self);

				self->inTuples = FALSE;
				if (self->num_total_rows > 0)
				{
					qlog("    [ fetched %d rows ]\n", self->num_total_rows);
					mylog("_next_tuple: 'C' fetch_max && fcount = %d\n", self->num_total_rows);

					/* set to first row */
					self->tupleField = self->backend_tuples + (offset * self->num_fields);
					return TRUE;
				}
				else
				{
					/* We are surely done here (we read 0 tuples) */
					qlog("    [ fetched 0 rows ]\n");
					mylog("_next_tuple: 'C': DONE (fcount == 0)\n");
					return -1;	/* end of tuples */
				}

			case 'E':			/* Error */
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				QR_set_message(self, msgbuffer);
				self->status = PGRES_FATAL_ERROR;

				abort_opt = 0;
				if (!strncmp(msgbuffer, "FATAL", 5))
					abort_opt = NO_TRANS | CONN_DEAD;
				CC_on_abort(self->conn, abort_opt);
				QR_set_status(self, PGRES_FATAL_ERROR);
				QR_set_message(self, msgbuffer);
				QR_set_aborted(self, TRUE);

				mylog("ERROR from backend in next_tuple: '%s'\n", msgbuffer);
				qlog("ERROR from backend in next_tuple: '%s'\n", msgbuffer);

				return FALSE;

			case 'N':			/* Notice */
				SOCK_get_string(sock, msgbuffer, ERROR_MSG_LENGTH);
				QR_set_message(self, msgbuffer);
				self->status = PGRES_NONFATAL_ERROR;
				qlog("NOTICE from backend in next_tuple: '%s'\n", msgbuffer);
				continue;

			default:			/* this should only happen if the backend
								 * dumped core */
				mylog("QR_next_tuple: Unexpected result from backend: id = '%c' (%d)\n", id, id);
				qlog("QR_next_tuple: Unexpected result from backend: id = '%c' (%d)\n", id, id);
				QR_set_message(self, "Unexpected result from backend. It probably crashed");
				self->status = PGRES_FATAL_ERROR;
				CC_on_abort(self->conn, NO_TRANS | CONN_DEAD);
				return FALSE;
		}
	}
	return TRUE;
}


char
QR_read_tuple(QResultClass *self, char binary)
{
	Int2		field_lf;
	TupleField *this_tuplefield;
	KeySet	*this_keyset = NULL;
	char		bmp,
				bitmap[MAX_FIELDS];		/* Max. len of the bitmap */
	Int2		bitmaplen;		/* len of the bitmap in bytes */
	Int2		bitmap_pos;
	Int2		bitcnt;
	Int4		len;
	char	   *buffer;
	int		ci_num_fields = QR_NumResultCols(self);	/* speed up access */
	int		num_fields = self->num_fields;	/* speed up access */
	SocketClass *sock = CC_get_socket(self->conn);
	ColumnInfoClass *flds;
	int		effective_cols;
	char		tidoidbuf[32];

	/* set the current row to read the fields into */
	effective_cols = ci_num_fields;
	this_tuplefield = self->backend_tuples + (self->num_backend_rows * num_fields);
	if (self->haskeyset)
	{
		this_keyset = self->keyset + self->num_total_rows;
		this_keyset->status = 0;
		effective_cols -= 2;
	}

	bitmaplen = (Int2) ci_num_fields / BYTELEN;
	if ((ci_num_fields % BYTELEN) > 0)
		bitmaplen++;

	/*
	 * At first the server sends a bitmap that indicates which database
	 * fields are null
	 */
	SOCK_get_n_char(sock, bitmap, bitmaplen);

	bitmap_pos = 0;
	bitcnt = 0;
	bmp = bitmap[bitmap_pos];
	flds = self->fields;

	for (field_lf = 0; field_lf < ci_num_fields; field_lf++)
	{
		/* Check if the current field is NULL */
		if (!(bmp & 0200))
		{
			/* YES, it is NULL ! */
			this_tuplefield[field_lf].len = 0;
			this_tuplefield[field_lf].value = 0;
		}
		else
		{
			/*
			 * NO, the field is not null. so get at first the length of
			 * the field (four bytes)
			 */
			len = SOCK_get_int(sock, VARHDRSZ);
			if (!binary)
				len -= VARHDRSZ;

			if (field_lf >= effective_cols)
				buffer = tidoidbuf;
			else
				buffer = (char *) malloc(len + 1);
			SOCK_get_n_char(sock, buffer, len);
			buffer[len] = '\0';

			mylog("qresult: len=%d, buffer='%s'\n", len, buffer);

			if (field_lf >= effective_cols)
			{
				if (field_lf == effective_cols)
					sscanf(buffer, "(%lu,%hu)",
						&this_keyset->blocknum, &this_keyset->offset);
				else
					this_keyset->oid = strtoul(buffer, NULL, 10);
			}
			else
			{
				this_tuplefield[field_lf].len = len;
				this_tuplefield[field_lf].value = buffer;

			/*
			 * This can be used to set the longest length of the column
			 * for any row in the tuple cache.	It would not be accurate
			 * for varchar and text fields to use this since a tuple cache
			 * is only 100 rows. Bpchar can be handled since the strlen of
			 * all rows is fixed, assuming there are not 100 nulls in a
			 * row!
			 */

				if (flds && flds->display_size && flds->display_size[field_lf] < len)
					flds->display_size[field_lf] = len;
			}
		}

		/*
		 * Now adjust for the next bit to be scanned in the next loop.
		 */
		bitcnt++;
		if (BYTELEN == bitcnt)
		{
			bitmap_pos++;
			bmp = bitmap[bitmap_pos];
			bitcnt = 0;
		}
		else
			bmp <<= 1;
	}
	self->currTuple++;
	return TRUE;
}
