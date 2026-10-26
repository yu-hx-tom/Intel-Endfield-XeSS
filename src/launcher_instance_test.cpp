#include "launcher_instance.hpp"
#include <cstdio>
#include <vector>

namespace {
unsigned activations{}, checks{};
UINT activationMessage{};
void check(bool good, const char* why) {
    ++checks;
    if (!good) throw std::runtime_error(why);
}
LRESULT CALLBACK fixtureProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == activationMessage) { ++activations; return 1; }
    return DefWindowProcW(window, message, wp, lp);
}
HANDLE spawn(const std::wstring& name, const std::wstring& klass, const std::wstring& ready) {
    wchar_t executable[32768]{};
    check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Fixture executable path");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + name + L"\" \"" + klass + L"\" \"" + ready + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &process) != FALSE, "Start secondary fixture");
    CloseHandle(process.hThread);
    return process.hProcess;
}
void waitAndPump(HANDLE process) {
    const auto deadline = GetTickCount64() + 8000;
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(1, &process, FALSE, 50, QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    }
    DWORD result = 99;
    const bool finished = WaitForSingleObject(process, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &result);
    CloseHandle(process);
    check(finished && result == 0, "Secondary activates first process and exits");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        using xess_vk::launcher_instance::Gate;
        activationMessage = RegisterWindowMessageW(xess_vk::launcher_instance::activationName);
        if (argc == 5 && std::wstring(argv[1]) == L"--child") {
            HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]);
            if (!ready) return 3;
            SetEvent(ready); CloseHandle(ready);
            Gate secondary(argv[2], argv[3], activationMessage);
            return secondary.enter() ? 4 : 0;
        }
        const auto suffix = std::to_wstring(GetCurrentProcessId());
        const auto name = L"Local\\EndfieldXeSSLauncher.Test." + suffix;
        const auto klass = L"EndfieldXeSSLauncher.TestWindow." + suffix;
        const auto readyName = L"Local\\EndfieldXeSSLauncher.TestReady." + suffix;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        check(ready != nullptr, "Create child-start event");
        WNDCLASSW wc{}; wc.lpfnWndProc = fixtureProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = klass.c_str();
        check(RegisterClassW(&wc) != 0, "Register private fixture window");
        {
            Gate primary(name, klass, activationMessage);
            check(primary.enter(), "First process owns instance lock");
            HANDLE child = spawn(name, klass, readyName);
            check(WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0, "Secondary started before first window exists");
            Sleep(100);
            HWND window = CreateWindowW(klass.c_str(), L"Single-instance regression fixture", 0,
                                        0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
            check(window != nullptr, "Create late fixture window without showing it");
            waitAndPump(child);
            check(activations == 1, "Startup race sends one activation");
            std::vector<HANDLE> children;
            for (int i = 0; i < 3; ++i) children.push_back(spawn(name, klass, readyName));
            for (auto process : children) waitAndPump(process);
            check(activations == 4, "Repeated launches activate the same first window");
            DestroyWindow(window);
        }
        {
            Gate replacement(name, klass, activationMessage);
            check(replacement.enter(), "Normal exit releases lock for next launch");
        }
        check(xess_vk::launcher_instance::mutexName().starts_with(L"Local\\EndfieldXeSSLauncher.S-"), "Instance lock is scoped to current Windows user");
        CloseHandle(ready);
        UnregisterClassW(klass.c_str(), wc.hInstance);
        std::printf("{\"passed\":true,\"checks\":%u,\"secondaryProcesses\":4,\"activations\":%u,\"gameStarted\":false}\n", checks, activations);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Launcher instance test: %s\n", error.what()); return 1;
    }
}
