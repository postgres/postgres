/* GetPrivateProfileString() -- approximate implementation of */
/* Windows NT System Services version of GetPrivateProfileString() */
/* probably doesn't handle the NULL key for section name or value key */
/* correctly also, doesn't provide Microsoft backwards compatability */
/* wrt TAB characters in the value string -- Microsoft terminates value */
/* at the first TAB, but I couldn't discover what the behavior should */
/* be regarding TABS in quoted strings so, I treat tabs like any other */
/* characters -- NO comments following value string separated by a TAB */
/* are allowed (that is an anachronism anyway) */
/* Added code to search for ODBC_INI file in users home directory on */
/* Unix */

#ifndef WIN32

#if HAVE_CONFIG_H
#include "config.h"	/* produced by configure */
#endif

#include <stdio.h>
#include <unistd.h>
#include <ctype.h>

#if HAVE_PWD_H
#include <pwd.h>
#endif

#include <sys/types.h>
#include <string.h>
#include "gpps.h"
#include "misc.h"

#ifndef TRUE
#define TRUE	((BOOL)1)
#endif
#ifndef FALSE
#define FALSE	((BOOL)0)
#endif


DWORD
GetPrivateProfileString(char *theSection,	/* section name */
			char *theKey,		/* search key name */
			char *theDefault,	/* default value if not found */
			char *theReturnBuffer,	/* return value stored here */
			size_t theReturnBufferLength,	/* byte length of return buffer */
			char *theIniFileName)		/* pathname of ini file to search */
{
	char buf[MAXPGPATH];
	char* ptr = 0;
	FILE* aFile = 0;
	size_t aLength;
	char aLine[2048];
	char *aValue;
	char *aStart;
	char *aString;
	size_t aLineLength;
	size_t aReturnLength = 0;

	BOOL aSectionFound = FALSE;
	BOOL aKeyFound = FALSE;
	int j = 0;
	
	j = strlen(theIniFileName) + 1;
	ptr = (char*)getpwuid(getuid());	/* get user info */

	if( ptr == NULL)
	{
		if( MAXPGPATH-1 < j )
			theIniFileName[MAXPGPATH-1] = '\0';

		sprintf(buf,"%s",theIniFileName);
	}
	ptr = ((struct passwd*)ptr)->pw_dir;	/* get user home dir */
	if( ptr == NULL || *ptr == '\0' )
		ptr = "/home";

	/* This doesn't make it so we find an ini file but allows normal
	 * processing to continue further on down. The likelihood is that
	 * the file won't be found and thus the default value will be
	 * returned.
	*/
	if( MAXPGPATH-1 < strlen(ptr) + j )
	{
		if( MAXPGPATH-1 < strlen(ptr) )
			ptr[MAXPGPATH-1] = '\0';
		else
			theIniFileName[MAXPGPATH-1-strlen(ptr)] = '\0';
	}

	sprintf( buf, "%s/%s",ptr,theIniFileName );

	  /* This code makes it so that a file in the users home dir
	   * overrides a the "default" file as passed in
	  */
#ifndef __CYGWIN32__
	aFile = (FILE*)(buf ? fopen(buf, "r") : NULL);
#else
	aFile = (FILE*)(buf ? fopen(buf, "rb") : NULL);
#endif
	if(!aFile) {
		sprintf(buf,"%s",theIniFileName);
#ifndef __CYGWIN32__
		aFile = (FILE*)(buf ? fopen(buf, "r") : NULL);
#else
		aFile = (FILE*)(buf ? fopen(buf, "rb") : NULL);
#endif
	}

		
	aLength = (theDefault == NULL) ? 0 : strlen(theDefault);

	if(theReturnBufferLength == 0 || theReturnBuffer == NULL)
	{
		if(aFile)
		{
			fclose(aFile);
		}
		return 0;
	}

	if(aFile == NULL)
	{
		/* no ini file specified, return the default */

		++aLength;	/* room for NULL char */
		aLength = theReturnBufferLength < aLength ?
			theReturnBufferLength : aLength;
		strncpy(theReturnBuffer, theDefault, aLength);
		theReturnBuffer[aLength - 1] = '\0';
		return aLength - 1;
	}


	while(fgets(aLine, sizeof(aLine), aFile) != NULL)
	{
		aLineLength = strlen(aLine);
		/* strip final '\n' */
		if(aLineLength > 0 && aLine[aLineLength - 1] == '\n')
		{
			aLine[aLineLength - 1] = '\0';
		}
		switch(*aLine)
		{
			case ' ': /* blank line */
			case ';': /* comment line */
				continue;
			break;
			
			case '[':	/* section marker */

				if( (aString = strchr(aLine, ']')) )
				{
					aStart = aLine + 1;
					aString--;
					while (isspace(*aStart)) aStart++;
					while (isspace(*aString)) aString--;
					*(aString+1) = '\0';

					/* accept as matched if NULL key or exact match */

					if(!theSection || !strcmp(aStart, theSection))
					{
						aSectionFound = TRUE;
					}
				}

			break;

			default:

				/* try to match value keys if in proper section */

				if(aSectionFound)
				{
					/* try to match requested key */
	
					if( (aString = aValue = strchr(aLine, '=')) )
					{
						*aValue = '\0';
						++aValue;

						/* strip leading blanks in value field */

						while(*aValue == ' ' && aValue < aLine + sizeof(aLine))
						{
							*aValue++ = '\0';
						}
						if(aValue >= aLine + sizeof(aLine))
						{
							aValue = "";
						}
					}
					else
					{
						aValue = "";
					}

					aStart = aLine;
					while(isspace(*aStart)) aStart++;

					/* strip trailing blanks from key */

					if(aString)
					{
						while(--aString >= aStart && *aString == ' ')
						{
							*aString = '\0';
						}
					}

					/* see if key is matched */

					if(theKey == NULL || !strcmp(theKey, aStart))
					{
						/* matched -- first, terminate value part */

						aKeyFound = TRUE;
						aLength = strlen(aValue);

						/* remove trailing blanks from aValue if any */

						aString = aValue + aLength - 1;
						
						while(--aString > aValue && *aString == ' ')
						{
							*aString = '\0';
							--aLength;
						}

						/* unquote value if quoted */

						if(aLength >= 2 && aValue[0] == '"' &&
							aValue[aLength - 1] == '"')
						{
							/* string quoted with double quotes */

							aValue[aLength - 1] = '\0';
							++aValue;
							aLength -= 2;
						}
						else
						{
							/* single quotes allowed also... */

							if(aLength >= 2 && aValue[0] == '\'' &&
								aValue[aLength - 1] == '\'')
							{
								aValue[aLength - 1] = '\0';
								++aValue;
								aLength -= 2;
							}
						}

						/* compute maximum length copyable */

						aLineLength = (aLength <
							theReturnBufferLength - aReturnLength) ? aLength :
							theReturnBufferLength - aReturnLength;

						/* do the copy to return buffer */

						if(aLineLength)
						{
							strncpy(&theReturnBuffer[aReturnLength],
								aValue, aLineLength);
							aReturnLength += aLineLength;
							if(aReturnLength < theReturnBufferLength)
							{
								theReturnBuffer[aReturnLength] = '\0';
								++aReturnLength;
							}
						}
						if(aFile)
						{
							fclose(aFile);
							aFile = NULL;
						}

						return aReturnLength > 0 ? aReturnLength - 1 : 0;
					}
				}

			break;
		}
	}

	if(aFile)
	{
		fclose(aFile);
	}

	if(!aKeyFound) {	/* key wasn't found return default */
		++aLength;	/* room for NULL char */
		aLength = theReturnBufferLength < aLength ?
			theReturnBufferLength : aLength;
		strncpy(theReturnBuffer, theDefault, aLength);
		theReturnBuffer[aLength - 1] = '\0';
		aReturnLength = aLength - 1;
	}
	return aReturnLength > 0 ? aReturnLength - 1 : 0;
}

