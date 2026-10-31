using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;
class LaunchMixer
{
    [STAThread]
    static void Main(string[] args)
    {
        // Serialise the startup gap before the engine becomes discoverable.
        // Repeated shortcuts/F6 already reuse the panel's per-game mutex/event.
        bool first;
        using (var instance = new Mutex(true, "Local\\JSRFAudioMixerLauncher", out first))
        {
            if (!first)
                return;
            try
            {
                Run(args);
            }
            finally
            {
                instance.ReleaseMutex();
            }
        }
    }
    static void Run(string[] args)
    {
        string root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        try
        {
            if (Process.GetProcessesByName("jsrf_recomp").Length > 0)
            {
                MessageBox.Show("Close JSRF normally before launching the audio mixer build.",
                                "JSRF Unleashed - Audio mixer");
                return;
            }
            string config = Path.Combine(root, "mixer.ini"),
                   engine = Path.Combine(root, "StageMixer", "jsrf_recomp.exe"),
                   profile = Path.Combine(root, "Profile"),
                   game = File.Exists(Path.Combine(root, "game-directory.txt"))
                              ? File.ReadAllText(Path.Combine(root, "game-directory.txt")).Trim()
                              : Path.Combine(root, "SandboxGame");
            if (!Directory.Exists(game))
                throw new DirectoryNotFoundException(
                    "Set your extracted game directory in game-directory.txt.");
            if (!File.Exists(config))
                File.WriteAllText(config, "master=100\r\nmusic=100\r\neffects=100\r\ndialogue=" +
                                          "100\r\ndj=100\r\nambience=100\r\nother=100\r\n");
            string log = Path.Combine(
                root, "Logs", DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-mixer-stderr.log");
            var start = new ProcessStartInfo(engine) {
                UseShellExecute = false, WorkingDirectory = profile, RedirectStandardOutput = true,
                RedirectStandardError = true, CreateNoWindow = true
            };
            string script = args.Length > 0 && args[0] == "--auto-test"
                                ? "40 START@1;45 START@1;50 A@1;55 A@1"
                                : "";
            var env = new Dictionary<string, string> {
                { "RECOMP_USB", "1" },
                { "RECOMP_GL", "1" },
                { "RECOMP_PB_EXEC", "1" },
                { "RECOMP_VBLANK", "1" },
                { "RECOMP_AC97_READY_NOTRAP", "1" },
                { "RECOMP_TRACE_BUDGET", "0" },
                { "RECOMP_GL_READBACK", "0" },
                { "RECOMP_FPS", "60" },
                { "RECOMP_ABI_RESTORE", "1" },
                { "RECOMP_LOCK_IDLE", "1" },
                { "RECOMP_GL_CURTAIN", "skip" },
                { "RECOMP_MUTE", "0" },
                { "RECOMP_WINDOW", "1" },
                { "RECOMP_GL_SCALE", "4" },
                { "RECOMP_FULLSCREEN", "0" },
                { "JSRF_GAME_DIR", game },
                { "JSRF_SAVE_DIR", Path.Combine(profile, "save") },
                { "JSRF_SETTINGS_FILE", Path.Combine(profile, "settings") },
                { "JSRF_MIXER_FILE", config },
                { "JSRF_MIXER_STATUS_FILE", Path.Combine(root, "mixer-status.json") },
                { "JSRF_PAD_SCRIPT", script },
                { "JSRF_VOICE_LOG", "1" },
                { "JSRF_MIXER_APP", Path.Combine(root, "JSRF-Mixer.exe") }
            };
            foreach (var item in env)
                start.EnvironmentVariables[item.Key] = item.Value;
            using (var error = new StreamWriter(log)) using (
                var output = new StreamWriter(log.Replace("-stderr", "-stdout")))
            {
                error.AutoFlush = true;
                output.AutoFlush = true;
                var p = new Process { StartInfo = start };
                p.ErrorDataReceived += (s, e) =>
                {
                    if (e.Data != null)
                        lock (error) error.WriteLine(e.Data);
                };
                p.OutputDataReceived += (s, e) =>
                {
                    if (e.Data != null)
                        lock (output) output.WriteLine(e.Data);
                };
                p.Start();
                p.BeginErrorReadLine();
                p.BeginOutputReadLine();
                var receipt =
                    new Dictionary<string, object> { { "PID", p.Id },
                                                     { "At", DateTimeOffset.Now.ToString("o") },
                                                     { "Engine", engine },
                                                     { "Game", game },
                                                     { "Settings",
                                                       Path.Combine(profile, "settings") },
                                                     { "Log", log },
                                                     { "PadScript", script },
                                                     { "MixerConfig", config } };
                File.WriteAllText(Path.Combine(root, "current-mixer-run.json"),
                                  new JavaScriptSerializer().Serialize(receipt));
                Process.Start(
                    new ProcessStartInfo(Path.Combine(root, "MixerControl.exe"),
                                         "\"" + root + "\" " + p.Id) { UseShellExecute = false });
                p.WaitForExit();
            }
        }
        catch (Exception e)
        {
            MessageBox.Show(e.Message, "JSRF Unleashed - Audio mixer");
        }
    }
}
