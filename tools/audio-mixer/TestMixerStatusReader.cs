using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Diagnostics;
using System.Web.Script.Serialization;
using System.Collections.Generic;
class TestMixerStatusReader
{
    static void Require(bool ok, string message)
    {
        if (!ok)
            throw new Exception(message);
    }
    static string Payload()
    {
        var s = new StringBuilder("{\"revision\":5,\"groups\":[");
        string[] keys = { "master", "music", "effects", "dialogue", "dj", "ambience", "other" };
        for (int i = 0; i < keys.Length; i++)
        {
            if (i > 0)
                s.Append(',');
            s.Append("{\"key\":\"" + keys[i] + "\",\"active\":1,\"peak\":0.25}");
        }
        return s + "]}";
    }
    static int Main(string[] args)
    {
        try
        {
            string folder = Path.Combine(args[0], "status-tests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            string path = Path.Combine(folder, "status.json");
            Require(new JavaScriptSerializer().Deserialize<Dictionary<string, object>>("") == null,
                    "Expected reproduction: empty JSON deserializes to null");
            MixerStatusGroup[] snapshot;
            string valid = Payload();
            foreach (string bad in new[] { "", " ", "null", "{}", "{\"groups\":null}",
                                           "{\"groups\":[null]}", "{\"groups\":{}}",
                                           valid.Replace("\"active\":1", "\"active\":null"),
                                           valid.Replace("\"active\":1", "\"active\":\"bad\""),
                                           valid.Replace("\"peak\":0.25", "\"peak\":-1"),
                                           valid.Replace("\"peak\":0.25", "\"peak\":\"NaN\""),
                                           valid.Replace("\"music\"", "\"master\"") })
            {
                File.WriteAllText(path, bad);
                Require(!MixerStatusReader.TryRead(path, out snapshot) && snapshot == null,
                        "Invalid snapshot accepted: " + bad);
            }
            for (int i = 0; i < valid.Length; i++)
            {
                File.WriteAllText(path, valid.Substring(0, i));
                Require(!MixerStatusReader.TryRead(path, out snapshot),
                        "Truncated snapshot accepted");
            }
            File.WriteAllText(path, valid);
            Require(MixerStatusReader.TryRead(path, out snapshot) && snapshot.Length == 7 &&
                        snapshot[1].Key == "music" && snapshot[1].Peak == 0.25,
                    "Complete snapshot rejected");
            int done = 0, accepted = 0, rejected = 0;
            Exception writerError = null;
            var writer = new Thread(() =>
                                    {
                                        try
                                        {
                                            byte[] bytes = Encoding.UTF8.GetBytes(valid);
                                            for (int i = 0; i < 200; i++)
                                                using (var stream = new FileStream(
                                                           path, FileMode.Create, FileAccess.Write,
                                                           FileShare.ReadWrite | FileShare.Delete))
                                                {
                                                    stream.Flush();
                                                    Thread.Sleep(1);
                                                    stream.Write(bytes, 0, bytes.Length / 2);
                                                    stream.Flush();
                                                    Thread.Sleep(1);
                                                    stream.Write(bytes, bytes.Length / 2,
                                                                 bytes.Length - bytes.Length / 2);
                                                    stream.Flush();
                                                    Thread.Sleep(1);
                                                }
                                        }
                                        catch (Exception e)
                                        {
                                            writerError = e;
                                        }
                                        finally
                                        {
                                            Interlocked.Exchange(ref done, 1);
                                        }
                                    });
            writer.Start();
            while (Interlocked.CompareExchange(ref done, 0, 0) == 0)
            {
                if (MixerStatusReader.TryRead(path, out snapshot))
                    accepted++;
                else
                    rejected++;
                Thread.Sleep(1);
            }
            writer.Join();
            Require(writerError == null, "Writer failed: " + writerError);
            Require(rejected > 0, "Race did not exercise empty/partial reads");
            Require(MixerStatusReader.TryRead(path, out snapshot),
                    "Final complete state not recovered");
            Console.WriteLine("PASS: original empty-JSON null reproduced; " +
                              "empty/null/malformed/duplicate/missing groups and every truncated " +
                              "prefix rejected; 200 concurrent in-place writes; valid snapshots=" +
                              accepted + ", incomplete snapshots=" + rejected +
                              "; final recovery. Evidence: " + folder);
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e);
            return 1;
        }
    }
}
