/*
 *  Edmund Mergl <E.Mergl@bawue.de>
 *
 *  $Id: oracle_compat.c,v 1.4 1997/03/16 01:17:49 scrappy Exp $
 *
 */


#include <ctype.h>
#include "postgres.h"


/********************************************************************
 *
 * LOWER
 *
 * Syntax:
 *
 *   text *LOWER(text *string)
 *
 * Purpose:
 *
 *   Returns string, with all letters forced to lowercase.
 *
 ********************************************************************/

text *
LOWER(text *string)
{
  text *ret;
  char *ptr, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ret = (text *)malloc(VARSIZE(string));
  VARSIZE(ret) = VARSIZE(string);

  ptr     = VARDATA(string);
  ptr_ret = VARDATA(ret);

  while (m--) {
    *ptr_ret++ = tolower(*ptr++);
  }

  return ret;
}


/********************************************************************
 *
 * UPPER
 *
 * Syntax:
 *
 *   text *UPPER(text *string)
 *
 * Purpose:
 *
 *   Returns string, with all letters forced to uppercase.
 *
 ********************************************************************/

text *
UPPER(text *string)
{
  text *ret;
  char *ptr, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ret = (text *)malloc(VARSIZE(string));
  VARSIZE(ret) = VARSIZE(string);

  ptr     = VARDATA(string);
  ptr_ret = VARDATA(ret);

  while (m--) {
    *ptr_ret++ = toupper(*ptr++);
  }

  return ret;
}


/********************************************************************
 *
 * INITCAP
 *
 * Syntax:
 *
 *   text *INITCAP(text *string)
 *
 * Purpose:
 *
 *   Returns string, with first letter of each word in uppercase,
 *   all other letters in lowercase. A word is delimited by white
 *   space.
 *
 ********************************************************************/

text *
INITCAP(text *string)
{
  text *ret;
  char *ptr, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ret = (text *)malloc(VARSIZE(string));
  VARSIZE(ret) = VARSIZE(string);

  ptr  = VARDATA(string);
  ptr_ret = VARDATA(ret);

  *ptr_ret++ = toupper(*ptr++);
  --m;

  while (m--) {
    if (*(ptr_ret - 1) == ' ' || *(ptr_ret - 1) == '	') {
      *ptr_ret++ = toupper(*ptr++);
    } else {
      *ptr_ret++ = tolower(*ptr++);
    }
  }

  return ret;
}


/********************************************************************
 *
 * LPAD
 *
 * Syntax:
 *
 *   text *LPAD(text *string1, int4 len, text *string2)
 *
 * Purpose:
 *
 *   Returns string1, left-padded to length len with the sequence of
 *   characters in string2.
 *
 ********************************************************************/

text *
LPAD(text *string1, int4 len, text *string2)
{
  text *ret;
  char *ptr1, *ptr2, *ptr_ret;
  int m, n;

  m = len - VARSIZE(string1) + VARHDRSZ;

  if (m <= 0 || (VARSIZE(string2) - VARHDRSZ) <= 0) {
    return string1;
  }

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  ptr2    = VARDATA(string2);
  ptr_ret = VARDATA(ret);

  while (m--) {
    *ptr_ret++ = *ptr2;
    ptr2 = ptr2 == VARDATA(string2) + VARSIZE(string2) - VARHDRSZ - 1 ? VARDATA(string2) : ++ptr2;
  }

  n    = VARSIZE(string1) - VARHDRSZ;
  ptr1 = VARDATA(string1);

  while (n--) {
    *ptr_ret++ = *ptr1++;
  }

  return ret;
}


/********************************************************************
 *
 * RPAD
 *
 * Syntax:
 *
 *   text *RPAD(text *string1, int4 len, text *string2)
 *
 * Purpose:
 *
 *   Returns string1, right-padded to length len with the sequence of
 *   characters in string2.
 *
 ********************************************************************/

