#include "nanocnp.h"
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <err.h>
#include <stdio.h>
#include <stdalign.h>
#include <stdlib.h>

#ifdef __clang__
# undef alignas
# define alignas(x)
#endif

static void ncnp_assert(bool condition)
{
	if (!condition)
		abort();  /* for now */
}

static uint32_t ncnp_ptrval_type(uint64_t ptrval)
{
	return ptrval & 3;
}

static uint32_t ncnp_ptrval_offset(uint64_t ptrval)
{
	return (uint32_t)((int32_t)(ptrval & 0xffffffff) >> 2);
}

static uint16_t ncnp_structptrval_n_data_words(uint64_t ptrval)
{
	return (uint16_t)(ptrval >> 32);
}

static uint16_t ncnp_structptrval_n_pointers(uint64_t ptrval)
{
	return (uint16_t)(ptrval >> 48);
}

static uint32_t ncnp_listptrval_elemtype(uint64_t ptrval)
{
	return (uint32_t)((ptrval >> 32) & 7);
}

static uint32_t ncnp_listptrval_len(uint64_t ptrval)
{
	return (uint32_t)(ptrval >> 35);
}

static uint64_t ncnp_load_word(ncnp_word_rptr p)
{
	/* Unfortunately, neither gcc nor clang can optimize this:

	return ((uint64_t)p.bytes[0] |
		(uint64_t)p.bytes[1] << 8 |
		(uint64_t)p.bytes[2] << 16 |
		(uint64_t)p.bytes[3] << 24 |
		(uint64_t)p.bytes[4] << 32 |
		(uint64_t)p.bytes[5] << 40 |
		(uint64_t)p.bytes[6] << 48 |
		(uint64_t)p.bytes[7] << 56);
	*/

	/*
	 * This does not invoke undefined behavior, and it is correct
	 * on any little-endian architecture.
	 */
	union {
		uint64_t val;
		unsigned char bytes[8];
	} ret;
	memcpy(&ret.bytes, p, 8);
	return ret.val;
}

/*
 * This does two things.  It valides a structptr and it decodes everything
 * except the data.  Once you've called this and checked the return value,
 * though, it's safe to look at the data area without further bounds
 * checking.
 */
int ncnp_decode_structptr(struct ncnp_struct_meta *meta,
			  ncnp_word_rptr pptr,
			  struct ncnp_rbuf targetbuf)
{
	uint64_t ptrval = ncnp_load_word(pptr);

	if (ncnp_ptrval_type(ptrval) != 0)
		return -1;  /* Root pointer must be a struct pointer */

	if (ptrval == 0)
		return -1;  /* NULL */

	/*
	 * This cannot cause undefined behavior: pptr and targetbuf.start
	 * are pointers into the same array.  Furthermore, the result
	 * is between -2^29 and 2^29, since the maximum length of the
	 * array is 2^29 words.
	 */
	int32_t offset_to_start = targetbuf.start - pptr;
	int32_t offset_to_end = targetbuf.end - pptr;

	/* These cannot overflow; none of the fields are large enough. */
	int32_t obj_start = 1 + ncnp_ptrval_offset(ptrval);
	int32_t ptr_start = obj_start + ncnp_structptrval_n_data_words(ptrval);
	int32_t obj_end = ptr_start + ncnp_structptrval_n_pointers(ptrval);

	if ((uint32_t)(obj_start - offset_to_start) > (offset_to_end - offset_to_start))
		return -1;  /* The target starts out of bounds. */
	if ((uint32_t)(obj_end - offset_to_start) > (offset_to_end - offset_to_start))
		return -1;  /* The target ends out of bounds. */

	meta->ptr_target_area = targetbuf;
	meta->data = pptr + obj_start;
	meta->n_data_words = ncnp_structptrval_n_data_words(ptrval);
	meta->n_pointers = ncnp_structptrval_n_pointers(ptrval);

	return 0;
}

struct ncnp_list_type
{
	unsigned int stride_in_bits;
	uint16_t n_full_data_words, n_pointers;
};

