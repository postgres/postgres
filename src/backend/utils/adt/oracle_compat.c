/*
 *  Edmund Mergl <E.Mergl@bawue.de>
 *
 *  $Id: oracle_compat.c,v 1.1 1997/03/04 05:32:06 scrappy Exp $
 *
 */


#include <ctype.h>
#include "postgres.h"


/********************************************************************
 *
 * lower
 *
 * Syntax:
 *
 *   text *lower(text *string)
 *
 * Purpose:
 *
 *   Returns string, with all letters forced to lowercase.
 *
 ********************************************************************/

text *
lower(text *string)
{
  char *ptr;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ptr = VARDATA(string);

  while (m--) {
    *ptr = tolower(*ptr);
    ++ptr;
  }

  return string;
}


/********************************************************************
 *
 * upper
 *
 * Syntax:
 *
 *   text *upper(text *string)
 *
 * Purpose:
 *
 *   Returns string, with all letters forced to uppercase.
 *
 ********************************************************************/

text *
upper(text *string)
{
  char *ptr;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ptr = VARDATA(string);

  while (m--) {
    *ptr = toupper(*ptr);
    ++ptr;
  }

  return string;
}


/********************************************************************
 *
 * initcap
 *
 * Syntax:
 *
 *   text *initcap(text *string)
 *
 * Purpose:
 *
 *   Returns string, with first letter of each word in uppercase,
 *   all other letters in lowercase. A word is delimited by white
 *   space.
 *
 ********************************************************************/

text *
initcap(text *string)
{
  char *ptr;
  int m;

  m = VARSIZE(string) - VARHDRSZ;

  if (m <= 0) {
    return string;
  }

  ptr = VARDATA(string);

  *ptr = toupper(*ptr);
  ++ptr;
  --m;

  while (m--) {
    if (*(ptr - 1) == ' ' || *(ptr - 1) == '	') {
      *ptr = toupper(*ptr);
    } else {
      *ptr = tolower(*ptr);
    }
    ++ptr;
  }

  return string;
}


/********************************************************************
 *
 * lpad
 *
 * Syntax:
 *
 *   text *lpad(text *string1, int4 len, text *string2)
 *
 * Purpose:
 *
 *   Returns string1, left-padded to length len with the sequence of
 *   characters in string2.
 *
 ********************************************************************/

text *
lpad(text *string1, int4 len, text *string2)
{
  text *ret;
  char *ptr, *ptr1, *ptr2;
  int m, n;

  m = len - VARSIZE(string1) + VARHDRSZ;

  if (m <= 0 || (VARSIZE(string2) - VARHDRSZ) <= 0) {
    return string1;
  }

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  ptr   = VARDATA(ret);
  ptr2  = VARDATA(string2);

  while (m--) {
    *ptr++ = *ptr2;
    ptr2 = ptr2 == VARDATA(string2) + VARSIZE(string2) - VARHDRSZ - 1 ? VARDATA(string2) : ++ptr2;
  }

  n    = VARSIZE(string1) - VARHDRSZ;
  ptr1 = VARDATA(string1);

  while (n--) {
    *ptr++ = *ptr1++;
  }

  return ret;
}


/********************************************************************
 *
 * rpad
 *
 * Syntax:
 *
 *   text *rpad(text *string1, int4 len, text *string2)
 *
 * Purpose:
 *
 *   Returns string1, right-padded to length len with the sequence of
 *   characters in string2.
 *
 ********************************************************************/

text *
rpad(text *string1, int4 len, text *string2)
{
  text *ret;
  char *ptr, *ptr1, *ptr2;
  int m, n;

  m = len - VARSIZE(string1) + VARHDRSZ;

  if (m <= 0 || (VARSIZE(string2) - VARHDRSZ) <= 0) {
    return string1;
  }

  ret = (text *)malloc(VARHDRSZ + len);
  VARSIZE(ret) = VARHDRSZ + len;

  n    = VARSIZE(string1) - VARHDRSZ;
  ptr  = VARDATA(ret);
  ptr1 = VARDATA(string1);

  while (n--) {
    *ptr++ = *ptr1++;
  }

  ptr2  = VARDATA(string2);

  while (m--) {
    *ptr++ = *ptr2;
    ptr2 = ptr2 == VARDATA(string2) + VARSIZE(string2) - VARHDRSZ - 1 ? VARDATA(string2) : ++ptr2;
  }

  return ret;
}


/********************************************************************
 *
 * ltrim
 *
 * Syntax:
 *
 *   text *ltrim(text *string, text *set)
 *
 * Purpose:
 *
 *   Returns string with initial characters removed up to the first
 *   character not in set.
 *
 ********************************************************************/

text *
ltrim(text *string, text *set)
{
  char *ptr, *ptr2, *end2;
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
    ++ptr;
    ptr2 = VARDATA(set);
  }

  m    = VARDATA(string) + VARSIZE(string) - VARHDRSZ - ptr + 1;
  ptr2 = VARDATA(string);

  while (m--) {
    *ptr2++ = *ptr++;
  }

  VARSIZE(string) -= ptr - ptr2;

  return string;
}


/********************************************************************
 *
 * rtrim
 *
 * Syntax:
 *
 *   text *rtrim(text *string, text *set)
 *
 * Purpose:
 *
 *   Returns string with final characters removed after the last
 *   character not in set.
 *
 ********************************************************************/

text *
rtrim(text *string, text *set)
{
  char *ptr, *ptr2, *end2;
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

  VARSIZE(string) -= VARDATA(string) + VARSIZE(string) - VARHDRSZ - 1 - ptr;

  return string;
}


/********************************************************************
 *
 * substr
 *
 * Syntax:
 *
 *   text *substr(text *string, int4 m, int4 n)
 *
 * Purpose:
 *
 *   Returns a portion of string, beginning at character m, n
 *   characters long. The first position of string is 1.
 *
 ********************************************************************/

text *
substr(text *string, int4 m, int4 n)
{
  char *ptr, *ptr1;
  int len;

  len = VARSIZE(string) - VARHDRSZ - m;

  if (m <= 0 || n <= 0 || len <= 0) {
    return string;
  }

  ptr  = VARDATA(string);
  ptr1 = ptr + m - 1;
  len  = len + 1 < n ? len + 1 : n;

  while (len--) {
    *ptr++ = *ptr1++;
  }

  VARSIZE(string) -= ptr1 - ptr;

  return string;
}


/********************************************************************
 *
 * translate
 *
 * Syntax:
 *
 *   text *translate(text *string, char from, char to)
 *
 * Purpose:
 *
 *   Returns string after replacing all occurences of from with
 *   the corresponding character in to. TRANSLATE will not remove
 *    characters.
 *
 ********************************************************************/

text *
translate(text *string, char from, char to)
{
  char *ptr;
  int m;

  m   = VARSIZE(string) - VARHDRSZ;
  ptr = VARDATA(string);

  while (m--) {
    if (*ptr == from) {
      *ptr = to;
    }
    ++ptr;
  }

  return string;
}


/* EOF */
