#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#define SQLERRMC_LEN	70

#ifdef __cplusplus
extern		"C"
{
#endif

	struct sqlca
	{
		char		sqlcaid[8];
		long		sqlabc;
		long		sqlcode;
		struct
		{
			int			sqlerrml;
			char		sqlerrmc[SQLERRMC_LEN];
		}			sqlerrm;
		char		sqlerrp[8];
		long		sqlerrd[6];
		/* Element 0: empty						*/
		/* 1: OID of processed tuple if applicable  		*/
		/* 2: number of rows processed	*/
		/* after an INSERT, UPDATE or */
		/* DELETE statement			 */
		/* 3: empty						*/
		/* 4: empty						*/
		/* 5: empty						*/
		char		sqlwarn[8];
		/* Element 0: set to 'W' if at least one other is 'W' */
		/* 1: if 'W' at least one character string	  */
		/* value was truncated when it was		   */
		/* stored into a host variable.			   */
		/* 2: empty									  */
		/* 3: empty									  */
		/* 4: empty									  */
		/* 5: empty									  */
		/* 6: empty									  */
		/* 7: empty									  */

		char		sqlext[8];
	};

	extern struct sqlca sqlca;


#ifdef __cplusplus
}
#endif

#endif