text *
RPAD(text *string1, int4 len, text *string2)
{
  text *ret;
  char *ptr1, *ptr2, *ptr_ret;
  int m, n;

  m = len - VARSIZE(string1) + VARHDRSZ;

  if (m <= 0 || (VARSIZE(string2) - VARHDRSZ) <= 0) {
    return string1;
  }

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  n       = VARSIZE(string1) - VARHDRSZ;
  ptr1    = VARDATA(string1);
  ptr_ret = VARDATA(ret);

  while (n--) {
    *ptr_ret++ = *ptr1++;
  }

  ptr2  = VARDATA(string2);

  while (m--) {
    *ptr_ret++ = *ptr2;
    ptr2 = ptr2 == VARDATA(string2) + VARSIZE(string2) - VARHDRSZ - 1 ? VARDATA(string2) : ++ptr2;
  }

  return ret;
}


/********************************************************************
 *
 * LTRIM
 *
 * Syntax:
 *
 *   text *LTRIM(text *string, text *set)
 *
 * Purpose:
 *
 *   Returns string with initial characters removed up to the first
 *   character not in set.
 *
 ********************************************************************/

text *
LTRIM(text *string, text *set)
{
  text *ret;
  char *ptr, *ptr2, *end2, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0 || VARSIZE(set) - VARHDRSZ <= 0) {
    return string;
  }

  ptr  = VARDATA(string);
  ptr2 = VARDATA(set);
  end2 = VARDATA(set) + VARSIZE(set) - VARHDRSZ - 1;
  
  while (m--) {
    while (ptr2 <= end2) {
      if (*ptr == *ptr2) {
        break;
      }
      ++ptr2;
    }
    if (*ptr != *ptr2) {
      break;
    }
    ptr++;
    ptr2 = VARDATA(set);
  }

  ++m;

  ret = (text *)malloc(VARHDRSZ + m);
  VARSIZE(ret) = VARHDRSZ + m;

  ptr_ret = VARDATA(ret);

  while (m--) {
    *ptr_ret++ = *ptr++;
  }

  return ret;
}


/********************************************************************
 *
 * RTRIM
 *
 * Syntax:
 *
 *   text *RTRIM(text *string, text *set)
 *
 * Purpose:
 *
 *   Returns string with final characters removed after the last
 *   character not in set.
 *
 ********************************************************************/

text *
RTRIM(text *string, text *set)
{
  text *ret;
  char *ptr, *ptr2, *end2, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0 || VARSIZE(set) - VARHDRSZ <= 0) {
    return string;
  }

  ptr   = VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1;
  ptr2  = VARDATA(set);
  end2  = VARDATA(set)    + VARSIZE(set)    - VARHDRSZ - 1;
  
  while (m--) {
    while (ptr2 <= end2) {
      if (*ptr == *ptr2) {
        break;
      }
      ++ptr2;
    }
    if (*ptr != *ptr2) {
      break;
    }
    --ptr;
    ptr2 = VARDATA(set);
  }

  ++m;

  ret = (text *)malloc(VARHDRSZ + m);
  VARSIZE(ret) = VARHDRSZ + m;

  ptr_ret = VARDATA(ret) + m - 1;

  while (m--) {
    *ptr_ret-- = *ptr--;
  }

  return ret;
}


/********************************************************************
 *
 * SUBSTR
 *
 * Syntax:
 *
 *   text *SUBSTR(text *string, int4 m, int4 n)
 *
 * Purpose:
 *
 *   Returns a portion of string, beginning at character m, n
 *   characters long. The first position of string is 1.
 *
 ********************************************************************/

text *
SUBSTR(text *string, int4 m, int4 n)
{
  text *ret;
  char *ptr, *ptr_ret;
  int len;

  len = VARSIZE(string) - VARHDRSZ - m;

  if (m <= 0 || n <= 0 || len <= 0) {
    return string;
  }

  len = len + 1 < n ? len + 1 : n;

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  ptr     = VARDATA(string) + m - 1;
  ptr_ret = VARDATA(ret);

  while (len--) {
    *ptr_ret++ = *ptr++;
  }

  return ret;
}


/********************************************************************
 *
 * TRANSLATE
 *
 * Syntax:
 *
 *   text *TRANSLATE(text *string, char from, char to)
 *
 * Purpose:
 *
 *   Returns string after replacing all occurences of from with
 *   the corresponding character in to. TRANSLATE will not remove
 *    characters.
 *
 ********************************************************************/

