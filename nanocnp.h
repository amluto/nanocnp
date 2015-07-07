#pragma once
#ifndef _NANOCNP_H_INCLUDED
#define _NANOCNP_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ncnp_word
{
	unsigned char bytes[8];
} __attribute__((aligned(8)));

typedef struct ncnp_word const *ncnp_word_rptr;

struct ncnp_rbuf
{
	/* end points one past the last word. */
	const struct ncnp_word *start, *end;
};

struct ncnp_struct_meta
{
	struct ncnp_rbuf ptr_target_area;

	/*
	 * If n_pointers != 0, then this is guaranteed to point to a real
	 * struct, and pointers in that struct may be followed.  Otherwise
	 * this may be null if it's in, say, an ncnp_struct_1w.
	 */
	ncnp_word_rptr data;

	uint16_t n_data_words;  /* if nonzero, data points somewhere */
	uint16_t n_pointers;
};

struct ncnp_struct_1w
{
	struct ncnp_struct_meta meta;
	struct ncnp_word copy;  /* meta->data may point here */
};

struct ncnp_list_meta
{
	struct ncnp_rbuf ptr_target_area;
	ncnp_word_rptr data;
	unsigned int elemtype : 3;
	uint32_t list_elems : 29;
	uint32_t nott1_stride_in_bytes;

	uint16_t n_full_data_words;
	uint16_t n_pointers;
};

static inline uint64_t ncnp_load_word(ncnp_word_rptr p)
{
	return ((uint64_t)p->bytes[0] |
		(uint64_t)p->bytes[1] << 8 |
		(uint64_t)p->bytes[2] << 16 |
		(uint64_t)p->bytes[3] << 24 |
		(uint64_t)p->bytes[4] << 32 |
		(uint64_t)p->bytes[5] << 40 |
		(uint64_t)p->bytes[6] << 48 |
		(uint64_t)p->bytes[7] << 56);

	/*
	 * This does not invoke undefined behavior, and it is correct
	 * on any little-endian architecture.  Gcc 5.0 and above
	 * can optimize the code above, though, so we don't need
	 * it.
	union {
		uint64_t val;
		unsigned char bytes[8];
	} ret;
	memcpy(&ret.bytes, p, 8);
	return ret.val;
	*/
}

static uint32_t ncnp_ptrval_type(uint64_t ptrval)
{
	return ptrval & 3;
}

int ncnp_decode_structptr(struct ncnp_struct_meta *meta,
			  ncnp_word_rptr pptr,
			  struct ncnp_rbuf targetbuf);

int ncnp_decode_root_1w(struct ncnp_struct_1w *obj,
			struct ncnp_rbuf in);

int ncnp_decode_listptr(struct ncnp_list_meta *meta,
			ncnp_word_rptr pptr,
			struct ncnp_rbuf targetbuf);

bool ncnp_list_get_bit(const struct ncnp_list_meta *list, size_t i);

unsigned char *ncnp_list_get_datum(const struct ncnp_list_meta *list, size_t i);

/*
 * This is a little bit inefficient if the caller knows that the
 * list has type 0, 5, 6, or, 7, but the only callers who care
 * are likely to be traversing untyped data, and performance
 * doesn't matter so much there.
 */
void ncnp_list_get_1welement(struct ncnp_struct_1w *dest,
			     const struct ncnp_list_meta *list,
			     size_t i);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _NANOCNP_H_INCLUDED */