static struct ncnp_list_type list_types[] = {
	{	/* type 0: void */
		.stride_in_bits = 0,
		.n_full_data_words = 0,
		.n_pointers = 0,
	},
	{	/* type 1: bits */
		.stride_in_bits = 1,
		.n_full_data_words = 0,
		.n_pointers = 0,
	},
	{	/* type 2: bytes */
		.stride_in_bits = 8,
		.n_full_data_words = 0,
		.n_pointers = 0,
	},
	{	/* type 3: 2-byte elements */
		.stride_in_bits = 16,
		.n_full_data_words = 0,
		.n_pointers = 0,
	},
	{	/* type 4: 4-byte elements */
		.stride_in_bits = 32,
		.n_full_data_words = 0,
		.n_pointers = 0,
	},
	{	/* type 5: data words */
		.stride_in_bits = 64,
		.n_full_data_words = 1,
		.n_pointers = 0,
	},
	{	/* type 6: pointer words */
		.stride_in_bits = 64,
		.n_full_data_words = 0,
		.n_pointers = 1,
	},
};

int ncnp_decode_listptr(struct ncnp_list_meta *meta,
			ncnp_word_rptr pptr,
			struct ncnp_rbuf targetbuf)
{
	uint64_t ptrval = ncnp_load_word(pptr);

	if (ncnp_ptrval_type(ptrval) != 1)
		return -1;  /* Root pointer must be a struct pointer */

	/*
	 * This cannot cause undefined behavior: pptr and targetbuf.start
	 * are pointers into the same array.  Furthermore, the result
	 * is between -2^29 and 2^29, since the maximum length of the
	 * array is 2^29 words.
	 */
	int32_t offset_to_start = targetbuf.start - pptr;
	int32_t offset_to_end = targetbuf.end - pptr;

	uint32_t total_words, list_elems, stride_in_bits;
	unsigned int elemtype = ncnp_listptrval_elemtype(ptrval);
	if (elemtype != 7) {
		stride_in_bits = list_types[elemtype].stride_in_bits;
		/* 2^29 words is more than 2^32 bits. */
		total_words =
			((uint64_t)ncnp_listptrval_len(ptrval) *
			 list_types[elemtype].stride_in_bits + 63) /
			64;
	} else {
		/* we'll fill in stride_in_bits later */
		total_words = ncnp_listptrval_len(ptrval) + 1;
	}

	/* These cannot overflow; none of the fields are large enough. */
	int32_t data_start = 1 + ncnp_ptrval_offset(ptrval);
	int32_t data_end = data_start + total_words;

	if ((uint32_t)(data_start - offset_to_start) > (offset_to_end - offset_to_start))
		return -1;  /* The target starts out of bounds. */
	if ((uint32_t)(data_end - offset_to_start) > (offset_to_end - offset_to_start))
		return -1;  /* The target ends out of bounds. */

	/* Handle the composite case */
	int32_t list_start;
	uint32_t list_words;
	if (elemtype >= 7) {
		uint64_t tagval = ncnp_load_word(pptr + data_start);
		if (ncnp_ptrval_type(tagval) != 0)
			return -1;  /* This could indicate a matrix some day. */
		/* Ignore ncnp_ptrval_offset(tagval); it is reserved. */

		list_start = data_start + 1;
		list_words = total_words - 1;
		uint32_t stride_in_words =
			(ncnp_structptrval_n_data_words(tagval) +
			 ncnp_structptrval_n_pointers(tagval));
		list_elems = list_words / stride_in_words;
		if (list_words % stride_in_words != 0)
			return -1;  /* Invalid! */
		stride_in_bits = 64 * stride_in_words;

		if ((uint64_t)stride_in_bits * (uint64_t)list_elems !=
		    (uint64_t)list_words * 64)
			return -1;  /* List tag is inconsistent. */

		meta->n_full_data_words =
			ncnp_structptrval_n_data_words(tagval);
		meta->n_pointers = ncnp_structptrval_n_pointers(tagval);
	} else {
		list_start = data_start;
		list_elems = ncnp_listptrval_len(ptrval);
		list_words = total_words;
		meta->n_full_data_words =
			list_types[elemtype].n_full_data_words;
		meta->n_pointers = list_types[elemtype].n_pointers;
	}

