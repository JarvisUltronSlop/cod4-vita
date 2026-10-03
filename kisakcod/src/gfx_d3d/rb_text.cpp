// Original KisakCOD 2D text layout, pulse/decay, glow and icon routines.
// Extracted from rb_backend.cpp so desktop and Vita use the same algorithms.
#include <universal/q_shared.h>
#include "rb_backend.h"
#include "r_utils.h"
#include "r_font.h"
#include <client/client.h>
#include <stringed/stringed_hooks.h>
#if !defined(KISAK_VITA_SLICE)
#include "rb_state.h"
static int RB_TextSceneTime() { return gfxCmdBufSourceState.sceneDef.time; }
#else
// Renderer clock boundary; supplied by the Vita frame packet backend.
int RB_TextSceneTime();
#endif

// This text-only table must not collide with VitaGL's C texture palette.
static GfxColor color_table[8] =
{
  { 4278190080u },
  { 4284243199u },
  { 4278255360u },
  { 4278255615u },
  { 4294901760u },
  { 4294967040u },
  { 4294925567u },
  { 4294967295u }
}; // weak
void __cdecl RB_LookupColor(uint8_t c, GfxColor *color)
{
    GfxColor *p_color_axis; // [esp+8h] [ebp-Ch]
    GfxColor *p_color_allies; // [esp+Ch] [ebp-8h]
    uint32_t index; // [esp+10h] [ebp-4h]

    index = ColorIndex(c);
    if (index >= 8)
    {
        if (c == 56)
        {
            if (rg.team == 2)
                p_color_allies = &rg.color_allies;
            else
                p_color_allies = &rg.color_axis;
            color->packed = p_color_allies->packed;
        }
        else if (c == 57)
        {
            if (rg.team == 2)
                p_color_axis = &rg.color_axis;
            else
                p_color_axis = &rg.color_allies;
            color->packed = p_color_axis->packed;
        }
        else
        {
            color->packed = -1;
        }
    }
    else
    {
        color->packed = (uint32_t)color_table[index];
    }
}

void __cdecl RB_DrawText(const char *text, Font_s *font, float x, float y, GfxColor color)
{
    DrawText2D(text, x, y, font, 1.0, 1.0, 0.0, 1.0, color, 0x7FFFFFFF, 0, 0, 0, 0.0, color, 0, 0, 0, 0, 0, 0);
}

