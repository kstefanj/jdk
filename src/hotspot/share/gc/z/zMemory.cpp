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
#include "gc/z/zList.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zMemory.inline.hpp"

ZMemory* ZMemoryManager::create(zoffset start, size_t size) {
  ZMemory* const area = new ZMemory(start, size);
  if (_callbacks._create != nullptr) {
    _callbacks._create(area);
  }
  return area;
}

void ZMemoryManager::destroy(ZMemory* area) {
  if (_callbacks._destroy != nullptr) {
    _callbacks._destroy(area);
  }
  delete area;
}

void ZMemoryManager::shrink_from_front(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_front != nullptr) {
    _callbacks._shrink_from_front(area, size);
  }
  area->shrink_from_front(size);
}

void ZMemoryManager::shrink_from_back(ZMemory* area, size_t size) {
  if (_callbacks._shrink_from_back != nullptr) {
    _callbacks._shrink_from_back(area, size);
  }
  area->shrink_from_back(size);
}

void ZMemoryManager::grow_from_front(ZMemory* area, size_t size) {
  if (_callbacks._grow_from_front != nullptr) {
    _callbacks._grow_from_front(area, size);
  }
  area->grow_from_front(size);
}

void ZMemoryManager::grow_from_back(ZMemory* area, size_t size) {
  if (_callbacks._grow_from_back != nullptr) {
    _callbacks._grow_from_back(area, size);
  }
  area->grow_from_back(size);
}

ZMemoryManager::Callbacks::Callbacks()
  : _create(nullptr),
    _destroy(nullptr),
    _shrink_from_front(nullptr),
    _shrink_from_back(nullptr),
    _grow_from_front(nullptr),
    _grow_from_back(nullptr) {}

ZMemoryManager::ZMemoryManager()
  : _freelist(),
    _callbacks() {}

bool ZMemoryManager::free_is_contiguous() const {
  return _freelist.size() == 1;
}

void ZMemoryManager::register_callbacks(const Callbacks& callbacks) {
  _callbacks = callbacks;
}

zoffset ZMemoryManager::peek_low_address() const {
  ZLocker<ZLock> locker(&_lock);

  if (_freelist.empty()) {
    // Out of memory
    return zoffset(UINTPTR_MAX);
  }

  return _freelist.front().start();
}

zoffset ZMemoryManager::alloc_low_address(size_t size) {
  ZLocker<ZLock> locker(&_lock);

  for (ZMemory& area : _freelist) {
    if (area.size() >= size) {
      if (area.size() == size) {
        // Exact match, remove area
        const zoffset start = area.start();
        _freelist.erase(_freelist.iterator_to(area));
        destroy(&area);
        return start;
      } else {
        // Larger than requested, shrink area
        const zoffset start = area.start();
        shrink_from_front(&area, size);
        return start;
      }
    }
  }

  // Out of memory
  return zoffset(UINTPTR_MAX);
}

zoffset ZMemoryManager::alloc_low_address_at_most(size_t size, size_t* allocated) {
  ZLocker<ZLock> locker(&_lock);

  if (_freelist.empty()) {
    // Out of memory
    *allocated = 0;
    return zoffset(UINTPTR_MAX);
  }

  ZMemory& area = _freelist.front();
  if (area.size() <= size) {
    // Smaller than or equal to requested, remove area
    const zoffset start = area.start();
    *allocated = area.size();
    _freelist.erase(_freelist.iterator_to(area));
    destroy(&area);
    return start;
  } else {
    // Larger than requested, shrink area
    const zoffset start = area.start();
    shrink_from_front(&area, size);
    *allocated = size;
    return start;
  }
}

zoffset ZMemoryManager::alloc_high_address(size_t size) {
  ZLocker<ZLock> locker(&_lock);

  for (auto area = _freelist.rbegin() ; area != _freelist.rend(); area++) {
    if (area->size() >= size) {
      if (area->size() == size) {
        // Exact match, remove area
        const zoffset start = area->start();
        _freelist.erase(area);
        destroy(&(*area));
        return start;
      } else {
        // Larger than requested, shrink area
        shrink_from_back(&(*area), size);
        return to_zoffset(area->end());
      }
    }
  }

  // Out of memory
  return zoffset(UINTPTR_MAX);
}

void ZMemoryManager::free(zoffset start, size_t size) {
  assert(start != zoffset(UINTPTR_MAX), "Invalid address");
  const zoffset_end end = to_zoffset_end(start, size);

  ZLocker<ZLock> locker(&_lock);

  for (auto area = _freelist.begin(); area != _freelist.end(); ++area) {
    if (start < area->start()) {
      if (area != _freelist.begin()) {
        // Can we merge with previous entry
        ZMemory& prev = *(area--);
        if (start == prev.end()) {
          if (end == area->start()) {
            // Merge with prev and current area
            grow_from_back(&prev, size + area->size());
            _freelist.erase(area);
            delete &(*area);
          } else {
            // Merge with prev area
            grow_from_back(&prev, size);
          }
          return;
        }
      }

      if (end == area->start()) {
        // Merge with current area
        grow_from_front(&(*area), size);
        return;
      } else {
        // Insert new area before current area
        assert(end < area->start(), "Areas must not overlap");
        ZMemory* const new_area = create(start, size);
        _freelist.insert(area, *new_area);
      }
      // Done
      return;
    }
  }

  // Insert last
  if (!_freelist.empty()) {
    ZMemory& last = _freelist.back();
    if (start == last.end()) {
      // Merge with last area
      grow_from_back(&last, size);
    }
  }

  // Insert new area last
  ZMemory* const new_area = create(start, size);
  _freelist.push_back(*new_area);
}
