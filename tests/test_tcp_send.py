"""Compile the production TCPSend body against a controlled partial-write socket.

Run from an MSVC developer prompt: python tests/test_tcp_send.py
On Linux, CXX (default c++) must support C++20. --revision HEAD tests the old code
without changing the worktree, and must FAIL before upstream #271 is applied.
The socket double forces a split header and contention, then independently checks
every byte of the resulting stream. No production send algorithm is duplicated.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = "src/Network/VehicleEvent.cpp"

HARNESS = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
std::atomic<bool> Terminate = false;
bool g_CombinedMode = false;
uint64_t TCPSock = 7;
std::string UlStatus;
std::vector<std::string> queued;
std::string wire;
std::mutex socketMutex;
std::condition_variable socketCv;
bool firstPartial = false, otherEntered = false, contend = false;
std::thread::id firstWriter;
int sendResult = 1, sendCalls = 0, closed = 0;
void debug(const std::string&) {}
void error(const std::string& s) { throw std::runtime_error(s); }
int WSAGetLastError() { return 10054; }
int KillSocket(uint64_t) { ++closed; return 0; }
void CombinedServerSendTCP(const std::string& s) { queued.push_back(s); }
bool CheckBytes(int32_t Bytes, int32_t Expected = -1);
int send(uint64_t, const char* bytes, int size, int) {
    std::unique_lock guard(socketMutex);
    ++sendCalls;
    if (sendResult <= 0) return sendResult;
    if (contend && !firstPartial) {
        // Stop halfway through the 4-byte header, then give the second caller
        // an opportunity to enter send. A correct frame lock blocks that caller.
        wire.append(bytes, 2);
        firstWriter = std::this_thread::get_id();
        firstPartial = true;
        socketCv.notify_all();
        socketCv.wait_for(guard, 250ms, [] { return otherEntered; });
        return 2;
    }
    if (contend && std::this_thread::get_id() != firstWriter) {
        otherEntered = true;
        socketCv.notify_all();
    }
    const int count = std::min(size, 37); // many short writes, including tail
    wire.append(bytes, count);
    return count;
}
// PRODUCTION_SOURCE
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
// Decode the emitted stream instead of relying on the sender's byte counters.
std::vector<std::string> decode() {
    std::vector<std::string> frames;
    size_t offset = 0;
    while (offset < wire.size()) {
        require(wire.size() - offset >= 4, "incomplete frame header");
        int32_t size;
        std::memcpy(&size, wire.data() + offset, 4);
        offset += 4;
        require(size >= 0 && size_t(size) <= wire.size() - offset,
                "interleaved or incomplete frame");
        frames.emplace_back(wire.substr(offset, size));
        offset += size;
    }
    return frames;
}
int main() {
    try {
        const std::string first = "ABG:" + std::string(32768, 'a');
        const std::string second = "Vi:0-1:{\"t\":1,\"b\":0,\"s\":0}";
        contend = true;
        std::thread a([&] { TCPSend(first, TCPSock); });
        {
            std::unique_lock lock(socketMutex);
            socketCv.wait(lock, [] { return firstPartial; });
        }
        std::thread b([&] { TCPSend(second, TCPSock); });
        a.join();
        b.join();
        require(decode() == std::vector<std::string>{first, second},
                "concurrent partial sends corrupted payloads");
        std::cout << "PASS concurrent partial writes: both complete frames intact\n";

        contend = false;
        for (int failure : {0, -1}) {
            wire.clear();
            Terminate = false;
            sendCalls = 0;
            sendResult = failure;
            TCPSend("C:failed", TCPSock);
            require(Terminate && sendCalls == 1 && wire.empty(),
                    "failed send must terminate without retrying");
            require(UlStatus == "UlDisconnected: Socket Closed Code 2",
                    "send failure diagnostic changed");
            // A subsequent call must acquire the mutex after the early return.
            sendResult = 1;
            TCPSend("C:next connection", TCPSock);
            require(decode() == std::vector<std::string>{"C:next connection"},
                    "failed send did not release its frame lock");
        }
        require(closed == 1, "negative send must close the socket once");
        std::cout << "PASS zero/error sends terminate and release frame lock\n";

        Terminate = false;
        sendCalls = 0;
        TCPSend("C:invalid", uint64_t(-1));
        require(Terminate && sendCalls == 0 &&
                UlStatus == "UlDisconnected: Invalid Socket",
                "invalid socket must fail before writing");
        std::cout << "PASS invalid socket retains existing failure behavior\n";

        Terminate = false;
        g_CombinedMode = true;
        TCPSend(second, uint64_t(-1));
        require(!Terminate && sendCalls == 0 &&
                queued == std::vector<std::string>{second},
                "combined mode must enqueue raw message without socket framing");
        std::cout << "PASS combined mode preserves raw message boundary\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL " << ex.what() << '\n';
        return 1;
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="read source from this Git revision")
    args = parser.parse_args()
    source = (
        subprocess.check_output(
            ["git", "show", f"{args.revision}:{SOURCE_PATH}"], cwd=ROOT, text=True
        )
        if args.revision
        else (ROOT / SOURCE_PATH).read_text(encoding="utf-8")
    )
    # These boundaries intentionally fail loudly if upstream reorganizes the file.
    production = source[
        source.index("bool CheckBytes(") : source.index("int RecvWaitAll(")
    ]
    with tempfile.TemporaryDirectory(prefix="tcp-send-test-") as temporary:
        work = Path(temporary)
        cpp = work / "tcp_send_test.cpp"
        exe = work / ("tcp_send_test.exe" if os.name == "nt" else "tcp_send_test")
        cpp.write_text(HARNESS.replace("// PRODUCTION_SOURCE", production), encoding="utf-8")
        if os.name == "nt":
            command = ["cl", "/nologo", "/std:c++20", "/EHsc", str(cpp), f"/Fe:{exe}"]
        else:
            command = [os.environ.get("CXX", "c++"), "-std=c++20", "-pthread", str(cpp), "-o", str(exe)]
        subprocess.run(command, cwd=work, check=True)
        return subprocess.run([str(exe)], cwd=work, timeout=15).returncode


if __name__ == "__main__":
    raise SystemExit(main())
