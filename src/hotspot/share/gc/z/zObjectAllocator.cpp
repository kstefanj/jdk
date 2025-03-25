/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/z/zGlobals.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zHeuristics.hpp"
#include "gc/z/zLock.inline.hpp"
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

ZObjectAllocator::ZObjectAllocator(ZPageAge age)
  : _age(age),
    _use_per_cpu_shared_small_pages(ZHeuristics::use_per_cpu_shared_small_pages()),
    _shared_small_page(nullptr),
    _shared_medium_page(nullptr),
    _medium_page_alloc_lock() {}

ZPage** ZObjectAllocator::shared_small_page_addr() {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

ZPage* const* ZObjectAllocator::shared_small_page_addr() const {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

ZPage* ZObjectAllocator::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags) {
  return ZHeap::heap()->alloc_page(type, size, flags, _age);
}

ZPage* ZObjectAllocator::alloc_page_for_relocation(ZPageType type, size_t size, ZAllocationFlags flags) {
  return ZHeap::heap()->alloc_page(type, size, flags, _age);
}

void ZObjectAllocator::undo_alloc_page(ZPage* page) {
  ZHeap::heap()->undo_alloc_page(page);
}

bool ZObjectAllocator::page_is_active(const ZPage* page) const {
  if (page == nullptr) {
    // No page installed
    return false;
  }

  if (!page->is_allocating()) {
    // Installed page is not allocating and needs to be retired
    return false;
  }

  return true;
}

zaddress ZObjectAllocator::alloc_object_in_shared_page(ZPage** shared_page,
                                                       ZPageType page_type,
                                                       size_t page_size,
                                                       size_t size,
                                                       ZAllocationFlags flags) {
  zaddress addr = zaddress::null;
  ZPage* page = Atomic::load_acquire(shared_page);

  // To avoid having to explicitly retire pages in the safepoint we
  // make sure to only allocate from active pages.
  if (page_is_active(page)) {
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    // Allocate new page
    ZPage* const new_page = alloc_page(page_type, page_size, flags);
    if (new_page != nullptr) {
      assert(new_page->is_allocating(), "Inv");
      // Allocate object before installing the new page
      addr = new_page->alloc_object(size);

    retry:
      // Install new page
      ZPage* const prev_page = Atomic::cmpxchg(shared_page, page, new_page);
      if (prev_page != page) {
        if (prev_page == nullptr || !prev_page->is_allocating()) {
          // Previous page was retired, retry installing the new page
          page = prev_page;
          goto retry;
        }

        // Another page already installed, try allocation there first
        assert(prev_page->is_allocating(), "Inv");
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

zaddress ZObjectAllocator::alloc_object_in_medium_page(size_t size,
                                                       ZAllocationFlags flags) {
  zaddress addr = zaddress::null;
  ZPage** shared_medium_page = _shared_medium_page.addr();
  ZPage* page = Atomic::load_acquire(shared_medium_page);

  // To avoid having to explicitly retire pages in the safepoint we
  // make sure to only allocate from active pages.
  if (page_is_active(page)) {
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    // When a new medium page is required, we synchronize the allocation of the
    // new page using a lock. This is to avoid having multiple threads allocate
    // medium pages when we know only one of them will succeed in installing
    // the page at this layer.
    ZLocker<ZLock> locker(&_medium_page_alloc_lock);

    // When holding the lock we can't allow the page allocator to stall,
    // which in the common case it won't. The page allocation is thus done
    // in a non-blocking fashion and only if this fails we below (while not
    // holding the lock) do the blocking page allocation.
    ZAllocationFlags non_blocking_flags = flags;
    non_blocking_flags.set_non_blocking();

    addr = alloc_object_in_shared_page(shared_medium_page, ZPageType::medium, ZPageSizeMedium, size, non_blocking_flags);
  }

  if (is_null(addr) && !flags.non_blocking()) {
    // The above allocation attempts failed and this allocation should stall
    // until memory is available. Redo the allocation with blocking enabled.
    addr = alloc_object_in_shared_page(shared_medium_page, ZPageType::medium, ZPageSizeMedium, size, flags);
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
  return alloc_object_in_medium_page(size, flags);
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

size_t ZObjectAllocator::remaining() const {
  assert(Thread::current()->is_Java_thread(), "Should be a Java thread");

  const ZPage* const page = Atomic::load_acquire(shared_small_page_addr());
  // Make sure to only check active pages
  if (page_is_active(page)) {
    return page->remaining();
  }

  return 0;
}

static void retire_at(ZPage** page_addr) {
  ZPage* page = Atomic::load_acquire(page_addr);
  if (page != nullptr && !page->is_allocating()) {
    // Try to retire
    ZPage* const prev_page = Atomic::cmpxchg(page_addr, page, (ZPage*) nullptr);
    assert(prev_page == page || prev_page->is_allocating(), "Either we retired or someone else should have installed a valid page");
  }
}

void ZObjectAllocator::concurrent_retire_pages() {
  // Reset allocation pages
  ZPerCPUIterator<ZPage*> iter(&_shared_small_page);
  for (ZPage** page_addr; iter.next(&page_addr);) {
    retire_at(page_addr);
  }
  retire_at(_shared_medium_page.addr());
}