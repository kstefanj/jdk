/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

/*
 * @test id=no-specific-gc
 * @summary Run test with default/user options.
 * @requires os.family == "linux"
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:-SegmentedCodeCache TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:-SegmentedCodeCache -XX:+UseLargePages TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:-SegmentedCodeCache -XX:+UseTransparentHugePages TestTracePageSizes
 */

/*
 * @test id=with-G1
 * @summary Run tests with G1
 * @requires os.family == "linux"
 * @requires vm.gc.G1
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseG1GC TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseG1GC -XX:+UseLargePages TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseG1GC -XX:+UseTransparentHugePages TestTracePageSizes
*/

/*
 * @test id=with-Parallel
 * @summary Run tests with Parallel
 * @requires os.family == "linux"
 * @requires vm.gc.Parallel
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseParallelGC TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseParallelGC -XX:+UseLargePages TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseParallelGC -XX:+UseTransparentHugePages TestTracePageSizes
*/

/*
 * @test id=with-Serial
 * @summary Run tests with Serial
 * @requires os.family == "linux"
 * @requires vm.gc.Serial
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseSerialGC TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseSerialGC -XX:+UseLargePages TestTracePageSizes
 * @run main/othervm -XX:+AlwaysPreTouch -Xlog:pagesize:ps-%p.log -XX:+UseSerialGC -XX:+UseTransparentHugePages TestTracePageSizes
*/

import java.io.File;
import java.util.LinkedList;
import java.util.Scanner;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/* Check that page sizes logged match what is recorded in /proc/self/smaps.
 * The file smaps file is parsed and compared against what's logged by the
 * JVM. For transparent huge pages the matching is best effort since we
 * can't know for sure what the underlying page size is.
 */
public class TestTracePageSizes {
    // Store address ranges with known page size.
    private static LinkedList<RangeWithPageSize> ranges = new LinkedList<>();
    private static boolean debug;

    // Parse /proc/self/smaps using a regexp capturing the address
    // ranges, what page size they have and if they might use
    // transparent huge pages. The pattern is not greedy and will
    // match as little as possible so each "segment" in the file
    // will generate a match.
    private static void parseSmaps() throws Exception {
        String smapsPatternString = "(\\w+)-(\\w+).*?" +
                                    "KernelPageSize:\\s*(\\d*) kB.*?" +
                                    "THPeligible:\\s*([01])";
        Pattern smapsPattern = Pattern.compile(smapsPatternString, Pattern.DOTALL);

        Scanner smapsScanner = new Scanner(new File("/proc/self/smaps"));
        smapsScanner.findAll(smapsPattern).forEach(mr -> {
            String start = mr.group(1);
            String end = mr.group(2);
            String ps = mr.group(3);
            String thp = mr.group(4);
            ranges.add(new RangeWithPageSize(start, end, ps, thp));
        });
        smapsScanner.close();
    }

    private static RangeWithPageSize getRange(String addr) {
        long laddr = Long.decode(addr);
        for (RangeWithPageSize range : ranges) {
            if (range.includes(laddr)) {
                return range;
            }
        }
        return null;
    }

    // Helper to get the page size in KB given a page size parsed
    // from log_info(pagesize) output.
    private static long pageSizeInKB(String pageSize) {
        String value = pageSize.substring(0, pageSize.length()-1);
        String unit = pageSize.substring(pageSize.length()-1);
        long ret = Long.parseLong(value);
        if (unit.equals("K")) {
            return ret;
        } else if (unit.equals("M")) {
            return ret * 1024;
        } else if (unit.equals("G")) {
            return ret * 1024 * 1024;
        }
        return 0;
    }

    // This test needs to be run with:
    //  * -Xlog:pagesize:ps-%p.log - To generate the log file parsed
    //  * -XX:+AlwaysPreTouch - To make sure mapped memory is touched
    //    and recorded in the relevant information in the maps files.
    public static void main(String args[]) throws Exception {
        // Check if we should do debug printing.
        if (args.length > 0 && args[0].equals("-debug")) {
            debug = true;
        } else {
            debug = false;
        }

        // Parse /proc/self/smaps to compare with values logged in the VM.
        parseSmaps();

        // Setup patters for the JVM page size logging.
        String traceLinePatternString = ".*base=(0x[0-9A-Fa-f]*).*page_size=([^ ]+).*";
        Pattern traceLinePattern = Pattern.compile(traceLinePatternString);

        // The test needs to be run with page size logging printed to ps-$pid.log.
        Scanner fileScanner = new Scanner(new File("./ps-" + ProcessHandle.current().pid() + ".log"));
        while (fileScanner.hasNextLine()) {
            String line = fileScanner.nextLine();
            if (line.matches(traceLinePatternString)){
                Matcher trace = traceLinePattern.matcher(line);
                trace.find();

                String address = trace.group(1);
                String pageSize = trace.group(2);
                RangeWithPageSize range = getRange(address);
                long pageSizeFromSmaps = range.getPageSize();
                long pageSizeFromTrace = pageSizeInKB(pageSize);

                debug("From logfile: " + line);
                debug("From smaps: " + range);

                if (pageSizeFromSmaps != pageSizeFromTrace) {
                    if (pageSizeFromTrace > pageSizeFromSmaps && range.isThpEligible()) {
                        // Page sizes mismatch because we can't know what underlying page size will
                        // be used when THP is enabled. So this is not a failure.
                        debug("Success: " + pageSizeFromTrace + " > " + pageSizeFromSmaps + " and THP enabled");
                    } else {
                        debug("Failure: " + pageSizeFromSmaps + " != " + pageSizeFromTrace);
                        throw new AssertionError("Page sizes mismatch: " + pageSizeFromSmaps + " != " + pageSizeFromTrace);
                    }
                } else {
                    debug("Success: " + pageSizeFromSmaps + " == " + pageSizeFromTrace);
                }
            }
            debug("---");
        }
        fileScanner.close();
    }

    private static void debug(String str) {
        if (debug) {
            System.out.println(str);
        }
    }
}

class RangeWithPageSize {
    private long start;
    private long end;
    private long pageSize;
    private boolean thpEligible;

    public RangeWithPageSize(String start, String end, String pageSize, String thp) {
        this.start = Long.parseUnsignedLong(start, 16);
        this.end = Long.parseUnsignedLong(end, 16);;
        this.pageSize = Long.parseLong(pageSize);
        this.thpEligible = thp.equals("1");
    }

    public long getPageSize() {
        return pageSize;
    }

    public boolean isThpEligible() {
        return thpEligible;
    }

    public boolean includes(long addr) {
        return start <= addr && addr < end;
    }

    public String toString() {
        return "[" + Long.toHexString(start) + ", " + Long.toHexString(end) + ") pageSize=" + pageSize + "KB isTHP=" + thpEligible;
    }
}
