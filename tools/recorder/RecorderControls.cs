using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Microsoft.Win32.SafeHandles;

internal static class RecorderReadyIdentity
{
    internal static bool IsReady(string state, int pid, long created, int expectedPid, long expectedCreated,
        long started, long ready, long frequency)
    {
        return state == "recording" && pid > 0 && pid == expectedPid && created > 0 && created == expectedCreated
            && started > 0 && ready >= started && frequency == Stopwatch.Frequency;
    }
}

internal static class RecorderRouteConfiguration
{
    // Match diagnostic_flags.hpp: a bounded raw-byte read, optional leading UTF-8 BOM, ASCII whitespace only.
    internal static int ParseAsyncFrames(byte[] bytes, int count, out bool valid)
    {
        valid = false;
        if (bytes == null || count < 0 || count > bytes.Length || count > 16) return 2;
        int begin = count >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
        int end = count - 1;
        while (begin <= end && IsSpace(bytes[begin])) begin++;
        while (end >= begin && IsSpace(bytes[end])) end--;
        if (begin == end && (bytes[begin] == (byte)'1' || bytes[begin] == (byte)'2'))
        {
            valid = true;
            return bytes[begin] - (byte)'0';
        }
        return 2;
    }
    private static bool IsSpace(byte value) { return value == 0x20 || value == 0x09 || value == 0x0d || value == 0x0a; }
}

internal sealed class RecorderMarkerWriter
{
    private readonly BlockingCollection<Dictionary<string, object>> queue;
    private readonly Task writerTask;
    private readonly string directory;
    private readonly bool testOnly;
    private readonly Action beep;
    private readonly object gate = new object();
    private long accepted, written, dropped, sequence;
    private int closing;
    private string error;

    internal RecorderMarkerWriter(string directory, bool testOnly, Action beep, int capacity)
    {
        this.directory = directory;
        this.testOnly = testOnly;
        this.beep = beep;
        queue = new BlockingCollection<Dictionary<string, object>>(capacity);
        // The backend checks this exclusive-write lease before hashing the capture.
        StreamWriter file = new StreamWriter(new FileStream(Path.Combine(directory, "markers.jsonl"),
            FileMode.CreateNew, FileAccess.Write, FileShare.Read), new UTF8Encoding(false));
        file.NewLine = "\n";
        writerTask = Task.Run(delegate
        {
            try
            {
                JavaScriptSerializer json = new JavaScriptSerializer();
                foreach (Dictionary<string, object> marker in queue.GetConsumingEnumerable())
                {
                    file.WriteLine(json.Serialize(marker));
                    file.Flush();
                    Interlocked.Increment(ref written);
                }
            }
            catch (Exception ex) { error = ex.GetType().Name + ": " + ex.Message; }
            finally { try { file.Dispose(); } catch (Exception ex) { error = ex.GetType().Name + ": " + ex.Message; } }
        });
    }

