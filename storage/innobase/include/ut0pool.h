/*****************************************************************************

Copyright (c) 2013, 2014, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file include/ut0pool.h
Object pool.

Created 2012-Feb-26 Sunny Bains
***********************************************************************/

#ifndef ut0pool_h
#define ut0pool_h

#include <vector>
#include <queue>
#include <functional>

#include "ut0new.h"

/** Allocate the memory for the object in blocks. We keep the objects sorted
on pointer so that they are closer together in case they have to be iterated
over in a list. */
template <typename Type, typename Factory, typename LockStrategy>
struct Pool {

	typedef Type value_type;

	// FIXME: Add an assertion to check alignment and offset is
	// as we expect it. Also, sizeof(void*) can be 8, can we impove on this.
	struct Element {
		Pool*		m_pool;
		value_type	m_type;
	};

	/** Constructor
	@param size size of the memory block */
	Pool(size_t size)
		:
		m_end(),
		m_start(),
		m_size(size),
		m_last()
	{
		ut_a(size >= sizeof(Element));

		m_lock_strategy.create();

		ut_a(m_start == 0);

		m_start = reinterpret_cast<Element*>(ut_zalloc_nokey(m_size));

		m_last = m_start;

		m_end = &m_start[m_size / sizeof(*m_start)];

		/* Note: Initialise only a small subset, even though we have
		allocated all the memory. This is required only because PFS
		(MTR) results change if we instantiate too many mutexes up
		front. */

		init(ut_min(size_t(16), size_t(m_end - m_start)));

		ut_ad(m_pqueue.size() <= size_t(m_last - m_start));
	}

	/** Destructor */
	~Pool()
	{
		m_lock_strategy.destroy();

		for (Element* elem = m_start; elem != m_last; ++elem) {

			ut_ad(elem->m_pool == this);
			/* Unpoison the memory for AddressSanitizer */
			MEM_UNDEFINED(&elem->m_type, sizeof elem->m_type);
			/* Declare the contents as initialized for Valgrind;
			we checked this in mem_free(). */
			UNIV_MEM_VALID(&elem->m_type, sizeof elem->m_type);
			Factory::destroy(&elem->m_type);
		}

		ut_free(m_start);
		m_end = m_last = m_start = 0;
		m_size = 0;
	}

	/** Get an object from the pool.
	@retrun a free instance or NULL if exhausted. */
	Type*	get()
	{
		Element*	elem;

		m_lock_strategy.enter();

		if (!m_pqueue.empty()) {

			elem = m_pqueue.top();
			m_pqueue.pop();

		} else if (m_last < m_end) {

			/* Initialise the remaining elements. */
			init(m_end - m_last);

			ut_ad(!m_pqueue.empty());

			elem = m_pqueue.top();
			m_pqueue.pop();
		} else {
			elem = NULL;
		}

#if defined HAVE_valgrind || defined __SANITIZE_ADDRESS__
		if (elem) {
			/* Unpoison the memory for AddressSanitizer */
			MEM_UNDEFINED(&elem->m_type, sizeof elem->m_type);
			/* Declare the memory initialized for Valgrind.
			The trx_t that are released to the pool are
			actually initialized; we checked that by
			UNIV_MEM_ASSERT_RW() in mem_free() below. */
			UNIV_MEM_VALID(&elem->m_type, sizeof elem->m_type);
		}
#endif

		m_lock_strategy.exit();
		return elem ? &elem->m_type : NULL;
	}

	/** Add the object to the pool.
	@param ptr object to free */
	static void mem_free(value_type* ptr)
	{
		Element*	elem;
		byte*		p = reinterpret_cast<byte*>(ptr + 1);

		elem = reinterpret_cast<Element*>(p - sizeof(*elem));
		UNIV_MEM_ASSERT_RW(&elem->m_type, sizeof elem->m_type);

		elem->m_pool->m_lock_strategy.enter();

		elem->m_pool->putl(elem);
		MEM_NOACCESS(&elem->m_type, sizeof elem->m_type);

		elem->m_pool->m_lock_strategy.exit();
	}

protected:
	// Disable copying
	Pool(const Pool&);
	Pool& operator=(const Pool&);

private:

	/* We only need to compare on pointer address. */
	typedef std::priority_queue<
		Element*,
		std::vector<Element*, ut_allocator<Element*> >,
		std::greater<Element*> >	pqueue_t;

