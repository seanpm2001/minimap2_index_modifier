#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#define __STDC_LIMIT_MACROS
#include "kvec.h"
#include "mmpriv.h"

unsigned char seq_nt4_table[256] = {
	0, 1, 2, 3,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  3, 3, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  3, 3, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
};

static inline uint64_t hash64(uint64_t key, uint64_t mask)
{
	key = (~key + (key << 21)) & mask; // key = (key << 21) - key - 1;
	key = key ^ key >> 24;
	key = ((key + (key << 3)) + (key << 8)) & mask; // key * 265
	key = key ^ key >> 14;
	key = ((key + (key << 2)) + (key << 4)) & mask; // key * 21
	key = key ^ key >> 28;
	key = (key + (key << 31)) & mask;
	return key;
}

typedef struct { // a simplified version of kdq
	int front, count;
	int a[32];
} tiny_queue_t;

static inline void tq_push(tiny_queue_t *q, int x)
{
	q->a[((q->count++) + q->front) & 0x1f] = x;
}

static inline int tq_shift(tiny_queue_t *q)
{
	int x;
	if (q->count == 0) return -1;
	x = q->a[q->front++];
	q->front &= 0x1f;
	--q->count;
	return x;
}

/**
 * Find symmetric (w,k)-minimizers on a DNA sequence
 *
 * @param km     thread-local memory pool; using NULL falls back to malloc()
 * @param str    DNA sequence
 * @param len    length of $str
 * @param w      find a minimizer for every $w consecutive k-mers
 * @param k      k-mer size
 * @param rid    reference ID; will be copied to the output $p array
 * @param is_hpc homopolymer-compressed or not
 * @param p      minimizers
 *               p->a[i].x = kMer<<8 | kmerSpan
 *               p->a[i].y = rid<<32 | lastPos<<1 | strand
 *               where lastPos is the position of the last base of the i-th minimizer,
 *               and strand indicates whether the minimizer comes from the top or the bottom strand.
 *               Callers may want to set "p->n = 0"; otherwise results are appended to p
 */
void mm_sketch(void *km, const char *str, int len, int w, int k, uint32_t rid, int is_hpc, mm128_v *p)
{
	uint64_t shift1 = 2 * (k - 1), mask = (1ULL<<2*k) - 1, kmer[2] = {0,0};
	int i, j, l, buf_pos, min_pos, kmer_span = 0;
	mm128_t buf[256], min = { UINT64_MAX, UINT64_MAX };
	tiny_queue_t tq;

	assert(len > 0 && (w > 0 && w < 256) && (k > 0 && k <= 28)); // 56 bits for k-mer; could use long k-mers, but 28 enough in practice
	memset(buf, 0xff, w * 16);
	memset(&tq, 0, sizeof(tiny_queue_t));
	kv_resize(mm128_t, km, *p, p->n + len/w);

	for (i = l = buf_pos = min_pos = 0; i < len; ++i) {
		int c = seq_nt4_table[(uint8_t)str[i]];
		mm128_t info = { UINT64_MAX, UINT64_MAX };
		if (c < 4) { // not an ambiguous base
			int z;
			if (is_hpc) {
				int skip_len = 1;
				if (i + 1 < len && seq_nt4_table[(uint8_t)str[i + 1]] == c) {
					for (skip_len = 2; i + skip_len < len; ++skip_len)
						if (seq_nt4_table[(uint8_t)str[i + skip_len]] != c)
							break;
					i += skip_len - 1; // put $i at the end of the current homopolymer run
				}
				tq_push(&tq, skip_len);
				kmer_span += skip_len;
				if (tq.count > k) kmer_span -= tq_shift(&tq);
			} else kmer_span = l + 1 < k? l + 1 : k;
			kmer[0] = (kmer[0] << 2 | c) & mask;           // forward k-mer
			kmer[1] = (kmer[1] >> 2) | (3ULL^c) << shift1; // reverse k-mer
			if (kmer[0] == kmer[1]) continue; // skip "symmetric k-mers" as we don't know it strand
			z = kmer[0] < kmer[1]? 0 : 1; // strand
			++l;
			if (l >= k && kmer_span < 256) {
				info.x = hash64(kmer[z], mask) << 8 | kmer_span;
				info.y = (uint64_t)rid<<32 | (uint32_t)i<<1 | z;
			}
		} else l = 0, tq.count = tq.front = 0, kmer_span = 0;
		buf[buf_pos] = info; // need to do this here as appropriate buf_pos and buf[buf_pos] are needed below
		if (l == w + k - 1 && min.x != UINT64_MAX) { // special case for the first window - because identical k-mers are not stored yet
			for (j = buf_pos + 1; j < w; ++j)
				if (min.x == buf[j].x && buf[j].y != min.y) kv_push(mm128_t, km, *p, buf[j]);
			for (j = 0; j < buf_pos; ++j)
				if (min.x == buf[j].x && buf[j].y != min.y) kv_push(mm128_t, km, *p, buf[j]);
		}
		if (info.x <= min.x) { // a new minimum; then write the old min
			if (l >= w + k && min.x != UINT64_MAX) kv_push(mm128_t, km, *p, min);
			min = info, min_pos = buf_pos;
		} else if (buf_pos == min_pos) { // old min has moved outside the window
			if (l >= w + k - 1 && min.x != UINT64_MAX) kv_push(mm128_t, km, *p, min);
			for (j = buf_pos + 1, min.x = UINT64_MAX; j < w; ++j) // the two loops are necessary when there are identical k-mers
				if (min.x >= buf[j].x) min = buf[j], min_pos = j; // >= is important s.t. min is always the closest k-mer
			for (j = 0; j <= buf_pos; ++j)
				if (min.x >= buf[j].x) min = buf[j], min_pos = j;
			if (l >= w + k - 1 && min.x != UINT64_MAX) { // write identical k-mers
				for (j = buf_pos + 1; j < w; ++j) // these two loops make sure the output is sorted
					if (min.x == buf[j].x && min.y != buf[j].y) kv_push(mm128_t, km, *p, buf[j]);
				for (j = 0; j <= buf_pos; ++j)
					if (min.x == buf[j].x && min.y != buf[j].y) kv_push(mm128_t, km, *p, buf[j]);
			}
		}
		if (++buf_pos == w) buf_pos = 0;
	}
	if (min.x != UINT64_MAX)
		kv_push(mm128_t, km, *p, min);
}

