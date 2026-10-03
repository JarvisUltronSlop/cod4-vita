#pragma once
namespace kisak::vita {void VitaCpuShaderCache_Initialize();unsigned VitaCpuShaderCache_CompileFailures();bool VitaCpuShaderCache_WorldPrecisionReady();
bool VitaCpuShaderCache_WorldScalingReady();
void VitaCpuShaderCache_SetWorldScalingReady(bool ready);void VitaCpuShaderCache_SetWorldPrecisionReady(bool ready);}
