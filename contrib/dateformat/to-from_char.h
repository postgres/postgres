
#ifndef TO_FROM_CHAR_H
#define TO_FROM_CHAR_H

/*------ 
 * For postgres 
 *------ 
 */
extern text	*to_char(DateTime *dt, text *format);
extern DateTime *from_char(text *date_str, text *format);
extern DateADT	to_date(text *date_str, text *format);

extern text 	*ordinal(int4 num, text *type);

extern char	*months_full[];		/* full months name 		*/
extern char	*rm_months[];		/* roman numeral of months	*/

#endif