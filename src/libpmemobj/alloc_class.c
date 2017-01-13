/*
 * Copyright 2016-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * alloc_class.c -- implementation of allocation classes
 */

#include <float.h>
#include <string.h>

#include "alloc_class.h"
#include "heap_layout.h"
#include "util.h"
#include "out.h"
#include "bucket.h"

/*
 * Value used to mark a reserved spot in the bucket array.
 */
#define ACLASS_RESERVED ((void *)0xFFFFFFFFULL)

/*
 * The last size that is handled by runs.
 */
#define MAX_RUN_SIZE (CHUNKSIZE * 10)

/*
 * Maximum number of bytes the allocation class generation algorithm can decide
 * to waste in a single run chunk.
 */
#define MAX_RUN_WASTED_BYTES 1024

/*
 * Allocation categories are used for allocation classes generation. Each one
 * defines the biggest handled size (in alloc blocks) and step of the generation
 * process. The bigger the step the bigger is the acceptable internal
 * fragmentation. For each category the internal fragmentation can be calculated
 * as: step/size. So for step == 1 the acceptable fragmentation is 0% and so on.
 */
#define MAX_ALLOC_CATEGORIES 6

/*
 * The first size (in alloc blocks) which is actually used in the allocation
 * class generation algorithm. All smaller sizes use the first predefined bucket
 * with the smallest run unit size.
 */
#define FIRST_GENERATED_CLASS_SIZE 2

static struct {
	size_t size;
	size_t step;
} categories[MAX_ALLOC_CATEGORIES] = {
	/* dummy category - the first allocation class is predefined */
	{FIRST_GENERATED_CLASS_SIZE, 0},

	{16, 1},
	{64, 2},
	{256, 4},
	{512, 8},
	{1024, 128},
};

#define RUN_UNIT_MAX 64U
#define RUN_UNIT_MAX_ALLOC 8U

/*
 * Every allocation has to be a multiple of a cacheline because we need to
 * ensure proper alignment of every pmem structure.
 */
#define ALLOC_BLOCK_SIZE 64

/*
 * Converts size (in bytes) to number of allocation blocks.
 */
#define SIZE_TO_CLASS_MAP_INDEX(_s) (1 + (((_s) - 1) / ALLOC_BLOCK_SIZE))

/*
 * Calculates the size in bytes of a single run instance
 */
#define RUN_SIZE_BYTES(size_idx)\
(RUNSIZE + ((size_idx - 1) * CHUNKSIZE))

/*
 * Target number of allocations per run instance.
 */
#define RUN_MIN_NALLOCS 500

/*
 * Hard limit of chunks per single run.
 */
#define RUN_SIZE_IDX_CAP (8)

struct alloc_class_collection {
	struct alloc_class *aclasses[MAX_ALLOCATION_CLASSES];

	/*
	 * The last size (in bytes) that is handled by runs, everything bigger
	 * uses the default class.
	 */
	size_t last_run_max_size;

	/* maps allocation classes to allocation sizes, excluding the header! */
	uint8_t class_map_by_alloc_size[(MAX_RUN_SIZE / ALLOC_BLOCK_SIZE) + 1];

	/* maps allocation classes to run unit sizes */
	uint8_t class_map_by_unit_size[(MAX_RUN_SIZE / ALLOC_BLOCK_SIZE) + 1];

	struct alloc_class *default_allocation_class;
};


/*
 * alloc_class_find_first_free_slot -- (internal) searches for the
 *	first available allocation class slot
 *
 * This function must be thread-safe because allocation classes can be created
 * at runtime.
 */
static int
alloc_class_find_first_free_slot(struct alloc_class_collection *ac,
	uint8_t *slot)
{
	for (int n = 0; n < MAX_ALLOCATION_CLASSES; ++n) {
		if (util_bool_compare_and_swap64(&ac->aclasses[n],
				NULL, ACLASS_RESERVED)) {
			*slot = (uint8_t)n;
			return 0;
		}
	}

	return -1;
}

/*
 * alloc_class_new -- (internal) creates a new allocation class
 */
