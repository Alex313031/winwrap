// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Everything winwrap substitutes from shell32, in one file because the DLL is
// what they have in common: both families are *delay*-imported by their
// consumers rather than statically imported, and each section below says what
// follows from that for it.
//
//   - The Application User Model pair. Windows 7.
//   - SHGetKnownFolderPath. Vista.

#include "dispatch.h"
#include "winwrap.h"

// ---------------------------------------------------------------------------
// Application User Model IDs
// ---------------------------------------------------------------------------
//
// shell32, Windows 7 and later. A family rather than two functions, so the
// rest of it has somewhere to go: SHGetPropertyStoreForWindow and the
// relaunch-command properties would belong here too if they are ever wanted.
//
// An AppUserModelID identifies a process to the Windows 7 taskbar so that its
// windows group under a single button and a jump list can be attached to them.
// Vista's taskbar has neither grouping nor jump lists, so there is no state to
// keep and nothing to report. These are not partial implementations; they are
// the whole of the concept as it exists before Windows 7.
//
// Unlike the rest of winwrap, these two are *delay*-imported, so an absent
// export is not fatal at load. It surfaces on the first call, where Chromium's
// delay-load failure hook ends in NOTREACHED() -- an int3, reported by Windows
// as exception code 0x80000003 and nothing else. static_redirects.cc keeps
// the call from ever reaching shell32, and it works for a delay import for
// the same reason it works for a static one: the reference is satisfied
// here, so shell32.lib's member is never extracted and no delay thunk is
// generated for it. winwrap.dll.lib is a link input, which the toolchain lists
// ahead of `libs`, so it is reached before the explicit shell32.lib.
//
// That has a consequence these two used to get wrong. With no delay thunk,
// nothing loads shell32 when one of them is called; they probed for it with
// GetModuleHandleW, as the rest of winwrap does for its DLLs, and a process
// that had not yet loaded shell32 for some other reason was told the system
// had no such function. On Windows 7 and later that meant the fallback
// answered S_OK and the real call -- the one that makes the taskbar group the
// process's windows -- was never made, for the life of the process, since
// the answer is cached. Now they load shell32 as the thunk would have, by the
// same route SHGetKnownFolderPath below takes; see dispatch.h.

namespace {

  std::atomic<void*> g_shell32{nullptr};
  std::atomic<void*> g_set_app_id{nullptr};
  std::atomic<void*> g_get_app_id{nullptr};

  HMODULE Shell32() {
    return ::winwrap::LoadSystemLibrary(g_shell32, L"shell32.dll");
  }

} // namespace

extern "C" {

HRESULT WINAPI ww_SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          reinterpret_cast<decltype(&ww_SetCurrentProcessExplicitAppUserModelID)>(
              ::winwrap::ResolveFrom(g_set_app_id, Shell32(),
                                     "SetCurrentProcessExplicitAppUserModelID"))) {
    return system_fn(AppID);
  }
  // Reject a null ID the way the real function does, rather than reporting
  // success for a call that could not have meant anything. There is no ID to
  // store here, so the argument is otherwise unused -- but a shim that answers
  // S_OK to everything teaches a caller that its input was accepted, and the
  // first place that matters is a caller being ported to this platform.
  if (!AppID) {
    return E_INVALIDARG;
  }

  // The documented constraints on a valid ID -- at most 128 characters, no
  // spaces -- are deliberately not enforced. Windows rejects those, so a
  // caller that trips them is already handling a failure on Win7 and later,
  // and accepting one here costs nothing because nothing is kept. Enforcing
  // them would only add a way for this to disagree with the real function in
  // the opposite, harder-to-notice direction.
  return S_OK;
}

HRESULT WINAPI ww_GetCurrentProcessExplicitAppUserModelID(PWSTR* AppID) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          reinterpret_cast<decltype(&ww_GetCurrentProcessExplicitAppUserModelID)>(
              ::winwrap::ResolveFrom(g_get_app_id, Shell32(),
                                     "GetCurrentProcessExplicitAppUserModelID"))) {
    return system_fn(AppID);
  }
  // E_FAIL is what Windows itself returns for a process that never called Set,
  // so every caller already handles it -- Electron's GetRawAppUserModelID()
  // takes that branch and derives an ID from the application name instead
  // (electron/shell/common/application_info_win.cc).
  //
  // Clear the out parameter regardless of what the caller passed in. That same
  // Electron function declares an uninitialised PWSTR and hands it to
  // CoTaskMemFree() unconditionally, after the branch where this call failed,
  // which is only safe if the pointer is null.
  if (AppID) {
    *AppID = nullptr;
  }
  return E_FAIL;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Known folders
