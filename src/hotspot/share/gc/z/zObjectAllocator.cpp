/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "gc/z/zFuture.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zHeuristics.hpp"
#include "gc/z/zObjectAllocator.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageTable.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zValue.inline.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"

static const ZStatCounter ZCounterUndoObjectAllocationSucceeded("Memory", "Undo Object Allocation Succeeded", ZStatUnitOpsPerSecond);
static const ZStatCounter ZCounterUndoObjectAllocationFailed("Memory", "Undo Object Allocation Failed", ZStatUnitOpsPerSecond);

ZMediumPageSynchronizer::ZMediumPageSynchronizer()
  : _lock(),
    _is_allocating(false) {}

bool ZMediumPageSynchronizer::claim_or_wait() {
  ZLocker<ZConditionLock> locker(&_lock);

  if (!Atomic::load(&_is_allocating)) {
    Atomic::store(&_is_allocating, true);
    return true;
  }

  // Allocation handled by another thread, wait for it
  _lock.wait();
  return false;
}

void ZMediumPageSynchronizer::notify_waiters() {
  ZLocker<ZConditionLock> locker(&_lock);
  Atomic::store(&_is_allocating, false);
  _lock.notify_all();
}

class ZWaiter {
  friend class ZList<ZWaiter>;
private:
  ZFuture<bool>      _future;
  ZListNode<ZWaiter> _node;

public:
 ZWaiter()
  : _future(),
    _node() {}

  void notify() {
    _future.set(true);
  }

  void wait() {
    _future.get();
  }
};


// Eden page allocations might stall so we need a waiting mechanism that
// can handle safepointing. All waiters will put a ZFuture on the _waiters
// list and be notified when the allocation is done. Very similar to how
// the actual allocation stalls are handled.
class ZMediumEdenPageSynchronizer : public ZMediumPageSynchronizer {
private:
  ZList<ZWaiter> _waiters;

public:
  ZMediumEdenPageSynchronizer()
    : ZMediumPageSynchronizer(),
      _waiters() {}

  bool claim_or_wait() {
    ZWaiter waiter;

    {
      ZLocker<ZConditionLock> locker(&_lock);

      if (!Atomic::load(&_is_allocating)) {
        // This thread should allocate
        Atomic::store(&_is_allocating, true);
        return true;
      }

      // Put this future on the waiters list
      _waiters.insert_last(&waiter);
    }

    waiter.wait();
    return false;
  }

  void notify_waiters() {
    ZLocker<ZConditionLock> locker(&_lock);
    Atomic::store(&_is_allocating, false);

    ZListRemoveIterator<ZWaiter> iter(&_waiters);
    for (ZWaiter* waiter; iter.next(&waiter);) {
      waiter->notify();
    }
  }
};

ZObjectAllocator::ZObjectAllocator(ZPageAge age)
  : _age(age),
    _use_per_cpu_shared_small_pages(ZHeuristics::use_per_cpu_shared_small_pages()),
    _used(0),
    _undone(0),
    _shared_medium_page(nullptr),
    _shared_small_page(nullptr),
    _medium_page_synchronizer(_age == ZPageAge::eden ?
                              new ZMediumEdenPageSynchronizer() :
                              new ZMediumPageSynchronizer()) {}

