#ifndef _LINUX_IDR_H_
#define _LINUX_IDR_H_

struct ida {
};

#define DEFINE_IDA(x) struct ida x


static inline int ida_pre_get(struct ida *ida, gfp_t gfp_mask)
{
	return 0;
}

static inline int ida_get_new_above(struct ida *ida, int starting_id, int *p_id)
{
	*p_id = starting_id + 1;
	return 0;
}

static inline int ida_get_new(struct ida *ida, int *p_id)
{
	*p_id = 1;
	return 0;
}

static inline void ida_remove(struct ida *ida, int id)
{
}

static inline void ida_destroy(struct ida *ida)
{
}

static inline void ida_init(struct ida *ida)
{
}


#endif
