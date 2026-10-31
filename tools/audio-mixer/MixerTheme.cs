using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;

public sealed class MixerTheme
{
    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);
    public bool Dark;
    public Color Background, Foreground, Muted, Track, Accent;
    public static MixerTheme Current(string preview)
    {
        bool dark = false;
        try
        {
            using (var key = Registry.CurrentUser.OpenSubKey(
                       @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")) dark =
                key != null && Convert.ToInt32(key.GetValue("AppsUseLightTheme", 1)) == 0;
        }
        catch (System.Security.SecurityException)
        {
        }
        if (preview == "dark")
            dark = true;
        if (preview == "light")
            dark = false;
        if (SystemInformation.HighContrast)
            return new MixerTheme { Dark = false,
                                    Background = SystemColors.Window,
                                    Foreground = SystemColors.WindowText,
                                    Muted = SystemColors.WindowText,
                                    Track = SystemColors.GrayText,
                                    Accent = SystemColors.Highlight };
        return new MixerTheme {
            Dark = dark,
            Background = dark ? Color.FromArgb(29, 32, 36) : Color.FromArgb(248, 249, 250),
            Foreground = dark ? Color.FromArgb(239, 242, 245) : Color.FromArgb(26, 31, 36),
            Muted = dark ? Color.FromArgb(166, 178, 190) : Color.FromArgb(84, 99, 112),
            Track = dark ? Color.FromArgb(66, 76, 85) : Color.FromArgb(205, 215, 221),
            Accent = dark ? Color.FromArgb(63, 218, 196) : Color.FromArgb(0, 125, 112)
        };
    }
    public void Apply(Control control)
    {
        control.BackColor = Background;
        control.ForeColor = Foreground;
        var slider = control as MixerSlider;
        if (slider != null)
        {
            slider.TrackColor = Track;
            slider.AccentColor = Accent;
            slider.Invalidate();
        }
        var meter = control as MixerMeter;
        if (meter != null)
        {
            meter.TrackColor = Track;
            meter.AccentColor = Accent;
            meter.Invalidate();
        }
        var button = control as Button;
        if (button != null)
        {
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderColor = Track;
            button.FlatAppearance.MouseOverBackColor = Track;
        }
        foreach (Control child in control.Controls)
            Apply(child);
        var form = control as Form;
        if (form != null && form.IsHandleCreated)
        {
            int dark = Dark ? 1 : 0;
            try
            {
                if (DwmSetWindowAttribute(form.Handle, 20, ref dark, 4) != 0)
                    DwmSetWindowAttribute(form.Handle, 19, ref dark, 4);
            }
            catch (DllNotFoundException)
            {
            }
            catch (EntryPointNotFoundException)
            {
            }
        }
    }
}
