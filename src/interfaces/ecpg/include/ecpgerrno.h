/* src/interfaces/ecpg/include/ecpgerrno.h */

#ifndef _ECPG_ERRNO_H
#define _ECPG_ERRNO_H

#include <errno.h>

/* This is a list of all error codes the embedded SQL program can return */
#define ECPG_NO_ERROR		0
#define ECPG_NOT_FOUND		100

/* system error codes returned by ecpglib get the correct number,
 * but are made negative
 */
#define ECPG_OUT_OF_MEMORY	-ENOMEM

/* first we have a set of ecpg messages, they start at 200 */
#define ECPG_UNSUPPORTED		-200
#define ECPG_TOO_MANY_ARGUMENTS		-201
#define ECPG_TOO_FEW_ARGUMENTS		-202
#define ECPG_TOO_MANY_MATCHES		-203
#define ECPG_INT_FORMAT			-204
#define ECPG_UINT_FORMAT		-205
#define ECPG_FLOAT_FORMAT		-206
#define ECPG_NUMERIC_FORMAT		-207
#define ECPG_INTERVAL_FORMAT		-208
#define ECPG_DATE_FORMAT		-209
#define ECPG_TIMESTAMP_FORMAT		-210
#define ECPG_CONVERT_BOOL		-211
#define ECPG_EMPTY			-212
#define ECPG_MISSING_INDICATOR		-213
#define ECPG_NO_ARRAY			-214
#define ECPG_DATA_NOT_ARRAY		-215
#define ECPG_ARRAY_INSERT		-216

#define ECPG_NO_CONN			-220
#define ECPG_NOT_CONN			-221

#define ECPG_INVALID_STMT		-230

/* dynamic SQL related */
#define ECPG_UNKNOWN_DESCRIPTOR		-240
#define ECPG_INVALID_DESCRIPTOR_INDEX	-241
#define ECPG_UNKNOWN_DESCRIPTOR_ITEM	-242
#define ECPG_VAR_NOT_NUMERIC		-243
#define ECPG_VAR_NOT_CHAR		-244

/* finally the backend error messages, they start at 400 */
#define ECPG_PGSQL			-400
#define ECPG_TRANS			-401
#define ECPG_CONNECT			-402
#define ECPG_DUPLICATE_KEY		-403
#define ECPG_SUBSELECT_NOT_ONE		-404

/* for compatibility we define some different error codes for the same error
 * if adding a new one make sure to not double define it */
#define ECPG_INFORMIX_DUPLICATE_KEY -239
#define ECPG_INFORMIX_SUBSELECT_NOT_ONE -284

/* backend WARNINGs, starting at 600 */
#define ECPG_WARNING_UNRECOGNIZED	   -600
 /* WARNING:  (transaction aborted): queries ignored until END */

 /*
  * WARNING:  current transaction is aborted, queries ignored until end of
  * transaction block
  */
#define ECPG_WARNING_QUERY_IGNORED	   -601
 /* WARNING:  PerformPortalClose: portal "*" not found */
#define ECPG_WARNING_UNKNOWN_PORTAL    -602
 /* WARNING:  BEGIN: already a transaction in progress */
#define ECPG_WARNING_IN_TRANSACTION    -603
 /* WARNING:  AbortTransaction and not in in-progress state */
 /* WARNING:  COMMIT: no transaction in progress */
#define ECPG_WARNING_NO_TRANSACTION    -604
 /* WARNING:  BlankPortalAssignName: portal * already exists */
#define ECPG_WARNING_PORTAL_EXISTS	   -605

#endif							/* !_ECPG_ERRNO_H */
