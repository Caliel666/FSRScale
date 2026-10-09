using Microsoft.Win32;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Threading;

namespace NRLiveUI;

public partial class MainWindow : Window
{
    private const int ToggleHotkeyId = 0x4E52;
    private const uint ModAlt = 0x0001, ModControl = 0x0002, ModShift = 0x0004, ModWin = 0x0008, ModNoRepeat = 0x4000;
    private const int WmHotkey = 0x0312;
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private HwndSource? _source;
    private Process? _process;
    private int _secondsLeft;
    private bool _loadingProfile;
    private bool _initialized;
    private bool _registeredHotkey;
    private string _currentProfile = "Default";
    private string Root => AppContext.BaseDirectory;
    private string ProfilesDir => Path.Combine(Root, "profiles");
    private string UiConfigPath => Path.Combine(Root, "ui_config.json");
    private string ProfilePath(string name) => Path.Combine(ProfilesDir, SafeName(name) + ".json");

    private sealed class Profile
    {
        public string target_mode { get; set; } = "picker";
        public string target_text { get; set; } = "";
        public int delay { get; set; } = 5;
        public bool no_overlay { get; set; }
        public string motion { get; set; } = "fast";
        public double sharpness { get; set; } = 0.65;
        public string scale_hotkey { get; set; } = "ctrl+alt+s";
        public string stop_key { get; set; } = "ctrl+shift+a";
        public string overlay_key { get; set; } = "ctrl+home";
        public string exe_path { get; set; } = "";
    }

    public MainWindow()
    {
        InitializeComponent();
        _timer.Tick += Timer_Tick;
        TargetModeBox.SelectionChanged += (_, _) => UpdatePreview();
        MotionBox.SelectionChanged += (_, _) => UpdatePreview();
        DelayBox.TextChanged += (_, _) => UpdatePreview();
        TargetTextBox.TextChanged += (_, _) => UpdatePreview();
        StopHotkeyBox.TextChanged += (_, _) => UpdatePreview();
        OverlayHotkeyBox.TextChanged += (_, _) => UpdatePreview();
        ScaleHotkeyBox.TextChanged += (_, _) => UpdatePreview();
        ExePathBox.TextChanged += (_, _) => UpdatePreview();
        HudCheckBox.Checked += (_, _) => UpdatePreview();
        HudCheckBox.Unchecked += (_, _) => UpdatePreview();
    }