const uint8_t MY_ALTCOLOR_TWO[4] = { 0xE6, 0xFF, 0xE6, 0xDC };
const float MY_OFFSETS_0[4][2] =
{
    { -1.0f, 1.0f },
    { -1.0f, 1.0f },
    { 1.0f, -1.0f },
    { 1.0f, 1.0f }
};
void __cdecl DrawText2D(
    const char *text,
    float x,
    float y,
    Font_s *font,
    float xScale,
    float yScale,
    float sinAngle,
    float cosAngle,
    GfxColor color,
    int maxLength,
    __int16 renderFlags,
    int cursorPos,
    char cursorLetter,
    float padding,
    GfxColor glowForcedColor,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration,
    const Material *fxMaterial,
    const Material *fxMaterialGlow)
{
    int v21; // esi
    const char *v22; // eax
    double v23; // st7
    float v24; // [esp+3Ch] [ebp-1B4h]
    float v25; // [esp+44h] [ebp-1ACh]
    float v26; // [esp+5Ch] [ebp-194h]
    float v27; // [esp+64h] [ebp-18Ch]
    float v28; // [esp+74h] [ebp-17Ch]
    float h; // [esp+7Ch] [ebp-174h]
    float v30; // [esp+9Ch] [ebp-154h]
    float v31; // [esp+A0h] [ebp-150h]
    float v32; // [esp+A4h] [ebp-14Ch]
    float v33; // [esp+A8h] [ebp-148h]
    GfxColor v34; // [esp+ACh] [ebp-144h]
    float v35; // [esp+C0h] [ebp-130h]
    float v36; // [esp+C4h] [ebp-12Ch]
    float v37; // [esp+C8h] [ebp-128h]
    float v38; // [esp+CCh] [ebp-124h]
    GfxColor v39; // [esp+D0h] [ebp-120h]
    float v40; // [esp+E4h] [ebp-10Ch]
    float v41; // [esp+E8h] [ebp-108h]
    float w; // [esp+ECh] [ebp-104h]
    float v43; // [esp+F0h] [ebp-100h]
    float resizeOffsY; // [esp+148h] [ebp-A8h]
    int offIdx; // [esp+14Ch] [ebp-A4h]
    float resizeOffsX; // [esp+150h] [ebp-A0h]
    int ofs; // [esp+154h] [ebp-9Ch]
    const Glyph *glyphOriginal; // [esp+158h] [ebp-98h]
    int tempSeed; // [esp+15Ch] [ebp-94h] BYREF
    float iconWidth; // [esp+160h] [ebp-90h]
    GfxColor lookupColor; // [esp+164h] [ebp-8Ch] BYREF
    const uint8_t *altColorTwo; // [esp+168h] [ebp-88h]
    GfxColor finalColor; // [esp+16Ch] [ebp-84h] BYREF
    bool drawExtraFxChar; // [esp+173h] [ebp-7Dh] BYREF
    const Glyph *glyph; // [esp+174h] [ebp-7Ch]
    float yAdj; // [esp+178h] [ebp-78h]
    float decayOffset; // [esp+17Ch] [ebp-74h]
    float xAdj; // [esp+180h] [ebp-70h]
    bool skipDrawing; // [esp+187h] [ebp-69h] BYREF
    uint32_t letter; // [esp+188h] [ebp-68h] BYREF
    int extraFxChar; // [esp+18Ch] [ebp-64h]
    float deltaX; // [esp+190h] [ebp-60h]
    uint32_t origLetter; // [esp+194h] [ebp-5Ch]
    uint8_t fadeAlpha; // [esp+19Bh] [ebp-55h] BYREF
    float yRot; // [esp+19Ch] [ebp-54h] BYREF
    int passRandSeed; // [esp+1A0h] [ebp-50h] BYREF
    int maxLengthRemaining; // [esp+1A4h] [ebp-4Ch]
    float xRot; // [esp+1A8h] [ebp-48h] BYREF
    bool subtitleAllowGlow; // [esp+1AFh] [ebp-41h]
    GfxColor currentColor; // [esp+1B0h] [ebp-40h]
    const char *curText; // [esp+1B4h] [ebp-3Ch] BYREF
    int count; // [esp+1B8h] [ebp-38h]
    int passIdx; // [esp+1BCh] [ebp-34h]
    GfxColor dropShadowColor; // [esp+1C0h] [ebp-30h]
    const Material *material; // [esp+1C4h] [ebp-2Ch]
    bool drawRandomCharAtEnd; // [esp+1CBh] [ebp-25h] BYREF
    int randSeed; // [esp+1CCh] [ebp-24h] BYREF
    float startX; // [esp+1D0h] [ebp-20h]
    int decayTimeElapsed; // [esp+1D4h] [ebp-1Ch] BYREF
    const Material *glowMaterial; // [esp+1D8h] [ebp-18h]
    float monospaceWidth; // [esp+1E0h] [ebp-10h]
    bool decaying; // [esp+1E7h] [ebp-9h] BYREF
    float startY; // [esp+1E8h] [ebp-8h]
    int passCount; // [esp+1ECh] [ebp-4h]
    float xa; // [esp+1FCh] [ebp+Ch]
    float ya; // [esp+200h] [ebp+10h]

    iassert( text );
    iassert( font );
    dropShadowColor.packed = 0;
    dropShadowColor.array[3] = color.array[3];
    randSeed = 1;
    drawRandomCharAtEnd = 0;
    monospaceWidth = GetMonospaceWidth(font, renderFlags);
    glowMaterial = 0;
    material = Material_FromHandle(font->material);
    iassert( material );
    if ((renderFlags & 0x40) != 0 && (!fxMaterial || !fxMaterial->techniqueSet))
        MyAssertHandler(
            ".\\rb_backend.cpp",
            2143,
            0,
            "%s",
            "!(renderFlags & TEXT_RENDERFLAG_FX_DECODE) || (fxMaterial && fxMaterial->techniqueSet)");
    if ((renderFlags & 0x40) != 0 && (!fxMaterialGlow || !fxMaterialGlow->techniqueSet))
        MyAssertHandler(
            ".\\rb_backend.cpp",
            2144,
            0,
            "%s",
            "!(renderFlags & TEXT_RENDERFLAG_FX_DECODE) || (fxMaterialGlow && fxMaterialGlow->techniqueSet)");
    if (SetupPulseFXVars(
        text,
        maxLength,
        renderFlags,
        fxBirthTime,
        fxLetterTime,
        fxDecayStartTime,
        fxDecayDuration,
        &drawRandomCharAtEnd,
        &randSeed,
        &maxLength,
        &decaying,
        &decayTimeElapsed))
    {
        passCount = 1;
        if ((renderFlags & 0x10) != 0)
        {
            glowMaterial = Material_FromHandle(font->glowMaterial);
            iassert( glowMaterial );
            ++passCount;
        }
        if ((renderFlags & 0x40) != 0)
        {
            iassert( fxMaterialGlow );
            iassert( fxMaterial );
        }
        startX = x - xScale * 0.5;
        startY = y - yScale * 0.5;
        for (passIdx = 0; passIdx < passCount; ++passIdx)
        {
            maxLengthRemaining = maxLength;
            passRandSeed = randSeed;
            currentColor.packed = color.packed;
            xa = startX;
            ya = startY;
            subtitleAllowGlow = 0;
            count = 0;
            curText = text;
            while (*curText && maxLengthRemaining)
            {
                letter = SEH_ReadCharFromString(&curText, 0);
                skipDrawing = 0;
                fadeAlpha = 0;
                drawExtraFxChar = 0;
                extraFxChar = 0;
                if (letter == 94 && curText && *curText != 94 && *curText >= 48 && *curText <= 57)
                {
                    subtitleAllowGlow = 0;
                    v21 = ColorIndex(*curText);
                    if (v21 == ColorIndex(0x37u))
                    {
                        currentColor.packed = color.packed;
                    }
                    else if ((renderFlags & 0x100) != 0 && ColorIndex(*curText) == 2)
                    {
                        altColorTwo = MY_ALTCOLOR_TWO;
                        currentColor.array[3] = ModulateByteColors(MY_ALTCOLOR_TWO[3], color.array[3]);
                        currentColor.array[0] = altColorTwo[2];
                        currentColor.array[1] = altColorTwo[1];
                        currentColor.array[2] = *altColorTwo;
                        subtitleAllowGlow = 1;
                    }
                    else
                    {
                        RB_LookupColor(*curText, &lookupColor);
                        currentColor.array[3] = color.array[3];
                        currentColor.array[0] = lookupColor.array[2];
                        currentColor.array[1] = lookupColor.array[1];
                        currentColor.array[2] = lookupColor.array[0];
                    }
                    ++curText;
                    count += 2;
                }
                else
                {
                    if (drawRandomCharAtEnd && maxLengthRemaining == 1)
                    {
                        letter = R_FontGetRandomLetter(font, passRandSeed);
                        fadeAlpha = -64;
                        if ((int)RandWithSeed(&passRandSeed) % 2)
                        {
                            drawExtraFxChar = 1;
                            letter = 79;
                        }
                    }
                    if (letter == 94 && (*curText == 1 || *curText == 2))
                    {
                        RotateXY(cosAngle, sinAngle, startX, startY, xa, ya, &xRot, &yRot);
                        iconWidth = RB_DrawHudIcon(
                            curText,
                            xRot,
                            yRot,
                            sinAngle,
                            cosAngle,
                            font,
                            xScale,
                            yScale,
                            currentColor.packed);
                        if (iconWidth <= 0.0)
                        {
                            v22 = va("Invalid hud icon.  Text: \"%s\"", text);
                            MyAssertHandler(".\\rb_backend.cpp", 2266, 0, "%s\n\t%s", "iconWidth > 0", v22);
                        }
                        xa = xa + iconWidth;
                        if ((renderFlags & 0x80) != 0)
                            xa = padding * xScale + xa;
                        curText += 7;
                        ++count;
                        --maxLengthRemaining;
                    }
                    else if (letter == 10)
                    {
                        xa = startX;
                        ya = (double)font->pixelHeight * yScale + ya;
                    }
                    else if (letter == 13)
                    {
                        xa = startX;
                    }
                    else
                    {
                        origLetter = letter;
                        if (decaying)
                            GetDecayingLetterInfo(
                                letter,
                                font,
                                &passRandSeed,
                                decayTimeElapsed,
                                fxBirthTime,
                                fxDecayDuration,
                                currentColor.array[3],
                                &skipDrawing,
                                &fadeAlpha,
                                &letter,
                                &drawExtraFxChar);
                        if (drawExtraFxChar)
                        {
                            tempSeed = passRandSeed;
                            extraFxChar = RandWithSeed(&tempSeed);
                        }
                        glyph = R_GetCharacterGlyph(font, letter);
                        if (letter == origLetter)
                        {
                            decayOffset = 0.0;
                            deltaX = (float)glyph->dx;
                        }
                        else
                        {
                            glyphOriginal = R_GetCharacterGlyph(font, origLetter);
                            decayOffset = (double)glyphOriginal->pixelWidth * 0.5 - (double)glyph->pixelWidth * 0.5;
                            deltaX = (float)glyphOriginal->dx;
                        }
                        xAdj = ((double)glyph->x0 + decayOffset) * xScale;
                        yAdj = (double)glyph->y0 * yScale;
                        finalColor.packed = LongNoSwap(currentColor.packed);
                        if (decaying || drawRandomCharAtEnd && maxLengthRemaining == 1)
                            finalColor.array[3] = ModulateByteColors(finalColor.array[3], fadeAlpha);
                        if (!skipDrawing)
                        {
                            if (passIdx)
                            {
                                if (passIdx == 1 && ((renderFlags & 0x100) == 0 || subtitleAllowGlow))
                                {
                                    GlowColor(&finalColor, finalColor, glowForcedColor, renderFlags);
                                    resizeOffsX = (double)glyph->pixelWidth * -0.75 * 0.5 * xScale;
                                    resizeOffsY = (double)glyph->pixelHeight * -0.125 * 0.5 * yScale;
                                    for (offIdx = 0; offIdx < 4; ++offIdx)
                                    {
                                        xRot = xa + xAdj + resizeOffsX + (float)MY_OFFSETS_0[offIdx][0] * 2.0 * xScale;
                                        yRot = ya + yAdj + resizeOffsY + (float)MY_OFFSETS_0[offIdx][1] * 2.0 * yScale;
                                        RotateXY(cosAngle, sinAngle, startX, startY, xRot, yRot, &xRot, &yRot);
                                        iassert( glowMaterial );
                                        if (drawExtraFxChar)
                                        {
                                            v25 = (double)glyph->pixelHeight * yScale;
                                            v24 = (double)glyph->pixelWidth * xScale;
                                            DrawTextFxExtraCharacter(
                                                fxMaterialGlow,
                                                extraFxChar,
                                                xRot,
                                                yRot,
                                                v24,
                                                v25,
                                                sinAngle,
                                                cosAngle,
                                                finalColor.packed);
                                        }
                                        else
                                        {
                                            v30 = xRot;
                                            v31 = yRot;
                                            v32 = (0.75 + 1.0) * (xScale * (double)glyph->pixelWidth);
                                            v33 = (0.125 + 1.0) * (yScale * (double)glyph->pixelHeight);
                                            v34.packed = finalColor.packed;
                                            if (Material_HasAnyFogableTechnique(glowMaterial))
                                                R_WarnOncePerFrame(R_WARN_FOGABLE_2DTEXT, glowMaterial->info.name);
                                            else
                                                RB_DrawStretchPicRotate(
                                                    glowMaterial,
                                                    v30,
                                                    v31,
                                                    v32,
                                                    v33,
                                                    glyph->s0,
                                                    glyph->t0,
                                                    glyph->s1,
                                                    glyph->t1,
                                                    sinAngle,
                                                    cosAngle,
                                                    v34.packed,
                                                    GFX_PRIM_STATS_HUD);
                                        }
                                    }
                                }
                            }
                            else
                            {
                                if ((renderFlags & 4) != 0)
                                {
                                    ofs = 1;
                                    if ((renderFlags & 8) != 0)
                                        ofs = 2;
                                    xRot = xa + xAdj + (double)ofs;
                                    yRot = ya + yAdj + (double)ofs;
                                    RotateXY(cosAngle, sinAngle, startX, startY, xRot, yRot, &xRot, &yRot);
                                    if (drawExtraFxChar)
                                    {
                                        h = (double)glyph->pixelHeight * yScale;
                                        v28 = (double)glyph->pixelWidth * xScale;
                                        DrawTextFxExtraCharacter(
                                            fxMaterial,
                                            extraFxChar,
                                            xRot,
                                            yRot,
                                            v28,
                                            h,
                                            sinAngle,
                                            cosAngle,
                                            dropShadowColor.packed);
                                    }
                                    else
                                    {
                                        v40 = xRot;
                                        v41 = yRot;
                                        w = xScale * (double)glyph->pixelWidth;
                                        v43 = yScale * (double)glyph->pixelHeight;
                                        if (Material_HasAnyFogableTechnique(material))
                                            R_WarnOncePerFrame(R_WARN_FOGABLE_2DTEXT, material->info.name);
                                        else
                                            RB_DrawStretchPicRotate(
                                                material,
                                                v40,
                                                v41,
                                                w,
                                                v43,
                                                glyph->s0,
                                                glyph->t0,
                                                glyph->s1,
                                                glyph->t1,
                                                sinAngle,
                                                cosAngle,
                                                dropShadowColor.packed,
                                                GFX_PRIM_STATS_HUD);
                                    }
                                }
                                xRot = xa + xAdj;
                                yRot = ya + yAdj;
                                RotateXY(cosAngle, sinAngle, startX, startY, xRot, yRot, &xRot, &yRot);
                                if (drawExtraFxChar)
                                {
                                    v27 = (double)glyph->pixelHeight * yScale;
                                    v26 = (double)glyph->pixelWidth * xScale;
                                    DrawTextFxExtraCharacter(
                                        fxMaterial,
                                        extraFxChar,
                                        xRot,
                                        yRot,
                                        v26,
                                        v27,
                                        sinAngle,
                                        cosAngle,
                                        finalColor.packed);
                                }
                                else
                                {
                                    v35 = xRot;
                                    v36 = yRot;
                                    v37 = xScale * (double)glyph->pixelWidth;
                                    v38 = yScale * (double)glyph->pixelHeight;
                                    v39.packed = finalColor.packed;
                                    if (Material_HasAnyFogableTechnique(material))
                                        R_WarnOncePerFrame(R_WARN_FOGABLE_2DTEXT, material->info.name);
                                    else
                                        RB_DrawStretchPicRotate(
                                            material,
                                            v35,
                                            v36,
                                            v37,
                                            v38,
                                            glyph->s0,
                                            glyph->t0,
                                            glyph->s1,
                                            glyph->t1,
                                            sinAngle,
                                            cosAngle,
                                            v39.packed,
                                            GFX_PRIM_STATS_HUD);
                                }
                                if ((renderFlags & 2) != 0 && count == cursorPos)
                                {
                                    xRot = xa + xAdj;
                                    RotateXY(cosAngle, sinAngle, startX, startY, xRot, ya, &xRot, &yRot);
                                    RB_DrawCursor(
                                        material,
                                        cursorLetter,
                                        xRot,
                                        yRot,
                                        sinAngle,
                                        cosAngle,
                                        font,
                                        xScale,
                                        yScale,
                                        finalColor.packed);
                                }
                            }
                        }
                        if ((renderFlags & 1) != 0)
                            v23 = monospaceWidth * xScale + xa;
                        else
                            v23 = deltaX * xScale + xa;
                        xa = v23;
                        if ((renderFlags & 0x80) != 0)
                            xa = padding * xScale + xa;
                        ++count;
                        --maxLengthRemaining;
                    }
                }
            }
            if ((renderFlags & 2) != 0 && count == cursorPos)
            {
                xRot = xa;
                RotateXY(cosAngle, sinAngle, startX, startY, xa, ya, &xRot, &yRot);
                RB_DrawCursor(material, cursorLetter, xRot, yRot, sinAngle, cosAngle, font, xScale, yScale, color.packed);
            }
        }
    }
}

