#include "postgres.h"
#include "varbit.h"
#include <stdio.h>

const int numb = 8;
/*
const char *b[] = { "B0010", "B11011011", "B0001", "X3F12", "X27", "B",
                    "X11", "B100111"};
int atttypmod[] = {-1, -1, -1,-1,-1,-1,-1,-1 };
*/
const char *b[] = { "B0010", "B11011011", "B10001", "X3D12", "X27", "B",
                    "X11", "B100111"};
int atttypmod[] = { 7, 9, 6, 18, 11, 6, -1, -1 };


void print_details (unsigned char *s) 
{
  int i;
  printf ("Length in bytes    : %d\n",VARSIZE(s));
  printf ("Length of bitstring: %d\n",VARBITLEN(s));
  for (i=8; i<VARSIZE(s); i++) 
    printf ("%X%X ",s[i]>>4,s[i]&0xF);
  printf("\n");
}

void
main () 
{
  int i, j;
  char *s[numb];

    for (i=0; i<numb; i++) {
    printf ("Input: %s\n",b[i]);
    s[i] = zpbitin(b[i], 0, atttypmod[i]);
    //print_details(s[i]);
    printf ("%s = %s\n",zpbitout(s[i]),zpbitsout(s[i]));
  }

  printf ("\nCOMPARISONS:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s <=> %s = %d\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     bitcmp(s[i],s[j]));

  printf ("\nCONCATENATION:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s || %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitcat(s[i],s[j])));

  printf("\nSUBSTR:\n");
  printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),1,8,
	 zpbitsout(bitsubstr(s[3],1,8)));
  printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),9,8,
	 zpbitsout(bitsubstr(s[3],9,8)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),1,9,
	 zpbitsout(bitsubstr(s[3],1,9)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),3,5,
	 zpbitsout(bitsubstr(s[3],3,5)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),3,9,
	 zpbitsout(bitsubstr(s[3],3,9)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),3,17,
	 zpbitsout(bitsubstr(s[3],3,17)));
  printf ("\nLOGICAL AND:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s & %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitand(s[i],s[j])));

  printf ("\nLOGICAL OR:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s | %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitor(s[i],s[j])));

  printf ("\nLOGICAL XOR:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s ^ %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitxor(s[i],s[j])));
  
  printf ("\nLOGICAL NOT:\n");
  for (i=0; i<numb; i++)
    printf("~%s = %s\n",zpbitsout(s[i]),zpbitsout(bitnot(s[i])));


  printf ("\nSHIFT LEFT:\n");
  for (i=0; i<numb; i++) {
    printf("%s\n",zpbitsout(s[i]));
    for (j=0; j<=VARBITLEN(s[i]); j++)
      printf("\t%3d\t%s\n",j,zpbitsout(bitshiftleft(s[i],j)));
  }

  printf ("\nSHIFT RIGHT:\n");
  for (i=0; i<numb; i++) {
    printf("%s\n",zpbitsout(s[i]));
    for (j=0; j<=VARBITLEN(s[i]); j++)
      printf("\t%3d\t%s\n",j,zpbitsout(bitshiftright(s[i],j)));
  }

  printf ("\n\n ********** VARYING **********\n");
  for (i=0; i<numb; i++) {
    printf ("Input: %s\n",b[i]);
    s[i] = varbitin(b[i], 0, atttypmod[i]);
    /*print_details(s);*/
    printf ("%s\n",zpbitout(s[i]));
    printf ("%s\n",zpbitsout(s[i]));
  }

  printf ("\nCOMPARISONS:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s <=> %s = %d\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     bitcmp(s[i],s[j]));

  printf ("\nCONCATENATION:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s || %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitcat(s[i],s[j])));

  printf("\nSUBSTR:\n");
  printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),1,8,
	 zpbitsout(bitsubstr(s[3],1,8)));
  printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),9,8,
	 zpbitsout(bitsubstr(s[3],9,8)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),1,9,
	 zpbitsout(bitsubstr(s[3],1,9)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),3,5,
	 zpbitsout(bitsubstr(s[3],3,5)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),3,9,
	 zpbitsout(bitsubstr(s[3],3,9)));
 printf("%s (%d,%d) => %s\n",zpbitsout(s[3]),3,17,
	 zpbitsout(bitsubstr(s[3],3,17)));
  printf ("\nLOGICAL AND:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s & %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitand(s[i],s[j])));

  printf ("\nLOGICAL OR:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s | %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitor(s[i],s[j])));

  printf ("\nLOGICAL XOR:\n");
  for (i=0; i<numb; i++)
    for (j=i+1; j<numb; j++)
      printf("%s ^ %s = %s\n",zpbitsout(s[i]),zpbitsout(s[j]),
	     zpbitsout(bitxor(s[i],s[j])));
  
  printf ("\nLOGICAL NOT:\n");
  for (i=0; i<numb; i++)
    printf("~%s = %s\n",zpbitsout(s[i]),zpbitsout(bitnot(s[i])));


  printf ("\nSHIFT LEFT:\n");
  for (i=0; i<numb; i++) {
    printf("%s\n",zpbitsout(s[i]));
    for (j=0; j<=VARBITLEN(s[i]); j++)
      printf("\t%3d\t%s\n",j,zpbitsout(bitshiftleft(s[i],j)));
  }

  printf ("\nSHIFT RIGHT:\n");
  for (i=0; i<numb; i++) {
    printf("%s\n",zpbitsout(s[i]));
    for (j=0; j<=VARBITLEN(s[i]); j++)
      printf("\t%3d\t%s\n",j,zpbitsout(bitshiftright(s[i],j)));
  }

}
