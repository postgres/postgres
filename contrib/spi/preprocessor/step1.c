#include <stdio.h>

char *
strtoupper(char *string)
{
	int			i;

	for (i = 0; i < strlen(string); i++)
		string[i] = toupper(string[i]);
	return string;
}



void
main(char argc, char **argv)
{
	char		str[250];
	int			sw = 0;

	while (fgets(str, 240, stdin))
	{
		if (sw == 0)
			printf("%s", strtoupper(str));
	}

}