double __cdecl RB_DrawHudIcon(
    const char *text,
    float x,
    float y,
    float sinAngle,
    float cosAngle,
    Font_s *font,
    float xScale,
    float yScale,
    uint32_t color)
{
    const Material *v9; // eax
    float s1; // [esp+40h] [ebp-10h]
    float s0; // [esp+44h] [ebp-Ch]
    float h; // [esp+48h] [ebp-8h]
    float w; // [esp+4Ch] [ebp-4h]
    float ya; // [esp+60h] [ebp+10h]

    iassert( text );
    if (*text == 1)
    {
        s0 = 0.0;
        s1 = 1.0;
    }
    else
    {
        iassert( text[0] == CONTXTCMD_TYPE_HUDICON_FLIP );
        s0 = 1.0;
        s1 = 0.0;
    }
    w = (double)((font->pixelHeight * (text[1] - 16) + 16) / 32) * xScale;
    h = (double)((font->pixelHeight * (text[2] - 16) + 16) / 32) * yScale;
    ya = y - ((double)font->pixelHeight * yScale + h) * 0.5;
    iassert( w > 0 );
    iassert( h > 0 );
    if (!IsValidMaterialHandle(*(Material *const *)(text + 3)))
        return 0.0;
    v9 = Material_FromHandle(*(Material **)(text + 3));
    RB_DrawStretchPicRotate(v9, x, ya, w, h, s0, 0.0, s1, 1.0, sinAngle, cosAngle, color, GFX_PRIM_STATS_HUD);
    return w;
}

