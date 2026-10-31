using System;
using System.IO;
using System.Text;
using System.Drawing;
using System.Diagnostics;
using System.Globalization;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Win32;

class MixerControl : Form
{
    [DllImport("user32.dll")]
    static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint key);
    [DllImport("user32.dll")]
    static extern bool UnregisterHotKey(IntPtr h, int id);
    [DllImport("user32.dll")]
    static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")]
    static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    static extern int SetCurrentProcessExplicitAppUserModelID(string id);
    readonly string config, statusFile, preview;
    readonly int gamePid;
    readonly string[] keys = { "master", "music",    "effects", "dialogue",
                               "dj",     "ambience", "other" };
    readonly string[] names = { "Master", "Music",    "Effects / samples", "Dialogue / radio",
                                "DJ",     "Ambience", "Other streams" };
    readonly MixerSlider[] sliders = new MixerSlider[7];
    readonly CheckBox[] mutes = new CheckBox[7];
    readonly Label[] values = new Label[7], activity = new Label[7];
    readonly MixerMeter[] meters = new MixerMeter[7];
    readonly MixerConfigStore configStore;
    readonly EventWaitHandle openRequest;
    readonly System.Windows.Forms.Timer timer, saveTimer;
    Label connection, saveState, footer;
    bool loading, quitting, quickLowered;
    int quickRestore = 100;
    DateTime lastConfig;
    NotifyIcon tray;
    ToolTip tips;
    public MixerControl(string root, int pid, string themePreview)
    {
        gamePid = pid;
        preview = themePreview;
        config = Path.Combine(root, "mixer.ini");
        statusFile = Path.Combine(root, "mixer-status.json");
        configStore = new MixerConfigStore(config);
        openRequest = new EventWaitHandle(false, EventResetMode.AutoReset,
                                          "Local\\JSRFAudioMixerOpen_" + gamePid);
        Text = "JSRF Unleashed - Audio mixer";
        ClientSize = new Size(720, 590);
        Font = new Font("Segoe UI", 9.5F);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        KeyPreview = true;
        Icon = new Icon(Path.Combine(root, "Assets", "jsrf-mixer.ico"));
        Controls.Add(new Label { Text = "AUDIO MIXER",
                                 Font = new Font("Segoe UI", 16F, FontStyle.Bold),
                                 Location = new Point(22, 18), Size = new Size(500, 34) });
        Controls.Add(new Label { Text = "JET SET RADIO FUTURE  /  UNLEASHED",
                                 Location = new Point(24, 54), Size = new Size(640, 24) });
        connection = new Label { Text = "Connecting to the game...", Location = new Point(24, 91),
                                 Size = new Size(674, 27) };
        Controls.Add(connection);
        tips = new ToolTip();
        for (int i = 0; i < 7; i++)
        {
            int row = i, y = 128 + i * 52;
            Controls.Add(new Label { Text = names[i], Location = new Point(24, y + 9),
                                     Size = new Size(145, 25) });
            values[i] = new Label { Text = "100%", TextAlign = ContentAlignment.MiddleRight,
                                    Location = new Point(473, y + 6), Size = new Size(48, 27) };
            sliders[i] =
                new MixerSlider { Value = 100, Location = new Point(176, y),
                                  Size = new Size(285, 40), AccessibleName = names[i] + " volume",
                                  TabIndex = i * 2 };
            sliders[i].ValueChanged += (s, e) =>
            {
                values[row].Text = sliders[row].Value + "%";
                if (!loading)
                {
                    if (row == 1)
                        quickLowered = false;
                    SaveConfig();
                }
            };
            Controls.Add(sliders[i]);
            Controls.Add(values[i]);
            mutes[i] = new CheckBox { Text = "Mute", Location = new Point(539, y + 7),
                                      Size = new Size(65, 27), TabIndex = i * 2 + 1,
                                      AccessibleName = "Mute " + names[i] };
            mutes[i].CheckedChanged += (s, e) =>
            {
                if (!loading)
                    SaveConfig();
            };
            Controls.Add(mutes[i]);
            meters[i] = new MixerMeter { Location = new Point(620, y + 8), Size = new Size(76, 6) };
            Controls.Add(meters[i]);
            activity[i] = new Label { Text = "0 active", Location = new Point(620, y + 18),
                                      Size = new Size(80, 24), Font = new Font("Segoe UI", 8F) };
            Controls.Add(activity[i]);
        }
        tips.SetToolTip(sliders[1], "Music includes vocals already recorded into a song. Click " +
                                    "anywhere to set a level, or drag the orange thumb.");
        tips.SetToolTip(sliders[2], "Effects, sample-bank sounds and unclassified hardware " +
                                    "voices. Vocal samples remain in this group.");
        tips.SetToolTip(sliders[3], "Separate dialogue and radio streams, when present.");
        tips.SetToolTip(
            sliders[6],
            "ADX streams outside the recognised music, dialogue, DJ and ambience folders.");
        var reset = new Button { Text = "Reset mixer", Location = new Point(562, 505),
                                 Size = new Size(134, 33), TabIndex = 14 };
        reset.Click += (s, e) =>
        {
            loading = true;
            for (int i = 0; i < 7; i++)
            {
                sliders[i].Value = 100;
                mutes[i].Checked = false;
            }
            quickLowered = false;
            loading = false;
            SaveConfig();
        };
        Controls.Add(reset);
        saveState = new Label { Location = new Point(24, 505), Size = new Size(520, 35) };
        Controls.Add(saveState);
        footer = new Label {
            Text = "F6: open / hide mixer  |  Ctrl+Alt+M: lower / restore Music\nF6 returns to " +
                   "the game. Close also hides the mixer in the notification area.",
            Location = new Point(24, 550), Size = new Size(674, 38),
            Font = new Font("Segoe UI", 8.5F)
        };
        Controls.Add(footer);
        LoadConfig();
        ApplyTheme();
        tray = new NotifyIcon { Icon = Icon, Text = "JSRF Unleashed - Audio mixer",
                                Visible = preview == null };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Open audio mixer", null, (s, e) => OpenPanel());
        menu.Items.Add("Exit mixer", null,
                       (s, e) =>
                       {
                           quitting = true;
                           Close();
                       });
        tray.ContextMenuStrip = menu;
        tray.DoubleClick += (s, e) => OpenPanel();
        ApplyTheme();
        SystemEvents.UserPreferenceChanged += PreferenceChanged;
        timer = new System.Windows.Forms.Timer { Interval = 250 };
        timer.Tick += (s, e) => TickMixer();
        timer.Start();
        saveTimer = new System.Windows.Forms.Timer { Interval = 75 };
        saveTimer.Tick += (s, e) => FlushConfig(false);
        saveTimer.Start();
    }
    void OpenPanel()
    {
        Show();
        WindowState = FormWindowState.Normal;
        Activate();
    }
    void HideAndReturnToGame()
    {
        Hide();
        if (gamePid <= 0)
            return;
        try
        {
            using (var game = Process.GetProcessById(gamePid))
            {
                IntPtr window = game.MainWindowHandle;
                if (window != IntPtr.Zero)
                {
                    if (IsIconic(window))
                        ShowWindow(window, 9);
                    SetForegroundWindow(window);
                }
            }
        }
        catch (ArgumentException)
        {
        }
        catch (InvalidOperationException)
        {
        }
        catch (System.ComponentModel.Win32Exception)
        {
        }
    }
    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (keyData == Keys.F6)
        {
            // A held F6 must not immediately hide the panel it just opened.
            if ((msg.LParam.ToInt64() & (1L << 30)) == 0)
                HideAndReturnToGame();
            return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }
    void ApplyTheme()
    {
        var theme = MixerTheme.Current(preview);
        theme.Apply(this);
        connection.ForeColor = saveState.ForeColor = footer.ForeColor = theme.Muted;
        foreach (var label in activity)
            label.ForeColor = theme.Muted;
        if (tray != null)
        {
            tray.ContextMenuStrip.BackColor = theme.Background;
            tray.ContextMenuStrip.ForeColor = theme.Foreground;
            tray.ContextMenuStrip.Renderer =
                new ToolStripProfessionalRenderer(new MenuColors(theme));
        }
    }
    sealed class MenuColors : ProfessionalColorTable
    {
        readonly MixerTheme t;
        public MenuColors(MixerTheme theme)
        {
            t = theme;
            UseSystemColors = false;
        }
        public override Color ToolStripDropDownBackground
        {
            get {
                return t.Background;
            }
        }
        public override Color ImageMarginGradientBegin
        {
            get {
                return t.Background;
            }
        }
        public override Color ImageMarginGradientMiddle
        {
            get {
                return t.Background;
            }
        }
        public override Color ImageMarginGradientEnd
        {
            get {
                return t.Background;
            }
        }
        public override Color MenuItemSelected
        {
            get {
                return t.Track;
            }
        }
        public override Color MenuItemBorder
        {
            get {
                return t.Accent;
            }
        }
    }
    void PreferenceChanged(object s, UserPreferenceChangedEventArgs e)
    {
        if (!IsDisposed && IsHandleCreated)
            try
            {
                BeginInvoke((Action)ApplyTheme);
            }
            catch (InvalidOperationException)
            {
            }
    }
    void LoadConfig()
    {
        bool legacyDuck = false;
        loading = true;
        try
        {
            string contents =
                configStore.HasPending
                    ? configStore.PendingText
                    : (File.Exists(config) ? MixerConfigStore.ReadSharedText(config) : "");
            foreach (string line in contents.Split(new[] { '\r', '\n' },
                                                   StringSplitOptions.RemoveEmptyEntries))
            {
                string[] a = line.Split('=');
                int value;
                if (a.Length != 2 || !int.TryParse(a[1], out value))
                    continue;
                if (a[0] == "music_duck")
                    legacyDuck = value != 0;
                for (int i = 0; i < 7; i++)
                {
                    if (a[0] == keys[i] && value >= 0 && value <= 100)
                        sliders[i].Value = value;
                    if (a[0] == keys[i] + "_mute")
                        mutes[i].Checked = value != 0;
                }
            }
            if (legacyDuck)
                sliders[1].Value =
                    (int)Math.Round(sliders[1].Value * 0.25, MidpointRounding.AwayFromZero);
            lastConfig = File.Exists(config) ? File.GetLastWriteTimeUtc(config) : DateTime.MinValue;
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        finally
        {
            loading = false;
        }
        quickLowered = false;
        if (legacyDuck)
            SaveConfig();
        RefreshSaveState();
    }
    void RefreshSaveState()
    {
        saveState.Text =
            configStore.HasPending
                ? (configStore.LastError != null ? "Settings pending. Retrying automatically..."
                                                 : "Saving settings...")
                : "Levels saved automatically. Click a track or drag its thumb.";
    }
    void SaveConfig()
    {
        var text = new StringBuilder();
        for (int i = 0; i < 7; i++)
        {
            text.AppendLine(keys[i] + "=" + sliders[i].Value);
            text.AppendLine(keys[i] + "_mute=" + (mutes[i].Checked ? 1 : 0));
        }
        configStore.Queue(text.ToString());
        RefreshSaveState();
    }
    void FlushConfig(bool force)
    {
        if (!configStore.HasPending)
            return;
        if (configStore.TryFlush(force))
            lastConfig = configStore.LastCommittedWriteUtc;
        RefreshSaveState();
    }
    void QuickMusic()
    {
        int next;
        if (quickLowered)
            next = quickRestore;
        else
        {
            quickRestore = sliders[1].Value;
            next = (int)Math.Round(quickRestore * 0.25, MidpointRounding.AwayFromZero);
        }
        bool lower = !quickLowered;
        loading = true;
        sliders[1].Value = next;
        loading = false;
        quickLowered = lower;
        SaveConfig();
    }
    void TickMixer()
    {
        if (openRequest.WaitOne(0))
            OpenPanel();
        if (gamePid > 0)
        {
            try
            {
                using (var p = Process.GetProcessById(gamePid)) if (p.HasExited)
                {
                    quitting = true;
                    Close();
                    return;
                }
            }
            catch (ArgumentException)
            {
                quitting = true;
                Close();
                return;
            }
        }
        try
        {
            if (!configStore.HasPending && File.Exists(config) &&
                File.GetLastWriteTimeUtc(config) != lastConfig)
                LoadConfig();
            if (!File.Exists(statusFile) ||
                (DateTime.UtcNow - File.GetLastWriteTimeUtc(statusFile)).TotalSeconds > 3)
            {
                connection.Text = "Waiting for the game. Your levels are kept.";
                for (int i = 0; i < 7; i++)
                {
                    meters[i].Level = 0;
                    activity[i].Text = "0 active";
                }
                return;
            }
            MixerStatusGroup[] snapshot;
            if (!MixerStatusReader.TryRead(statusFile, out snapshot))
                return;
            foreach (var group in snapshot)
            {
                int i = Array.IndexOf(keys, group.Key);
                if (i < 0)
                    continue;
                activity[i].Text = group.Active + " active";
                meters[i].Level = group.Peak;
            }
            connection.Text =
                "Connected to JSRF. Source groups become active as the game plays them.";
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
        catch (KeyNotFoundException)
        {
        }
    }
    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (preview == null)
            RegisterHotKey(Handle, 9147, 0x4003, 0x4D);
        ApplyTheme();
    }
    protected override void WndProc(ref Message m)
    {
        if (m.Msg == 0x312 && m.WParam.ToInt32() == 9147)
            QuickMusic();
        base.WndProc(ref m);
    }
    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (!quitting && preview == null && e.CloseReason == CloseReason.UserClosing)
        {
            e.Cancel = true;
            Hide();
            return;
        }
        base.OnFormClosing(e);
    }
    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        timer.Stop();
        saveTimer.Stop();
        FlushConfig(true);
        SystemEvents.UserPreferenceChanged -= PreferenceChanged;
        UnregisterHotKey(Handle, 9147);
        tray.Visible = false;
        tray.Dispose();
        tips.Dispose();
        timer.Dispose();
        saveTimer.Dispose();
        openRequest.Dispose();
        base.OnFormClosed(e);
    }
    [STAThread]
    static void Main(string[] args)
    {
        SetCurrentProcessExplicitAppUserModelID("JSRF.Unleashed.AudioMixer");
        int pid = 0;
        string preview = null;
        foreach (string arg in args)
        {
            int n;
            if (int.TryParse(arg, out n))
                pid = n;
            if (arg == "--preview-dark")
                preview = "dark";
            if (arg == "--preview-light")
                preview = "light";
        }
        bool first;
        using (var mutex =
                   new Mutex(true, "Local\\JSRFAudioMixerPanel_" + pid + "_" + preview, out first))
        {
            if (!first)
            {
                try
                {
                    using (var signal = EventWaitHandle.OpenExisting("Local\\JSRFAudioMixerOpen_" +
                                                                     pid)) signal.Set();
                }
                catch (WaitHandleCannotBeOpenedException)
                {
                }
                return;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MixerControl(AppDomain.CurrentDomain.BaseDirectory, pid, preview));
        }
    }
}
