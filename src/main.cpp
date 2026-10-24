#define NOMINMAX
#include <windows.h>
#include <winspool.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <array>
#include <algorithm>
#include <streambuf>
#include <limits>
#include <charconv>
#include <cctype>
#include <chrono>
#include <thread>
#include <stop_token>
#include <sstream>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iomanip>
#include <optional>
#include <string_view>

#if defined(_MSC_VER)
#include <setupapi.h>
#include <usbprint.h>
#include <libcapt/BasicCaptPrinter.hpp>
#include <libcapt/Compression/ScoaStreambuf.hpp>
#include <libcapt/Protocol/Protocol.hpp>
#include <libcapt/Protocol/Enums.hpp>
#include <libcapt/Protocol/PageParams.hpp>
#include <libcapt/Utility/BufferedPage.hpp>
#endif

static void print_win_error(const wchar_t* what) {
    std::wcerr << what << L" failed. Win32 error: " << GetLastError() << L"\n";
}

static bool list_printers() {
    DWORD needed = 0, returned = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2,
                  nullptr, 0, &needed, &returned);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
        print_win_error(L"EnumPrinters(size query)");
        return false;
    }
    std::vector<BYTE> buffer(needed);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2,
                       buffer.data(), needed, &needed, &returned)) {
        print_win_error(L"EnumPrinters");
        return false;
    }
    auto* info = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
    std::wcout << L"Printers visible to Windows:\n";
    for (DWORD i = 0; i < returned; ++i) {
        std::wcout << L"  " << (info[i].pPrinterName ? info[i].pPrinterName : L"(unnamed)") << L"\n";
    }
    return true;
}

static bool scan_usb_printers() {
#if defined(_MSC_VER)
    constexpr GUID usbPrintInterfaceGuid{
        0x28d78fad, 0x5a12, 0x11d1, {0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2}};
    HDEVINFO devices = SetupDiGetClassDevsW(&usbPrintInterfaceGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        print_win_error(L"SetupDiGetClassDevs");
        return false;
    }

    bool found = false;
    bool queried = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &usbPrintInterfaceGuid,
                                         index, &interfaceData)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                print_win_error(L"SetupDiEnumDeviceInterfaces");
            }
            break;
        }

        found = true;
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
            print_win_error(L"SetupDiGetDeviceInterfaceDetail(size query)");
            continue;
        }

        std::vector<BYTE> detailBuffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required,
                                              nullptr, &deviceInfo)) {
            print_win_error(L"SetupDiGetDeviceInterfaceDetail");
            continue;
        }

        wchar_t friendlyName[256]{};
        DWORD propertySize = 0;
        if (SetupDiGetDeviceRegistryPropertyW(devices, &deviceInfo, SPDRP_FRIENDLYNAME,
                                             nullptr, reinterpret_cast<PBYTE>(friendlyName),
                                             sizeof(friendlyName), &propertySize)) {
            std::wcout << L"USB printer: " << friendlyName << L"\n";
        } else {
            std::wcout << L"USB printer interface detected.\n";
        }

        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            print_win_error(L"Open USBPRINT interface");
            continue;
        }

        std::array<BYTE, 4094> idBuffer{};
        DWORD bytesReturned = 0;
        if (DeviceIoControl(device, IOCTL_USBPRINT_GET_1284_ID, nullptr, 0,
                            idBuffer.data(), static_cast<DWORD>(idBuffer.size()),
                            &bytesReturned, nullptr) && bytesReturned >= 2) {
            const DWORD idLength = static_cast<DWORD>(idBuffer[0]) |
                                   (static_cast<DWORD>(idBuffer[1]) << 8);
            const DWORD available = (std::min)(idLength, bytesReturned - 2);
            std::wstring deviceId;
            for (DWORD i = 0; i < available && idBuffer[i + 2] != 0; ++i) {
                deviceId.push_back(static_cast<wchar_t>(idBuffer[i + 2]));
            }
            std::wcout << L"IEEE 1284 ID: " << deviceId << L"\n";
            queried = true;
        } else {
            print_win_error(L"IOCTL_USBPRINT_GET_1284_ID");
        }

        BYTE status = 0;
        if (DeviceIoControl(device, IOCTL_USBPRINT_GET_LPT_STATUS, nullptr, 0,
                            &status, sizeof(status), &bytesReturned, nullptr)) {
            std::wcout << L"USB printer status byte: 0x" << std::hex
                       << static_cast<unsigned>(status) << std::dec << L"\n";
        } else {
            print_win_error(L"IOCTL_USBPRINT_GET_LPT_STATUS");
        }
        CloseHandle(device);
    }

    SetupDiDestroyDeviceInfoList(devices);
    if (!found) {
        std::wcout << L"No present USBPRINT interfaces found.\n";
    }
    return queried;
#else
    std::wcerr << L"--usb-scan requires an MSVC build with the Windows SDK.\n";
    return false;
#endif
}