void __cdecl RB_DrawCursor(
    const Material *material,
    uint8_t cursor,
    float x,
    float y,
    float sinAngle,
    float cosAngle,
    Font_s *font,
    float xScale,
    float yScale,
    uint32_t color)
{
    float v10; // [esp+3Ch] [ebp-24h]
    float w; // [esp+40h] [ebp-20h]
    float h; // [esp+44h] [ebp-1Ch]
    const Glyph *cursorGlyph; // [esp+58h] [ebp-8h]
    uint32_t newColor; // [esp+5Ch] [ebp-4h]

    iassert( font );
    if ((((
#ifndef KISAK_RADIANT
        CL_ScaledMilliseconds()
#else
        Sys_Milliseconds()
#endif
        / 256) & 1) == 0))
    {
        cursorGlyph = R_GetCharacterGlyph(font, cursor);
        newColor = LongNoSwap(color);
        v10 = (double)cursorGlyph->y0 * yScale + y;
        w = xScale * (double)cursorGlyph->pixelWidth;
        h = yScale * (double)cursorGlyph->pixelHeight;
        if (Material_HasAnyFogableTechnique(material))
            R_WarnOncePerFrame(R_WARN_FOGABLE_2DTEXT, material->info.name);
        else
            RB_DrawStretchPicRotate(
                material,
                x,
                v10,
                w,
                h,
                cursorGlyph->s0,
                cursorGlyph->t0,
                cursorGlyph->s1,
                cursorGlyph->t1,
                sinAngle,
                cosAngle,
                newColor,
                GFX_PRIM_STATS_HUD);
    }
}

