/*
 * Copyright (c) 1988 Mike Haertel
 * Copyright (c) 1991 Douglas C. Schmidt
 * Copyright (c) 2012 Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * Sorting routines that do not call zalloc() or xmalloc().
 *
 * Most of this code comes from the GNU C library and was adapted by Raphael
 * Manfredi for inclusion into this library, mostly to remove all malloc()
 * dependency, strip libc internal dependencies, and reformat to our coding
 * standards.
 *
 * The excellent quicksort() implementation from Douglas C. Schmidt was further
 * optimized: maximize the chances of picking a good pivot when the partition
 * is large, optimize insertsort() when dealing with aligned items that are
 * multiples of words, and detect an already sorted partition or one that is
 * almost-sorted to discontinue quicksort() and switch to insertsort() instead.
 *
 * @author Mike Haertel
 * @date 1988
 * @author Douglas C. Schmidt
 * @date 1991
 * @author Raphael Manfredi
 * @date 2012
 */

#include "common.h"

#include "xsort.h"
#include "getphysmemsize.h"
#include "mempcpy.h"
#include "op.h"
#include "unsigned.h"
#include "vmm.h"

#include "override.h"			/* Must be the last header included */

/*
 * Quicksort algorithm.
 * Written by Douglas C. Schmidt (schmidt@ics.uci.edu).
 */

/*
 * If you consider tuning this algorithm, you should consult first:
 * Engineering a sort function; Jon Bentley and M. Douglas McIlroy;
 * Software - Practice and Experience; Vol. 23 (11), 1249-1265, 1993.
 */

/*
 * Discontinue quicksort algorithm when partition gets below this size.
 * 4 was a particular magic number chosen to work best on a Sun 4/260.
 * 7 seems to be working well on Intel CPUs.
 */
#define MAX_THRESH 7

/*
 * Use carefully-chosen median insted of median-of-3 when there are more
 * items in the partition than this minimum.
 */
#define MIN_MEDIAN	40

/*
 * Threshold on the amount of items we swap in a partition to guide us in
 * deciding whether it is almost sorted and insersort would be more efficient
 * than quicksort to complete the sorting.
 */
#define SWAP_THRESH 1

/* Stack node declarations used to store unfulfilled partition obligations. */
typedef struct {
	char *lo;
	char *hi;
} stack_node;

/*
 * The next 4 #defines implement a very fast in-line stack abstraction.
 *
 * The stack needs log (total_elements) entries (we could even subtract
 * log(MAX_THRESH)).  Since total_elements has type size_t, we get as
 * upper bound for log (total_elements):
 * bits per byte (CHAR_BIT) * sizeof(size_t).
 */
#define STACK_SIZE	(CHAR_BIT * sizeof(size_t))
#define PUSH(low, high)	((void) ((top->lo = (low)), (top->hi = (high)), ++top))
#define	POP(low, high)	((void) (--top, (low = top->lo), (high = top->hi)))
#define	STACK_NOT_EMPTY	(stack < top)

/**
 * Insertion sort for small partions or ones that are believed already sorted.
 *
 * When the partition is larger than MAX_THRESH items, detect that we are
 * facing pathological input and bail out in the middle if needed.
 *
 * @return NULL if OK, the address of the last sorted item if we decided
 * to bail-out.
 */
