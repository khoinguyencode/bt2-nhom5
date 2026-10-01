// client.cpp - Client của giao thức ASP (Array Sum Protocol)
// Client nhận danh sách N số từ server, tính tổng, gửi tổng về server
// rồi in thông báo ĐÚNG / SAI mà server gửi lại.
//
// Biên dịch:  Windows (MinGW): g++ -std=c++11 client.cpp -o client.exe -lws2_32
//             Linux / macOS  : g++ -std=c++11 client.cpp -o client
// Chạy:       client [dia_chi_server] [port] [-m]
//             -m: người dùng tự nhập tổng thay vì để chương trình tính (để thử trường hợp SAI)

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

// ===================== PHẦN DÙNG CHUNG (giống hệt trong server.cpp) =====================

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

// ===================== PHẦN RIÊNG CỦA CLIENT =====================

static void print_usage() {
    std::cerr << "Cach dung: client [dia_chi_server] [port] [-m]\n"
              << "  dia_chi_server : IPv4 (vd 192.168.1.15) hoac ten may (vd localhost), mac dinh 127.0.0.1\n"
              << "  port           : 1..65535, mac dinh " << DEFAULT_PORT << "\n"
              << "  -m             : tu nhap tong tu ban phim thay vi de chuong trinh tinh\n";
}

// Đổi "192.168.1.15" hoặc tên máy (vd "localhost") thành địa chỉ IPv4.
static bool resolve_ipv4(const std::string& host, in_addr& out) {
    auto ip = inet_addr(host.c_str());
    if (ip != INADDR_NONE) {
        out.s_addr = ip;
        return true;
    }
    hostent* entry = gethostbyname(host.c_str());
    if (entry == nullptr || entry->h_addrtype != AF_INET || entry->h_addr_list[0] == nullptr) return false;
    std::memcpy(&out, entry->h_addr_list[0], sizeof(out));
    return true;
}

// Chế độ -m: người dùng tự nhập tổng. Trả về false nếu hết dữ liệu nhập (Ctrl+Z / Ctrl+D).
static bool read_sum_from_keyboard(int64_t& sum) {
    std::string line;
    for (;;) {
        std::cout << "[CLIENT] Nhap tong cua day so tren: ";
        if (!std::getline(std::cin, line)) return false;
        const char* text = line.c_str();
        char* end = nullptr;
        errno = 0;
        long long value = std::strtoll(text, &end, 10);
        const bool has_digits = end != text;
        while (*end == ' ' || *end == '\t' || *end == '\r') ++end;
        if (errno == 0 && has_digits && *end == '\0') {
            sum = value;
            return true;
        }
        std::cout << "[CLIENT] Khong hop le, hay nhap mot so nguyen.\n";
    }
}

// Kiểm tra (không chờ) xem server đã gửi gì tới chưa.
static bool data_waiting(SOCKET s) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval no_wait = {0, 0};
    return select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, &no_wait) > 0;
}

// Một phiên làm việc: nhận ARRAY -> gửi SUM -> nhận RESULT. Trả về mã thoát của chương trình.
static int run_session(SOCKET s, bool manual) {
    // Bước 1: nhận ARRAY = [N: 2 byte][N số nguyên, mỗi số 4 byte]
    Packet pkt;
    if (!receive_expected(s, OP_ARRAY, pkt, "[CLIENT]")) return 1;
    const uint16_t n = get_u16(&pkt.payload[0]);
    std::vector<int32_t> numbers(n);
    for (size_t i = 0; i < numbers.size(); ++i)
        numbers[i] = static_cast<int32_t>(get_u32(&pkt.payload[2 + 4 * i]));

    std::cout << "[CLIENT] Nhan duoc N = " << n << " so:";
    for (size_t i = 0; i < numbers.size(); ++i) std::cout << ' ' << numbers[i];
    std::cout << "\n";

    // Bước 2: tính tổng (hoặc để người dùng tự nhập) rồi gửi SUM = [tổng: 8 byte]
    int64_t sum = 0;
    if (manual) {
        if (!read_sum_from_keyboard(sum)) {
            send_error(s, "Client huy phien lam viec");
            return 1;
        }
        // Nếu người dùng nhập quá lâu, server đã gửi ERROR (hết thời gian chờ) và đóng kết nối.
        // Phải đọc gói đó trước: gửi vào kết nối đã đóng sẽ bị RST và gói ERROR có thể mất.
        if (data_waiting(s)) {
            receive_expected(s, OP_ERROR, pkt, "[CLIENT]");
            return 1;
        }
    } else {
        for (size_t i = 0; i < numbers.size(); ++i) sum += numbers[i];
        std::cout << "[CLIENT] Tong tinh duoc: " << sum << "\n";
    }
    std::vector<uint8_t> sum_payload(8);
    put_u64(&sum_payload[0], static_cast<uint64_t>(sum));
    if (!send_packet(s, OP_SUM, sum_payload)) {
        std::cerr << "[CLIENT] Khong gui duoc tong len server.\n";
        return 1;
    }
    std::cout << "[CLIENT] Da gui tong " << sum << " len server.\n";

    // Bước 3: nhận RESULT = [trạng thái: 1 byte][tổng đúng: 8 byte]
    if (!receive_expected(s, OP_RESULT, pkt, "[CLIENT]")) return 1;
    const bool correct = pkt.payload[0] == 1;
    const int64_t correct_sum = static_cast<int64_t>(get_u64(&pkt.payload[1]));
    std::cout << "[CLIENT] ===== SERVER THONG BAO: " << (correct ? "DUNG" : "SAI")
              << " (tong dung la " << correct_sum << ") =====\n";
    return 0;
}

int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);

    std::string host = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;
    bool manual = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-m") {
            manual = true;
        } else if (!arg.empty() && arg[0] == '-') {
            print_usage();
            return 1;
        } else {
            args.push_back(arg);
        }
    }
    if (args.size() > 2 || (args.size() == 2 && !parse_port(args[1].c_str(), port))) {
        print_usage();
        return 1;
    }
    if (!args.empty()) host = args[0];

    NetworkInit net;
    if (!net.ok) {
        std::cerr << "[CLIENT] Khong khoi tao duoc Winsock.\n";
        return 1;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!resolve_ipv4(host, addr.sin_addr)) {
        std::cerr << "[CLIENT] Khong tim duoc dia chi IPv4 cua \"" << host << "\".\n";
        return 1;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        std::cerr << "[CLIENT] Khong tao duoc socket.\n";
        return 1;
    }

    std::cout << "[CLIENT] Dang ket noi toi " << host << ':' << port << " ...\n";
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[CLIENT] Ket noi that bai. Kiem tra server da chay chua, dia chi/port va tuong lua.\n";
        CLOSE_SOCKET(s);
        return 1;
    }
    std::cout << "[CLIENT] Da ket noi.\n";

    set_recv_timeout(s, RECV_TIMEOUT_SEC);
    int rc = run_session(s, manual);
    close_gracefully(s);
    return rc;
}