static struct alloc_class *
alloc_class_new(struct alloc_class_collection *ac,
	enum alloc_class_type type,
	size_t unit_size,
	unsigned unit_max, unsigned unit_max_alloc,
	uint32_t size_idx)
{
	struct alloc_class *c = Malloc(sizeof(*c));
	if (c == NULL)
		return NULL;

	c->unit_size = unit_size;
	c->header_type = HEADER_COMPACT;
	c->type = type;

	switch (type) {
		case CLASS_HUGE:
			c->id = DEFAULT_ALLOC_CLASS_ID;
			ac->default_allocation_class = c;
			break;
		case CLASS_RUN:
			c->run.unit_max = unit_max;
			c->run.unit_max_alloc = unit_max_alloc;
			c->run.size_idx = size_idx;

			/*
			 * Here the bitmap definition is calculated based on the
			 * size of the available memory and the size of
			 * a memory block - the result of dividing those two
			 * numbers is the number of possible allocations from
			 * that block, and in other words, the amount of bits
			 * in the bitmap.
			 */
			c->run.bitmap_nallocs = (uint32_t)
				(RUN_SIZE_BYTES(c->run.size_idx) / unit_size);

			/*
			 * The two other numbers that define our bitmap is the
			 * size of the array that represents the bitmap and the
			 * last value of that array with the bits that exceed
			 * number of blocks marked as set (1).
			 */
			ASSERT(c->run.bitmap_nallocs <= RUN_BITMAP_SIZE);
			unsigned unused_bits =
				RUN_BITMAP_SIZE - c->run.bitmap_nallocs;

			unsigned unused_values = unused_bits / BITS_PER_VALUE;

			ASSERT(MAX_BITMAP_VALUES >= unused_values);
			c->run.bitmap_nval = MAX_BITMAP_VALUES - unused_values;

			ASSERT(unused_bits >= unused_values * BITS_PER_VALUE);
			unused_bits -= unused_values * BITS_PER_VALUE;

			c->run.bitmap_lastval = unused_bits ?
				(((1ULL << unused_bits) - 1ULL) <<
					(BITS_PER_VALUE - unused_bits)) : 0;

			uint8_t slot;
			if (alloc_class_find_first_free_slot(ac, &slot) != 0) {
				Free(c);
				return NULL;
			}

			c->id = slot;
			ac->aclasses[slot] = c;

			break;
		default:
			ASSERT(0);
	}

	return c;
}

/*
 * alloc_class_delete -- (internal) deletes an allocation class
 */
static void
alloc_class_delete(struct alloc_class_collection *ac,
	struct alloc_class *c)
{
	ac->aclasses[c->id] = NULL;
	Free(c);
}

/*
 * alloc_class_find_or_create -- (internal) searches for the
 * biggest allocation class for which unit_size is evenly divisible by n.
 * If no such class exists, create one.
 */
static struct alloc_class *
alloc_class_find_or_create(struct alloc_class_collection *ac, size_t n)
{
	COMPILE_ERROR_ON(MAX_ALLOCATION_CLASSES > UINT8_MAX);
	uint64_t required_size_bytes = (uint32_t)n * RUN_MIN_NALLOCS;
	uint32_t required_size_idx = 1;
	if (required_size_bytes > RUNSIZE) {
		required_size_bytes -= RUNSIZE;
		required_size_idx +=
			CALC_SIZE_IDX(CHUNKSIZE, required_size_bytes);
		if (required_size_idx > RUN_SIZE_IDX_CAP)
			required_size_idx = RUN_SIZE_IDX_CAP;
	}

	for (int i = MAX_ALLOCATION_CLASSES - 1; i >= 0; --i) {
		struct alloc_class *c = ac->aclasses[i];

		if (c == NULL || c->run.size_idx < required_size_idx)
			continue;

		if (n % c->unit_size == 0 &&
			n / c->unit_size <= c->run.unit_max_alloc)
			return c;
	}

	/*
	 * In order to minimize the wasted space at the end of the run the
	 * run data size must be divisible by the allocation class unit size
	 * with the smallest possible remainder, preferably 0.
	 */
	size_t runsize_bytes = RUN_SIZE_BYTES(required_size_idx);
	while ((runsize_bytes % n) > MAX_RUN_WASTED_BYTES) {
		n += ALLOC_BLOCK_SIZE;
	}

	/*
	 * Now that the desired unit size is found the existing classes need
	 * to be searched for possible duplicates. If a class with the
	 * calculated unit size already exists, simply return that.
	 */
	for (int i = MAX_ALLOCATION_CLASSES - 1; i >= 0; --i) {
		struct alloc_class *c = ac->aclasses[i];
		if (c == NULL)
			continue;
		if (c->unit_size == n)
			return c;
	}

	return alloc_class_new(ac, CLASS_RUN, n,
		RUN_UNIT_MAX, RUN_UNIT_MAX_ALLOC, required_size_idx);
}

