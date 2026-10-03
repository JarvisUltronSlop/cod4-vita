#include <cstdint>
#include <cstring>

#include <universal/q_shared.h>
#include <script/scr_vm.h>

// These byte readers are intentionally internal to scr_vm.cpp, so keep the
// probe's declarations local instead of widening the public script ABI.
int Scr_ReadInt(const char** position);
float Scr_ReadFloat(const char** position);

// Link a small, dependency-free reader from the real script VM translation
// unit.  This proves the ARM object can be selected from the engine archive
// without pulling the desktop VM lifecycle or a custom script interpreter.
int main()
{
    const char integerBytes[] = { '\x78', '\x56', '\x34', '\x12' };
    const char* integerPosition = integerBytes;
    if (Scr_ReadInt(&integerPosition) != 0x12345678
        || integerPosition != integerBytes + sizeof(integerBytes))
        return 2;

    const char floatBytes[] = { '\x00', '\x00', '\x80', '\x3f' };
    const char* floatPosition = floatBytes;
    if (Scr_ReadFloat(&floatPosition) != 1.0f
        || floatPosition != floatBytes + sizeof(floatBytes))
        return 3;
    return 0;
}
