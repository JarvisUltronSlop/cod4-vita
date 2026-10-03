#include <qcommon/qcommon.h>
#include <cstring>

// Original common.cpp text-field initialization, shared by every platform.
void __cdecl Field_Clear(field_t* edit)
{
    memset((uint8_t*)edit->buffer, 0, sizeof(edit->buffer));
    edit->cursor = 0;
    edit->scroll = 0;
    edit->drawWidth = 256;
}
