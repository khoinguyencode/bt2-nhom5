// server.cpp - Server của giao thức ASP (Array Sum Protocol)
// Server sinh ngẫu nhiên N số nguyên, gửi cho client, nhận tổng do client tính
// rồi gửi lại thông báo ĐÚNG / SAI kèm tổng chính xác.
//
// Biên dịch:  Windows (MinGW): g++ -std=c++11 server.cpp -o server.exe -lws2_32
//             Linux / macOS  : g++ -std=c++11 server.cpp -o server
// Chạy:       server [port]              (mặc định port 8080)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
    #ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
    #define _WINSOCK_DEPRECATED_NO_WARNINGS  // MSVC coi inet_addr/inet_ntoa/gethostbyname là "deprecated"
    #endif
    #include <winsock2.h>
    #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")       // chỉ MSVC tự liên kết; g++ cần thêm -lws2_32
    #endif
    typedef int socklen_t;
    #define CLOSE_SOCKET closesocket
    #define SHUT_SEND SD_SEND
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <signal.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <unistd.h>
    typedef int SOCKET;
    const SOCKET INVALID_SOCKET = -1;
    #define CLOSE_SOCKET close
    #define SHUT_SEND SHUT_WR
#endif

// ===================== PHẦN DÙNG CHUNG (giống hệt trong client.cpp) =====================

const uint16_t DEFAULT_PORT = 8080;
const uint8_t  MAGIC        = 0xAA;
const size_t   HEADER_SIZE  = 4;                 // Magic(1) + Opcode(1) + Length(2)
const uint16_t MAX_N        = (0xFFFF - 2) / 4;  // 16383: để 2 + 4N vẫn vừa trường Length 16 bit

#ifndef RECV_TIMEOUT_SEC
#define RECV_TIMEOUT_SEC 60                      // chờ mỗi gói tin tối đa 60 giây
#endif

enum Opcode : uint8_t {
    OP_ARRAY  = 0x01,  // Server -> Client: N và N số nguyên
    OP_SUM    = 0x02,  // Client -> Server: tổng client tính được
    OP_RESULT = 0x03,  // Server -> Client: ĐÚNG/SAI và tổng chính xác
    OP_ERROR  = 0xFF   // Hai chiều: báo lỗi giao thức, sau đó đóng kết nối
};

struct Packet {
    uint8_t opcode;
    std::vector<uint8_t> payload;
};

enum RecvStatus { RECV_OK, RECV_CLOSED, RECV_TIMEOUT, RECV_ERROR, RECV_BAD_MAGIC };

// Khởi tạo / giải phóng thư viện socket (Winsock chỉ cần trên Windows).
struct NetworkInit {
    bool ok;
    NetworkInit() : ok(true) {
#ifdef _WIN32
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        signal(SIGPIPE, SIG_IGN);  // gửi vào kết nối đã đóng thì báo lỗi thay vì làm chết chương trình
#endif
    }
    ~NetworkInit() {
#ifdef _WIN32
        if (ok) WSACleanup();
#endif
    }
};