void __cdecl RotateXY(
    float cosAngle,
    float sinAngle,
    float pivotX,
    float pivotY,
    float x,
    float y,
    float *outX,
    float *outY)
{
    float tempOutX; // [esp+0h] [ebp-8h]
    float tempOutY; // [esp+4h] [ebp-4h]

    tempOutX = (x - pivotX) * cosAngle + pivotX - (y - pivotY) * sinAngle;
    tempOutY = (y - pivotY) * cosAngle + pivotY + (x - pivotX) * sinAngle;
    *outX = tempOutX;
    *outY = tempOutY;
}

double __cdecl GetMonospaceWidth(Font_s *font, char renderFlags)
{
    if ((renderFlags & 1) != 0)
        return (double)R_GetCharacterGlyph(font, 0x6Fu)->dx;
    else
        return 0.0;
}

void __cdecl GlowColor(GfxColor *result, GfxColor baseColor, GfxColor forcedGlowColor, char renderFlags)
{
    if ((renderFlags & 0x20) != 0)
    {
        *(_WORD *)((char *)&result->packed + 1) = *(_WORD *)((char *)&forcedGlowColor.packed + 1);
        result->array[0] = forcedGlowColor.array[0];
    }
    else
    {
        result->array[2] = (int)((double)baseColor.array[2] * 0.059999999);
        result->array[1] = (int)((double)baseColor.array[1] * 0.059999999);
        result->array[0] = (int)((double)baseColor.array[0] * 0.059999999);
    }
}