void mm_sketch_syncmer(void *km, const char *str, int len, int smer, int k, uint32_t rid, int is_hpc, mm128_v *p)
{
	uint64_t shift1 = 2 * (k - 1), mask = (1ULL<<2*k) - 1, smask = (1ULL<<2*smer) - 1, kmer[2] = {0,0};
	int i, j, l, buf_pos, min_pos, kmer_span = 0;
	tiny_queue_t tq;

	assert(len > 0 && (smer > 0 && smer <= k) && (k > 0 && k <= 28)); // 56 bits for k-mer; could use long k-mers, but 28 enough in practice
	memset(&tq, 0, sizeof(tiny_queue_t));
	kv_resize(mm128_t, km, *p, p->n + len/(k - smer));

	for (i = l = buf_pos = min_pos = 0; i < len; ++i) {
		int c = seq_nt4_table[(uint8_t)str[i]];
		if (c < 4) { // not an ambiguous base
			int z;
			if (is_hpc) {
				int skip_len = 1;
				if (i + 1 < len && seq_nt4_table[(uint8_t)str[i + 1]] == c) {
					for (skip_len = 2; i + skip_len < len; ++skip_len)
						if (seq_nt4_table[(uint8_t)str[i + skip_len]] != c)
							break;
					i += skip_len - 1; // put $i at the end of the current homopolymer run
				}
				tq_push(&tq, skip_len);
				kmer_span += skip_len;
				if (tq.count > k) kmer_span -= tq_shift(&tq);
			} else kmer_span = l + 1 < k? l + 1 : k;
			kmer[0] = (kmer[0] << 2 | c) & mask;           // forward k-mer
			kmer[1] = (kmer[1] >> 2) | (3ULL^c) << shift1; // reverse k-mer
			if (kmer[0] == kmer[1]) continue; // skip "symmetric k-mers" as we don't know it strand
			z = kmer[0] < kmer[1]? 0 : 1; // strand
			++l;
			if (l >= k && kmer_span < 256) {
				uint64_t x, min = UINT64_MAX;
				x = hash64(kmer[z], mask);
				for (j = 0; j <= k - smer; ++j) {
					uint64_t y = x >> (j + j) & smask;
					min = min < y? min : y;
				}
				if ((x & smask) == min) {
					mm128_t t;
					t.x = x << 8 | kmer_span;
					t.y = (uint64_t)rid<<32 | (uint32_t)i<<1 | z;
					kv_push(mm128_t, km, *p, t);
				}
			}
		} else l = 0, tq.count = tq.front = 0, kmer_span = 0;
	}
}

