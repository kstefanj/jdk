/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
 * @test UnloadObjArrayTest
 * @requires vm.opt.final.ClassUnloading
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @library classes
 * @build jdk.test.whitebox.WhiteBox test.Empty
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -Xmn8m -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -Xlog:gc*,class+unload=debug UnloadObjArrayTest
 */
import jdk.test.whitebox.WhiteBox;
import jdk.test.lib.classloader.ClassUnloadCommon;

/**
 * Test that verifies that object array classes are only unloaded when
 * they are no longer reachable.
 */
public class UnloadObjArrayTest {
    private static String className = "test.Empty";

    public static void main(String... args) throws Exception {
        final WhiteBox wb = WhiteBox.getWhiteBox();

        ClassUnloadCommon.failIf(wb.isClassAlive(className), "is not expected to be alive yet");

        var classLoader = ClassUnloadCommon.newClassLoader();
        var loadedClass = classLoader.loadClass(className);
        var array = java.lang.reflect.Array.newInstance(loadedClass, 10);
        var arrayName = array.getClass().getName();

        ClassUnloadCommon.failIf(!wb.isClassAlive(arrayName), "should be live here: " + arrayName);
        ClassUnloadCommon.failIf(!wb.isClassAlive(className), "should be live here: " + className);

        classLoader = null;
        loadedClass = null;

        ClassUnloadCommon.triggerUnloading(); // WhiteBox Full GC

        // Here 'array' is still live so classes should be live.
        ClassUnloadCommon.failIf(!wb.isClassAlive(arrayName), "should be live here: " + arrayName);
        ClassUnloadCommon.failIf(!wb.isClassAlive(className), "should be live here: " + className);

        array = null;
        ClassUnloadCommon.triggerUnloading(); // WhiteBox Full GC

        ClassUnloadCommon.failIf(wb.isClassAlive(arrayName), "should have been unloaded: " + arrayName);
        ClassUnloadCommon.failIf(wb.isClassAlive(className), "should have been unloaded: " + className);
    }
}