static G_GNUC_HOT char *
insertsort(void *const pbase, size_t lastoff, size_t size, xsort_cmp_t cmp)
{
	char *base_ptr = pbase;
	char *const end_ptr = &base_ptr[lastoff];	/* Last item */
	char *tmp_ptr = base_ptr;
	char *thresh_ptr;
	register char *run_ptr;
	size_t n;
	size_t moved = 0;

	if G_UNLIKELY(0 == lastoff)
		return NULL;

	/*
	 * We're called with a supposedly almost sorted array.
	 *
	 * Find smallest element in the first few locations and place it at the
	 * array's beginning.  This is likely the smallest array element, and the
	 * operation speeds up insertion sort's inner loop.
	 */

	thresh_ptr = ptr_add_offset(pbase, MAX_THRESH * size);
	thresh_ptr = MIN(thresh_ptr, end_ptr);

	for (run_ptr = tmp_ptr + size; run_ptr <= thresh_ptr; run_ptr += size) {
		if ((*cmp)(run_ptr, tmp_ptr) < 0)
			tmp_ptr = run_ptr;
	}

	if G_LIKELY(tmp_ptr != base_ptr) {
		SWAP(tmp_ptr, base_ptr, size);
		moved = size;
	}

	/* Insertion sort, running from left-hand-side up to right-hand-side */

	run_ptr = base_ptr + size;
	n = (0 == size % OPSIZ && 0 == (base_ptr - (char *) 0) % OPSIZ) ?
		size / OPSIZ : 0;

	while ((run_ptr += size) <= end_ptr) {
		tmp_ptr = run_ptr - size;
		while ((*cmp)(run_ptr, tmp_ptr) < 0) {
			tmp_ptr -= size;
		}

		tmp_ptr += size;
		if (tmp_ptr != run_ptr) {
			/*
			 * If the partition is larger than MAX_THRESH items, then attempt
			 * to detect when we're not facing sorted input and we run the
			 * risk of approaching O(n^2) complexity.
			 *
			 * In that case, bail out and quicksort() will pick up where
			 * we left.
			 *
			 * The criteria is that we must not move around more than about
			 * twice the size of the arena.  This is only checked past the
			 * threshold to prevent any value checking from quicksort() when
			 * we are called with a small enough partition, where complexity
			 * is not an issue.
			 *
			 * Exception when we reach the last item: regardless of where it
			 * will land, the cost now should be less than bailing out and
			 * resuming quicksort() on the partition, so finish off the sort.
			 */

			if G_UNLIKELY(run_ptr > thresh_ptr && run_ptr != end_ptr) {
				if (moved > 2 * lastoff)
					return run_ptr - size;	/* Last sorted address */
			}

			moved += ptr_diff(run_ptr, tmp_ptr) + size;

			if G_LIKELY(n != 0) {
				/* Operates on words */
				op_t *trav = (op_t *) (run_ptr + size);
				op_t *r = (op_t *) run_ptr;
				op_t *t = (op_t *) tmp_ptr;

				while (--trav >= r) {
					op_t c = *trav;
					register op_t *hi, *lo;

					for (hi = lo = trav; (lo -= n) >= t; hi = lo) {
						*hi = *lo;
					}
					*hi = c;
				}
			} else {
				/* Operates on bytes */
				char *trav = run_ptr + size;

				while (--trav >= run_ptr) {
					char c = *trav;
					register char *hi, *lo;

					for (hi = lo = trav; (lo -= size) >= tmp_ptr; hi = lo) {
						*hi = *lo;
					}
					*hi = c;
				}
			}
		}
	}

	return NULL;	/* OK, fully sorted */
}

/**
 * Sort 3 items to ensure lo <= mid <= hi.
 */
static inline void 
order_three(void *lo, void *mid, void *hi, size_t size, xsort_cmp_t cmp)
{
	if ((*cmp)(mid, lo) < 0)
		SWAP(mid, lo, size);
	/* lo <= mid */
	if ((*cmp)(hi, mid) < 0) {
		SWAP(hi, mid, size);
		/* mid < hi */
		if ((*cmp)(mid, lo) < 0)
			SWAP(mid, lo, size);
		/* lo <= mid < hi */
	}
	/* lo <= mid <= hi */
}

/**
 * Return position of median among 3 items without re-arranging items.
 */
static inline void *
median_three(void *a, void *b, void *c, xsort_cmp_t cmp)
{
	return (*cmp)(a, b) < 0 ?
		((*cmp)(b, c) < 0 ? b : ((*cmp)(a, c) < 0 ? c : a )) :
		((*cmp)(b, c) > 0 ? b : ((*cmp)(a, c) < 0 ? a : c ));
}

/*
 * Order size using quicksort.  This implementation incorporates
 * four optimizations discussed in Sedgewick:
 *
 * 1. Non-recursive, using an explicit stack of pointer that store the
 *    next array partition to sort.  To save time, this maximum amount
 *    of space required to store an array of SIZE_MAX is allocated on the
 *    stack.  Assuming a 32-bit (64 bit) integer for size_t, this needs
 *    only 32 * sizeof(stack_node) == 256 bytes (for 64 bit: 1024 bytes).
 *    Pretty cheap, actually.
 *
 * 2. Chose the pivot element using a median-of-three decision tree.
 *    This reduces the probability of selecting a bad pivot value and
 *    eliminates certain extraneous comparisons.
 *
 * 3. Only quicksorts TOTAL_ELEMS / MAX_THRESH partitions, leaving
 *    insertion sort to order the MAX_THRESH items within each partition.
 *    This is a big win, since insertion sort is faster for small, mostly
 *    sorted array segments.
 *
 * 4. The larger of the two sub-partitions is always pushed onto the
 *    stack first, with the algorithm then concentrating on the
 *    smaller partition.  This *guarantees* no more than log (total_elems)
 *    stack size is needed (actually O(1) in this case)!
 */

