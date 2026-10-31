using System;
using System.IO;
using System.Diagnostics;
using System.Threading;
using System.Runtime.InteropServices;
using System.Windows.Forms;
class OpenMixer
{
    [DllImport("user32.dll")]
    static extern bool AllowSetForegroundWindow(int pid);
    [STAThread]
    static void Main()
    {
        string root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        try
        {
            Process[] games = Process.GetProcessesByName("jsrf_recomp");
            if (games.Length == 0)
            {
                Process.Start(new ProcessStartInfo(
                    Path.Combine(root, "JSRF-Audio-Mixer.exe")) { UseShellExecute = false });
                return;
            }
            if (games.Length != 1 ||
                !string.Equals(games[0].MainModule.FileName,
                               Path.Combine(root, "StageMixer", "jsrf_recomp.exe"),
                               StringComparison.OrdinalIgnoreCase))
            {
                MessageBox.Show("Another JSRF build is running. Close it normally before opening " +
                                "this audio mixer build.",
                                "JSRF Unleashed - Audio mixer");
                return;
            }
            // The game gives this opener foreground permission. Pass it to the
            // existing companion before signalling it, so F6 also raises a hidden UI.
            foreach (Process panel in Process.GetProcessesByName("MixerControl"))
                using (panel)
                {
                    try
                    {
                        if (string.Equals(panel.MainModule.FileName,
                                          Path.Combine(root, "MixerControl.exe"),
                                          StringComparison.OrdinalIgnoreCase))
                            AllowSetForegroundWindow(panel.Id);
                    }
                    catch (InvalidOperationException)
                    {
                    }
                    catch (System.ComponentModel.Win32Exception)
                    {
                    }
                }
            try
            {
                using (EventWaitHandle open =
                           EventWaitHandle.OpenExisting("Local\\JSRFAudioMixerOpen_" + games[0].Id))
                {
                    open.Set();
                    return;
                }
            }
            catch (WaitHandleCannotBeOpenedException)
            {
            }
            Process.Start(new ProcessStartInfo(Path.Combine(root, "MixerControl.exe"),
                                               "\"" + root + "\" " +
                                                   games[0].Id) { UseShellExecute = false });
        }
        catch (Exception e)
        {
            MessageBox.Show(e.Message, "JSRF Unleashed - Audio mixer");
        }
    }
}
