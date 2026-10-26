#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace xess_vk::diagnostics {
enum class Stream : unsigned { Verbose, Detail, Summary, Observation, Count };
struct WriterStats {
    std::uint64_t accepted{},written{},writeFailures{},pending{},maximumWriteNs{};
    std::array<std::uint64_t,4> dropped{};
    bool active{};
};
// The queue lock covers only index updates and one bounded producer copy; the
// sink and the worker's copy run without it. Producers therefore wait at most
// for another bounded copy, never for storage, and lose records only when the
// queue is full or stopped.
class AsyncLogWriter {
public:
    using Sink=bool(*)(void*,Stream,const char*,DWORD) noexcept;
    using Finished=void(*)(void*,const WriterStats&) noexcept;
    explicit AsyncLogWriter(unsigned capacity=128) noexcept:capacity_(capacity){}
    ~AsyncLogWriter(){stop(INFINITE);}
    AsyncLogWriter(const AsyncLogWriter&)=delete;
    AsyncLogWriter& operator=(const AsyncLogWriter&)=delete;
    bool start(Sink sink,void* user,Finished finished=nullptr) noexcept {
        if(thread_){if(WaitForSingleObject(thread_,0)!=WAIT_OBJECT_0)return accepting_.load();CloseHandle(thread_);thread_=nullptr;}
        if(!capacity_||capacity_>1024||!sink)return false;
        try{entries_=std::make_unique<Entry[]>(capacity_);}catch(...){return false;}
        sink_=sink;user_=user;finished_=finished;head_=tail_=count_=0;stopping_=false;module_=nullptr;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);frequency_=frequency.QuadPart;
        HMODULE owner{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&threadEntry),&owner)){entries_.reset();return false;}
        if(owner!=GetModuleHandleW(nullptr)&&!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&threadEntry),&module_)){entries_.reset();return false;}
        accepting_.store(true);active_.store(true);
        thread_=CreateThread(nullptr,0,threadEntry,this,0,nullptr);
        if(!thread_){accepting_.store(false);active_.store(false);if(module_)FreeLibrary(module_);module_=nullptr;entries_.reset();return false;}
        return true;
    }
    bool enqueue(Stream stream,const char* data,DWORD bytes) noexcept {
        const auto error=GetLastError();const auto index=static_cast<unsigned>(stream);
        if(index>=dropped_.size()){SetLastError(error);return false;}
        if(!data||!bytes||bytes>recordBytes||!accepting_.load()){
            dropped_[index].fetch_add(1);SetLastError(error);return false;
        }
        AcquireSRWLockExclusive(&lock_);
        const bool room=accepting_.load()&&count_<capacity_;
        if(room){auto& entry=entries_[tail_];entry.stream=stream;entry.bytes=bytes;std::memcpy(entry.data.data(),data,bytes);
            tail_=(tail_+1)%capacity_;++count_;accepted_.fetch_add(1);pending_.fetch_add(1);}
        else dropped_[index].fetch_add(1);
        ReleaseSRWLockExclusive(&lock_);
        if(room)WakeConditionVariable(&wake_);
        SetLastError(error);return room;
    }
    // Called only at startup/instance destruction, never from a Present or DllMain.
    bool stop(DWORD timeoutMs=2000) noexcept {
        if(!thread_)return true;
        AcquireSRWLockExclusive(&lock_);accepting_.store(false);stopping_=true;ReleaseSRWLockExclusive(&lock_);
        WakeConditionVariable(&wake_);
        if(WaitForSingleObject(thread_,timeoutMs)!=WAIT_OBJECT_0)return false;
        CloseHandle(thread_);thread_=nullptr;entries_.reset();return true;
    }
    WriterStats stats() const noexcept {
        WriterStats out{accepted_.load(),written_.load(),writeFailures_.load(),pending_.load(),maximumWriteNs_.load()};
        for(unsigned i=0;i<out.dropped.size();++i)out.dropped[i]=dropped_[i].load();
        out.active=active_.load();return out;
    }
private:
    static constexpr DWORD recordBytes=32768;
    struct Entry {Stream stream{};DWORD bytes{};std::array<char,recordBytes> data{};};
    SRWLOCK lock_=SRWLOCK_INIT;CONDITION_VARIABLE wake_=CONDITION_VARIABLE_INIT;
    std::unique_ptr<Entry[]> entries_;
    const unsigned capacity_;unsigned head_{},tail_{},count_{};bool stopping_{};
    HANDLE thread_{};HMODULE module_{};Sink sink_{};Finished finished_{};void* user_{};LONGLONG frequency_{};
    std::atomic<bool> accepting_{},active_{};
    std::atomic<std::uint64_t> accepted_{},written_{},writeFailures_{},pending_{},maximumWriteNs_{};
    std::array<std::atomic<std::uint64_t>,4> dropped_{};
    void run() noexcept {
        SetThreadDescription(GetCurrentThread(),L"XeSS async log writer");
        Entry entry;
        for(;;){
            AcquireSRWLockExclusive(&lock_);
            while(!count_&&!stopping_)SleepConditionVariableSRW(&wake_,&lock_,INFINITE,0);
            if(!count_&&stopping_){ReleaseSRWLockExclusive(&lock_);break;}
            // Producers write only at tail_; the head slot stays counted, so it
            // cannot be overwritten while it is copied outside the lock.
            const auto& source=entries_[head_];
            ReleaseSRWLockExclusive(&lock_);
            entry.stream=source.stream;entry.bytes=source.bytes;std::memcpy(entry.data.data(),source.data.data(),source.bytes);
            AcquireSRWLockExclusive(&lock_);head_=(head_+1)%capacity_;--count_;ReleaseSRWLockExclusive(&lock_);
            LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
            if(sink_(user_,entry.stream,entry.data.data(),entry.bytes))written_.fetch_add(1);else writeFailures_.fetch_add(1);
            QueryPerformanceCounter(&end);
            const auto ns=frequency_?static_cast<std::uint64_t>((end.QuadPart-begin.QuadPart)*1e9/frequency_):0;
            if(ns>maximumWriteNs_.load())maximumWriteNs_.store(ns);
            pending_.fetch_sub(1);
        }
        active_.store(false);
        if(finished_)finished_(user_,stats());
    }
    static DWORD WINAPI threadEntry(void* argument) noexcept {
        auto* self=static_cast<AsyncLogWriter*>(argument);self->run();
        // Keep callback code resident if a bounded shutdown timed out on storage.
        if(const auto module=self->module_)FreeLibraryAndExitThread(module,0);
        return 0;
    }
};
}