#if defined(_MSC_VER)
static HANDLE open_lbp810_usbprint() {
    constexpr GUID usbPrintInterfaceGuid{
        0x28d78fad, 0x5a12, 0x11d1, {0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2}};
    HDEVINFO devices = SetupDiGetClassDevsW(&usbPrintInterfaceGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        print_win_error(L"SetupDiGetClassDevs");
        return INVALID_HANDLE_VALUE;
    }

    HANDLE printer = INVALID_HANDLE_VALUE;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &usbPrintInterfaceGuid,
                                         index, &interfaceData)) {
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
            continue;
        }
        std::vector<BYTE> detailBuffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required,
                                              nullptr, &deviceInfo)) {
            continue;
        }

        std::array<wchar_t, 256> friendlyName{};
        DWORD propertySize = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &deviceInfo, SPDRP_FRIENDLYNAME,
                                               nullptr, reinterpret_cast<PBYTE>(friendlyName.data()),
                                               static_cast<DWORD>(sizeof(friendlyName)), &propertySize) ||
            std::wstring(friendlyName.data()).find(L"LBP-810") == std::wstring::npos) {
            continue;
        }

        if (printer != INVALID_HANDLE_VALUE) {
            CloseHandle(printer);
            printer = INVALID_HANDLE_VALUE;
            std::wcerr << L"More than one Canon LBP-810 USB interface is present; refusing an ambiguous device selection.\n";
            break;
        }
        printer = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (printer == INVALID_HANDLE_VALUE) {
            print_win_error(L"Open Canon LBP-810 USBPRINT interface");
        }
    }

    SetupDiDestroyDeviceInfoList(devices);
    if (printer == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Canon LBP-810 USBPRINT interface was not opened.\n";
    }
    return printer;
}

class UsbPrintStreambuf final : public std::streambuf {
public:
    explicit UsbPrintStreambuf(HANDLE device) noexcept : device_(device) {
        setg(input_.data(), input_.data(), input_.data());
    }

    ~UsbPrintStreambuf() override {
        if (device_ != INVALID_HANDLE_VALUE) {
            CloseHandle(device_);
        }
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        DWORD received = 0;
        if (!ReadFile(device_, input_.data(), static_cast<DWORD>(input_.size()), &received, nullptr)) {
            print_win_error(L"ReadFile(CAPT response)");
            return traits_type::eof();
        }
        if (received == 0) {
            return traits_type::eof();
        }
        setg(input_.data(), input_.data(), input_.data() + received);
        return traits_type::to_int_type(*gptr());
    }

    std::streamsize xsputn(const char* data, std::streamsize size) override {
        std::streamsize totalWritten = 0;
        while (totalWritten < size) {
            const auto remaining = static_cast<unsigned long long>(size - totalWritten);
            const DWORD chunk = static_cast<DWORD>((std::min)(
                remaining, static_cast<unsigned long long>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(device_, data + totalWritten, chunk, &written, nullptr) || written == 0) {
                print_win_error(L"WriteFile(CAPT command)");
                break;
            }
            totalWritten += written;
        }
        return totalWritten;
    }

    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        const char byte = traits_type::to_char_type(character);
        return xsputn(&byte, 1) == 1 ? character : traits_type::eof();
    }

    int sync() override {
        return 0;
    }

private:
    HANDLE device_;
    std::array<char, 4096> input_{};
};
#endif

static bool query_capt_printer_info() {
#if defined(_MSC_VER)
    HANDLE device = open_lbp810_usbprint();
    if (device == INVALID_HANDLE_VALUE) {
        return false;
    }

    UsbPrintStreambuf buffer(device);
    std::iostream stream(&buffer);
    try {
        const Capt::PrinterInfo info = Capt::Protocol::PC_GET_PRINTER_INFO(stream);
        std::wcout << L"CAPT device id: " << static_cast<unsigned>(info.DeviceId)
                   << L", type: " << static_cast<unsigned>(info.Type)
                   << L", firmware: " << static_cast<unsigned>(info.VersionMajor) << L'.'
                   << static_cast<unsigned>(info.VersionMinor)
                   << L", block size: " << info.BlockSize
                   << L", buffers: " << info.Buffers << L"\n";
        return true;
    } catch (const std::exception& error) {
        std::cerr << "CAPT printer-info query failed: " << error.what() << "\n";
        return false;
    }
#else
    std::wcerr << L"--capt-info requires an MSVC build with the Windows SDK.\n";
    return false;
#endif
}

// The LBP-810 always receives an A4 canvas at 600 dpi with one bit per pixel.
// Rasters use PBM/libcapt polarity: a set bit is a black dot, a clear bit is paper.
constexpr unsigned kPageWidth = 4960;
constexpr unsigned kPageHeight = 7014;
constexpr unsigned kPageLineBytes = kPageWidth / 8;
constexpr unsigned kMaxPbmLines = 7040;

struct MonoPage {
    unsigned width = 0;
    unsigned height = 0;
    std::string raster;
};

using PageSource = std::function<std::optional<MonoPage>()>;

static std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::ostringstream text;
    text << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return text.str();
}

static std::string read_pbm_token(std::istream& input) {
    for (;;) {
        const int next = input.peek();
        if (next == std::char_traits<char>::eof()) {
            throw std::runtime_error("PBM header ended unexpectedly");
        }
        if (std::isspace(static_cast<unsigned char>(next))) {
            input.get();
            continue;
        }
        if (next == '#') {
            input.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
            continue;
        }
        break;
    }

    std::string token;
    while (input.good()) {
        const int next = input.peek();
        if (next == std::char_traits<char>::eof() ||
            std::isspace(static_cast<unsigned char>(next))) {
            break;
        }
        token.push_back(static_cast<char>(input.get()));
    }
    if (token.empty()) {
        throw std::runtime_error("PBM header contains an empty token");
    }

    const int delimiter = input.get();
    if (delimiter == std::char_traits<char>::eof() ||
        !std::isspace(static_cast<unsigned char>(delimiter))) {
        throw std::runtime_error("PBM header has no raster delimiter");
    }
    if (delimiter == '\r' && input.peek() == '\n') {
        input.get();
    }
    return token;
}