static G_GNUC_HOT void
quicksort(void *const pbase, size_t total_elems, size_t size, xsort_cmp_t cmp)
{
	char *base_ptr = pbase;
	const size_t max_thresh = MAX_THRESH * size;

	if G_UNLIKELY(total_elems == 0)
		return;	/* Avoid lossage with unsigned arithmetic below.  */

	if (total_elems > MAX_THRESH) {
		char *lo = base_ptr;
		char *hi = &lo[size * (total_elems - 1)];
		stack_node stack[STACK_SIZE];
		stack_node *top = stack + 1;

		while (STACK_NOT_EMPTY) {
			register char *left_ptr;
			register char *right_ptr;
			size_t items = (hi - lo) / size;
			register char *pivot = lo + size * (items >> 1);
			size_t swapped;

			/*
			 * If there are more than MIN_MEDIAN items, it pays to spend
			 * more time selecting a good pivot by doing a median over
			 * several items.
			 *		--RAM, 2012-03-02
			 */

			if (items > MIN_MEDIAN) {
				size_t d = size * (items >> 3);
				char *plo, *phi;

				plo = median_three(lo, lo + d, lo + 2*d, cmp);
				pivot = median_three(pivot - d, pivot, pivot + d, cmp);
				phi = median_three(hi - 2*d, hi - d, hi, cmp);
				pivot = median_three(plo, pivot, phi, cmp);
				left_ptr = lo + (lo == pivot ? size : 0);
				right_ptr = hi - (hi == pivot ? size : 0);
			} else {
				/*
				 * Select median value from among LO, MID, and HI. Rearrange
				 * LO and HI so the three values are sorted. This lowers the
				 * probability of picking a pathological pivot value and
				 * skips a comparison for both the LEFT_PTR and RIGHT_PTR in
				 * the while loops.
				 */

				order_three(lo, pivot, hi, size, cmp);
				left_ptr  = lo + size;
				right_ptr = hi - size;
			}

			/*
			 * Here's the famous ``collapse the walls'' section of quicksort.
			 * Gotta like those tight inner loops!  They are the main reason
			 * that this algorithm runs much faster than others.
			 */

			swapped = 0;	/* Detect sorted partition --RAM */

			do {
				/*
				 * Optimiziation by Raphael Manfredi to avoid a comparison
				 * of the pivot element with itself.
				 *
				 * This also protects code that asserts no two items can be
				 * identical when we have meta-knowledge that all the items
				 * in the sorted array are different.
				 *
				 * Some code in gtk-gnutella expects the sort callback to
				 * never be called with items that would compare as equal
				 * when we are sorting a set!
				 *		--RAM, 2012-03-01.
				 */

				while (left_ptr != pivot && (*cmp)(left_ptr, pivot) < 0)
					left_ptr += size;

				while (right_ptr != pivot && (*cmp)(pivot, right_ptr) < 0)
					right_ptr -= size;

				if G_LIKELY(left_ptr < right_ptr) {
					SWAP(left_ptr, right_ptr, size);
					swapped++;

					/* Update pivot address */
					if (left_ptr == pivot)
						pivot = right_ptr;	/* New pivot: we swapped items */
					else if (right_ptr == pivot)
						pivot = left_ptr;

					left_ptr += size;
					right_ptr -= size;
				} else if (left_ptr == right_ptr) {
					left_ptr += size;
					right_ptr -= size;
					break;
				}
			} while (left_ptr <= right_ptr);

			/*
			 * Optimization by Raphael Manfredi: if we only swapped a few
			 * items in the partition, use insertsort() on it and do not
			 * recurse.  This greatly accelerates quicksort() on already
			 * sorted arrays.
			 *
			 * However, because we may have guessed wrong, intersort() monitors
			 * pathological cases and can bail out (when we hand out more than
			 * MAX_THRESH items). Hence we must monitor the result and continue
			 * as if we hadn't call insertsort() when it returns a non-NULL
			 * pointer.
			 *
			 * This works because insertsort() processes its input from left
			 * to right and therefore will not disrupt the "left/right"
			 * partitionning with respect to the pivot value and the already
			 * computed left_ptr and right_ptr boundaries.
			 */
			if G_UNLIKELY(swapped <= SWAP_THRESH) {
				/* Switch to insertsort() to completely sort this partition */
				char *last = insertsort(lo, ptr_diff(hi, lo), size, cmp);
				if (NULL == last) {
					POP(lo, hi);	/* Done with partition */
					continue;
				}

				if (last >= right_ptr)
					right_ptr = lo;		/* Mark "left" as fully sorted */

				/* Continue as if we hadn't called insertsort() */
			}

			/*
			 * Set up pointers for next iteration.  First determine whether
			 * left and right partitions are below the threshold size.  If so,
			 * insertsort one or both.  Otherwise, push the larger partition's
			 * bounds on the stack and continue quicksorting the smaller one.
			 *
			 * Change by Raphael Manfredi: immediately do the insertsort of
			 * the small partitions instead of waiting for the end of quicksort
			 * to benefit from the locality of reference, at the expense of
			 * more setup costs.
			 */

			if G_UNLIKELY(ptr_diff(right_ptr, lo) <= max_thresh) {
				insertsort(lo, ptr_diff(right_ptr, lo), size, cmp);
				if G_UNLIKELY(ptr_diff(hi, left_ptr) <= max_thresh) {
					insertsort(left_ptr, ptr_diff(hi, left_ptr), size, cmp);
					POP(lo, hi);	/* Ignore both small partitions. */
				} else
					lo = left_ptr;	/* Ignore small left partition. */
			} else if G_UNLIKELY(ptr_diff(hi, left_ptr) <= max_thresh) {
				insertsort(left_ptr, ptr_diff(hi, left_ptr), size, cmp);
				hi = right_ptr;		/* Ignore small right partition. */
			} else if (ptr_diff(right_ptr, lo) > ptr_diff(hi, left_ptr)) {
				/* Push larger left partition indices. */
				PUSH(lo, right_ptr);
				lo = left_ptr;
			} else {
				/* Push larger right partition indices. */
				PUSH (left_ptr, hi);
				hi = right_ptr;
			}
		}
	} else {
		insertsort(pbase, (total_elems - 1) * size, size, cmp);
	}
}