	meta->ptr_target_area = targetbuf;
	meta->data = pptr + list_start;
	meta->list_elems = list_elems;
	meta->nott1_stride_in_bytes = stride_in_bits / 8;  /* not for type 1 */
	meta->elemtype = elemtype;

	return 0;
}

bool ncnp_list_get_bit(const struct ncnp_list_meta *list, size_t i)
{
	ncnp_assert(i < list->list_elems);
	ncnp_assert(list->elemtype == 1);

	uint64_t word = ncnp_load_word(list->data + i / 64);
	return (word >> (i % 64)) & 1;
}

unsigned char *ncnp_list_get_datum(const struct ncnp_list_meta *list, size_t i)
{
	ncnp_assert(i < list->list_elems);
	ncnp_assert(list->elemtype <= 5 && list->elemtype != 1);

	return (unsigned char *)list->data + i * list->nott1_stride_in_bytes;
}

void ncnp_list_get_1welement(struct ncnp_struct_oneword *dest,
			     const struct ncnp_list_meta *list,
			     size_t i)
{
	ncnp_assert(i < list->list_elems);

	dest->copy = (struct ncnp_word){{0}};
	dest->meta.n_pointers = list->n_pointers;
	dest->meta.n_data_words = list->n_full_data_words;

	if (list->elemtype == 1) {
		dest->copy.bytes[0] = (unsigned char)ncnp_list_get_bit(list, i);
		dest->meta.data = (void *)0;
	} else if (list->elemtype <= 5) {
		/* Give the optimizer lots of help. */
		size_t bytes_to_copy = list->nott1_stride_in_bytes;
		if (bytes_to_copy > 8)
			__builtin_unreachable();
		memcpy(&dest->copy, ncnp_list_get_datum(list, i),
		       bytes_to_copy);
		dest->meta.data = (void *)0;
	} else {
		dest->meta.ptr_target_area = list->ptr_target_area;
		dest->meta.data = (ncnp_word_rptr)
			((unsigned char *)list->data +
			 i * list->nott1_stride_in_bytes);
		if (list->n_full_data_words)
			dest->copy = *dest->meta.data;
	}
}

static int ncnp_decode_root(struct ncnp_struct_meta *meta,
			    struct ncnp_rbuf in)
{
	int ret;
	size_t total_len = in.end - in.start;
	if (total_len < 1)
		return -1;

	ret = ncnp_decode_structptr(meta, in.start, in);
	if (ret == -1)
		return -1;

/*
	ncnp_decode_data((struct ncnp_word *)(out + 1),
			 out->n_data_words,
			 in.start + offset + 1,
			 n_data_words);
*/

	return 0;
}

static void ncnp_dump_struct(FILE *f,
			     const struct ncnp_struct_meta *meta, int level)
{
	fprintf(f, "%*sStruct, %d data words, %d pointers:\n",
		level, "",
		meta->n_data_words, (int)meta->n_pointers);

	if (!meta->data) {
		fprintf(f, "%*s[cannot display data portion]\n",
			level, "");
	} else {
		for (size_t i = 0; i < meta->n_data_words; i++)
			fprintf(f, "%*s0x%02x%02x%02x%02x%02x%02x%02x%02x\n",
				level, "",
				(int)meta->data[i].bytes[7],
				(int)meta->data[i].bytes[6],
				(int)meta->data[i].bytes[5],
				(int)meta->data[i].bytes[4],
				(int)meta->data[i].bytes[3],
				(int)meta->data[i].bytes[2],
				(int)meta->data[i].bytes[1],
				(int)meta->data[i].bytes[0]);
	}
}

static void ncnp_dump_recursive(FILE *f, const struct ncnp_struct_meta *meta, int level);