static unsigned parse_pbm_dimension(const std::string& token) {
    unsigned value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        throw std::runtime_error("PBM width and height must be unsigned integers");
    }
    return value;
}

// Reads the next image of a (possibly multi-image) binary PBM stream; nullopt at end of stream.
static std::optional<MonoPage> read_pbm_page(std::istream& input) {
    if (input.peek() == std::char_traits<char>::eof()) {
        return std::nullopt;
    }
    if (read_pbm_token(input) != "P4") {
        throw std::runtime_error("only binary PBM (P4) is supported");
    }
    MonoPage page;
    page.width = parse_pbm_dimension(read_pbm_token(input));
    page.height = parse_pbm_dimension(read_pbm_token(input));
    if (page.width == 0 || page.width > kPageWidth || page.width % 8 != 0 ||
        page.height == 0 || page.height > kMaxPbmLines) {
        throw std::runtime_error("PBM dimensions must fit A4 at 600 dpi and width must be divisible by 8");
    }

    page.raster.resize(static_cast<std::size_t>(page.width / 8) * page.height);
    input.read(page.raster.data(), static_cast<std::streamsize>(page.raster.size()));
    if (input.gcount() != static_cast<std::streamsize>(page.raster.size())) {
        throw std::runtime_error("PBM raster is truncated");
    }
    return page;
}

static uint32_t read_big_endian_u32(const std::array<unsigned char, 1796>& header,
                                    std::size_t offset) {
    return (static_cast<uint32_t>(header[offset]) << 24) |
           (static_cast<uint32_t>(header[offset + 1]) << 16) |
           (static_cast<uint32_t>(header[offset + 2]) << 8) |
           static_cast<uint32_t>(header[offset + 3]);
}

// Decodes PWG Raster (PWG 5102.4) pages into 600-dpi A4 MonoPage rasters.
// Supported document types: Srgb_8/Rgb_8/AdobeRgb_8, Sgray_8/Gray_8, Black_8,
// Sgray_1/Gray_1 and Black_1 at 300 or 600 dpi. Smaller media is placed at the
// top-left corner of the A4 canvas; larger media is cropped.
class PwgPageReader {
public:
    explicit PwgPageReader(std::istream& input) : input_(input) {
        std::array<char, 4> sync{};
        input_.read(sync.data(), sync.size());
        if (input_.gcount() != static_cast<std::streamsize>(sync.size()) ||
            std::string_view(sync.data(), sync.size()) != "RaS2") {
            throw std::runtime_error("expected a PWG Raster RaS2 stream");
        }
    }

    unsigned pages_read() const noexcept {
        return pages_;
    }

