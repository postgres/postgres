/*
	This should work, but non-patched flex 2.5.3 fails because input()
	doesn't return EOF
	Bruce Momjian <root@candle.pha.pa.us>
*/
%%
.		{
				while (input() != EOF)
				;
		}
