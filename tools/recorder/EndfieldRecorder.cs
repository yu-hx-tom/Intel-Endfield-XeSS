using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

internal static class Program
{
    [DllImport("user32.dll")]
    private static extern bool SetProcessDPIAware();

    [STAThread]
    private static int Main(string[] args)
    {
        bool selfTest = args.Length > 0 && args[0] == "--self-test";
        if ((!selfTest && args.Length != 0) || (selfTest && args.Length > 2))
        {
            MessageBox.Show("用法：EndfieldRecorder.exe [--self-test [输出目录]]", "终末地性能录制器");
            return 2;
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        SetProcessDPIAware();
        string executable = Application.ExecutablePath;
        if (!selfTest && !new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
        {
            try
            {
                Process.Start(new ProcessStartInfo(executable) { UseShellExecute = true, Verb = "runas" });
                return 0;
            }
            catch (Win32Exception ex)
            {
                MessageBox.Show(ex.NativeErrorCode == 1223 ? "已取消管理员授权。再次打开即可重试。" : "无法启动录制器：" + ex.Message,
                    "终末地性能录制器", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return 1;
            }
        }

        try
        {
            bool createdNew;
            using (System.Threading.Mutex singleInstance = new System.Threading.Mutex(true, @"Local\EndfieldRecorder", out createdNew))
            {
                if (!createdNew)
                {
                    MessageBox.Show("录制器已经打开，请使用现有窗口。", "终末地性能录制器");
                    return 0;
                }
                try
                {
                    string appDirectory = Path.GetDirectoryName(executable);
                    string capturesDirectory = selfTest && args.Length == 2
                        ? Path.GetFullPath(args[1]) : Path.Combine(appDirectory, "Captures");
                    using (RecorderForm form = new RecorderForm(appDirectory, capturesDirectory, selfTest))
                    {
                        Application.Run(form);
                        return form.ExitCode;
                    }
                }
                finally
                {
                    singleInstance.ReleaseMutex();
                }
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show("无法打开录制器：" + ex.Message, "终末地性能录制器", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    internal static string QuoteArgument(string value)
    {
        StringBuilder quoted = new StringBuilder("\"");
        int slashes = 0;
        foreach (char character in value)
        {
            if (character == '\\')
                slashes++;
            else
            {
                quoted.Append('\\', character == '"' ? slashes * 2 + 1 : slashes);
                quoted.Append(character);
                slashes = 0;
            }
        }
        quoted.Append('\\', slashes * 2);
        return quoted.Append('"').ToString();
    }
}

internal sealed class RecorderForm : Form
{
    private readonly string appDirectory;
    private readonly string capturesDirectory;
    private readonly bool selfTest;
    private readonly int ownerPid;
    private readonly long ownerCreationFileTime;
    private readonly Button startButton = new Button();
    private readonly Button stopButton = new Button();
    private readonly Button openButton = new Button();
    private readonly Label elapsedLabel = new Label();
    private readonly Label statusLabel = new Label();
    private readonly Label settingsWarningLabel = new Label();
    private readonly TextBox outputText = new TextBox();
    private readonly ComboBox presetBox = new ComboBox();
    private readonly ComboBox labelBox = new ComboBox();
    private readonly TextBox notesBox = new TextBox();
    private readonly ComboBox xiaomiBox = new ComboBox();
    private readonly CheckBox menuKeysCheck = new CheckBox();
    private readonly CheckBox detailCheck = new CheckBox();
    private readonly ComboBox routeBox = new ComboBox();
    private readonly Button routeButton = new Button();
    private readonly Label routeLabel = new Label();
    private readonly RecorderRouteSwitcher routeSwitcher;
    private const bool SharedRouteSupported = true;
    private const string SharedRouteRequirement = "同步 + 共享须安装支持该路线的新版；安装版本未确认，实际生效以测量流为准。";
    private static readonly string[] LabelCodes = { "sync", "k2", "k1", "sync-shared", "k2-depth", "t4-observe", "t4-phase1", "official", "freeplay" };
    private static readonly string[] LabelNames = { "同步", "K2", "K1", "同步 + 共享", "K2 + T3.3", "T4 观察", "T4 阶段 1", "官方对照", "自由游玩" };
    private static readonly string[] RouteCodes = { "sync", "k2", "k1", "sync-shared" };
    private readonly Dictionary<string, CheckBox> collectorChecks = new Dictionary<string, CheckBox>();
    private readonly string settingsPath;
    private readonly Timer timer = new Timer();
    private static readonly string[] CollectorKeys = new string[]
    {
        "presentmonRaw", "pmdp", "rtss", "hardware", "foreground", "system", "threads", "measurements", "logs"
    };
    private static readonly string[] PresetKeys = new string[] { "performance", "stutter", "stability", "custom" };
    private bool applyingSettings;
    private int selfTestOptionChecks;
    private Dictionary<string, bool> selfTestRequestedCollectors;
    private Process worker;
    private string outputDirectory;
    private StatusSnapshot lastStatus;
    private bool busy;
    private bool polling;
    private bool outputCreated;
    private bool stopRequested;
    private bool closeAfterSave;
    private bool selfTestSawRecording;
    private bool selfTestClickedStop;
    private bool selfTestFinishing;
    private string selfTestError;
    private DateTime selfTestDeadline;
    private DateTime selfTestStopAt;
    private DateTime? exitObservedAt;
    private long stopQpcTicks;
    private string stopUtc;
    private int? childExitCode;
    private long workerCreationFileTime;
    private RecorderMarkerWriter markerWriter;
    private RecorderInputCapture markerInput;
    private Task markerCloseTask;
    private bool markersStarted;
    private string markerWarning;
    private bool routePolling, routeSwitchBusy, routeGameRunning;
    private DateTime nextRoutePoll;

    internal int ExitCode { get; private set; }

    internal RecorderForm(string appDirectory, string capturesDirectory, bool selfTest)
    {
        this.appDirectory = appDirectory;
        this.capturesDirectory = capturesDirectory;
        this.selfTest = selfTest;
        settingsPath = selfTest ? null : Path.Combine(appDirectory, "recorder-settings.json");
        routeSwitcher = selfTest
            ? RecorderRouteSwitcher.ForTests(Path.Combine(capturesDirectory, "TEST-ONLY-route-switches"),
                Path.Combine(capturesDirectory, "TEST-ONLY-route-switch.jsonl"), delegate { return false; }, SharedRouteSupported)
            : RecorderRouteSwitcher.ForMachine(Path.Combine(capturesDirectory, "route-switch.jsonl"), SharedRouteSupported);
        using (Process current = Process.GetCurrentProcess())
        {
            ownerPid = current.Id;
            ownerCreationFileTime = current.StartTime.ToUniversalTime().ToFileTimeUtc();
        }

        Text = selfTest ? "终末地性能录制器 · 模拟自检" : "终末地性能录制器";
        Font = new Font("Microsoft YaHei UI", 10F);
        ClientSize = new Size(800, 850);
        MinimumSize = new Size(540, 440);
        ShowIcon = false;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = SystemColors.Window;

        TableLayoutPanel layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(14), ColumnCount = 1, RowCount = 10
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int row = 0; row < 10; row++)
            layout.RowStyles.Add(new RowStyle(row == 2 ? SizeType.Percent : SizeType.AutoSize, row == 2 ? 100 : 0));
        Label heading = new Label
        {
            Text = "游戏内手动录制", Dock = DockStyle.Fill, AutoSize = true,
            Font = new Font(Font.FontFamily, 14F, FontStyle.Bold), Margin = new Padding(0, 0, 0, 8)
        };
        Label instructions = new Label
        {
            Text = "先进入游戏并停在需要测试的场景，再点击“开始录制”。\r\n结束后点击“停止并保存”，等待文件保存完成。",
            Dock = DockStyle.Fill, AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 0, 0, 8)
        };
        Panel optionsViewport = new Panel { Dock = DockStyle.Fill, AutoScroll = true, Margin = new Padding(0, 0, 0, 8) };
        TableLayoutPanel options = new TableLayoutPanel
        {
            Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1, RowCount = 5, Padding = new Padding(0, 0, SystemInformation.VerticalScrollBarWidth, 0)
        };
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int row = 0; row < 5; row++) options.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        FlowLayoutPanel presets = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        presets.Controls.Add(new Label { Text = "采集预设", AutoSize = true, Margin = new Padding(0, 6, 12, 0) });
        presetBox.DropDownStyle = ComboBoxStyle.DropDownList;
        presetBox.Width = 210;
        presetBox.AccessibleName = "采集预设";
        presetBox.Items.AddRange(new object[] { "常用性能", "卡顿排查（全部）", "长时间评估（汇总）", "自定义" });
        presets.Controls.Add(presetBox);
        options.Controls.Add(presets, 0, 0);
        options.Controls.Add(CreateCaptureDetails(), 0, 1);
        options.Controls.Add(CreateMarkerOptions(), 0, 2);
        options.Controls.Add(CreateCollectorGroup("帧数据源", new string[]
        {
            "presentmonRaw", "独立 PresentMon：完整帧与延迟",
            "pmdp", "现有 PMDP：上屏帧",
            "rtss", "RTSS：提交帧间隔"
        }), 0, 3);
        options.Controls.Add(CreateCollectorGroup("状态与诊断", new string[]
        {
            "hardware", "硬件：功耗、温度、频率、显存",
            "foreground", "前台与窗口状态",
            "system", "后台 CPU、磁盘、DPC、服务",
            "threads", "游戏线程起止快照",
            "measurements", "测量流：窗口汇总 / 等待 / Present / GPU 时间 / 复制计数",
            "logs", "Observer / EFU 诊断日志"
        }), 0, 4);
        optionsViewport.Controls.Add(options);
        Label diagnosticNote = new Label
        {
            Text = "每次均记录电源状态和首尾配置快照。长时间评估默认只拷贝 60 秒测量汇总；缺失数据会标注。",
            Dock = DockStyle.Fill, AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 0, 0, 8)
        };
        settingsWarningLabel.AutoSize = true;
        settingsWarningLabel.Dock = DockStyle.Fill;
        settingsWarningLabel.ForeColor = Color.DarkOrange;
        settingsWarningLabel.UseMnemonic = false;
        settingsWarningLabel.Margin = new Padding(0, 0, 0, 8);
        settingsWarningLabel.Visible = false;
        FlowLayoutPanel buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        startButton.Text = "开始录制";
        stopButton.Text = "停止并保存";
        openButton.Text = "打开输出文件夹";
        foreach (Button button in new Button[] { startButton, stopButton, openButton })
        {
            button.Size = new Size(button == openButton ? 176 : 144, 36);
            button.Margin = new Padding(0, 0, 10, 0);
            buttons.Controls.Add(button);
        }
        elapsedLabel.Text = "已录制 00:00.0";
        elapsedLabel.Dock = DockStyle.Fill;
        elapsedLabel.AutoSize = true;
        elapsedLabel.Margin = new Padding(0, 5, 0, 5);
        statusLabel.Text = "就绪：进入游戏后点击“开始录制”。";
        statusLabel.Dock = DockStyle.Fill;
        statusLabel.AutoSize = true;
        statusLabel.UseMnemonic = false;
        statusLabel.Margin = new Padding(0, 0, 0, 8);
        outputText.ReadOnly = true;
        outputText.Dock = DockStyle.Fill;
        outputText.Text = capturesDirectory;
        outputText.AccessibleName = "输出目录";
        layout.Controls.Add(heading, 0, 0);
        layout.Controls.Add(instructions, 0, 1);
        layout.Controls.Add(optionsViewport, 0, 2);
        layout.Controls.Add(CreateRouteOptions(), 0, 3);
        layout.Controls.Add(diagnosticNote, 0, 4);
        layout.Controls.Add(settingsWarningLabel, 0, 5);
        layout.Controls.Add(buttons, 0, 6);
        layout.Controls.Add(elapsedLabel, 0, 7);
        layout.Controls.Add(statusLabel, 0, 8);
        layout.Controls.Add(outputText, 0, 9);
        Controls.Add(layout);
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;

        string settingsWarning;
        RecorderSettings settings = LoadSettings(settingsPath, out settingsWarning);
        ApplySettings(settings);
        ShowSettingsWarning(settingsWarning);
        presetBox.SelectedIndexChanged += PresetChanged;
        startButton.Click += StartClicked;
        stopButton.Click += delegate { RequestStop(); };
        openButton.Click += OpenOutputClicked;
        FormClosing += ClosingWhileBusy;
        timer.Interval = 200;
        timer.Tick += PollTick;
        Shown += delegate
        {
            Rectangle workArea = Screen.FromControl(this).WorkingArea;
            MinimumSize = new Size(Math.Min(MinimumSize.Width, workArea.Width - 24), Math.Min(MinimumSize.Height, workArea.Height - 24));
            Size = new Size(Math.Min(Width, workArea.Width - 24), Math.Min(Height, workArea.Height - 24));
            Location = new Point(workArea.Left + (workArea.Width - Width) / 2, workArea.Top + (workArea.Height - Height) / 2);
            timer.Start();
            if (selfTest)
            {
                selfTestDeadline = DateTime.UtcNow.AddSeconds(90);
                BeginInvoke((MethodInvoker)delegate { RunSelectionSelfTests(); startButton.PerformClick(); });
            }
        };
        UpdateButtons();
    }

    private GroupBox CreateCollectorGroup(string title, string[] entries)
    {
        GroupBox group = new GroupBox
        {
            Text = title, Dock = DockStyle.Fill, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Padding = new Padding(12, 28, 12, 8), Margin = new Padding(0, 8, 0, 0)
        };
        TableLayoutPanel rows = new TableLayoutPanel
        {
            Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1, RowCount = entries.Length / 2
        };
        rows.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int index = 0; index < entries.Length; index += 2)
        {
            CheckBox check = new CheckBox
            {
                Text = entries[index + 1], AutoSize = true, Dock = DockStyle.Fill,
                UseMnemonic = false, Margin = new Padding(0, 0, 0, 6)
            };
            check.CheckedChanged += CollectorChanged;
            collectorChecks.Add(entries[index], check);
            rows.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            rows.Controls.Add(check, 0, index / 2);
        }
        group.Controls.Add(rows);
        return group;
    }

    private Control CreateCaptureDetails()
    {
        FlowLayoutPanel panel = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true, Margin = new Padding(0, 8, 0, 0) };
        panel.Controls.Add(new Label { Text = "本局标签", AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
        labelBox.DropDownStyle = ComboBoxStyle.DropDownList;
        labelBox.Width = 160;
        labelBox.AccessibleName = "本局标签";
        labelBox.Items.AddRange(LabelNames);
        labelBox.SelectedIndex = 8;
        panel.Controls.Add(labelBox);
        panel.Controls.Add(new Label { Text = "备注（可选）", AutoSize = true, Margin = new Padding(12, 6, 8, 0) });
        notesBox.Width = 280;
        notesBox.MaxLength = 512;
        notesBox.AccessibleName = "本局备注";
        panel.Controls.Add(notesBox);
        panel.SetFlowBreak(notesBox, true);
        panel.Controls.Add(new Label { Text = "小米性能模式（用户自选）", AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
        xiaomiBox.DropDownStyle = ComboBoxStyle.DropDownList;
        xiaomiBox.Items.AddRange(new object[] { "狂暴", "均衡", "安静", "智能", "不清楚" });
        xiaomiBox.SelectedIndex = 4;
        xiaomiBox.Width = 120;
        xiaomiBox.AccessibleName = "小米性能模式，用户自选";
        panel.Controls.Add(xiaomiBox);
        return panel;
    }

    private Control CreateMarkerOptions()
    {
        FlowLayoutPanel panel = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, FlowDirection = FlowDirection.TopDown,
            WrapContents = false, Margin = new Padding(0, 8, 0, 0) };
        panel.Controls.Add(new Label { Text = "录制中 Ctrl+Shift+F9 打点并短响；仅游戏在前台时记录。", AutoSize = true, UseMnemonic = false });
        menuKeysCheck.Text = "可选：只记录游戏前台的 M / Esc 按下时刻（不判断菜单状态）";
        menuKeysCheck.AutoSize = true;
        menuKeysCheck.UseMnemonic = false;
        panel.Controls.Add(menuKeysCheck);
        detailCheck.Text = "保留逐帧测量明细（体积较大；长时间评估默认关闭）";
        detailCheck.Checked = true;
        detailCheck.AutoSize = true;
        detailCheck.UseMnemonic = false;
        panel.Controls.Add(detailCheck);
        return panel;
    }

    private Control CreateRouteOptions()
    {
        FlowLayoutPanel panel = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true, Margin = new Padding(0, 8, 0, 0) };
        panel.Controls.Add(new Label { Text = "下一局用", AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
        routeBox.DropDownStyle = ComboBoxStyle.DropDownList;
        routeBox.Width = 225;
        routeBox.AccessibleName = "下一局路线";
        routeBox.Items.AddRange(new object[] { "同步", "K2", "K1", SharedRouteSupported ? "同步 + 共享（须安装新版）" : "同步 + 共享（暂不可用）" });
        routeBox.SelectedIndex = 0;
        routeBox.SelectedIndexChanged += delegate { UpdateButtons(); };
        panel.Controls.Add(routeBox);
        routeButton.Text = "应用到下一局";
        routeButton.AutoSize = true;
        routeButton.Click += SwitchRouteClicked;
        panel.Controls.Add(routeButton);
        panel.SetFlowBreak(routeButton, true);
        routeLabel.Text = "当前开关请求下一局使用：正在读取…";
        routeLabel.AutoSize = true;
        routeLabel.UseMnemonic = false;
        routeLabel.MaximumSize = new Size(700, 0);
        panel.Controls.Add(routeLabel);
        panel.SetFlowBreak(routeLabel, true);
        panel.Controls.Add(new Label { Text = SharedRouteRequirement, AutoSize = true, MaximumSize = new Size(700, 0), UseMnemonic = false });
        return panel;
    }

    private async void SwitchRouteClicked(object sender, EventArgs args)
    {
        if (busy || routeSwitchBusy || routeBox.SelectedIndex < 0) return;
        string route = RouteCodes[routeBox.SelectedIndex];
        routeSwitchBusy = true;
        UpdateButtons();
        try
        {
            await Task.Run(delegate { routeSwitcher.Switch(route); });
            statusLabel.ForeColor = Color.DarkGreen;
            statusLabel.Text = route == "sync-shared" ? "已保存下一局开关请求。" + SharedRouteRequirement
                : "已保存下一局路线开关。开关在下次启动游戏时读取，实际生效以测量流为准。";
        }
        catch (Exception ex)
        {
            statusLabel.ForeColor = Color.Firebrick;
            statusLabel.Text = "路线未能应用：" + ex.Message;
        }
        finally { routeSwitchBusy = false; nextRoutePoll = DateTime.MinValue; UpdateButtons(); RefreshRouteState(); }
    }

    private async void RefreshRouteState()
    {
        if (routePolling || routeSwitchBusy || DateTime.UtcNow < nextRoutePoll) return;
        routePolling = true;
        nextRoutePoll = DateTime.UtcNow.AddSeconds(1);
        try
        {
            string current = await Task.Run(delegate { return routeSwitcher.CurrentRoute(); });
            routeGameRunning = !selfTest && RecorderRouteSwitcher.AnyGameRunning();
            if (!IsDisposed) routeLabel.Text = "当前开关请求下一局使用：" + current
                + (routeGameRunning ? "。Endfield.exe 正在运行，不能修改。" : "。仅在下次启动游戏时生效。");
        }
        catch (Exception ex) { if (!IsDisposed) routeLabel.Text = "无法安全读取下一局路线：" + ex.Message; }
        finally { routePolling = false; if (!IsDisposed) UpdateButtons(); }
    }

    private static Dictionary<string, bool> PresetCollectors(string preset)
    {
        Dictionary<string, bool> values = new Dictionary<string, bool>();
        foreach (string key in CollectorKeys)
            values.Add(key, preset == "stutter" || (preset == "stability"
                ? key == "pmdp" || key == "hardware" || key == "foreground" || key == "measurements"
                : key == "pmdp" || key == "rtss" || key == "hardware" || key == "foreground" || key == "measurements"));
        return values;
    }

    private Dictionary<string, bool> SelectedCollectors()
    {
        Dictionary<string, bool> values = new Dictionary<string, bool>();
        foreach (string key in CollectorKeys) values.Add(key, collectorChecks[key].Checked);
        return values;
    }

    private static bool AnyCollectorSelected(Dictionary<string, bool> values)
    {
        foreach (bool selected in values.Values) if (selected) return true;
        return false;
    }

    private void ApplySettings(RecorderSettings settings)
    {
        applyingSettings = true;
        try
        {
            foreach (string key in CollectorKeys) collectorChecks[key].Checked = settings.Collectors[key];
            presetBox.SelectedIndex = Array.IndexOf(PresetKeys, settings.Preset);
            if (settings.Preset != "custom") detailCheck.Checked = settings.Preset != "stability";
        }
        finally { applyingSettings = false; }
    }

    private void PresetChanged(object sender, EventArgs args)
    {
        if (applyingSettings || busy || presetBox.SelectedIndex < 0) return;
        string preset = PresetKeys[presetBox.SelectedIndex];
        if (preset != "custom")
            ApplySettings(new RecorderSettings { Preset = preset, Collectors = PresetCollectors(preset) });
        PersistSettings();
    }

    private void CollectorChanged(object sender, EventArgs args)
    {
        if (applyingSettings || busy) return;
        applyingSettings = true;
        try { presetBox.SelectedIndex = 3; }
        finally { applyingSettings = false; }
        PersistSettings();
    }

    private void PersistSettings()
    {
        if (settingsPath == null) return;
        try
        {
            SaveSettings(settingsPath, PresetKeys[presetBox.SelectedIndex], SelectedCollectors());
            ShowSettingsWarning("");
        }
        catch (Exception ex)
        {
            ShowSettingsWarning("无法保存设置，本次仍按当前勾选录制：" + ex.Message);
        }
    }

    private void ShowSettingsWarning(string warning)
    {
        settingsWarningLabel.Text = warning;
        settingsWarningLabel.Visible = !String.IsNullOrEmpty(warning);
    }

    private static RecorderSettings LoadSettings(string path, out string warning)
    {
        warning = "";
        try
        {
            if (path != null && File.Exists(path))
            {
                if (new FileInfo(path).Length > 65536) throw new InvalidDataException("设置文件过大。");
                Dictionary<string, object> values = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(path, Encoding.UTF8));
                object version, preset, collectors;
                if (values == null || !values.TryGetValue("version", out version) || !(version is int) || (int)version != 1
                    || !values.TryGetValue("preset", out preset) || !(preset is string) || Array.IndexOf(PresetKeys, (string)preset) < 0
                    || !values.TryGetValue("collectors", out collectors) || !(collectors is Dictionary<string, object>))
                    throw new InvalidDataException("设置格式不受支持。");
                Dictionary<string, bool> selected = ParseCollectors(collectors);
                string selectedPreset = (string)preset;
                if (selectedPreset != "custom")
                {
                    Dictionary<string, bool> expected = PresetCollectors(selectedPreset);
                    foreach (string key in CollectorKeys)
                        if (selected[key] != expected[key]) throw new InvalidDataException("预设与勾选项不一致。");
                }
                return new RecorderSettings { Preset = selectedPreset, Collectors = selected };
            }
        }
        catch (Exception)
        {
            warning = "设置文件无法读取或格式已旧 / 损坏，已恢复“常用性能”默认项。";
        }
        return new RecorderSettings { Preset = "performance", Collectors = PresetCollectors("performance") };
    }

    private static Dictionary<string, bool> ParseCollectors(object value)
    {
        Dictionary<string, object> stored = value as Dictionary<string, object>;
        if (stored == null || stored.Count != CollectorKeys.Length) throw new InvalidDataException("采集项不完整。");
        Dictionary<string, bool> selected = new Dictionary<string, bool>();
        foreach (string key in CollectorKeys)
        {
            object item;
            if (!stored.TryGetValue(key, out item) || !(item is bool))
                throw new InvalidDataException("采集项必须是布尔值。");
            selected.Add(key, (bool)item);
        }
        return selected;
    }

    private static void SaveSettings(string path, string preset, Dictionary<string, bool> collectors)
    {
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, new JavaScriptSerializer().Serialize(new Dictionary<string, object>
            {
                { "version", 1 }, { "preset", preset }, { "collectors", collectors }
            }), new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temporary, path, null);
            else File.Move(temporary, path);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private async void StartClicked(object sender, EventArgs args)
    {
        if (busy || selfTestFinishing)
            return;
        Dictionary<string, bool> collectors = SelectedCollectors();
        if (!AnyCollectorSelected(collectors))
        {
            statusLabel.ForeColor = Color.Firebrick;
            statusLabel.Text = "请至少勾选一项采集内容，再开始录制。";
            return;
        }
        PersistSettings();
        if (selfTest) selfTestRequestedCollectors = new Dictionary<string, bool>(collectors);
        DateTime requestedUtc = DateTime.UtcNow;
        long requestedQpc = Stopwatch.GetTimestamp();
        string labelCode = LabelCodes[labelBox.SelectedIndex];
        string label = LabelNames[labelBox.SelectedIndex];
        string notes = notesBox.Text;
        string xiaomiMode = (string)xiaomiBox.SelectedItem;
        bool menuKeys = menuKeysCheck.Checked;
        string measurementMode = detailCheck.Checked ? "full" : "summary";
        outputDirectory = Path.Combine(capturesDirectory,
            requestedUtc.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture) + "-" + labelCode + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        outputText.Text = outputDirectory;
        busy = true;
        outputCreated = false;
        stopRequested = false;
        stopQpcTicks = 0;
        childExitCode = null;
        lastStatus = null;
        exitObservedAt = null;
        markersStarted = false;
        markerWriter = null;
        markerInput = null;
        markerCloseTask = null;
        markerWarning = "";
        statusLabel.ForeColor = SystemColors.ControlText;
        statusLabel.Text = "正在启动采集，请稍候…";
        elapsedLabel.Text = "已录制 00:00.0";
        UpdateButtons();
        try
        {
            worker = await Task.Run(delegate
            {
                string script = Path.Combine(appDirectory, "record.ps1");
                if (!File.Exists(script))
                    throw new FileNotFoundException("找不到采集脚本 record.ps1。", script);
                string powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                    @"PowerShell\7\pwsh.exe");
                if (!File.Exists(powershell))
                    throw new FileNotFoundException("需要已安装的 PowerShell 7，找不到 " + powershell + "。请安装 PowerShell 7 后重试；不会回退到 Windows PowerShell。", powershell);
                if (FileVersionInfo.GetVersionInfo(powershell).FileMajorPart != 7)
                    throw new InvalidOperationException("采集器仅支持 PowerShell 7：" + powershell);
                Directory.CreateDirectory(outputDirectory);
                WriteJson(outputDirectory, "request.json", new Dictionary<string, object>
                {
                    { "utc", requestedUtc.ToString("o", CultureInfo.InvariantCulture) },
                    { "qpcTicks", requestedQpc }, { "qpcFrequency", Stopwatch.Frequency },
                    { "collectors", collectors }, { "label", label }, { "labelCode", labelCode }, { "notes", notes },
                    { "xiaomiPerformanceMode", xiaomiMode }, { "xiaomiPerformanceModeSource", "user-selected" },
                    { "measurementMode", measurementMode }, { "capturePowerState", true }, { "recordMenuKeys", menuKeys },
                    { "routeSwitchRequirements", new Dictionary<string, object> { { "sharedCopySync", "requires-supporting-installation" },
                        { "installationSupportVerified", false }, { "effectiveRouteSource", "measurement_status" }, { "note", SharedRouteRequirement } } },
                    { "markerProtocol", new Dictionary<string, object> { { "schema", 1 }, { "writer", "gui" }, { "closeBeforeStop", true } } }
                });
                string arguments = "-NoLogo -NoProfile -ExecutionPolicy Bypass -File " + Program.QuoteArgument(script)
                    + " -OutputDirectory " + Program.QuoteArgument(outputDirectory)
                    + " -OwnerPid " + ownerPid.ToString(CultureInfo.InvariantCulture)
                    + " -OwnerCreationFileTime " + ownerCreationFileTime.ToString(CultureInfo.InvariantCulture)
                    + (selfTest ? " -Simulate" : "");
                ProcessStartInfo startInfo = new ProcessStartInfo(powershell, arguments)
                {
                    UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
                    WorkingDirectory = appDirectory
                };
                startInfo.EnvironmentVariables.Remove("PSModulePath");
                Process process = Process.Start(startInfo);
                if (process == null)
                    throw new InvalidOperationException("无法创建采集进程。");
                return process;
            });
            workerCreationFileTime = worker.StartTime.ToUniversalTime().ToFileTimeUtc();
            outputCreated = true;
            UpdateButtons();
            if (stopRequested)
                await PersistStopRequest();
        }
        catch (Exception ex)
        {
            FinishRun(false, "无法开始录制：" + ex.Message);
        }
    }

    private async void RequestStop()
    {
        if (!busy || stopRequested)
            return;
        stopQpcTicks = Stopwatch.GetTimestamp();
        stopUtc = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture);
        stopRequested = true;
        DisableMarkerInput();
        statusLabel.Text = closeAfterSave ? "正在停止并保存，完成后将关闭窗口。" : "已请求停止，正在保存，请稍候…";
        UpdateButtons();
        if (worker != null)
            await PersistStopRequest();
    }

    private async Task PersistStopRequest()
    {
        string directory = outputDirectory;
        long requestedQpc = stopQpcTicks;
        string requestedUtc = stopUtc;
        try
        {
            await CloseMarkersAsync();
            await Task.Run(delegate
            {
                if (!File.Exists(Path.Combine(directory, "stop.request")))
                    WriteJson(directory, "stop.request", new Dictionary<string, object>
                    {
                        { "qpcTicks", requestedQpc }, { "qpcFrequency", Stopwatch.Frequency },
                        { "utc", requestedUtc }, { "reason", "user" }
                    });
            });
        }
        catch (Exception ex)
        {
            if (busy && outputDirectory == directory)
            {
                stopRequested = false;
                statusLabel.ForeColor = Color.Firebrick;
                statusLabel.Text = "停止请求写入失败，请重试：" + ex.Message;
                UpdateButtons();
            }
        }
    }

    private async void PollTick(object sender, EventArgs args)
    {
        RefreshRouteState();
        UpdateElapsed();
        DriveSelfTest();
        if (!busy || worker == null || polling)
            return;
        polling = true;
        try
        {
            StatusSnapshot snapshot = await Task.Run(delegate { return ReadStatus(outputDirectory); });
            if (!busy || IsDisposed)
                return;
            if (snapshot != null)
            {
                lastStatus = snapshot;
                ApplyStatus(snapshot);
            }
            if (worker.HasExited)
            {
                await CloseMarkersAsync();
                childExitCode = worker.ExitCode;
                if (lastStatus != null && (lastStatus.State == "complete" || lastStatus.State == "failed"))
                {
                    bool success = lastStatus.State == "complete" && childExitCode == 0;
                    FinishRun(success, success ? "保存完成。" + (lastStatus.Message.IndexOf("复用已有PresentMon", StringComparison.Ordinal) >= 0
                        ? "\r\n复用已有PresentMon，缺少独立raw。" : "") : "录制失败：" +
                        (String.IsNullOrWhiteSpace(lastStatus.Message) ? "采集进程退出码 " + childExitCode : lastStatus.Message));
                }
                else if (!exitObservedAt.HasValue)
                    exitObservedAt = DateTime.UtcNow;
                else if ((DateTime.UtcNow - exitObservedAt.Value).TotalMilliseconds >= 750)
                    FinishRun(false, "采集进程已退出，但未写入完成状态。退出码：" + childExitCode);
            }
        }
        catch (Exception ex)
        {
            statusLabel.ForeColor = Color.Firebrick;
            statusLabel.Text = "无法读取采集状态：" + ex.Message;
        }
        finally
        {
            polling = false;
        }
    }

    private void ApplyStatus(StatusSnapshot status)
    {
        if (status.State == "recording" && !stopRequested) StartMarkers(status);
        else if (status.State == "saving" || status.State == "complete" || status.State == "failed") CloseMarkersAsync();
        string message;
        switch (status.State)
        {
            case "preparing": message = "正在准备采集…"; break;
            case "recording": message = "正在录制。Ctrl+Shift+F9 打点；完成后点击“停止并保存”。"; break;
            case "saving": message = "正在保存，请稍候…"; break;
            case "complete": message = "保存完成，正在结束采集进程…"; break;
            default: message = "录制失败，正在结束采集进程…"; break;
        }
        if (stopRequested && (status.State == "preparing" || status.State == "recording"))
            message = "已请求停止，正在等待保存…";
        if (closeAfterSave)
            message += " 完成后将关闭窗口。";
        statusLabel.Text = message + (String.IsNullOrWhiteSpace(status.Message) ? "" : "\r\n" + status.Message);
        if (!String.IsNullOrEmpty(markerWarning)) statusLabel.Text += "\r\n" + markerWarning;
        statusLabel.ForeColor = status.State == "recording" ? Color.Red
            : status.State == "failed" ? Color.Firebrick : SystemColors.ControlText;
        UpdateButtons();
        if (selfTest && status.State == "recording" && !selfTestSawRecording)
        {
            selfTestSawRecording = true;
            selfTestStopAt = DateTime.UtcNow.AddMilliseconds(1200);
            if (startButton.Enabled || !stopButton.Enabled)
                selfTestError = "录制期间按钮状态不正确。";
            if (presetBox.Enabled) selfTestError = "录制期间预设未锁定。";
            foreach (CheckBox check in collectorChecks.Values)
                if (check.Enabled) selfTestError = "录制期间采集项未锁定。";
        }
        UpdateElapsed();
    }

    private void UpdateElapsed()
    {
        double duration = 0;
        if (lastStatus != null)
        {
            duration = lastStatus.DurationSeconds;
            if (busy && lastStatus.RecordingReadyQpcTicks > 0 && lastStatus.QpcFrequency > 0
                && lastStatus.State == "recording")
            {
                long end = stopQpcTicks > 0 ? stopQpcTicks : Stopwatch.GetTimestamp();
                duration = Math.Max(0, (double)(end - lastStatus.RecordingReadyQpcTicks) / lastStatus.QpcFrequency);
            }
            else if (lastStatus.State == "preparing") duration = 0;
        }
        if (Double.IsNaN(duration) || Double.IsInfinity(duration) || duration < 0)
            duration = 0;
        TimeSpan elapsed = TimeSpan.FromSeconds(Math.Min(duration, 31536000));
        elapsedLabel.Text = "已录制 " + ((int)elapsed.TotalMinutes).ToString("00", CultureInfo.InvariantCulture)
            + ":" + elapsed.Seconds.ToString("00", CultureInfo.InvariantCulture)
            + "." + (elapsed.Milliseconds / 100).ToString(CultureInfo.InvariantCulture);
    }

    private void StartMarkers(StatusSnapshot status)
    {
        if (markersStarted || markerCloseTask != null) return;
        if (worker == null || !RecorderReadyIdentity.IsReady(status.State, status.BackendPid, status.BackendCreationFileTime,
            worker.Id, workerCreationFileTime, status.StartQpcTicks, status.RecordingReadyQpcTicks, status.QpcFrequency))
        {
            markerWarning = "采集进程 ready 身份未通过验证，已停止此次录制。";
            RequestStop();
            return;
        }
        try
        {
            markerWriter = new RecorderMarkerWriter(outputDirectory, selfTest,
                selfTest ? (Action)null : delegate { RecorderNativeInput.Beep(880, 60); }, 256);
            markersStarted = true;
            if (selfTest)
            {
                markerWriter.Enqueue("manual", null);
                markerWriter.Enqueue("key", "M");
                markerWriter.Enqueue("key", "Esc");
                return;
            }
            markerInput = new RecorderInputCapture(new RecorderNativeInput(Handle, status.GamePid, status.GameCreationFileTime), markerWriter.Enqueue);
            markerInput.Start(menuKeysCheck.Checked);
        }
        catch (Exception ex)
        {
            markerWarning = "打点初始化失败：" + ex.Message;
            RequestStop();
        }
    }

    private void DisableMarkerInput()
    {
        RecorderInputCapture input = markerInput;
        if (input == null) return;
        try { input.Dispose(); markerInput = null; }
        catch (Exception ex) { markerWarning = "按键记录已停用；卸载时报告：" + ex.Message + " 若不能再次开始，请重新打开录制器。"; }
    }

    private Task CloseMarkersAsync()
    {
        DisableMarkerInput();
        if (markerCloseTask != null) return markerCloseTask;
        if (!outputCreated) return Task.FromResult(true);
        RecorderMarkerWriter writer = markerWriter;
        string directory = outputDirectory;
        string inputError = markerWarning;
        markerCloseTask = writer != null ? writer.CloseAsync(inputError)
            : Task.Run(delegate { RecorderMarkerWriter.CloseWithoutStarting(directory, selfTest, inputError); });
        return markerCloseTask;
    }

    protected override void WndProc(ref Message message)
    {
        if (message.Msg == 0x312 && message.WParam.ToInt32() == RecorderInputCapture.HotkeyId && markerInput != null)
            markerInput.ManualMarker();
        base.WndProc(ref message);
    }

    private void FinishRun(bool success, string message)
    {
        DisableMarkerInput();
        busy = false;
        if (worker != null)
        {
            worker.Dispose();
            worker = null;
        }
        outputCreated = !String.IsNullOrEmpty(outputDirectory) && Directory.Exists(outputDirectory);
        statusLabel.Text = message + (String.IsNullOrEmpty(markerWarning) ? "" : "\r\n" + markerWarning);
        statusLabel.ForeColor = success ? Color.DarkGreen : Color.Firebrick;
        UpdateButtons();
        UpdateElapsed();
        if (selfTest)
            CompleteSelfTest(success, message);
        else if (closeAfterSave)
            Close();
    }

    private void UpdateButtons()
    {
        startButton.Enabled = !busy && !selfTestFinishing && !routeSwitchBusy && markerInput == null;
        presetBox.Enabled = !busy && !selfTestFinishing;
        foreach (Control control in new Control[] { labelBox, notesBox, xiaomiBox, menuKeysCheck, detailCheck })
            control.Enabled = !busy && !selfTestFinishing;
        routeBox.Enabled = !busy && !routeSwitchBusy && !routeGameRunning;
        routeButton.Enabled = !busy && !routeSwitchBusy && !routeGameRunning && routeBox.SelectedIndex >= 0
            && (routeBox.SelectedIndex != 3 || SharedRouteSupported);
        foreach (CheckBox check in collectorChecks.Values) check.Enabled = !busy && !selfTestFinishing;
        stopButton.Enabled = busy && worker != null && !stopRequested
            && (lastStatus == null || lastStatus.State == "preparing" || lastStatus.State == "recording");
        openButton.Enabled = outputCreated;
    }

    private void OpenOutputClicked(object sender, EventArgs args)
    {
        if (!outputCreated)
            return;
        try
        {
            Process.Start(new ProcessStartInfo(outputDirectory) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            statusLabel.ForeColor = Color.Firebrick;
            statusLabel.Text = "无法打开输出目录：" + ex.Message;
        }
    }

    private void ClosingWhileBusy(object sender, FormClosingEventArgs args)
    {
        if (!busy)
            return;
        args.Cancel = true;
        closeAfterSave = true;
        RequestStop();
        statusLabel.Text = "正在停止并保存，完成后将关闭窗口。";
    }

    private void CheckSelectionSelfTest(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
        selfTestOptionChecks++;
    }

    private static bool SameCollectors(Dictionary<string, bool> first, Dictionary<string, bool> second)
    {
        foreach (string key in CollectorKeys) if (first[key] != second[key]) return false;
        return true;
    }

    private void RunSelectionSelfTests()
    {
        string fixture = Path.Combine(Path.GetTempPath(), "EndfieldRecorder-settings-" + Guid.NewGuid().ToString("N") + ".json");
        try
        {
            CheckSelectionSelfTest(collectorChecks.Count == 9 && presetBox.SelectedIndex == 0
                && SameCollectors(SelectedCollectors(), PresetCollectors("performance")), "默认采集项不正确。");
            CheckSelectionSelfTest(LabelCodes.Length == 9 && labelBox.Items.Count == 9
                && String.Join(",", LabelCodes) == "sync,k2,k1,sync-shared,k2-depth,t4-observe,t4-phase1,official,freeplay", "本局标签白名单不正确。");
            Dictionary<string, bool> performance = PresetCollectors("performance");
            CheckSelectionSelfTest(!performance["presentmonRaw"] && performance["pmdp"] && performance["rtss"]
                && performance["hardware"] && performance["foreground"] && !performance["system"]
                && !performance["threads"] && performance["measurements"] && !performance["logs"], "常用性能预设不正确。");
            presetBox.SelectedIndex = 1;
            foreach (bool enabled in SelectedCollectors().Values) CheckSelectionSelfTest(enabled, "卡顿排查未全选。");
            collectorChecks["pmdp"].Checked = false;
            CheckSelectionSelfTest(presetBox.SelectedIndex == 3 && !collectorChecks["pmdp"].Checked, "手动勾选未切换到自定义。");
            foreach (string key in CollectorKeys)
                CheckSelectionSelfTest(collectorChecks[key].Checked == (key != "pmdp"), "手动修改覆盖了其他采集项。");
            Dictionary<string, bool> custom = SelectedCollectors();
            collectorChecks["logs"].Checked = false;
            string serialized = new JavaScriptSerializer().Serialize(new Dictionary<string, object> { { "collectors", custom } });
            Dictionary<string, object> request = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(serialized);
            CheckSelectionSelfTest(SameCollectors(ParseCollectors(request["collectors"]), custom), "采集项序列化或快照不正确。");

            SaveSettings(fixture, "custom", custom);
            string warning;
            RecorderSettings loaded = LoadSettings(fixture, out warning);
            CheckSelectionSelfTest(warning == "" && loaded.Preset == "custom" && SameCollectors(loaded.Collectors, custom), "自定义设置未正确持久化。");
            ApplySettings(loaded);
            CheckSelectionSelfTest(presetBox.SelectedIndex == 3 && SameCollectors(SelectedCollectors(), custom), "重新打开设置未保留勾选项。");
            presetBox.SelectedIndex = 2;
            Dictionary<string, bool> stability = SelectedCollectors();
            CheckSelectionSelfTest(!stability["presentmonRaw"] && stability["pmdp"] && !stability["rtss"]
                && stability["hardware"] && stability["foreground"] && !stability["system"]
                && !stability["threads"] && stability["measurements"] && !stability["logs"] && !detailCheck.Checked,
                "长时间评估预设不正确。");
            SaveSettings(fixture, "stability", stability);
            loaded = LoadSettings(fixture, out warning);
            CheckSelectionSelfTest(warning == "" && loaded.Preset == "stability" && SameCollectors(loaded.Collectors, stability), "覆盖保存设置失败。");
            foreach (string invalid in new string[]
            {
                "{broken", "{\"preset\":\"performance\"}",
                "{\"version\":1,\"preset\":\"custom\",\"collectors\":{\"pmdp\":\"true\"}}"
            })
            {
                File.WriteAllText(fixture, invalid, new UTF8Encoding(false));
                loaded = LoadSettings(fixture, out warning);
                CheckSelectionSelfTest(warning != "" && loaded.Preset == "performance"
                    && SameCollectors(loaded.Collectors, performance) && File.ReadAllText(fixture) == invalid,
                    "旧 / 损坏设置未只读回退到默认项。");
            }
            Dictionary<string, bool> disabled = new Dictionary<string, bool>();
            foreach (string key in CollectorKeys) disabled.Add(key, false);
            ApplySettings(new RecorderSettings { Preset = "custom", Collectors = disabled });
            string previousOutput = outputDirectory;
            StartClicked(null, EventArgs.Empty);
            CheckSelectionSelfTest(!busy && worker == null && outputDirectory == previousOutput
                && statusLabel.Text.IndexOf("至少勾选", StringComparison.Ordinal) >= 0, "全未选时未拒绝开始。");
            busy = true;
            UpdateButtons();
            CheckSelectionSelfTest(!presetBox.Enabled && !startButton.Enabled, "录制期间预设未锁定。");
            foreach (CheckBox check in collectorChecks.Values) CheckSelectionSelfTest(!check.Enabled, "录制期间采集项未锁定。");
            foreach (Control control in new Control[] { labelBox, notesBox, xiaomiBox, menuKeysCheck, detailCheck })
                CheckSelectionSelfTest(!control.Enabled, "录制期间标签 / 模式 / 按键选项未锁定。");
            CheckSelectionSelfTest(!routeBox.Enabled && !routeButton.Enabled, "录制期间路线切换器未锁定。");
            busy = false;
            UpdateButtons();
            CheckSelectionSelfTest(presetBox.Enabled && startButton.Enabled, "完成后未恢复选项。");
            foreach (CheckBox check in collectorChecks.Values) CheckSelectionSelfTest(check.Enabled, "完成后采集项仍被锁定。");
            foreach (Control control in new Control[] { labelBox, notesBox, xiaomiBox, menuKeysCheck, detailCheck })
                CheckSelectionSelfTest(control.Enabled, "完成后标签 / 模式 / 按键选项仍被锁定。");
        }
        catch (Exception ex) { selfTestError = "采集选项自检失败：" + ex.Message; }
        finally
        {
            busy = false;
            ApplySettings(new RecorderSettings { Preset = "performance", Collectors = PresetCollectors("performance") });
            labelBox.SelectedIndex = 8;
            notesBox.Text = "TEST ONLY: GUI metadata / marker lifecycle";
            xiaomiBox.SelectedIndex = 1;
            menuKeysCheck.Checked = true;
            UpdateButtons();
            try { if (File.Exists(fixture)) File.Delete(fixture); }
            catch (IOException ex) { selfTestError = "无法移除自检设置文件：" + ex.Message; }
            catch (UnauthorizedAccessException ex) { selfTestError = "无法移除自检设置文件：" + ex.Message; }
        }
    }

    private void DriveSelfTest()
    {
        if (!selfTest || selfTestFinishing)
            return;
        if (selfTestSawRecording && !selfTestClickedStop && DateTime.UtcNow >= selfTestStopAt && stopButton.Enabled)
        {
            selfTestClickedStop = true;
            stopButton.PerformClick();
        }
        if (selfTestDeadline != DateTime.MinValue && DateTime.UtcNow > selfTestDeadline && selfTestError != "模拟自检超时。")
        {
            selfTestError = "模拟自检超时。";
            RequestStop();
            if (!busy)
                CompleteSelfTest(false, selfTestError);
        }
    }

    private async void CompleteSelfTest(bool success, string message)
    {
        if (selfTestFinishing)
            return;
        List<string> errors = new List<string>();
        if (!success) errors.Add(message);
        if (!String.IsNullOrEmpty(selfTestError)) errors.Add(selfTestError);
        if (!selfTestSawRecording) errors.Add("未观察到 recording 状态。");
        if (!selfTestClickedStop) errors.Add("未通过停止按钮请求保存。");
        if (!startButton.Enabled || stopButton.Enabled) errors.Add("完成后的按钮状态不正确。");
        if (!presetBox.Enabled) errors.Add("完成后预设仍被锁定。");
        foreach (CheckBox check in collectorChecks.Values) if (!check.Enabled) errors.Add("完成后采集项仍被锁定。");
        if (selfTestOptionChecks < 40) errors.Add("采集选项自检未完成。");
        if (lastStatus == null || lastStatus.State != "complete") errors.Add("最终状态不是 complete。");
        if (lastStatus == null || lastStatus.Mode.IndexOf("simulat", StringComparison.OrdinalIgnoreCase) < 0)
            errors.Add("后端未报告模拟模式。");
        if (lastStatus == null || lastStatus.StartQpcTicks <= 0 || lastStatus.QpcFrequency <= 0 || lastStatus.DurationSeconds <= 0)
            errors.Add("后端时间数据不完整。");
        if (lastStatus == null || String.IsNullOrEmpty(lastStatus.SessionId)) errors.Add("后端未报告 sessionId。");
        selfTestFinishing = true;
        string screenshot = Path.Combine(outputDirectory, "self-test-window.png");
        byte[] screenshotBytes = null;
        try
        {
            using (Bitmap bitmap = new Bitmap(Width, Height))
            using (MemoryStream bytes = new MemoryStream())
            {
                DrawToBitmap(bitmap, new Rectangle(0, 0, Width, Height));
                bitmap.Save(bytes, ImageFormat.Png);
                screenshotBytes = bytes.ToArray();
            }
            await Task.Run(delegate
            {
                Directory.CreateDirectory(outputDirectory);
                if (!File.Exists(Path.Combine(outputDirectory, "request.json"))) errors.Add("缺少 request.json。");
                else
                {
                    try
                    {
                        Dictionary<string, object> request = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(
                            File.ReadAllText(Path.Combine(outputDirectory, "request.json"), Encoding.UTF8));
                        object requested;
                        if (request == null || !request.TryGetValue("collectors", out requested)
                            || selfTestRequestedCollectors == null || !SameCollectors(ParseCollectors(requested), selfTestRequestedCollectors))
                            errors.Add("request.json 未保留九项布尔选择快照。");
                        if (GetString(request, "labelCode") != "freeplay" || GetString(request, "label") != "自由游玩"
                            || GetString(request, "notes") != "TEST ONLY: GUI metadata / marker lifecycle"
                            || GetString(request, "xiaomiPerformanceMode") != "均衡"
                            || GetString(request, "xiaomiPerformanceModeSource") != "user-selected"
                            || GetString(request, "measurementMode") != "full") errors.Add("request.json 未保留本局标签 / 备注 / 模式。");
                        object routeRequirement;
                        if (!request.TryGetValue("routeSwitchRequirements", out routeRequirement)
                            || !(routeRequirement is Dictionary<string, object>)) errors.Add("request.json 缺少路线支持前提。");
                        else
                        {
                            Dictionary<string, object> requirement = (Dictionary<string, object>)routeRequirement;
                            if (GetString(requirement, "sharedCopySync") != "requires-supporting-installation"
                                || GetString(requirement, "effectiveRouteSource") != "measurement_status"
                                || !requirement.ContainsKey("installationSupportVerified") || !(requirement["installationSupportVerified"] is bool)
                                || (bool)requirement["installationSupportVerified"] || GetString(requirement, "note") != SharedRouteRequirement)
                                errors.Add("request.json 错把文件开关视为安装支持已确认。");
                        }
                    }
                    catch (Exception ex) { errors.Add("request.json 选择快照无效：" + ex.Message); }
                }
                if (!File.Exists(Path.Combine(outputDirectory, "stop.request"))) errors.Add("缺少 stop.request。");
                string markersPath = Path.Combine(outputDirectory, "markers.jsonl");
                string closedPath = Path.Combine(outputDirectory, "markers.closed.json");
                if (!File.Exists(markersPath) || !File.Exists(closedPath)) errors.Add("缺少已关闭的打点文件。");
                else
                {
                    Dictionary<string, object> closed = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(closedPath));
                    if (File.ReadAllLines(markersPath).Length != 3 || GetLong(closed, "written") != 3
                        || GetLong(closed, "dropped") != 0 || GetLong(closed, "unwritten") != 0)
                        errors.Add("模拟打点数量或队列关闭状态不正确。");
                    using (FileStream exclusive = new FileStream(markersPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
                }
                if (lastStatus == null || String.IsNullOrEmpty(lastStatus.ZipPath) || !File.Exists(lastStatus.ZipPath)
                    || new FileInfo(lastStatus.ZipPath).Length == 0)
                    errors.Add("缺少非空录制压缩包。");
                File.WriteAllBytes(screenshot, screenshotBytes);
                WriteJson(outputDirectory, "self-test-result.json", new Dictionary<string, object>
                {
                    { "passed", errors.Count == 0 }, { "errors", errors.ToArray() },
                    { "outputDirectory", outputDirectory }, { "screenshotPath", screenshot },
                    { "sawRecording", selfTestSawRecording }, { "clickedStop", selfTestClickedStop },
                    { "selectionChecks", selfTestOptionChecks }, { "collectors", selfTestRequestedCollectors },
                    { "childExitCode", childExitCode }, { "state", lastStatus == null ? "" : lastStatus.State },
                    { "mode", lastStatus == null ? "" : lastStatus.Mode },
                    { "zipPath", lastStatus == null ? "" : lastStatus.ZipPath },
                    { "finishedUtc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) }
                });
            });
            ExitCode = errors.Count == 0 ? 0 : 1;
        }
        catch (Exception ex)
        {
            ExitCode = 1;
            statusLabel.Text = "无法保存自检结果：" + ex.Message;
        }
        Close();
    }

    private static void WriteJson(string directory, string name, object value)
    {
        string path = Path.Combine(directory, name);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, new JavaScriptSerializer().Serialize(value), new UTF8Encoding(false));
            File.Move(temporary, path);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    private static StatusSnapshot ReadStatus(string directory)
    {
        try
        {
            string text;
            using (FileStream file = new FileStream(Path.Combine(directory, "status.json"), FileMode.Open,
                FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                if (file.Length > 65536) return null;
                using (StreamReader reader = new StreamReader(file, Encoding.UTF8)) text = reader.ReadToEnd();
            }
            Dictionary<string, object> values = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(text);
            if (values == null) return null;
            string state = GetString(values, "state");
            if (state != "preparing" && state != "recording" && state != "saving" && state != "complete" && state != "failed")
                return null;
            return new StatusSnapshot
            {
                State = state, Message = GetString(values, "message"), Mode = GetString(values, "mode"),
                SessionId = GetString(values, "sessionId"), ZipPath = GetString(values, "zipPath"),
                StartQpcTicks = GetLong(values, "startQpcTicks"), QpcFrequency = GetLong(values, "qpcFrequency"),
                RecordingReadyQpcTicks = GetLong(values, "recordingReadyQpcTicks"),
                BackendPid = checked((int)GetLong(values, "backendPid")), BackendCreationFileTime = GetLong(values, "backendCreationFileTime"),
                GamePid = checked((int)GetLong(values, "gamePid")), GameCreationFileTime = GetLong(values, "gameCreationFileTime"),
                DurationSeconds = values.ContainsKey("durationSeconds")
                    ? Convert.ToDouble(values["durationSeconds"], CultureInfo.InvariantCulture) : 0
            };
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (ArgumentException) { return null; }
        catch (InvalidOperationException) { return null; }
        catch (FormatException) { return null; }
        catch (InvalidCastException) { return null; }
        catch (OverflowException) { return null; }
    }

    private static string GetString(Dictionary<string, object> values, string key)
    {
        object value;
        return values.TryGetValue(key, out value) && value != null ? Convert.ToString(value, CultureInfo.InvariantCulture) : "";
    }

    private static long GetLong(Dictionary<string, object> values, string key)
    {
        object value;
        return values.TryGetValue(key, out value) && value != null ? Convert.ToInt64(value, CultureInfo.InvariantCulture) : 0;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            DisableMarkerInput();
            if (outputCreated && markerCloseTask == null) CloseMarkersAsync();
            timer.Dispose();
        }
        base.Dispose(disposing);
    }

    private sealed class StatusSnapshot
    {
        internal string State;
        internal string Message;
        internal string Mode;
        internal string SessionId;
        internal string ZipPath;
        internal long StartQpcTicks;
        internal long QpcFrequency;
        internal long RecordingReadyQpcTicks;
        internal int BackendPid;
        internal long BackendCreationFileTime;
        internal int GamePid;
        internal long GameCreationFileTime;
        internal double DurationSeconds;
    }

    private sealed class RecorderSettings
    {
        internal string Preset;
        internal Dictionary<string, bool> Collectors;
    }
}
