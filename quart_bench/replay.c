/*-------------------------------------------------------------------------
 *
 * replay.c
 *		Offline replay of a captured VACUUM key stream into PostgreSQL's
 *		radix tree (lib/radixtree.h), comparing three insertion paths:
 *
 *		art   - RT_SET, one root-down descent per key (PostgreSQL's ART)
 *		stail - RT_SET_QUART, the QuART stail fast path
 *		bulk  - guarded bulk loading: a linear guard pass admits a
 *				contiguous stream (keys[i] == keys[0] + i, any offset) to
 *				RT_SET_QUART_BULK; any other stream goes to RT_SET_QUART
 *
 * No server, TID store, I/O or WAL is involved: the same keys go through the
 * same tree code, so the difference isolates the insertion path.  Keys are
 * block numbers stored as little-endian uint64 (see quart_keydump_file).
 *
 * Each trial times every path "reps" times, rotating the order of the paths
 * per repetition, and keeps the fastest run per path; the reported time is
 * the median over trials.  Every tree is verified against the key stream.
 *
 * Usage: replay [-r reps] [-t trials] [-H] keys.bin
 * Prints one CSV row (with -H, a header line first).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include "utils/memutils.h"

#define RT_PREFIX rt
#define RT_SCOPE static
#define RT_DECLARE
#define RT_DEFINE
#define RT_USE_QUART
#define RT_VALUE_TYPE uint64
#include "lib/radixtree.h"

#define NPATHS 3
static const char *const path_names[NPATHS] = {"art", "stail", "bulk"};

static uint64 *keys;
static uint64 *values;
static size_t nkeys;
static size_t ndistinct;
static bool contiguous;

static double
now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

static int
cmp_double(const void *a, const void *b)
{
	double		x = *(const double *) a;
	double		y = *(const double *) b;

	return (x > y) - (x < y);
}

/* the bulk guard: one linear pass, stopping at the first gap */
static bool
guard_contiguous(void)
{
	for (size_t i = 1; i < nkeys; i++)
		if (keys[i] != keys[i - 1] + 1)
			return false;
	return nkeys > 0;
}

/* build a tree through one insertion path; returns elapsed seconds */
static double
build(int path, rt_radix_tree **tree_out)
{
	rt_radix_tree *tree = rt_create(CurrentMemoryContext);
	double		start = now_sec();

	switch (path)
	{
		case 0:
			for (size_t i = 0; i < nkeys; i++)
				rt_set(tree, keys[i], &values[i]);
			break;
		case 1:
			for (size_t i = 0; i < nkeys; i++)
				rt_set_quart(tree, keys[i], &values[i]);
			break;
		case 2:
			if (guard_contiguous())
				rt_set_quart_bulk(tree, keys, values, nkeys);
			else
				for (size_t i = 0; i < nkeys; i++)
					rt_set_quart(tree, keys[i], &values[i]);
			break;
	}

	start = now_sec() - start;
	if (tree_out)
		*tree_out = tree;
	else
		rt_free(tree);
	return start;
}

/* every key must map to its value, and nothing else may be in the tree */
static bool
verify(int path)
{
	rt_radix_tree *tree;
	bool		ok = true;

	build(path, &tree);
	for (size_t i = 0; i < nkeys && ok; i++)
	{
		uint64	   *v = rt_find(tree, keys[i]);

		ok = (v != NULL && *v == values[i]);
	}
	if (ok && tree->ctl->num_keys != (int64) ndistinct)
		ok = false;
	if (!ok)
		fprintf(stderr, "verify FAILED for path %s\n", path_names[path]);
	rt_free(tree);
	return ok;
}

