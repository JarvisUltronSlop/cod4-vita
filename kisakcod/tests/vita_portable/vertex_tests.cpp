#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_static_model_vertex.h>
#include <array>
#include <cassert>
#include <limits>

int main() {
    using namespace kisak::vita;
    GfxDrawVertex source{};
    source.shadowVisibility = 1.0f;
    source.worldPosition[0] = 32767.998046875f;
    source.worldPosition[1] = -1234.56787109375f;
    source.worldPosition[2] = -0.0f;
    source.texCoord[0] = 0.123456791f;
    source.texCoord[1] = 175.9999847f;
    source.normal[0] = -1.0f;
    source.normal[1] = 1.0f;
    source.tangent[2] = 1.0f;
    source.color[0] = 0.0f;
    source.color[1] = 127.0f / 255.0f;
    source.color[2] = 128.0f / 255.0f;
    source.color[3] = 1.0f;
    std::array<VitaStaticModelVertex, 2> vertices{};
    assert(VitaPackStaticModelVertex(source, &vertices[0]));
    assert(std::memcmp(vertices[0].worldPosition, source.worldPosition, 12) == 0);
    assert(std::memcmp(vertices[0].texCoord, source.texCoord, 8) == 0);
    assert(vertices[0].normal[0] == -127 && vertices[0].normal[1] == 127);
    assert(vertices[0].normal[2] == 0 && vertices[0].normal[3] == 0);
    assert(vertices[0].tangent[2] == 127 && vertices[0].tangent[3] == 0);
    assert(vertices[0].color[0] == 0 && vertices[0].color[1] == 127);
    assert(vertices[0].color[2] == 128 && vertices[0].color[3] == 255);
    source.worldPosition[0] += 1.0f;
    assert(VitaPackStaticModelVertex(source, &vertices[1]));
    assert(vertices[1].worldPosition[0] != vertices[0].worldPosition[0]);
    assert(reinterpret_cast<char*>(&vertices[1]) - reinterpret_cast<char*>(&vertices[0]) == 32);
    const auto unchanged = vertices[0];
    source.lightmapCoord[0] = 0.5f;
    assert(!VitaPackStaticModelVertex(source, &vertices[0]));
    source.lightmapCoord[0] = 0.0f;
    source.lightmapCoord[1] = std::numeric_limits<float>::quiet_NaN();
    assert(!VitaPackStaticModelVertex(source, &vertices[0]));
    source.lightmapCoord[1] = 0.0f;
    source.shadowVisibility = 0.5f;
    assert(!VitaPackStaticModelVertex(source, &vertices[0]));
    assert(std::memcmp(&unchanged, &vertices[0], sizeof(unchanged)) == 0);
    assert(!VitaPackStaticModelVertex(source, nullptr));
    return 0;
}
