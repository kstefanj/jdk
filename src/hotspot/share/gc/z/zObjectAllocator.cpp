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

ZObjectAllocator::ZObjectAllocator(ZPageAge age)
  : _age(age),
    _use_per_cpu_shared_small_pages(ZHeuristics::use_per_cpu_shared_small_pages()),
    _used(0),
    _undone(0),
    _shared_medium_page(nullptr),
    _shared_small_page(nullptr),
    _serial(age) {}

ZPage** ZObjectAllocator::shared_small_page_addr() {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

ZPage* const* ZObjectAllocator::shared_small_page_addr() const {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

ZPage* ZObjectAllocator::alloc_page(ZAllocationRequest* request) {
  bool success = ZHeap::heap()->alloc_page(request, _age);
  if (success) {
    // Increment used bytes
    Atomic::add(_used.addr(), request->page_size);
  }

  return request->result;
}

ZPage* ZObjectAllocator::alloc_page_for_relocation(ZPageType type, size_t size, ZAllocationFlags flags) {
  ZAllocationRequest request(0, flags, &_serial);
  request.page_size = size;
  request.type = type;

  ZHeap::heap()->alloc_page(&request, _age);
  return request.result;
}

void ZObjectAllocator::undo_alloc_page(ZPage* page, size_t size) {
  // Increment undone bytes
  Atomic::add(_undone.addr(), page->size());

  ZHeap::heap()->undo_alloc_page(page, size);
}

zaddress ZObjectAllocator::alloc_object_in_shared_page(ZPage** shared_page,
                                                       ZAllocationRequest* request) {
  zaddress addr = zaddress::null;
  ZPage* page = Atomic::load_acquire(shared_page);

  if (page != nullptr) {
    addr = page->alloc_object_atomic(request->size);
  }

  if (is_null(addr)) {
    // Allocate new page
    ZPage* const new_page = alloc_page(request);
    if (new_page != nullptr) {
      // Allocate object before installing the new page
      addr = new_page->alloc_object(request->size);

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
        const zaddress prev_addr = prev_page->alloc_object_atomic(request->size);
        if (is_null(prev_addr)) {
          // Allocation failed, retry installing the new page
          page = prev_page;
          goto retry;
        }

        // Allocation succeeded in already installed page
        addr = prev_addr;

        // Undo new page allocation
        undo_alloc_page(new_page, request->size);
      }
    }
  }

  return addr;
}

void ZObjectAllocator::stall_page() {
  ZAllocationFlags flags;
  ZAllocationRequest stall(0, flags, nullptr);
  ZHeap::heap()->stall_page(&stall, _age);
}

ZMediumSerializer::ZMediumSerializer(ZPageAge age)
  : _lock(),
    _count(0),
    _ticket(1),
    _stalled(false),
    _age(age) {}

  // Should caller allocated page
bool ZMediumSerializer::claim(size_t *ticket) {
  ZLocker<ZConditionLock> locker(&_lock);

  // Add to the count, first one gets the claim.
  size_t old_count = Atomic::fetch_then_add(&_count, (size_t) 1);
  if (old_count == 0) {
    guarantee(!_stalled, "Thread claiming the current ticket should not see a stalled state");
    return true;
  }
  *ticket = Atomic::load(&_ticket);
  return false;
}

bool ZMediumSerializer::wait(size_t ticket) {
  ZLocker<ZConditionLock> locker(&_lock);
  if (Atomic::load(&_ticket) > ticket) {
    // Ticket already completeded
    return true;
  }

  // If current allocation is stalled, just stall this thread as well
  if (Atomic::load(&_stalled)) {
    return false;
  }

  // Wait for allocating thread to complete page allocation for this ticket
  _lock.wait();

  if (Atomic::load(&_ticket) > ticket)  {
    // Ticket completed, either done or allocation failed
    return true;
  } else {
    guarantee(_stalled, "should be stalled");
    return false;
  }
}

void ZMediumSerializer::complete_ticket() {
  Atomic::inc(&_ticket);
  Atomic::store(&_count, (size_t) 0);
  Atomic::store(&_stalled, false);
}

void ZMediumSerializer::install(ZPage** location, ZPage* page) {
  ZLocker<ZConditionLock> locker(&_lock);
  Atomic::store(location, page);
  complete_ticket();
  _lock.notify_all();
}

void ZMediumSerializer::abort() {
  ZLocker<ZConditionLock> locker(&_lock);
  complete_ticket();
  _lock.notify_all();
}

void ZMediumSerializer::notify_stalled() {
  ZLocker<ZConditionLock> locker(&_lock);
  Atomic::store(&_stalled, true);
  _lock.notify_all();
}

zaddress ZObjectAllocator::alloc_object_in_shared_medium_page(ZPage** shared_page,
                                                              ZAllocationRequest* request) {
restart:
  zaddress addr = zaddress::null;
  ZPage* page = Atomic::load_acquire(shared_page);
  size_t size = request->size;

  if (page != nullptr) {
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    size_t ticket = 0;
    if (_serial.claim(&ticket)) {
      // Claim successful, do allocation
      ZPage* new_page = alloc_page(request);
      if (new_page == nullptr) {
        // Allocation failed, notify waiters
        _serial.abort();
        return addr;
      }

      // Allocate object before installing the new page
      addr = new_page->alloc_object(size);

      // Install new page and notify the waiters
      _serial.install(shared_page, new_page);
    } else {
      // Wait for this ticket to complete
      bool completed = _serial.wait(ticket);
      if(!completed) {
        // Notified but allocation stalled, stall this request as well
        stall_page();
      }
      // Now there should be a new page ready
      goto restart;
    }
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_large_object(ZAllocationRequest* request) {
  zaddress addr = zaddress::null;

  // Allocate new large page
  request->page_size = align_up(request->size, ZGranuleSize);
  request->type = ZPageType::large;
  ZPage* const page = alloc_page(request);
  if (page != nullptr) {
    // Allocate the object
    addr = page->alloc_object(request->size);
  }

  return addr;
}

zaddress ZObjectAllocator::alloc_medium_object(ZAllocationRequest* request) {
  request->type = ZPageType::medium;
  request->page_size = ZPageSizeMedium;
  return alloc_object_in_shared_medium_page(_shared_medium_page.addr(), request);
}

zaddress ZObjectAllocator::alloc_small_object(ZAllocationRequest* request) {
  request->type = ZPageType::small;
  request->page_size = ZPageSizeSmall;
  return alloc_object_in_shared_page(shared_small_page_addr(), request);
}

zaddress ZObjectAllocator::alloc_object(size_t size, ZAllocationFlags flags) {
  ZAllocationRequest request(size, flags, &_serial);
  if (size <= ZObjectSizeLimitSmall) {
    // Small
    return alloc_small_object(&request);
  } else if (size <= ZObjectSizeLimitMedium) {
    // Medium
    return alloc_medium_object(&request);
  } else {
    // Large
    return alloc_large_object(&request);
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
    undo_alloc_page(page, size);
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
