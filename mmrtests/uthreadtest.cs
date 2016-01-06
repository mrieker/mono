// COPYRIGHT 2009,2016 Mike Rieker, Beverly, MA, USA, mrieker@nii.net
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

using Mono.Tasklets;
using System;
using System.IO;
using System.Threading;

public class UThreadTest {

    public static bool dogc;
    public static bool stopping;

    public static int Main (string[] args)
    {
        GCSense gcSense;
        int i;
        ThreadOne thread1 = null;
        ThreadTwo thread2 = null;

        /*
         * Print some silly message to start.
         */
        Console.WriteLine ("Hello World!");

        /*
         * Test exception passing madness.
         */
        {
            Console.WriteLine ("testing exceptions");
            ThreadExcept thex = new ThreadExcept ();
            Exception ex = thex.StartEx (thex.Main);
            if (ex.Message != "suspend to start") throw ex;
            ex = thex.ResumeEx (new Exception ("resume to suspend"));
            if (ex.Message != "exit to resume") throw ex;
            thex.Dispose ();
            if (!thex.success) {
                Console.WriteLine ("exceptions failed");
                return -1;
            }
            Console.WriteLine ("exceptions are ok");
        }

        /*
         * Have a real thread mallocing in background.
         */
        new Thread (MallocMadness).Start ();

        /*
         * Pound out some thread switching with garbage collection.
         */
        for (i = 0; i < 1000; i ++) {
            dogc = (i % 32) == 0;
            Console.WriteLine ("Main: iteration {0} dogc={1}", i, dogc);

            /*
             * GarbageCollect() doesn't call destructor in here anywhere
             * but it seems to get called a couple steps later.
             */
        ////    new GCSense ("Main/Temp");
        ////    Console.WriteLine ("before GC");
        ////    GarbageCollect ();
        ////    Console.WriteLine ("after GC");
            gcSense = new GCSense ("Main/Perm");

            /*
             * Create a couple MMRUThread derived objects.
             * Use different objects so we switch RIPs and such.
             */
            thread1 = new ThreadOne ();
            thread2 = new ThreadTwo ();

            gcSense.status = "start thread 1";
            gcSense.Print ();
            GarbageCollect ();
            thread1.Start (thread1.Main);
            GarbageCollect ();
            gcSense.status = "start thread 2";
            gcSense.Print ();
            GarbageCollect ();
            thread2.Start (thread2.Main);
            GarbageCollect ();

            /*
             * This is our 'scheduler' loop.  Run threads until all are
             * inactive.
             */
            while ((thread1.Active () < 0) || (thread2.Active () < 0)) {
                if (thread1.Active () < 0) {
                    gcSense.status = "resume thread 1";
                    gcSense.Print ();
                    GarbageCollect ();
                    thread1.Resume ();
                    GarbageCollect ();
                }
                if (thread2.Active () < 0) {
                    gcSense.status = "resume thread 2";
                    gcSense.Print ();
                    GarbageCollect ();
                    thread2.Resume ();
                    GarbageCollect ();
                }
            }

            if (thread1.Active () != 0) {
                Console.WriteLine ("Main: thread 1 is still active");
            }
            if (thread2.Active () != 0) {
                Console.WriteLine ("Main: thread 2 is still active");
            }

            /*
             * Release the 8MB stacks, thread objects can't be re-used.
             */
            thread1.Dispose ();
            thread2.Dispose ();
        }

        stopping = true;
        Console.WriteLine ("Main: final cleanup");
        thread1 = null;
        thread2 = null;
        gcSense = null;
        GarbageCollect ();
        Console.WriteLine ("Main: all done!");

        return 0;
    }

    public static void GarbageCollect ()
    {
        if (dogc) {
            GC.Collect ();
            GC.WaitForPendingFinalizers ();
        }
    }

    public class ThreadExcept : MMRUThread {
        public bool success = false;
        public void Main ()
        {
            Exception ex;

            ex = SuspendEx (new Exception ("suspend to start"));
            if (ex.Message != "resume to suspend") throw ex;
            success = true;
            ex = ExitEx (new Exception ("exit to resume"));
            success = false;
            throw ex;
        }
    }

    public class ThreadOne : MMRUThread {

        public ThreadOne ()
        {
            Console.WriteLine ("ThreadOne {0}", this.GetHashCode());
        }

        ~ThreadOne ()
        {
            Console.WriteLine ("ThreadOne destroyed");
        }

        public void Main ()
        {
            GCSense gcSense = new GCSense ("ThreadOne");
            gcSense.status = "started";
            gcSense.Print ();
            GarbageCollect ();
            Suspend ();
            GarbageCollect ();
            gcSense.status = "step 1";
            gcSense.Print ();
            GarbageCollect ();
            Suspend ();
            GarbageCollect ();
            gcSense.status = "finished";
            gcSense.Print ();
            GarbageCollect ();
            Console.WriteLine ("ThreadOne: returning");
            gcSense = null;
        }
    }

    public class ThreadTwo : MMRUThread {

        public ThreadTwo ()
        {
            Console.WriteLine ("ThreadTwo {0}", this.GetHashCode());
        }

        ~ThreadTwo ()
        {
            Console.WriteLine ("ThreadTwo destroyed");
        }

        public void Main ()
        {
            GCSense gcSense = new GCSense ("ThreadTwo");
            gcSense.status = "started";
            gcSense.Print ();
            GarbageCollect ();
            Suspend ();
            GarbageCollect ();
            gcSense.status = "step 1";
            gcSense.Print ();
            GarbageCollect ();
            Suspend ();
            GarbageCollect ();
            gcSense.status = "step 2";
            gcSense.Print ();
            GarbageCollect ();
            Suspend ();
            GarbageCollect ();
            gcSense.status = "finished";
            gcSense.Print ();
            GarbageCollect ();
            Console.WriteLine ("ThreadTwo: exiting");
            gcSense = null;
            Exit ();
            Console.WriteLine ("ThreadTwo: don't see this");
        }
    }

    // Simple object that we can see when it gets destroyed
    // We use it to see that it is retained by GC even when uthread not running
    public class GCSense {
        public string title;
        public string status;

        public GCSense (string t)
        {
            title = t;
            Console.WriteLine ("GCSense {0} {1}", title, this.GetHashCode());
        }

        ~GCSense ()
        {
            Console.WriteLine ("GCSense {0} destroyed", title);
        }

        public void Print ()
        {
            Console.WriteLine ("GCSense: {0} {1}", title, status);
        }
    }

    public static object[][] keeparound;

    public static void MallocMadness ()
    {
        int minsize = 1000;
        int maxsize = 1000000;
        int numkeep = 100;

        keeparound = new object[numkeep][];

        int ikeep = 0;
        int isize = minsize;
        while (!stopping) {
            keeparound[ikeep] = new object[isize];
            if (++ ikeep == numkeep) ikeep = 0;
            isize += isize / 2;
            if (isize > maxsize) {
                isize = (int) ((long) isize * minsize / maxsize);
            }
        }
    }
}
