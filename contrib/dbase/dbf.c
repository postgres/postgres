/* Routines to read and write xBase-files (.dbf)

   By Maarten Boekhold, 29th of oktober 1995

   Modified by Frank Koormann (fkoorman@usf.uni-osnabrueck.de), Jun 10 1996
	prepare dataarea with memset
	get systemtime and set filedate
	set formatstring for real numbers
*/

#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>

#include "dbf.h"

/* open a dbf-file, get it's field-info and store this information */

dbhead *dbf_open(u_char *file, int flags) {
	int				file_no;
	dbhead			*dbh;
	f_descr			*fields;
	dbf_header		*head;
	dbf_field		*fieldc;
	int			t;

	if ((dbh = (dbhead *)malloc(sizeof(dbhead))) == NULL) {
		return (dbhead *)DBF_ERROR;
	}

	if ((head = (dbf_header *)malloc(sizeof(dbf_header))) == NULL) {
		free(dbh);
		return (dbhead *)DBF_ERROR;
	}

	if ((fieldc = (dbf_field *)malloc(sizeof(dbf_field))) == NULL) {
		free(head);
		free(dbh);
		return (dbhead *)DBF_ERROR;
	}

	if ((file_no = open(file, flags)) == -1) {
		free(fieldc);
		free(head);
		free(dbh);
		return (dbhead *)DBF_ERROR;
	}

/* read in the disk-header */

	if (read(file_no, head, sizeof(dbf_header)) == -1) {
		close(file_no);
		free(fieldc);
		free(head);
		free(dbh);
		return (dbhead *)DBF_ERROR;
	}

	if (!(head->dbh_dbt & DBH_NORMAL)) {
        close(file_no);
        free(fieldc);
        free(head);
        free(dbh);
        return (dbhead *)DBF_ERROR;
	}

	dbh->db_fd = file_no;
	if (head->dbh_dbt & DBH_MEMO) {
		dbh->db_memo = 1;
	} else {
		dbh->db_memo = 0;
	}
	dbh->db_year = head->dbh_year;
	dbh->db_month = head->dbh_month;
	dbh->db_day = head->dbh_day;
	dbh->db_hlen = get_short((u_char *)&head->dbh_hlen);
	dbh->db_records = get_long((u_char *)&head->dbh_records);
	dbh->db_currec = 0;
	dbh->db_rlen = get_short((u_char *)&head->dbh_rlen);
	dbh->db_nfields = (dbh->db_hlen - sizeof(dbf_header)) / sizeof(dbf_field);

								/* dbh->db_hlen - sizeof(dbf_header) isn't the
									correct size, cos dbh->hlen is in fact
									a little more cos of the 0x0D (and
									possibly another byte, 0x4E, I have
									seen this somewhere). Because of rounding
									everything turns out right :) */

	if ((fields = (f_descr *)calloc(dbh->db_nfields, sizeof(f_descr)))
		== NULL) {
                close(file_no);
                free(fieldc);
                free(head);
                free(dbh);
                return (dbhead *)DBF_ERROR;
	}

	for (t = 0; t < dbh->db_nfields; t++) {
/* Maybe I have calculated the number of fields incorrectly. This can happen
   when programs reserve lots of space at the end of the header for future
   expansion. This will catch this situation */ 
		if (fields[t].db_name[0] == 0x0D) {
			dbh->db_nfields = t;
			break;
		}
		read(file_no, fieldc, sizeof(dbf_field));
		strncpy(fields[t].db_name, fieldc->dbf_name, DBF_NAMELEN);
		fields[t].db_type = fieldc->dbf_type;	
		fields[t].db_flen = fieldc->dbf_flen;
		fields[t].db_dec  = fieldc->dbf_dec;
	}

	dbh->db_offset = dbh->db_hlen;
	dbh->db_fields = fields;

	if ((dbh->db_buff = (u_char *)malloc(dbh->db_rlen)) == NULL) {
            return (dbhead *)DBF_ERROR;
    }

    free(fieldc);
    free(head);

	return dbh;
}

int	dbf_write_head(dbhead *dbh) {
	dbf_header	head;
	time_t now;
	struct tm *dbf_time;

	if (lseek(dbh->db_fd, 0, SEEK_SET) == -1) {
		return DBF_ERROR;
	}

/* fill up the diskheader */

/* Set dataarea of head to '\0' */
	memset(&head,'\0',sizeof(dbf_header));

	head.dbh_dbt = DBH_NORMAL;
	if (dbh->db_memo) head.dbh_dbt = DBH_MEMO;

	now = time((time_t *)NULL); 
	dbf_time = localtime(&now);
	head.dbh_year = dbf_time->tm_year;
	head.dbh_month = dbf_time->tm_mon + 1; /* Months since January + 1 */
	head.dbh_day = dbf_time->tm_mday;

	put_long(head.dbh_records, dbh->db_records);
	put_short(head.dbh_hlen, dbh->db_hlen);
	put_short(head.dbh_rlen, dbh->db_rlen);
	
	if (write(dbh->db_fd, &head, sizeof(dbf_header)) == -1 ) {
		return DBF_ERROR;
	}

	return 0;
}

