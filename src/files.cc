// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// File information by handle. kernel32, Vista (ReOpenFile, Server 2003).
//
// These four are the first functions here that are genuinely implementable
// rather than merely approximable, because every one of them is a thin Win32
// shell over an NT call that XP already has. The capability was always
// present; Vista only added the public way in.
//
// So this file breaks winwrap's avoidance of ntdll. That avoidance was never
// absolute -- it was about keyed events specifically, which are undocumented
// *and* carry an XP hang bug. NtQueryInformationFile and its neighbours are
// documented in the WDK, unchanged since NT 3.1, and are what kernel32 itself
// calls one frame further down. Resolved through GetProcAddress rather than an
// import library, so winwrap still links against nothing but kernel32 and a
// consumer inherits no new dependency.
//
// Every information class that can be served from XP is served, not only the
// ones Electron happens to reach: the pass-through classes, directory
// enumeration through NtQueryDirectoryFile, the rename class with its DOS to
// NT path translation, and FileIdInfo assembled from the volume serial number
// and the NTFS index. A class that genuinely has no XP answer fails with
// ERROR_INVALID_PARAMETER, which is what Windows itself returns for a class
// the running version does not know.

// winwrap.h first, deliberately, against the usual order: it brings in
// windows.h, and winternl.h pulls winnt.h without the target-architecture
// macros windows.h establishes, so the other way round fails to compile with
// "No Target Architecture".
#include "winwrap.h"

namespace {

  // NT information classes. winternl.h declares the enum but only names one
  // member of it, so the values used here are spelled out.
  constexpr ULONG kFileBasicInformation        = 4;
  constexpr ULONG kFileStandardInformation     = 5;
  constexpr ULONG kFileNameInformation         = 9;
  constexpr ULONG kFileDispositionInformation  = 13;
  constexpr ULONG kFileEndOfFileInformation    = 20;
  constexpr ULONG kFileAllocationInformation   = 19;
  constexpr ULONG kFileAttributeTagInformation = 35;

  constexpr ULONG kFileStreamInformation          = 22;
  constexpr ULONG kFileCompressionInformation     = 28;
  constexpr ULONG kFileInternalInformation        = 6;
  constexpr ULONG kFileIdBothDirectoryInformation = 37;
  constexpr ULONG kFileFullDirectoryInformation   = 2;
  constexpr ULONG kFileIoPriorityHintInformation  = 43;
  constexpr ULONG kFileRemoteProtocolInformation  = 55;
  constexpr ULONG kFileRenameInformation          = 10;

  constexpr ULONG kObjectNameInformation   = 1;
  constexpr ULONG kFileFsVolumeInformation = 1;

  // FILE_INFO_BY_HANDLE_CLASS and every structure it selects are gated on
  // NTDDI_LONGHORN, so none of them exist at the level winwrap compiles for.
  // The values are the enum's declaration order in minwinbase.h.
  enum Win32FileInfoClass {
    kBasicInfo                  = 0,
    kStandardInfo               = 1,
    kNameInfo                   = 2,
    kRenameInfo                 = 3,
    kDispositionInfo            = 4,
    kAllocationInfo             = 5,
    kEndOfFileInfo              = 6,
    kStreamInfo                 = 7,
    kCompressionInfo            = 8,
    kAttributeTagInfo           = 9,
    kIdBothDirectoryInfo        = 10,
    kIdBothDirectoryRestartInfo = 11,
    kIoPriorityHintInfo         = 12,
    kRemoteProtocolInfo         = 13,
    kFullDirectoryInfo          = 14,
    kFullDirectoryRestartInfo   = 15,
    kIdInfo                     = 18,
  };

  // Only the two structures whose contents are actually touched need declaring;
  // everything else is handed to NT unexamined.
  struct Win32FileRenameInfo {
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
  };

  struct Win32FileIdInfo {
    ULONGLONG VolumeSerialNumber;
    BYTE FileId[16];
  };

  constexpr NTSTATUS kStatusSuccess            = 0;
  constexpr NTSTATUS kStatusBufferOverflow     = 0x80000005L;
  constexpr NTSTATUS kStatusBufferTooSmall     = 0xC0000023L;
  constexpr NTSTATUS kStatusInfoLengthMismatch = 0xC0000004L;