char __cdecl SetupPulseFXVars(
    const char *text,
    int maxLength,
    char renderFlags,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration,
    bool *resultDrawRandChar,
    int *resultRandSeed,
    int *resultMaxLength,
    bool *resultDecaying,
    int *resultdecayTimeElapsed)
{
    int timeRemainder; // [esp+0h] [ebp-24h]
    int timeElapsed; // [esp+8h] [ebp-1Ch]
    int randSeed; // [esp+10h] [ebp-14h] BYREF
    int strLength; // [esp+14h] [ebp-10h]
    bool drawRandCharAtEnd; // [esp+1Bh] [ebp-9h]
    int decayTimeElapsed; // [esp+1Ch] [ebp-8h]
    bool decaying; // [esp+23h] [ebp-1h]
    int maxLengtha; // [esp+30h] [ebp+Ch]

    if ((renderFlags & 0x40) != 0)
    {
        drawRandCharAtEnd = 0;
        randSeed = 1;
        decaying = 0;
        decayTimeElapsed = 0;
        timeElapsed = RB_TextSceneTime() - fxBirthTime;
        iassert( timeElapsed >= 0 );
        strLength = SEH_PrintStrlen(text);
        if (strLength > maxLength)
            strLength = maxLength;
        if (timeElapsed <= fxDecayDuration + fxDecayStartTime)
        {
            if (timeElapsed < fxLetterTime * strLength)
            {
                iassert( fxLetterTime );
                maxLengtha = timeElapsed / fxLetterTime;
                drawRandCharAtEnd = 1;
                timeRemainder = timeElapsed % fxLetterTime;
                if (fxLetterTime / 4)
                    timeRemainder /= fxLetterTime / 4;
                randSeed = maxLengtha + timeRemainder + strLength + fxBirthTime;
                RandWithSeed(&randSeed);
                RandWithSeed(&randSeed);
                maxLength = maxLengtha + 1;
            }
            else if (timeElapsed > fxDecayStartTime)
            {
                decaying = 1;
                randSeed = strLength + fxBirthTime;
                RandWithSeed(&randSeed);
                RandWithSeed(&randSeed);
                decayTimeElapsed = timeElapsed - fxDecayStartTime;
            }
            *resultDrawRandChar = drawRandCharAtEnd;
            *resultRandSeed = randSeed;
            *resultMaxLength = maxLength;
            *resultDecaying = decaying;
            *resultdecayTimeElapsed = decayTimeElapsed;
            return 1;
        }
        else
        {
            *resultDrawRandChar = 0;
            *resultRandSeed = 1;
            *resultMaxLength = maxLength;
            *resultDecaying = 0;
            *resultdecayTimeElapsed = 0;
            return 0;
        }
    }
    else
    {
        *resultDrawRandChar = 0;
        *resultRandSeed = 1;
        *resultMaxLength = maxLength;
        *resultDecaying = 0;
        *resultdecayTimeElapsed = 0;
        return 1;
    }
}