// ---------------------------------------------------------------------------
//
// SHGetKnownFolderPath. shell32, Vista.
//
// Vista replaced the CSIDL numbers with KNOWNFOLDERID GUIDs and kept the old
// folders, so for every folder XP has, the GUID names something
// SHGetFolderPathW can already find. The shim is that mapping, the flag
// translation, and the one real difference in contract: the new function
// allocates the string with CoTaskMemAlloc for the caller to free, where the
// old one fills a MAX_PATH buffer.
//
// Folders Vista invented have no CSIDL and fail with ERROR_FILE_NOT_FOUND,
// which is what the real function says of an ID the system does not have:
// Downloads, SavedGames, Links, LocalAppDataLow, Public and the like. Nothing
// is approximated -- YY-Thunks answers LocalAppDataLow with LocalAppData and
// the sample-media folders with the public ones, and a caller told "here is
// your low-integrity directory" is worse off than one told there is none.
// The virtual folders (Control Panel, Recycle Bin, ...) are absent too, as
// they have no path under either API.
//
// Two things here are unlike the rest of winwrap.
//
// It loads libraries. dispatch.h's rule is GetModuleHandleW only, so that
// asking a question never changes what is in the process -- but the consumers
// of this function delay-load shell32, and since the redirect means no delay
// thunk is ever generated for it, nothing else is going to load shell32 on
// the caller's behalf. Calling SHGetKnownFolderPath *is* a request for
// shell32; the shim honours it the way the thunk would have. That also makes
// the system's own function reachable on a Windows that has it, which a
// GetModuleHandleW probe made before shell32 arrived would have missed for
// the life of the process. ole32 is loaded for CoTaskMemAlloc on the same
// reasoning: the caller is obliged to CoTaskMemFree the result.
//
// And it resolves SHGetFolderPathW and CoTaskMemAlloc at run time instead of
// importing them, so that winwrap goes on linking against kernel32 and
// advapi32 and nothing else. Importing them would put shell32.lib and
// ole32.lib on every consumer's link line, for one function few of them call.

namespace {

  struct KnownFolder {
    GUID id;
    int csidl;
  };