text *
TRANSLATE(text *string, char from, char to)
{
  text *ret;
  char *ptr, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ret = (text *)malloc(VARSIZE(string));
  VARSIZE(ret) = VARSIZE(string);

  ptr     = VARDATA(string);
  ptr_ret = VARDATA(ret);

  while (m--) {
    *ptr_ret++ = *ptr == from ? to : *ptr;
    ptr++;
  }

  return ret;
}

#if 0

/********************************************************************
 *
 * Functions in lower case are the equivalents with the last 
 * parameter beeing set to a reasonable default. 
 *
 ********************************************************************/


/********************************************************************
 *
 * lpad
 *
 * Syntax:
 *
 *   text *lpad(text *string1, int4 len)
 *
 * Purpose:
 *
 *   Returns string1, left-padded to length len with blanks.
 *
 ********************************************************************/

text *
lpad(text *string1, int4 len)
{
  text *ret;
  char *ptr1, *ptr_ret;
  int m, n;

  m = len - VARSIZE(string1) + VARHDRSZ;

  if (m <= 0) {
    return string1;
  }

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  ptr_ret = VARDATA(ret);

  while (m--) {
    *ptr_ret++ = ' ';
  }

  n    = VARSIZE(string1) - VARHDRSZ;
  ptr1 = VARDATA(string1);

  while (n--) {
    *ptr_ret++ = *ptr1++;
  }

  return ret;
}


/********************************************************************
 *
 * rpad
 *
 * Syntax:
 *
 *   text *rpad(text *string1, int4 len)
 *
 * Purpose:
 *
 *   Returns string1, right-padded to length len with blanks.
 *
 ********************************************************************/

text *
rpad(text *string1, int4 len)
{
  text *ret;
  char *ptr1, *ptr_ret;
  int m, n;

  m = len - VARSIZE(string1) + VARHDRSZ;

  if (m <= 0) {
    return string1;
  }

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  n       = VARSIZE(string1) - VARHDRSZ;
  ptr1    = VARDATA(string1);
  ptr_ret = VARDATA(ret);

  while (n--) {
    *ptr_ret++ = *ptr1++;
  }

  while (m--) {
    *ptr_ret++ = ' ';
  }

  return ret;
}


/********************************************************************
 *
 * ltrim
 *
 * Syntax:
 *
 *   text *ltrim(text *string)
 *
 * Purpose:
 *
 *   Returns string with initial blanks removed.
 *
 ********************************************************************/

text *
ltrim(text *string)
{
  text *ret;
  char *ptr, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ptr = VARDATA(string);
  
  while (m--) {
    if (*ptr != ' ') {
      break;
    }
    ++ptr;
  }

  ++m;

  ret = (text *)malloc(VARHDRSZ + m);
  VARSIZE(ret) = VARHDRSZ + m;

  ptr_ret = VARDATA(ret);

  while (m--) {
    *ptr_ret++ = *ptr++;
  }

  return ret;
}


/********************************************************************
 *
 * rtrim
 *
 * Syntax:
 *
 *   text *rtrim(text *string)
 *
 * Purpose:
 *
 *   Returns string with final blanks removed.
 *
 ********************************************************************/

text *
rtrim(text *string)
{
  text *ret;
  char *ptr, *ptr_ret;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ptr = VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1;
  
  while (m--) {
    if (*ptr != ' ') {
      break;
    }
    --ptr;
  }

  ++m;

  ret = (text *)malloc(VARHDRSZ + m);
  VARSIZE(ret) = VARHDRSZ + m;

  ptr_ret = VARDATA(ret) + m - 1;

  while (m--) {
    *ptr_ret-- = *ptr--;
  }

  return ret;
}


/********************************************************************
 *
 * substr
 *
 * Syntax:
 *
 *   text *substr(text *string, int4 m)
 *
 * Purpose:
 *
 *   Returns a portion of string, beginning at character m, up to
 *   the end of string. The first position of string is 1.
 *
 ********************************************************************/

text *
substr(text *string, int4 m)
{
  text *ret;
  char *ptr, *ptr_ret;
  int len;

  len = VARSIZE(string) - VARHDRSZ - m;

  if (m <= 0 || len <= 0) {
    return string;
  }

  len = len + 1;

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  ptr      = VARDATA(string) + m - 1;
  ptr_ret  = VARDATA(ret);

  while (len--) {
    *ptr_ret++ = *ptr++;
  }

  return ret;
}

#endif 

/* EOF */
