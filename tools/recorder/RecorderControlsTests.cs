using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

internal static class RecorderControlsTests
{
    private static int checks;
    private static void Check(bool value, string message) { if (!value) throw new InvalidOperationException(message); checks++; }
    private static void Refuses(Action action, string message)
    {
        bool refused = false;
        try { action(); } catch (Exception) { refused = true; }
        Check(refused, message);
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateHardLink(string name, string existing, IntPtr security);

    private sealed class FakeInput : IRecorderInputPlatform
    {
        internal bool Foreground = true, Registered, Hooked, Disposed;
        internal bool RegisterFails, HookFails, UnregisterFails, RemoveFails;
        internal Action<int, int> Callback;
        public bool RegisterHotKey() { Registered = !RegisterFails; return Registered; }
        public void UnregisterHotKey() { Registered = false; if (UnregisterFails) throw new IOException("TEST ONLY unregister failure"); }
        public void InstallKeyboardHook(Action<int, int> callback) { if (HookFails) throw new IOException("TEST ONLY hook failure"); Hooked = true; Callback = callback; }
        public void RemoveKeyboardHook() { Hooked = false; if (RemoveFails) throw new IOException("TEST ONLY unhook failure"); }
        public bool IsTargetForeground() { return Foreground; }
        public void Dispose() { Disposed = true; }
    }

    private static void TestInput()
    {
        int emitted = 0;
        FakeInput fake = new FakeInput();
        RecorderInputCapture input = new RecorderInputCapture(fake, delegate(string kind, string key) { emitted++; return true; });
        Check(!input.ManualMarker() && emitted == 0, "Marker was active before recording ready.");
        input.Start(true);
        Check(fake.Registered && fake.Hooked && input.ManualMarker() && emitted == 1, "Ready capture did not register its hotkey/hook.");
        for (int key = 0; key < 256; key++)
        {
            foreach (int message in new int[] { 0x100, 0x104, 0x101, 0x105, -1 })
            {
                string filtered = RecorderInputCapture.FilterKey(key, message, true, true);
                bool expected = (key == 0x4d || key == 0x1b) && (message == 0x100 || message == 0x104);
                Check((filtered != null) == expected, "The key whitelist or key-down filter leaked an input.");
                Check(RecorderInputCapture.FilterKey(key, message, false, true) == null, "An inactive key was recorded.");
                Check(RecorderInputCapture.FilterKey(key, message, true, false) == null, "A background key was recorded.");
            }
            fake.Callback(key, RecorderInputCapture.KeyDown);
        }
        Check(emitted == 3, "The keyboard callback recorded more than M and Esc.");
        fake.Foreground = false;
        fake.Callback(0x4d, RecorderInputCapture.KeyDown);
        Check(!input.ManualMarker() && emitted == 3, "A marker escaped the target-foreground guard.");
        fake.Foreground = true;
        input.Dispose();
        fake.Callback(0x4d, RecorderInputCapture.KeyDown);
        Check(!input.ManualMarker() && emitted == 3 && fake.Disposed && !fake.Registered && !fake.Hooked,
            "Stop did not immediately disable all capture paths.");
        input.Dispose();

        FakeInput optional = new FakeInput();
        using (RecorderInputCapture capture = new RecorderInputCapture(optional, delegate { return true; }))
        { capture.Start(false); Check(optional.Registered && !optional.Hooked && optional.Callback == null, "Disabled menu keys installed a hook."); }
        foreach (int failure in new int[] { 0, 1, 2, 3 })
        {
            FakeInput broken = new FakeInput { RegisterFails = failure == 0, HookFails = failure == 1,
                UnregisterFails = failure == 2, RemoveFails = failure == 3 };
            RecorderInputCapture capture = new RecorderInputCapture(broken, delegate { return true; });
            if (failure < 2) Refuses(delegate { capture.Start(true); }, "An input initialization error was ignored.");
            else { capture.Start(true); Refuses(capture.Dispose, "An input cleanup error was hidden."); }
            Check(broken.Disposed && !broken.Registered && !broken.Hooked, "Input cleanup skipped a resource after an exception.");
            Check(!capture.ManualMarker(), "Input remained active after an exceptional stop.");
            broken.RegisterFails = broken.HookFails = broken.UnregisterFails = broken.RemoveFails = false;
            capture.Dispose();
            Check(broken.Disposed && !broken.Registered && !broken.Hooked, "A retry did not release failed input cleanup.");
        }
    }

    private static void TestIdentity()
    {
        long frequency = Stopwatch.Frequency;
        Check(RecorderReadyIdentity.IsReady("recording", 12, 34, 12, 34, 100, 101, frequency), "Valid backend ready identity failed.");
        Check(!RecorderReadyIdentity.IsReady("preparing", 12, 34, 12, 34, 100, 101, frequency), "Preparing was treated as recording ready.");
        Check(!RecorderReadyIdentity.IsReady("saving", 12, 34, 12, 34, 100, 101, frequency), "Saving re-enabled input capture.");
        Check(!RecorderReadyIdentity.IsReady("recording", 13, 34, 12, 34, 100, 101, frequency), "Another backend PID was trusted.");
        Check(!RecorderReadyIdentity.IsReady("recording", 12, 35, 12, 34, 100, 101, frequency), "Reused backend PID was trusted.");
        Check(!RecorderReadyIdentity.IsReady("recording", 12, 34, 12, 34, 100, 99, frequency), "Invalid ready clock was trusted.");
        Check(!RecorderReadyIdentity.IsReady("recording", 12, 34, 12, 34, 100, 101, frequency + 1), "A different QPC clock was trusted.");
    }

    private static Dictionary<string, object> ReadJson(string path)
    { return new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(path)); }
    private static long Number(Dictionary<string, object> value, string key) { return Convert.ToInt64(value[key]); }