  // Generated from KnownFolders.h and ShlObj_core.h in the 10.0.26100 SDK:
  // each GUID is the SDK's, each number the SDK's CSIDL of the same folder.
  // Written out rather than taken from those headers because the FOLDERID_*
  // objects live in uuid.lib, and because the CSIDL header drags in the whole
  // shell interface set.
  const KnownFolder kFolders[] = {
    // FOLDERID_Desktop -> CSIDL_DESKTOPDIRECTORY
    {{0xB4BFCC3A, 0xDB2C, 0x424C, {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}}, 0x0010},
    // FOLDERID_Programs -> CSIDL_PROGRAMS
    {{0xA77F5D77, 0x2E2B, 0x44C3, {0xA6, 0xA2, 0xAB, 0xA6, 0x01, 0x05, 0x4A, 0x51}}, 0x0002},
    // FOLDERID_Documents -> CSIDL_PERSONAL
    {{0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}}, 0x0005},
    // FOLDERID_Favorites -> CSIDL_FAVORITES
    {{0x1777F761, 0x68AD, 0x4D8A, {0x87, 0xBD, 0x30, 0xB7, 0x59, 0xFA, 0x33, 0xDD}}, 0x0006},
    // FOLDERID_Startup -> CSIDL_STARTUP
    {{0xB97D20BB, 0xF46A, 0x4C97, {0xBA, 0x10, 0x5E, 0x36, 0x08, 0x43, 0x08, 0x54}}, 0x0007},
    // FOLDERID_Recent -> CSIDL_RECENT
    {{0xAE50C081, 0xEBD2, 0x438A, {0x86, 0x55, 0x8A, 0x09, 0x2E, 0x34, 0x98, 0x7A}}, 0x0008},
    // FOLDERID_SendTo -> CSIDL_SENDTO
    {{0x8983036C, 0x27C0, 0x404B, {0x8F, 0x08, 0x10, 0x2D, 0x10, 0xDC, 0xFD, 0x74}}, 0x0009},
    // FOLDERID_StartMenu -> CSIDL_STARTMENU
    {{0x625B53C3, 0xAB48, 0x4EC1, {0xBA, 0x1F, 0xA1, 0xEF, 0x41, 0x46, 0xFC, 0x19}}, 0x000b},
    // FOLDERID_Music -> CSIDL_MYMUSIC
    {{0x4BD8D571, 0x6D19, 0x48D3, {0xBE, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0E, 0x43}}, 0x000d},
    // FOLDERID_Videos -> CSIDL_MYVIDEO
    {{0x18989B1D, 0x99B5, 0x455B, {0x84, 0x1C, 0xAB, 0x7C, 0x74, 0xE4, 0xDD, 0xFC}}, 0x000e},
    // FOLDERID_Pictures -> CSIDL_MYPICTURES
    {{0x33E28130, 0x4E1E, 0x4676, {0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}}, 0x0027},
    // FOLDERID_NetHood -> CSIDL_NETHOOD
    {{0xC5ABBF53, 0xE17F, 0x4121, {0x89, 0x00, 0x86, 0x62, 0x6F, 0xC2, 0xC9, 0x73}}, 0x0013},
    // FOLDERID_PrintHood -> CSIDL_PRINTHOOD
    {{0x9274BD8D, 0xCFD1, 0x41C3, {0xB3, 0x5E, 0xB1, 0x3F, 0x55, 0xA7, 0x58, 0xF4}}, 0x001b},
    // FOLDERID_Fonts -> CSIDL_FONTS
    {{0xFD228CB7, 0xAE11, 0x4AE3, {0x86, 0x4C, 0x16, 0xF3, 0x91, 0x0A, 0xB8, 0xFE}}, 0x0014},
    // FOLDERID_Templates -> CSIDL_TEMPLATES
    {{0xA63293E8, 0x664E, 0x48DB, {0xA0, 0x79, 0xDF, 0x75, 0x9E, 0x05, 0x09, 0xF7}}, 0x0015},
    // FOLDERID_RoamingAppData -> CSIDL_APPDATA
    {{0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}}, 0x001a},
    // FOLDERID_LocalAppData -> CSIDL_LOCAL_APPDATA
    {{0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}}, 0x001c},
    // FOLDERID_InternetCache -> CSIDL_INTERNET_CACHE
    {{0x352481E8, 0x33BE, 0x4251, {0xBA, 0x85, 0x60, 0x07, 0xCA, 0xED, 0xCF, 0x9D}}, 0x0020},
    // FOLDERID_Cookies -> CSIDL_COOKIES
    {{0x2B0F765D, 0xC0E9, 0x4171, {0x90, 0x8E, 0x08, 0xA6, 0x11, 0xB8, 0x4F, 0xF6}}, 0x0021},
    // FOLDERID_History -> CSIDL_HISTORY
    {{0xD9DC8A3B, 0xB784, 0x432E, {0xA7, 0x81, 0x5A, 0x11, 0x30, 0xA7, 0x59, 0x63}}, 0x0022},
    // FOLDERID_Profile -> CSIDL_PROFILE
    {{0x5E6C858F, 0x0E22, 0x4760, {0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73}}, 0x0028},
    // FOLDERID_AdminTools -> CSIDL_ADMINTOOLS
    {{0x724EF170, 0xA42D, 0x4FEF, {0x9F, 0x26, 0xB6, 0x0E, 0x84, 0x6F, 0xBA, 0x4F}}, 0x0030},
    // FOLDERID_CDBurning -> CSIDL_CDBURN_AREA
    {{0x9E52AB10, 0xF80D, 0x49DF, {0xAC, 0xB8, 0x43, 0x30, 0xF5, 0x68, 0x78, 0x55}}, 0x003b},
    // FOLDERID_CommonStartMenu -> CSIDL_COMMON_STARTMENU
    {{0xA4115719, 0xD62E, 0x491D, {0xAA, 0x7C, 0xE7, 0x4B, 0x8B, 0xE3, 0xB0, 0x67}}, 0x0016},
    // FOLDERID_CommonPrograms -> CSIDL_COMMON_PROGRAMS
    {{0x0139D44E, 0x6AFE, 0x49F2, {0x86, 0x90, 0x3D, 0xAF, 0xCA, 0xE6, 0xFF, 0xB8}}, 0x0017},
    // FOLDERID_CommonStartup -> CSIDL_COMMON_STARTUP
    {{0x82A5EA35, 0xD9CD, 0x47C5, {0x96, 0x29, 0xE1, 0x5D, 0x2F, 0x71, 0x4E, 0x6E}}, 0x0018},
    // FOLDERID_CommonTemplates -> CSIDL_COMMON_TEMPLATES
    {{0xB94237E7, 0x57AC, 0x4347, {0x91, 0x51, 0xB0, 0x8C, 0x6C, 0x32, 0xD1, 0xF7}}, 0x002d},
    // FOLDERID_CommonAdminTools -> CSIDL_COMMON_ADMINTOOLS
    {{0xD0384E7D, 0xBAC3, 0x4797, {0x8F, 0x14, 0xCB, 0xA2, 0x29, 0xB3, 0x92, 0xB5}}, 0x002f},
    // FOLDERID_CommonOEMLinks -> CSIDL_COMMON_OEM_LINKS
    {{0xC1BAE2D0, 0x10DF, 0x4334, {0xBE, 0xDD, 0x7A, 0xA2, 0x0B, 0x22, 0x7A, 0x9D}}, 0x003a},
    // FOLDERID_PublicDesktop -> CSIDL_COMMON_DESKTOPDIRECTORY
    {{0xC4AA340D, 0xF20F, 0x4863, {0xAF, 0xEF, 0xF8, 0x7E, 0xF2, 0xE6, 0xBA, 0x25}}, 0x0019},
    // FOLDERID_PublicDocuments -> CSIDL_COMMON_DOCUMENTS
    {{0xED4824AF, 0xDCE4, 0x45A8, {0x81, 0xE2, 0xFC, 0x79, 0x65, 0x08, 0x36, 0x34}}, 0x002e},
    // FOLDERID_PublicMusic -> CSIDL_COMMON_MUSIC
    {{0x3214FAB5, 0x9757, 0x4298, {0xBB, 0x61, 0x92, 0xA9, 0xDE, 0xAA, 0x44, 0xFF}}, 0x0035},
    // FOLDERID_PublicPictures -> CSIDL_COMMON_PICTURES
    {{0xB6EBFB86, 0x6907, 0x413C, {0x9A, 0xF7, 0x4F, 0xC2, 0xAB, 0xF0, 0x7C, 0xC5}}, 0x0036},
    // FOLDERID_PublicVideos -> CSIDL_COMMON_VIDEO
    {{0x2400183A, 0x6185, 0x49FB, {0xA2, 0xD8, 0x4A, 0x39, 0x2A, 0x60, 0x2B, 0xA3}}, 0x0037},
    // FOLDERID_ProgramData -> CSIDL_COMMON_APPDATA
    {{0x62AB5D82, 0xFDC1, 0x4DC3, {0xA9, 0xDD, 0x07, 0x0D, 0x1D, 0x49, 0x5D, 0x97}}, 0x0023},
    // FOLDERID_Windows -> CSIDL_WINDOWS
    {{0xF38BF404, 0x1D43, 0x42F2, {0x93, 0x05, 0x67, 0xDE, 0x0B, 0x28, 0xFC, 0x23}}, 0x0024},
    // FOLDERID_System -> CSIDL_SYSTEM
    {{0x1AC14E77, 0x02E7, 0x4E5D, {0xB7, 0x44, 0x2E, 0xB1, 0xAE, 0x51, 0x98, 0xB7}}, 0x0025},
    // FOLDERID_SystemX86 -> CSIDL_SYSTEMX86
    {{0xD65231B0, 0xB2F1, 0x4857, {0xA4, 0xCE, 0xA8, 0xE7, 0xC6, 0xEA, 0x7D, 0x27}}, 0x0029},
    // FOLDERID_ProgramFiles -> CSIDL_PROGRAM_FILES
    {{0x905e63b6, 0xc1bf, 0x494e, {0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a}}, 0x0026},
    // FOLDERID_ProgramFilesX86 -> CSIDL_PROGRAM_FILESX86
    {{0x7C5A40EF, 0xA0FB, 0x4BFC, {0x87, 0x4A, 0xC0, 0xF2, 0xE0, 0xB9, 0xFA, 0x8E}}, 0x002a},
    // FOLDERID_ProgramFilesCommon -> CSIDL_PROGRAM_FILES_COMMON
    {{0xF7F1ED05, 0x9F6D, 0x47A2, {0xAA, 0xAE, 0x29, 0xD3, 0x17, 0xC6, 0xF0, 0x66}}, 0x002b},
    // FOLDERID_ProgramFilesCommonX86 -> CSIDL_PROGRAM_FILES_COMMONX86
    {{0xDE974D24, 0xD9C6, 0x4D3E, {0xBF, 0x91, 0xF4, 0x45, 0x51, 0x20, 0xB9, 0x17}}, 0x002c},
    // FOLDERID_ResourceDir -> CSIDL_RESOURCES
    {{0x8AD10C31, 0x2ADB, 0x4296, {0xA8, 0xF7, 0xE4, 0x70, 0x12, 0x32, 0xC9, 0x72}}, 0x0038},
    // FOLDERID_LocalizedResourcesDir -> CSIDL_RESOURCES_LOCALIZED
    {{0x2A00375E, 0x224C, 0x49DE, {0xB8, 0xD1, 0x44, 0x0D, 0xF7, 0xEF, 0x3D, 0xDC}}, 0x0039},
