#pragma once
#ifndef _NANOCNP_H_INCLUDED
#define _NANOCNP_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
	ncnp_word_rptr data;
	uint16_t n_data_words;  /* TODO: Should be n_data_bits, I think. */
	uint16_t n_pointers;
};

struct ncnp_list_meta
{
	struct ncnp_rbuf ptr_target_area;
	ncnp_word_rptr data;
	unsigned int elemtype : 3;
	uint32_t list_elems : 29;
	uint32_t nott1_stride_in_bytes;

	/* Only valid if elemtype >= 5. */
	uint16_t t567_n_data_words;
	uint16_t t567_n_pointers;
};

int ncnp_decode_structptr(struct ncnp_struct_meta *meta,
			  ncnp_word_rptr pptr,
			  struct ncnp_rbuf targetbuf);

int ncnp_decode_listptr(struct ncnp_list_meta *meta,
			ncnp_word_rptr pptr,
			struct ncnp_rbuf targetbuf);

bool ncnp_list_get_bit(const struct ncnp_list_meta *list, size_t i);

unsigned char *ncnp_list_get_datum(const struct ncnp_list_meta *list, size_t i);

int ncnp_list_get_element(struct ncnp_struct_meta *meta,
			  struct ncnp_word *copy_dest,
			  uint16_t copy_n_data_words,
			  const struct ncnp_list_meta *list,
			  size_t i);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _NANOCNP_H_INCLUDED */
