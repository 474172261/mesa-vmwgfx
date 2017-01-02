/*
 * Copyright (C) 2012-2014 Canonical Ltd (Maarten Lankhorst)
 *
 * Based on bo.c which bears the following copyright notice,
 * but is dual licensed:
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 */

#ifndef VMWGFX_STANDALONE

#include <linux/reservation.h>
#include <linux/export.h>

#else

#include "core/reservation.h"
#undef EXPORT_SYMBOL
#define EXPORT_SYMBOL(_dummy)
#undef EXPORT_SYMBOL_GPL
#define EXPORT_SYMBOL_GPL(_dummy)

#endif

/**
 * DOC: Reservation Object Overview
 *
 * The reservation object provides a mechanism to manage shared and
 * exclusive fences associated with a buffer.  A reservation object
 * can have attached one exclusive fence (normally associated with
 * write operations) or N shared fences (read operations).  The RCU
 * mechanism is used to protect read access to fences from locked
 * write-side updates.
 */

DEFINE_WW_CLASS(reservation_ww_class);
EXPORT_SYMBOL(reservation_ww_class);

struct lock_class_key reservation_seqcount_class;
EXPORT_SYMBOL(reservation_seqcount_class);

const char reservation_seqcount_string[] = "reservation_seqcount";
EXPORT_SYMBOL(reservation_seqcount_string);

/**
 * reservation_object_reserve_shared - Reserve space to add a shared
 * fence to a reservation_object.
 * @obj: reservation object
 *
 * Should be called before reservation_object_add_shared_fence().  Must
 * be called with obj->lock held.
 *
 * RETURNS
 * Zero for success, or -errno
 */
int reservation_object_reserve_shared(struct reservation_object *obj)
{
	struct reservation_object_list *fobj, *old;
	u32 max;

	old = reservation_object_get_list(obj);

	if (old && old->shared_max) {
		if (old->shared_count < old->shared_max) {
			/* perform an in-place update */
			kfree(obj->staged);
			obj->staged = NULL;
			return 0;
		} else
			max = old->shared_max * 2;
	} else
		max = 4;

	/*
	 * resize obj->staged or allocate if it doesn't exist,
	 * noop if already correct size
	 */
	fobj = krealloc(obj->staged, offsetof(typeof(*fobj), shared[max]),
			GFP_KERNEL);
	if (!fobj)
		return -ENOMEM;

	obj->staged = fobj;
	fobj->shared_max = max;
	return 0;
}
EXPORT_SYMBOL(reservation_object_reserve_shared);

static void
reservation_object_add_shared_inplace(struct reservation_object *obj,
				      struct reservation_object_list *fobj,
				      struct dma_fence *fence)
{
	u32 i;

	dma_fence_get(fence);

	preempt_disable();
	write_seqcount_begin(&obj->seq);

	for (i = 0; i < fobj->shared_count; ++i) {
		struct dma_fence *old_fence;

		old_fence = rcu_dereference_protected(fobj->shared[i],
						reservation_object_held(obj));

		if (old_fence->context == fence->context) {
			/* memory barrier is added by write_seqcount_begin */
			RCU_INIT_POINTER(fobj->shared[i], fence);
			write_seqcount_end(&obj->seq);
			preempt_enable();

			dma_fence_put(old_fence);
			return;
		}
	}

	/*
	 * memory barrier is added by write_seqcount_begin,
	 * fobj->shared_count is protected by this lock too
	 */
	RCU_INIT_POINTER(fobj->shared[fobj->shared_count], fence);
	fobj->shared_count++;

	write_seqcount_end(&obj->seq);
	preempt_enable();
}

static void
reservation_object_add_shared_replace(struct reservation_object *obj,
				      struct reservation_object_list *old,
				      struct reservation_object_list *fobj,
				      struct dma_fence *fence)
{
	unsigned i;
	struct dma_fence *old_fence = NULL;

	dma_fence_get(fence);

	if (!old) {
		RCU_INIT_POINTER(fobj->shared[0], fence);
		fobj->shared_count = 1;
		goto done;
	}

	/*
	 * no need to bump fence refcounts, rcu_read access
	 * requires the use of kref_get_unless_zero, and the
	 * references from the old struct are carried over to
	 * the new.
	 */
	fobj->shared_count = old->shared_count;

	for (i = 0; i < old->shared_count; ++i) {
		struct dma_fence *check;

		check = rcu_dereference_protected(old->shared[i],
						reservation_object_held(obj));

		if (!old_fence && check->context == fence->context) {
			old_fence = check;
			RCU_INIT_POINTER(fobj->shared[i], fence);
		} else
			RCU_INIT_POINTER(fobj->shared[i], check);
	}
	if (!old_fence) {
		RCU_INIT_POINTER(fobj->shared[fobj->shared_count], fence);
		fobj->shared_count++;
	}

done:
	preempt_disable();
	write_seqcount_begin(&obj->seq);
	/*
	 * RCU_INIT_POINTER can be used here,
	 * seqcount provides the necessary barriers
	 */
	RCU_INIT_POINTER(obj->fence, fobj);
	write_seqcount_end(&obj->seq);
	preempt_enable();