DWORD
WritePrivateProfileString(char *theSection,	/* section name */
			  char *theKey,		/* write key name */
			  char *theBuffer,	/* input buffer */
			  char *theIniFileName)	/* pathname of ini file to write */
{
	return 0;
}

#if 0
/* Ok. What the hell's the default behaviour for a null input buffer, and null
 * section name. For now if either are null I ignore the request, until
 * I find out different.
 */
DWORD
WritePrivateProfileString(char *theSection,	/* section name */
			  char *theKey,		/* write key name */
			  char *theBuffer,	/* input buffer */
			  char *theIniFileName)	/* pathname of ini file to write */
{
	char buf[MAXPGPATH];
	char* ptr = 0;
	FILE* aFile = 0;
	size_t aLength;
	char aLine[2048];
	char *aValue;
	char *aString;
	size_t aLineLength;
	size_t aReturnLength = 0;

	BOOL aSectionFound = FALSE;
	BOOL keyFound = FALSE;
	int j = 0;
	
	/* If this isn't correct processing we'll change it later  */
	if(theSection == NULL || theKey == NULL || theBuffer == NULL || 
		theIniFileName == NULL) return 0;

	aLength = strlen(theBuffer);
	if(aLength == 0) return 0;

	j = strlen(theIniFileName) + 1;
	ptr = (char*)getpwuid(getuid());	/* get user info */

	if( ptr == NULL)
	{
		if( MAXPGPATH-1 < j )
			theIniFileName[MAXPGPATH-1] = '\0';

		sprintf(buf,"%s",theIniFileName);
	}
	ptr = ((struct passwd*)ptr)->pw_dir;	/* get user home dir */
	if( ptr == NULL || *ptr == '\0' )
		ptr = "/home";

	/* This doesn't make it so we find an ini file but allows normal */
	/*  processing to continue further on down. The likelihood is that */
	/* the file won't be found and thus the default value will be */
	/* returned. */
	/* */
	if( MAXPGPATH-1 < strlen(ptr) + j )
	{
		if( MAXPGPATH-1 < strlen(ptr) )
			ptr[MAXPGPATH-1] = '\0';
		else
			theIniFileName[MAXPGPATH-1-strlen(ptr)] = '\0';
	}

	sprintf( buf, "%s/%s",ptr,theIniFileName );

	/* This code makes it so that a file in the users home dir */
	/*  overrides a the "default" file as passed in */
	/* */
	aFile = (FILE*)(buf ? fopen(buf, "r+") : NULL);
	if(!aFile) {
		sprintf(buf,"%s",theIniFileName);
		aFile = (FILE*)(buf ? fopen(buf, "r+") : NULL);
		if(!aFile) return 0;
	}

		
	aLength = strlen(theBuffer);

	/* We have to search for theKey, because if it already */
	/* exists we have to overwrite it. If it doesn't exist */
	/* we just write a new line to the file. */
	/* */
	while(fgets(aLine, sizeof(aLine), aFile) != NULL)
	{
		aLineLength = strlen(aLine);
		/* strip final '\n' */
		if(aLineLength > 0 && aLine[aLineLength - 1] == '\n')
		{
			aLine[aLineLength - 1] = '\0';
		}
		switch(*aLine)
		{
			case ' ': /* blank line */
			case ';': /* comment line */
				continue;
			break;
			
			case '[':	/* section marker */

				if( (aString = strchr(aLine, ']')) )
				{
					*aString = '\0';

					/* accept as matched if key exact match */

					if(!strcmp(aLine + 1, theSection))
					{
						aSectionFound = TRUE;
					}
				}

			break;

			default:

				/* try to match value keys if in proper section */

				if(aSectionFound)
				{
					/* try to match requested key */
	
					if( (aString = aValue = strchr(aLine, '=')) )
					{
						*aValue = '\0';
						++aValue;

						/* strip leading blanks in value field */

						while(*aValue == ' ' && aValue < aLine + sizeof(aLine))
						{
							*aValue++ = '\0';
						}
						if(aValue >= aLine + sizeof(aLine))
						{
							aValue = "";
						}
					}
					else
					{
						aValue = "";
					}

					/* strip trailing blanks from key */

					if(aString)
					{
						while(--aString >= aLine && *aString == ' ')
						{
							*aString = '\0';
						}
					}

					/* see if key is matched */

					if(!strcmp(theKey, aLine))
					{
						keyFound = TRUE;
						/* matched -- first, terminate value part */

						/* overwrite current value */
						fseek(aFile,-aLineLength,SEEK_CUR);
						/* overwrite key and value */
						sprintf(aLine,"%s = %s\n",theKey,theBuffer);
						fputs(aLine,aFile);
						}
					}
				}

			break;
		}
	}

	if(!keyFound) {		/* theKey wasn't in file so  */
	if(aFile)
	{
		fclose(aFile);
	}

	return aReturnLength > 0 ? aReturnLength - 1 : 0;
}
#endif


#endif