#if defined(_WIN64)
    // The explicitly 64-bit program directories exist only for a 64-bit
    // caller. The real function refuses them to a 32-bit one, and so, by
    // leaving them out, does this.
    // FOLDERID_ProgramFilesX64 -> CSIDL_PROGRAM_FILES
    {{0x6d809377, 0x6af0, 0x444b, {0x89, 0x57, 0xa3, 0x77, 0x3f, 0x02, 0x20, 0x0e}}, 0x0026},
    // FOLDERID_ProgramFilesCommonX64 -> CSIDL_PROGRAM_FILES_COMMON
    {{0x6365d5a7, 0xf0d, 0x45e5, {0x87, 0xf6, 0xd, 0xa5, 0x6b, 0x6a, 0x4f, 0x7d}}, 0x002b},
#endif
  };

  // KNOWN_FOLDER_FLAG values. Five of them are CSIDL_FLAG_* bit for bit,
  // because that is where Vista got them.
  constexpr DWORD kFlagCreate       = 0x00008000; // CSIDL_FLAG_CREATE
  constexpr DWORD kFlagDontVerify   = 0x00004000; // CSIDL_FLAG_DONT_VERIFY
  constexpr DWORD kFlagDontUnexpand = 0x00002000; // CSIDL_FLAG_DONT_UNEXPAND
  constexpr DWORD kFlagNoAlias      = 0x00001000; // CSIDL_FLAG_NO_ALIAS
  constexpr DWORD kFlagInit         = 0x00000800; // CSIDL_FLAG_PER_USER_INIT
  constexpr DWORD kFlagDefaultPath  = 0x00000400; // -> SHGFP_TYPE_DEFAULT
  constexpr DWORD kFlagAliasOnly    = 0x80000000;
  constexpr DWORD kCsidlFlags =
      kFlagCreate | kFlagDontVerify | kFlagDontUnexpand | kFlagNoAlias | kFlagInit;

  constexpr DWORD kShgfpTypeCurrent = 0;
  constexpr DWORD kShgfpTypeDefault = 1;

  using SHGetFolderPathWFn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
  using CoTaskMemAllocFn   = LPVOID(WINAPI*)(SIZE_T);

