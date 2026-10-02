# Bài tập 2 - Nhóm 5

Server gửi `N` số qua TCP; client tính tổng và gửi lại; server báo `DUNG` hoặc `SAI`.

## Biên dịch

Cần `g++` (`g++ --version` để kiểm tra). MSYS2 chỉ là một cách cài `g++` trên Windows.

**Windows (PowerShell):**

```powershell
g++ -std=c++11 server.cpp -o server.exe -lws2_32
g++ -std=c++11 client.cpp -o client.exe -lws2_32
```

**Linux:**

```bash
g++ -std=c++11 server.cpp -o server
g++ -std=c++11 client.cpp -o client
```

## Chạy trên hai máy cùng LAN/Wi-Fi

Máy A chạy server (`192.168.1.3` trong lần test); lấy IPv4 bằng `ipconfig` (Windows) hoặc `ip -4 addr` (Linux), rồi chạy:

```powershell
.\server.exe 8080
```

Máy B chạy client (`192.168.1.5` trong lần test), thay `<IP_SERVER>` bằng IPv4 của máy A:

```powershell
.\client.exe <IP_SERVER> 8080
# Ví dụ: .\client.exe 192.168.1.3 8080
```

Trên Linux, dùng `./server 8080` và `./client <IP_SERVER> 8080`. Thêm `-m` vào lệnh client để tự nhập tổng và thử trường hợp sai. Hai máy phải dùng cùng port; cho phép kết nối TCP vào port `8080` trên firewall của máy A. Muốn đổi port, thay `8080` ở cả hai lệnh.

`protocol.docx` mô tả giao thức; `suggest.docx` mô tả hướng mở rộng.