    internal bool Enqueue(string kind, string key)
    {
        if (Volatile.Read(ref closing) != 0) return false;
        if (kind != "manual" && (kind != "key" || (key != "M" && key != "Esc"))) return false;
        Dictionary<string, object> marker = new Dictionary<string, object>
        {
            { "sequence", Interlocked.Increment(ref sequence) }, { "qpcTicks", Stopwatch.GetTimestamp() },
            { "qpcFrequency", Stopwatch.Frequency }, { "utc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) },
            { "kind", kind }
        };
        if (kind == "key") marker.Add("key", key);
        if (testOnly) marker.Add("testOnly", true);
        lock (gate)
        {
            if (closing != 0) return false;
            if (!queue.TryAdd(marker)) { Interlocked.Increment(ref dropped); return false; }
            Interlocked.Increment(ref accepted);
        }
        if (kind == "manual" && beep != null)
            ThreadPool.QueueUserWorkItem(delegate { try { beep(); } catch (Exception) { } });
        return true;
    }

    internal async Task CloseAsync(string inputError = null)
    {
        lock (gate) { if (closing == 0) { closing = 1; queue.CompleteAdding(); } }
        await writerTask.ConfigureAwait(false);
        long lost = Math.Max(0, Interlocked.Read(ref accepted) - Interlocked.Read(ref written));
        RecorderControlJson.Write(Path.Combine(directory, "markers.closed.json"), new Dictionary<string, object>
        {
            { "schema", 1 }, { "closed", true }, { "accepted", Interlocked.Read(ref accepted) },
            { "written", Interlocked.Read(ref written) }, { "dropped", Interlocked.Read(ref dropped) },
            { "unwritten", lost }, { "error", error }, { "inputError", inputError }, { "testOnly", testOnly },
            { "utc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) }
        });
    }

    internal static void CloseWithoutStarting(string directory, bool testOnly, string inputError = null)
    {
        RecorderControlJson.Write(Path.Combine(directory, "markers.closed.json"), new Dictionary<string, object>
        {
            { "schema", 1 }, { "closed", true }, { "accepted", 0 }, { "written", 0 },
            { "dropped", 0 }, { "unwritten", 0 }, { "error", null }, { "inputError", inputError }, { "testOnly", testOnly },
            { "utc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) }
        });
    }
}

internal interface IRecorderInputPlatform : IDisposable
{
    bool RegisterHotKey();
    void UnregisterHotKey();
    void InstallKeyboardHook(Action<int, int> callback);
    void RemoveKeyboardHook();
    bool IsTargetForeground();
}

internal sealed class RecorderInputCapture : IDisposable
{
    internal const int HotkeyId = 0x4e46;
    internal const int KeyDown = 0x100, SystemKeyDown = 0x104;
    private readonly IRecorderInputPlatform platform;
    private readonly Func<string, string, bool> emit;
    private bool active, registered, hooked;

    internal RecorderInputCapture(IRecorderInputPlatform platform, Func<string, string, bool> emit)
    {
        this.platform = platform;
        this.emit = emit;
    }

    internal void Start(bool menuKeys)
    {
        try
        {
            registered = platform.RegisterHotKey();
            if (!registered) throw new InvalidOperationException("Ctrl+Shift+F9 已被占用，无法注册打点热键。");
            if (menuKeys) { platform.InstallKeyboardHook(KeyEvent); hooked = true; }
            active = true;
        }
        catch { Dispose(); throw; }
    }

    internal bool ManualMarker()
    {
        return active && platform.IsTargetForeground() && emit("manual", null);
    }

    internal static string FilterKey(int virtualKey, int message, bool active, bool targetForeground)
    {
        if (!active || !targetForeground || (message != KeyDown && message != SystemKeyDown)) return null;
        return virtualKey == 0x4d ? "M" : virtualKey == 0x1b ? "Esc" : null;
    }

    private void KeyEvent(int virtualKey, int message)
    {
        // No disk access or blocking work belongs in the low-level keyboard callback.
        if (!active || (virtualKey != 0x4d && virtualKey != 0x1b)) return;
        string key = FilterKey(virtualKey, message, active, platform.IsTargetForeground());
        if (key != null) emit("key", key);
    }

    public void Dispose()
    {
        active = false;
        Exception failure = null;
        try { if (hooked) { platform.RemoveKeyboardHook(); hooked = false; } }
        catch (Exception ex) { failure = ex; }
        try { if (registered) { platform.UnregisterHotKey(); registered = false; } }
        catch (Exception ex) { if (failure == null) failure = ex; }
        finally { platform.Dispose(); }
        if (failure != null) throw failure;
    }
}

internal sealed class RecorderNativeInput : IRecorderInputPlatform
{
    private readonly IntPtr window;
    private readonly uint targetPid;
    private SafeWaitHandle targetProcess;
    private IntPtr hook;
    private HookProcedure procedure;
    private delegate IntPtr HookProcedure(int code, IntPtr message, IntPtr data);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool RegisterHotKey(IntPtr window, int id, uint modifiers, uint key);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool UnregisterHotKey(IntPtr window, int id);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SetWindowsHookEx(int id, HookProcedure callback, IntPtr module, uint thread);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool UnhookWindowsHookEx(IntPtr hook);
    [DllImport("user32.dll")] private static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr message, IntPtr data);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern SafeWaitHandle OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessTimes(SafeWaitHandle process, out long created, out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll")] private static extern uint WaitForSingleObject(SafeWaitHandle process, uint milliseconds);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32.dll")] internal static extern bool Beep(uint frequency, uint milliseconds);

    internal RecorderNativeInput(IntPtr window, int pid, long created)
    {
        this.window = window;
        targetPid = checked((uint)pid);
        targetProcess = OpenProcess(0x1000 | 0x100000, false, targetPid);
        long actual, exit, kernel, user;
        if (targetProcess.IsInvalid || !GetProcessTimes(targetProcess, out actual, out exit, out kernel, out user)
            || actual != created || WaitForSingleObject(targetProcess, 0) != 258)
        {
            targetProcess.Dispose();
            throw new InvalidOperationException("游戏进程身份已变化，未启用按键采集。");
        }
    }

    public bool RegisterHotKey() { return RegisterHotKey(window, RecorderInputCapture.HotkeyId, 0x4000 | 0x2 | 0x4, 0x78); }
    public void UnregisterHotKey()
    {
        if (!UnregisterHotKey(window, RecorderInputCapture.HotkeyId))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "无法注销打点热键。");
    }
    public void InstallKeyboardHook(Action<int, int> callback)
    {
        procedure = delegate(int code, IntPtr message, IntPtr data)
        {
            try
            {
                int type = unchecked((int)message.ToInt64());
                if (code >= 0 && (type == RecorderInputCapture.KeyDown || type == RecorderInputCapture.SystemKeyDown))
                {
                    int key = Marshal.ReadInt32(data);
                    if (key == 0x4d || key == 0x1b) callback(key, type);
                }
            }
            catch (Exception) { }
            // Always continue the chain, including keys we observe.
            return CallNextHookEx(hook, code, message, data);
        };
        hook = SetWindowsHookEx(13, procedure, GetModuleHandle(null), 0);
        if (hook == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "无法启用 M / Esc 时间记录。");
    }
    public void RemoveKeyboardHook()
    {
        if (hook != IntPtr.Zero && !UnhookWindowsHookEx(hook))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "无法卸载 M / Esc 时间记录。");
        hook = IntPtr.Zero;
    }
    public bool IsTargetForeground()
    {
        uint pid;
        return targetProcess != null && !targetProcess.IsClosed && WaitForSingleObject(targetProcess, 0) == 258
            && GetWindowThreadProcessId(GetForegroundWindow(), out pid) != 0 && pid == targetPid;
    }
    public void Dispose() { if (targetProcess != null) { targetProcess.Dispose(); targetProcess = null; } }
}

