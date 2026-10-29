#include "fsr4_xmx.hpp"
#include "sr_backend.hpp"
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool condition,const char* what){if(!condition)throw std::runtime_error(what);}
void check(HRESULT result,const char* what){if(FAILED(result))throw std::runtime_error(std::string(what)+": "+std::to_string(static_cast<unsigned>(result)));}
struct Event {HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}};
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
}
float half(std::uint16_t value){
    const auto exponent=(value>>10)&31, mantissa=value&1023;const float magnitude=exponent==31?(mantissa?NAN:INFINITY):
        exponent?std::ldexp(1.0f+float(mantissa)/1024,int(exponent)-15):std::ldexp(float(mantissa),-24);
    return value&0x8000?-magnitude:magnitude;
}
}
int wmain(int argc,wchar_t** argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try {
        if(argc==2&&std::wstring_view(argv[1])==L"--self-test"){
            using namespace xess_vk::sr_backend;
            require(parse(L"")==Kind::XeSS&&parse(L"xess")==Kind::XeSS,"default SR backend changed");
            require(parse(L"fsr4_xmx_lite")==Kind::Fsr4XmxLite,"Lite startup selection rejected");
            require(parse(L"fsr4")==Kind::Invalid&&parse(L"fsr4_xmx_lite_skip")==Kind::Invalid,"unselected model admitted");
            require(xess_vk::fsr4::validConfiguration({640,360,1280,720}),"valid extent");
            require(!xess_vk::fsr4::validConfiguration({0,360,1280,720}),"zero input rejected");
            require(!xess_vk::fsr4::validConfiguration({1920,1080,1280,720}),"downscale rejected");
            require(!xess_vk::fsr4::validConfiguration({640,360,32768,720}),"oversize rejected");
            bool refused=false;try{xess_vk::fsr4::Context relative(L"relative");}catch(const std::invalid_argument&){refused=true;}
            require(refused,"relative DLL path rejected");std::cout<<"FSR4 XMX CPU checks passed\n";return 0;
        }
        require(argc==2,"Usage: xess-fsr4-xmx-test <absolute-payload-directory> | --self-test");
        ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Create DXGI factory");
        ComPtr<ID3D12Device> device;DXGI_ADAPTER_DESC1 adapterDesc{};
        for(UINT n=0;;++n){ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(n,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Adapter description");
            if(desc.VendorId==0x8086&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))){adapterDesc=desc;break;}}
        require(device.Get()!=nullptr,"No Intel D3D12 adapter");
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};
        check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"Create queue");
        ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Create allocator");
        ComPtr<ID3D12GraphicsCommandList> commands;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"Create command list");
        ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create fence");Event event;require(event.value!=nullptr,"Create event");
        const xess_vk::fsr4::Configuration configuration{640,360,1280,720};
        const std::array formats{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
        std::array<ComPtr<ID3D12Resource>,4> images;std::array<ComPtr<ID3D12Resource>,3> uploads;
        auto buffer=[&](std::uint64_t bytes,D3D12_HEAP_TYPE heapType,D3D12_RESOURCE_STATES state){
            D3D12_HEAP_PROPERTIES heap{};heap.Type=heapType;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width=bytes;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> result;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&result)),"Create buffer");return result;
        };
        for(unsigned n=0;n<4;++n){const bool output=n==3;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=output?configuration.outputWidth:configuration.inputWidth;
            desc.Height=output?configuration.outputHeight:configuration.inputHeight;desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=formats[n];desc.SampleDesc.Count=1;
            desc.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&images[n])),"Create image");
            if(output)continue;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows{};UINT64 rowSize{},bytes{};device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowSize,&bytes);
            uploads[n]=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped{};D3D12_RANGE none{};check(uploads[n]->Map(0,&none,&mapped),"Map input");
            std::memset(mapped,0,bytes);
            for(unsigned y=0;y<configuration.inputHeight;++y)for(unsigned x=0;x<configuration.inputWidth;++x){auto* pixel=static_cast<std::uint8_t*>(mapped)+footprint.Footprint.RowPitch*y+x*(n?4:8);
                if(n==0){const std::array<std::uint16_t,4> value{std::uint16_t(x<configuration.inputWidth/2?0x3400:0x3800),0x3800,0x3a00,0x3c00};std::memcpy(pixel,value.data(),8);}
                else if(n==1){const float value=0.5f;std::memcpy(pixel,&value,4);}}
            uploads[n]->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=uploads[n].Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprint;
            D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=images[n].Get();target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);const auto b=barrier(images[n].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);commands->ResourceBarrier(1,&b);
        }
        xess_vk::fsr4::Context sdk{std::filesystem::path(argv[1])};sdk.initialize(device.Get(),configuration);
        xess_vk::fsr4::Frame frame;frame.commands=commands.Get();frame.color=images[0].Get();frame.depth=images[1].Get();frame.motion=images[2].Get();frame.output=images[3].Get();
        frame.velocityScaleX=float(configuration.inputWidth);frame.velocityScaleY=float(configuration.inputHeight);frame.reset=true;sdk.record(frame);
        const auto toCopy=barrier(images[3].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->ResourceBarrier(1,&toCopy);
        const auto desc=images[3]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows{};UINT64 rowSize{},bytes{};device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowSize,&bytes);
        auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=images[3].Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=footprint;
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);check(commands->Close(),"Close commands");
        ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),1),"Signal completion");check(fence->SetEventOnCompletion(1,event.value),"Set completion event");
        require(WaitForSingleObject(event.value,30000)==WAIT_OBJECT_0,"FSR4 XMX GPU completion timed out");
        check(device->GetDeviceRemovedReason(),"D3D12 device status");void* mapped{};const D3D12_RANGE range{0,SIZE_T(bytes)};check(readback->Map(0,&range,&mapped),"Map output");
        unsigned finite=0,nonzero=0,samples=0;float minimum=INFINITY,maximum=-INFINITY;
        for(unsigned y=16;y<configuration.outputHeight;y+=31)for(unsigned x=16;x<configuration.outputWidth;x+=37){std::array<std::uint16_t,4> pixel{};
            std::memcpy(pixel.data(),static_cast<const std::uint8_t*>(mapped)+y*footprint.Footprint.RowPitch+x*8,8);
            for(unsigned channel=0;channel<3;++channel){const float value=half(pixel[channel]);++samples;if(std::isfinite(value)){++finite;minimum=std::min(minimum,value);maximum=std::max(maximum,value);if(value>0.001f)++nonzero;}}}
        const D3D12_RANGE noWrites{};readback->Unmap(0,&noWrites);require(finite==samples&&nonzero>samples/2&&maximum>minimum+0.01f,"FSR4 XMX output is invalid or empty");
        std::cout<<"{\"passed\":true,\"adapter_device\":"<<adapterDesc.DeviceId<<",\"provider\":\""<<sdk.selected().name<<"\",\"samples\":"<<samples<<",\"finite\":"<<finite<<",\"nonzero\":"<<nonzero<<",\"minimum\":"<<minimum<<",\"maximum\":"<<maximum<<"}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FSR4 XMX test failed: "<<e.what()<<'\n';return 1;}
}