ZPage** ZObjectAllocator::shared_small_page_addr() {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

ZPage* const* ZObjectAllocator::shared_small_page_addr() const {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

ZPage* ZObjectAllocator::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags) {
  ZPage* const page = ZHeap::heap()->alloc_page(type, size, flags, _age);
  if (page != nullptr) {
    // Increment used bytes
    Atomic::add(_used.addr(), size);
  }

  return page;
}

ZPage* ZObjectAllocator::alloc_page_for_relocation(ZPageType type, size_t size, ZAllocationFlags flags) {
  return ZHeap::heap()->alloc_page(type, size, flags, _age);
}

void ZObjectAllocator::undo_alloc_page(ZPage* page) {
  // Increment undone bytes
  Atomic::add(_undone.addr(), page->size());

  ZHeap::heap()->undo_alloc_page(page);
}

zaddress ZObjectAllocator::alloc_object_in_shared_page(ZPage** shared_page,
                                                       ZPageType page_type,
                                                       size_t page_size,
                                                       size_t size,
                                                       ZAllocationFlags flags) {
  zaddress addr = zaddress::null;
  ZPage* page = Atomic::load_acquire(shared_page);

  if (page != nullptr) {
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    // Allocate new page
    ZPage* const new_page = alloc_page(page_type, page_size, flags);
    if (new_page != nullptr) {
      // Allocate object before installing the new page
      addr = new_page->alloc_object(size);

    retry:
      // Install new page
      ZPage* const prev_page = Atomic::cmpxchg(shared_page, page, new_page);
      if (prev_page != page) {
        if (prev_page == nullptr) {
          // Previous page was retired, retry installing the new page
          page = prev_page;
          goto retry;
        }

        // Another page already installed, try allocation there first
        const zaddress prev_addr = prev_page->alloc_object_atomic(size);
        if (is_null(prev_addr)) {
          // Allocation failed, retry installing the new page
          page = prev_page;
          goto retry;
        }

        // Allocation succeeded in already installed page
        addr = prev_addr;

        // Undo new page allocation
        undo_alloc_page(new_page);
      }
    }
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_object_in_medium_page(ZPageType page_type,
                                                       size_t page_size,
                                                       size_t size,
                                                       ZAllocationFlags flags) {
  ZPage** shared_page = _shared_medium_page.addr();
retry:
  zaddress addr = zaddress::null;
  ZPage* page = Atomic::load_acquire(shared_page);

  if (page != nullptr) {
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    // New medium page needed
    if (_medium_page_synchronizer->claim_or_wait()) {
      // Make sure a new page has not already been installed
      if (page != Atomic::load_acquire(shared_page)) {
        // New page available, reset state and notify
        _medium_page_synchronizer->notify_waiters();
        goto retry;
      }

      // Allocate new page
      ZPage* const new_page = alloc_page(page_type, page_size, flags);
      if (new_page != nullptr) {
        // Allocate object before installing the new page
        addr = new_page->alloc_object(size);
      }

      // Install new page
      Atomic::release_store(shared_page, new_page);
      _medium_page_synchronizer->notify_waiters();
    } else {
      // Someone else allocated and installed a new page
      goto retry;
    }
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_large_object(size_t size, ZAllocationFlags flags) {
  zaddress addr = zaddress::null;

  // Allocate new large page
  const size_t page_size = align_up(size, ZGranuleSize);
  ZPage* const page = alloc_page(ZPageType::large, page_size, flags);
  if (page != nullptr) {
    // Allocate the object
    addr = page->alloc_object(size);
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_medium_object(size_t size, ZAllocationFlags flags) {
  return alloc_object_in_medium_page(ZPageType::medium, ZPageSizeMedium, size, flags);
}

zaddress ZObjectAllocator::alloc_small_object(size_t size, ZAllocationFlags flags) {
  return alloc_object_in_shared_page(shared_small_page_addr(), ZPageType::small, ZPageSizeSmall, size, flags);
}

zaddress ZObjectAllocator::alloc_object(size_t size, ZAllocationFlags flags) {
  if (size <= ZObjectSizeLimitSmall) {
    // Small
    return alloc_small_object(size, flags);
  } else if (size <= ZObjectSizeLimitMedium) {
    // Medium
    return alloc_medium_object(size, flags);
  } else {
    // Large
    return alloc_large_object(size, flags);
  }
}

zaddress ZObjectAllocator::alloc_object(size_t size) {
  const ZAllocationFlags flags;
  return alloc_object(size, flags);
}

zaddress ZObjectAllocator::alloc_object_for_relocation(size_t size) {
  ZAllocationFlags flags;
  flags.set_non_blocking();

  return alloc_object(size, flags);
}

void ZObjectAllocator::undo_alloc_object_for_relocation(zaddress addr, size_t size) {
  ZPage* const page = ZHeap::heap()->page(addr);

  if (page->is_large()) {
    undo_alloc_page(page);
    ZStatInc(ZCounterUndoObjectAllocationSucceeded);
  } else {
    if (page->undo_alloc_object_atomic(addr, size)) {
      ZStatInc(ZCounterUndoObjectAllocationSucceeded);
    } else {
      ZStatInc(ZCounterUndoObjectAllocationFailed);
    }
  }
}

ZPageAge ZObjectAllocator::age() const {
  return _age;
}

size_t ZObjectAllocator::used() const {
  size_t total_used = 0;
  size_t total_undone = 0;

  ZPerCPUConstIterator<size_t> iter_used(&_used);
  for (const size_t* cpu_used; iter_used.next(&cpu_used);) {
    total_used += *cpu_used;
  }

  ZPerCPUConstIterator<size_t> iter_undone(&_undone);
  for (const size_t* cpu_undone; iter_undone.next(&cpu_undone);) {
    total_undone += *cpu_undone;
  }

  return total_used - total_undone;
}

size_t ZObjectAllocator::remaining() const {
  assert(Thread::current()->is_Java_thread(), "Should be a Java thread");

  const ZPage* const page = Atomic::load_acquire(shared_small_page_addr());
  if (page != nullptr) {
    return page->remaining();
  }

  return 0;
}

void ZObjectAllocator::retire_pages() {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");

  // Reset used and undone bytes
  _used.set_all(0);
  _undone.set_all(0);

  // Reset allocation pages
  _shared_medium_page.set(nullptr);
  _shared_small_page.set_all(nullptr);
}