int dbf_put_fields(dbhead *dbh) {
	dbf_field	field;
	u_long		t;
	u_char		end = 0x0D;

	if (lseek(dbh->db_fd, sizeof(dbf_header), SEEK_SET) == -1) {
		return DBF_ERROR;
	}

/* Set dataarea of field to '\0' */
	memset(&field,'\0',sizeof(dbf_field));

	for (t = 0; t < dbh->db_nfields; t++) {
		strncpy(field.dbf_name, dbh->db_fields[t].db_name, DBF_NAMELEN - 1);
		field.dbf_type = dbh->db_fields[t].db_type;
		field.dbf_flen = dbh->db_fields[t].db_flen;
		field.dbf_dec = dbh->db_fields[t].db_dec;

		if (write(dbh->db_fd, &field, sizeof(dbf_field)) == -1) {
			return DBF_ERROR;
		}
	}

	if (write(dbh->db_fd, &end, 1) == -1) {
		return DBF_ERROR;
	}

	return 0;
}

int dbf_add_field(dbhead *dbh, u_char *name, u_char type,
								u_char length, u_char dec) {
f_descr	*ptr;
u_char	*foo;
u_long	size, field_no;

	size = (dbh->db_nfields + 1) * sizeof(f_descr);
	if (!(ptr = (f_descr *) realloc(dbh->db_fields, size))) {
		return DBF_ERROR;
	}
	dbh->db_fields = ptr;

	field_no = dbh->db_nfields;
	strncpy(dbh->db_fields[field_no].db_name, name, DBF_NAMELEN);
	dbh->db_fields[field_no].db_type = type;
	dbh->db_fields[field_no].db_flen = length;
	dbh->db_fields[field_no].db_dec = dec;

	dbh->db_nfields++;
	dbh->db_hlen += sizeof(dbf_field);
	dbh->db_rlen += length;

	if (!(foo = (u_char *) realloc(dbh->db_buff, dbh->db_rlen))) {
		return DBF_ERROR;
	}

	dbh->db_buff = foo;

	return 0;
}

dbhead *dbf_open_new(u_char *name, int flags) {
dbhead	*dbh;

	if (!(dbh = (dbhead *)malloc(sizeof(dbhead)))) {
		return (dbhead *)DBF_ERROR;
	}

	if (flags & O_CREAT) {
		if ((dbh->db_fd = open(name, flags, DBF_FILE_MODE)) == -1) {
			free(dbh);
			return (dbhead *)DBF_ERROR;
		}
	} else {
		if ((dbh->db_fd = open(name, flags)) == -1) {
			free(dbh);
			return (dbhead *)DBF_ERROR;
		}
	}
		

	dbh->db_offset = 0;
	dbh->db_memo = 0;
	dbh->db_year = 0;
	dbh->db_month = 0;
	dbh->db_day	= 0;
	dbh->db_hlen = sizeof(dbf_header) + 1;
	dbh->db_records = 0;
	dbh->db_currec = 0;
	dbh->db_rlen = 1;
	dbh->db_nfields = 0;
	dbh->db_buff = NULL;
	dbh->db_fields = (f_descr *)NULL;

	return dbh;
}
	
void dbf_close(dbhead *dbh) {
	int t;

	close(dbh->db_fd);

	for (t = 0; t < dbh->db_nfields; t++) {
		free(&dbh->db_fields[t]);
	}

	if (dbh->db_buff != NULL) {
		free(dbh->db_buff);
	}

	free(dbh);
}
	
int dbf_get_record(dbhead *dbh, field *fields,  u_long rec) {
	u_char  *data;
	int     t, i, offset;
	u_char  *dbffield, *end;

/* calculate at which offset we have to read. *DON'T* forget the
   0x0D which seperates field-descriptions from records!

	Note (april 5 1996): This turns out to be included in db_hlen
*/
	offset = dbh->db_hlen + (rec * dbh->db_rlen);

	if (lseek(dbh->db_fd, offset, SEEK_SET) == -1) {
		lseek(dbh->db_fd, 0, SEEK_SET);
		dbh->db_offset = 0;
		return DBF_ERROR;
	}

	dbh->db_offset 	= offset;
	dbh->db_currec	= rec;
	data = dbh->db_buff;

    read(dbh->db_fd, data, dbh->db_rlen);

    if (data[0] == DBF_DELETED) {
            return DBF_DELETED;
    }

    dbffield = &data[1];
    for (t = 0; t < dbh->db_nfields; t++) {
            strncpy(fields[t].db_name, dbh->db_fields[t].db_name, DBF_NAMELEN);
            fields[t].db_type = dbh->db_fields[t].db_type;
            fields[t].db_flen = dbh->db_fields[t].db_flen;
            fields[t].db_dec  = dbh->db_fields[t].db_dec;

            if (fields[t].db_type == 'C') {
				end = &dbffield[fields[t].db_flen - 1 ];
				i = fields[t].db_flen;
                while (( i > 0) && ((*end < 0x21) || (*end > 0x7E))) {
       	          	end--;
					i--;
                }
				strncpy(fields[t].db_contents, dbffield, i);
                fields[t].db_contents[i] = '\0';
            } else {
				end = dbffield;
				i = fields[t].db_flen;
                while (( i > 0) && ((*end < 0x21) || (*end > 0x7E))) {
					end++;
					i--;
				}
				strncpy(fields[t].db_contents, end, i);
				fields[t].db_contents[i] = '\0';
            }

            dbffield += fields[t].db_flen;
	}

    dbh->db_offset += dbh->db_rlen;

	return DBF_VALID;
}

