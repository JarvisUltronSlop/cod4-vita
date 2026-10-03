#include <universal/q_shared.h>
#include "r_model_skin.h"
#include "r_init.h"
#include "r_dvars.h"
#include <qcommon/com_playerprofile.h>
#include "r_buffers.h"
#include <universal/profile.h>
#include "r_dobj_skin.h"


//uint32_t const *const g_shortBoneWeightPerm__uint4 820f4950     gfx_d3d : r_model_skin.obj
//struct __vector4 const g_shortBoneWeightPerm 85b981d0     gfx_d3d : r_model_skin.obj

void R_SkinXModelCmd(_WORD *data)
{
    if (dx.deviceLost) return;

    PROF_SCOPED("R_SkinXModel");

    bool sseEnabled = sys_SSE->current.enabled && r_sse_skinning->current.enabled;
    bool sseStateUsed = false;

    SkinXModelCmd* skinCmd = (SkinXModelCmd*)data;

    iassert(skinCmd);
    iassert(skinCmd->surfCount);

    GfxModelSkinnedSurface * surfPos = (GfxModelSkinnedSurface*)skinCmd->modelSurfs;

    int boneIndex = -1;
    GfxPackedVertex *skinVerticesOut = NULL;

    DObjSkelMat __declspec(align(16)) boneSkelMats[128];
    memset(boneSkelMats, 0, sizeof(boneSkelMats));

    for (uint32_t i = 0; i < skinCmd->surfCount; i++)
    {
        GfxModelSkinnedSurface *skinnedSurf = surfPos;

        if (skinnedSurf->skinnedCachedOffset == -3)
        {
            surfPos = (GfxModelSkinnedSurface *)((char *)surfPos + 4);
            continue;
        }

        if (boneIndex != skinnedSurf->info.boneIndex)
        {
            boneIndex = skinnedSurf->info.boneIndex;
            const int totalBones = boneIndex + skinnedSurf->info.boneCount;
            const DObjAnimMat* baseMats = &skinnedSurf->info.baseMat[-boneIndex];
            for (uint32_t j = boneIndex; j < totalBones; j++)
            {
                if ((skinCmd->surfacePartBits[j >> 5] & (0x80000000 >> (j & 0x1F))) == 0)
                    continue;

                if (sseStateUsed)
                {
                    sseStateUsed = false;
                    _m_empty();
                }

                DObjSkelMat mat0, mat1;

                ConvertQuatToInverseSkelMat(&baseMats[j], &mat0);
                ConvertQuatToSkelMat(&skinCmd->mat[j], &mat1);
                R_MultiplySkelMat(&mat0, &mat1, &boneSkelMats[j]);

                boneSkelMats[j].axis[0][3] = 0.0f;
                boneSkelMats[j].axis[1][3] = 0.0f;
                boneSkelMats[j].axis[2][3] = 0.0f;
                boneSkelMats[j].origin[3] = 1.0f;
            }
        }

        if (skinnedSurf->skinnedCachedOffset == -2)
        {
            surfPos = (GfxModelSkinnedSurface *)((char *)surfPos + 56);
            continue;
        }

        surfPos = skinnedSurf + 1;
        XSurface *xsurf = skinnedSurf->xsurf;

        iassert(xsurf);

        if (skinnedSurf->skinnedCachedOffset < 0)
        {
            iassert((reinterpret_cast<uint>(skinnedSurf->skinnedVert) & 15) == 0);
            skinVerticesOut = skinnedSurf->skinnedVert;
        }
        else
        {
            iassert(gfxBuf.skinnedCacheLockAddr);
            iassert(((reinterpret_cast<uint>(gfxBuf.skinnedCacheLockAddr) & 15) == 0));
            iassert(((skinnedSurf->skinnedCachedOffset & 15) == 0));
            skinVerticesOut = (GfxPackedVertex*)&gfxBuf.skinnedCacheLockAddr[skinnedSurf->skinnedCachedOffset];
        }

        if (sseEnabled)
        {
            if (!sseStateUsed)
            {
                sseStateUsed = true;
                _m_empty();
            }
        
            GfxPackedVertexNormal *skinVertNormalIn = 0, *skinVertNormalOut = 0;
            if (gfxBuf.fastSkin)
            {
                if (skinnedSurf->skinnedCachedOffset >= 0)
                    skinVertNormalOut = &gfxBuf.skinnedCacheNormalsAddr[skinnedSurf->skinnedCachedOffset >> 5];
                if (skinnedSurf->skinnedVert)
                    skinVertNormalIn = &gfxBuf.oldSkinnedCacheNormalsAddr[(int)skinnedSurf->skinnedVert >> 5];
            }
            R_SkinXSurfaceSkinnedSse(xsurf, &boneSkelMats[boneIndex], skinVertNormalIn, skinVertNormalOut, skinVerticesOut);
        }
        else
        {
            R_SkinXSurfaceSkinned(xsurf, &boneSkelMats[boneIndex], skinVerticesOut);
        }
    }

    if (sseStateUsed)
        _m_empty();
}


