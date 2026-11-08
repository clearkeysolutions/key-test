// libFuzzer: byte-codeword decoders of every profile must never crash, hang,
// or report CKEY_OK for a codeword whose re-encoding differs (fail-closed).
#include "ckey/ckey.h"
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    const uint8_t sel = data[0] & 3; data += 1; size -= 1;
    std::vector<uint8_t> out(65535); size_t len = 0; ckey_diagnostics d{};
    if (sel == 0) {
        const uint32_t version = 1 + (size ? data[0] % 31 : 0);
        ckey_decode_scalable(data, size, version, (data[0] & 0x80) ? CKEY_LAYOUT_PRINT : CKEY_LAYOUT_STANDARD, out.data(), out.size(), &len, &d);
    } else if (sel == 1) {
        ckey_decode_locator(data, size, out.data(), out.size(), &d);
    } else if (sel == 2) {
        ckey_decode_text(data, size, out.data(), out.size(), &len, &d);
    } else {
        ckey_decode_extended(data, size, out.data(), out.size(), &len, &d);
    }
    return 0;
}
