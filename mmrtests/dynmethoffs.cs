// COPYRIGHT 2016 Mike Rieker, Beverly, MA, USA, mrieker@nii.net
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

using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;

public class UThreadTest {

    public delegate int IntByIntDel (string a, string b);

    public static void Main (String[] args)
    {
        DynamicMethod dynmeth = new DynamicMethod ("IntRatio", typeof (int), new Type[] { typeof (string), typeof (string) });
        ILGenerator ilgen = dynmeth.GetILGenerator ();
        ilgen.Emit (OpCodes.Ldarg_0);
        ilgen.Emit (OpCodes.Call, typeof (int).GetMethod ("Parse", new Type[] { typeof (string) }));
        ilgen.Emit (OpCodes.Ldarg_1);
        ilgen.Emit (OpCodes.Call, typeof (int).GetMethod ("Parse", new Type[] { typeof (string) }));
        ilgen.Emit (OpCodes.Div);
        ilgen.Emit (OpCodes.Ret);

        IntByIntDel intratio = (IntByIntDel) dynmeth.CreateDelegate (typeof (IntByIntDel));

        try {
            Console.WriteLine (intratio (args[0], args[1]));
        } catch (Exception e) {
            StackTrace st = new StackTrace (e, true);
            foreach (StackFrame sf in st.GetFrames ()) {
                Console.WriteLine ("      column=" + sf.GetFileColumnNumber ());
                Console.WriteLine ("        line=" + sf.GetFileLineNumber ());
                Console.WriteLine ("    filename=" + (sf.GetFileName () == null ? "<<null>>" : sf.GetFileName ()));
                Console.WriteLine ("    iloffset=" + sf.GetILOffset ());
                MethodBase meth = sf.GetMethod ();
                Console.WriteLine ("      method=" + (meth == null ? "<<null>>" : (meth.ReflectedType.ToString () + "::" + meth.Name)));
                Console.WriteLine ("nativeoffset=" + sf.GetNativeOffset ());
                Console.WriteLine ("      string=" + sf.ToString ());
                Console.WriteLine ("");
            }
            Console.WriteLine (e.ToString ());
        }
    }
}
