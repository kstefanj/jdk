/*
 * Copyright (c) 2002, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_COLLECTORCPUTIMECOUNTERS_HPP
#define SHARE_GC_SHARED_COLLECTORCPUTIMECOUNTERS_HPP

#include "runtime/perfData.hpp"
#include "runtime/perfDataTypes.hpp"

class CollectorCPUTimeGroups : public AllStatic {
public:
  enum Name {
    total,
    gc_parallel_workers,
    gc_conc_mark,
    gc_conc_refine,
    gc_service,
    count,
  };

  static const char* to_string(Name val);
};

class CollectorCPUTimeCounters: public CHeapObj<mtGC> {
private:
  // Perf counter to track total CPU time across all threads. Defined here in
  // order to be reused for all collectors.
  PerfCounter* _cpu_time_counters[CollectorCPUTimeGroups::count];

  // A long which atomically tracks how much CPU time has been spent doing GC
  // since the last time we called `publish_total_cpu_time()`.
  // It is incremented using Atomic::add() to prevent race conditions, and
  // is added to `_total_cpu_time` at the end of GC.
  volatile jlong _total_cpu_time_diff;
  void create_counter(CounterNS ns, CollectorCPUTimeGroups::Name name);

public:
  CollectorCPUTimeCounters();

  ~CollectorCPUTimeCounters();

  // Methods to modify and update counter for total CPU time spent doing GC.
  void inc_total_cpu_time(jlong diff);
  void publish_total_cpu_time();

  void create_counter(CollectorCPUTimeGroups::Name name);
  PerfCounter* get_counter(CollectorCPUTimeGroups::Name name);
};

#endif // SHARE_GC_SHARED_COLLECTORCOUNTERS_HPP
