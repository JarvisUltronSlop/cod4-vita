#pragma once
#include <vita/vita_sha256.h>
#include <array>
#include <cstdint>

namespace kisak::vita {
struct VitaTextureIdentity {
    VitaSha256::Digest digest{};
    uint32_t width=0, height=0, levels=0, format=0, flags=0, bytes=0;
    uint32_t storageClass=0; // Protected images must never alias evictable storage.
    bool operator==(const VitaTextureIdentity& other) const {
        return digest==other.digest && width==other.width && height==other.height
            && levels==other.levels && format==other.format && flags==other.flags && bytes==other.bytes
            && storageClass==other.storageClass;
    }
};

// Distinct opaque leases may share one immutable GPU image. Releasing a lease
// never invalidates another owner's copy. Slot generations reject stale map
// handles, including after Clear, and never wrap back to an old generation.
template<size_t Images=2048, size_t Leases=4095>
class VitaTexturePool {
    static_assert(Images>0 && Images<65535 && Leases>0 && Leases<=4095);
public:
    uint32_t Acquire(const VitaTextureIdentity& key) {
        for (size_t i=0; i<Images; ++i)
            if (images_[i].references && images_[i].key==key) return NewLease(i);
        return 0;
    }
    uint32_t Insert(const VitaTextureIdentity& key, uint32_t native, uint32_t certificate=0, uint32_t backing=0) {
        if (!native && !backing) return 0;
        for (size_t i=0; i<Images; ++i) {
            if (images_[i].references) continue;
            images_[i].key=key; images_[i].native=native; images_[i].certificate=certificate;images_[i].backing=backing;
            const uint32_t lease=NewLease(i);
            if (lease) { if(native)resident_+=key.bytes; ++count_; }
            else images_[i]={};
            return lease;
        }
        return 0;
    }
    uint32_t Resolve(uint32_t handle) const {
        const Lease* lease=Find(handle);
        return lease ? images_[lease->image-1].native : 0;
    }
    uint32_t Certificate(uint32_t handle) const {
        const Lease* lease=Find(handle);
        return lease ? images_[lease->image-1].certificate : 0;
    }
    const VitaTextureIdentity* Identity(uint32_t handle) const {
        const auto* lease=Find(handle);return lease?&images_[lease->image-1].key:nullptr;
    }
    uint32_t Backing(uint32_t handle) const {
        const auto* lease=Find(handle);return lease?images_[lease->image-1].backing:0;
    }
    bool SameImage(uint32_t a,uint32_t b) const {
        const auto* first=Find(a);const auto* second=Find(b);
        return first && second && first->image==second->image;
    }
    bool SetResident(uint32_t handle,uint32_t native) {
        auto* lease=Find(handle);if(!lease || !native)return false;
        auto& image=images_[lease->image-1];if(image.native)return false;
        image.native=native;resident_+=image.key.bytes;shared_+=uint64_t(image.references-1)*image.key.bytes;return true;
    }
    uint32_t Evict(uint32_t handle) {
        auto* lease=Find(handle);if(!lease)return 0;
        auto& image=images_[lease->image-1];if(!image.backing || !image.native)return 0;
        auto native=image.native;image.native=0;resident_-=image.key.bytes;
        shared_-=uint64_t(image.references-1)*image.key.bytes;return native;
    }
    template<typename Visit> void VisitResident(Visit visit) const {
        for(size_t image=0;image<Images;++image)if(images_[image].native && images_[image].backing)
            for(size_t slot=0;slot<Leases;++slot)if(leaseSlots_[slot].image==image+1) {
                visit((leaseSlots_[slot].generation<<12)|uint32_t(slot+1));break;
            }
    }
    // Returns the GPU object only when its final lease has been released.
    uint32_t Release(uint32_t handle,uint32_t* backing=nullptr) {
        if(backing)*backing=0;
        Lease* lease=Find(handle);
        if (!lease) return 0;
        Image& image=images_[lease->image-1];
        lease->image=0; --leases_;
        if (--image.references) { if(image.native)shared_-=image.key.bytes; return 0; }
        const uint32_t native=image.native;
        if(backing)*backing=image.backing;
        if(native)resident_-=image.key.bytes; --count_; image={};
        return native;
    }
    template<typename Delete>
    void Clear(Delete deleteNative) {
        for (Image& image:images_) {
            if (image.references) deleteNative(image.native);
            image={};
        }
        for (Lease& lease:leaseSlots_) lease.image=0;
        resident_=shared_=0; count_=leases_=0;
    }
    uint64_t ResidentBytes() const { return resident_; }
    uint64_t SharedBytes() const { return shared_; }
    uint32_t ImageCount() const { return count_; }
    uint32_t LeaseCount() const { return leases_; }
private:
    struct Image { VitaTextureIdentity key{}; uint32_t native=0, references=0, certificate=0,backing=0; };
    struct Lease { uint32_t generation=0; uint16_t image=0; };
    uint32_t NewLease(size_t image) {
        for (size_t i=0; i<Leases; ++i) {
            Lease& lease=leaseSlots_[i];
            if (lease.image || lease.generation==0xfffff) continue;
            lease.image=static_cast<uint16_t>(image+1); ++lease.generation;
            if (images_[image].references++ && images_[image].native) shared_+=images_[image].key.bytes;
            ++leases_;
            return (lease.generation<<12)|static_cast<uint32_t>(i+1);
        }
        return 0;
    }
    const Lease* Find(uint32_t handle) const {
        const uint32_t slot=handle&0xfff;
        if (!slot || slot>Leases) return nullptr;
        const Lease& lease=leaseSlots_[slot-1];
        return lease.image && lease.generation==(handle>>12) ? &lease : nullptr;
    }
    Lease* Find(uint32_t handle) {
        return const_cast<Lease*>(static_cast<const VitaTexturePool*>(this)->Find(handle));
    }
    std::array<Image, Images> images_{};
    std::array<Lease, Leases> leaseSlots_{};
    uint64_t resident_=0, shared_=0;
    uint32_t count_=0, leases_=0;
};
} // namespace kisak::vita
