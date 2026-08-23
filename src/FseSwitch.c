#include <windows.h>
#include <gamingexperience.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "onecore.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

static int usage(void) {
  fprintf(stderr, "Usage: FseSwitch.exe xbox|desktop|status\n");
  return 2;
}

static int is_system(void) {
  HANDLE tok = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    return 0;
  DWORD len = 0;
  GetTokenInformation(tok, TokenUser, NULL, 0, &len);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    CloseHandle(tok);
    return 0;
  }
  BYTE *buf = (BYTE *)LocalAlloc(LPTR, len);
  int sys = 0;
  if (buf && GetTokenInformation(tok, TokenUser, buf, len, &len)) {
    TOKEN_USER *tu = (TOKEN_USER *)buf;
    SID_IDENTIFIER_AUTHORITY a = SECURITY_NT_AUTHORITY;
    PSID s = NULL;
    if (AllocateAndInitializeSid(&a, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0,
                                 0, 0, 0, &s)) {
      sys = EqualSid(tu->User.Sid, s);
      FreeSid(s);
    }
  }
  LocalFree(buf);
  CloseHandle(tok);
  return sys;
}

static HANDLE get_user_token(void) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return NULL;
  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);
  HANDLE found = NULL;
  DWORD best = WTSGetActiveConsoleSessionId();
  if (Process32First(snap, &pe)) {
    do {
      if (_stricmp(pe.szExeFile, "explorer.exe") == 0) {
        HANDLE proc =
            OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
        if (!proc)
          continue;
        HANDLE tok = NULL;
        if (OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY, &tok)) {
          DWORD s = 0, rl = sizeof(s);
          GetTokenInformation(tok, TokenSessionId, &s, sizeof(s), &rl);
          if (!found || s == best) {
            if (found)
              CloseHandle(found);
            found = tok;
            if (s == best) {
              CloseHandle(proc);
              break;
            }
          } else
            CloseHandle(tok);
        }
        CloseHandle(proc);
      }
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  if (!found) {
    DWORD sess = WTSGetActiveConsoleSessionId();
    if (sess != 0xFFFFFFFF)
      WTSQueryUserToken(sess, &found);
  }
  return found;
}

static int run_fse_direct(const char *cmd) {
  if (!IsGamingFullScreenExperienceSupported()) {
    fprintf(stderr, "Not supported\n");
    return 3;
  }
  if (strcmp(cmd, "status") == 0) {
    printf("supported=true can_set=%s active=%s\n",
           CanSetGamingFullScreenExperience() ? "true" : "false",
           IsGamingFullScreenExperienceActive() ? "true" : "false");
    return 0;
  }
  BOOL active;
  if (strcmp(cmd, "xbox") == 0)
    active = TRUE;
  else if (strcmp(cmd, "desktop") == 0)
    active = FALSE;
  else
    return usage();
  HRESULT hr = SetGamingFullScreenExperience(active);
  if (FAILED(hr)) {
    fprintf(stderr,
            "SetGamingFullScreenExperience(%s) failed: 0x%08lX can_set=%s "
            "active=%s\n",
            active ? "true" : "false", (unsigned long)hr,
            CanSetGamingFullScreenExperience() ? "true" : "false",
            IsGamingFullScreenExperienceActive() ? "true" : "false");
    return (int)hr;
  }
  printf("requested=%s\n", active ? "xbox" : "desktop");
  for (int i = 0; i < 30; i++) {
    if (IsGamingFullScreenExperienceActive() == active)
      break;
    Sleep(200);
  }
  if (IsGamingFullScreenExperienceActive() != active)
    fprintf(stderr, "Warning: active still %s after 6s\n",
            IsGamingFullScreenExperienceActive() ? "true" : "false");
  else
    printf("confirmed active=%s\n",
           IsGamingFullScreenExperienceActive() ? "true" : "false");
  return 0;
}

static int run_as_user(const char *cmd) {
  HANDLE tok = get_user_token();
  if (!tok) {
    fprintf(stderr, "get_user_token failed\n");
    return 5;
  }
  HANDLE primary = NULL;
  if (!DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                        TokenPrimary, &primary)) {
    fprintf(stderr, "DuplicateTokenEx failed: %lu\n", GetLastError());
    CloseHandle(tok);
    return 5;
  }
  CloseHandle(tok);
  char exe[MAX_PATH];
  GetModuleFileNameA(NULL, exe, MAX_PATH);
  char line[MAX_PATH + 64];
  wsprintfA(line, "\"%s\" %s --child", exe, cmd);
  void *env = NULL;
  CreateEnvironmentBlock(&env, primary, FALSE);
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;
  HANDLE rPipe = NULL, wPipe = NULL;
  CreatePipe(&rPipe, &wPipe, &sa, 0);
  SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.lpDesktop = "winsta0\\default";
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wPipe;
  si.hStdError = wPipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  BOOL ok = CreateProcessAsUserA(primary, NULL, line, NULL, NULL, TRUE,
                                 CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                 env, NULL, &si, &pi);
  if (!ok) {
    fprintf(stderr, "CreateProcessAsUser failed: %lu\n", GetLastError());
    if (env)
      DestroyEnvironmentBlock(env);
    CloseHandle(primary);
    CloseHandle(rPipe);
    CloseHandle(wPipe);
    return 5;
  }
  if (env)
    DestroyEnvironmentBlock(env);
  CloseHandle(primary);
  CloseHandle(wPipe);
  char buf[4096];
  DWORD avail, read;
  for (int i = 0; i < 150; i++) {
    DWORD wr = WaitForSingleObject(pi.hProcess, 200);
    while (PeekNamedPipe(rPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
      if (ReadFile(rPipe, buf, sizeof(buf) - 1, &read, NULL) && read > 0) {
        buf[read] = 0;
        fwrite(buf, 1, read, stdout);
        fwrite(buf, 1, read, stderr);
      }
    }
    if (wr == WAIT_OBJECT_0)
      break;
  }
  while (PeekNamedPipe(rPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
    if (ReadFile(rPipe, buf, sizeof(buf) - 1, &read, NULL) && read > 0) {
      buf[read] = 0;
      fwrite(buf, 1, read, stdout);
    }
  }
  CloseHandle(rPipe);
  DWORD ec = 0;
  GetExitCodeProcess(pi.hProcess, &ec);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)ec;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return usage();
  int child = (argc == 3 && strcmp(argv[2], "--child") == 0);
  if (child)
    return run_fse_direct(argv[1]);
  if (argc == 3)
    return usage();
  if (!is_system())
    return run_fse_direct(argv[1]);
  if (strcmp(argv[1], "status") == 0)
    return run_fse_direct(argv[1]);
  fprintf(stderr, "SYSTEM detected, launching as active user...\n");
  return run_as_user(argv[1]);
}

