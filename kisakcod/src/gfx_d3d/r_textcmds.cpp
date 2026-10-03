// Original KisakCOD text command generation. Platform renderers supply
// bounded command storage; formatting, style and effect flags stay shared.
#include <universal/q_shared.h>
#include "r_rendercmds.h"
#include "rb_backend.h"
#include "r_utils.h"
#include <universal/profile.h>
#include <qcommon/com_pack.h>

void __cdecl R_AddCmdDrawTextWithCursor(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    int cursorPos,
    char cursor)
{
    AddBaseDrawTextCmd(text, maxChars, font, x, y, xScale, yScale, rotation, color, style, cursorPos, cursor);
}

GfxCmdDrawText2D *__cdecl AddBaseDrawTextCmd(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    int cursorPos,
    char cursor)
{
    uint32_t v13; // [esp+0h] [ebp-4Ch]
    GfxCmdDrawText2D *cmd; // [esp+48h] [ebp-4h]

    iassert( maxChars > 0 );
    iassert( text );
    if (!*text && cursorPos < 0)
        return 0;
    v13 = strlen(text);
    cmd = (GfxCmdDrawText2D *)R_GetCommandBuffer(RC_DRAW_TEXT_2D, (v13 + 84) & 0xFFFFFFFC);
    if (!cmd)
        return 0;
    cmd->x = x;
    cmd->y = y;
    cmd->rotation = rotation;
    cmd->font = font;
    cmd->xScale = xScale;
    cmd->yScale = yScale;
    R_ConvertColorToBytes(color, (uint32_t *)&cmd->color);
    cmd->maxChars = maxChars;
    cmd->renderFlags = 0;
    switch (style)
    {
    case 3:
        cmd->renderFlags |= 4u;
        break;
    case 6:
        cmd->renderFlags |= 0xCu;
        break;
    case 128:
        cmd->renderFlags |= 1u;
        break;
    }
    if (cursorPos > -1)
    {
        cmd->renderFlags |= 2u;
        cmd->cursorPos = cursorPos;
        cmd->cursorLetter = cursor;
    }
    {
        PROF_SCOPED("R_memcpy");
        memcpy((uint8_t *)cmd->text, (uint8_t *)text, v13);
    }
    cmd->text[v13] = 0;
    return cmd;
}

void __cdecl R_AddCmdDrawText(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style)
{
    R_AddCmdDrawTextWithCursor(text, maxChars, font, x, y, xScale, yScale, rotation, color, style, -1, 0);
}

void __cdecl R_AddCmdDrawTextSubtitle(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    const float *glowColor,
    bool cinematic)
{
    GfxCmdDrawText2D *cmd; // [esp+24h] [ebp-4h]

    cmd = AddBaseDrawTextCmd(text, maxChars, font, x, y, xScale, yScale, rotation, color, style, -1, 0);
    if (cmd)
    {
        if (cinematic)
            cmd->renderFlags |= 0x200u;
        cmd->renderFlags |= 0x100u;
        SetDrawText2DGlowParms(cmd, color, glowColor);
    }
}

char __cdecl SetDrawText2DGlowParms(GfxCmdDrawText2D *cmd, const float *color, const float *glowColor)
{
    float scaledGlowColor[4]; // [esp+0h] [ebp-10h] BYREF

    if (!glowColor)
        return 0;
    if (glowColor[3] == 0.0)
        return 0;
    cmd->renderFlags |= 0x30u;
    scaledGlowColor[0] = *glowColor * 0.1000000014901161;
    scaledGlowColor[1] = glowColor[1] * 0.1000000014901161;
    scaledGlowColor[2] = glowColor[2] * 0.1000000014901161;
    scaledGlowColor[3] = glowColor[3] * color[3];
    R_ConvertColorToBytes(scaledGlowColor, (uint8_t *)&cmd->glowForceColor);
    return 1;
}

void __cdecl R_AddCmdDrawTextWithEffects(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    float rotation,
    const float *color,
    int style,
    const float *glowColor,
    Material *fxMaterial,
    Material *fxMaterialGlow,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration)
{
    Material *v17; // [esp+24h] [ebp-30h]
    Material *defaultMaterial; // [esp+28h] [ebp-2Ch]
    const Material *actualMaterial; // [esp+4Ch] [ebp-8h]
    GfxCmdDrawText2D *cmd; // [esp+50h] [ebp-4h]

    if (fxMaterial)
        defaultMaterial = (Material *)Material_FromHandle(fxMaterial);
    else
        defaultMaterial = rgp.defaultMaterial;
    actualMaterial = defaultMaterial;
    if (Material_HasAnyFogableTechnique(defaultMaterial) && !Material_IsDefault(defaultMaterial)
        || (!fxMaterialGlow ? (v17 = rgp.defaultMaterial) : (v17 = (Material *)Material_FromHandle(fxMaterialGlow)),
            (actualMaterial = v17, Material_HasAnyFogableTechnique(v17)) && !Material_IsDefault(v17)))
    {
        R_WarnOncePerFrame(R_WARN_FOGABLE_2DTEXT, actualMaterial->info.name, text);
    }
    else
    {
        cmd = AddBaseDrawTextCmd(text, maxChars, font, x, y, xScale, yScale, rotation, color, style, -1, 0);
        if (cmd)
        {
            SetDrawText2DGlowParms(cmd, color, glowColor);
            SetDrawText2DPulseFXParms(
                cmd,
                fxMaterial,
                fxMaterialGlow,
                fxBirthTime,
                fxLetterTime,
                fxDecayStartTime,
                fxDecayDuration);
        }
    }
}

