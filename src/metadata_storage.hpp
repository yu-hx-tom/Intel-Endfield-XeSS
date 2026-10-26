#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace xess_vk::metadata_storage {
// Allocate payload only when a slot is first used. Pages never move or shrink
// while the tracker lives, so retained object pointers and slot IDs stay valid.
template<class T,std::size_t N> class Slots {
    static constexpr std::size_t pageSlots=std::clamp<std::size_t>(65536/sizeof(T),1,256);
    std::array<std::unique_ptr<T[]>,(N+pageSlots-1)/pageSlots> pages_{};
    std::size_t allocated_{},extent_{};
public:
    T* get(std::size_t i) noexcept {return i<N&&pages_[i/pageSlots]?&pages_[i/pageSlots][i%pageSlots]:nullptr;}
    const T* get(std::size_t i)const noexcept {return i<N&&pages_[i/pageSlots]?&pages_[i/pageSlots][i%pageSlots]:nullptr;}
    T* ensure(std::size_t i) noexcept {
        if(i>=N)return nullptr;
        const auto page=i/pageSlots;
        if(!pages_[page])try{
            const auto count=std::min(pageSlots,N-page*pageSlots);
            pages_[page]=std::make_unique<T[]>(count);allocated_+=count;
            extent_=std::max(extent_,page*pageSlots+count);
        }catch(...){return nullptr;}
        return get(i);
    }
    T& operator[](std::size_t i) noexcept{return *get(i);}
    const T& operator[](std::size_t i)const noexcept{return *get(i);}
    std::size_t allocatedBytes()const noexcept{return allocated_*sizeof(T);}
    std::size_t extent()const noexcept{return extent_;}
};

// Keep the existing half-full handle index and dense/free slot bookkeeping.
// Only large object payloads are paged; lookups neither allocate nor relocate.
template<class T,std::size_t N> struct Table {
    static_assert(N&&!(N&(N-1)));
    static constexpr auto none=UINT32_MAX;
    Slots<T,N> entries;
    std::array<std::uint32_t,N*2> index{};
    std::array<std::uint32_t,N> freeNext{},dense{},denseAt{};
    std::uint32_t freeHead{};std::size_t limit{N},live{};
    Table(){index.fill(none);for(std::size_t i=0;i<N;++i)freeNext[i]=i+1<N?static_cast<std::uint32_t>(i+1):none;}
    static std::size_t bucket(std::uint64_t value) noexcept {
        value^=value>>30;value*=UINT64_C(0xbf58476d1ce4e5b9);value^=value>>27;value*=UINT64_C(0x94d049bb133111eb);
        return static_cast<std::size_t>(value^(value>>31))&(N*2-1);
    }
    std::size_t position(std::uint64_t handle)const noexcept {
        auto n=bucket(handle);while(index[n]!=none&&entries[index[n]].key.handle!=handle)n=(n+1)&(N*2-1);return n;
    }
    T* find(std::uint64_t h) noexcept {if(!h)return nullptr;const auto i=index[position(h)];return i==none?nullptr:&entries[i];}
    const T* find(std::uint64_t h)const noexcept {if(!h)return nullptr;const auto i=index[position(h)];return i==none?nullptr:&entries[i];}
    T* insert(decltype(T{}.key) key) noexcept {
        if(live>=limit||freeHead==none)return nullptr;
        const auto slot=freeHead;auto* row=entries.ensure(slot);if(!row)return nullptr;
        freeHead=freeNext[slot];index[position(key.handle)]=slot;dense[live]=slot;denseAt[slot]=static_cast<std::uint32_t>(live);
        *row={};row->key=key;++live;return row;
    }
    void erase(std::uint64_t h) noexcept {
        if(!h)return;auto hole=position(h);if(index[hole]==none)return;
        const auto old=index[hole];entries[old]={};freeNext[old]=freeHead;freeHead=old;
        const auto at=denseAt[old],last=dense[live-1];dense[at]=last;denseAt[last]=at;--live;
        for(auto scan=(hole+1)&(N*2-1);index[scan]!=none;scan=(scan+1)&(N*2-1)){
            const auto home=bucket(entries[index[scan]].key.handle);
            if(((hole-home)&(N*2-1))<((scan-home)&(N*2-1))){index[hole]=index[scan];hole=scan;}
        }
        index[hole]=none;
    }
    std::size_t allocatedBytes()const noexcept{return entries.allocatedBytes();}
};
}