/*
 * An alternative to qsort(), with an identical interface.
 * Written by Mike Haertel, September 1988.
 */

static void
msort_with_tmp(void *b, size_t n, size_t s, xsort_cmp_t cmp, char *t)
{
	char *tmp;
	char *b1, *b2;
	size_t n1, n2;

	if (n <= 1)
		return;

	n1 = n / 2;
	n2 = n - n1;
	b1 = b;
	b2 = ptr_add_offset(b, n1 * s);

	msort_with_tmp(b1, n1, s, cmp, t);
	msort_with_tmp(b2, n2, s, cmp, t);

	tmp = t;

	if (s == OPSIZ && (b1 - (char *) 0) % OPSIZ == 0) {
		op_t *otmp = (op_t *) tmp;
		op_t *ob1 = (op_t *) b1;
		op_t *ob2 = (op_t *) b2;

		/* We are operating on aligned words.  Use direct word stores. */

		while (n1 > 0 && n2 > 0) {
			if ((*cmp)(ob1, ob2) <= 0) {
				--n1;
				*otmp++ = *ob1++;
			} else {
				--n2;
				*otmp++ = *ob2++;
			}
		}

		tmp = (char *) otmp;
		b1 = (char *) ob1;
		b2 = (char *) ob2;
	} else {
		while (n1 > 0 && n2 > 0) {
			if ((*cmp) (b1, b2) <= 0) {
				tmp = mempcpy(tmp, b1, s);
				b1 += s;
				--n1;
			} else {
				tmp = mempcpy(tmp, b2, s);
				b2 += s;
				--n2;
			}
		}
	}

	if (n1 > 0)
		memcpy(tmp, b1, n1 * s);

	memcpy(b, t, (n - n2) * s);
}

/**
 * Sort array with ``n'' elements of size ``s''.  The base ``b'' points to
 * the start of the array.
 *
 * This routine allocates memory on the stack or through the VMM layer and
 * prefers to use mergesort, reserving quicksort to cases where there would
 * be too much memory required for the mergesort.
 *
 * The contents are sorted in ascending order, as defined by the comparison
 * function ``cmp''.
 */
void
xsort(void *b, size_t n, size_t s, xsort_cmp_t cmp)
{
	const size_t size = size_saturate_mult(n, s);

	g_assert(b != NULL);
	g_assert(cmp != NULL);
	g_assert(size_is_non_negative(n));
	g_assert(size_is_positive(s));


	if (size < 1024) {
		/* The temporary array is small, so put it on the stack */
		void *buf = alloca(size);

		msort_with_tmp(b, n, s, cmp, buf);
	} else {
		static uint64 memsize;

		/*
		 * We should avoid allocating too much memory since this might
		 * have to be backed up by swap space.
		 */

		if G_UNLIKELY(0 == memsize) {
			memsize = getphysmemsize();
			if (0 == memsize)
				memsize = (uint64) -1;		/* Assume plenty! */
		}

		/* If the memory requirements are too high don't allocate memory */
		if ((uint64) size > memsize / 4) {
			quicksort(b, n, s, cmp);
		} else {
			char *tmp;

			/* It's somewhat large, so alloc it through VMM */

			tmp = vmm_alloc(size);
			msort_with_tmp(b, n, s, cmp, tmp);
			vmm_free(tmp, size);
		}
	}
}

/**
 * Sort array in-place (no memory allocated) with ``n'' elements of size ``s''.
 * The base ``b'' points to the start of the array.
 *
 * The contents are sorted in ascending order, as defined by the comparison
 * function ``cmp''.
 */
void
xqsort(void *b, size_t n, size_t s, xsort_cmp_t cmp)
{
	quicksort(b, n, s, cmp);
}

/* vi: set ts=4 sw=4 cindent: */