    private static string SafeName(string name)
    {
        foreach (char c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '_');
        return string.IsNullOrWhiteSpace(name) ? "Default" : name.Trim();
    }

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(ProfilesDir);
        if (!File.Exists(ProfilePath("Default")))
            File.WriteAllText(ProfilePath("Default"), JsonSerializer.Serialize(new Profile(), new JsonSerializerOptions { WriteIndented = true }));
        var names = Directory.GetFiles(ProfilesDir, "*.json").Select(Path.GetFileNameWithoutExtension).Where(x => x != null).OrderBy(x => x).ToList();
        ProfileBox.ItemsSource = names;
        string preferred = "Default";
        try
        {
            if (File.Exists(UiConfigPath))
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(UiConfigPath));
                if (doc.RootElement.TryGetProperty("last_profile", out var p)) preferred = p.GetString() ?? "Default";
                if (doc.RootElement.TryGetProperty("exe_path", out var exe) && !string.IsNullOrWhiteSpace(exe.GetString()))
                    ExePathBox.Text = exe.GetString()!;
            }
        }
        catch { }
        ProfileBox.SelectedItem = names.Contains(preferred) ? preferred : "Default";
        if (ProfileBox.SelectedItem == null) LoadProfile("Default");
        _initialized = true;
        _source = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
        _source?.AddHook(WndProc);
        RegisterToggleHotkey();
        UpdatePreview();
    }

    private void ProfileBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loadingProfile || ProfileBox.SelectedItem is not string name) return;
        if (!_initialized) { LoadProfile(name); return; }
        SaveCurrentProfile(false);
        LoadProfile(name);
    }

    private void LoadProfile(string name)
    {
        _loadingProfile = true;
        try
        {
            _currentProfile = name;
            var p = new Profile();
            try { if (File.Exists(ProfilePath(name))) p = JsonSerializer.Deserialize<Profile>(File.ReadAllText(ProfilePath(name))) ?? p; } catch { }
            TargetModeBox.SelectedIndex = p.target_mode switch { "front" => 1, "pid" => 2, "pname" => 3, "window" => 4, _ => 0 };
            TargetTextBox.Text = p.target_text;
            DelayBox.Text = Math.Clamp(p.delay, 1, 30).ToString();
            HudCheckBox.IsChecked = !p.no_overlay;
            MotionBox.SelectedIndex = p.motion == "amdof" ? 1 : 0;
            SharpnessSlider.Value = Math.Clamp(p.sharpness, 0, 1);
            ScaleHotkeyBox.Text = p.scale_hotkey;
            StopHotkeyBox.Text = p.stop_key;
            OverlayHotkeyBox.Text = p.overlay_key;
            if (!string.IsNullOrWhiteSpace(p.exe_path)) ExePathBox.Text = p.exe_path;
            ProfileTitle.Text = name == "Default" ? "Scaling setup" : name;
            SidebarHotkey.Text = p.scale_hotkey.Replace("+", " + ").ToUpperInvariant();
        }
        finally { _loadingProfile = false; }
        RegisterToggleHotkey();
        UpdatePreview();
    }

    private Profile ReadProfile() => new()
    {
        target_mode = TargetModeBox.SelectedIndex switch { 1 => "front", 2 => "pid", 3 => "pname", 4 => "window", _ => "picker" },
        target_text = TargetTextBox.Text.Trim(),
        delay = int.TryParse(DelayBox.Text, out int delay) ? Math.Clamp(delay, 1, 30) : 5,
        no_overlay = HudCheckBox.IsChecked != true,
        motion = MotionBox.SelectedIndex == 1 ? "amdof" : "fast",
        sharpness = Math.Round(SharpnessSlider.Value, 2),
        scale_hotkey = ScaleHotkeyBox.Text.Trim(),
        stop_key = StopHotkeyBox.Text.Trim(),
        overlay_key = OverlayHotkeyBox.Text.Trim(),
        exe_path = ExePathBox.Text.Trim()
    };

    private void SaveCurrentProfile(bool notify)
    {
        try
        {
            Directory.CreateDirectory(ProfilesDir);
            File.WriteAllText(ProfilePath(_currentProfile), JsonSerializer.Serialize(ReadProfile(), new JsonSerializerOptions { WriteIndented = true }));
            File.WriteAllText(UiConfigPath, JsonSerializer.Serialize(new { last_profile = _currentProfile, exe_path = ExePathBox.Text.Trim() }, new JsonSerializerOptions { WriteIndented = true }));
            if (notify) FooterStatus.Text = $"Saved profile “{_currentProfile}”.";
        }
        catch (Exception ex) { if (notify) MessageBox.Show(this, ex.Message, "Could not save profile", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    private void SaveProfile_Click(object sender, RoutedEventArgs e) => SaveCurrentProfile(true);

    private void SharpnessSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (SharpnessValue != null) SharpnessValue.Text = SharpnessSlider.Value.ToString("0.00");
        UpdatePreview();
    }

    private void BrowseExe_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "Select NRLive.exe", Filter = "NRLive executable|NRLive.exe;*.exe|All files|*.*", CheckFileExists = true };
        if (dialog.ShowDialog(this) == true) ExePathBox.Text = dialog.FileName;
    }

    private void ScaleHotkeyBox_LostFocus(object sender, RoutedEventArgs e)
    {
        RegisterToggleHotkey();
    }

    private void RegisterToggleHotkey()
    {
        if (_source == null) return;
        if (_registeredHotkey)
        {
            UnregisterHotKey(_source.Handle, ToggleHotkeyId);
            _registeredHotkey = false;
        }
        string toggle = ScaleHotkeyBox.Text.Trim();
        if (SameHotkey(toggle, StopHotkeyBox.Text) || SameHotkey(toggle, OverlayHotkeyBox.Text) || SameHotkey(StopHotkeyBox.Text, OverlayHotkeyBox.Text))
        {
            FooterStatus.Text = "Toggle hotkey conflicts with NRLive's stop or overlay key.";
            return;
        }
        if (!TryParseHotkey(toggle, out uint modifiers, out uint vk))
        {
            FooterStatus.Text = "Invalid toggle hotkey. Example: ctrl+alt+s";
            return;
        }
        _registeredHotkey = RegisterHotKey(_source.Handle, ToggleHotkeyId, modifiers | ModNoRepeat, vk);
        FooterStatus.Text = _registeredHotkey ? $"Global toggle registered: {toggle}" : $"Windows could not register {toggle}; another app may use it.";
        SidebarHotkey.Text = toggle.Replace("+", " + ").ToUpperInvariant();
    }

    private static bool SameHotkey(string a, string b) => string.Equals(NormalizeHotkey(a), NormalizeHotkey(b), StringComparison.OrdinalIgnoreCase);
    private static string NormalizeHotkey(string s) => string.Join("+", s.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(x => x.ToLowerInvariant()).OrderBy(x => x));

    private static bool TryParseHotkey(string text, out uint modifiers, out uint vk)
    {
        modifiers = 0; vk = 0;
        string key = "";
        foreach (string raw in text.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            switch (raw.ToLowerInvariant())
            {
                case "ctrl": case "control": modifiers |= ModControl; break;
                case "alt": modifiers |= ModAlt; break;
                case "shift": modifiers |= ModShift; break;
                case "win": case "windows": modifiers |= ModWin; break;
                default: if (key.Length != 0) return false; key = raw; break;
            }
        }
        if (key.Length == 1 && char.IsAsciiLetterOrDigit(key[0])) { vk = char.ToUpperInvariant(key[0]); return true; }
        if (key.Length >= 2 && (key[0] == 'f' || key[0] == 'F') && int.TryParse(key[1..], out int f) && f is >= 1 and <= 24) { vk = (uint)(0x70 + f - 1); return true; }
        vk = key.ToLowerInvariant() switch
        {
            "home" => 0x24, "end" => 0x23, "insert" => 0x2D, "delete" or "del" => 0x2E,
            "space" => 0x20, "tab" => 0x09, "enter" => 0x0D, "esc" or "escape" => 0x1B,
            "pageup" => 0x21, "pagedown" => 0x22, "up" => 0x26, "down" => 0x28, "left" => 0x25, "right" => 0x27,
            _ => 0
        };
        return vk != 0;
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmHotkey && wParam.ToInt32() == ToggleHotkeyId)
        {
            ToggleScaling();
            handled = true;
        }
        return IntPtr.Zero;
    }

    private void ScaleButton_Click(object sender, RoutedEventArgs e) => ToggleScaling();

    private void ToggleScaling()
    {
        if (_process != null && !_process.HasExited) StopScaling();
        else StartScaling();
    }

    private void StartScaling()
    {
        SaveCurrentProfile(false);
        var p = ReadProfile();
        if (SameHotkey(p.scale_hotkey, p.stop_key) || SameHotkey(p.scale_hotkey, p.overlay_key) || SameHotkey(p.stop_key, p.overlay_key))
        {
            MessageBox.Show(this, "The Scale / Unscale hotkey must differ from both the NRLive stop key and overlay key.", "Hotkey conflict", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        string exe = p.exe_path;
        if (string.IsNullOrWhiteSpace(exe) || !File.Exists(exe))
        {
            MessageBox.Show(this, "Select a valid NRLive.exe first.", "Executable missing", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (p.target_mode == "pid" && !uint.TryParse(p.target_text, out _)) { MessageBox.Show(this, "Enter a numeric process ID.", "Target", MessageBoxButton.OK, MessageBoxImage.Warning); return; }
        if ((p.target_mode == "pname" || p.target_mode == "window") && string.IsNullOrWhiteSpace(p.target_text)) { MessageBox.Show(this, "Enter a process name or window-title regex.", "Target", MessageBoxButton.OK, MessageBoxImage.Warning); return; }
        if (!TryParseHotkey(p.scale_hotkey, out _, out _) || !TryParseHotkey(p.stop_key, out _, out _) || !TryParseHotkey(p.overlay_key, out _, out _))
        {
            MessageBox.Show(this, "The NRLive stop and overlay hotkeys must be valid combinations.", "Hotkey", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var start = new ProcessStartInfo(exe) { WorkingDirectory = Path.GetDirectoryName(exe) ?? Root, UseShellExecute = false };
        if (p.target_mode == "picker") { start.ArgumentList.Add("-picker"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "front") { start.ArgumentList.Add("-front"); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "pid") { start.ArgumentList.Add("-pid"); start.ArgumentList.Add(p.target_text); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "pname") { start.ArgumentList.Add("-pname"); start.ArgumentList.Add(p.target_text); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "window") { start.ArgumentList.Add("-window"); start.ArgumentList.Add(p.target_text); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        if (p.no_overlay) start.ArgumentList.Add("-nooverlay");
        start.ArgumentList.Add("--mv"); start.ArgumentList.Add(p.motion);
        start.ArgumentList.Add("--sharpness"); start.ArgumentList.Add(p.sharpness.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
        start.ArgumentList.Add("--key"); start.ArgumentList.Add(p.stop_key);
        start.ArgumentList.Add("--overlaykey"); start.ArgumentList.Add(p.overlay_key);

        try
        {
            _process = Process.Start(start);
            if (_process == null) throw new InvalidOperationException("Windows did not start NRLive.");
            _secondsLeft = p.delay;
            ScaleButton.Content = $"Starting · {_secondsLeft}";
            ScaleButton.Background = System.Windows.Media.Brushes.DarkOrange;
            StatusText.Text = "Switch to the game now. NRLive is silently tracking the foreground window.";
            FooterStatus.Text = "Countdown running — toggle hotkey also cancels scaling.";
            _timer.Start();
            SaveCurrentProfile(false);
        }
        catch (Exception ex) { _process = null; MessageBox.Show(this, ex.Message, "Launch failed", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    private void Timer_Tick(object? sender, EventArgs e)
    {
        if (_process == null || _process.HasExited) { ResetScaling("NRLive exited before scaling became active."); return; }
        _secondsLeft--;
        if (_secondsLeft > 0) ScaleButton.Content = $"Starting · {_secondsLeft}";
        else
        {
            _timer.Stop();
            ScaleButton.Content = "Unscale  ■";
            ScaleButton.Background = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(194, 73, 91));
            StatusText.Text = "Scaling session is running. Press Unscale or the toggle hotkey to stop NRLive.";
            FooterStatus.Text = "Active — press the same toggle hotkey to unscale.";
        }
    }

    private void StopScaling()
    {
        _timer.Stop();
        try
        {
            if (_process != null && !_process.HasExited) _process.Kill(entireProcessTree: true);
        }
        catch (Exception ex) { FooterStatus.Text = $"Stop failed: {ex.Message}"; }
        finally { _process?.Dispose(); _process = null; ResetScaling("Scaling stopped."); }
    }

    private void ResetScaling(string status)
    {
        _timer.Stop();
        ScaleButton.Content = "Scale  →";
        ScaleButton.Background = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(181, 108, 255));
        StatusText.Text = "Press Scale, then switch to the game during the countdown.";
        FooterStatus.Text = status;
        if (_process?.HasExited == true) { _process.Dispose(); _process = null; }
    }

    private void UpdatePreview()
    {
        if (CommandPreview == null || _loadingProfile) return;
        var p = ReadProfile();
        string target = p.target_mode switch { "front" => "-front", "pid" => $"-pid {p.target_text}", "pname" => $"-pname \"{p.target_text}\"", "window" => $"-window \"{p.target_text}\"", _ => $"-picker {p.delay}" };
        CommandPreview.Text = $"{p.exe_path} {target} --mv {p.motion} --sharpness {p.sharpness:0.00} --key {p.stop_key} --overlaykey {p.overlay_key}";
    }

    private void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _timer.Stop();
        try { if (_process != null && !_process.HasExited) _process.Kill(entireProcessTree: true); } catch { }
        if (_registeredHotkey && _source != null) UnregisterHotKey(_source.Handle, ToggleHotkeyId);
        _source?.RemoveHook(WndProc);
        _source = null;
        SaveCurrentProfile(false);
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);
}
