
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

Datum		text_metaphone(PG_FUNCTION_ARGS);
Datum text_metaphone_length(PG_FUNCTION_ARGS);

void phonetic(char *name, char *metaph, int metalen);

#define METAPHONE_LEN 50
#undef METAPHONE_TEST

#define _textin(str) DirectFunctionCall1(textin, CStringGetDatum(str))
#define _textout(str) DatumGetPointer(DirectFunctionCall1(textout, PointerGetDatum(str)))

#define NULLCHAR (char *) 0

char *VOWELS="AEIOU",
     *FRONTV="EIY",   /* special cases for letters in FRONT of these */
     *VARSON="CSPTG", /* variable sound--those modified by adding an "h"    */
     *DOUBLE=".";     /* let these double letters through */

char *excpPAIR="AGKPW", /* exceptions "ae-", "gn-", "kn-", "pn-", "wr-" */
     *nextLTR ="ENNNR";
char *chrptr, *chrptr1;

void phonetic(name,metaph,metalen)
char *name, *metaph;
int metalen;
{

int  ii, jj, silent, hard, Lng, lastChr;

char curLtr, prevLtr, nextLtr, nextLtr2, nextLtr3;

int vowelAfter, vowelBefore, frontvAfter;

char wname[60];
char *ename=wname;

  jj = 0;
  for (ii=0; name[ii] != '\0'; ii++) {
    if ( isalpha(name[ii]) ) {
      ename[jj] = toupper(name[ii]);
      jj++;
    }
  }
  ename[jj] = '\0';

  if (strlen(ename) == 0) return;

                        /* if ae, gn, kn, pn, wr then drop the first letter */
  if ( (chrptr=strchr(excpPAIR,ename[0]) ) != NULLCHAR ) {
    chrptr1 = nextLTR + (chrptr-excpPAIR);
    if ( *chrptr1 == ename[1] )  strcpy(ename,&ename[1]);
  }
                                            /* change x to s */
  if  (ename[0] == 'X') ename[0] = 'S';
                                           /* get rid of the "h" in "wh" */
  if ( strncmp(ename,"WH",2) == 0 ) strcpy(&ename[1], &ename[2]);

  Lng = strlen(ename);
  lastChr = Lng -1;   /* index to last character in string makes code easier*/

                      /* Remove an S from the end of the string */
  if ( ename[lastChr] == 'S' ) {
    ename[lastChr] = '\0';
    Lng = strlen(ename);
    lastChr = Lng -1;
  }

  for (ii=0; ( (strlen(metaph) < metalen) && (ii < Lng) ); ii++) {

    curLtr = ename[ii];

    vowelBefore = FALSE;  prevLtr = ' ';
    if (ii > 0) {
      prevLtr = ename[ii-1];
      if ( strchr(VOWELS,prevLtr) != NULLCHAR ) vowelBefore = TRUE;
    }
                                      /* if first letter is a vowel KEEP it */
    if (ii == 0 && (strchr(VOWELS,curLtr) != NULLCHAR) ) {
      strncat(metaph,&curLtr,1);
      continue;
    }

    vowelAfter = FALSE;   frontvAfter = FALSE;   nextLtr = ' ';
    if ( ii < lastChr ) {
      nextLtr = ename[ii+1];
      if ( strchr(VOWELS,nextLtr) != NULLCHAR ) vowelAfter = TRUE;
      if ( strchr(FRONTV,nextLtr) != NULLCHAR ) frontvAfter = TRUE;
    }
                                 /* skip double letters except ones in list */
    if (curLtr == nextLtr && (strchr(DOUBLE,nextLtr) == NULLCHAR) ) continue;

    nextLtr2 = ' ';
    if (ii < (lastChr-1) ) nextLtr2 = ename[ii+2];

    nextLtr3 = ' ';
    if (ii < (lastChr-2) ) nextLtr3 = ename[ii+3];

    switch (curLtr) {

    case 'B': silent = FALSE;
              if (ii == lastChr && prevLtr == 'M') silent = TRUE;
              if (! silent) strncat(metaph,&curLtr,1);
              break;

                                /*silent -sci-,-sce-,-scy-;  sci-, etc OK*/
    case 'C': if (! (ii > 1 && prevLtr == 'S' && frontvAfter) ) {

                if ( ii > 0 && nextLtr == 'I' && nextLtr2 == 'A' )
                  strncat(metaph,"X",1);
                else
                  if (frontvAfter)
                    strncat(metaph,"S",1);
                  else
                    if (ii > 1 && prevLtr == 'S' && nextLtr == 'H')
                      strncat(metaph,"K",1);
                    else
                      if (nextLtr == 'H')
                        if (ii == 0 && (strchr(VOWELS,nextLtr2) == NULLCHAR) )
                          strncat(metaph,"K",1);
                        else
                          strncat(metaph,"X",1);
                      else
                        if (prevLtr == 'C')
                          strncat(metaph,"C",1);
                        else
                          strncat(metaph,"K",1);
    }
              break;

    case 'D': if (nextLtr == 'G' && (strchr(FRONTV,nextLtr2) != NULLCHAR))
                strncat(metaph,"J",1);
              else
                strncat(metaph,"T",1);
              break;

    case 'G': silent=FALSE;
                     /* SILENT -gh- except for -gh and no vowel after h */
              if ( (ii < (lastChr-1) && nextLtr == 'H')
                          && (strchr(VOWELS,nextLtr2) == NULLCHAR) )
                 silent=TRUE;

              if ( (ii == (lastChr-3) )
                   && nextLtr == 'N' && nextLtr2 == 'E' && nextLtr3 == 'D')
                silent=TRUE;
              else
                if ( (ii == (lastChr-1)) && nextLtr == 'N') silent=TRUE;

              if (prevLtr == 'D' && frontvAfter) silent=TRUE;

              if (prevLtr == 'G')
                hard=TRUE;
              else
                hard=FALSE;

              if (!silent) {
                if (frontvAfter && (! hard) ) 
                  strncat(metaph,"J",1);
                else
                  strncat(metaph,"K",1);
	      }
              break;

    case 'H': silent = FALSE;
              if ( strchr(VARSON,prevLtr) != NULLCHAR ) silent = TRUE;

              if ( vowelBefore && !vowelAfter) silent = TRUE;

              if (!silent) strncat(metaph,&curLtr,1);
              break;

    case 'F':
    case 'J':
    case 'L':
    case 'M':
    case 'N':
    case 'R': strncat(metaph,&curLtr,1);
              break;

    case 'K': if (prevLtr != 'C') strncat(metaph,&curLtr,1);
              break;

    case 'P': if (nextLtr == 'H')
                 strncat(metaph,"F",1);
               else
                 strncat(metaph,"P",1);
              break;

    case 'Q': strncat(metaph,"K",1);
              break;

    case 'S': if (ii > 1 && nextLtr == 'I'
                         && ( nextLtr2 == 'O' || nextLtr2 == 'A') )
                strncat(metaph,"X",1);
              else
                if (nextLtr == 'H')
                  strncat(metaph,"X",1);
                else
                  strncat(metaph,"S",1);
              break;

    case 'T': if (ii > 1 && nextLtr == 'I'
                         && ( nextLtr2 == 'O' || nextLtr2 == 'A') )
                strncat(metaph,"X",1);
              else
                if (nextLtr == 'H')         /* The=0, Tho=T, Withrow=0 */
                  if (ii > 0 || (strchr(VOWELS,nextLtr2) != NULLCHAR) )
                    strncat(metaph,"0",1);
                  else
                    strncat(metaph,"T",1);
                else
                  if (! (ii < (lastChr-2) && nextLtr == 'C' && nextLtr2 == 'H'))
                    strncat(metaph,"T",1);
              break;

    case 'V': strncat(metaph,"F",1);
              break;

    case 'W':
    case 'Y': if (ii < lastChr && vowelAfter) strncat(metaph,&curLtr,1);
              break;

    case 'X': strncat(metaph,"KS",2);
              break;

    case 'Z': strncat(metaph,"S",1);
              break;
    }

  }

/*  DON'T DO THIS NOW, REMOVING "S" IN BEGINNING HAS the same effect
    with plurals, in addition imbedded S's in the Metaphone are included
  Lng = strlen(metaph);
  lastChr = Lng -1;
  if ( metaph[lastChr] == 'S' && Lng >= 3 ) metaph[lastChr] = '\0';
*/

  return;
}