internal static class RecorderControlJson
{
    internal static void Write(string path, object value)
    {
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, new JavaScriptSerializer().Serialize(value), new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temporary, path, null);
            else File.Move(temporary, path);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}

internal sealed class RecorderRouteSwitcher
{
    private readonly string root, log;
    private readonly Func<bool> gameRunning;
    internal readonly bool SharedSupported;
    private static readonly string[] Whitelist = { "SyncCopyPath", "AsyncCopyFrames", "SharedCopySync" };

    private RecorderRouteSwitcher(string root, string log, Func<bool> gameRunning, bool sharedSupported)
    {
        this.root = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar);
        this.log = log;
        this.gameRunning = gameRunning;
        SharedSupported = sharedSupported;
    }
    internal static RecorderRouteSwitcher ForMachine(string log, bool sharedSupported)
    {
        return new RecorderRouteSwitcher(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "EndfieldXeSSDiag"), log, AnyGameRunning, sharedSupported);
    }
    internal static RecorderRouteSwitcher ForTests(string root, string log, Func<bool> gameRunning, bool sharedSupported)
    {
        string full = Path.GetFullPath(root);
        string programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData).TrimEnd('\\') + "\\";
        if (full.StartsWith(programData, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Route-switch tests cannot target ProgramData.");
        return new RecorderRouteSwitcher(root, log, gameRunning, sharedSupported);
    }
    internal static bool AnyGameRunning()
    {
        Process[] processes = Process.GetProcessesByName("Endfield");
        try { return processes.Length != 0; }
        finally { foreach (Process process in processes) process.Dispose(); }
    }
    private void RequireNoGame()
    {
        if (gameRunning()) throw new InvalidOperationException("Endfield.exe 正在运行，拒绝修改。开关在下次启动游戏时生效。");
    }
    internal string CurrentRoute()
    {
        using (RecorderRouteDirectory directory = RecorderRouteDirectory.Open(root, false))
        {
            if (directory == null) return "同步 + 共享（默认；开关目录不存在）";
            if (directory.Exists("SyncCopyPath"))
                return "同步";
            if (directory.Exists("SharedCopySync"))
                return directory.SharedDisabled() ? "同步（共享被 DisableSharedCopyPath 禁用）"
                    : "同步 + 共享" + (SharedSupported ? "" : "（支持状态未确认）");
            string sharedNote = directory.SharedDisabled() ? "共享被 DisableSharedCopyPath 禁用" : null;
            if (directory.Exists("AsyncCopyFrames"))
            {
                string numericNote;
                int frames = directory.ReadAsyncFrames(out numericNote);
                string note = numericNote == null ? sharedNote : numericNote + (sharedNote == null ? "" : "；" + sharedNote);
                return (frames == 0 ? "未知" : numericNote == null || directory.AsyncEnabled()
                    ? "K" + frames.ToString(CultureInfo.InvariantCulture) : sharedNote == null ? "同步 + 共享" : "同步")
                    + (note == null ? "" : "（" + note + "）");
            }
            return (directory.AsyncEnabled() ? "K2" : sharedNote == null ? "同步 + 共享" : "同步")
                + (sharedNote == null ? "" : "（" + sharedNote + "）");
        }
    }

    internal void Switch(string route)
    {
        List<Dictionary<string, object>> before = null, after = null;
        string rejection = null;
        bool changed = false;
        try
        {
            if (route != "sync" && route != "k2" && route != "k1" && route != "sync-shared")
                throw new InvalidOperationException("路线不在白名单内。");
            if (route == "sync-shared" && !SharedSupported)
                throw new InvalidOperationException("同步 + 共享尚未确认支持，不能切换。");
            using (RecorderRouteDirectory existing = RecorderRouteDirectory.Open(root, false))
            {
                before = existing == null ? new List<Dictionary<string, object>>() : existing.Snapshot();
                after = before;
                RequireNoGame();
            }
            using (RecorderRouteDirectory directory = RecorderRouteDirectory.Open(root, true))
            {
                before = directory.Snapshot();
                try
                {
                    // Validate all switch files before making the first change.
                    foreach (string name in Whitelist) if (directory.Exists(name)) directory.ValidateFile(name);
                    if (route == "sync-shared" && directory.SharedDisabled())
                        throw new InvalidOperationException("DisableSharedCopyPath 已禁用共享；切换器不会修改此文件。");
                    RequireNoGame();
                    if (route == "sync")
                    {
                        RequireNoGame(); directory.Write("SyncCopyPath", "1"); changed = true;
                        RequireNoGame(); directory.Delete("AsyncCopyFrames");
                        RequireNoGame(); directory.Delete("SharedCopySync");
                    }
                    else
                    {
                        RequireNoGame();
                        if (route == "sync-shared") directory.Write("SharedCopySync", "1");
                        else if (route == "k1") directory.Write("AsyncCopyFrames", "1");
                        else directory.Write("AsyncCopyFrames", "2");
                        changed = true;
                        RequireNoGame();
                        if (route == "sync-shared") directory.Delete("AsyncCopyFrames");
                        else directory.Delete("SharedCopySync");
                        RequireNoGame(); directory.Delete("SyncCopyPath");
                    }
                }
                finally { after = directory.Snapshot(); }
            }
        }
        catch (Exception ex) { rejection = ex.Message; throw; }
        finally
        {
            Directory.CreateDirectory(Path.GetDirectoryName(log));
            Dictionary<string, object> row = new Dictionary<string, object>
            {
                { "utc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) }, { "requestedRoute", route },
                { "switchDirectory", root }, { "before", before }, { "after", after },
                { "changed", changed }, { "rejection", rejection }, { "applies", "next-game-launch" }
            };
            File.AppendAllText(log, new JavaScriptSerializer().Serialize(row) + "\n", new UTF8Encoding(false));
        }
    }
}

// Hold every ancestor without delete-sharing; never follow a reparse point when changing a switch.
internal sealed class RecorderRouteDirectory : IDisposable
{
    private readonly string root;
    private readonly List<SafeFileHandle> directoryHandles = new List<SafeFileHandle>();
    private const uint ReadAttributes = 0x80, DeleteAccess = 0x10000, OpenExisting = 3, OpenAlways = 4;
    private const uint BackupSemantics = 0x02000000, OpenReparsePoint = 0x00200000;
    [StructLayout(LayoutKind.Sequential, Pack = 4)] private struct FileInformation
    {
        internal uint Attributes;
        internal long Created, Accessed, Written;
        internal uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Disposition { [MarshalAs(UnmanagedType.U1)] internal bool Delete; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out FileInformation info);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetFileInformationByHandle(SafeFileHandle handle, int type, ref Disposition info, uint size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandle(SafeFileHandle handle, StringBuilder path, uint length, uint flags);

    private RecorderRouteDirectory(string root) { this.root = root; }
    internal static RecorderRouteDirectory Open(string root, bool create)
    {
        RecorderRouteDirectory directory = new RecorderRouteDirectory(root);
        try
        {
            string current = Path.GetPathRoot(root);
            string relative = root.Substring(current.Length);
            List<string> paths = new List<string> { current };
            foreach (string segment in relative.Split(Path.DirectorySeparatorChar))
            {
                if (segment.Length == 0) continue;
                current = Path.Combine(current, segment);
                paths.Add(current);
            }
            foreach (string path in paths)
            {
                if (!Directory.Exists(path))
                {
                    if (path != root || !create) { directory.Dispose(); return null; }
                    Directory.CreateDirectory(path);
                }
                SafeFileHandle handle = CreateFile(path, ReadAttributes, 3, IntPtr.Zero, OpenExisting,
                    BackupSemantics | OpenReparsePoint, IntPtr.Zero);
                if (handle.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "无法安全打开开关目录。");
                directory.directoryHandles.Add(handle);
                FileInformation info = Info(handle, path);
                if ((info.Attributes & (uint)FileAttributes.ReparsePoint) != 0 || (info.Attributes & (uint)FileAttributes.Directory) == 0)
                    throw new InvalidOperationException("开关目录或其父目录包含重解析点，拒绝修改：" + path);
            }
            return directory;
        }
        catch { directory.Dispose(); throw; }
    }
    private string FilePath(string name)
    {
        if (name != "SyncCopyPath" && name != "AsyncCopyFrames" && name != "SharedCopySync")
            throw new InvalidOperationException("开关文件不在白名单内。");
        return Path.Combine(root, name);
    }
    private static FileInformation Info(SafeFileHandle handle, string expected)
    {
        FileInformation info;
        if (!GetFileInformationByHandle(handle, out info)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        StringBuilder final = new StringBuilder(32768);
        uint count = GetFinalPathNameByHandle(handle, final, (uint)final.Capacity, 0);
        if (count == 0 || count >= final.Capacity) throw new InvalidOperationException("无法确认开关路径。");
        string actual = final.ToString();
        if (actual.StartsWith(@"\\?\", StringComparison.Ordinal)) actual = actual.Substring(4);
        if (!String.Equals(Path.GetFullPath(actual).TrimEnd('\\'), Path.GetFullPath(expected).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("开关路径在检查期间发生变化。");
        return info;
    }
    private SafeFileHandle OpenFile(string name, uint access, uint creation, uint share = 0)
    {
        string path = FilePath(name);
        SafeFileHandle handle = CreateFile(path, access | ReadAttributes, share, IntPtr.Zero, creation, OpenReparsePoint, IntPtr.Zero);
        if (handle.IsInvalid) { handle.Dispose(); throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "无法安全打开开关文件：" + name); }
        try
        {
            FileInformation info = Info(handle, path);
            if ((info.Attributes & ((uint)FileAttributes.Directory | (uint)FileAttributes.ReparsePoint)) != 0 || info.Links != 1)
                throw new InvalidOperationException("开关文件是目录、链接或重解析点，拒绝修改：" + name);
            return handle;
        }
        catch { handle.Dispose(); throw; }
    }
    internal bool Exists(string name)
    {
        return PathExists(FilePath(name));
    }
    internal bool SharedDisabled() { return PathExists(Path.Combine(root, "DisableSharedCopyPath")); }
    internal bool AsyncEnabled() { return PathExists(Path.Combine(root, "AsyncCopyPath")); }
    private static bool PathExists(string path)
    {
        try { File.GetAttributes(path); return true; }
        catch (FileNotFoundException) { return false; }
        catch (DirectoryNotFoundException) { return false; }
    }
    internal void ValidateFile(string name) { using (SafeFileHandle file = OpenFile(name, 0, OpenExisting)) { } }
    internal int ReadAsyncFrames(out string note)
    {
        try
        {
            FileAttributes attributes = File.GetAttributes(FilePath("AsyncCopyFrames"));
            if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
                throw new InvalidOperationException("Unsafe numeric marker.");
            using (SafeFileHandle handle = OpenFile("AsyncCopyFrames", 0x80000000, OpenExisting, 7))
            using (FileStream file = new FileStream(handle, FileAccess.Read, 17))
            {
                byte[] bytes = new byte[17];
                int count = file.Read(bytes, 0, bytes.Length);
                bool valid;
                int frames = RecorderRouteConfiguration.ParseAsyncFrames(bytes, count, out valid);
                note = valid ? null : count >= 17 ? "AsyncCopyFrames 超过 16 字节，忽略数值" : "AsyncCopyFrames 内容无效，忽略数值";
                return frames;
            }
        }
        // A safety-policy refusal says nothing about what the native loader would read.
        catch (InvalidOperationException) { note = "AsyncCopyFrames 安全检查拒绝读取，需清理不安全 marker"; return 0; }
        catch (Exception) { note = "AsyncCopyFrames 读取失败，忽略数值"; return 2; }
    }
    internal void Write(string name, string contents)
    {
        using (SafeFileHandle handle = OpenFile(name, 0x40000000, OpenAlways))
        using (FileStream file = new FileStream(handle, FileAccess.Write))
        {
            byte[] bytes = Encoding.ASCII.GetBytes(contents);
            file.SetLength(0); file.Write(bytes, 0, bytes.Length); file.Flush(true);
        }
    }
    internal void Delete(string name)
    {
        if (!Exists(name)) return;
        using (SafeFileHandle handle = OpenFile(name, DeleteAccess, OpenExisting))
        {
            Disposition disposition = new Disposition { Delete = true };
            if (!SetFileInformationByHandle(handle, 4, ref disposition, (uint)Marshal.SizeOf(typeof(Disposition))))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "无法删除白名单开关：" + name);
        }
    }
    internal List<Dictionary<string, object>> Snapshot()
    {
        List<Dictionary<string, object>> rows = new List<Dictionary<string, object>>();
        foreach (string path in Directory.GetFileSystemEntries(root))
        {
            FileAttributes attributes = File.GetAttributes(path);
            bool ordinary = (attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) == 0;
            FileInfo info = ordinary ? new FileInfo(path) : null;
            rows.Add(new Dictionary<string, object>
            {
                { "name", Path.GetFileName(path) }, { "attributes", attributes.ToString() },
                { "bytes", ordinary ? (object)info.Length : null },
                { "lastWriteUtc", ordinary ? info.LastWriteTimeUtc.ToString("o", CultureInfo.InvariantCulture) : null }
            });
        }
        rows.Sort(delegate(Dictionary<string, object> a, Dictionary<string, object> b)
        { return StringComparer.OrdinalIgnoreCase.Compare((string)a["name"], (string)b["name"]); });
        return rows;
    }
    public void Dispose() { foreach (SafeFileHandle handle in directoryHandles) handle.Dispose(); directoryHandles.Clear(); }
}
