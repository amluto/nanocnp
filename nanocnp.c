#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <err.h>
#include <stdio.h>
#include <stdalign.h>

// #undef alignas
// #define alignas(x)

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

struct ncnp_decoded_object_header
{
	struct ncnp_rbuf ptr_target_area;
	ncnp_word_rptr first_pointer;
	uint16_t n_data_words;
	uint16_t n_pointers;
	uint32_t pad1;
};

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

int ncnp_decode_root(struct ncnp_decoded_object_header *out,
		     struct ncnp_rbuf in)
{
	size_t total_len = in.end - in.start;
	if (total_len < 1)
		return -1;

	uint64_t ptrval = ncnp_load_word(in.start);
	uint32_t offset_and_type = (uint32_t)ptrval;
	if ((offset_and_type & 0x3) != 0)
		return -1;  /* Root pointer must be a struct pointer */

	uint16_t n_data_words = (uint16_t)(ptrval >> 32);
	uint16_t n_pointers = (uint16_t)(ptrval >> 48);

	uint32_t offset = offset_and_type >> 2;

	/* This cannot overflow; it is bounded by 2^30 + 2^17 + 1. */
	uint32_t len_needed = 1 + offset + n_data_words + n_pointers;
	if (len_needed > in.end - in.start)
		return -1;  /* The pointer points out of bounds. */

	ncnp_decode_data((struct ncnp_word *)(out + 1),
			 out->n_data_words,
			 in.start + offset + 1,
			 n_data_words);

	out->ptr_target_area = in;
	out->first_pointer = in.start + 1 + offset + n_data_words;
	out->n_pointers = n_pointers;

	return 0;
}

void ncnp_dump_object(FILE *f, const struct ncnp_decoded_object_header *hdr)
{
	const struct ncnp_word *words = (const struct ncnp_word *)(hdr + 1);

	fprintf(f, "Object, %d data words, %d pointers:\n",
		hdr->n_data_words, (int)hdr->n_pointers);

	for (size_t i = 0; i < hdr->n_data_words; i++)
		fprintf(f, "  0x%02x%02x%02x%02x%02x%02x%02x%02x\n",
			(int)words[i].bytes[7],
			(int)words[i].bytes[6],
			(int)words[i].bytes[5],
			(int)words[i].bytes[4],
			(int)words[i].bytes[3],
			(int)words[i].bytes[2],
			(int)words[i].bytes[1],
			(int)words[i].bytes[0]);
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
		struct ncnp_decoded_object_header hdr;
		struct ncnp_word data[2];
	} foo;
	foo.hdr.n_data_words = 2;
	struct ncnp_rbuf in = {(struct ncnp_word *)buf,
			       (struct ncnp_word *)(buf + len)};
	if (ncnp_decode_root(&foo.hdr, in) != 0)
		errx(1, "ncnp_decode_root");

	ncnp_dump_object(stdout, &foo.hdr);

	return 0;
}
