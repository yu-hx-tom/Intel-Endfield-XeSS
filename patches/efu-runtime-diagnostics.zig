// Local EFU diagnostic support. Only the native worker writes this bounded log.
// Game callbacks publish atomic counters and never call this module.
const std = @import("std");
const windows = std.os.windows;
const HANDLE = windows.HANDLE;
extern "kernel32" fn GetEnvironmentVariableW([*:0]const u16, [*]u16, u32) callconv(.winapi) u32;
extern "kernel32" fn CreateFileW([*:0]const u16, u32, u32, ?*anyopaque, u32, u32, ?HANDLE) callconv(.winapi) HANDLE;
extern "kernel32" fn WriteFile(HANDLE, [*]const u8, u32, *u32, ?*anyopaque) callconv(.winapi) windows.BOOL;
extern "kernel32" fn CloseHandle(HANDLE) callconv(.winapi) windows.BOOL;
extern "kernel32" fn GetCurrentProcessId() callconv(.winapi) u32;
extern "kernel32" fn GetTickCount64() callconv(.winapi) u64;

var log_file: ?HANDLE = null;
var written_lines: u32 = 0;
var written_bytes: usize = 0;

pub fn now() u64 { return GetTickCount64(); }
pub fn init() void {
    var path: [1024]u16 = undefined;
    const name = std.unicode.utf8ToUtf16LeStringLiteral("EFU_RUNTIME_LOG");
    const count = GetEnvironmentVariableW(name, &path, path.len);
    if (count == 0 or count >= path.len) return;
    const handle = CreateFileW(@ptrCast(&path), 4, 7, null, 4, 0x80, null);
    if (@intFromPtr(handle) == std.math.maxInt(usize)) return;
    log_file = handle;
}
pub fn close() void {
    if (log_file) |handle| _ = CloseHandle(handle);
    log_file = null;
}
fn append(line: []const u8) void {
    const handle = log_file orelse return;
    if (written_lines >= 128 or written_bytes + line.len > 64 * 1024) return;
    var wrote: u32 = 0;
    _ = WriteFile(handle, line.ptr, @intCast(line.len), &wrote, null);
    written_lines += 1;
    written_bytes += line.len;
}
pub fn event(stage: []const u8, detail: []const u8, result: i64) void {
    var buffer: [1024]u8 = undefined;
    const line = std.fmt.bufPrint(&buffer,
        "{{\"event\":\"{s}\",\"detail\":\"{s}\",\"code\":{d},\"pid\":{d},\"uptime_ms\":{d}}}\n",
        .{ stage, detail, result, GetCurrentProcessId(), now() }) catch return;
    append(line);
}
pub fn target(module_base: usize, address: usize) void {
    const prefix: *const [32]u8 = @ptrFromInt(address);
    const hex = std.fmt.bytesToHex(prefix.*, .lower);
    event("target_ready", &hex, @intCast(address - module_base));
}
pub fn counters(entered: u64, enabled_seen: u64, clear_called: u64, clear_returned: u64, toggles: u64, enabled: bool) void {
    var buffer: [1024]u8 = undefined;
    const line = std.fmt.bufPrint(&buffer,
        "{{\"event\":\"dither_counters\",\"entered\":{d},\"enabled_seen\":{d},\"force_clear_called\":{d},\"force_clear_returned\":{d},\"toggles\":{d},\"enabled\":{},\"pid\":{d},\"uptime_ms\":{d}}}\n",
        .{ entered, enabled_seen, clear_called, clear_returned, toggles, enabled, GetCurrentProcessId(), now() }) catch return;
    append(line);
}
