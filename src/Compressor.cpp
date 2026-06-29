/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/


#include "Logger.h"
#include <span>
#include <vector>
#include <zconf.h>
#include <zlib.h>
#ifdef __linux__
#include <cstring>
#endif

std::vector<char> Comp(std::span<const char> input) {
    auto max_size = compressBound(input.size());
    std::vector<char> output(max_size);
    uLongf output_size = output.size();
    int res = compress(
        reinterpret_cast<Bytef*>(output.data()),
        &output_size,
        reinterpret_cast<const Bytef*>(input.data()),
        static_cast<uLongf>(input.size()));
    if (res != Z_OK) {
        error("zlib compress() failed (code: " + std::to_string(res) + ", message: " + zError(res) + ")");
        throw std::runtime_error("zlib compress() failed");
    }
    debug("zlib compressed " + std::to_string(input.size()) + " B to " + std::to_string(output_size) + " B");
    output.resize(output_size);
    return output;
}

std::vector<char> DeComp(std::span<const char> input) {
    // An empty body (e.g. a bare 4-byte "ABG:" frame) sizes output_buffer to 0; zlib then loops on
    // Z_BUF_ERROR forever (0*2 stays 0, the size cap never trips) -> a CPU-spinning hang of the recv
    // loop on the host launcher. Reject empty input up front. (Mirrors the server-side DeComp guard.)
    if (input.empty()) {
        return {};
    }
    std::vector<char> output_buffer(std::min<size_t>(input.size() * 5, 15 * 1024 * 1024));

    uLongf output_size = output_buffer.size();

    while (true) {
        int res = uncompress(
            reinterpret_cast<Bytef*>(output_buffer.data()),
            &output_size,
            reinterpret_cast<const Bytef*>(input.data()),
            static_cast<uLongf>(input.size()));
        if (res == Z_BUF_ERROR) {
            if (output_buffer.size() >= 30 * 1024 * 1024) {
                throw std::runtime_error("decompressed packet size of 30 MB exceeded");
            }
            // Grow CAPPED at 30MB (matches the server-side DeComp). Without the std::min, a legit large
            // vehicle config jumps 15MB->30MB->60MB before the throw; the >=30MB guard above still
            // terminates the loop, so capping here is safe and avoids the over-allocation spike.
            output_buffer.resize(std::min<size_t>(output_buffer.size() * 2, 30u * 1024 * 1024));
            debug("zlib uncompress() failed, trying with a larger buffer size of " + std::to_string(output_buffer.size()));
            output_size = output_buffer.size();
        } else if (res != Z_OK) {
            error("zlib uncompress() failed (code: " + std::to_string(res) + ", message: " + zError(res) + ")");
            throw std::runtime_error("zlib uncompress() failed");
        } else if (res == Z_OK) {
            break;
        }
    }    output_buffer.resize(output_size);
    return output_buffer;
}
