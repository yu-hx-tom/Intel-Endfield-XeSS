#pragma once
#include <windows.h>
#include <cwchar>
#include <cstdint>
#include <atomic>

namespace xess_vk::measurement {
enum class LogStream { Detail, Summary, Observation };
// Caller serializes writes. Rotation never depends on the verbose log's byte budget.
class Log {
    HANDLE file_{INVALID_HANDLE_VALUE};
    std::uint64_t bytes_{},limit_,writtenBytes_{},rotations_{};
    std::atomic<std::uint64_t> failures_{};
    unsigned index_{};
    LogStream stream_;
public:
    explicit Log(std::uint64_t limit=16ull*1024*1024,LogStream stream=LogStream::Detail) noexcept:limit_(limit),stream_(stream){}
    ~Log(){if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);}
    Log(const Log&)=delete;
    Log& operator=(const Log&)=delete;
    std::uint64_t failures() const noexcept{return failures_.load(std::memory_order_relaxed);}
    std::uint64_t writtenBytes() const noexcept{return writtenBytes_;}
    std::uint64_t rotations() const noexcept{return rotations_;}
    bool write(const wchar_t* prefix,const char* data,DWORD size) noexcept {
        if(!prefix||!*prefix||!data||!size||size>limit_||wcsnlen(prefix,32768)+48>=32768){++failures_;return false;}
        if(file_==INVALID_HANDLE_VALUE||size>limit_-bytes_){
            if(file_!=INVALID_HANDLE_VALUE){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;index_^=1;++rotations_;}
            wchar_t path[32768]{};
            std::swprintf(path,32768,stream_==LogStream::Summary?L"%ls.measurements-summary-%u.jsonl":
                stream_==LogStream::Observation?L"%ls.sr-observation-%u.jsonl":L"%ls.measurements-%u.jsonl",prefix,index_);
            file_=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);bytes_=0;
        }
        DWORD written{};
        if(file_==INVALID_HANDLE_VALUE||!WriteFile(file_,data,size,&written,nullptr)||written!=size){
            if(file_!=INVALID_HANDLE_VALUE){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}
            ++failures_;return false;
        }
        bytes_+=written;writtenBytes_+=written;return true;
    }
};
class SummaryLog final : public Log {
public:
    explicit SummaryLog(std::uint64_t limit=2ull*1024*1024) noexcept:Log(limit,LogStream::Summary){}
};
}