char __cdecl SetDrawText2DPulseFXParms(
    GfxCmdDrawText2D *cmd,
    Material *fxMaterial,
    Material *fxMaterialGlow,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration)
{
    if (!fxMaterial)
        return 0;
    if (!fxMaterialGlow)
        return 0;
    if (!fxBirthTime)
        return 0;
    cmd->renderFlags |= 0xC0u;
    cmd->fxMaterial = fxMaterial;
    cmd->fxMaterialGlow = fxMaterialGlow;
    cmd->fxBirthTime = fxBirthTime;
    cmd->fxLetterTime = fxLetterTime;
    cmd->fxDecayStartTime = fxDecayStartTime;
    cmd->fxDecayDuration = fxDecayDuration;
    cmd->padding = 0.0;
    return 1;
}

void __cdecl R_AddCmdDrawConsoleText(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style)
{
    AddBaseDrawConsoleTextCmd(textPool, poolSize, firstChar, charCount, font, x, y, xScale, yScale, color, style);
}

GfxCmdDrawText2D *__cdecl AddBaseDrawConsoleTextCmd(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style)
{
    GfxCmdDrawText2D *cmd; // [esp+4h] [ebp-4h]

    iassert( charCount >= 0 );
    iassert( textPool );
    if (!charCount)
        return 0;
    cmd = (GfxCmdDrawText2D *)R_GetCommandBuffer(RC_DRAW_TEXT_2D, (charCount + 84) & 0xFFFFFFFC);
    if (!cmd)
        return 0;
    cmd->x = x;
    cmd->y = y;
    cmd->rotation = 0.0;
    cmd->font = font;
    cmd->xScale = xScale;
    cmd->yScale = yScale;
    R_ConvertColorToBytes(color, (uint8_t *)&cmd->color);
    cmd->maxChars = 0x7FFFFFFF;
    cmd->renderFlags = 0;
    switch (style)
    {
    case 3:
        cmd->renderFlags |= 4u;
        break;
    case 6:
        cmd->renderFlags |= 0xCu;
        break;
    case 128:
        cmd->renderFlags |= 1u;
        break;
    }
    CopyPoolTextToCmd(textPool, poolSize, firstChar, charCount, cmd);
    return cmd;
}

void __cdecl CopyPoolTextToCmd(char *textPool, int poolSize, int firstChar, int charCount, GfxCmdDrawText2D *cmd)
{
    uint32_t poolRemaining; // [esp+30h] [ebp-4h]

    iassert(cmd);
    
    PROF_SCOPED("R_memcpy");
    poolRemaining = poolSize - firstChar;

    if (charCount > poolSize - firstChar)
    {
        memcpy((uint8_t *)cmd->text, (uint8_t *)&textPool[firstChar], poolRemaining);
        memcpy((uint8_t *)&cmd->text[poolRemaining], (uint8_t *)textPool, charCount - poolRemaining);
    }
    else
    {
        memcpy((uint8_t *)cmd->text, (uint8_t *)&textPool[firstChar], charCount);
    }

    cmd->text[charCount] = 0;
}

void __cdecl R_AddCmdDrawConsoleTextSubtitle(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style,
    const float *glowColor)
{
    GfxCmdDrawText2D *cmd; // [esp+18h] [ebp-4h]

    cmd = AddBaseDrawConsoleTextCmd(textPool, poolSize, firstChar, charCount, font, x, y, xScale, yScale, color, style);
    if (cmd)
    {
        cmd->renderFlags |= 0x100u;
        SetDrawText2DGlowParms(cmd, color, glowColor);
    }
}

void __cdecl R_AddCmdDrawConsoleTextPulseFX(
    char *textPool,
    int poolSize,
    int firstChar,
    int charCount,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style,
    const float *glowColor,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration,
    Material *fxMaterial,
    Material *fxMaterialGlow)
{
    Material *v18; // [esp+18h] [ebp-30h]
    Material *defaultMaterial; // [esp+1Ch] [ebp-2Ch]
    const Material *actualMaterial; // [esp+40h] [ebp-8h]
    GfxCmdDrawText2D *cmd; // [esp+44h] [ebp-4h]

    if (fxMaterial)
        defaultMaterial = (Material *)Material_FromHandle(fxMaterial);
    else
        defaultMaterial = rgp.defaultMaterial;
    actualMaterial = defaultMaterial;
    if (Material_HasAnyFogableTechnique(defaultMaterial) && !Material_IsDefault(defaultMaterial)
        || (!fxMaterialGlow ? (v18 = rgp.defaultMaterial) : (v18 = (Material *)Material_FromHandle(fxMaterialGlow)),
            (actualMaterial = v18, Material_HasAnyFogableTechnique(v18)) && !Material_IsDefault(v18)))
    {
        R_WarnOncePerFrame(R_WARN_FOGABLE_2DTEXT, actualMaterial->info.name, textPool);
    }
    else
    {
        cmd = AddBaseDrawConsoleTextCmd(textPool, poolSize, firstChar, charCount, font, x, y, xScale, yScale, color, style);
        if (cmd)
        {
            SetDrawText2DGlowParms(cmd, color, glowColor);
            SetDrawText2DPulseFXParms(
                cmd,
                fxMaterial,
                fxMaterialGlow,
                fxBirthTime,
                fxLetterTime,
                fxDecayStartTime,
                fxDecayDuration);
        }
    }
}


void __cdecl R_ConvertColorToBytes(const float *colorFloat, uint32_t *colorBytes)
{
    if (colorFloat)
        Byte4PackVertexColor(colorFloat, (unsigned char*)colorBytes);
    else
        *colorBytes = -1;
}

