#include <vita/vita_texture_pool.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void Check(bool value) { if (!value) { std::fprintf(stderr,"texture contract failed\n"); std::abort(); } }
static std::string Hex(const kisak::vita::VitaSha256::Digest& digest) {
    std::string value;
    for (uint8_t byte: digest) { value+="0123456789abcdef"[byte>>4]; value+="0123456789abcdef"[byte&15]; }
    return value;
}
int main() {
    using namespace kisak::vita;
    const struct { const char* input; const char* digest; } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}
    };
    for (const auto& v:vectors) {
        const std::string bytes(v.input);
        Check(Hex(VitaSha256::Hash(bytes.data(),bytes.size()))==v.digest);
        for (size_t chunk=1; chunk<=65; ++chunk) {
            VitaSha256 stream;
            for (size_t at=0; at<bytes.size(); at+=chunk)
                stream.Update(bytes.data()+at,std::min(chunk,bytes.size()-at));
            Check(Hex(stream.Finish())==v.digest);
        }
    }
    const std::string million(1000000,'a');
    Check(Hex(VitaSha256::Hash(million.data(),million.size()))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    VitaTexturePool<2,3> pool;
    VitaTextureIdentity image{VitaSha256::Hash("test",4),64,64,7,123,0,256};
    auto other=image; other.width=32; // Same payload, different upload layout.
    const uint32_t a=pool.Insert(image,15), b=pool.Acquire(image), c=pool.Insert(other,16);
    Check(a && b && c && a!=b && pool.Resolve(a)==15 && pool.Resolve(b)==15 && pool.Resolve(c)==16);
    Check(pool.ResidentBytes()==512 && pool.SharedBytes()==256 && pool.ImageCount()==2 && pool.LeaseCount()==3);
    Check(!pool.Acquire(image)); // Full lease table cannot steal another owner.
    Check(!pool.Insert(other,17));
    Check(!pool.Release(a) && !pool.Resolve(a) && pool.Resolve(b)==15);
    const uint32_t replacement=pool.Acquire(image);
    Check(replacement && replacement!=a && !pool.Release(a) && pool.Resolve(replacement)==15);
    Check(!pool.Release(b) && pool.Release(replacement)==15);
    Check(pool.Release(c)==16 && !pool.ResidentBytes() && !pool.SharedBytes());
    uint32_t old=0;
    for (unsigned cycle=0; cycle<10000; ++cycle) {
        const uint32_t first=pool.Insert(image,21), second=pool.Acquire(image);
        Check(first && second && !pool.Resolve(old));
        unsigned deleted=0;
        pool.Clear([&](uint32_t native){Check(native==21); ++deleted;});
        Check(deleted==1 && !pool.Resolve(first) && !pool.Resolve(second) && !pool.LeaseCount());
        old=first;
    }
    const auto paged=pool.Insert(image,0,233,42), alias=pool.Acquire(image);
    Check(paged && alias && pool.SameImage(paged,alias));
    Check(!pool.Resolve(paged) && !pool.ResidentBytes() && pool.Backing(alias)==42);
    Check(pool.SetResident(paged,27) && pool.Resolve(alias)==27);
    Check(pool.ResidentBytes()==256 && pool.SharedBytes()==256);
    Check(!pool.SetResident(alias,28));
    Check(pool.Evict(alias)==27 && !pool.Resolve(paged) && !pool.SharedBytes());
    Check(pool.Identity(paged) && pool.Certificate(alias)==233);
    Check(pool.SetResident(alias,29));
    uint32_t backing=999;
    Check(!pool.Release(paged,&backing) && !backing && pool.Resolve(alias)==29);
    Check(pool.Release(alias,&backing)==29 && backing==42 && !pool.ResidentBytes());
    Check(!pool.SetResident(paged,30) && !pool.Evict(alias));
    const auto fixed=pool.Insert(image,31);
    Check(fixed && !pool.Evict(fixed) && pool.Resolve(fixed)==31);
    auto pageableIdentity=image;pageableIdentity.storageClass=1;
    Check(!pool.Acquire(pageableIdentity));
    const auto isolated=pool.Insert(pageableIdentity,0,0,99);
    Check(isolated && !pool.SameImage(fixed,isolated) && pool.SetResident(isolated,32));
    Check(pool.Evict(isolated)==32 && pool.Resolve(fixed)==31);
    std::puts("SHA-256, leases, exhaustion, 10000 generations and shared paged residency passed");
}
