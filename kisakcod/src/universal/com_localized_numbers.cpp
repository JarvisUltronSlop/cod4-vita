// Source localization helpers shared by desktop and Vita.
#include <qcommon/qcommon.h>
#include <stringed/stringed_hooks.h>

char __cdecl Com_GetDecimalDelimiter()
{
    int lang; // [esp+0h] [ebp-4h]

    lang = loc_language->current.integer;
    if (lang == 1 || lang == 2 || lang == 3 || lang == 4 || lang == 6 || lang == 7 || lang == 14)
        return 44;
    else
        return 46;
}

void __cdecl Com_LocalizedFloatToString(float f, char* buffer, uint32_t maxlen, uint32_t numDecimalPlaces)
{
    uint32_t charPos; // [esp+8h] [ebp-8h]
    char delimiter; // [esp+Fh] [ebp-1h]

    _snprintf(buffer, maxlen - 1, "%.*f", numDecimalPlaces, f);
    buffer[maxlen - 1] = 0;
    delimiter = Com_GetDecimalDelimiter();
    if (delimiter != 46)
    {
        for (charPos = 0; charPos < maxlen; ++charPos)
        {
            if (buffer[charPos] == 46)
            {
                buffer[charPos] = delimiter;
                return;
            }
        }
    }
}