    std::optional<MonoPage> next() {
        if (input_.peek() == std::char_traits<char>::eof()) {
            if (pages_ == 0) {
                throw std::runtime_error("PWG stream contains no pages");
            }
            return std::nullopt;
        }

        std::array<unsigned char, 1796> header{};
        input_.read(reinterpret_cast<char*>(header.data()), header.size());
        if (input_.gcount() != static_cast<std::streamsize>(header.size())) {
            throw std::runtime_error("truncated PWG page header");
        }

        const uint32_t dpiX = read_big_endian_u32(header, 276);
        const uint32_t dpiY = read_big_endian_u32(header, 280);
        const uint32_t width = read_big_endian_u32(header, 372);
        const uint32_t height = read_big_endian_u32(header, 376);
        const uint32_t bitsPerColor = read_big_endian_u32(header, 384);
        const uint32_t bitsPerPixel = read_big_endian_u32(header, 388);
        const uint32_t bytesPerLine = read_big_endian_u32(header, 392);
        const uint32_t colorOrder = read_big_endian_u32(header, 396);
        const uint32_t colorSpace = read_big_endian_u32(header, 400);

        if ((dpiX != 300 && dpiX != 600) || dpiY != dpiX) {
            throw std::runtime_error("PWG page resolution must be 300x300 or 600x600 dpi");
        }
        if (width == 0 || height == 0 || width > 2 * kPageWidth || height > 2 * kPageHeight) {
            throw std::runtime_error("PWG page dimensions are out of range");
        }
        if (colorOrder != 0) {
            throw std::runtime_error("PWG page must use chunky color order");
        }

        enum class Format { Rgb8, Gray8, Gray1 } format;
        bool inkValues = false;  // true for Black_*: larger values mean more toner
        if ((colorSpace == 1 || colorSpace == 19 || colorSpace == 20) &&
            bitsPerColor == 8 && bitsPerPixel == 24) {
            format = Format::Rgb8;
        } else if ((colorSpace == 0 || colorSpace == 18 || colorSpace == 3) &&
                   bitsPerColor == 8 && bitsPerPixel == 8) {
            format = Format::Gray8;
            inkValues = colorSpace == 3;
        } else if ((colorSpace == 0 || colorSpace == 18 || colorSpace == 3) &&
                   bitsPerColor == 1 && bitsPerPixel == 1) {
            format = Format::Gray1;
            inkValues = colorSpace == 3;
        } else {
            std::ostringstream message;
            message << "unsupported PWG color space " << colorSpace << " with "
                    << bitsPerColor << " bits per color and " << bitsPerPixel << " bits per pixel";
            throw std::runtime_error(message.str());
        }
        if (bytesPerLine != (static_cast<uint64_t>(width) * bitsPerPixel + 7) / 8) {
            throw std::runtime_error("PWG BytesPerLine does not match width and bits per pixel");
        }

        const uint32_t scale = dpiX == 300 ? 2u : 1u;
        const std::size_t unit = bitsPerPixel >= 8 ? bitsPerPixel / 8 : 1;
        const unsigned char blankByte = inkValues ? 0x00 : 0xff;
        const auto isDark = [&](const std::vector<unsigned char>& row, uint32_t x) -> bool {
            switch (format) {
            case Format::Rgb8: {
                const std::size_t offset = static_cast<std::size_t>(x) * 3;
                const unsigned luminance = (299u * row[offset] + 587u * row[offset + 1] +
                                            114u * row[offset + 2] + 500u) / 1000u;
                return luminance < kDarkThreshold;
            }
            case Format::Gray8:
                return inkValues ? (255u - row[x]) < kDarkThreshold : row[x] < kDarkThreshold;
            case Format::Gray1: {
                const bool bit = ((row[x / 8] >> (7 - x % 8)) & 1u) != 0;
                return inkValues ? bit : !bit;
            }
            }
            return false;
        };

        MonoPage page;
        page.width = kPageWidth;
        page.height = kPageHeight;
        page.raster.assign(static_cast<std::size_t>(kPageLineBytes) * kPageHeight, '\0');

        std::vector<unsigned char> row(bytesPerLine);
        std::array<char, kPageLineBytes> outputRow{};
        uint32_t sourceY = 0;
        while (sourceY < height) {
            const uint32_t rowCopies = static_cast<uint32_t>(read_byte()) + 1;
            if (rowCopies > height - sourceY) {
                throw std::runtime_error("PWG row repeat exceeds page height");
            }

            std::size_t linePosition = 0;
            while (linePosition < row.size()) {
                const unsigned char command = read_byte();
                if (command == 128) {
                    std::fill(row.begin() + static_cast<std::ptrdiff_t>(linePosition), row.end(), blankByte);
                    linePosition = row.size();
                } else if ((command & 0x80) != 0) {
                    const std::size_t literalBytes = static_cast<std::size_t>(257 - command) * unit;
                    if (literalBytes > row.size() - linePosition) {
                        throw std::runtime_error("PWG literal run exceeds row width");
                    }
                    read_exact(row.data() + linePosition, literalBytes);
                    linePosition += literalBytes;
                } else {
                    const std::size_t repeatBytes = (static_cast<std::size_t>(command) + 1) * unit;
                    if (repeatBytes > row.size() - linePosition) {
                        throw std::runtime_error("PWG repeat run exceeds row width");
                    }
                    std::array<unsigned char, 3> pixel{};
                    read_exact(pixel.data(), unit);
                    for (std::size_t offset = 0; offset < repeatBytes; ++offset) {
                        row[linePosition + offset] = pixel[offset % unit];
                    }
                    linePosition += repeatBytes;
                }
            }

            outputRow.fill('\0');
            for (uint32_t x = 0; x < width && x * scale < kPageWidth; ++x) {
                if (!isDark(row, x)) {
                    continue;
                }
                for (uint32_t subPixel = 0; subPixel < scale; ++subPixel) {
                    const uint32_t outputX = x * scale + subPixel;
                    if (outputX < kPageWidth) {
                        outputRow[outputX / 8] = static_cast<char>(
                            static_cast<unsigned char>(outputRow[outputX / 8]) | (0x80u >> (outputX % 8)));
                    }
                }
            }

            for (uint32_t copy = 0; copy < rowCopies; ++copy, ++sourceY) {
                for (uint32_t subRow = 0; subRow < scale; ++subRow) {
                    const uint64_t outputY = static_cast<uint64_t>(sourceY) * scale + subRow;
                    if (outputY < kPageHeight) {
                        std::copy(outputRow.begin(), outputRow.end(),
                                  page.raster.begin() + static_cast<std::ptrdiff_t>(outputY * kPageLineBytes));
                    }
                }
            }
        }

        ++pages_;
        std::wcout << L"PWG page " << pages_ << L": " << width << L"x" << height << L" at "
                   << dpiX << L" dpi, color space " << colorSpace << L", "
                   << bitsPerPixel << L" bpp.\n";
        return page;
    }

private:
    static constexpr unsigned kDarkThreshold = 160;

    unsigned char read_byte() {
        const int value = input_.get();
        if (value == std::char_traits<char>::eof()) {
            throw std::runtime_error("truncated PWG raster data");
        }
        return static_cast<unsigned char>(value);
    }

    void read_exact(unsigned char* destination, std::size_t size) {
        input_.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(size));
        if (input_.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error("truncated PWG raster data");
        }
    }

    std::istream& input_;
    unsigned pages_ = 0;
};

// Decodes the whole PWG file without printing; returns the page count.
static unsigned validate_pwg_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open PWG input file");
    }
    PwgPageReader reader(input);
    while (reader.next()) {
    }
    return reader.pages_read();
}

