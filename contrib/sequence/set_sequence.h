#ifndef SET_SEQUENCE_H
#define SET_SEQUENCE_H

int set_currval(struct varlena *sequence, int4 nextval);
int next_id(struct varlena *sequence);
int last_id(struct varlena *sequence);
int set_last_id(struct varlena *sequence, int4 nextval);

#endif
