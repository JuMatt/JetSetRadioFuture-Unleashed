using System;
using System.IO;
using System.Drawing;
using System.Reflection;
using System.Windows.Forms;
class TestMixerUi
{
    static void Require(bool ok, string text)
    {
        if (!ok)
            throw new Exception(text);
    }
    static object Call(object o, string name, params object[] args)
    {
        return o.GetType()
            .GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)
            .Invoke(o, args);
    }
    static object Field(object o, string name)
    {
        return o.GetType()
            .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)
            .GetValue(o);
    }
    [STAThread]
    static int Main(string[] args)
    {
        try
        {
            Application.EnableVisualStyles();
            using (var slider = new MixerSlider { Width = 284, Height = 40 })
            {
                Require(MixerSlider.PositionToValue(12, 284) == 0, "Left endpoint");
                Require(MixerSlider.PositionToValue(142, 284) == 50, "Track midpoint");
                Require(MixerSlider.PositionToValue(272, 284) == 100, "Right endpoint");
                Require(MixerSlider.PositionToValue(-200, 284) == 0 &&
                            MixerSlider.PositionToValue(400, 284) == 100,
                        "Clamp outside track");
                slider.AccessibilityObject.Value = "60";
                Require(slider.Value == 60, "Accessible set value");
                Call(slider, "OnKeyDown", new KeyEventArgs(Keys.Left));
                Require(slider.Value == 59, "Keyboard fine adjustment");
                Call(slider, "OnKeyDown", new KeyEventArgs(Keys.End));
                Require(slider.Value == 100, "Keyboard End");
                Call(slider, "OnMouseDown", new MouseEventArgs(MouseButtons.Left, 1, 142, 20, 0));
                Require(slider.Value == 50 && slider.IsDragging,
                        "Track click jumps directly and starts drag");
                using (var bitmap = new Bitmap(284, 40))
                {
                    slider.DrawToBitmap(bitmap, new Rectangle(0, 0, 284, 40));
                    Require(bitmap.GetPixel(142, 20).ToArgb() == slider.DragColor.ToArgb(),
                            "Orange thumb during drag");
                }
                Call(slider, "OnMouseMove", new MouseEventArgs(MouseButtons.Left, 0, 272, 20, 0));
                Require(slider.Value == 100, "Drag follows pointer");
                Call(slider, "OnMouseUp", new MouseEventArgs(MouseButtons.Left, 1, 272, 20, 0));
                Require(!slider.IsDragging, "Drag ends on release");
            }
            string dir = Path.Combine(args[0], "ui-tests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Path.Combine(dir, "Assets"));
            File.Copy(Path.Combine(args[0], "Assets", "jsrf-mixer.ico"),
                      Path.Combine(dir, "Assets", "jsrf-mixer.ico"));
            File.WriteAllText(Path.Combine(dir, "mixer.ini"),
                              "master=80\nmusic=80\nmusic_duck=1\neffects=65\ndialogue=45\n");
            using (var mixer = new MixerControl(dir, 0, "dark"))
            {
                var sliders = (MixerSlider[])Field(mixer, "sliders");
                Require(sliders[1].Value == 20, "Legacy gain migrates once to Music fader");
                Call(mixer, "FlushConfig", true);
                Require(!File.ReadAllText(Path.Combine(dir, "mixer.ini")).Contains("music_duck"),
                        "No legacy filter persisted");
                sliders[1].Value = 100;
                Call(mixer, "QuickMusic");
                Require(sliders[1].Value == 25, "Shortcut moves Music fader to 25 percent");
                Require(sliders[0].Value == 80 && sliders[2].Value == 65 && sliders[3].Value == 45,
                        "Other groups unchanged");
                Call(mixer, "QuickMusic");
                Require(sliders[1].Value == 100, "Shortcut restores fader");
                Call(mixer, "QuickMusic");
                sliders[1].Value = 60;
                Call(mixer, "QuickMusic");
                Require(sliders[1].Value == 15, "Manual change becomes new shortcut baseline");
                Call(mixer, "QuickMusic");
                Require(sliders[1].Value == 60, "Restore new baseline");
                Call(mixer, "FlushConfig", true);
                Require(File.ReadAllText(Path.Combine(dir, "mixer.ini")).Contains("music=60"),
                        "Final fader is persisted");
            }
            if (!SystemInformation.HighContrast)
                Require(MixerTheme.Current("dark").Dark && !MixerTheme.Current("light").Dark,
                        "Both app-only theme previews");
            Console.WriteLine(
                "PASS: positional clicks/endpoints/clamping; accessible values; keyboard; drag " +
                "follows pointer; orange thumb; legacy migration; shortcut changes/restores only " +
                "Music; manual baseline; persistence; dark/light palettes. Evidence: " +
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