  // NtCreateFile CreateDisposition and CreateOptions.
  constexpr ULONG kFileOpen                    = 1;
  constexpr ULONG kFileWriteThrough            = 0x00000002;
  constexpr ULONG kFileSequentialOnly          = 0x00000004;
  constexpr ULONG kFileNoIntermediateBuffering = 0x00000008;
  constexpr ULONG kFileSynchronousIoNonalert   = 0x00000020;
  constexpr ULONG kFileNonDirectoryFile        = 0x00000040;
  constexpr ULONG kFileRandomAccess            = 0x00000800;
  constexpr ULONG kFileDeleteOnClose           = 0x00001000;
  constexpr ULONG kFileOpenForBackupIntent     = 0x00004000;
  constexpr ULONG kFileOpenReparsePoint        = 0x00200000;
  constexpr ULONG kFileOpenNoRecall            = 0x00400000;

#if !defined(VOLUME_NAME_DOS)
 #define VOLUME_NAME_DOS  0x0
 #define VOLUME_NAME_GUID 0x1
 #define VOLUME_NAME_NT   0x2
 #define VOLUME_NAME_NONE 0x4
#endif
#if !defined(FILE_NAME_NORMALIZED)
 #define FILE_NAME_NORMALIZED 0x0
 #define FILE_NAME_OPENED     0x8
#endif

  using NtQueryInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  using NtSetInformationFileFn   = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  using NtQueryObjectFn          = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  using NtCreateFileFn           = NTSTATUS(NTAPI*)(PHANDLE,
                                                    ACCESS_MASK,
                                                    POBJECT_ATTRIBUTES,
                                                    PIO_STATUS_BLOCK,
                                                    PLARGE_INTEGER,
                                                    ULONG,
                                                    ULONG,
                                                    ULONG,
                                                    ULONG,
                                                    PVOID,
                                                    ULONG);
  using NtQueryDirectoryFileFn   = NTSTATUS(NTAPI*)(HANDLE,
                                                    HANDLE,
                                                    PVOID,
                                                    PVOID,
                                                    PIO_STATUS_BLOCK,
                                                    PVOID,
                                                    ULONG,
                                                    ULONG,
                                                    BOOLEAN,
                                                    PUNICODE_STRING,
                                                    BOOLEAN);
  using NtQueryVolumeInformationFileFn =
      NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  using RtlDosPathNameToNtPathNameFn = BOOLEAN(NTAPI*)(PCWSTR, PUNICODE_STRING, PCWSTR*, PVOID);
  using RtlFreeUnicodeStringFn       = VOID(NTAPI*)(PUNICODE_STRING);
  using RtlNtStatusToDosErrorFn      = ULONG(NTAPI*)(NTSTATUS);

  constexpr NTSTATUS kStatusPending = 0x00000103L;

  FARPROC NtdllProc(const char* name) {
    // ntdll is mapped into every process before anything of ours runs, so a
    // module handle is always enough and nothing is ever loaded here.
    static const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll ? ::GetProcAddress(ntdll, name) : nullptr;
  }

  // Turns an NT status into the Win32 error a caller expects to read back from
  // GetLastError, which is what kernel32 does at this boundary.
  void SetLastErrorFromStatus(NTSTATUS status) {
    static const auto translate =
        reinterpret_cast<RtlNtStatusToDosErrorFn>(NtdllProc("RtlNtStatusToDosError"));
    ::SetLastError(translate ? translate(status) : ERROR_GEN_FAILURE);
  }

  // Maps a Win32 class onto its NT equivalent, for the classes whose Win32 and
  // NT structures are laid out identically -- which is why a pass-through works
  // at all. Buffer sizes are deliberately not checked here: NtQueryInformationFile
  // validates them and returns STATUS_INFO_LENGTH_MISMATCH, which
  // RtlNtStatusToDosError turns into the same ERROR_BAD_LENGTH kernel32 reports.
  bool NtClassForQuery(int win32_class, ULONG* nt_class) {
    switch (win32_class) {
      case kBasicInfo:
        *nt_class = kFileBasicInformation;
        return true;
      case kStandardInfo:
        *nt_class = kFileStandardInformation;
        return true;
      case kNameInfo:
        *nt_class = kFileNameInformation;
        return true;
      case kStreamInfo:
        *nt_class = kFileStreamInformation;
        return true;
      case kCompressionInfo:
        *nt_class = kFileCompressionInformation;
        return true;
      case kAttributeTagInfo:
        *nt_class = kFileAttributeTagInformation;
        return true;
      // Vista and Windows 7 kernels respectively. On XP the I/O manager rejects
      // the class and that becomes ERROR_INVALID_PARAMETER, which is the honest
      // answer: the information does not exist to be reported.
      case kIoPriorityHintInfo:
        *nt_class = kFileIoPriorityHintInformation;
        return true;
      case kRemoteProtocolInfo:
        *nt_class = kFileRemoteProtocolInformation;
        return true;
      default:
        return false;
    }
  }

