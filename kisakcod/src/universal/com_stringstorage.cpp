#include <universal/q_shared.h>
#include <script/scr_stringlist.h>
#include <script/scr_memorytree.h>

/*
========================
CopyString

 NOTE:	never write over the memory CopyString returns because
		memory from a memstatic_t might be returned
========================
*/
const char* CopyString(const char* in)
{
	uint32_t out;

	iassert(in);

	out = SL_GetString_(in, 0, MT_TYPE_GENERIC);
	return SL_ConvertToString(out);
}

void FreeString(const char* str)
{
	iassert(str);

	uint32_t out = SL_FindString(str);

	iassert(out);

	SL_RemoveRefToString(out);
}