static bool convert_pwg_to_pbm(const std::wstring& inputPath, const std::wstring& outputPath) {
    std::ifstream input(std::filesystem::path(inputPath), std::ios::binary);
    if (!input) {
        std::wcerr << L"Cannot open PWG input file.\n";
        return false;
    }

    std::ofstream output(std::filesystem::path(outputPath), std::ios::binary | std::ios::trunc);
    if (!output) {
        std::wcerr << L"Cannot create PBM output file.\n";
        return false;
    }

    try {
        PwgPageReader reader(input);
        while (const auto page = reader.next()) {
            output << "P4\n" << page->width << ' ' << page->height << '\n';
            output.write(page->raster.data(), static_cast<std::streamsize>(page->raster.size()));
            if (!output) {
                throw std::runtime_error("failed to write PBM output");
            }
        }
        std::wcout << L"Converted " << reader.pages_read() << L" PWG page(s) to "
                   << kPageWidth << L"x" << kPageHeight << L" 1-bit PBM at 600 dpi.\n";
        return true;
    } catch (const std::exception& error) {
        output.close();
        std::error_code filesystemError;
        std::filesystem::remove(std::filesystem::path(outputPath), filesystemError);
        std::cerr << "Invalid PWG raster: " << error.what() << "\n";
        return false;
    }
}

#if defined(_MSC_VER)
static Capt::PageParams make_page_params(unsigned width, unsigned height) {
    return Capt::PageParams{
        .PaperSize = 0x09,
        .TonerDensity = 0x3f,
        .Mode = 0,
        .Resolution = Capt::RES_600,
        .SmoothEnable = true,
        .TonerSaving = false,
        .MarginLeft = 1,
        .MarginTop = 1,
        .ImageLineSize = static_cast<uint16_t>(width / 8),
        .ImageLines = static_cast<uint16_t>(height),
        .PaperWidth = kPageWidth,
        .PaperHeight = kPageHeight,
    };
}

using CaptPrinter = Capt::BasicCaptPrinter<std::stop_token>;

constexpr auto kReadyTimeout = std::chrono::minutes(30);
constexpr auto kPageTimeout = std::chrono::minutes(3);
constexpr auto kDeviceTimeout = std::chrono::minutes(10);

static const wchar_t* describe_not_ready(const Capt::ExtendedStatus& status) {
    if ((status.Engine & Capt::DOOR_OPEN) != 0) return L"cover is open";
    if ((status.Engine & Capt::NO_CARTRIDGE) != 0) return L"no toner cartridge";
    if ((status.Engine & Capt::NO_PRINT_PAPER) != 0) return L"out of paper";
    if ((status.Engine & Capt::JAM) != 0) return L"paper jam";
    if ((status.Engine & Capt::WAITING) != 0) return L"warming up";
    if ((status.Engine & Capt::CLEANING) != 0) return L"cleaning";
    if ((status.Engine & Capt::TEST_PRINTING) != 0) return L"printing a test page";
    if (status.ClearErrorNeeded()) return L"controller error, clearing";
    return L"not ready";
}

