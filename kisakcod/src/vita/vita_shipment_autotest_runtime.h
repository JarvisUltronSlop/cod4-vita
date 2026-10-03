#pragma once
#include <vita/vita_shipment_autotest.h>
#if KISAK_VITA_CPU_PROFILE && KISAK_VITA_SOURCE_RUNTIME
namespace kisak::vita {
// All accesses occur on the sole source/frontend owner, including text layout.
inline VitaShipmentAutoTest shipmentAutoTest;
}
#endif