void mm_idx_manipulate(/*FILE *fp,*/ mm_idx_t * mi) {
    FILE *vcf;
    vcf = fopen("ispras/test_snp+indel.vcf", "r");

    if(vcf == NULL) {
        perror("Error in opening VCF file");
        exit(-1);
    }

    char buf[128];
    while (fgets(buf, 126, vcf)) {
        //printf("%s\n", buf);
        char *token = strtok(buf, "\t");
        char *str[5];
        // Read line
        for (int i = 0; i < 5; i++) {
            //printf("%s\n", token);
            str[i] = token;
            token = strtok(NULL, "\t");
        }

        if (str[0][0] == '\n' || str[0][0] == '\r') return;

        const char *snp_contig_name = str[0];
        const char *snp_position = str[1];
        // Should be used for testing
        //const char * snp_from = str[2];
        // WARNING! str[4][0] because of \n as str[4][1]
        const char snp_to = str[4][0];

        //Find seq
        uint64_t contig_offset;
        uint64_t seq_num;
        for (int i = 0; i < mi->n_seq; i++) {
            if (strcmp(snp_contig_name, mi->seq[i].name) == 0) {
                contig_offset = mi->seq[i].offset;
                seq_num = i;
            }
        }
        uint32_t seq[7];
        seq[0] = mi->S[(contig_offset + atol(snp_position) - 1) / 8 - 3];
        seq[1] = mi->S[(contig_offset + atol(snp_position) - 1) / 8 - 2];
        seq[2] = mi->S[(contig_offset + atol(snp_position) - 1) / 8 - 1];
        seq[3] = mi->S[(contig_offset + atol(snp_position) - 1) / 8];
        seq[4] = mi->S[(contig_offset + atol(snp_position) - 1) / 8 + 1];
        seq[5] = mi->S[(contig_offset + atol(snp_position) - 1) / 8 + 2];
        seq[6] = mi->S[(contig_offset + atol(snp_position) - 1) / 8 + 3];


        //printf("%lu %lu %lu %lu %lu\n", seq[0], seq[1], seq[2], seq[3], seq[4]);
        char original_ref_seq[57];
        original_ref_seq[56] = '\0';
        for (int i = 0; i < 7; i++) {
            uint32_t tmp_seq = seq[i];
            for (int j = 0; j < 8; j++) {
                uint32_t tmp = tmp_seq % 16;
                switch (tmp) {
                    case 0:
                        original_ref_seq[i * 8 + j] = 'A';
                        break;
                    case 1:
                        original_ref_seq[i * 8 + j] = 'C';
                        break;
                    case 2:
                        original_ref_seq[i * 8 + j] = 'G';
                        break;
                    case 3:
                        original_ref_seq[i * 8 + j] = 'T';
                        break;
                    case 4:
                        original_ref_seq[i * 8 + j] = 'N';
                }
                tmp_seq = tmp_seq / 16;
            }
        }
        if (strlen(str[3]) == 1 && strlen(str[4]) == 1) {
            //SNP
            char new_ref_seq[57];
            memcpy(new_ref_seq, original_ref_seq, 57);
            new_ref_seq[24 + (contig_offset + atol(snp_position) - 1) % 8] = snp_to;

            //Finds minimizer in window
            mm128_v minimizer_array = {0, 0, 0};
            mm_sketch(0, &new_ref_seq[(contig_offset + atol(snp_position) - 1) % 8], 49, mi->w, mi->k,
                      0, mi->flag & MM_I_HPC, &minimizer_array);

            for (int i = 0; i < minimizer_array.n; i++) {
                //printf("minimizer_pos\t%ul\t%ul\n", minimizer_array.a[i].x, minimizer_array.a[i].y);
                if (minimizer_array.a[i].y < 48) continue;
                if (minimizer_array.a[i].y > 77) continue;
                minimizer_array.a[i].y = (seq_num << 32) + (atol(snp_position) - 25 + minimizer_array.a[i].y / 2) * 2 +
                                         (minimizer_array.a[i].y % 2);
                mm_idx_push(mi, minimizer_array.a[i].x, minimizer_array.a[i].y/*, fp*/);
            }
        } else if (strlen(str[3]) > 1 && strlen(str[4]) == 1) {
            //Deletion
            //Gather extra seq
            int ext_chunk_count = (strlen(str[3]) - 2) / 8 + 1;
            char * original_ref_seq_ext;
            original_ref_seq_ext = malloc(sizeof(char) * (8 * ext_chunk_count + 1));
            original_ref_seq_ext[8 * ext_chunk_count] = '\0';
            for (int i = 0; i < ext_chunk_count; i++) {
                uint32_t tmp_seq = mi->S[(contig_offset + atol(snp_position) - 1) / 8 + 3 + i + 1];;
                for (int j = 0; j < 8; j++) {
                    uint32_t tmp = tmp_seq % 16;
                    switch (tmp) {
                        case 0:
                            original_ref_seq_ext[i * 8 + j] = 'A';
                            break;
                        case 1:
                            original_ref_seq_ext[i * 8 + j] = 'C';
                            break;
                        case 2:
                            original_ref_seq_ext[i * 8 + j] = 'G';
                            break;
                        case 3:
                            original_ref_seq_ext[i * 8 + j] = 'T';
                            break;
                        case 4:
                            original_ref_seq_ext[i * 8 + j] = 'N';
                    }
                    tmp_seq = tmp_seq / 16;
                }
            }

            //Create new window
            char * new_ref_seq;
            new_ref_seq = malloc(sizeof(char) * (57 - strlen(str[3]) + 1 + ext_chunk_count * 8));
            memcpy(new_ref_seq, original_ref_seq, 24 + (contig_offset + atol(snp_position) - 1) % 8);
            new_ref_seq[24 + (contig_offset + atol(snp_position) - 1) % 8] = snp_to;
            new_ref_seq[24 + (contig_offset + atol(snp_position) - 1) % 8 + 1] = '\0';
            new_ref_seq = strcat(new_ref_seq, &original_ref_seq[24 + (contig_offset + atol(snp_position) - 1) % 8 + strlen(str[3])]);
            new_ref_seq = strcat(new_ref_seq, original_ref_seq_ext);

            //Finds minimizer in window
            mm128_v minimizer_array = {0, 0, 0};
            mm_sketch(0, &new_ref_seq[(contig_offset + atol(snp_position) - 1) % 8], 49, mi->w, mi->k,
                      0, mi->flag & MM_I_HPC, &minimizer_array);


            free(original_ref_seq_ext);
            free(new_ref_seq);

            for (int i = 0; i < minimizer_array.n; i++) {
                //printf("minimizer_pos\t%ul\t%ul\n", minimizer_array.a[i].x, minimizer_array.a[i].y);
                if (minimizer_array.a[i].y < 50) continue; // TODO change 50 to 48 for similarity with SNPS (it will take extra calculation but no changes)
                if (minimizer_array.a[i].y > 77) continue;
                minimizer_array.a[i].y = (seq_num << 32) + (atol(snp_position) - 25 + minimizer_array.a[i].y / 2) * 2 +
                                         (minimizer_array.a[i].y % 2) + (strlen(str[3]) - 1) * 2; // TODO do not add (strlen(str[3]) - 1) * 2
                mm_idx_push(mi, minimizer_array.a[i].x, minimizer_array.a[i].y/*, fp*/);
            }
        } else if (strlen(str[3]) == 1 && strlen(str[4]) > 1) {
            //Insertion
            //Create new window
            char * new_ref_seq;
            new_ref_seq = malloc(sizeof(char) * (57 + strlen(str[4]) - 1));
            memcpy(new_ref_seq, original_ref_seq, 24 + (contig_offset + atol(snp_position) - 1) % 8);
            new_ref_seq[24 + (contig_offset + atol(snp_position) - 1) % 8] = '\0';
            new_ref_seq = strcat(new_ref_seq, str[4]);
            new_ref_seq = strcat(new_ref_seq, &original_ref_seq[24 + (contig_offset + atol(snp_position) - 1) % 8 + 1]);

            //Finds minimizer in window
            mm128_v minimizer_array = {0, 0, 0};
            mm_sketch(0, &new_ref_seq[(contig_offset + atol(snp_position) - 1) % 8], 49 + strlen(str[4]) - 1, mi->w, mi->k,
                      0, mi->flag & MM_I_HPC, &minimizer_array);



            for (int i = 0; i < minimizer_array.n; i++) {
                //printf("minimizer_pos\t%ul\t%ul\n", minimizer_array.a[i].x, minimizer_array.a[i].y);
                if (minimizer_array.a[i].y < 50) continue;
                if (minimizer_array.a[i].y > 77 + (strlen(str[4]) - 1) * 2) continue;
                minimizer_array.a[i].y = (seq_num << 32) + (atol(snp_position) - 25 + minimizer_array.a[i].y / 2) * 2 +
                                         (minimizer_array.a[i].y % 2);// + (strlen(str[4]) - 1) * 2;
                mm_idx_push(mi, minimizer_array.a[i].x, minimizer_array.a[i].y/*, fp*/);
            }
            free(new_ref_seq);
        } else {
            printf("This mutation doesn't support yet.\n");
        }
    }
    fclose(vcf);
}