/*
 * alloc_class_find_min_frag -- searches for an existing allocation
 * class that will provide the smallest internal fragmentation for the given
 * size.
 */
static struct alloc_class *
alloc_class_find_min_frag(struct alloc_class_collection *ac, size_t n)
{
	struct alloc_class *best_c = NULL;
	float best_frag = FLT_MAX;

	ASSERTne(n, 0);

	/*
	 * Start from the largest buckets in order to minimize unit size of
	 * allocated memory blocks.
	 */
	for (int i = MAX_ALLOCATION_CLASSES - 1; i >= 0; --i) {
		struct alloc_class *c = ac->aclasses[i];

		if (c == NULL)
			continue;

		size_t units = CALC_SIZE_IDX(c->unit_size, n);
		/* can't exceed the maximum allowed run unit max */
		if (units > c->run.unit_max_alloc)
			break;

		float frag = (float)(c->unit_size * units) / (float)n;
		if (frag == 1.f)
			return c;

		ASSERT(frag >= 1.f);
		if (frag < best_frag) {
			best_c = c;
			best_frag = frag;
		}
	}

	ASSERTne(best_c, NULL);
	return best_c;
}

/*
 * alloc_class_collection_new -- creates a new collection of allocation classes
 */
struct alloc_class_collection *
alloc_class_collection_new(void)
{
	struct alloc_class_collection *ac = Malloc(sizeof(*ac));
	if (ac == NULL)
		return NULL;

	memset(ac->aclasses, 0, sizeof(ac->aclasses));

	ac->last_run_max_size = MAX_RUN_SIZE;

	if (alloc_class_new(ac, CLASS_HUGE, CHUNKSIZE, 0, 0, 1) == NULL)
		goto error_alloc_class_create;

	struct alloc_class *predefined_class =
		alloc_class_new(ac, CLASS_RUN, MIN_RUN_SIZE,
			RUN_UNIT_MAX, RUN_UNIT_MAX_ALLOC, 1);
	if (predefined_class == NULL)
		goto error_alloc_class_create;

	for (size_t i = 0; i < FIRST_GENERATED_CLASS_SIZE; ++i) {
		ac->class_map_by_unit_size[i] = predefined_class->id;
		ac->class_map_by_alloc_size[i] = predefined_class->id;
	}

	/*
	 * Based on the defined categories, a set of allocation classes is
	 * created. The unit size of those classes is depended on the category
	 * initial size and step.
	 */
	size_t size = 0;
	for (int c = 1; c < MAX_ALLOC_CATEGORIES; ++c) {
		for (size_t i = categories[c - 1].size + 1;
			i <= categories[c].size; i += categories[c].step) {

			size = i + (categories[c].step - 1);
			if (alloc_class_find_or_create(ac,
				size * ALLOC_BLOCK_SIZE) == NULL)
				goto error_alloc_class_create;
		}
	}

	/*
	 * Find the largest alloc class and use it's unit size as run allocation
	 * threshold.
	 */
	uint8_t largest_aclass_slot;
	for (largest_aclass_slot = MAX_ALLOCATION_CLASSES - 1;
			largest_aclass_slot > 0 &&
			ac->aclasses[largest_aclass_slot] == NULL;
			--largest_aclass_slot) {
		/* intentional NOP */
	}

	struct alloc_class *c = ac->aclasses[largest_aclass_slot];

	/*
	 * The actual run might contain less unit blocks than the theoretical
	 * unit max variable. This may be the case for very large unit sizes.
	 */
	size_t real_unit_max = c->run.bitmap_nallocs < c->run.unit_max_alloc ?
		c->run.bitmap_nallocs : c->run.unit_max_alloc;

	size_t theoretical_run_max_size = c->unit_size * real_unit_max;

	ac->last_run_max_size = MAX_RUN_SIZE > theoretical_run_max_size ?
		theoretical_run_max_size : MAX_RUN_SIZE;

	/*
	 * Now that the alloc classes are created, the bucket with the minimal
	 * internal fragmentation for that size is chosen.
	 */
	for (size_t i = FIRST_GENERATED_CLASS_SIZE;
		i <= ac->last_run_max_size / ALLOC_BLOCK_SIZE; ++i) {
		struct alloc_class *c = alloc_class_find_min_frag(ac,
				i * ALLOC_BLOCK_SIZE);
		ac->class_map_by_unit_size[i] = c->id;
		size_t header_offset = CALC_SIZE_IDX(ALLOC_BLOCK_SIZE,
			header_type_to_size[c->header_type]);
		ac->class_map_by_alloc_size[i - header_offset] = c->id;
		/* this is here to make sure the last entry is filled */
		ac->class_map_by_alloc_size[i] = c->id;
	}

#ifdef DEBUG
	/*
	 * Verify that each bucket's unit size points back to the bucket by the
	 * bucket map. This must be true for the default allocation classes,
	 * otherwise duplicate buckets will be created.
	 */
	for (size_t i = 0; i < MAX_ALLOCATION_CLASSES; ++i) {
		struct alloc_class *c = ac->aclasses[i];

		if (c != NULL) {
			ASSERTeq(i, c->id);
			uint8_t class_id = ac->class_map_by_unit_size[
				SIZE_TO_CLASS_MAP_INDEX(c->unit_size)];

			ASSERTeq(class_id, c->id);
		}
	}
#endif

	return ac;

error_alloc_class_create:
	alloc_class_collection_delete(ac);

	return NULL;
}