	if (old)
		kfree_rcu(old, rcu);

	if (old_fence)
		dma_fence_put(old_fence);
}

/**
 * reservation_object_add_shared_fence - Add a fence to a shared slot
 * @obj: the reservation object
 * @fence: the shared fence to add
 *
 * Add a fence to a shared slot, obj->lock must be held, and
 * reservation_object_reserve_shared() has been called.
 */
void reservation_object_add_shared_fence(struct reservation_object *obj,
					 struct dma_fence *fence)
{
	struct reservation_object_list *old, *fobj = obj->staged;

	old = reservation_object_get_list(obj);
	obj->staged = NULL;

	if (!fobj) {
		BUG_ON(old->shared_count >= old->shared_max);
		reservation_object_add_shared_inplace(obj, old, fence);
	} else
		reservation_object_add_shared_replace(obj, old, fobj, fence);
}
EXPORT_SYMBOL(reservation_object_add_shared_fence);

/**
 * reservation_object_add_excl_fence - Add an exclusive fence.
 * @obj: the reservation object
 * @fence: the shared fence to add
 *
 * Add a fence to the exclusive slot.  The obj->lock must be held.
 */
void reservation_object_add_excl_fence(struct reservation_object *obj,
				       struct dma_fence *fence)
{
	struct dma_fence *old_fence = reservation_object_get_excl(obj);
	struct reservation_object_list *old;
	u32 i = 0;

	old = reservation_object_get_list(obj);
	if (old)
		i = old->shared_count;

	if (fence)
		dma_fence_get(fence);

	preempt_disable();
	write_seqcount_begin(&obj->seq);
	/* write_seqcount_begin provides the necessary memory barrier */
	RCU_INIT_POINTER(obj->fence_excl, fence);
	if (old)
		old->shared_count = 0;
	write_seqcount_end(&obj->seq);
	preempt_enable();

	/* inplace update, no shared fences */
	while (i--)
		dma_fence_put(rcu_dereference_protected(old->shared[i],
						reservation_object_held(obj)));

	if (old_fence)
		dma_fence_put(old_fence);
}
EXPORT_SYMBOL(reservation_object_add_excl_fence);

/**
 * reservation_object_get_fences_rcu - Get an object's shared and exclusive
 * fences without update side lock held
 * @obj: the reservation object
 * @pfence_excl: the returned exclusive fence (or NULL)
 * @pshared_count: the number of shared fences returned
 * @pshared: the array of shared fence ptrs returned (array is krealloc'd to
 * the required size, and must be freed by caller)
 *
 * RETURNS
 * Zero or -errno
 */
int reservation_object_get_fences_rcu(struct reservation_object *obj,
				      struct dma_fence **pfence_excl,
				      unsigned *pshared_count,
				      struct dma_fence ***pshared)
{
	struct dma_fence **shared = NULL;
	struct dma_fence *fence_excl;
	unsigned int shared_count;
	int ret = 1;

	do {
		struct reservation_object_list *fobj;
		unsigned seq;
		unsigned int i;

		shared_count = i = 0;

		rcu_read_lock();
		seq = read_seqcount_begin(&obj->seq);

		fence_excl = rcu_dereference(obj->fence_excl);
		if (fence_excl && !dma_fence_get_rcu(fence_excl))
			goto unlock;

		fobj = rcu_dereference(obj->fence);
		if (fobj) {
			struct dma_fence **nshared;
			size_t sz = sizeof(*shared) * fobj->shared_max;

			nshared = krealloc(shared, sz,
					   GFP_NOWAIT | __GFP_NOWARN);
			if (!nshared) {
				rcu_read_unlock();
				nshared = krealloc(shared, sz, GFP_KERNEL);
				if (nshared) {
					shared = nshared;
					continue;
				}

				ret = -ENOMEM;
				break;
			}
			shared = nshared;
			shared_count = fobj->shared_count;

			for (i = 0; i < shared_count; ++i) {
				shared[i] = rcu_dereference(fobj->shared[i]);
				if (!dma_fence_get_rcu(shared[i]))
					break;
			}
		}

		if (i != shared_count || read_seqcount_retry(&obj->seq, seq)) {
			while (i--)
				dma_fence_put(shared[i]);
			dma_fence_put(fence_excl);
			goto unlock;
		}

		ret = 0;
unlock:
		rcu_read_unlock();
	} while (ret);

