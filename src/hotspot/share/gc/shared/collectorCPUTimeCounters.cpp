/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/collectorCPUTimeCounters.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"

const char* CollectorCPUTimeGroups::to_string(Name val) {
  switch (val) {
    case total:
      return "total_gc_cpu_time";
    case gc_parallel_workers:
      return "gc_parallel_workers";
    case gc_conc_mark:
      return "gc_conc_mark";
    case gc_conc_refine:
      return "gc_conc_refine";
    case gc_service:
      return "gc_service";
    case count:
      return "Illegal counter";
  };
  ShouldNotReachHere();
}

CollectorCPUTimeCounters::CollectorCPUTimeCounters() :
    _cpu_time_counters{nullptr},
    _total_cpu_time_diff(0) {

  create_counter(SUN_THREADS, CollectorCPUTimeGroups::total);
}

void CollectorCPUTimeCounters::inc_total_cpu_time(jlong diff) {
  Atomic::add(&_total_cpu_time_diff, diff);
}

void CollectorCPUTimeCounters::publish_total_cpu_time() {
  // Ensure that we are only incrementing atomically by using Atomic::cmpxchg
  // to set the value to zero after we obtain the new CPU time difference.
  jlong old_value;
  jlong fetched_value = Atomic::load(&_total_cpu_time_diff);
  jlong new_value = 0;
  do {
    old_value = fetched_value;
    fetched_value = Atomic::cmpxchg(&_total_cpu_time_diff, old_value, new_value);
  } while (old_value != fetched_value);
  get_counter(CollectorCPUTimeGroups::total)->inc(fetched_value);
}

void CollectorCPUTimeCounters::create_counter(CounterNS ns, CollectorCPUTimeGroups::Name name) {
  if (UsePerfData) {
    EXCEPTION_MARK;
    if (os::is_thread_cpu_time_supported()) {
      _cpu_time_counters[name] =
                  PerfDataManager::create_counter(ns, CollectorCPUTimeGroups::to_string(name),
                                                  PerfData::U_Ticks, CHECK);
    }
  }
}

void CollectorCPUTimeCounters::create_counter(CollectorCPUTimeGroups::Name group) {
  create_counter(SUN_THREADS_CPUTIME, group);
}

PerfCounter* CollectorCPUTimeCounters::get_counter(CollectorCPUTimeGroups::Name name) {
  return _cpu_time_counters[name];
}