	/** Release the object to the free pool
	@param elem element to free */
	void putl(Element* elem)
	{
		ut_ad(elem >= m_start && elem < m_last);

		ut_ad(Factory::debug(&elem->m_type));

		m_pqueue.push(elem);
	}

	/** Initialise the elements.
	@param n_elems Number of elements to initialise */
	void init(size_t n_elems)
	{
		ut_ad(size_t(m_end - m_last) >= n_elems);

		for (size_t i = 0; i < n_elems; ++i, ++m_last) {

			m_last->m_pool = this;
			Factory::init(&m_last->m_type);
			m_pqueue.push(m_last);
		}

		ut_ad(m_last <= m_end);
	}

private:
	/** Pointer to the last element */
	Element*		m_end;

	/** Pointer to the first element */
	Element*		m_start;

	/** Size of the block in bytes */
	size_t			m_size;

	/** Upper limit of used space */
	Element*		m_last;

	/** Priority queue ordered on the pointer addresse. */
	pqueue_t		m_pqueue;

	/** Lock strategy to use */
	LockStrategy		m_lock_strategy;
};

template <typename Pool, typename LockStrategy>
struct PoolManager {

	typedef Pool PoolType;
	typedef typename PoolType::value_type value_type;

	PoolManager(size_t size)
		:
		m_size(size)
	{
		create();
	}

	~PoolManager()
	{
		destroy();

		ut_a(m_pools.empty());
	}

	/** Get an element from one of the pools.
	@return instance or NULL if pool is empty. */
	value_type* get()
	{
		size_t		index = 0;
		size_t		delay = 1;
		value_type*	ptr = NULL;

		do {
			m_lock_strategy.enter();

			ut_ad(!m_pools.empty());

			size_t	n_pools = m_pools.size();

			PoolType*	pool = m_pools[index % n_pools];

			m_lock_strategy.exit();

			ptr = pool->get();

			if (ptr == 0 && (index / n_pools) > 2) {

				if (!add_pool(n_pools)) {

					ib::error() << "Failed to allocate"
						" memory for a pool of size "
						<< m_size << " bytes. Will"
						" wait for " << delay
						<< " seconds for a thread to"
						" free a resource";

					/* There is nothing much we can do
					except crash and burn, however lets
					be a little optimistic and wait for
					a resource to be freed. */
					os_thread_sleep(delay * 1000000);

					if (delay < 32) {
						delay <<= 1;
					}

				} else {
					delay = 1;
				}
			}

			++index;

		} while (ptr == NULL);

		return(ptr);
	}

	static void mem_free(value_type* ptr)
	{
		PoolType::mem_free(ptr);
	}

private:
	/** Add a new pool
	@param n_pools Number of pools that existed when the add pool was
			called.
	@return true on success */
	bool add_pool(size_t n_pools)
	{
		bool	added = false;

		m_lock_strategy.enter();

		if (n_pools < m_pools.size()) {
			/* Some other thread already added a pool. */
			added = true;
		} else {
			PoolType*	pool;

			ut_ad(n_pools == m_pools.size());

			pool = UT_NEW_NOKEY(PoolType(m_size));

			if (pool != NULL) {

				ut_ad(n_pools <= m_pools.size());

				m_pools.push_back(pool);

				ib::info() << "Number of pools: "
					<< m_pools.size();

				added = true;
			}
		}

		ut_ad(n_pools < m_pools.size() || !added);

		m_lock_strategy.exit();

		return(added);
	}

	/** Create the pool manager. */
	void create()
	{
		ut_a(m_size > sizeof(value_type));
		m_lock_strategy.create();

		add_pool(0);
	}

	/** Release the resources. */
	void destroy()
	{
		typename Pools::iterator it;
		typename Pools::iterator end = m_pools.end();

		for (it = m_pools.begin(); it != end; ++it) {
			PoolType*	pool = *it;

			UT_DELETE(pool);
		}

		m_pools.clear();

		m_lock_strategy.destroy();
	}
private:
	// Disable copying
	PoolManager(const PoolManager&);
	PoolManager& operator=(const PoolManager&);

	typedef std::vector<PoolType*, ut_allocator<PoolType*> >	Pools;

	/** Size of each block */
	size_t		m_size;

	/** Pools managed this manager */
	Pools		m_pools;

	/** Lock strategy to use */
	LockStrategy		m_lock_strategy;
};

#endif /* ut0pool_h */
