#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

struct sqlca {
    int sqlcode;
    struct {
	int sqlerrml;
	char sqlerrmc[1000];
    } sqlerrm;
} sqlca;
#endif
