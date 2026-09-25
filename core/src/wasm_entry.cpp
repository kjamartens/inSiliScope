// Link anchor for the WASM module: pulls in the C ABI from the static core
// library (EXPORTED_FUNCTIONS keeps the symbols alive). No main().
#include "insilicell/insilicell.h"

extern "C" ISC_API int32_t isc_wasm_anchor(void) { return isc_abi_version(); }