void __cdecl GetDecayingLetterInfo(
    uint32_t letter,
    Font_s *font,
    int *randSeed,
    int decayTimeElapsed,
    int fxBirthTime,
    int fxDecayDuration,
    uint8_t alpha,
    bool *resultSkipDrawing,
    uint8_t *resultAlpha,
    uint32_t *resultLetter,
    bool *resultDrawExtraFxChar)
{
    int scrambleSeed; // [esp+28h] [ebp-20h] BYREF
    float tickRatio; // [esp+2Ch] [ebp-1Ch]
    int tickPeriod; // [esp+30h] [ebp-18h]
    bool drawExtraFxChar; // [esp+37h] [ebp-11h]
    float fade; // [esp+38h] [ebp-10h]
    int tickCount; // [esp+3Ch] [ebp-Ch]
    bool skipDrawing; // [esp+43h] [ebp-5h]
    int timeLimit; // [esp+44h] [ebp-4h]

    skipDrawing = 0;
    fade = 1.0;
    drawExtraFxChar = 0;
    tickRatio = (double)fxDecayDuration / 1000.0;
    tickCount = (int)(tickRatio * 30.0);
    tickPeriod = fxDecayDuration / tickCount;
    timeLimit = fxDecayDuration / tickCount * ((int)RandWithSeed(randSeed) % tickCount);
    if (decayTimeElapsed < timeLimit)
    {
        if (decayTimeElapsed + 60 >= timeLimit)
        {
            scrambleSeed = decayTimeElapsed + letter + fxBirthTime;
            if ((int)RandWithSeed(&scrambleSeed) % 2)
            {
                drawExtraFxChar = 1;
                letter = 79;
            }
            else
            {
                letter = R_FontGetRandomLetter(font, scrambleSeed);
            }
            fade = (double)(decayTimeElapsed + 60 - timeLimit) / 60.0;
            fade = 1.0 - fade;
            fade = (double)alpha / 255.0 * fade;
        }
    }
    else
    {
        skipDrawing = 1;
    }
    *resultSkipDrawing = skipDrawing;
    *resultLetter = letter;
    *resultAlpha = CLAMP(SnapFloatToInt(fade * 255.0f), 0, 255);    
    *resultDrawExtraFxChar = drawExtraFxChar;
}

void __cdecl DrawTextFxExtraCharacter(
    const Material *material,
    int charIndex,
    float x,
    float y,
    float w,
    float h,
    float sinAngle,
    float cosAngle,
    uint32_t color)
{
    float s1; // [esp+38h] [ebp-8h]
    float s0; // [esp+3Ch] [ebp-4h]

    s0 = (double)(charIndex % 16) * 0.0625;
    s1 = s0 + 0.0625;
    RB_DrawStretchPicRotate(material, x, y, w, h, s0, 0.0, s1, 1.0, sinAngle, cosAngle, color, GFX_PRIM_STATS_HUD);
}

uint8_t __cdecl ModulateByteColors(uint8_t colorA, uint8_t colorB)
{
    return (int)((double)colorA / 255.0 * ((double)colorB / 255.0) * 255.0);
}