static void ncnp_dump_list_recursive(FILE *f, const struct ncnp_list_meta *list,
				     int level)
{
	if (list->elemtype == 0) {
		fprintf(f, "%*sLIST of %d void elements\n", level, "",
			(int)list->list_elems);
		ncnp_assert(list->nott1_stride_in_bytes == 0);
	} else if (list->elemtype == 1) {
		fprintf(f, "%*sLIST of %d bits\n%*s", level, "",
			(int)list->list_elems, level + 1, "");
		if (list->list_elems == 0)
			fprintf(f, "[empty]");
		for (int i = 0; i < list->list_elems; i++)
			fprintf(f, "%c",
				(ncnp_list_get_bit(list, i) ? '1' : '0'));
		fprintf(f, "\n");
	} else if (list->elemtype <= 5) {
		fprintf(f, "%*sLIST of %d %u-byte data elements\n",
			level, "",
			(int)list->list_elems, list->nott1_stride_in_bytes);
		if (list->list_elems == 0)
			fprintf(f, "[empty]");
		for (int i = 0; i < list->list_elems; i++) {
			fprintf(f, "%*s0x", level + 1, "");
			unsigned char *val = ncnp_list_get_datum(list, i);
			for (int j = list->nott1_stride_in_bytes - 1; j >= 0;
			     j--)
				fprintf(f, "%02X", val[j]);
			fprintf(f, "\n");
		}
	} else {
		fprintf(f, "%*sLIST of %d %s, stride %u bytes\n", level, "",
			list->list_elems,
			list->elemtype == 6 ? "pointers" : "structs",
			list->nott1_stride_in_bytes);
		for (int i = 0; i < list->list_elems; i++) {
			if (i != 0)
				fprintf(f, "\n");
			struct ncnp_struct_oneword obj;
			ncnp_list_get_1welement(&obj, list, i);
			ncnp_dump_recursive(f, &obj.meta, level + 1);
		}
	}
}

static void ncnp_dump_recursive(
	FILE *f, const struct ncnp_struct_meta *meta, int level)
{
	ncnp_dump_struct(f, meta, level);

	for (size_t i = 0; i < meta->n_pointers; i++) {
		ncnp_word_rptr pptr = meta->data + meta->n_data_words + i;
		uint64_t ptrval = ncnp_load_word(pptr);
		uint32_t type = ncnp_ptrval_type(ptrval);
		if (ptrval == 0) {
			fprintf(f, "%*snullptr\n", level, "");
		} else if (type == 0) {
			struct ncnp_struct_meta obj;
			if (ncnp_decode_structptr(&obj, pptr,
						  meta->ptr_target_area) != 0) {
				fprintf(f, "%*sbad structptr\n", level, "");
			} else {
				ncnp_dump_recursive(f, &obj, level + 1);
			}
		} else if (type == 1) {
			struct ncnp_list_meta list;
			if (ncnp_decode_listptr(&list, pptr,
						meta->ptr_target_area) != 0) {
				fprintf(f, "%*sbad listptr\n", level, "");
			} else {
				ncnp_dump_list_recursive(f, &list, level);
			}
		} else if (type == 2) {
			fprintf(f, "%*sFARPTR\n", level, "");
		} else {
			fprintf(f, "%*sOTHER\n", level, "");
		}
	}
}

int main()
{
	unsigned char buf[16384];
	size_t len = 0;
	while (true) {
		if (len >= sizeof(buf))
			errx(1, "Too much input\n");
		ssize_t bytes = read(0, buf + len, sizeof(buf) - len);
		if (bytes < 0)
			err(1, "read");
		if (bytes == 0)
			break;
		else
			len += bytes;
	}

	if (len % 8 != 0)
		errx(1, "Input length is not a multiple of 8\n");

	struct foo {
		struct ncnp_struct_meta meta;
		struct ncnp_word data[2];
	} foo;
	struct ncnp_rbuf in = {(struct ncnp_word *)buf,
			       (struct ncnp_word *)(buf + len)};
	if (ncnp_decode_root(&foo.meta, in) != 0)
		errx(1, "ncnp_decode_root failed");

	ncnp_dump_recursive(stdout, &foo.meta, 0);

	return 0;
}
