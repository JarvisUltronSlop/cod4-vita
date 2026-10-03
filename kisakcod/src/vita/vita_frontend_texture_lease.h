#pragma once

#include <gfx/gfx_backend.h>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <vector>

namespace kisak::vita {

// The common menu, two buttons and font atlas outlive every selected map.
// Transfer ownership before map teardown; borrowed handles never enter the
// map's destruction list. No additional allocation is needed for this lease.
class VitaFrontendTextureLease {
public:
    bool Adopt(std::vector<GfxTextureHandle>& mapOwned,
               std::initializer_list<GfxTextureHandle> textures) {
        auto next = textures_;
        for (const auto handle : textures) {
            if (!handle.value || Contains(next, handle)) continue;
            if (std::none_of(mapOwned.begin(), mapOwned.end(),
                    [&](auto item) { return item.value == handle.value; }))
                return false;
            const auto empty = std::find_if(next.begin(), next.end(),
                [](auto item) { return item.value == 0; });
            if (empty == next.end()) return false;
            *empty = handle;
        }
        textures_ = next;
        mapOwned.erase(std::remove_if(mapOwned.begin(), mapOwned.end(),
            [&](auto item) { return Contains(textures_, item); }), mapOwned.end());
        return true;
    }

    void Release(const GfxBackendApi* gfx) {
        for (auto& handle : textures_) {
            if (handle.value && gfx && gfx->DestroyTexture)
                gfx->DestroyTexture(handle);
            handle = {};
        }
    }

private:
    static bool Contains(const std::array<GfxTextureHandle, 4>& items,
                         GfxTextureHandle handle) {
        return handle.value && std::any_of(items.begin(), items.end(),
            [&](auto item) { return item.value == handle.value; });
    }
    std::array<GfxTextureHandle, 4> textures_{};
};

} // namespace kisak::vita
