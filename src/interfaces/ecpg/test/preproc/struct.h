
struct mytype {
	int	id;
	char	t[64];
	double	d1; /* dec_t */
	double	d2;
	char	c[30];
};
typedef struct mytype MYTYPE;

struct mynulltype {
	int	id;
	int	t;
	int	d1;
	int	d2;
	int	c;
};
typedef struct mynulltype MYNULLTYPE;
