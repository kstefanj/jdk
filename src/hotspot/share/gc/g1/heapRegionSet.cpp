/*
 * Copyright (c) 2011, 2023, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#include "precompiled.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1NUMA.hpp"
#include "gc/g1/heapRegionRemSet.inline.hpp"
#include "gc/g1/heapRegionSet.inline.hpp"

uint FreeRegionList::_unrealistically_long_length = 0;

#ifndef PRODUCT
void HeapRegionSetBase::verify_region(HeapRegion* hr) {
  assert(hr->containing_set() == this, "Inconsistent containing set for %u", hr->hrm_index());
  assert(!hr->is_young(), "Adding young region %u", hr->hrm_index()); // currently we don't use these sets for young regions
  assert(_checker == nullptr || _checker->is_correct_type(hr), "Wrong type of region %u (%s) and set %s",
         hr->hrm_index(), hr->get_type_str(), name());
  assert(!hr->is_free() || hr->is_empty(), "Free region %u is not empty for set %s", hr->hrm_index(), name());
  assert(!hr->is_empty() || hr->is_free(), "Empty region %u is not free or old for set %s", hr->hrm_index(), name());
}
#endif

void HeapRegionSetBase::verify() {
  // It's important that we also observe the MT safety protocol even
  // for the verification calls. If we do verification without the
  // appropriate locks and the set changes underneath our feet
  // verification might fail and send us on a wild goose chase.
  check_mt_safety();

  guarantee_heap_region_set(( is_empty() && length() == 0) ||
                            (!is_empty() && length() > 0),
                            "invariant");
}

void HeapRegionSetBase::verify_start() {
  // See comment in verify() about MT safety and verification.
  check_mt_safety();
  assert_heap_region_set(!_verify_in_progress, "verification should not be in progress");

  // Do the basic verification first before we do the checks over the regions.
  HeapRegionSetBase::verify();

  _verify_in_progress = true;
}

void HeapRegionSetBase::verify_end() {
  // See comment in verify() about MT safety and verification.
  check_mt_safety();
  assert_heap_region_set(_verify_in_progress, "verification should be in progress");

  _verify_in_progress = false;
}

void HeapRegionSetBase::print_on(outputStream* out, bool print_contents) {
  out->cr();
  out->print_cr("Set: %s (" PTR_FORMAT ")", name(), p2i(this));
  out->print_cr("  Region Type         : %s", _checker->get_description());
  out->print_cr("  Length              : %14u", length());
}

HeapRegionSetBase::HeapRegionSetBase(const char* name, HeapRegionSetChecker* checker)
  : _checker(checker), _length(0), _name(name), _verify_in_progress(false)
{
}

void FreeRegionList::set_unrealistically_long_length(uint len) {
  guarantee(_unrealistically_long_length == 0, "should only be set once");
  _unrealistically_long_length = len;
}

void FreeRegionList::abandon() {
  check_mt_safety();
  _list.clear();
  clear();
  verify_optional();
}

void FreeRegionList::remove_all() {
  check_mt_safety();
  verify_optional();

  auto dispose = [=](const HeapRegion& hr) {
    const_cast<HeapRegion&>(hr).set_containing_set(nullptr);
    decrease_length(hr.node_index());
  };

  _list.clear_and_dispose(dispose);
  clear();

  verify_optional();
}

void FreeRegionList::add_list_common_start(FreeRegionList* from_list) {
  check_mt_safety();
  from_list->check_mt_safety();
  verify_optional();
  from_list->verify_optional();

  if (from_list->is_empty()) {
    return;
  }

  if (_node_info != nullptr && from_list->_node_info != nullptr) {
    _node_info->add(from_list->_node_info);
  }

  #ifdef ASSERT
  for(HeapRegion& hr: from_list->list()) {
    // In set_containing_set() we check that we either set the value
    // from null to non-null or vice versa to catch bugs. So, we have
    // to null it first before setting it to the value.
    hr.set_containing_set(nullptr);
    hr.set_containing_set(this);
  }
  #endif // ASSERT
}

void FreeRegionList::add_list_common_end(FreeRegionList* from_list) {
  _length += from_list->length();
  from_list->clear();

  verify_optional();
  from_list->verify_optional();
}

void FreeRegionList::append_ordered(FreeRegionList* from_list) {
  add_list_common_start(from_list);

  if (from_list->is_empty()) {
    return;
  }

  _list.splice(_list.end(), from_list->_list);
  add_list_common_end(from_list);
}

void FreeRegionList::add_ordered(FreeRegionList* from_list) {
  add_list_common_start(from_list);

  if (from_list->is_empty()) {
    return;
  }

  HeapRegion::FreeList& other = from_list->list();
  if (is_empty()) {
    assert_free_region_list(length() == 0 && _list.empty(), "invariant");
    _list.splice(_list.end(), other);
  } else {
    auto to_iter = _list.begin();
    for (auto from_iter = other.begin(); from_iter != other.end();) {
      while (to_iter != _list.end() && to_iter->hrm_index() < from_iter->hrm_index()) {
        ++to_iter;
      }

      if (to_iter == _list.end()) {
        // End on list, transfer rest of from
        _list.splice(to_iter, other, from_iter, other.end());
        break;
      } else {
        // Transfer current element, but make sure to
        // advance the iterator and use a copy for the
        // splice.
        _list.splice(to_iter, other, from_iter++);
      }
    }
   }

  add_list_common_end(from_list);
}

void FreeRegionList::remove_starting_at(HeapRegion* first, uint num_regions) {
  check_mt_safety();
  assert_free_region_list(num_regions >= 1, "pre-condition");
  assert_free_region_list(!is_empty(), "pre-condition");
  assert_free_region_list(length() >= num_regions, "pre-condition");

  verify_optional();
  DEBUG_ONLY(uint old_length = length();)

  auto dispose = [=](const HeapRegion& hr) {
    if (_last == &hr) {
      _last = nullptr;
    }
    remove(const_cast<HeapRegion*>(&hr));
    decrease_length(hr.node_index());
  };
  auto curr = _list.iterator_to(*first);
  uint count = 0;
  for (; count < num_regions; count++) {
    assert(curr != _list.end(), "invariant");
    verify_region(&(*curr));
    curr = _list.erase_and_dispose(curr, dispose);
  }

  assert(count == num_regions,
         "[%s] count: %u should be == num_regions: %u",
         name(), count, num_regions);
  assert(length() + num_regions == old_length,
         "[%s] new length should be consistent "
         "new length: %u old length: %u num_regions: %u",
         name(), length(), old_length, num_regions);

  verify_optional();
}

void FreeRegionList::verify() {
  // See comment in HeapRegionSetBase::verify() about MT safety and
  // verification.
  check_mt_safety();

  // This will also do the basic verification too.
  verify_start();

  verify_list();

  verify_end();
}

void FreeRegionList::clear() {
  assert(_list.empty(), "Should be no elements in: %s", name());
  _length = 0;
  _last = nullptr;

  if (_node_info!= nullptr) {
    _node_info->clear();
  }
}

void FreeRegionList::verify_list() {
  uint count = 0;
  size_t capacity = 0;
  uint last_index = 0;

  for (HeapRegion& hr : _list) {
    HeapRegion* curr = &hr;
    verify_region(curr);

    count++;
    guarantee(count < _unrealistically_long_length,
              "[%s] the calculated length: %u seems very long, is there maybe a cycle? curr: " PTR_FORMAT " length: %u",
              name(), count, p2i(curr), length());

    guarantee(curr->hrm_index() == 0 || curr->hrm_index() > last_index, "List should be sorted");
    last_index = curr->hrm_index();

    capacity += curr->capacity();
  }
  guarantee(length() == count, "%s count mismatch. Expected %u, actual %u.", name(), length(), count);
}


FreeRegionList::FreeRegionList(const char* name, HeapRegionSetChecker* checker):
  HeapRegionSetBase(name, checker),
  _list(),
  _node_info(G1NUMA::numa()->is_enabled() ? new NodeInfo() : nullptr) {

  clear();
}

FreeRegionList::~FreeRegionList() {
  if (_node_info != nullptr) {
    delete _node_info;
  }
}

FreeRegionList::NodeInfo::NodeInfo() : _numa(G1NUMA::numa()), _length_of_node(nullptr),
                                       _num_nodes(_numa->num_active_nodes()) {
  assert(UseNUMA, "Invariant");

  _length_of_node = NEW_C_HEAP_ARRAY(uint, _num_nodes, mtGC);
}

FreeRegionList::NodeInfo::~NodeInfo() {
  FREE_C_HEAP_ARRAY(uint, _length_of_node);
}

void FreeRegionList::NodeInfo::clear() {
  for (uint i = 0; i < _num_nodes; ++i) {
    _length_of_node[i] = 0;
  }
}

void FreeRegionList::NodeInfo::add(NodeInfo* info) {
  for (uint i = 0; i < _num_nodes; ++i) {
    _length_of_node[i] += info->_length_of_node[i];
  }
}