#if !defined(_WIN64)
  // The plain counterpart of one of the three "X86" folders, or -1.
  //
  // On 32-bit Windows there is one Program Files and one system directory,
  // and the real function answers the X86 IDs with those. The X86 CSIDLs are
  // another matter: they are backed by registry values that only a 64-bit
  // Windows has, so 32-bit XP may well have nothing to say for them. When
  // one comes back empty the plain CSIDL is asked instead. Under WOW64 the
  // X86 CSIDL succeeds and this is never reached -- and would be right if it
  // were, since a 32-bit process there sees the x86 directories as the plain
  // ones. A 64-bit build needs none of it: every x64 Windows has both.
  int PlainFormOf(int csidl) {
    switch (csidl) {
      case 0x0029: // CSIDL_SYSTEMX86
        return 0x0025; // CSIDL_SYSTEM
      case 0x002a: // CSIDL_PROGRAM_FILESX86
        return 0x0026; // CSIDL_PROGRAM_FILES
      case 0x002c: // CSIDL_PROGRAM_FILES_COMMONX86
        return 0x002b; // CSIDL_PROGRAM_FILES_COMMON
      default:
        return -1;
    }
  }
#endif

  bool SameGuid(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
  }

  std::atomic<void*> g_ole32{nullptr};
  std::atomic<void*> g_known_folder_path{nullptr};
  std::atomic<void*> g_folder_path{nullptr};
  std::atomic<void*> g_task_alloc{nullptr};

} // namespace

