#include "async_log.hpp"
#include "measurement_log.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
using namespace xess_vk::diagnostics;
namespace {
unsigned checks{};
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Capture {
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr),gate=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::vector<std::string> records;WriterStats drained;DWORD sinkThread{};bool failBad{},finished{};
    ~Capture(){SetEvent(gate);CloseHandle(entered);CloseHandle(gate);}
    static bool sink(void* pointer,Stream,const char* data,DWORD bytes) noexcept {
        auto& self=*static_cast<Capture*>(pointer);self.sinkThread=GetCurrentThreadId();SetEvent(self.entered);
        WaitForSingleObject(self.gate,INFINITE);
        if(self.failBad&&std::string_view(data,bytes)=="bad")return false;
        try{self.records.emplace_back(data,bytes);return true;}catch(...){return false;}
    }
    static void end(void* pointer,const WriterStats& stats) noexcept {
        auto& self=*static_cast<Capture*>(pointer);self.drained=stats;self.finished=true;
    }
};
void blockedStorage(){
    Capture capture;AsyncLogWriter writer(2);
    struct Release {HANDLE event;~Release(){SetEvent(event);}} release{capture.gate};
    check(writer.start(Capture::sink,&capture,Capture::end),"Start blocked writer");
    check(writer.enqueue(Stream::Detail,"first",5),"First record admitted");
    check(WaitForSingleObject(capture.entered,2000)==WAIT_OBJECT_0,"Sink did not enter");
    const auto done=CreateEventW(nullptr,TRUE,FALSE,nullptr);bool a{},b{},c{};DWORD preserved{};
    std::thread producer([&]{SetLastError(0xbeef);a=writer.enqueue(Stream::Summary,"second",6);
        b=writer.enqueue(Stream::Observation,"third",5);c=writer.enqueue(Stream::Detail,"full",4);
        preserved=GetLastError();SetEvent(done);});
    const bool returned=WaitForSingleObject(done,500)==WAIT_OBJECT_0;
    // Always release the fake disk before asserting, including a producer-blocking regression.
    const auto before=writer.stats();const bool boundedStop=writer.stop(10);
    SetEvent(capture.gate);producer.join();CloseHandle(done);
    check(writer.stop(3000),"Drain after storage recovered");
    check(returned&&a&&b&&!c,"A blocked sink or full queue blocked its producer");
    check(preserved==0xbeef,"Enqueue changed caller LastError");
    check(!boundedStop&&before.pending==3&&before.dropped[1]==1,"Full queue/drop/timeout not explicit");
    check(capture.records==std::vector<std::string>{"first","second","third"},"Accepted records reordered or lost on shutdown");
    check(capture.sinkThread!=GetCurrentThreadId(),"Disk sink ran on the producer thread");
    check(capture.finished&&capture.drained.accepted==3&&capture.drained.written==3&&!capture.drained.pending,"Final drain did not account for all accepted records");
    check(!writer.enqueue(Stream::Detail,"late",4)&&writer.stats().dropped[1]==2,"Post-shutdown data was silently accepted");
}
void errorsAndRestart(){
    Capture capture;SetEvent(capture.gate);capture.failBad=true;AsyncLogWriter writer;
    check(!writer.start(nullptr,nullptr),"Null sink accepted");
    check(writer.start(Capture::sink,&capture,Capture::end),"Start error fixture");
    check(writer.enqueue(Stream::Detail,"bad",3),"Bad backend record not admitted");
    check(writer.stop(3000),"Error fixture drain");
    check(writer.stats().writeFailures==1&&writer.stats().written==0&&!writer.stats().pending,"Sink error disappeared");
    check(writer.start(Capture::sink,&capture,Capture::end),"Sequential instance could not restart writer");
    check(writer.enqueue(Stream::Summary,"good",4)&&writer.stop(3000),"Restarted writer did not drain");
    check(writer.stats().accepted==2&&writer.stats().written==1&&writer.stats().writeFailures==1,"Restart reset process failure history");
}
void concurrentProducers(){
    Capture capture;SetEvent(capture.gate);AsyncLogWriter writer(16);
    check(writer.start(Capture::sink,&capture),"Start concurrent writer");
    std::vector<std::thread> producers;
    for(unsigned n=0;n<4;++n)producers.emplace_back([&]{for(unsigned i=0;i<1000;++i)writer.enqueue(Stream::Detail,"record",6);});
    for(auto& thread:producers)thread.join();
    check(writer.stop(3000),"Concurrent writer did not drain");const auto stats=writer.stats();
    check(stats.accepted+stats.dropped[1]==4000&&stats.written==stats.accepted&&!stats.pending&&!stats.writeFailures,
        "Concurrent admission/loss/persistence accounting is inconsistent");
    check(capture.records.size()==stats.written,"Reported writes differ from persisted records");
}
// Bursts of records with a little formatting work between them, while the
// woken worker copies the previous one: the in-game measurement-window burst
// where try-lock admission lost up to 13% of the later records in each burst.
// The queue never fills here, so any loss is contention loss.
void contendedAdmission(){
    Capture capture;SetEvent(capture.gate);AsyncLogWriter writer(1024);
    check(writer.start(Capture::sink,&capture),"Start contended writer");
    const std::string record(4096,'x');constexpr unsigned count=1000;volatile std::uint64_t formatting{};
    std::thread producer([&]{for(unsigned i=0;i<count;++i){
        for(unsigned spin=0;spin<2000;++spin)formatting=formatting+spin; // stands in for JSON formatting
        writer.enqueue(Stream::Detail,record.data(),static_cast<DWORD>(record.size()));}});
    producer.join();
    check(writer.stop(10000),"Contended writer did not drain");const auto stats=writer.stats();
    check(stats.accepted==count&&stats.dropped[1]==0&&stats.written==count&&capture.records.size()==count,
        "Lock contention dropped records while the queue had room");
}
void actualFiles(){
    const auto root=std::filesystem::temp_directory_path()/(L"xefg-async-log-"+std::to_wstring(GetCurrentProcessId()));
    check(std::filesystem::create_directory(root),"Temporary directory already exists");
    struct Files {std::wstring prefix;xess_vk::measurement::Log detail{}; xess_vk::measurement::SummaryLog summary{};};
    {
        Files files{(root/L"observer.jsonl").wstring()};AsyncLogWriter writer;
        const auto sink=[](void* pointer,Stream stream,const char* data,DWORD bytes) noexcept {
            auto& files=*static_cast<Files*>(pointer);
            return stream==Stream::Summary?files.summary.write(files.prefix.c_str(),data,bytes):files.detail.write(files.prefix.c_str(),data,bytes);
        };
        check(writer.start(sink,&files),"Start actual-file writer");
        check(writer.enqueue(Stream::Detail,"detail\n",7),"Queue actual detail");
        // Stop/join is the persistence boundary, not merely successful admission.
        check(writer.stop(3000)&&writer.stats().written==1,"Actual detail was not persisted");
        check(writer.start(sink,&files)&&writer.enqueue(Stream::Summary,"summary\n",8)&&writer.stop(3000),"Actual summary was not persisted");
    }
    for(const auto& entry:std::filesystem::directory_iterator(root)){
        std::ifstream input(entry.path(),std::ios::binary);const std::string text((std::istreambuf_iterator<char>(input)),{});
        check(text==(entry.path().filename().wstring().find(L"summary")!=std::wstring::npos?"summary\n":"detail\n"),"Wrong persisted log stream");
        input.close();std::filesystem::remove(entry.path());
    }
    std::filesystem::remove(root);
}
}
int main(){try{blockedStorage();errorsAndRestart();concurrentProducers();contendedAdmission();actualFiles();
    std::printf("{\"passed\":true,\"checks\":%u,\"blockedSinkDidNotBlockProducer\":true,\"shutdownDrainVerified\":true,\"gameTouched\":false}\n",checks);return 0;
}catch(const std::exception& error){std::fprintf(stderr,"Async log: %s\n",error.what());return 1;}}
