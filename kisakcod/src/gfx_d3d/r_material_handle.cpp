#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <qcommon/com_pack.h>
#include <qcommon/qcommon.h>
#if !defined(KISAK_VITA_SLICE)
#include <gfx_d3d/r_dvars.h>
#endif

// Shared source handle check; it has no dependency on the graphics API.
bool __cdecl IsValidMaterialHandle(Material *const handle)
{
    iassert((0x0003 & reinterpret_cast<uintptr_t>(handle)) == 0x0);
    return handle && handle->info.name && *handle->info.name;
}

const Material *__cdecl Material_FromHandle(Material *handle)
{
    iassert(handle);
    iassert(handle->info.name);
    iassert(handle->info.name[0]);

    return handle;
}

void __cdecl R_UpdateTeamColors(int team, const float *color_allies, const float *color_axis)
{
    rg.team = team;
    Byte4PackRgba(color_allies, (uint8_t *)&rg.color_allies);
    Byte4PackRgba(color_axis, (uint8_t *)&rg.color_axis);
}

bool __cdecl Material_HasAnyFogableTechnique(const Material *material)
{
    return Material_GetTechnique(material, TECHNIQUE_LIT_BEGIN) || Material_GetTechnique(material, TECHNIQUE_EMISSIVE);
}

const MaterialTechnique *__cdecl Material_GetTechnique(const Material *material, MaterialTechniqueType techType)
{
    const MaterialTechnique *technique; // [esp+0h] [ebp-8h]
    const MaterialTechniqueSet *techSet; // [esp+4h] [ebp-4h]

    techSet = Material_GetTechniqueSet(material);
    iassert( techSet );
    technique = techSet->techniques[techType];
#if defined(KISAK_VITA_SLICE)
    // Source-facing materials hold identities and state; Vita's GPU shaders
    // are backend handles. A desktop shader object cannot be executed here.
    if (technique)
        Com_Error(ERR_DROP, "Vita: desktop shader technique reached the native renderer (%s)", material->info.name);
#else
    if (technique
        && technique->passArray[0].pixelShader->prog.loadDef.loadForRenderer != r_rendererInUse->current.integer)
    {
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\r_material.h",
            320,
            0,
            "technique->passArray[0].pixelShader->prog.loadDef.loadForRenderer == r_rendererInUse->current.integer\n\t%i, %i",
            technique->passArray[0].pixelShader->prog.loadDef.loadForRenderer,
            r_rendererInUse->current.integer);
    }
#endif
    return technique;
}

MaterialTechniqueSet *__cdecl Material_GetTechniqueSet(const Material *material)
{
    iassert( material );
    if (!material->techniqueSet)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\r_material.h",
            300,
            0,
            "%s\n\t(material->info.name) = %s",
            "(material->techniqueSet)",
            material->info.name);
    return material->techniqueSet->remappedTechniqueSet;
}