extern "C" HRESULT WINAPI ww_SHGetKnownFolderPath(const GUID& rfid,
                                                  DWORD dwFlags,
                                                  HANDLE hToken,
                                                  PWSTR* ppszPath) {
  const HMODULE shell32 = Shell32();

  // Hand straight over where Windows has this itself.
  if (const auto system_fn = reinterpret_cast<decltype(&ww_SHGetKnownFolderPath)>(
          ::winwrap::ResolveFrom(g_known_folder_path, shell32, "SHGetKnownFolderPath"))) {
    return system_fn(rfid, dwFlags, hToken, ppszPath);
  }

  if (!ppszPath) {
    return E_INVALIDARG;
  }
  // Null on every failure, as documented, so a caller that frees the result
  // without checking -- libuv's does -- frees nothing.
  *ppszPath = nullptr;

  const auto get_folder_path =
      reinterpret_cast<SHGetFolderPathWFn>(::winwrap::ResolveFrom(g_folder_path, shell32, "SHGetFolderPathW"));
  const auto task_alloc = reinterpret_cast<CoTaskMemAllocFn>(
      ::winwrap::ResolveFrom(g_task_alloc, ::winwrap::LoadSystemLibrary(g_ole32, L"ole32.dll"), "CoTaskMemAlloc"));
  if (!get_folder_path || !task_alloc) {
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }

  // The alias form of a path is a shell namespace notion with no CSIDL
  // counterpart; there is nothing to return for a caller that wants only it.
  if (dwFlags & kFlagAliasOnly) {
    return E_INVALIDARG;
  }

  int csidl = -1;
  for (const KnownFolder& folder : kFolders) {
    if (SameGuid(folder.id, rfid)) {
      csidl = folder.csidl;
      break;
    }
  }
  if (csidl < 0) {
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }

  // The remaining flags -- KF_FLAG_NOT_PARENT_RELATIVE, KF_FLAG_SIMPLE_IDLIST
  // and the app-container ones later Windows added -- shape results this
  // function does not produce or contexts XP does not have, and are ignored.
  wchar_t path[MAX_PATH] = {};
  const int csidl_flags  = static_cast<int>(dwFlags & kCsidlFlags);
  const DWORD path_type  = (dwFlags & kFlagDefaultPath) ? kShgfpTypeDefault : kShgfpTypeCurrent;
  HRESULT result = get_folder_path(nullptr, csidl | csidl_flags, hToken, path_type, path);
#if !defined(_WIN64)
  if (result != S_OK && PlainFormOf(csidl) >= 0) {
    path[0] = L'\0';
    result  = get_folder_path(nullptr, PlainFormOf(csidl) | csidl_flags, hToken, path_type, path);
  }
#endif
  if (FAILED(result)) {
    return result;
  }
  // S_FALSE is SHGetFolderPathW's "a valid folder, but it does not exist",
  // a distinction the new function reports as an error.
  if (result != S_OK) {
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }

  const size_t bytes = (wcslen(path) + 1) * sizeof(wchar_t);
  PWSTR copy         = static_cast<PWSTR>(task_alloc(bytes));
  if (!copy) {
    return E_OUTOFMEMORY;
  }
  memcpy(copy, path, bytes);
  *ppszPath = copy;
  return S_OK;
}

// The same function under its undecorated-import name: SHGetKnownFolderPath
// itself, _SHGetKnownFolderPath@16 to the x86 linker.
//
// Every other redirect in winwrap is an `__imp_<name>` slot and nothing more,
// because every other function is declared __declspec(dllimport) in the SDK
// and a call to it compiles to a load through that slot. This one is not.
// ShlObj_core.h declares it STDAPI -- no dllimport -- so a caller references
// the function by name, the way it would reference something in its own
// program, and expects the linker to find a body. shell32.lib has one: its
// member for this function defines both the name, as a jump through the slot,
// and the slot. Pulling that member in for the name therefore brings a second
// `__imp__SHGetKnownFolderPath@16` with it, and the link fails with LNK2005
// against winwrap's.
//
// Defining the name here means the reference is settled before shell32.lib is
// asked, its member is never extracted, and there is one slot. That depends
// on winwrap coming ahead of shell32.lib among the libraries, which is where
// a consumer already has to put it. Should the order ever be the other way
// round the result is the same LNK2005 -- a build failure, not a binary that
// quietly calls the wrong thing.
//
// winwrap.h does not declare this; it is not a ww_ stand-in and nothing in
// winwrap calls it. The definition is its own declaration.
extern "C" HRESULT WINAPI SHGetKnownFolderPath(const GUID& rfid,
                                               DWORD dwFlags,
                                               HANDLE hToken,
                                               PWSTR* ppszPath) {
  return ww_SHGetKnownFolderPath(rfid, dwFlags, hToken, ppszPath);
}
