main()
{
	yylex();
	return;
}

yywrap()
{
	return 1;
};
