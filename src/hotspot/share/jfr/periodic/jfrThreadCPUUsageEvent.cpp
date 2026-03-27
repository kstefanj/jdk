/*
 * Copyright (c) 2017, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "jfr/jfrEvents.hpp"
#include "jfr/periodic/jfrThreadCPUUsageEvent.hpp"
#include "jfr/utilities/jfrThreadIterator.hpp"
#include "jfr/utilities/jfrTime.hpp"
#include "logging/log.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"

jlong JfrThreadCPUUsageEvent::get_wallclock_time() {
  return os::javaTimeNanos();
}

void JfrThreadCPUUsageEvent::send_usage_events() {
  Thread* periodic_thread = Thread::current();
  traceid periodic_thread_id = JfrThreadLocal::thread_id(periodic_thread);
  JfrTicks event_time = JfrTicks::now();
  jlong cur_wallclock_time = JfrThreadCPUUsageEvent::get_wallclock_time();

  JfrJavaThreadIterator iter;
  int number_of_threads = 0;
  while (iter.has_next()) {
    JavaThread* const jt = iter.next();
    assert(jt != nullptr, "invariant");
    ++number_of_threads;
    EventThreadCPUUsage event(UNTIMED);
    if (update_event(event, jt, cur_wallclock_time)) {
      event.set_starttime(event_time);
      event.set_endtime(event_time);
      if (jt != periodic_thread) {
        // Commit reads the thread id from this thread's trace data, so put it there temporarily
        JfrThreadLocal::impersonate(periodic_thread, JFR_JVM_THREAD_ID(jt));
      } else {
        JfrThreadLocal::impersonate(periodic_thread, periodic_thread_id);
      }
      event.commit();
    }
  }
  log_trace(jfr)("Measured CPU usage for %d threads in %.3f milliseconds", number_of_threads,
    (double)(JfrTicks::now() - event_time).milliseconds());
  JfrThreadLocal::stop_impersonating(periodic_thread);
}

void JfrThreadCPUUsageEvent::send_event_for_thread(JavaThread* jt) {
  jlong wallclock_time = get_wallclock_time();
  EventThreadCPUUsage event;
  if (event.should_commit()) {
    if (update_event(event, jt, wallclock_time)) {
      event.commit();
    }
  }
}

bool JfrThreadCPUUsageEvent::update_event(EventThreadCPUUsage& event, JavaThread* thread, jlong cur_wallclock_time) {
  jlong cur_cpu_total = os::thread_cpu_time(thread, true);
  jlong cur_cpu_user = os::thread_cpu_time(thread, false);

  jlong prev_cpu_total = thread->last_cpu_total();
  jlong prev_cpu_user = thread->last_cpu_user();
  jlong prev_wall = thread->last_wall();

  jlong user_diff = cur_cpu_user - prev_cpu_user;
  jlong total_diff = cur_cpu_total - prev_cpu_total;
  jlong wall_diff = cur_wallclock_time - prev_wall;

  if (total_diff == 0) {
    return false;
  }

  event.set_threadState(thread->thread_state());
  event.set_totalNanosDelta(total_diff);
  event.set_userNanosDelta(user_diff);
  event.set_wallNanosDelta(wall_diff);

  thread->set_last_cpu_total(cur_cpu_total);
  thread->set_last_cpu_user(cur_cpu_user);
  thread->set_last_wall(cur_wallclock_time);
  return true;
}

