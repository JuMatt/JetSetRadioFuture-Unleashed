using System;
using System.IO;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Web.Script.Serialization;

public sealed class MixerStatusGroup
{
    public string Key;
    public int Active;
    public double Peak;
}

// The game rewrites telemetry in place. A poll can see an empty or partial
// document; validate the complete snapshot before touching any UI control.
public static class MixerStatusReader
{
    static readonly string[] keys = { "master", "music",    "effects", "dialogue",
                                      "dj",     "ambience", "other" };
    public static bool TryRead(string file, out MixerStatusGroup[] snapshot)
    {
        snapshot = null;
        try
        {
            var data = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(
                MixerConfigStore.ReadSharedText(file));
            object raw;
            if (data == null || !data.TryGetValue("groups", out raw))
                return false;
            var groups = raw as IEnumerable;
            if (groups == null || raw is string)
                return false;
            var result = new List<MixerStatusGroup>();
            var seen = new HashSet<string>();
            foreach (object item in groups)
            {
                var group = item as Dictionary<string, object>;
                object key, active, peak;
                if (group == null || !group.TryGetValue("key", out key) ||
                    !group.TryGetValue("active", out active) ||
                    !group.TryGetValue("peak", out peak))
                    return false;
                string name = key as string;
                if (name == null || Array.IndexOf(keys, name) < 0 || !seen.Add(name) ||
                    active == null || peak == null)
                    return false;
                int count = Convert.ToInt32(active, CultureInfo.InvariantCulture);
                double level = Convert.ToDouble(peak, CultureInfo.InvariantCulture);
                if (count < 0 || count > 256 || double.IsNaN(level) || double.IsInfinity(level) ||
                    level < 0)
                    return false;
                result.Add(new MixerStatusGroup { Key = name, Active = count, Peak = level });
            }
            if (seen.Count != keys.Length)
                return false;
            snapshot = result.ToArray();
            return true;
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        catch (ArgumentException)
        {
        }
        catch (InvalidOperationException)
        {
        }
        catch (FormatException)
        {
        }
        catch (OverflowException)
        {
        }
        catch (InvalidCastException)
        {
        }
        return false;
    }
}
