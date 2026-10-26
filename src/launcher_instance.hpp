#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <utility>

namespace xess_vk::launcher_instance {
inline constexpr wchar_t windowClass[] = L"EndfieldXeSSLauncherWindow";
inline constexpr wchar_t activationName[] = L"EndfieldXeSSLauncher.Activate.v1";

inline std::wstring mutexName() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Cannot read launcher user identity");
    DWORD bytes{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    const bool obtained = bytes && GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes);
    CloseHandle(token);
    if (!obtained) throw std::runtime_error("Cannot read launcher user SID");
    LPWSTR text{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text))
        throw std::runtime_error("Cannot encode launcher user SID");
    std::wstring name = L"Local\\EndfieldXeSSLauncher.";
    name += text;
    LocalFree(text);
    return name;
}

class Gate {
    HANDLE mutex_{};
    bool owns_{};
    std::wstring windowClass_;
    UINT activation_{};
public:
    Gate(const std::wstring& name, std::wstring klass, UINT activation)
        : windowClass_(std::move(klass)), activation_(activation) {
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) throw std::runtime_error("Cannot create launcher instance lock");
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    ~Gate() { if (owns_) ReleaseMutex(mutex_); if (mutex_) CloseHandle(mutex_); }

    // The mutex spans startup and shutdown; a second process waits for the first window.
    bool enter() {
        const auto deadline = GetTickCount64() + 5000;
        do {
            const auto result = WaitForSingleObject(mutex_, 0);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { owns_ = true; return true; }
            if (result != WAIT_TIMEOUT) throw std::runtime_error("Cannot acquire launcher instance lock");
            if (const auto window = FindWindowW(windowClass_.c_str(), nullptr)) {
                DWORD process{};
                GetWindowThreadProcessId(window, &process);
                AllowSetForegroundWindow(process);
                DWORD_PTR reply{};
                if (SendMessageTimeoutW(window, activation_, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &reply))
                    return false;
                throw std::runtime_error("The existing launcher is not responding. Please try again shortly.");
            }
            Sleep(25);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error("The launcher is still starting. Please try again shortly.");
    }
};
}
