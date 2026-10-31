using System;
using System.IO;
using System.Text;
using System.Diagnostics;
using System.Threading;
using System.Runtime.InteropServices;
class TestMixerConfigStore
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileEx(string source, string destination, int flags);
    static string Payload(int value)
    {
        return "master=100\r\nmusic=" + value +
               ("\r\neffects=100\r\ndialogue=100\r\ndj=100\r\nambience=100\r\nother=100\r\n#" +
                "complete=1\r\n");
    }
    static void Require(bool condition, string error)
    {
        if (!condition)
            throw new Exception(error);
    }
    static void Drain(MixerConfigStore store)
    {
        var timeout = Stopwatch.StartNew();
        while (store.HasPending && timeout.ElapsedMilliseconds < 5000)
        {
            store.TryFlush(false);
            Thread.Sleep(20);
        }
        Require(!store.HasPending, "Queued latest state did not commit");
    }
    static int Main(string[] args)
    {
        try
        {
            string dir = Path.Combine(Path.GetFullPath(args[0]),
                                      "save-tests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, "mixer.ini");
            File.WriteAllText(path, Payload(100));
            string oldTemp = path + ".old.tmp";
            File.WriteAllText(oldTemp, Payload(20));
            var store = new MixerConfigStore(path);
            int reproducedError;
            using (var legacy =
                       new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                Require(!MoveFileEx(oldTemp, path, 9),
                        "Expected reproduction of old failed replace");
                reproducedError = Marshal.GetLastWin32Error();
                store.Queue(Payload(30));
                Require(!store.TryFlush(true), "Reader lock was not reproduced");
                Require(store.HasPending, "Lost pending slider state");
                store.Queue(Payload(63));
                Require(store.PendingText == Payload(63), "Newest slider value was lost");
                Require(!store.TryFlush(true), "Unexpected commit through legacy lock");
            }
            Drain(store);
            Require(MixerConfigStore.ReadSharedText(path) == Payload(63),
                    "Did not persist final slider position");
            using (var compatible = new FileStream(path, FileMode.Open, FileAccess.Read,
                                                   FileShare.ReadWrite | FileShare.Delete))
            {
                store.Queue(Payload(47));
                if (!store.TryFlush(true))
                    Console.WriteLine("Shared-reader retry: " + store.LastErrorCode + " " +
                                      store.LastError);
                Drain(store);
                Require(MixerConfigStore.ReadSharedText(path) == Payload(47),
                        "Atomic replace wrote wrong state");
            }
            int stop = 0, partial = 0, readErrors = 0;
            var reader = new Thread(
                () =>
                {
                    while (Interlocked.CompareExchange(ref stop, 0, 0) == 0)
                    {
                        try
                        {
                            using (var f = new FileStream(
                                       path, FileMode.Open, FileAccess.Read,
                                       FileShare.Read)) using (var r = new StreamReader(f))
                            {
                                string text = r.ReadToEnd();
                                if (!text.StartsWith("master=100\r\n") ||
                                    !text.EndsWith("#complete=1\r\n"))
                                    Interlocked.Increment(ref partial);
                                Thread.Sleep(2);
                            }
                        }
                        catch (IOException)
                        {
                            Interlocked.Increment(ref readErrors);
                        }
                        Thread.Sleep(1);
                    }
                });
            reader.Start();
            for (int i = 0; i < 500; i++)
            {
                store.Queue(Payload(i % 101));
                store.TryFlush(false);
                Thread.Sleep(2);
            }
            store.Queue(Payload(17));
            Interlocked.Exchange(ref stop, 1);
            reader.Join();
            Drain(store);
            Require(partial == 0, "Reader observed a partial configuration");
            Require(MixerConfigStore.ReadSharedText(path) == Payload(17),
                    "Rapid updates did not preserve latest value");
            using (var locked =
                       new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                store.Queue(Payload(71));
                Require(!store.TryFlush(true), "Expected held reader for recovery test");
                Require(File.Exists(store.RecoveryFile), "Pending recovery journal missing");
            }
            var recovered = new MixerConfigStore(path);
            Require(recovered.PendingText == Payload(71), "Pending settings were not recovered");
            Drain(recovered);
            Require(MixerConfigStore.ReadSharedText(path) == Payload(71),
                    "Recovered settings did not persist");
            Console.WriteLine("PASS: old save failure reproduced (Win32 " + reproducedError +
                              ("); latest state retained/retried; shared-delete read; 500 rapid " +
                               "updates; no partial reads; journal recovery. Evidence: ") +
                              dir);
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e);
            return 1;
        }
    }
}
