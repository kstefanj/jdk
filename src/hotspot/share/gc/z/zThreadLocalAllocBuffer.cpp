/*
 * Copyright (c) 2018, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/tlab_globals.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zStackWatermark.hpp"
#include "gc/z/zThreadLocalAllocBuffer.hpp"
#include "gc/z/zValue.inline.hpp"
#include "jfr/jfrEvents.hpp"
#include "jfr/support/jfrThreadId.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/stackWatermarkSet.inline.hpp"

ZPerWorker<ThreadLocalAllocStats>* ZThreadLocalAllocBuffer::_stats = nullptr;

void ZThreadLocalAllocBuffer::initialize() {
  if (UseTLAB) {
    assert(_stats == nullptr, "Already initialized");
    _stats = new ZPerWorker<ThreadLocalAllocStats>();
    reset_statistics();
  }
}

void ZThreadLocalAllocBuffer::reset_statistics() {
  if (UseTLAB) {
    ZPerWorkerIterator<ThreadLocalAllocStats> iter(_stats);
    for (ThreadLocalAllocStats* stats; iter.next(&stats);) {
      stats->reset();
    }
  }
}

void ZThreadLocalAllocBuffer::publish_statistics() {
  if (UseTLAB) {
    ThreadLocalAllocStats total;

    ZPerWorkerIterator<ThreadLocalAllocStats> iter(_stats);
    for (ThreadLocalAllocStats* stats; iter.next(&stats);) {
      total.update(*stats);
    }

    total.publish();
  }
}

void ZThreadLocalAllocBuffer::retire_event(JavaThread* thread, ThreadLocalAllocStats* stats, size_t actual_tlab_size, size_t prev_desired_size) {
  EventZTLABRetire event;
  event.set_refills(stats->_total_refills);
  event.set_tlab_size(actual_tlab_size); // Already in bytes
  event.set_sum_tlabs(stats->_total_allocations * HeapWordSize);
  event.set_prev_desired_size(prev_desired_size * HeapWordSize);
  event.set_desired_size(thread->tlab().desired_size() * HeapWordSize);
  event.set_gc_waste(stats->_total_gc_waste * HeapWordSize);
  event.set_refill_waste(stats->_total_refill_waste * HeapWordSize);
  event.set_thread(JFR_JVM_THREAD_ID(thread));
  event.commit();
}

void ZThreadLocalAllocBuffer::retire(JavaThread* thread, ThreadLocalAllocStats* stats) {
  if (UseTLAB) {
    stats->reset();
    size_t desired_size = thread->tlab().desired_size();
    size_t actual_tlab_size = thread->tlab().size_bytes();
    thread->tlab().retire(stats);
    if (ResizeTLAB) {
      thread->tlab().resize();
    }
    retire_event(thread, stats, actual_tlab_size, desired_size);
  }
}

void ZThreadLocalAllocBuffer::update_stats(JavaThread* thread) {
  if (UseTLAB) {
    ZStackWatermark* const watermark = StackWatermarkSet::get<ZStackWatermark>(thread, StackWatermarkKind::gc);
    _stats->addr()->update(watermark->stats());
  }
}
