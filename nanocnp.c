#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <err.h>
#include <stdio.h>
#include <stdalign.h>

#ifdef __clang__
# undef alignas
# define alignas(x)
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

struct ncnp_pointer
{
	/* opaque */
	unsigned char bytes[8];
};

struct ncnp_object_size
{
	/* This is a silly microoptimization */
	uint32_t val;  /* (ptrwords << 16) | datawords */
};

struct ncnp_struct_meta
{
	struct ncnp_rbuf ptr_target_area;
	ncnp_word_rptr data;
	uint16_t n_data_words;
	uint16_t n_pointers;
};

struct ncnp_list_meta
{
	struct ncnp_rbuf ptr_target_area;
	ncnp_word_rptr data;
	unsigned int elemtype : 3;
	unsigned int list_words : 29;
	uint32_t list_elems;
	uint32_t stride_in_bits;
};

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

void ncnp_decode_data(struct ncnp_word *data_out,
		      uint16_t n_words_out,
		      const struct ncnp_word *data_in,
		      uint16_t n_words_in)
{
	/* TODO: Handle endianness here? */
	if (n_words_out <= n_words_in) {
		memcpy(data_out, data_in, n_words_out * 8);
	} else {
		memcpy(data_out, data_in, n_words_in * 8);
		memset(data_out + n_words_in, 0,
		       (n_words_out - n_words_in) * 8);
	}
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
	switch (elemtype) {
	case 0: /* void */
		total_words = 0;
		stride_in_bits = 0;
		break;

	case 1: /* bits */
		total_words = (ncnp_listptrval_len(ptrval) + 63) / 64;
		stride_in_bits = 1;
		break;

	case 2: /* bytes */
		total_words = (ncnp_listptrval_len(ptrval) + 7) / 8;
		stride_in_bits = 8;
		break;

	case 3: /* 2-byte objects */
		total_words = (ncnp_listptrval_len(ptrval) + 3) / 4;
		stride_in_bits = 16;
		break;

	case 4: /* 4-byte objects */
		stride_in_bits = 32;
		total_words = (ncnp_listptrval_len(ptrval) + 1) / 2;
		break;

	case 5: /* non-pointer words */
	case 6: /* pointers */
		stride_in_bits = 64;
		total_words = ncnp_listptrval_len(ptrval);
		break;

	case 7: /* composite */
		/* we'll fill in stride_in_bits later */
		total_words = ncnp_listptrval_len(ptrval) + 1;
		break;
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
		list_elems = ncnp_ptrval_offset(tagval);
		stride_in_bits = 64 * (ncnp_structptrval_n_data_words(tagval) +
				       ncnp_structptrval_n_pointers(tagval));

		list_start = data_start + 1;
		list_words = total_words - 1;

		if ((uint64_t)stride_in_bits * (uint64_t)list_elems !=
		    (uint64_t)list_words * 64)
			return -1;  /* List tag is inconsistent. */
	} else {
		list_start = data_start;
		list_elems = ncnp_listptrval_len(ptrval);
		list_words = total_words;
	}

	meta->ptr_target_area = targetbuf;
	meta->data = pptr + list_start;
	meta->list_words = list_words;
	meta->list_elems = list_elems;
	meta->stride_in_bits = stride_in_bits;
	meta->elemtype = elemtype;

	return 0;
}

int ncnp_decode_root(struct ncnp_struct_meta *meta,
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

void ncnp_dump_struct(FILE *f, const struct ncnp_struct_meta *meta, int level)
{
	fprintf(f, "%*sStruct, %d data words, %d pointers:\n",
		level, "",
		meta->n_data_words, (int)meta->n_pointers);

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

void ncnp_dump_recursive(FILE *f, const struct ncnp_struct_meta *meta, int level)
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
				fprintf(f, "%*sLIST, len %u, %u words, stride %u bits\n", level, "",
					list.list_elems,
					list.list_words,
					list.stride_in_bits);
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