  // Directory enumeration goes through a different NT call, and the "Restart"
  // variants differ only in rewinding the scan first.
  bool NtClassForDirectory(int win32_class, ULONG* nt_class, BOOLEAN* restart) {
    switch (win32_class) {
      case kIdBothDirectoryRestartInfo:
        *restart  = TRUE;
        *nt_class = kFileIdBothDirectoryInformation;
        return true;
      case kIdBothDirectoryInfo:
        *restart  = FALSE;
        *nt_class = kFileIdBothDirectoryInformation;
        return true;
      case kFullDirectoryRestartInfo:
        *restart  = TRUE;
        *nt_class = kFileFullDirectoryInformation;
        return true;
      case kFullDirectoryInfo:
        *restart  = FALSE;
        *nt_class = kFileFullDirectoryInformation;
        return true;
      default:
        return false;
    }
  }

  bool NtClassForSet(int win32_class, ULONG* nt_class) {
    switch (win32_class) {
      case kBasicInfo:
        *nt_class = kFileBasicInformation;
        return true;
      case kDispositionInfo:
        *nt_class = kFileDispositionInformation;
        return true;
      case kAllocationInfo:
        *nt_class = kFileAllocationInformation;
        return true;
      case kEndOfFileInfo:
        *nt_class = kFileEndOfFileInformation;
        return true;
      case kIoPriorityHintInfo:
        *nt_class = kFileIoPriorityHintInformation;
        return true;
      default:
        return false;
    }
  }

  struct NameInformation {
    UNICODE_STRING Name;
    WCHAR NameBuffer[1];
  };

  // The object's NT path, e.g. \Device\HarddiskVolume2\Windows\notepad.exe.
  // Returns a heap block the caller frees, or null on failure (last error set).
  NameInformation* QueryNtName(HANDLE file) {
    static const auto query = reinterpret_cast<NtQueryObjectFn>(NtdllProc("NtQueryObject"));
    if (!query) {
      ::SetLastError(ERROR_INVALID_FUNCTION);
      return nullptr;
    }

    ULONG size = sizeof(NameInformation) + MAX_PATH * sizeof(WCHAR);
    for (int attempt = 0; attempt < 3; ++attempt) {
      auto* buffer = static_cast<NameInformation*>(::HeapAlloc(::GetProcessHeap(), 0, size));
      if (!buffer) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
      }

      ULONG needed    = 0;
      NTSTATUS status = query(file, kObjectNameInformation, buffer, size, &needed);
      if (status == kStatusSuccess) {
        return buffer;
      }

      ::HeapFree(::GetProcessHeap(), 0, buffer);
      if (status != kStatusBufferOverflow && status != kStatusBufferTooSmall &&
          status != kStatusInfoLengthMismatch) {
        SetLastErrorFromStatus(status);
        return nullptr;
      }
      // Ask again with what it said it wanted, plus room for growth: the name
      // can change between the two calls.
      size = needed + 2 * sizeof(WCHAR);
    }

    ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return nullptr;
  }

  // Replaces a \Device\... prefix with its drive letter. Returns the number of
  // characters of `nt_path` that the device prefix occupied, or 0 if no drive
  // maps to it. QueryDosDeviceW is the inverse mapping and is on XP.
  size_t DriveForNtPath(const WCHAR* nt_path, WCHAR* drive_out) {
    WCHAR device[MAX_PATH];
    for (WCHAR letter = L'A'; letter <= L'Z'; ++letter) {
      const WCHAR dos_name[3] = {letter, L':', L'\0'};
      if (::QueryDosDeviceW(dos_name, device, ARRAYSIZE(device)) == 0) {
        continue;
      }
      size_t length = ::wcslen(device);
      if (length == 0 || ::wcsncmp(nt_path, device, length) != 0) {
        continue;
      }
      // A prefix match must land on a separator, or \Device\HarddiskVolume1
      // would also claim \Device\HarddiskVolume10.
      if (nt_path[length] != L'\\' && nt_path[length] != L'\0') {
        continue;
      }
      drive_out[0] = letter;
      drive_out[1] = L':';
      drive_out[2] = L'\0';
      return length;
    }
    return 0;
  }

  // Copies `text` out under GetFinalPathNameByHandleW's return convention: on
  // success the length without the terminator, and when it does not fit, the
  // length *with* it. The asymmetry is the documented behaviour, and callers
  // rely on it -- libuv sizes its buffer from the first answer and passes it
  // back as the second call's capacity.
  DWORD EmitPath(const WCHAR* text, size_t length, LPWSTR out, DWORD capacity) {
    if (out == nullptr || capacity < length + 1) {
      return static_cast<DWORD>(length + 1);
    }
    for (size_t i = 0; i < length; ++i) {
      out[i] = text[i];
    }
    out[length] = L'\0';
    return static_cast<DWORD>(length);
  }

} // namespace

