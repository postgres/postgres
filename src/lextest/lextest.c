/*
** lextest.c
**
** tests for flex 2.5.3 bug
*/

int
main()
{
	yylex();
	return 0;
}

int
yywrap()
{
	return 1;
};