// Đọc số port 1..65535; từ chối các chuỗi như "abc", "80abc", "0", "70000".
static bool parse_port(const char* text, uint16_t& port) {
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

// Ghi / đọc số nguyên theo thứ tự Big-Endian (Network Byte Order) bằng htons/htonl/ntohs/ntohl.
static void put_u16(uint8_t* p, uint16_t v) { uint16_t n = htons(v); std::memcpy(p, &n, 2); }
static void put_u32(uint8_t* p, uint32_t v) { uint32_t n = htonl(v); std::memcpy(p, &n, 4); }
static void put_u64(uint8_t* p, uint64_t v) {
    put_u32(p, static_cast<uint32_t>(v >> 32));  // 4 byte cao đứng trước
    put_u32(p + 4, static_cast<uint32_t>(v));
}
static uint16_t get_u16(const uint8_t* p) { uint16_t n; std::memcpy(&n, p, 2); return ntohs(n); }
static uint32_t get_u32(const uint8_t* p) { uint32_t n; std::memcpy(&n, p, 4); return ntohl(n); }
static uint64_t get_u64(const uint8_t* p) {
    return (static_cast<uint64_t>(get_u32(p)) << 32) | get_u32(p + 4);
}

static bool last_error_is_timeout() {
#ifdef _WIN32
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static void set_recv_timeout(SOCKET s, int seconds) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(seconds) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// TCP là luồng byte: một lần send/recv có thể chỉ xử lý một phần dữ liệu, nên phải lặp đến khi đủ.
static bool send_all(SOCKET s, const uint8_t* data, size_t len) {
    while (len > 0) {
        int sent = static_cast<int>(send(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0));
        if (sent <= 0) return false;
        data += sent;
        len -= static_cast<size_t>(sent);
    }
    return true;
}

static RecvStatus recv_all(SOCKET s, uint8_t* data, size_t len) {
    while (len > 0) {
        int got = static_cast<int>(recv(s, reinterpret_cast<char*>(data), static_cast<int>(len), 0));
        if (got == 0) return RECV_CLOSED;
        if (got < 0) return last_error_is_timeout() ? RECV_TIMEOUT : RECV_ERROR;
        data += got;
        len -= static_cast<size_t>(got);
    }
    return RECV_OK;
}

// Ghép Header + Payload thành một khối rồi gửi một lần.
static bool send_packet(SOCKET s, uint8_t opcode, const std::vector<uint8_t>& payload) {
    if (payload.size() > 0xFFFF) return false;
    std::vector<uint8_t> buf(HEADER_SIZE + payload.size());
    buf[0] = MAGIC;
    buf[1] = opcode;
    put_u16(&buf[2], static_cast<uint16_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), buf.begin() + HEADER_SIZE);
    return send_all(s, buf.data(), buf.size());
}

static void send_error(SOCKET s, const std::string& message) {
    send_packet(s, OP_ERROR, std::vector<uint8_t>(message.begin(), message.end()));
}

// Đọc Header 4 byte, sau đó đọc đúng Length byte Payload.
static RecvStatus recv_packet(SOCKET s, Packet& pkt) {
    uint8_t header[HEADER_SIZE];
    RecvStatus status = recv_all(s, header, HEADER_SIZE);
    if (status != RECV_OK) return status;
    if (header[0] != MAGIC) return RECV_BAD_MAGIC;  // không tin được Length nên dừng luôn
    pkt.opcode = header[1];
    pkt.payload.assign(get_u16(&header[2]), 0);
    if (pkt.payload.empty()) return RECV_OK;
    return recv_all(s, pkt.payload.data(), pkt.payload.size());
}

// Kiểm tra Length có khớp với cấu trúc Payload của từng loại gói tin không.
static bool payload_valid(const Packet& pkt) {
    const std::vector<uint8_t>& p = pkt.payload;
    switch (pkt.opcode) {
    case OP_ARRAY: {
        if (p.size() < 2) return false;
        uint16_t n = get_u16(&p[0]);
        return n >= 1 && n <= MAX_N && p.size() == 2 + 4u * n;
    }
    case OP_SUM:    return p.size() == 8;
    case OP_RESULT: return p.size() == 9 && p[0] <= 1;
    case OP_ERROR:  return true;
    default:        return false;
    }
}

// Nhận một gói tin và kiểm tra đúng loại mong đợi. Nếu bên kia vi phạm giao thức
// thì gửi lại gói ERROR nêu lý do. Trả về true khi nhận được gói hợp lệ.
static bool receive_expected(SOCKET s, uint8_t expected, Packet& pkt, const char* tag) {
    switch (recv_packet(s, pkt)) {
    case RECV_OK:
        break;
    case RECV_CLOSED:
        std::cerr << tag << " Ben kia da dong ket noi.\n";
        return false;
    case RECV_TIMEOUT:
        std::cerr << tag << " Qua " << RECV_TIMEOUT_SEC << " giay khong nhan duoc du lieu.\n";
        send_error(s, "Qua thoi gian cho, dong ket noi");
        return false;
    case RECV_ERROR:
        std::cerr << tag << " Loi khi nhan du lieu (ket noi bi ngat).\n";
        return false;
    case RECV_BAD_MAGIC:
        std::cerr << tag << " Du lieu khong thuoc giao thuc ASP (sai magic byte).\n";
        send_error(s, "Sai magic byte");
        return false;
    }
    if (pkt.opcode == OP_ERROR) {
        std::cerr << tag << " Ben kia bao loi: "
                  << std::string(pkt.payload.begin(), pkt.payload.end()) << "\n";
        return false;
    }
    if (pkt.opcode != expected || !payload_valid(pkt)) {
        std::cerr << tag << " Goi tin sai giao thuc (opcode 0x" << std::hex << static_cast<int>(pkt.opcode)
                  << std::dec << ", length " << pkt.payload.size() << ").\n";
        send_error(s, "Sai opcode hoac sai do dai goi tin");
        return false;
    }
    return true;
}

// Đóng kết nối "êm": báo hết dữ liệu gửi (FIN), đọc bỏ phần còn lại trong thời gian ngắn rồi mới đóng.
// Nếu đóng ngay khi còn dữ liệu chưa đọc, hệ điều hành gửi RST và bên kia có thể mất gói tin cuối.
static void close_gracefully(SOCKET s) {
    shutdown(s, SHUT_SEND);
    set_recv_timeout(s, 1);
    char discard[512];
    for (int i = 0; i < 16 && recv(s, discard, sizeof(discard), 0) > 0; ++i) {
    }
    CLOSE_SOCKET(s);
}

// ===================== PHẦN RIÊNG CỦA SERVER =====================

const int N_MIN     = 5;     // mỗi đề có từ 5 đến 10 số
const int N_MAX     = 10;
const int VALUE_MIN = -100;  // mỗi số nằm trong [-100, 100]
const int VALUE_MAX = 100;

// random_device trên MinGW cũ (GCC < 9.2) luôn trả cùng một dãy, nên trộn thêm thời gian hiện tại.
static std::mt19937 make_rng() {
    std::random_device rd;
    std::seed_seq seed{rd(), static_cast<unsigned>(std::time(nullptr)),
                       static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    return std::mt19937(seed);
}

// Xử lý trọn một phiên với một client: gửi ARRAY -> nhận SUM -> gửi RESULT.
static void handle_client(SOCKET client, std::mt19937& rng) {
    std::uniform_int_distribution<int> pick_n(N_MIN, N_MAX);
    std::uniform_int_distribution<int> pick_value(VALUE_MIN, VALUE_MAX);

    const uint16_t n = static_cast<uint16_t>(pick_n(rng));
    std::vector<int32_t> numbers(n);
    int64_t expected_sum = 0;
    for (size_t i = 0; i < numbers.size(); ++i) {
        numbers[i] = pick_value(rng);
        expected_sum += numbers[i];
    }

    std::cout << "[SERVER] De bai: N = " << n << ", day so:";
    for (size_t i = 0; i < numbers.size(); ++i) std::cout << ' ' << numbers[i];
    std::cout << "\n[SERVER] Tong dung: " << expected_sum << "\n";

    // Bước 1: gửi ARRAY = [N: 2 byte][N số nguyên, mỗi số 4 byte]
    std::vector<uint8_t> array_payload(2 + 4 * numbers.size());
    put_u16(&array_payload[0], n);
    for (size_t i = 0; i < numbers.size(); ++i)
        put_u32(&array_payload[2 + 4 * i], static_cast<uint32_t>(numbers[i]));
    if (!send_packet(client, OP_ARRAY, array_payload)) {
        std::cerr << "[SERVER] Khong gui duoc goi ARRAY.\n";
        return;
    }

    // Bước 2: nhận SUM = [tổng: 8 byte]
    Packet pkt;
    if (!receive_expected(client, OP_SUM, pkt, "[SERVER]")) return;
    const int64_t client_sum = static_cast<int64_t>(get_u64(&pkt.payload[0]));
    const bool correct = (client_sum == expected_sum);
    std::cout << "[SERVER] Client gui tong: " << client_sum << " -> " << (correct ? "DUNG" : "SAI") << "\n";

    // Bước 3: gửi RESULT = [trạng thái: 1 byte][tổng đúng: 8 byte]
    std::vector<uint8_t> result_payload(9);
    result_payload[0] = static_cast<uint8_t>(correct ? 1 : 0);
    put_u64(&result_payload[1], static_cast<uint64_t>(expected_sum));
    if (!send_packet(client, OP_RESULT, result_payload))
        std::cerr << "[SERVER] Khong gui duoc goi RESULT.\n";
}

int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);  // in log ngay, không đợi đầy bộ đệm

    uint16_t port = DEFAULT_PORT;
    if (argc > 2 || (argc == 2 && !parse_port(argv[1], port))) {
        std::cerr << "Cach dung: server [port]   (port tu 1 den 65535, mac dinh " << DEFAULT_PORT << ")\n";
        return 1;
    }

    NetworkInit net;
    if (!net.ok) {
        std::cerr << "[SERVER] Khong khoi tao duoc Winsock.\n";
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        std::cerr << "[SERVER] Khong tao duoc socket.\n";
        return 1;
    }

#ifndef _WIN32
    // Cho phép chạy lại server ngay trên cùng port (Linux giữ port ở trạng thái TIME_WAIT một lúc).
    // Không dùng trên Windows vì ở đó SO_REUSEADDR cho phép chương trình khác chiếm cùng port.
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // lắng nghe trên mọi địa chỉ IP của máy này
    addr.sin_port = htons(port);

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[SERVER] Khong bind duoc port " << port << " (co the port dang bi chuong trinh khac dung).\n";
        CLOSE_SOCKET(listener);
        return 1;
    }
    if (listen(listener, SOMAXCONN) != 0) {
        std::cerr << "[SERVER] listen() that bai.\n";
        CLOSE_SOCKET(listener);
        return 1;
    }

    std::cout << "[SERVER] Dang lang nghe port " << port << " tren moi dia chi IP cua may nay.\n"
              << "[SERVER] Phuc vu lan luot tung client, moi goi tin cho toi da " << RECV_TIMEOUT_SEC << " giay.\n"
              << "[SERVER] Nhan Ctrl+C de dung.\n";

    std::mt19937 rng = make_rng();
    unsigned long session = 0;
    for (;;) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client == INVALID_SOCKET) {
            std::cerr << "[SERVER] accept() loi, bo qua.\n";
            continue;
        }

        ++session;
        std::cout << "\n[SERVER] Phien #" << session << ": client " << inet_ntoa(client_addr.sin_addr)
                  << ':' << ntohs(client_addr.sin_port) << " da ket noi.\n";
        set_recv_timeout(client, RECV_TIMEOUT_SEC);
        handle_client(client, rng);
        close_gracefully(client);
        std::cout << "[SERVER] Phien #" << session << " ket thuc.\n";
    }
}
