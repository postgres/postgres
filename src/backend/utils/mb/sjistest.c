/*
 *	testing for sjis2mic() and mic2sjis()
 */

#include "conv.c"

int
main()
{
	unsigned char eucbuf[1024];
	unsigned char sjisbuf[1024];
	unsigned char sjis[] = {0x81, 0x40, 0xa1, 0xf0, 0x40, 0xf0, 0x9e, 0xf5, 0x40, 0xfa, 0x40, 0xfa, 0x54, 0xfa, 0x7b, 0x00};

	int			i;

	sjis2mic(sjis, eucbuf, 1024);
	for (i = 0; i < 1024; i++)
	{
		if (eucbuf[i])
			printf("%02x ", eucbuf[i]);
		else
		{
			printf("\n");
			break;
		}
	}

	mic2sjis(eucbuf, sjisbuf, 1024);
	for (i = 0; i < 1024; i++)
	{
		if (sjisbuf[i])
			printf("%02x ", sjisbuf[i]);
		else
		{
			printf("\n");
			break;
		}
	}

	return (0);
}
