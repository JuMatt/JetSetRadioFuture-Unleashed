using System;
using System.IO;
using System.Text;
using System.ComponentModel;
using System.Runtime.InteropServices;
// Used only on the UI thread. Queue changes immediately; a timer retries I/O.
// A failed replace leaves the newest requested state pending, never reloads
// the older on-disk state, and never throws an I/O exception into a UI event.
public sealed class MixerConfigStore
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileEx(string source, string destination, int flags);
    readonly string path, temp;
    string pending;
    DateTime retryAt = DateTime.MinValue;
    int failures;
    public bool HasPending
    {
        get {
            return pending != null;
        }
    }
    public string PendingText
    {
        get {
            return pending;
        }
    }
    public string LastError { get; private set; }
    public int LastErrorCode { get; private set; }
    public DateTime LastCommittedWriteUtc { get; private set; }
    public string RecoveryFile
    {
        get {
            return temp;
        }
    }
    public MixerConfigStore(string file)
    {
        path = Path.GetFullPath(file);
        temp = path + "." + Guid.NewGuid().ToString("N") + ".pending";
        // A complete journal can survive closing while a third-party reader holds
        // the target. Recover only a journal newer than the committed preferences.
        try
        {
            DateTime newest =
                File.Exists(path) ? File.GetLastWriteTimeUtc(path) : DateTime.MinValue;
            string recover = null;
            foreach (string candidate in Directory.GetFiles(
                         Path.GetDirectoryName(path), Path.GetFileName(path) + ".*.pending"))
            {
                DateTime stamp = File.GetLastWriteTimeUtc(candidate);
                if (stamp > newest)
                {
                    string text = ReadSharedText(candidate);
                    if (text.Contains("music=") && text.Contains("master=") &&
                        text.Contains("effects="))
                    {
                        newest = stamp;
                        recover = text;
                    }
                }
            }
            if (recover != null)
                Queue(recover);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
    public static string ReadSharedText(string file)
    {
        using (
            var stream = new FileStream(
                file, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite |
                    FileShare.Delete)) using (var reader =
                                                  new StreamReader(stream, Encoding.UTF8,
                                                                   true)) return reader.ReadToEnd();
    }
    public void Queue(string text)
    {
        bool wasPending = HasPending;
        pending = text;
        if (!wasPending)
            retryAt = DateTime.MinValue;
    }
    void Failed(int error, string message)
    {
        LastErrorCode = error;
        LastError = message;
        failures++;
        retryAt =
            DateTime.UtcNow.AddMilliseconds(Math.Min(1000, 50 * (1 << Math.Min(failures, 5))));
    }
    public bool TryFlush(bool force)
    {
        if (!HasPending)
            return true;
        if (!force && DateTime.UtcNow < retryAt)
            return false;
        try
        {
            byte[] bytes = Encoding.ASCII.GetBytes(pending);
            using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write,
                                               FileShare.Read | FileShare.Delete))
            {
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush(true);
            }
            // ReplaceFile supports an existing destination with shared-delete readers;
            // MoveFileEx(REPLACE_EXISTING) can still return ACCESS_DENIED in that case.
            if (File.Exists(path))
                File.Replace(temp, path, null);
            else if (!MoveFileEx(temp, path, 9))
            {
                int code = Marshal.GetLastWin32Error();
                Failed(code, new Win32Exception(code).Message);
                return false;
            }
            pending = null;
            failures = 0;
            LastError = null;
            LastErrorCode = 0;
            LastCommittedWriteUtc = File.GetLastWriteTimeUtc(path);
            retryAt = DateTime.MinValue;
            return true;
        }
        catch (IOException e)
        {
            Failed(e.HResult & 0xffff, e.Message);
            return false;
        }
        catch (UnauthorizedAccessException e)
        {
            Failed(5, e.Message);
            return false;
        }
    }
}
