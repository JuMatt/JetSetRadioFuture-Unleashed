using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Windows.Forms;

// A positional fader: track clicks and drags use exactly the same mapping.
public sealed class MixerSlider : Control
{
    int value = 100;
    bool dragging;
    public Color TrackColor = Color.Gray, AccentColor = Color.Teal;
    public Color DragColor = Color.FromArgb(255, 153, 60);
    public event EventHandler ValueChanged;
    public int Value
    {
        get {
            return value;
        }
        set {
            int next = Math.Max(0, Math.Min(100, value));
            if (this.value == next)
                return;
            this.value = next;
            Invalidate();
            AccessibilityNotifyClients(AccessibleEvents.ValueChange, -1);
            if (ValueChanged != null)
                ValueChanged(this, EventArgs.Empty);
        }
    }
    public bool IsDragging
    {
        get {
            return dragging;
        }
    }
    public MixerSlider()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.Selectable,
                 true);
        TabStop = true;
        Cursor = Cursors.Hand;
        Height = 40;
        AccessibleRole = AccessibleRole.Slider;
    }
    public static int PositionToValue(int x, int width)
    {
        int margin = 12, span = Math.Max(1, width - 2 * margin);
        return Math.Max(0, Math.Min(100, (int)Math.Round((x - margin) * 100.0 / span,
                                                         MidpointRounding.AwayFromZero)));
    }
    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (e.Button != MouseButtons.Left)
            return;
        Focus();
        dragging = true;
        Capture = true;
        Value = PositionToValue(e.X, Width);
        Invalidate();
    }
    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (dragging)
            Value = PositionToValue(e.X, Width);
    }
    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (e.Button != MouseButtons.Left)
            return;
        if (dragging)
            Value = PositionToValue(e.X, Width);
        dragging = false;
        Capture = false;
        Invalidate();
    }
    protected override void OnMouseCaptureChanged(EventArgs e)
    {
        base.OnMouseCaptureChanged(e);
        if (!Capture)
        {
            dragging = false;
            Invalidate();
        }
    }
    protected override bool IsInputKey(Keys keyData)
    {
        Keys key = keyData & Keys.KeyCode;
        return key == Keys.Left || key == Keys.Right || key == Keys.Up || key == Keys.Down ||
               key == Keys.Home || key == Keys.End || base.IsInputKey(keyData);
    }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        int step = e.Shift ? 10 : 1;
        switch (e.KeyCode)
        {
        case Keys.Left:
        case Keys.Down:
            Value -= step;
            break;
        case Keys.Right:
        case Keys.Up:
            Value += step;
            break;
        case Keys.PageDown:
            Value -= 10;
            break;
        case Keys.PageUp:
            Value += 10;
            break;
        case Keys.Home:
            Value = 0;
            break;
        case Keys.End:
            Value = 100;
            break;
        default:
            base.OnKeyDown(e);
            return;
        }
        e.Handled = true;
        e.SuppressKeyPress = true;
    }
    protected override void OnGotFocus(EventArgs e)
    {
        base.OnGotFocus(e);
        Invalidate();
    }
    protected override void OnLostFocus(EventArgs e)
    {
        base.OnLostFocus(e);
        Invalidate();
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        float y = Height / 2f, x = 12 + (Width - 24) * value / 100f;
        using (var p = new Pen(TrackColor, 5))
        {
            p.StartCap = p.EndCap = LineCap.Round;
            g.DrawLine(p, 12, y, Width - 12, y);
        }
        if (value > 0)
            using (var p = new Pen(AccentColor, 5))
            {
                p.StartCap = p.EndCap = LineCap.Round;
                g.DrawLine(p, 12, y, x, y);
            }
        if (Focused)
            using (var p = new Pen(AccentColor, 1)) g.DrawEllipse(p, x - 11, y - 11, 22, 22);
        using (var b = new SolidBrush(dragging ? DragColor : AccentColor))
            g.FillEllipse(b, x - 8, y - 8, 16, 16);
    }
    protected override AccessibleObject CreateAccessibilityInstance()
    {
        return new SliderAccessibility(this);
    }
    sealed class SliderAccessibility : ControlAccessibleObject
    {
        readonly MixerSlider slider;
        public SliderAccessibility(MixerSlider owner) : base(owner)
        {
            slider = owner;
        }
        public override AccessibleRole Role
        {
            get {
                return AccessibleRole.Slider;
            }
        }
        public override string Value
        {
            get {
                return slider.Value.ToString(CultureInfo.InvariantCulture);
            }
            set {
                int n;
                if (int.TryParse(value.TrimEnd('%'), out n))
                    slider.Value = n;
            }
        }
        public override string DefaultAction
        {
            get {
                return "Adjust level";
            }
        }
        public override string Description
        {
            get {
                return "0 to 100 percent. Click or drag; arrow keys adjust by one percent.";
            }
        }
    }
}

public sealed class MixerMeter : Control
{
    double level;
    public Color TrackColor = Color.Gray, AccentColor = Color.Teal;
    public double Level
    {
        get {
            return level;
        }
        set {
            level = Math.Max(0, Math.Min(1, value));
            Invalidate();
        }
    }
    public MixerMeter()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer,
                 true);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        using (var b = new SolidBrush(TrackColor)) e.Graphics.FillRectangle(b, ClientRectangle);
        using (var b = new SolidBrush(AccentColor))
            e.Graphics.FillRectangle(b, 0, 0, (int)(Width * level), Height);
    }
}
