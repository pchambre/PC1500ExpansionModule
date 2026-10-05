#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include <string>
#include <vector>

#include "flutter_window.h"
#include "utils.h"

namespace {

// The folder pc1500_ble.exe is in: everything it needs must be beside it.
std::wstring ExeFolder() {
  wchar_t path[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring folder(path, n);
  size_t slash = folder.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : folder.substr(0, slash);
}

bool Exists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// stderr when it's redirected to a pipe or a file (a script running it).
HANDLE RedirectedStderr() {
  HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
  if (err == nullptr || err == INVALID_HANDLE_VALUE) return nullptr;
  DWORD type = ::GetFileType(err);
  return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK ? err : nullptr;
}

// Text for the terminal pc1500_ble was started from (2026-10-03). A windowed
// program gets none of its own, so this writes to stderr if that's been
// redirected, else to the parent's console when there is one. Returns false
// if there's neither (started from Explorer).
bool ToTerminal(const std::wstring& text) {
  if (HANDLE err = RedirectedStderr()) {
    std::string utf8 = Utf8FromUtf16((text + L"\r\n").c_str());
    DWORD written;
    ::WriteFile(err, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    return true;
  }
  HANDLE out = ::CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (out == INVALID_HANDLE_VALUE) return false;
  DWORD written;
  // The shell has usually printed its prompt already: start on a new line.
  std::wstring line = L"\r\n" + text + L"\r\n";
  ::WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
  ::CloseHandle(out);
  return true;
}

// A problem that stops it starting: told to the terminal, or else in a box.
void Problem(const std::wstring& text, bool terminal) {
  if (!terminal || !ToTerminal(text)) ::MessageBoxW(nullptr, text.c_str(), L"pc1500_ble", MB_OK | MB_ICONERROR);
}

const wchar_t kUsage[] =
    L"pc1500_ble -- the PC-1500 BLE app: a feature server a PC-1500 connects to.\r\n"
    L"It opens a window and takes no arguments. Run it from anywhere, e.g. in\r\n"
    L"PowerShell:  & \"<folder>\\pc1500_ble.exe\"  (a quoted path alone is just a string).\r\n"
    L"Keep its folder together: it needs data\\ and its DLLs beside it.\r\n"
    L"Its log is %TEMP%\\pc1500_ble.log; files go to Documents\\PC1500-BLE.";

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  const bool terminal = ::AttachConsole(ATTACH_PARENT_PROCESS) != 0 || RedirectedStderr() != nullptr;
  if (!terminal && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  std::vector<std::string> command_line_arguments = GetCommandLineArguments();
  for (const std::string& arg : command_line_arguments) {
    if (arg == "--help" || arg == "-h" || arg == "/?" || arg == "-?") {
      if (!ToTerminal(kUsage)) ::MessageBoxW(nullptr, kUsage, L"pc1500_ble", MB_OK | MB_ICONINFORMATION);
      return EXIT_SUCCESS;
    }
  }
  if (!command_line_arguments.empty() && terminal) {
    ToTerminal(L"pc1500_ble takes no arguments; ignoring them (--help says more).");
  }

  // What the engine needs beside the exe (the engine and plugin DLLs are
  // delay-loaded -- runner/CMakeLists.txt -- so this runs before they're
  // needed). Without them it would just not start, silently.
  const std::wstring folder = ExeFolder();
  std::wstring missing;
  for (const wchar_t* need : {L"flutter_windows.dll", L"ble_peripheral_plugin.dll",
                              L"flutter_secure_storage_windows_plugin.dll", L"data\\icudtl.dat",
                              L"data\\flutter_assets", L"data\\app.so"}) {
#ifndef NDEBUG
    if (std::wstring(need) == L"data\\app.so") continue;  // a debug build runs from its kernel blob instead
#endif
    if (!Exists(folder + L"\\" + need)) missing += L"  " + std::wstring(need) + L"\r\n";
  }
  if (!missing.empty()) {
    Problem(L"pc1500_ble can't start: these are missing from " + folder + L":\r\n" + missing +
                L"Run it from its build folder (ble_app\\build\\windows\\x64\\runner\\Release), or copy\r\n"
                L"that whole folder -- the exe needs data\\ and the DLLs beside it.",
            terminal);
    return EXIT_FAILURE;
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(folder + L"\\data");

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  FlutterWindow window(project);
  Win32Window::Point origin(10, 10);
  Win32Window::Size size(1280, 720);
  if (!window.Create(L"pc1500_ble", origin, size)) {
    Problem(L"pc1500_ble couldn't open its window (the Flutter engine didn't start).", terminal);
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);
  if (terminal) ToTerminal(L"pc1500_ble started: its window is open (log: %TEMP%\\pc1500_ble.log).");

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