#ifdef METAPHONE_TEST
int
main(int argc, char *argv[])
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s string\n", argv[0]);
		return 1;
	}
	else
	{
		char		output[51]="";

		phonetic(argv[1], output, 50);
		printf("metaphone(%s) = %s\n", argv[1], output);
		return 0;
	}
}

#endif	 /* METAPHONE_TEST */

#ifndef METAPHONE_TEST
/*
 * SQL function: text_metaphone(text) returns text
 */
PG_FUNCTION_INFO_V1(text_metaphone);

Datum
text_metaphone(PG_FUNCTION_ARGS)
{
	char		outstr[51]="";
	char	   *arg;

	arg = _textout(PG_GETARG_TEXT_P(0));

	phonetic(arg, outstr, 50);

	PG_RETURN_TEXT_P(_textin(outstr));
}

/*
	char		outstr[51]="";
	char	   *arg;
	int32   metalen;

	arg = _textout(PG_GETARG_TEXT_P(0));
	metalen = PG_GETARG_INT32(1);


	phonetic(arg, outstr, metalen);
*/

PG_FUNCTION_INFO_V1(text_metaphone_length);

Datum
text_metaphone_length(PG_FUNCTION_ARGS)
{
	char		outstr[51]="";
	char	   *arg;
        int32      metalen;

	arg = _textout(PG_GETARG_TEXT_P(0));
	metalen = PG_GETARG_INT32(1);

	phonetic(arg, outstr, metalen);

	PG_RETURN_TEXT_P(_textin(outstr));
}


#endif	 /* not METAPHONE_TEST */
