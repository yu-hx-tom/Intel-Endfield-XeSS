using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

// Read-only PDH/process sampling, adapted from out/20261016-low-stutter/SystemSampler.cs.
public sealed class RecorderSystemSampler : IDisposable
{
    [StructLayout(LayoutKind.Sequential)] private struct CounterValue { public uint Status; public double Value; }
    [StructLayout(LayoutKind.Sequential)] private struct IoCounters
    {
        public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] private struct ThreadEntry
    {
        public uint Size, Usage, Id, OwnerPid;
        public int BasePriority, DeltaPriority;
        public uint Flags;
    }
    [StructLayout(LayoutKind.Sequential)] private struct ServiceStatus
    {
        public uint Type, State, ControlsAccepted, Win32ExitCode, ServiceExitCode, CheckPoint, WaitHint, Pid, Flags;
    }
    [StructLayout(LayoutKind.Sequential)] private struct ServiceEntry
    {
        public IntPtr Name, DisplayName;
        public ServiceStatus Status;
    }
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhOpenQueryW(string source, IntPtr user, out IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhAddEnglishCounterW(IntPtr query, string path, IntPtr user, out IntPtr counter);
    [DllImport("pdh.dll")] private static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll")] private static extern uint PdhGetFormattedCounterValue(IntPtr counter, uint format, out uint type, out CounterValue value);
    [DllImport("pdh.dll")] private static extern uint PdhCloseQuery(IntPtr query);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessTimes(IntPtr process, out long created, out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessIoCounters(IntPtr process, out IoCounters counters);
    [DllImport("kernel32.dll")] private static extern uint WaitForSingleObject(IntPtr process, uint milliseconds);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern ushort GetActiveProcessorGroupCount();
    [DllImport("kernel32.dll")] private static extern uint GetActiveProcessorCount(ushort group);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool Thread32First(IntPtr snapshot, ref ThreadEntry entry);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool Thread32Next(IntPtr snapshot, ref ThreadEntry entry);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern uint GetProcessIdOfThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetThreadTimes(IntPtr thread, out long created, out long exited, out long kernel, out long user);
    [DllImport("ntdll.dll")] private static extern int NtQueryInformationThread(IntPtr thread, int infoClass, out IntPtr information, int size, IntPtr length);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr OpenSCManagerW(string machine, string database, uint access);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool EnumServicesStatusExW(IntPtr manager, int level, uint type, uint state, IntPtr services, uint bytes, out uint needed, out uint count, ref uint resume, string group);
    [DllImport("advapi32.dll")] private static extern bool CloseServiceHandle(IntPtr handle);

    private sealed class Counter { public string Name; public IntPtr Handle; public double Scale; public uint AddStatus; }
    private sealed class ProcessSample { public long Created, CpuTicks; public IoCounters Io; public bool IoValid; }
    public sealed class LogicalCpu { public int Index, Group, Processor; public string Path; }
    public sealed class ProcessRate
    {
        public string Name, CreationFileTime;
        public int Pid;
        public double? CpuPercent, ReadBytesPerSecond, WriteBytesPerSecond;
    }
    public sealed class ThreadRow
    {
        public uint Tid;
        public string CreationFileTime, StartAddress, CpuError, StartAddressError;
        public long? KernelTime100ns, UserTime100ns, CpuTime100ns;
    }
    public sealed class ThreadSnapshot
    {
        public string State = "ready", Utc, Warning;
        public long QpcTicks;
        public double QueryCostMs;
        public int TargetPid, EnumeratedThreads, CpuReadFailures, StartAddressReadFailures;
        public string TargetCreationFileTime;
        public bool TargetLiveBefore, TargetLiveAfter;
        public List<ThreadRow> Threads = new List<ThreadRow>();
    }
    public sealed class ServiceRow { public string Name, DisplayName; public uint Pid, State; }
    public sealed class ServiceSnapshot
    {
        public string State = "ready", Utc, Warning;
        public long QpcTicks;
        public double QueryCostMs;
        public bool ReadOnly = true;
        public string Source = "SCM EnumServicesStatusEx (Win32 services; start/end only)";
        public List<ServiceRow> Services = new List<ServiceRow>();
    }

    public sealed class Target : IDisposable
    {
        private IntPtr handle;
        public readonly int Pid;
        public readonly ulong CreationFileTime;
        public Target(int pid, ulong creationFileTime)
        {
            Pid = pid; CreationFileTime = creationFileTime;
            if (pid <= 0 || creationFileTime == 0) throw new ArgumentException("Exact target PID and creation FILETIME are required.");
            handle = OpenProcess(0x00101000, false, pid); // SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION.
            long created, exited, kernel, user;
            if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot query target identity.");
            if (!GetProcessTimes(handle, out created, out exited, out kernel, out user) || unchecked((ulong)created) != creationFileTime || !IsLive)
            {
                Dispose();
                throw new InvalidOperationException("Target PID/creation identity is not live or does not match.");
            }
        }
        public bool IsLive { get { return handle != IntPtr.Zero && WaitForSingleObject(handle, 0) == 0x102; } }
        public void Dispose() { if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; } }
        public ThreadSnapshot SnapshotThreads()
        {
            var watch = Stopwatch.StartNew();
            var result = new ThreadSnapshot { TargetPid = Pid, TargetCreationFileTime = CreationFileTime.ToString(),
                Utc = DateTime.UtcNow.ToString("o"), QpcTicks = Stopwatch.GetTimestamp(), TargetLiveBefore = IsLive };
            IntPtr snapshot = new IntPtr(-1);
            try
            {
                if (!result.TargetLiveBefore) throw new InvalidOperationException("Target exited; an end thread snapshot is unavailable.");
                snapshot = CreateToolhelp32Snapshot(4, 0); // TH32CS_SNAPTHREAD, no game memory access.
                if (snapshot == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
                ThreadEntry entry = new ThreadEntry { Size = (uint)Marshal.SizeOf(typeof(ThreadEntry)) };
                bool more = Thread32First(snapshot, ref entry);
                if (!more && Marshal.GetLastWin32Error() != 18) throw new Win32Exception(Marshal.GetLastWin32Error());
                while (more)
                {
                    if (!IsLive) throw new InvalidOperationException("Target exited during thread enumeration.");
                    if (entry.OwnerPid == (uint)Pid)
                    {
                        result.EnumeratedThreads++;
                        result.Threads.Add(ReadThread(entry.Id, (uint)Pid));
                    }
                    entry.Size = (uint)Marshal.SizeOf(typeof(ThreadEntry));
                    more = Thread32Next(snapshot, ref entry);
                }
                if (Marshal.GetLastWin32Error() != 18) throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            catch (Exception error) { result.State = "partial"; result.Warning = error.Message; }
            finally { if (snapshot != new IntPtr(-1)) CloseHandle(snapshot); }
            foreach (ThreadRow row in result.Threads)
            {
                if (row.CpuError != null) result.CpuReadFailures++;
                if (row.StartAddressError != null) result.StartAddressReadFailures++;
            }
            result.TargetLiveAfter = IsLive;
            if (!result.TargetLiveAfter || result.CpuReadFailures != 0 || result.StartAddressReadFailures != 0) result.State = "partial";
            result.QueryCostMs = watch.Elapsed.TotalMilliseconds;
            return result;
        }
    }

    private IntPtr query;
    private uint queryStatus;
    private readonly List<Counter> counters = new List<Counter>();
    private Dictionary<int, ProcessSample> previous = new Dictionary<int, ProcessSample>();
    private long previousTimestamp;
    private long previousCounterTimestamp;
    private readonly int targetPid;
    private readonly ulong targetCreation;
    public readonly List<LogicalCpu> LogicalCpuLayout = new List<LogicalCpu>();
    public readonly Dictionary<string, string> CounterPaths = new Dictionary<string, string>();
    public readonly Dictionary<string, string> CounterErrors = new Dictionary<string, string>();
    public readonly Dictionary<string, string> InitializationErrors = new Dictionary<string, string>();
    public int LogicalProcessors { get { return LogicalCpuLayout.Count; } }
    public int CpuReadFailures { get; private set; }
    public int IoReadFailures { get; private set; }
    public int ProcessesObserved { get; private set; }
    public int? TargetThreadCount { get; private set; }
    public string TargetThreadCountError { get; private set; }
    public double ProcessIntervalSeconds { get; private set; }
    public double CounterQueryCostMs { get; private set; }
    public double CounterIntervalSeconds { get; private set; }
    public double ProcessQueryCostMs { get; private set; }
    public double InitializationCostMs { get; private set; }

    public RecorderSystemSampler(int pid, ulong creation)
    {
        var watch = Stopwatch.StartNew(); targetPid = pid; targetCreation = creation;
        queryStatus = PdhOpenQueryW(null, IntPtr.Zero, out query);
        Add("cpuPercent", @"\Processor(_Total)\% Processor Time");
        for (ushort group = 0; group < GetActiveProcessorGroupCount(); group++)
            for (int cpu = 0; cpu < GetActiveProcessorCount(group); cpu++)
            {
                int index = LogicalCpuLayout.Count;
                string path = @"\Processor Information(" + group + "," + cpu + @")\% Processor Time";
                LogicalCpuLayout.Add(new LogicalCpu { Index = index, Group = group, Processor = cpu, Path = path });
                Add("logicalCpu" + index + "Percent", path);
            }
        Add("dpcPercent", @"\Processor(_Total)\% DPC Time");
        Add("interruptPercent", @"\Processor(_Total)\% Interrupt Time");
        Add("processorQueueLength", @"\System\Processor Queue Length");
        Add("availableMemoryMB", @"\Memory\Available MBytes");
        Add("pagesInputPerSecond", @"\Memory\Pages Input/sec");
        Add("pageReadsPerSecond", @"\Memory\Page Reads/sec");
        Add("diskReadBytesPerSecond", @"\PhysicalDisk(_Total)\Disk Read Bytes/sec");
        Add("diskWriteBytesPerSecond", @"\PhysicalDisk(_Total)\Disk Write Bytes/sec");
        Add("diskReadsPerSecond", @"\PhysicalDisk(_Total)\Disk Reads/sec");
        Add("diskWritesPerSecond", @"\PhysicalDisk(_Total)\Disk Writes/sec");
        Add("diskReadLatencyMs", @"\PhysicalDisk(_Total)\Avg. Disk sec/Read", 1000);
        Add("diskWriteLatencyMs", @"\PhysicalDisk(_Total)\Avg. Disk sec/Write", 1000);
        Add("diskCurrentQueueLength", @"\PhysicalDisk(_Total)\Current Disk Queue Length");
        Add("diskAverageQueueLength", @"\PhysicalDisk(_Total)\Avg. Disk Queue Length");
        Add("c1Percent", @"\Processor Information(_Total)\% C1 Time");
        Add("c2Percent", @"\Processor Information(_Total)\% C2 Time");
        Add("c3Percent", @"\Processor Information(_Total)\% C3 Time");
        Add("processorPerformancePercent", @"\Processor Information(_Total)\% Processor Performance");
        Add("processorInformationTimePercent", @"\Processor Information(_Total)\% Processor Time");
        Add("processorUtilityPercent", @"\Processor Information(_Total)\% Processor Utility");
        Add("interruptsPerSecond", @"\Processor(_Total)\Interrupts/sec");
        Add("contextSwitchesPerSecond", @"\System\Context Switches/sec");
        Add("systemThreadCount", @"\System\Threads");
        if (queryStatus == 0)
        {
            uint prime = PdhCollectQueryData(query);
            previousCounterTimestamp = Stopwatch.GetTimestamp();
            if (prime != 0) InitializationErrors["collection"] = Hex(prime);
        }
        InitializationCostMs = watch.Elapsed.TotalMilliseconds;
    }
    private static string Hex(uint status) { return "0x" + status.ToString("X8"); }
    private void Add(string name, string path, double scale = 1)
    {
        IntPtr handle = IntPtr.Zero;
        uint status = queryStatus != 0 ? queryStatus : PdhAddEnglishCounterW(query, path, IntPtr.Zero, out handle);
        CounterPaths[name] = path;
        counters.Add(new Counter { Name = name, Handle = handle, Scale = scale, AddStatus = status });
        if (status != 0) InitializationErrors[name] = Hex(status);
    }
    private static double? DecodeCounter(uint status, uint valueStatus, double raw, double scale, out string error)
    {
        if (status == 0 && valueStatus > 1) status = valueStatus;
        error = status != 0 ? Hex(status) : (!Double.IsNaN(raw * scale) && !Double.IsInfinity(raw * scale) ? null : "non_finite");
        return error == null ? (double?)(raw * scale) : null;
    }
    public Dictionary<string, double?> ReadCounters()
    {
        var watch = Stopwatch.StartNew();
        var values = new Dictionary<string, double?>(); CounterErrors.Clear();
        uint collected = queryStatus != 0 ? queryStatus : PdhCollectQueryData(query);
        long timestamp = Stopwatch.GetTimestamp();
        CounterIntervalSeconds = previousCounterTimestamp == 0 ? 0 : (timestamp - previousCounterTimestamp) / (double)Stopwatch.Frequency;
        previousCounterTimestamp = timestamp;
        foreach (Counter counter in counters)
        {
            uint type; CounterValue value = new CounterValue();
            uint status = counter.AddStatus != 0 ? counter.AddStatus : collected;
            if (status == 0) status = PdhGetFormattedCounterValue(counter.Handle, 0x8200, out type, out value); // DOUBLE | NOCAP100.
            string error;
            values[counter.Name] = DecodeCounter(status, value.Status, value.Value, counter.Scale, out error);
            if (error != null) CounterErrors[counter.Name] = error;
        }
        CounterQueryCostMs = watch.Elapsed.TotalMilliseconds;
        return values;
    }
    private static double? Rate(long oldValue, long newValue, double seconds, int processors)
    {
        return seconds > 0 && processors > 0 && newValue >= oldValue ? (double?)((newValue - oldValue) / 10000000.0 / seconds / processors * 100) : null;
    }
    public List<ProcessRate> ReadProcesses()
    {
        var watch = Stopwatch.StartNew(); long timestamp = Stopwatch.GetTimestamp();
        ProcessIntervalSeconds = previousTimestamp == 0 ? 0 : (timestamp - previousTimestamp) / (double)Stopwatch.Frequency;
        var current = new Dictionary<int, ProcessSample>(); var rates = new List<ProcessRate>();
        CpuReadFailures = 0; IoReadFailures = 0; ProcessesObserved = 0; TargetThreadCount = null; TargetThreadCountError = "target_not_observed";
        foreach (Process process in Process.GetProcesses())
        {
            IntPtr handle = IntPtr.Zero;
            try
            {
                int pid = process.Id;
                handle = OpenProcess(0x1000, false, pid); // PROCESS_QUERY_LIMITED_INFORMATION only.
                long created, exited, kernel, user;
                if (handle == IntPtr.Zero || !GetProcessTimes(handle, out created, out exited, out kernel, out user)) { CpuReadFailures++; continue; }
                IoCounters io; bool ioValid = GetProcessIoCounters(handle, out io);
                if (!ioValid) IoReadFailures++;
                var sample = new ProcessSample { Created = created, CpuTicks = kernel + user, Io = io, IoValid = ioValid };
                current[pid] = sample; ProcessesObserved++;
                if (pid == targetPid && unchecked((ulong)created) == targetCreation)
                {
                    try { TargetThreadCount = process.Threads.Count; TargetThreadCountError = null; }
                    catch (Exception error) { TargetThreadCountError = error.Message; }
                }
                ProcessSample old;
                if (ProcessIntervalSeconds > 0 && previous.TryGetValue(pid, out old) && old.Created == created)
                {
                    var rate = new ProcessRate { Name = process.ProcessName, Pid = pid, CreationFileTime = unchecked((ulong)created).ToString(),
                        CpuPercent = Rate(old.CpuTicks, sample.CpuTicks, ProcessIntervalSeconds, LogicalProcessors) };
                    if (ioValid && old.IoValid && io.ReadBytes >= old.Io.ReadBytes && io.WriteBytes >= old.Io.WriteBytes)
                    {
                        rate.ReadBytesPerSecond = (io.ReadBytes - old.Io.ReadBytes) / ProcessIntervalSeconds;
                        rate.WriteBytesPerSecond = (io.WriteBytes - old.Io.WriteBytes) / ProcessIntervalSeconds;
                    }
                    rates.Add(rate);
                }
            }
            catch (InvalidOperationException) { CpuReadFailures++; }
            catch (Win32Exception) { CpuReadFailures++; }
            finally { if (handle != IntPtr.Zero) CloseHandle(handle); process.Dispose(); }
        }
        previous = current; previousTimestamp = timestamp;
        ProcessQueryCostMs = watch.Elapsed.TotalMilliseconds;
        return rates;
    }
    private static ThreadRow ReadThread(uint tid, uint ownerPid)
    {
        var result = new ThreadRow { Tid = tid }; IntPtr thread = OpenThread(0x0040, false, tid); // THREAD_QUERY_INFORMATION for the start address.
        bool fullQuery = thread != IntPtr.Zero;
        if (!fullQuery)
        {
            result.StartAddressError = "OpenThread(QUERY_INFORMATION): " + Marshal.GetLastWin32Error();
            thread = OpenThread(0x0800, false, tid); // Limited query can still provide CPU times.
        }
        if (thread == IntPtr.Zero) { result.CpuError = "OpenThread(QUERY_LIMITED_INFORMATION): " + Marshal.GetLastWin32Error(); return result; }
        try
        {
            if (GetProcessIdOfThread(thread) != ownerPid)
            {
                result.CpuError = result.StartAddressError = "Thread exited/reused or owner could not be verified.";
                return result;
            }
            long created, exited, kernel, user;
            if (GetThreadTimes(thread, out created, out exited, out kernel, out user))
            {
                result.CreationFileTime = unchecked((ulong)created).ToString();
                result.KernelTime100ns = kernel; result.UserTime100ns = user; result.CpuTime100ns = kernel + user;
            }
            else result.CpuError = "GetThreadTimes: " + Marshal.GetLastWin32Error();
            if (fullQuery)
            {
                IntPtr address;
                int status = NtQueryInformationThread(thread, 9, out address, IntPtr.Size, IntPtr.Zero);
                if (status == 0 && address != IntPtr.Zero) result.StartAddress = "0x" + unchecked((ulong)address.ToInt64()).ToString("x");
                else result.StartAddressError = status == 0 ? "Start address unavailable." : "NtQueryInformationThread: " + Hex(unchecked((uint)status));
            }
        }
        finally { CloseHandle(thread); }
        return result;
    }
    private static ServiceSnapshot ReadServices()
    {
        var watch = Stopwatch.StartNew();
        var result = new ServiceSnapshot { Utc = DateTime.UtcNow.ToString("o"), QpcTicks = Stopwatch.GetTimestamp() };
        IntPtr manager = IntPtr.Zero, buffer = IntPtr.Zero;
        try
        {
            manager = OpenSCManagerW(null, null, 0x0004); // SC_MANAGER_ENUMERATE_SERVICE, read-only.
            if (manager == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            uint needed, count, resume = 0;
            if (EnumServicesStatusExW(manager, 0, 0x30, 3, IntPtr.Zero, 0, out needed, out count, ref resume, null)) return result;
            if (Marshal.GetLastWin32Error() != 234) throw new Win32Exception(Marshal.GetLastWin32Error());
            for (int attempt = 0; attempt < 3; attempt++)
            {
                if (needed == 0 || needed > 16 * 1024 * 1024) throw new InvalidOperationException("Service enumeration buffer exceeds its bound.");
                uint size = needed; buffer = Marshal.AllocHGlobal((int)size); resume = 0;
                bool success = EnumServicesStatusExW(manager, 0, 0x30, 3, buffer, size, out needed, out count, ref resume, null);
                int error = Marshal.GetLastWin32Error();
                if (success)
                {
                    int stride = Marshal.SizeOf(typeof(ServiceEntry));
                    if (count > size / stride) throw new InvalidOperationException("Service row count exceeds its buffer.");
                    for (int i = 0; i < count; i++)
                    {
                        var row = (ServiceEntry)Marshal.PtrToStructure(IntPtr.Add(buffer, i * stride), typeof(ServiceEntry));
                        result.Services.Add(new ServiceRow { Name = Marshal.PtrToStringUni(row.Name), DisplayName = Marshal.PtrToStringUni(row.DisplayName), Pid = row.Status.Pid, State = row.Status.State });
                    }
                    return result;
                }
                Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero;
                if (error != 234) throw new Win32Exception(error);
            }
            throw new InvalidOperationException("Services changed repeatedly during the bounded snapshot.");
        }
        catch (Exception error) { result.State = "partial"; result.Warning = error.Message; return result; }
        finally
        {
            if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer);
            if (manager != IntPtr.Zero) CloseServiceHandle(manager);
            result.QueryCostMs = watch.Elapsed.TotalMilliseconds;
        }
    }
    public static ServiceSnapshot ReadServicesBounded(int timeoutMilliseconds)
    {
        if (timeoutMilliseconds < 1 || timeoutMilliseconds > 1000) throw new ArgumentOutOfRangeException("timeoutMilliseconds");
        ServiceSnapshot result = null; var watch = Stopwatch.StartNew(); long qpc = Stopwatch.GetTimestamp(); string utc = DateTime.UtcNow.ToString("o");
        var worker = new Thread(delegate() { result = ReadServices(); }); worker.IsBackground = true; worker.Start();
        if (worker.Join(timeoutMilliseconds)) return result;
        // shortcut: a stalled local SCM query is abandoned on a background thread; two snapshots per recorder only.
        return new ServiceSnapshot { State = "partial", Utc = utc, QpcTicks = qpc, QueryCostMs = watch.Elapsed.TotalMilliseconds,
            Warning = "Read-only service PID query exceeded " + timeoutMilliseconds + " ms; it will not delay capture/stop." };
    }
    public static ulong ReadCreationFileTime(int pid)
    {
        IntPtr process = OpenProcess(0x1000, false, pid);
        if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            long created, exited, kernel, user;
            if (!GetProcessTimes(process, out created, out exited, out kernel, out user)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return unchecked((ulong)created);
        }
        finally { CloseHandle(process); }
    }
    public static int SelfTest()
    {
        int checks = 0; string error;
        if (DecodeCounter(0, 0, 2.5, 1000, out error) != 2500 || error != null) throw new Exception("PDH scaling fixture failed."); checks++;
        if (DecodeCounter(0, 1, 202.8, 1, out error) != 202.8 || error != null) throw new Exception("PDH new-data/unclamped fixture failed."); checks++;
        if (DecodeCounter(0, 0xC0000BC6, 0, 1, out error) != null || error != "0xC0000BC6") throw new Exception("PDH missing value became zero."); checks++;
        if (DecodeCounter(5, 0, 3, 1, out error) != null || error != "0x00000005") throw new Exception("PDH API failure was ignored."); checks++;
        if (DecodeCounter(0, 0, Double.NaN, 1, out error) != null || error != "non_finite") throw new Exception("PDH NaN was accepted."); checks++;
        if (DecodeCounter(0, 0, Double.MaxValue, 1000, out error) != null || error != "non_finite") throw new Exception("PDH overflow was accepted."); checks++;
        if (Rate(0, 10000000, 1, 8) != 12.5 || Rate(10, 9, 1, 8) != null || Rate(0, 0, 0, 8) != null) throw new Exception("Process CPU rate fixture failed."); checks++;
        int pid = Process.GetCurrentProcess().Id; ulong created = ReadCreationFileTime(pid);
        using (var target = new Target(pid, created)) { if (!target.IsLive) throw new Exception("Own process identity was rejected."); } checks++;
        bool rejected = false;
        try { using (var target = new Target(pid, created + 1)) { } } catch (InvalidOperationException) { rejected = true; }
        if (!rejected) throw new Exception("Mismatched creation FILETIME was accepted."); checks++;
        rejected = false;
        try { using (var target = new Target(0, created)) { } } catch (ArgumentException) { rejected = true; }
        if (!rejected) throw new Exception("Invalid PID was accepted."); checks++;
        return checks;
    }
    public void Dispose() { if (query != IntPtr.Zero) { PdhCloseQuery(query); query = IntPtr.Zero; } }
}