static void
load_keys(const char *file)
{
	FILE	   *fp = fopen(file, "rb");
	long		bytes;
	uint64	   *sorted;

	if (fp == NULL || fseek(fp, 0, SEEK_END) != 0 || (bytes = ftell(fp)) < 0)
	{
		fprintf(stderr, "cannot read %s\n", file);
		exit(1);
	}
	nkeys = bytes / sizeof(uint64);
	keys = malloc(nkeys * sizeof(uint64) + 1);
	values = malloc(nkeys * sizeof(uint64) + 1);
	rewind(fp);
	if (fread(keys, sizeof(uint64), nkeys, fp) != nkeys)
	{
		fprintf(stderr, "short read on %s\n", file);
		exit(1);
	}
	fclose(fp);

	for (size_t i = 0; i < nkeys; i++)
		values[i] = keys[i];

	/* count distinct keys (a VACUUM stream has none repeated, but check) */
	sorted = malloc(nkeys * sizeof(uint64) + 1);
	memcpy(sorted, keys, nkeys * sizeof(uint64));
	qsort(sorted, nkeys, sizeof(uint64), cmp_u64);
	ndistinct = 0;
	for (size_t i = 0; i < nkeys; i++)
		if (i == 0 || sorted[i] != sorted[i - 1])
			ndistinct++;
	free(sorted);

	contiguous = guard_contiguous();
}

int
main(int argc, char **argv)
{
	int			reps = 10;
	int			trials = 3;
	bool		header = false;
	const char *file = NULL;
	double		best[NPATHS];
	double	   *trial_best[NPATHS];
	double		med[NPATHS];
	double		guard_best = 1e18;
	bool		ok = true;
	uint64		lo,
				hi;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
			reps = atoi(argv[++i]);
		else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			trials = atoi(argv[++i]);
		else if (strcmp(argv[i], "-H") == 0)
			header = true;
		else
			file = argv[i];
	}
	if (file == NULL || reps < 1 || trials < 1)
	{
		fprintf(stderr, "usage: %s [-r reps] [-t trials] [-H] keys.bin\n", argv[0]);
		return 1;
	}

	MemoryContextInit();
	load_keys(file);
	if (nkeys == 0)
	{
		fprintf(stderr, "%s holds no keys\n", file);
		return 1;
	}

	lo = hi = keys[0];
	for (size_t i = 1; i < nkeys; i++)
	{
		lo = Min(lo, keys[i]);
		hi = Max(hi, keys[i]);
	}

	for (int p = 0; p < NPATHS; p++)
		trial_best[p] = malloc(sizeof(double) * trials);

	for (int t = 0; t < trials; t++)
	{
		for (int p = 0; p < NPATHS; p++)
			best[p] = 1e18;
		for (int r = 0; r < reps; r++)
		{
			for (int k = 0; k < NPATHS; k++)
			{
				int			p = (r + k) % NPATHS;

				best[p] = Min(best[p], build(p, NULL));
			}
		}
		for (int p = 0; p < NPATHS; p++)
			trial_best[p][t] = best[p];
	}
	for (int p = 0; p < NPATHS; p++)
	{
		qsort(trial_best[p], trials, sizeof(double), cmp_double);
		med[p] = trial_best[p][trials / 2];
	}

	for (int r = 0; r < reps; r++)
	{
		double		start = now_sec();
		volatile bool admitted = guard_contiguous();

		(void) admitted;
		guard_best = Min(guard_best, now_sec() - start);
	}

	for (int p = 0; p < NPATHS; p++)
		ok &= verify(p);

	if (header)
		printf("file,keys,min_key,max_key,density,contiguous,"
			   "art_ns,stail_ns,stail_speedup,bulk_path,bulk_ns,bulk_speedup,"
			   "guard_ns,verify\n");
	printf("%s,%zu,%llu,%llu,%.3f,%s,%.2f,%.2f,%.3f,%s,%.2f,%.3f,%.3f,%s\n",
		   file, nkeys, (unsigned long long) lo, (unsigned long long) hi,
		   (double) ndistinct / (double) (hi - lo + 1),
		   contiguous ? "yes" : "no",
		   med[0] / nkeys * 1e9, med[1] / nkeys * 1e9, med[0] / med[1],
		   contiguous ? "bulk" : "stail",
		   med[2] / nkeys * 1e9, med[0] / med[2],
		   guard_best / nkeys * 1e9,
		   ok ? "ok" : "FAIL");

	return ok ? 0 : 1;
}