/*
 * alloc_class_collection_delete -- deletes the allocation class collection and
 *	all of the classes within it
 */
void
alloc_class_collection_delete(struct alloc_class_collection *ac)
{
	for (size_t i = 0; i < MAX_ALLOCATION_CLASSES; ++i) {
		struct alloc_class *c = ac->aclasses[i];
		if (c != NULL) {
			alloc_class_delete(ac, c);
		}
	}
	alloc_class_delete(ac, ac->default_allocation_class);
	Free(ac);
}

/*
 * alloc_class_get_create_by_unit_size -- searches for an allocation class
 *	with the unit size matching the provided size, if no such class exists
 *	creates one.
 */
struct alloc_class *
alloc_class_get_create_by_unit_size(struct alloc_class_collection *ac,
	size_t size)
{
	struct alloc_class *c = ac->aclasses[
			ac->class_map_by_unit_size[
				SIZE_TO_CLASS_MAP_INDEX(size)]
		];

	if (c == NULL || c->unit_size != size)
		c = alloc_class_new(ac, CLASS_RUN, size,
			RUN_UNIT_MAX, RUN_UNIT_MAX_ALLOC, 1);

	return c;
}

/*
 * alloc_class_by_alloc_size -- returns allocation class that is assigned
 *	to handle an allocation of the provided size
 */
struct alloc_class *
alloc_class_by_alloc_size(struct alloc_class_collection *ac, size_t size)
{
	if (size < ac->last_run_max_size) {
		return ac->aclasses[
				ac->class_map_by_alloc_size[
					SIZE_TO_CLASS_MAP_INDEX(size)]
			];
	} else {
		return ac->default_allocation_class;
	}
}

/*
 * alloc_class_by_id -- returns the allocation class with an id
 */
struct alloc_class *
alloc_class_by_id(struct alloc_class_collection *ac, uint8_t id)
{
	return id == DEFAULT_ALLOC_CLASS_ID ?
		ac->default_allocation_class : ac->aclasses[id];
}
