/* header-file for dbf.c
   declares routines for reading and writing xBase-files (.dbf), and
   associated structures

   Maarten Boekhold (maarten.boekhold@reuters.com) 29 oktober 1995
*/

#ifndef _DBF_H
#define _DBF_H

#include <sys/types.h>

/**********************************************************************

		The DBF-part

***********************************************************************/

#define DBF_FILE_MODE	0644

/* byte offsets for date in dbh_date */

#define DBH_DATE_YEAR	0
#define DBH_DATE_MONTH	1
#define DBH_DATE_DAY	2

/* maximum fieldname-length */

#define DBF_NAMELEN 11

/* magic-cookies for the file */

#define DBH_NORMAL	0x03
#define DBH_MEMO	0x83

/* magic-cookies for the fields */

#define DBF_ERROR	-1
#define DBF_VALID	0x20
#define DBF_DELETED 0x2A

/* diskheader */

typedef struct
{
	u_char		dbh_dbt;		/* indentification field */
	u_char		dbh_year;		/* last modification-date */
	u_char		dbh_month;
	u_char		dbh_day;
	u_char		dbh_records[4]; /* number of records */
	u_char		dbh_hlen[2];	/* length of this header */
	u_char		dbh_rlen[2];	/* length of a record */
	u_char		dbh_stub[20];	/* misc stuff we don't need */
}	dbf_header;

/* disk field-description */

typedef struct
{
	u_char		dbf_name[DBF_NAMELEN];	/* field-name terminated with \0 */
	u_char		dbf_type;		/* field-type */
	u_char		dbf_reserved[4];	/* some reserved stuff */
	u_char		dbf_flen;		/* field-length */
	u_char		dbf_dec;		/* number of decimal positions if type is
								 * 'N' */
	u_char		dbf_stub[14];	/* stuff we don't need */
}	dbf_field;

/* memory field-description */

typedef struct
{
	u_char		db_name[DBF_NAMELEN];	/* field-name terminated with \0 */
	u_char		db_type;		/* field-type */
	u_char		db_flen;		/* field-length */
	u_char		db_dec;			/* number of decimal positions */
}	f_descr;

/* memory dfb-header */

typedef struct
{
	int			db_fd;			/* file-descriptor */
	u_long		db_offset;		/* current offset in file */
	u_char		db_memo;		/* memo-file present */
	u_char		db_year;		/* last update as YYMMDD */
	u_char		db_month;
	u_char		db_day;
	u_long		db_hlen;		/* length of the diskheader, for
								 * calculating the offsets */
	u_long		db_records;		/* number of records */
	u_long		db_currec;		/* current record-number starting at 0 */
	u_short		db_rlen;		/* length of the record */
	u_char		db_nfields;		/* number of fields */
	u_char	   *db_buff;		/* record-buffer to save malloc()'s */
	f_descr    *db_fields;		/* pointer to an array of field-
								 * descriptions */
}	dbhead;

/* structure that contains everything a user wants from a field, including
   the contents (in ASCII). Warning! db_flen may be bigger than the actual
   length of db_name! This is because a field doesn't have to be completely
   filled */

typedef struct
{
	u_char		db_name[DBF_NAMELEN];	/* field-name terminated with \0 */
	u_char		db_type;		/* field-type */
	u_char		db_flen;		/* field-length */
	u_char		db_dec;			/* number of decimal positions */
	u_char	   *db_contents;	/* contents of the field in ASCII */
}	field;

/* prototypes for functions */

extern dbhead *dbf_open(u_char *file, int flags);
extern int	dbf_write_head(dbhead * dbh);
extern int	dbf_put_fields(dbhead * dbh);
extern int dbf_add_field(dbhead * dbh, u_char *name, u_char type,
			  u_char length, u_char dec);
extern dbhead *dbf_open_new(u_char *name, int flags);
extern void dbf_close(dbhead * dbh);
extern int	dbf_get_record(dbhead * dbh, field * fields, u_long rec);
extern field *dbf_build_record(dbhead * dbh);
extern void dbf_free_record(dbhead * dbh, field * fields);
extern int	dbf_put_record(dbhead * dbh, field * rec, u_long where);

/*********************************************************************

		The endian-part

***********************************************************************/

extern long get_long(u_char *cp);
extern void put_long(u_char *cp, long lval);
extern short get_short(u_char *cp);
extern void put_short(u_char *cp, short lval);

#endif   /* _DBF_H */
