extern void set_parse_buffer(char *s);
extern void reset_parse_buffer(void);
extern int	read_parse_buffer(void);
extern char *parse_buffer(void);
extern char *parse_buffer_ptr(void);
extern unsigned int parse_buffer_curr_char(void);
extern unsigned int parse_buffer_pos(void);
extern unsigned int parse_buffer_size(void);