extern "C" {

BOOL WINAPI winwrap_GetFileInformationByHandleEx(HANDLE hFile,
                                                 int FileInformationClass,
                                                 LPVOID lpFileInformation,
                                                 DWORD dwBufferSize) {
  if (!lpFileInformation) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  IO_STATUS_BLOCK iosb = {};
  ULONG nt_class       = 0;

  if (NtClassForQuery(FileInformationClass, &nt_class)) {
    static const auto query =
        reinterpret_cast<NtQueryInformationFileFn>(NtdllProc("NtQueryInformationFile"));
    if (!query) {
      ::SetLastError(ERROR_INVALID_FUNCTION);
      return FALSE;
    }
    NTSTATUS status = query(hFile, &iosb, lpFileInformation, dwBufferSize, nt_class);
    if (status != kStatusSuccess) {
      SetLastErrorFromStatus(status);
      return FALSE;
    }
    return TRUE;
  }

  BOOLEAN restart = FALSE;
  if (NtClassForDirectory(FileInformationClass, &nt_class, &restart)) {
    static const auto enumerate =
        reinterpret_cast<NtQueryDirectoryFileFn>(NtdllProc("NtQueryDirectoryFile"));
    if (!enumerate) {
      ::SetLastError(ERROR_INVALID_FUNCTION);
      return FALSE;
    }
    NTSTATUS status = enumerate(hFile, nullptr, nullptr, nullptr, &iosb, lpFileInformation,
                                dwBufferSize, nt_class, FALSE, nullptr, restart);
    if (status == kStatusPending) {
      // An asynchronous handle reports completion through the handle itself
      // rather than through the return value.
      ::WaitForSingleObject(hFile, INFINITE);
      status = iosb.Status;
    }
    if (status != kStatusSuccess) {
      // STATUS_NO_MORE_FILES lands here and becomes ERROR_NO_MORE_FILES, which
      // is how a caller learns the enumeration is finished.
      SetLastErrorFromStatus(status);
      return FALSE;
    }
    return TRUE;
  }

  if (FileInformationClass == kIdInfo) {
    // Windows 8 added this one, and it is assembled rather than fetched: the
    // 128-bit file ID it reports is, on a pre-ReFS volume, the 64-bit NTFS
    // index number zero-extended, paired with the volume serial number.
    if (dwBufferSize < sizeof(Win32FileIdInfo)) {
      ::SetLastError(ERROR_BAD_LENGTH);
      return FALSE;
    }
    static const auto query =
        reinterpret_cast<NtQueryInformationFileFn>(NtdllProc("NtQueryInformationFile"));
    static const auto query_volume =
        reinterpret_cast<NtQueryVolumeInformationFileFn>(NtdllProc("NtQueryVolumeInformationFile"));
    if (!query || !query_volume) {
      ::SetLastError(ERROR_INVALID_FUNCTION);
      return FALSE;
    }

    // FILE_FS_VOLUME_INFORMATION, declared inline: only the serial number is
    // wanted and the label that follows it is variable-length.
    struct {
      LARGE_INTEGER VolumeCreationTime;
      ULONG VolumeSerialNumber;
      ULONG VolumeLabelLength;
      BOOLEAN SupportsObjects;
      WCHAR VolumeLabel[MAX_PATH];
    } volume        = {};
    NTSTATUS status = query_volume(hFile, &iosb, &volume, sizeof(volume), kFileFsVolumeInformation);
    // A truncated label is fine; the serial number comes first.
    if (status != kStatusSuccess && status != kStatusBufferOverflow) {
      SetLastErrorFromStatus(status);
      return FALSE;
    }

    LARGE_INTEGER index = {};
    status              = query(hFile, &iosb, &index, sizeof(index), kFileInternalInformation);
    if (status != kStatusSuccess) {
      SetLastErrorFromStatus(status);
      return FALSE;
    }

    auto* out               = static_cast<Win32FileIdInfo*>(lpFileInformation);
    out->VolumeSerialNumber = volume.VolumeSerialNumber;
    for (size_t i = 0; i < sizeof(out->FileId); ++i) {
      out->FileId[i] = 0;
    }
    for (size_t i = 0; i < sizeof(index); ++i) {
      out->FileId[i] = reinterpret_cast<const BYTE*>(&index)[i];
    }
    return TRUE;
  }

  ::SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}

BOOL WINAPI winwrap_SetFileInformationByHandle(HANDLE hFile,
                                               int FileInformationClass,
                                               LPVOID lpFileInformation,
                                               DWORD dwBufferSize) {
  if (!lpFileInformation) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  static const auto set =
      reinterpret_cast<NtSetInformationFileFn>(NtdllProc("NtSetInformationFile"));
  if (!set) {
    ::SetLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
  }

  IO_STATUS_BLOCK iosb = {};

  if (FileInformationClass == kRenameInfo) {
    // The one class that is not a pass-through. The two structures match field
    // for field, but the Win32 form carries a DOS path and NT wants an NT
    // path, so the name has to be translated and the structure rebuilt around
    // it. A relative rename -- RootDirectory set -- needs no translation,
    // because the name is already relative to a handle.
    if (dwBufferSize < sizeof(Win32FileRenameInfo)) {
      ::SetLastError(ERROR_BAD_LENGTH);
      return FALSE;
    }
    auto* rename = static_cast<Win32FileRenameInfo*>(lpFileInformation);

    if (rename->RootDirectory != nullptr) {
      NTSTATUS status = set(hFile, &iosb, lpFileInformation, dwBufferSize, kFileRenameInformation);
      if (status != kStatusSuccess) {
        SetLastErrorFromStatus(status);
        return FALSE;
      }
      return TRUE;
    }

    static const auto to_nt_path =
        reinterpret_cast<RtlDosPathNameToNtPathNameFn>(NtdllProc("RtlDosPathNameToNtPathName_U"));
    static const auto free_string =
        reinterpret_cast<RtlFreeUnicodeStringFn>(NtdllProc("RtlFreeUnicodeString"));
    if (!to_nt_path || !free_string) {
      ::SetLastError(ERROR_INVALID_FUNCTION);
      return FALSE;
    }

    // FileName is counted, not terminated, and the translator wants a C
    // string.
    size_t name_chars = rename->FileNameLength / sizeof(WCHAR);
    auto* dos_name =
        static_cast<WCHAR*>(::HeapAlloc(::GetProcessHeap(), 0, (name_chars + 1) * sizeof(WCHAR)));
    if (!dos_name) {
      ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return FALSE;
    }
    for (size_t i = 0; i < name_chars; ++i) {
      dos_name[i] = rename->FileName[i];
    }
    dos_name[name_chars] = L'\0';

    UNICODE_STRING nt_name = {};
    BOOLEAN converted      = to_nt_path(dos_name, &nt_name, nullptr, nullptr);
    ::HeapFree(::GetProcessHeap(), 0, dos_name);
    if (!converted) {
      ::SetLastError(ERROR_INVALID_NAME);
      return FALSE;
    }

    DWORD rebuilt_size = static_cast<DWORD>(sizeof(Win32FileRenameInfo) + nt_name.Length);
    auto* rebuilt      = static_cast<Win32FileRenameInfo*>(
        ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, rebuilt_size));
    if (!rebuilt) {
      free_string(&nt_name);
      ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return FALSE;
    }
    rebuilt->ReplaceIfExists = rename->ReplaceIfExists;
    rebuilt->RootDirectory   = nullptr;
    rebuilt->FileNameLength  = nt_name.Length;
    for (size_t i = 0; i < nt_name.Length / sizeof(WCHAR); ++i) {
      rebuilt->FileName[i] = nt_name.Buffer[i];
    }

    NTSTATUS status = set(hFile, &iosb, rebuilt, rebuilt_size, kFileRenameInformation);
    ::HeapFree(::GetProcessHeap(), 0, rebuilt);
    free_string(&nt_name);
    if (status != kStatusSuccess) {
      SetLastErrorFromStatus(status);
      return FALSE;
    }
    return TRUE;
  }

  ULONG nt_class = 0;
  if (!NtClassForSet(FileInformationClass, &nt_class)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  NTSTATUS status = set(hFile, &iosb, lpFileInformation, dwBufferSize, nt_class);
  if (status != kStatusSuccess) {
    SetLastErrorFromStatus(status);
    return FALSE;
  }
  return TRUE;
}

DWORD WINAPI winwrap_GetFinalPathNameByHandleW(HANDLE hFile,
                                               LPWSTR lpszFilePath,
                                               DWORD cchFilePath,
                                               DWORD dwFlags) {
  if (hFile == INVALID_HANDLE_VALUE) {
    ::SetLastError(ERROR_INVALID_HANDLE);
    return 0;
  }

  // FILE_NAME_NORMALIZED is zero and FILE_NAME_OPENED is ignored: what comes
  // back from the object manager is the opened name, which for the paths these
  // callers deal in is the normalised one too.
  const DWORD volume_form =
      dwFlags & (VOLUME_NAME_DOS | VOLUME_NAME_GUID | VOLUME_NAME_NT | VOLUME_NAME_NONE);
  if (volume_form != VOLUME_NAME_DOS && volume_form != VOLUME_NAME_NT) {
    // VOLUME_NAME_GUID and VOLUME_NAME_NONE are valid requests that are simply
    // not built here; no caller in Electron asks for either. Saying
    // "unsupported" keeps that distinct from rejecting a malformed argument.
    ::SetLastError(volume_form == VOLUME_NAME_GUID || volume_form == VOLUME_NAME_NONE
                       ? ERROR_NOT_SUPPORTED
                       : ERROR_INVALID_PARAMETER);
    return 0;
  }

  NameInformation* info = QueryNtName(hFile);
  if (!info) {
    return 0;
  }

  const WCHAR* nt_path = info->Name.Buffer;
  size_t nt_length     = info->Name.Length / sizeof(WCHAR);
  DWORD result         = 0;

  if (volume_form == VOLUME_NAME_NT) {
    result = EmitPath(nt_path, nt_length, lpszFilePath, cchFilePath);
  } else {
    WCHAR drive[3]       = {};
    size_t device_length = DriveForNtPath(nt_path, drive);

    // \Device\Mup and \Device\LanmanRedirector are the network redirectors;
    // their paths become \\?\UNC\server\share, which is what the DOS form of a
    // UNC path looks like.
    static const WCHAR kMup[]    = L"\\Device\\Mup";
    static const WCHAR kLanman[] = L"\\Device\\LanmanRedirector";
    size_t unc_length            = 0;
    if (::wcsncmp(nt_path, kMup, ARRAYSIZE(kMup) - 1) == 0) {
      unc_length = ARRAYSIZE(kMup) - 1;
    } else if (::wcsncmp(nt_path, kLanman, ARRAYSIZE(kLanman) - 1) == 0) {
      unc_length = ARRAYSIZE(kLanman) - 1;
    }

    WCHAR* assembled        = nullptr;
    size_t assembled_length = 0;
    const WCHAR* prefix     = nullptr;
    size_t prefix_length    = 0;
    const WCHAR* remainder  = nullptr;
    size_t remainder_length = 0;

    if (device_length != 0) {
      prefix           = L"\\\\?\\";
      prefix_length    = 4;
      remainder        = nt_path + device_length;
      remainder_length = nt_length - device_length;
    } else if (unc_length != 0) {
      prefix           = L"\\\\?\\UNC";
      prefix_length    = 7;
      remainder        = nt_path + unc_length;
      remainder_length = nt_length - unc_length;
    } else {
      ::HeapFree(::GetProcessHeap(), 0, info);
      // A handle to something with no DOS name at all -- a pipe, a socket, a
      // volume that is not mounted to a letter.
      ::SetLastError(ERROR_PATH_NOT_FOUND);
      return 0;
    }

    assembled_length = prefix_length + (device_length ? 2 : 0) + remainder_length;
    assembled        = static_cast<WCHAR*>(
        ::HeapAlloc(::GetProcessHeap(), 0, (assembled_length + 1) * sizeof(WCHAR)));
    if (!assembled) {
      ::HeapFree(::GetProcessHeap(), 0, info);
      ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return 0;
    }

    size_t at = 0;
    for (size_t i = 0; i < prefix_length; ++i) {
      assembled[at++] = prefix[i];
    }
    if (device_length) {
      assembled[at++] = drive[0];
      assembled[at++] = drive[1];
    }
    for (size_t i = 0; i < remainder_length; ++i) {
      assembled[at++] = remainder[i];
    }
    assembled[at] = L'\0';

    result = EmitPath(assembled, assembled_length, lpszFilePath, cchFilePath);
    ::HeapFree(::GetProcessHeap(), 0, assembled);
  }

  ::HeapFree(::GetProcessHeap(), 0, info);
  return result;
}

HANDLE WINAPI winwrap_ReOpenFile(HANDLE hOriginalFile,
                                 DWORD dwDesiredAccess,
                                 DWORD dwShareMode,
                                 DWORD dwFlagsAndAttributes) {
  static const auto create = reinterpret_cast<NtCreateFileFn>(NtdllProc("NtCreateFile"));
  if (!create) {
    ::SetLastError(ERROR_INVALID_FUNCTION);
    return INVALID_HANDLE_VALUE;
  }

  // Opening a zero-length name relative to an existing handle is how NT says
  // "this same file again", which is all ReOpenFile is.
  UNICODE_STRING empty         = {0, 0, nullptr};
  OBJECT_ATTRIBUTES attributes = {};
  attributes.Length            = sizeof(attributes);
  attributes.RootDirectory     = hOriginalFile;
  attributes.ObjectName        = &empty;
  attributes.Attributes        = OBJ_CASE_INSENSITIVE;

  ULONG options = 0;
  if (dwFlagsAndAttributes & FILE_FLAG_WRITE_THROUGH) {
    options |= kFileWriteThrough;
  }
  if (dwFlagsAndAttributes & FILE_FLAG_NO_BUFFERING) {
    options |= kFileNoIntermediateBuffering;
  }
  if (dwFlagsAndAttributes & FILE_FLAG_SEQUENTIAL_SCAN) {
    options |= kFileSequentialOnly;
  }
  if (dwFlagsAndAttributes & FILE_FLAG_RANDOM_ACCESS) {
    options |= kFileRandomAccess;
  }
  if (dwFlagsAndAttributes & FILE_FLAG_OPEN_REPARSE_POINT) {
    options |= kFileOpenReparsePoint;
  }
  if (dwFlagsAndAttributes & FILE_FLAG_OPEN_NO_RECALL) {
    options |= kFileOpenNoRecall;
  }
  if (dwFlagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) {
    options |= kFileDeleteOnClose;
    dwDesiredAccess |= DELETE;
  }

  if (dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) {
    // Backup semantics is how a caller says "this might be a directory", so
    // constraining the open to non-directories here would defeat the only
    // reason to pass the flag. libuv relies on exactly this to reopen a
    // directory handle for FILE_WRITE_ATTRIBUTES.
    options |= kFileOpenForBackupIntent;
  } else {
    options |= kFileNonDirectoryFile;
  }

  if (!(dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED)) {
    // A handle without FILE_FLAG_OVERLAPPED is synchronous, and NT wants that
    // said explicitly along with the SYNCHRONIZE right that makes it possible.
    options |= kFileSynchronousIoNonalert;
    dwDesiredAccess |= SYNCHRONIZE;
  }

  HANDLE reopened      = INVALID_HANDLE_VALUE;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = create(&reopened, dwDesiredAccess, &attributes, &iosb, nullptr, 0, dwShareMode,
                           kFileOpen, options, nullptr, 0);
  if (status != kStatusSuccess) {
    SetLastErrorFromStatus(status);
    return INVALID_HANDLE_VALUE;
  }
  return reopened;
}

} // extern "C"
