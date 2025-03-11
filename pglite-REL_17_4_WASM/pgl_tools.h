#pragma once


#define STROPS_BUF 1024

char tmpstr[STROPS_BUF];

static
void mkdirp(const char *p) {
	if (!mkdir(p, 0700)) {
		fprintf(stderr, "# no '%s' directory, creating one ...\n", p);
	}
}


void
strconcat(char*p, const char *head, const char *tail) {
    int   len;

    len = strnlen(head, STROPS_BUF );
    p = memcpy(p, head, len);
    p += len;

    len = strnlen(tail, STROPS_BUF - len);
    p = memcpy(p, tail, len);
    p += len;
    *p = '\0';

}

char *
setdefault(const char* key, const char *value) {
    setenv(key, value, 0);
    return strdup(getenv(key));
}

char *
strcat_alloc(const char *head, const char *tail) {
    char buf[STROPS_BUF];
    strconcat( &buf[0], head, tail);
    return strdup((const char *)&buf[0]);
}

void
mksub_dir(const char *dir,const char *sub) {
    char buf[STROPS_BUF];
    strconcat(&buf[0], dir, sub);
    mkdirp(&buf[0]);
}


#if PGDEBUG
static void
print_bits(size_t const size, void const * const ptr)
{
    unsigned char *b = (unsigned char*) ptr;
    unsigned char byte;
    int i, j;

    for (i = size-1; i >= 0; i--) {
        for (j = 7; j >= 0; j--) {
            byte = (b[i] >> j) & 1;
            printf("%u", byte);
        }
    }
    puts("");
}
#endif // PGDEBUG




