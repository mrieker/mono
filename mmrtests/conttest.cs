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

public class ContTest {

    public static Continuation maincont;
    public static Continuation stepcont;

    public static void Main (string[] args)
    {
        maincont = new Continuation ();
        stepcont = new Continuation ();

        Console.WriteLine ("doing first mark");
        maincont.Mark ();

        try {
            Inner ();
        } finally {
            maincont.Dispose ();
        }
        Console.WriteLine ("main done");
    }

    public static void Inner ()
    {
        int rc = maincont.Store (20);
        if (rc == 20) {
            StepperLoop ();
        }

        while (rc == 21) {
            Console.WriteLine ("back in main");
            rc = maincont.Store (23);
            if (rc == 23) {
                stepcont.Restore (31);
                Console.WriteLine ("after restore 31");
            }
        }
        Console.WriteLine ("inner done");
    }

    public static void StepperLoop ()
    {
        Console.WriteLine ("doing second mark");
        stepcont.Mark ();
        try {
            for (int i = 0; i < 5; i ++) {
                Console.WriteLine ("step " + i);
                if (stepcont.Store (30) == 30) {
                    maincont.Restore (21);
                    Console.WriteLine ("after restore 21");
                }
            }
            Console.WriteLine ("step done");
            maincont.Restore (22);
            Console.WriteLine ("after restore 22");
        } finally {
            stepcont.Dispose ();
        }
    }
}
