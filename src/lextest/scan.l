%{
/*
	This should work, but non-patched flex 2.5.3 fails because input()
	doesn't return EOF or '\0'
	Bruce Momjian <root@candle.pha.pa.us>
*/
%}
%%
.		{
			int ch;
			while ((ch = input()) != EOF && ch != '\0')
				;
		}