field *dbf_build_record(dbhead *dbh) {
	int t;
	field	*fields;

	if (!(fields = (field *)calloc(dbh->db_nfields, sizeof(field)))) {
		return (field *)DBF_ERROR;
	}
	
	for ( t = 0; t < dbh->db_nfields; t++) {
		if (!(fields[t].db_contents =
			(u_char *)malloc(dbh->db_fields[t].db_flen + 1))) {
			for (t = 0; t < dbh->db_nfields; t++) {
				if (fields[t].db_contents != 0) {
					free(fields[t].db_contents);
					free(fields);
				}
				return (field *)DBF_ERROR;
			}
		}
		strncpy(fields[t].db_name, dbh->db_fields[t].db_name, DBF_NAMELEN);
		fields[t].db_type = dbh->db_fields[t].db_type;
		fields[t].db_flen = dbh->db_fields[t].db_flen;
		fields[t].db_dec  = dbh->db_fields[t].db_dec;
	}

	return fields;
}

void dbf_free_record(dbhead *dbh, field *rec) {
	int t;

	for ( t = 0; t < dbh->db_nfields; t++) {
		free(rec[t].db_contents);
	}

	free(rec);
}

int dbf_put_record(dbhead *dbh, field *rec, u_long where) {
	u_long	offset, new, idx, t, h, length;
	u_char	*data, end = 0x1a;
	double	fl;
	u_char	foo[128], format[32];

/*	offset:	offset in file for this record
	new:	real offset after lseek
	idx:	index to which place we are inside the 'hardcore'-data for this
			record
	t:		field-counter
	data:	the hardcore-data that is put on disk
	h:		index into the field-part in the hardcore-data
	length:	length of the data to copy
	fl:		a float used to get the right precision with real numbers
	foo:	copy of db_contents when field is not 'C'
	format:	sprintf format-string to get the right precision with real numbers

	NOTE: this declaration of 'foo' can cause overflow when the contents-field
	is longer the 127 chars (which is highly unlikely, cos it is not used
	in text-fields).
*/
/*	REMEMBER THAT THERE'S A 0x1A AT THE END OF THE FILE, SO DON'T
	DO A SEEK_END WITH 0!!!!!! USE -1 !!!!!!!!!!
*/

	if (where > dbh->db_records) {
		if ((new = lseek(dbh->db_fd, -1, SEEK_END)) == -1) {
			return DBF_ERROR;
		}
		dbh->db_records++;
	} else {
		offset = dbh->db_hlen + (where * dbh->db_rlen);
		if ((new = lseek(dbh->db_fd, offset, SEEK_SET)) == -1) {
			return DBF_ERROR;
		}
	}

	dbh->db_offset = new;

	data = dbh->db_buff;

/* Set dataarea of data to ' ' (space) */
	memset(data,' ',dbh->db_rlen);

/*	data[0] = DBF_VALID; */

	idx = 1;
	for (t = 0; t < dbh->db_nfields; t++) {
/* if field is empty, don't do a thing */
	  if (rec[t].db_contents[0] != '\0') {
/*	Handle text */
		if (rec[t].db_type == 'C') {
			if (strlen(rec[t].db_contents) > rec[t].db_flen) {
				length = rec[t].db_flen;
			} else {
				length = strlen(rec[t].db_contents);
			}
			strncpy(data+idx, rec[t].db_contents, length);
		} else {
/* Handle the rest */
/* Numeric is special, because of real numbers */
			if ((rec[t].db_type == 'N') && (rec[t].db_dec != 0)) {
				fl = atof(rec[t].db_contents);
				sprintf(format, "%%.%df", rec[t].db_dec); 
				sprintf(foo, format, fl);
			} else {
				strcpy(foo, rec[t].db_contents);
			}
			if (strlen(foo) > rec[t].db_flen) {
				length = rec[t].db_flen;
			} else {
				length = strlen(foo);
			}
			h = rec[t].db_flen - length;
			strncpy(data+idx+h, foo, length);
		}
	  }
	  idx += rec[t].db_flen;
	}

	if (write(dbh->db_fd, data, dbh->db_rlen) == -1) {
		return DBF_ERROR;
	}

/* There's a 0x1A at the end of a dbf-file */
	if (where == dbh->db_records) {
		if (write(dbh->db_fd, &end, 1) == -1) {
			return DBF_ERROR;
		}
	}

	dbh->db_offset += dbh->db_rlen;

	return 0;
}