// Waits until the printer is ready, clearing recoverable controller errors and
// reporting each new reason once (paper, cover, jam...). Throws on timeout or service call.
static Capt::ExtendedStatus wait_until_ready(CaptPrinter& printer) {
    const auto deadline = std::chrono::steady_clock::now() + kReadyTimeout;
    const wchar_t* lastReason = nullptr;
    for (;;) {
        const Capt::ExtendedStatus status = printer.GetStatus();
        if (status.Ready()) {
            if (lastReason != nullptr) {
                std::wcout << timestamp().c_str() << L" Printer is ready again.\n";
            }
            return status;
        }
        if (status.ServiceCall()) {
            throw std::runtime_error("printer reports a service call condition");
        }
        if (status.ClearErrorNeeded()) {
            printer.ClearError(&status);
        }
        const wchar_t* reason = describe_not_ready(status);
        if (reason != lastReason) {
            std::wcout << timestamp().c_str() << L" Waiting for printer: " << reason << L" (basic=0x"
                       << std::hex << static_cast<unsigned>(status.Basic) << L", engine=0x"
                       << static_cast<unsigned>(status.Engine) << std::dec << L").\n";
            lastReason = reason;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("timed out waiting for the printer to become ready");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

static Capt::ExtendedStatus wait_print_end(CaptPrinter& printer) {
    const auto deadline = std::chrono::steady_clock::now() + kPageTimeout;
    for (;;) {
        Capt::ExtendedStatus status = printer.GetStatus();
        if (!status.IsPrinting()) {
            return status;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("timed out waiting for the printer to finish printing");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Sends one page, re-sending it (or the previous page) when the engine rejects it
// after a jam or paper-out, following the libcapt printpbm reference flow.
static void write_page(CaptPrinter& printer, Capt::Utility::BufferedPage& page,
                       Capt::Utility::BufferedPage* previous, std::size_t blockSize) {
    Capt::ReprintStatus reprint = Capt::ReprintStatus::None;
    for (unsigned attempt = 0;; ++attempt) {
        if (attempt > 20) {
            throw std::runtime_error("page was rejected too many times");
        }
        Capt::Utility::BufferedPage& current =
            (previous != nullptr && reprint == Capt::ReprintStatus::Prev) ? *previous : page;
        current.pubseekpos(0);

        const Capt::ExtendedStatus ready = wait_until_ready(printer);
        if (!ready.Online() || ready.Start != current.PageNumber) {
            if (!printer.GoOnline(current.PageNumber)) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        std::wcout << timestamp().c_str() << (reprint == Capt::ReprintStatus::None ? L" Sending page " : L" Re-sending page ")
                   << current.PageNumber + 1 << L".\n";
        if (printer.WriteVideoData(std::stop_token{}, current.Params, current, blockSize)) {
            if (previous != nullptr && reprint == Capt::ReprintStatus::Prev) {
                reprint = Capt::ReprintStatus::None;
                continue;
            }
            return;
        }

        const Capt::ExtendedStatus status = wait_print_end(printer);
        if (status.VideoDataError()) {
            throw std::runtime_error("printer reported a CAPT video-data error");
        }
        reprint = status.GetReprintStatus();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

static void wait_last_page(CaptPrinter& printer, Capt::Utility::BufferedPage& page, std::size_t blockSize) {
    for (unsigned attempt = 0; attempt <= 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const Capt::ExtendedStatus status = wait_print_end(printer);
        if (status.VideoDataError()) {
            throw std::runtime_error("printer reported a CAPT video-data error");
        }
        if (status.GetReprintStatus() == Capt::ReprintStatus::None) {
            return;
        }
        write_page(printer, page, nullptr, blockSize);
    }
    throw std::runtime_error("last page was rejected too many times");
}

static HANDLE wait_for_lbp810_usbprint(std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        HANDLE device = open_lbp810_usbprint();
        if (device != INVALID_HANDLE_VALUE || std::chrono::steady_clock::now() >= deadline) {
            return device;
        }
        std::wcout << timestamp().c_str() << L" Printer is not connected; retrying in 10 seconds.\n";
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

// Prints every page produced by nextPage as one CAPT job. Returns the number of
// pages sent, or nullopt on failure.
static std::optional<unsigned> print_pages(const PageSource& nextPage,
                                           std::chrono::steady_clock::duration deviceTimeout = {}) {
    HANDLE device = wait_for_lbp810_usbprint(deviceTimeout);
    if (device == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    UsbPrintStreambuf usbBuffer(device);
    std::iostream printerStream(&usbBuffer);
    CaptPrinter printer(printerStream);
    bool unitMayBeReserved = false;

    try {
        const Capt::PrinterInfo info = printer.GetPrinterInfo();
        if (info.Type != 1 || info.BlockSize == 0 || info.BlockSize > UINT16_MAX - 4) {
            throw std::runtime_error("CAPT response is not a supported LBP-810 printer configuration");
        }

        unitMayBeReserved = true;
        printer.ReserveUnit();
        printer.ClearError();

        unsigned pageNumber = 0;
        Capt::Utility::BufferedPage previousPage;
        while (auto page = nextPage()) {
            if (pageNumber >= UINT16_MAX) {
                throw std::runtime_error("job has too many pages");
            }
            const Capt::PageParams params = make_page_params(page->width, page->height);
            std::stringbuf raster(std::move(page->raster), std::ios::in | std::ios::binary);
            Capt::Compression::ScoaStreambuf compressed(raster, params.ImageLineSize, params.ImageLines);
            Capt::Utility::BufferedPage currentPage(pageNumber, params, &compressed);
            write_page(printer, currentPage, pageNumber == 0 ? nullptr : &previousPage, info.BlockSize);
            previousPage = std::move(currentPage);
            ++pageNumber;
        }
        if (pageNumber != 0) {
            wait_last_page(printer, previousPage, info.BlockSize);
        }

        printer.GoOffline();
        printer.ReleaseUnit();
        unitMayBeReserved = false;
        std::wcout << timestamp().c_str() << L" Printer reports " << pageNumber << L" page(s) finished.\n";
        return pageNumber;
    } catch (const std::exception& error) {
        std::cerr << timestamp() << " CAPT print failed: " << error.what() << "\n";
        if (unitMayBeReserved) {
            try {
                printer.ReleaseUnit();
            } catch (const std::exception& releaseError) {
                std::cerr << "CAPT unit release failed: " << releaseError.what() << "\n";
            }
        }
        return std::nullopt;
    }
}

static bool print_pbm_file(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) {
        std::wcerr << L"Cannot open PBM input file.\n";
        return false;
    }

    try {
        // Validate the whole file first so a damaged later page cannot leave a half-printed job.
        unsigned pages = 0;
        while (read_pbm_page(input)) {
            ++pages;
        }
        if (pages == 0) {
            throw std::runtime_error("PBM file contains no images");
        }
        input.clear();
        input.seekg(0);
    } catch (const std::exception& error) {
        std::cerr << "Invalid PBM: " << error.what() << "\n";
        return false;
    }

    return print_pages([&input] { return read_pbm_page(input); }).has_value();
}

static bool print_pwg_file(const std::filesystem::path& path,
                           std::chrono::steady_clock::duration deviceTimeout = {}) {
    try {
        const unsigned pages = validate_pwg_file(path);
        std::wcout << timestamp().c_str() << L" PWG job has " << pages << L" valid page(s).\n";
    } catch (const std::exception& error) {
        std::cerr << timestamp() << " Invalid PWG raster: " << error.what() << "\n";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::wcerr << L"Cannot reopen PWG input file.\n";
        return false;
    }
    try {
        PwgPageReader reader(input);
        return print_pages([&reader] { return reader.next(); }, deviceTimeout).has_value();
    } catch (const std::exception& error) {
        std::cerr << timestamp() << " Invalid PWG raster: " << error.what() << "\n";
        return false;
    }
}

static bool print_test_page() {
    MonoPage page;
    page.width = kPageWidth;
    page.height = kPageHeight;
    page.raster.assign(static_cast<std::size_t>(kPageLineBytes) * kPageHeight, '\0');
    const auto setBlackPixel = [&page](unsigned x, unsigned y) {
        const std::size_t offset = static_cast<std::size_t>(y) * kPageLineBytes + x / 8;
        page.raster[offset] = static_cast<char>(
            static_cast<unsigned char>(page.raster[offset]) | (0x80u >> (x % 8)));
    };

    for (unsigned position = 100; position <= 900; ++position) {
        setBlackPixel(position, 100);
        setBlackPixel(position, 900);
        setBlackPixel(100, position);
        setBlackPixel(900, position);
    }
    for (unsigned bar = 0; bar < 8; ++bar) {
        for (unsigned y = 250; y < 750; ++y) {
            for (unsigned x = 160 + bar * 90; x < 220 + bar * 90; ++x) {
                if (bar % 2 == 0 || y % 2 == 0) {
                    setBlackPixel(x, y);
                }
            }
        }
    }

    std::optional<MonoPage> pending(std::move(page));
    return print_pages([&pending] {
        std::optional<MonoPage> next = std::move(pending);
        pending.reset();
        return next;
    }).has_value();
}

// Atomically renames the spooler's capture file once the spooler has closed it.
// Returns the claimed path, or nullopt if the file is still being written.
static std::optional<std::filesystem::path> claim_capture_file(const std::filesystem::path& capturePath) {
    HANDLE capture = CreateFileW(capturePath.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (capture == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION &&
            error != ERROR_FILE_NOT_FOUND) {
            std::wcerr << timestamp().c_str() << L" Could not open capture file, Win32 error " << error << L".\n";
        }
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(capture, &size) || size.QuadPart == 0) {
        // The spooler creates the file before it writes the job; wait for data.
        CloseHandle(capture);
        return std::nullopt;
    }

    const std::filesystem::path processingPath = capturePath.parent_path() /
        (capturePath.stem().wstring() + L"-" + std::to_wstring(GetTickCount64()) + L".processing.pwg");
    const std::wstring target = processingPath.wstring();
    std::vector<BYTE> renameBuffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(renameBuffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::copy(target.begin(), target.end(), rename->FileName);
    const bool renamed = SetFileInformationByHandle(capture, FileRenameInfo, rename,
                                                    static_cast<DWORD>(renameBuffer.size())) != FALSE;
    const DWORD renameError = GetLastError();
    CloseHandle(capture);
    if (!renamed) {
        std::wcerr << timestamp().c_str() << L" Could not claim capture file, Win32 error " << renameError << L".\n";
        return std::nullopt;
    }
    return processingPath;
}

static void process_captured_job(const std::filesystem::path& processingPath) {
    std::wcout << timestamp().c_str() << L" Processing " << processingPath.filename().wstring() << L".\n";
    const bool printed = print_pwg_file(processingPath, kDeviceTimeout);
    std::error_code filesystemError;
    if (printed) {
        std::filesystem::remove(processingPath, filesystemError);
        std::wcout << timestamp().c_str() << L" PWG job converted and printed successfully.\n";
        return;
    }

    std::filesystem::path failedPath = processingPath;
    failedPath.replace_extension(L".failed.pwg");
    MoveFileExW(processingPath.c_str(), failedPath.c_str(), MOVEFILE_WRITE_THROUGH);
    std::wcerr << timestamp().c_str() << L" PWG job failed and was retained as "
               << failedPath.filename().wstring() << L".\n";
}

// Points the C++ standard streams at the log file. This does not depend on the
// process having valid standard handles, which a scheduled task may not provide.
static bool redirect_output_to_log(const std::filesystem::path& logPath) {
    static std::ofstream narrowLog;
    static std::wofstream wideLog;
    narrowLog.open(logPath, std::ios::app);
    wideLog.open(logPath, std::ios::app);
    if (!narrowLog || !wideLog) {
        return false;
    }
    // unitbuf belongs to the writing stream, so it is set on the redirected standard streams.
    std::cout.setf(std::ios::unitbuf);
    std::wcout.setf(std::ios::unitbuf);
    std::cout.rdbuf(narrowLog.rdbuf());
    std::cerr.rdbuf(narrowLog.rdbuf());
    std::wcout.rdbuf(wideLog.rdbuf());
    std::wcerr.rdbuf(wideLog.rdbuf());
    return true;
}

static bool watch_captured_jobs(bool logToFile) {
    wchar_t programData[MAX_PATH]{};
    const DWORD programDataLength = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    if (programDataLength == 0 || programDataLength >= MAX_PATH) {
        wcscpy_s(programData, L"C:\\ProgramData");
    }
    const std::filesystem::path directory = std::filesystem::path(programData) /
                                            L"CanonLBP810" / L"Jobs";
    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError) {
        std::wcerr << L"Cannot create capture directory: " << directory.wstring() << L"\n";
        return false;
    }

    const std::filesystem::path capturePath = directory / L"from-windows-app.pwg";
    const std::filesystem::path logPath = directory / L"bridge.log";
    if (logToFile && !redirect_output_to_log(logPath)) {
        // Printing still works without the log, so the bridge keeps running.
        std::wcerr << L"Cannot open bridge log: " << logPath.wstring() << L"\n";
    }

    HANDLE notification = FindFirstChangeNotificationW(directory.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (notification == INVALID_HANDLE_VALUE) {
        print_win_error(L"FindFirstChangeNotification");
        return false;
    }

    std::wcout << timestamp().c_str() << L" Canon LBP-810 PWG-to-CAPT watcher started; watching "
               << capturePath.wstring() << L".\n";

    // Jobs left over from a crash or reboot are printed first.
    std::vector<std::filesystem::path> leftovers;
    for (const auto& entry : std::filesystem::directory_iterator(directory, filesystemError)) {
        if (entry.path().filename().wstring().ends_with(L".processing.pwg")) {
            leftovers.push_back(entry.path());
        }
    }
    filesystemError.clear();
    std::sort(leftovers.begin(), leftovers.end());
    for (const auto& leftover : leftovers) {
        process_captured_job(leftover);
    }

    for (;;) {
        while (const auto claimed = claim_capture_file(capturePath)) {
            process_captured_job(*claimed);
            std::wcout.clear();
            std::wcerr.clear();
        }

        // The timeout re-checks a file whose writer was still active at the last notification.
        const DWORD waitResult = WaitForSingleObject(notification, 5000);
        if (waitResult == WAIT_TIMEOUT) {
            continue;
        }
        if (waitResult != WAIT_OBJECT_0) {
            print_win_error(L"WaitForSingleObject(print capture)");
            FindCloseChangeNotification(notification);
            return false;
        }
        if (!FindNextChangeNotification(notification)) {
            print_win_error(L"FindNextChangeNotification");
            FindCloseChangeNotification(notification);
            return false;
        }
    }
}
#else
static bool print_pbm_file(const std::wstring&) {
    std::wcerr << L"--print-pbm requires an MSVC build with the Windows SDK.\n";
    return false;
}

static bool print_pwg_file(const std::filesystem::path&) {
    std::wcerr << L"--print-pwg requires an MSVC build with the Windows SDK.\n";
    return false;
}

static bool print_test_page() {
    std::wcerr << L"--print-test requires an MSVC build with the Windows SDK.\n";
    return false;
}

static bool watch_captured_jobs(bool) {
    std::wcerr << L"--watch-capture requires an MSVC build with the Windows SDK.\n";
    return false;
}
#endif


// Sends a file that is ALREADY encoded in the printer's native CAPT format.
// This function does NOT convert PDF/BMP/PBM into CAPT and does not implement CAPT.
static bool send_preencoded_capt(const std::wstring& printer_name,
                                 const std::wstring& file_path) {
    std::ifstream input(std::filesystem::path(file_path), std::ios::binary);
    if (!input) {
        std::wcerr << L"Cannot open input file.\n";
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        std::wcerr << L"Input file is empty.\n";
        return false;
    }

    HANDLE printer = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(printer_name.c_str()), &printer, nullptr)) {
        print_win_error(L"OpenPrinter");
        return false;
    }

    DOC_INFO_1W doc{};
    doc.pDocName = const_cast<LPWSTR>(L"Canon LBP-810 CAPT raw stream (prototype)");
    doc.pOutputFile = nullptr;
    doc.pDatatype = const_cast<LPWSTR>(L"RAW");

    DWORD job = StartDocPrinterW(printer, 1, reinterpret_cast<LPBYTE>(&doc));
    if (job == 0) {
        print_win_error(L"StartDocPrinter");
        ClosePrinter(printer);
        return false;
    }

    bool ok = StartPagePrinter(printer) != FALSE;
    if (!ok) print_win_error(L"StartPagePrinter");

    DWORD written = 0;
    if (ok) {
        ok = WritePrinter(printer, bytes.data(), static_cast<DWORD>(bytes.size()), &written) != FALSE;
        if (!ok) print_win_error(L"WritePrinter");
        else if (written != bytes.size()) {
            std::wcerr << L"Short write: " << written << L" of " << bytes.size() << L" bytes.\n";
            ok = false;
        }
    }

    if (ok && !EndPagePrinter(printer)) {
        print_win_error(L"EndPagePrinter");
        ok = false;
    }
    if (!EndDocPrinter(printer)) {
        print_win_error(L"EndDocPrinter");
        ok = false;
    }
    ClosePrinter(printer);
    if (ok) std::wcout << L"Spooler accepted the byte stream. This does NOT prove the printer can interpret it.\n";
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 1 || std::wstring(argv[1]) == L"--list") {
        return list_printers() ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--usb-scan") {
        return scan_usb_printers() ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--capt-info") {
        return query_capt_printer_info() ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--print-test") {
        return print_test_page() ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--watch-capture") {
        return watch_captured_jobs(true) ? 0 : 1;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--watch-capture" && std::wstring(argv[2]) == L"--console") {
        return watch_captured_jobs(false) ? 0 : 1;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--print-pbm") {
        return print_pbm_file(argv[2]) ? 0 : 1;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--print-pwg") {
        return print_pwg_file(std::filesystem::path(argv[2])) ? 0 : 1;
    }
    if (argc == 4 && std::wstring(argv[1]) == L"--pwg-to-pbm") {
        return convert_pwg_to_pbm(argv[2], argv[3]) ? 0 : 1;
    }
    if (argc != 4 || std::wstring(argv[1]) != L"--send-preencoded-capt") {
        std::wcerr << L"Usage:\n"
                   << L"  lbp810_spool.exe --list\n"
                   << L"  lbp810_spool.exe --usb-scan\n"
                   << L"  lbp810_spool.exe --capt-info\n"
                   << L"  lbp810_spool.exe --print-test\n"
                   << L"  lbp810_spool.exe --watch-capture [--console]\n"
                   << L"  lbp810_spool.exe --print-pbm \"pages.pbm\"\n"
                   << L"  lbp810_spool.exe --print-pwg \"job.pwg\"\n"
                   << L"  lbp810_spool.exe --pwg-to-pbm \"job.pwg\" \"pages.pbm\"\n"
                   << L"  lbp810_spool.exe --send-preencoded-capt \"Printer name\" \"file.capt\"\n\n"
                   << L"WARNING: file.capt must already be a valid CAPT stream for this model.\n";
        return 2;
    }
    return send_preencoded_capt(argv[2], argv[3]) ? 0 : 1;
}