    private sealed class NumericCase
    {
        internal readonly string Name;
        internal readonly byte[] Bytes;
        internal readonly int Frames;
        internal readonly bool Valid;
        internal NumericCase(string name, byte[] bytes, int frames, bool valid)
        { Name = name; Bytes = bytes; Frames = frames; Valid = valid; }
    }
    private static NumericCase AsciiCase(string name, string text, int frames, bool valid)
    { return new NumericCase(name, Encoding.ASCII.GetBytes(text), frames, valid); }

    private static void TestAsyncFrames(string root)
    {
        string switches = Path.Combine(root, "numeric-switches"); Directory.CreateDirectory(switches);
        string path = Path.Combine(switches, "AsyncCopyFrames"), audit = Path.Combine(root, "numeric-route-results.jsonl");
        RecorderRouteSwitcher route = RecorderRouteSwitcher.ForTests(switches, Path.Combine(root, "numeric-switch.log"), delegate { return false; }, true);
        NumericCase[] cases =
        {
            AsciiCase("ascii-1", "1", 1, true), AsciiCase("ascii-2", "2", 2, true), AsciiCase("empty", "", 2, false),
            AsciiCase("ascii-whitespace", " \t\r\n", 2, false), AsciiCase("padded-1", " \t1\r\n", 1, true),
            AsciiCase("padded-2", " \t2\r\n", 2, true), AsciiCase("16-byte-1-spaces", "1" + new string(' ', 15), 1, true),
            AsciiCase("17-byte-1-spaces", "1" + new string(' ', 16), 2, false), AsciiCase("larger-valid-prefix", "1" + new string(' ', 100), 2, false),
            AsciiCase("zero", "0", 2, false), AsciiCase("invalid-3", "3", 2, false), AsciiCase("double-digit", "01", 2, false),
            AsciiCase("trailing-x", "1x", 2, false), AsciiCase("two-values", "1 2", 2, false), AsciiCase("embedded-nul", "1\0", 2, false),
            AsciiCase("vertical-tab", "1\v", 2, false), AsciiCase("form-feed", "1\f", 2, false),
            new NumericCase("unicode-nbsp", Encoding.UTF8.GetBytes("1\u00a0"), 2, false),
            new NumericCase("utf16-le-bom", new byte[] { 0xff, 0xfe, 0x31, 0 }, 2, false),
            new NumericCase("utf16-le", new byte[] { 0x31, 0 }, 2, false),
            new NumericCase("utf16-be-bom", new byte[] { 0xfe, 0xff, 0, 0x31 }, 2, false),
            new NumericCase("utf8-bom-1", new byte[] { 0xef, 0xbb, 0xbf, 0x31, 0x0d, 0x0a }, 1, true),
            new NumericCase("utf8-bom-2", new byte[] { 0xef, 0xbb, 0xbf, 0x20, 0x32, 0x09 }, 2, true),
            new NumericCase("utf8-bom-only", new byte[] { 0xef, 0xbb, 0xbf }, 2, false),
            new NumericCase("bom-after-space", new byte[] { 0x20, 0xef, 0xbb, 0xbf, 0x31 }, 2, false),
            new NumericCase("double-bom", new byte[] { 0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf, 0x31 }, 2, false),
            new NumericCase("invalid-utf8", new byte[] { 0x31, 0xff }, 2, false)
        };
        foreach (NumericCase item in cases)
        {
            bool valid;
            int frames = RecorderRouteConfiguration.ParseAsyncFrames(item.Bytes, item.Bytes.Length, out valid);
            Check(frames == item.Frames && valid == item.Valid, "Raw numeric parser disagreed with native flags: " + item.Name);
            File.WriteAllBytes(path, item.Bytes);
            string expected = item.Valid ? "K" + item.Frames : "同步 + 共享", actual = route.CurrentRoute();
            Check(item.Valid ? actual == expected : actual.StartsWith(expected + "（AsyncCopyFrames", StringComparison.Ordinal),
                "File route or fallback reason disagreed with native flags: " + item.Name);
            File.AppendAllText(audit, new JavaScriptSerializer().Serialize(new Dictionary<string, object>
            {
                { "test", item.Name }, { "byteCount", item.Bytes.Length }, { "expectedFromNativeFlags", expected },
                { "valid", item.Valid }, { "actual", actual }, { "agrees", true }
            }) + "\n", new UTF8Encoding(false));
        }
        File.WriteAllText(path, "1", Encoding.ASCII);
        using (FileStream reader = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            Check(route.CurrentRoute() == "K1", "A compatible read-sharing holder incorrectly changed the native route preview.");
            Refuses(delegate { route.Switch("k2"); }, "Mutation used the permissive sharing mode intended only for route preview.");
            Check(File.ReadAllText(path) == "1", "A shared read preview/refused mutation changed the route file.");
        }
        using (FileStream locked = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        { Check(route.CurrentRoute().StartsWith("同步 + 共享（AsyncCopyFrames 读取失败", StringComparison.Ordinal), "An unreadable numeric file did not default to sync/shared with a reason."); }
        string disableShared = Path.Combine(switches, "DisableSharedCopyPath"); File.WriteAllText(disableShared, "KEEP");
        Check(route.CurrentRoute() == "K1（共享被 DisableSharedCopyPath 禁用）", "K1 failed to expose shared=false.");
        File.WriteAllText(path, "2", Encoding.ASCII);
        Check(route.CurrentRoute() == "K2（共享被 DisableSharedCopyPath 禁用）", "K2 failed to expose shared=false.");
        File.Delete(path);
        Check(route.CurrentRoute() == "同步（共享被 DisableSharedCopyPath 禁用）", "Default sync/shared failed to expose shared=false.");
        string asyncMarker = Path.Combine(switches, "AsyncCopyPath"); File.WriteAllText(asyncMarker, "KEEP");
        Check(route.CurrentRoute() == "K2（共享被 DisableSharedCopyPath 禁用）", "Legacy async opt-in lost its K2 route.");
        File.WriteAllText(path, "invalid", Encoding.ASCII);
        Check(route.CurrentRoute().StartsWith("K2（AsyncCopyFrames 内容无效", StringComparison.Ordinal), "Invalid numeric data masked the legacy async opt-in.");
        File.Delete(path); File.Delete(asyncMarker);
        File.WriteAllText(Path.Combine(switches, "SharedCopySync"), "1");
        Check(route.CurrentRoute() == "同步（共享被 DisableSharedCopyPath 禁用）", "Shared sync ignored the shared-disable override.");
        File.WriteAllText(Path.Combine(switches, "SyncCopyPath"), "1");
        Check(route.CurrentRoute() == "同步", "Legacy Sync lost precedence over shared/numeric flags.");
        File.Delete(Path.Combine(switches, "SyncCopyPath"));
        File.Delete(Path.Combine(switches, "SharedCopySync"));
        string hardlinkTarget = Path.Combine(root, "unsafe-numeric-target.txt");
        File.WriteAllText(hardlinkTarget, "1", Encoding.ASCII);
        Check(CreateHardLink(path, hardlinkTarget, IntPtr.Zero), "Could not create the content-1 hardlink preview fixture.");
        string unsafePreview = route.CurrentRoute();
        Check(unsafePreview.StartsWith("未知（AsyncCopyFrames 安全检查", StringComparison.Ordinal)
            && unsafePreview.Contains("需清理不安全 marker"), "A safety-refused hardlink preview falsely claimed the native K2 fallback.");
        Refuses(delegate { route.Switch("k2"); }, "An unsafe preview still allowed hardlink mutation.");
        Check(File.ReadAllText(path) == "1" && File.ReadAllText(hardlinkTarget) == "1", "The hardlink preview/refusal changed its target.");
        File.Delete(path);
        RecorderRouteSwitcher reparsePreview = RecorderRouteSwitcher.ForTests(Path.Combine(root, "numeric-unsafe-switches"),
            Path.Combine(root, "numeric-reparse-switch.log"), delegate { return false; }, true);
        Check(reparsePreview.CurrentRoute().StartsWith("未知（AsyncCopyFrames 安全检查", StringComparison.Ordinal),
            "A reparse marker preview falsely claimed the native K2 fallback.");
        Refuses(delegate { reparsePreview.Switch("k1"); }, "An unsafe reparse marker was writable through the switcher.");
        bool invalidCount;
        Check(RecorderRouteConfiguration.ParseAsyncFrames(null, 1, out invalidCount) == 2 && !invalidCount,
            "The numeric parser accepted a missing read buffer.");
    }

    private static void TestMarkers(string root)
    {
        string directory = Path.Combine(root, "markers");
        Directory.CreateDirectory(directory);
        RecorderMarkerWriter writer = new RecorderMarkerWriter(directory, true, null, 256);
        Refuses(delegate { using (FileStream held = new FileStream(Path.Combine(directory, "markers.jsonl"), FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { } },
            "The backend could package a marker file while its writer was still open.");
        Check(writer.Enqueue("manual", null) && writer.Enqueue("key", "M") && writer.Enqueue("key", "Esc"), "Valid markers were refused.");
        Check(!writer.Enqueue("key", "A") && !writer.Enqueue("text", "secret"), "Non-whitelisted key data was stored.");
        writer.CloseAsync().GetAwaiter().GetResult();
        Check(!writer.Enqueue("manual", null), "A marker was accepted after Stop.");
        string[] lines = File.ReadAllLines(Path.Combine(directory, "markers.jsonl"));
        Check(lines.Length == 3, "The marker queue did not drain exactly once.");
        for (int index = 0; index < lines.Length; index++)
        {
            Dictionary<string, object> marker = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(lines[index]);
            Check(Number(marker, "sequence") == index + 1 && Number(marker, "qpcTicks") > 0
                && Number(marker, "qpcFrequency") == Stopwatch.Frequency && marker.ContainsKey("utc") && (bool)marker["testOnly"],
                "A marker lost sequence/clock/test provenance.");
            Check(index == 0 ? !marker.ContainsKey("key") : (string)marker["key"] == (index == 1 ? "M" : "Esc"), "A marker stored unexpected key data.");
        }
        Dictionary<string, object> closed = ReadJson(Path.Combine(directory, "markers.closed.json"));
        Check((bool)closed["closed"] && Number(closed, "accepted") == 3 && Number(closed, "written") == 3
            && Number(closed, "dropped") == 0 && Number(closed, "unwritten") == 0, "The close handshake did not report the complete queue.");
        using (FileStream released = new FileStream(Path.Combine(directory, "markers.jsonl"), FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
        checks++;
        writer.CloseAsync().GetAwaiter().GetResult();

        string overflow = Path.Combine(root, "markers-overflow"); Directory.CreateDirectory(overflow);
        RecorderMarkerWriter small = new RecorderMarkerWriter(overflow, true, null, 1);
        for (int index = 0; index < 10000; index++) small.Enqueue("key", "M");
        small.CloseAsync().GetAwaiter().GetResult();
        closed = ReadJson(Path.Combine(overflow, "markers.closed.json"));
        Check(Number(closed, "dropped") > 0 && Number(closed, "accepted") + Number(closed, "dropped") == 10000
            && Number(closed, "written") == Number(closed, "accepted"), "A bounded queue lost marks without reporting its drop count.");
        string unstarted = Path.Combine(root, "markers-unstarted"); Directory.CreateDirectory(unstarted);
        RecorderMarkerWriter.CloseWithoutStarting(unstarted, true, "TEST ONLY failed hotkey registration");
        Check(Number(ReadJson(Path.Combine(unstarted, "markers.closed.json")), "written") == 0
            && !File.Exists(Path.Combine(unstarted, "markers.jsonl")), "Stop before ready created a fake marker stream.");
        Check((string)ReadJson(Path.Combine(unstarted, "markers.closed.json"))["inputError"] == "TEST ONLY failed hotkey registration",
            "An input capture error vanished from the marker close report.");

        string exited = Path.Combine(root, "marker-owner-exited"); Directory.CreateDirectory(exited);
        using (Process owner = Process.Start(new ProcessStartInfo(Process.GetCurrentProcess().MainModule.FileName,
            "--marker-owner-exit \"" + exited + "\"") { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden }))
        { Check(owner.WaitForExit(5000) && owner.ExitCode == 0, "The owned marker-writer exit fixture did not exit."); }
        using (FileStream released = new FileStream(Path.Combine(exited, "markers.jsonl"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        { Check(released.Length > 0, "An abrupt owner exit failed to leave a complete marker line."); }
        Check(!File.Exists(Path.Combine(exited, "markers.closed.json")), "Abrupt owner exit falsely reported a clean writer shutdown.");
    }

    private static void TestRoutes(string root)
    {
        string switches = Path.Combine(root, "switches"), log = Path.Combine(root, "route-switch.jsonl");
        Directory.CreateDirectory(switches);
        string other = Path.Combine(switches, "do-not-touch.txt"); File.WriteAllText(other, "KEEP");
        bool running = false;
        RecorderRouteSwitcher route = RecorderRouteSwitcher.ForTests(switches, log, delegate { return running; }, true);
        route.Switch("sync"); Check(route.CurrentRoute() == "同步", "Sync route was not set.");
        route.Switch("k1"); Check(route.CurrentRoute() == "K1" && File.ReadAllText(Path.Combine(switches, "AsyncCopyFrames")) == "1"
            && !File.Exists(Path.Combine(switches, "SyncCopyPath")), "K1 route files were not exact.");
        route.Switch("k2"); Check(route.CurrentRoute() == "K2" && File.ReadAllText(Path.Combine(switches, "AsyncCopyFrames")) == "2", "K2 did not explicitly enable async with two frames.");
        route.Switch("sync-shared"); Check(route.CurrentRoute() == "同步 + 共享" && File.Exists(Path.Combine(switches, "SharedCopySync"))
            && !File.Exists(Path.Combine(switches, "SyncCopyPath")) && !File.Exists(Path.Combine(switches, "AsyncCopyFrames")), "Shared sync route did not use its independent file switch.");
        File.WriteAllText(Path.Combine(switches, "SyncCopyPath"), "1");
        Check(route.CurrentRoute() == "同步", "Legacy SyncCopyPath lost highest priority.");
        route.Switch("k2"); Check(!File.Exists(Path.Combine(switches, "SharedCopySync")), "K2 left shared sync active.");
        route.Switch("sync-shared"); route.Switch("sync");
        Check(!File.Exists(Path.Combine(switches, "SharedCopySync")) && !File.Exists(Path.Combine(switches, "AsyncCopyFrames")), "Sync retained another route override.");
        string disableShared = Path.Combine(switches, "DisableSharedCopyPath");
        File.WriteAllText(disableShared, "KEEP");
        File.WriteAllText(Path.Combine(switches, "SharedCopySync"), "1");
        File.Delete(Path.Combine(switches, "SyncCopyPath"));
        Check(route.CurrentRoute().Contains("共享被 DisableSharedCopyPath 禁用"), "A disabled shared path was advertised as active sharing.");
        Refuses(delegate { route.Switch("sync-shared"); }, "The route switch ignored an external shared-copy disable override.");
        Check(File.ReadAllText(disableShared) == "KEEP", "The route switch modified a non-whitelisted disable override.");
        route.Switch("sync");
        File.Delete(disableShared);
        running = true;
        string before = File.ReadAllText(Path.Combine(switches, "SyncCopyPath"));
        Refuses(delegate { route.Switch("k1"); }, "A running game did not block route modification.");
        Check(File.ReadAllText(Path.Combine(switches, "SyncCopyPath")) == before && !File.Exists(Path.Combine(switches, "AsyncCopyFrames")), "A refused live switch still changed files.");
        string[] logRows = File.ReadAllLines(log);
        Dictionary<string, object> refusal = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(logRows[logRows.Length - 1]);
        Check(refusal["before"] != null && refusal["after"] != null && ((string)refusal["rejection"]).Contains("Endfield.exe"), "A refusal lost its before/after evidence or reason.");
        running = false;
        Refuses(delegate { route.Switch("../outside"); }, "The route whitelist accepted path traversal.");
        RecorderRouteSwitcher unsupported = RecorderRouteSwitcher.ForTests(switches, log, delegate { return false; }, false);
        Refuses(delegate { unsupported.Switch("sync-shared"); }, "An unverified shared route was enabled.");
        using (RecorderRouteDirectory directory = RecorderRouteDirectory.Open(switches, false))
        { Refuses(delegate { directory.Write("do-not-touch.txt", "BAD"); }, "The file whitelist was bypassed."); }
        Check(File.ReadAllText(other) == "KEEP", "A non-whitelisted file changed.");

        int queries = 0;
        RecorderRouteSwitcher race = RecorderRouteSwitcher.ForTests(switches, log, delegate { return ++queries >= 3; }, true);
        Refuses(delegate { race.Switch("k1"); }, "The game was not rechecked immediately before writing.");
        Check(!File.Exists(Path.Combine(switches, "AsyncCopyFrames")), "A stale no-game result allowed a write.");

        string absent = Path.Combine(root, "absent-live-switches");
        RecorderRouteSwitcher liveAbsent = RecorderRouteSwitcher.ForTests(absent, log, delegate { return true; }, true);
        Refuses(delegate { liveAbsent.Switch("sync"); }, "A running game allowed switch-directory creation.");
        Check(!Directory.Exists(absent), "A refused switch created its directory.");
        Refuses(delegate { RecorderRouteSwitcher.ForTests(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "EndfieldXeSSDiag"), log, delegate { return false; }, true); },
            "A mock route test accepted real ProgramData.");

        string linked = Path.Combine(switches, "AsyncCopyFrames");
        Check(CreateHardLink(linked, other, IntPtr.Zero), "Could not create the owned hardlink rejection fixture.");
        Refuses(delegate { route.Switch("k2"); }, "A hardlink route file was modified.");
        Check(File.ReadAllText(other) == "KEEP", "Hardlink rejection occurred after data was changed.");
        File.Delete(linked);

        Directory.CreateDirectory(linked);
        Refuses(delegate { route.Switch("k1"); }, "A route file path containing a directory was accepted.");
        Check(File.ReadAllText(other) == "KEEP", "Directory rejection changed unrelated data.");
        Directory.Delete(linked);

        string junction = Path.Combine(root, "junction-switches");
        Check(Directory.Exists(junction), "The owned junction fixture is missing.");
        RecorderRouteSwitcher reparse = RecorderRouteSwitcher.ForTests(junction, log, delegate { return false; }, true);
        Refuses(delegate { reparse.Switch("k2"); }, "A reparse switch directory was followed.");
        string nestedJunction = Path.Combine(root, "junction-parent", "switches");
        RecorderRouteSwitcher ancestor = RecorderRouteSwitcher.ForTests(nestedJunction, log, delegate { return false; }, true);
        Refuses(delegate { ancestor.Switch("sync"); }, "A reparse parent directory was followed.");
        Check(File.ReadAllText(other) == "KEEP", "A rejected directory traversal changed unrelated data.");
    }

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 2 && args[0] == "--marker-owner-exit")
            {
                RecorderMarkerWriter owner = new RecorderMarkerWriter(args[1], true, null, 4);
                owner.Enqueue("manual", null);
                string markerFile = Path.Combine(args[1], "markers.jsonl");
                DateTime deadline = DateTime.UtcNow.AddSeconds(2);
                while (new FileInfo(markerFile).Length == 0 && DateTime.UtcNow < deadline) Thread.Sleep(10);
                GC.KeepAlive(owner);
                Environment.Exit(0);
            }
            if (args.Length != 1 || !Directory.Exists(args[0])) throw new ArgumentException("Use the owned test folder created by test-recorder-controls.ps1.");
            TestInput(); TestIdentity(); TestMarkers(args[0]); TestAsyncFrames(args[0]); TestRoutes(args[0]);
            RecorderControlJson.Write(Path.Combine(args[0], "controls-test-result.json"), new Dictionary<string, object>
            { { "passed", true }, { "checks", checks }, { "liveKeyboardHooks", false }, { "realProgramDataWrites", false },
              { "gameStarted", false }, { "newEtwSessions", 0 }, { "utc", DateTime.UtcNow.ToString("o") } });
            Console.WriteLine("Recorder controls: " + checks + " checks passed.");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
