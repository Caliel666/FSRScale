#include "reshade_detect.h"
#include <tlhelp32.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace {
bool rangeFits(size_t offset, size_t length, size_t total)
{
  return offset <= total && length <= total - offset;
}

template <typename T>
bool readObject(const std::vector<uint8_t>& bytes, size_t offset, T& value)
{
  if (!rangeFits(offset, sizeof(T), bytes.size())) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

template <typename NtHeaders>
bool hasReShadeExportsForHeaders(const std::vector<uint8_t>& bytes, size_t ntOffset)
{
  NtHeaders nt{};
  if (!readObject(bytes, ntOffset, nt)) return false;
  if (nt.Signature != IMAGE_NT_SIGNATURE) return false;

  const size_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
  if (nt.FileHeader.SizeOfOptionalHeader < sizeof(typename std::conditional<
          std::is_same<NtHeaders, IMAGE_NT_HEADERS64>::value,
          IMAGE_OPTIONAL_HEADER64, IMAGE_OPTIONAL_HEADER32>::type)) return false;

  const size_t sectionOffset = optionalOffset + nt.FileHeader.SizeOfOptionalHeader;
  const size_t sectionBytes = static_cast<size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
  if (!rangeFits(sectionOffset, sectionBytes, bytes.size())) return false;

  const auto& optional = nt.OptionalHeader;
  if (optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
  const IMAGE_DATA_DIRECTORY exports = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!exports.VirtualAddress || exports.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return false;

  auto rvaToOffset = [&](DWORD rva, size_t required) -> size_t {
    if (rva < optional.SizeOfHeaders && rangeFits(rva, required, bytes.size()))
      return static_cast<size_t>(rva);

    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
      IMAGE_SECTION_HEADER section{};
      if (!readObject(bytes, sectionOffset + static_cast<size_t>(i) * sizeof(section), section))
        return std::numeric_limits<size_t>::max();
      const uint64_t begin = section.VirtualAddress;
      const uint64_t end = begin + section.SizeOfRawData;
      if (rva >= begin && static_cast<uint64_t>(rva) + required <= end) {
        const uint64_t offset = static_cast<uint64_t>(section.PointerToRawData) + (rva - begin);
        if (offset <= std::numeric_limits<size_t>::max() &&
            rangeFits(static_cast<size_t>(offset), required, bytes.size()))
          return static_cast<size_t>(offset);
      }
    }
    return std::numeric_limits<size_t>::max();
  };

  const size_t exportOffset = rvaToOffset(exports.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
  if (exportOffset == std::numeric_limits<size_t>::max()) return false;

  IMAGE_EXPORT_DIRECTORY directory{};
  if (!readObject(bytes, exportOffset, directory)) return false;
  if (directory.NumberOfNames == 0 || directory.NumberOfNames > 65536) return false;

  const size_t namesBytes = static_cast<size_t>(directory.NumberOfNames) * sizeof(DWORD);
  const size_t namesOffset = rvaToOffset(directory.AddressOfNames, namesBytes);
  if (namesOffset == std::numeric_limits<size_t>::max()) return false;

  bool hasRegister = false;
  bool hasUnregister = false;
  for (DWORD i = 0; i < directory.NumberOfNames; ++i) {
    DWORD nameRva = 0;
    if (!readObject(bytes, namesOffset + static_cast<size_t>(i) * sizeof(DWORD), nameRva))
      return false;
    const size_t nameOffset = rvaToOffset(nameRva, 1);
    if (nameOffset == std::numeric_limits<size_t>::max()) continue;

    // Export names are NUL-terminated strings within the mapped section.
    size_t end = nameOffset;
    while (end < bytes.size() && end - nameOffset <= 256 && bytes[end] != 0) ++end;
    if (end == bytes.size() || end - nameOffset > 256) continue;
    const std::string name(reinterpret_cast<const char*>(bytes.data() + nameOffset), end - nameOffset);
    if (name == "ReShadeRegisterAddon") hasRegister = true;
    if (name == "ReShadeUnregisterAddon") hasUnregister = true;
    if (hasRegister && hasUnregister) return true;
  }
  return false;
}

bool hasReShadeExports(const std::wstring& path)
{
  // Read the PE file as data. Do not LoadLibraryEx the target's modules into
  // NRLive: even DONT_RESOLVE_DLL_REFERENCES maps an arbitrary graphics DLL
  // into this process and can interfere with proxy DLLs / DXGI initialization.
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return false;
  const std::streamoff length = file.tellg();
  if (length < static_cast<std::streamoff>(sizeof(IMAGE_DOS_HEADER)) ||
      length > 512LL * 1024 * 1024) return false;

  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  file.seekg(0, std::ios::beg);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) return false;

  IMAGE_DOS_HEADER dos{};
  if (!readObject(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
    return false;
  const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
  DWORD signature = 0;
  if (!readObject(bytes, ntOffset, signature) || signature != IMAGE_NT_SIGNATURE) return false;

  IMAGE_FILE_HEADER fileHeader{};
  if (!readObject(bytes, ntOffset + sizeof(DWORD), fileHeader)) return false;
  WORD magic = 0;
  if (!readObject(bytes, ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER), magic))
    return false;

  if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    return hasReShadeExportsForHeaders<IMAGE_NT_HEADERS64>(bytes, ntOffset);
  if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    return hasReShadeExportsForHeaders<IMAGE_NT_HEADERS32>(bytes, ntOffset);
  return false;
}
}

ReShadeDetection detectReShadeInProcess(DWORD processId)
{
  ReShadeDetection result;
  if (!processId) return result;

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                             processId);
  if (snapshot == INVALID_HANDLE_VALUE) return result;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      if (hasReShadeExports(entry.szExePath)) {
        result.hooked = true;
        result.modulePath = entry.szExePath;
        break;
      }
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}
