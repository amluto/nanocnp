#include "nanocnp.h"
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <err.h>
#include <stdio.h>
#include <stdalign.h>
#include <stdlib.h>

static void ncnp_assert(bool condition)
{
	if (!condition)
		abort();  /* for now */
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
			struct ncnp_struct_meta obj;
			ncnp_list_get_struct(&obj, list, i);
			ncnp_dump_recursive(f, &obj, level + 1);
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

	struct ncnp_struct_meta root;
	struct ncnp_rbuf in = {(struct ncnp_word *)buf,
			       (struct ncnp_word *)(buf + len)};
	if (ncnp_decode_root(&root, in) != 0)
		errx(1, "ncnp_decode_root failed");

	ncnp_dump_recursive(stdout, &root, 0);

	return 0;
}