	if (!shared_count) {
		kfree(shared);
		shared = NULL;
	}

	*pshared_count = shared_count;
	*pshared = shared;
	*pfence_excl = fence_excl;

	return ret;
}
EXPORT_SYMBOL_GPL(reservation_object_get_fences_rcu);

/**
 * reservation_object_wait_timeout_rcu - Wait on reservation's objects
 * shared and/or exclusive fences.
 * @obj: the reservation object
 * @wait_all: if true, wait on all fences, else wait on just exclusive fence
 * @intr: if true, do interruptible wait
 * @timeout: timeout value in jiffies or zero to return immediately
 *
 * RETURNS
 * Returns -ERESTARTSYS if interrupted, 0 if the wait timed out, or
 * greater than zer on success.
 */
long reservation_object_wait_timeout_rcu(struct reservation_object *obj,
					 bool wait_all, bool intr,
					 unsigned long timeout)
{
	struct dma_fence *fence;
	unsigned seq, shared_count, i = 0;
	long ret = timeout ? timeout : 1;

retry:
	fence = NULL;
	shared_count = 0;
	seq = read_seqcount_begin(&obj->seq);
	rcu_read_lock();

	if (wait_all) {
		struct reservation_object_list *fobj =
						rcu_dereference(obj->fence);

		if (fobj)
			shared_count = fobj->shared_count;

		for (i = 0; i < shared_count; ++i) {
			struct dma_fence *lfence = rcu_dereference(fobj->shared[i]);

			if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
				     &lfence->flags))
				continue;

			if (!dma_fence_get_rcu(lfence))
				goto unlock_retry;

			if (dma_fence_is_signaled(lfence)) {
				dma_fence_put(lfence);
				continue;
			}

			fence = lfence;
			break;
		}
	}

	if (!shared_count) {
		struct dma_fence *fence_excl = rcu_dereference(obj->fence_excl);

		if (fence_excl &&
		    !test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
			      &fence_excl->flags)) {
			if (!dma_fence_get_rcu(fence_excl))
				goto unlock_retry;

			if (dma_fence_is_signaled(fence_excl))
				dma_fence_put(fence_excl);
			else
				fence = fence_excl;
		}
	}

	rcu_read_unlock();
	if (fence) {
		if (read_seqcount_retry(&obj->seq, seq)) {
			dma_fence_put(fence);
			goto retry;
		}

		ret = dma_fence_wait_timeout(fence, intr, ret);
		dma_fence_put(fence);
		if (ret > 0 && wait_all && (i + 1 < shared_count))
			goto retry;
	}
	return ret;

unlock_retry:
	rcu_read_unlock();
	goto retry;
}
EXPORT_SYMBOL_GPL(reservation_object_wait_timeout_rcu);


static inline int
reservation_object_test_signaled_single(struct dma_fence *passed_fence)
{
	struct dma_fence *fence, *lfence = passed_fence;
	int ret = 1;

	if (!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &lfence->flags)) {
		fence = dma_fence_get_rcu(lfence);
		if (!fence)
			return -1;

		ret = !!dma_fence_is_signaled(fence);
		dma_fence_put(fence);
	}
	return ret;
}

/**
 * reservation_object_test_signaled_rcu - Test if a reservation object's
 * fences have been signaled.
 * @obj: the reservation object
 * @test_all: if true, test all fences, otherwise only test the exclusive
 * fence
 *
 * RETURNS
 * true if all fences signaled, else false
 */
bool reservation_object_test_signaled_rcu(struct reservation_object *obj,
					  bool test_all)
{
	unsigned seq, shared_count;
	int ret;

	rcu_read_lock();
retry:
	ret = true;
	shared_count = 0;
	seq = read_seqcount_begin(&obj->seq);

	if (test_all) {
		unsigned i;

		struct reservation_object_list *fobj =
						rcu_dereference(obj->fence);

		if (fobj)
			shared_count = fobj->shared_count;

		for (i = 0; i < shared_count; ++i) {
			struct dma_fence *fence = rcu_dereference(fobj->shared[i]);

			ret = reservation_object_test_signaled_single(fence);
			if (ret < 0)
				goto retry;
			else if (!ret)
				break;
		}

		if (read_seqcount_retry(&obj->seq, seq))
			goto retry;
	}

	if (!shared_count) {
		struct dma_fence *fence_excl = rcu_dereference(obj->fence_excl);

		if (fence_excl) {
			ret = reservation_object_test_signaled_single(
								fence_excl);
			if (ret < 0)
				goto retry;

			if (read_seqcount_retry(&obj->seq, seq))
				goto retry;
		}
	}

	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(reservation_object_test_signaled_rcu);
