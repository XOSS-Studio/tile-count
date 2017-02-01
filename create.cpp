#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "tippecanoe/projection.hpp"
#include "header.hpp"
#include "serial.hpp"
#include "merge.hpp"

void usage(char **argv) {
	fprintf(stderr, "Usage: %s -o out.count [-z zoom] [in.csv ...]\n", argv[0]);
}

int indexcmp(const void *p1, const void *p2) {
	return memcmp(p1, p2, INDEX_BYTES);
}

void read_into(FILE *out, FILE *in, const char *fname, long long &seq, int maxzoom) {
	size_t line = 0;
	char s[2000];

	unsigned long long mask = 0xFFFFFFFF;
	if (maxzoom != 32) {
		mask = mask << (32 - maxzoom);
	}
	mask &= 0xFFFFFFFF;

	while (fgets(s, 2000, in)) {
		double lon, lat;
		unsigned long long count;

		line++;
		size_t n = sscanf(s, "%lf,%lf,%llu", &lon, &lat, &count);
		if (n == 2) {
			count = 1;
		} else if (n != 3) {
			fprintf(stderr, "%s:%zu: Can't understand %s", fname, line, s);
			continue;
		}

		if (seq % 100000 == 0) {
			fprintf(stderr, "Read %.1f million records\r", seq / 1000000.0);
		}
		seq++;

		long long x, y;
		projection->project(lon, lat, 32, &x, &y);
		x &= mask;
		y &= mask;
		unsigned long long index = encode(x, y);

		while (count > MAX_COUNT) {
			write64(out, index);
			write32(out, MAX_COUNT);

			count -= MAX_COUNT;
		}

		write64(out, index);
		write32(out, count);
	}
}

void sort_and_merge(int fd, FILE *out) {
	struct stat st;
	if (fstat(fd, &st) < 0) {
		perror("stat");
		exit(EXIT_FAILURE);
	}

	long long to_sort = st.st_size;
	int bytes = RECORD_BYTES;

	int page = sysconf(_SC_PAGESIZE);
	long long unit = (50 * 1024 * 1024 / bytes) * bytes;
	while (unit % page != 0) {
		unit += bytes;
	}

	int nmerges = (to_sort + unit - 1) / unit;
	struct merge merges[nmerges];

	long long start;
	for (start = 0; start < to_sort; start += unit) {
		long long end = start + unit;
		if (end > to_sort) {
			end = to_sort;
		}

		fprintf(stderr, "Sorting part %lld of %d     \r", start / unit + 1, nmerges);

		merges[start / unit].start = start;
		merges[start / unit].end = end;
		merges[start / unit].next = NULL;

		void *map = mmap(NULL, end - start, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, start);
		if (map == MAP_FAILED) {
			perror("mmap (sort)");
			exit(EXIT_FAILURE);
		}

		qsort(map, (end - start) / bytes, bytes, indexcmp);

		// Sorting and then copying avoids the need to
		// write out intermediate stages of the sort.

		void *map2 = mmap(NULL, end - start, PROT_READ | PROT_WRITE, MAP_SHARED, fd, start);
		if (map2 == MAP_FAILED) {
			perror("mmap (write)");
			exit(EXIT_FAILURE);
		}

		memcpy(map2, map, end - start);

		munmap(map, end - start);
		munmap(map2, end - start);
	}

	void *map = mmap(NULL, to_sort, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap (for merge)");
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < nmerges; i++) {
		merges[i].map = (unsigned char *) map;
	}

	if (fwrite(header_text, HEADER_LEN, 1, out) != 1) {
		perror("write header");
		exit(EXIT_FAILURE);
	}

	merge(merges, nmerges, out, bytes, to_sort / bytes);
	munmap(map, st.st_size);
}

int main(int argc, char **argv) {
	extern int optind;
	extern char *optarg;

	char *outfile = NULL;
	int zoom = 32;

	int i;
	while ((i = getopt(argc, argv, "fz:o:")) != -1) {
		switch (i) {
		case 'z':
			zoom = atoi(optarg);
			break;

		case 'o':
			outfile = optarg;
			break;

		default:
			usage(argv);
			exit(EXIT_FAILURE);
		}
	}

	if (outfile == NULL) {
		usage(argv);
		exit(EXIT_FAILURE);
	}

	int fd = open(outfile, O_RDWR | O_CREAT | O_TRUNC, 0777);
	if (fd < 0) {
		perror(outfile);
		exit(EXIT_FAILURE);
	}
	int fd2 = dup(fd);
	if (fd2 < 0) {
		perror("dup output file");
		exit(EXIT_FAILURE);
	}
	FILE *fp = fdopen(fd2, "wb");
	if (fp == NULL) {
		perror("fdopen output file");
		exit(EXIT_FAILURE);
	}
	if (unlink(outfile) != 0) {
		perror("unlink output file");
		exit(EXIT_FAILURE);
	}

	long long seq = 0;
	if (optind == argc) {
		read_into(fp, stdin, "standard input", seq, zoom);
	} else {
		for (; optind < argc; optind++) {
			FILE *in = fopen(argv[optind], "r");
			if (in == NULL) {
				perror(argv[optind]);
				exit(EXIT_FAILURE);
			} else {
				read_into(fp, in, argv[optind], seq, zoom);
				fclose(in);
			}
		}
	}

	if (fflush(fp) != 0) {
		perror("flush output file");
		exit(EXIT_FAILURE);
	}
	if (fclose(fp) != 0) {
		perror("close output file");
		exit(EXIT_FAILURE);
	}

	fp = fopen(outfile, "wb");
	if (fp == NULL) {
		perror(outfile);
		exit(EXIT_FAILURE);
	}

	sort_and_merge(fd, fp);
	fclose(fp);

	return 0;
}
