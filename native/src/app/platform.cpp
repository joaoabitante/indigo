#include "platform.h"

#include <SDL3/SDL.h>

#include <iterator>

#include "core/safe.h"

#ifdef SDL_PLATFORM_ANDROID
#include <jni.h>
#endif
#if defined(SDL_PLATFORM_LINUX) && !defined(SDL_PLATFORM_ANDROID)
#include <pwd.h>
#include <unistd.h>
#define INDIGO_XDG 1
#endif
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#endif

namespace indigo::platform {

namespace {
std::string lastPart(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

#ifdef SDL_PLATFORM_ANDROID
// chama um método da IndigoActivity (a referência da atividade vem do SDL e tem de ser liberada)
template <typename F>
void withActivity(F&& f) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto act = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !act) return;
  jclass cls = env->GetObjectClass(act);
  f(env, act, cls);
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(act);
}
#endif
}  // namespace

bool isMobile() {
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
  return true;
#else
  return simulatedPhone(nullptr, nullptr);
#endif
}

bool simulatedPhone(int* w, int* h) {
  // INDIGO_MOBILE=540x1200: simula o celular no PC (janela desse tamanho, mouse vira dedo) para testar a interface
  const char* v = SDL_getenv("INDIGO_MOBILE");
  int a = 0, b = 0;
  if (!v || SDL_sscanf(v, "%dx%d", &a, &b) != 2 || a <= 0 || b <= 0) return false;
  if (w) *w = a;
  if (h) *h = b;
  return true;
}

std::string displayName(const std::string& path) {
#ifdef SDL_PLATFORM_ANDROID
  if (path.rfind("content://", 0) == 0) {
    std::string out;
    withActivity([&](JNIEnv* env, jobject act, jclass cls) {
      jmethodID m = env->GetMethodID(cls, "displayName", "(Ljava/lang/String;)Ljava/lang/String;");
      if (!m) return;
      jstring arg = env->NewStringUTF(path.c_str());
      auto res = static_cast<jstring>(env->CallObjectMethod(act, m, arg));
      env->DeleteLocalRef(arg);
      if (res && !env->ExceptionCheck()) {
        const char* s = env->GetStringUTFChars(res, nullptr);
        if (s) { out = s; env->ReleaseStringUTFChars(res, s); }
      }
      if (res) env->DeleteLocalRef(res);
    });
    if (!out.empty()) return safeFileName(out);  // o nome vem do outro app: sem pastas nem ".."
  }
#endif
  return lastPart(path);
}

std::string takePendingRom() {
  std::string out;
#ifdef SDL_PLATFORM_ANDROID
  withActivity([&](JNIEnv* env, jobject act, jclass cls) {
    jmethodID m = env->GetMethodID(cls, "takePendingRom", "()Ljava/lang/String;");
    if (!m) return;
    auto res = static_cast<jstring>(env->CallObjectMethod(act, m));
    if (res && !env->ExceptionCheck()) {
      const char* s = env->GetStringUTFChars(res, nullptr);
      if (s) { out = s; env->ReleaseStringUTFChars(res, s); }
    }
    if (res) env->DeleteLocalRef(res);
  });
#endif
  return out;
}

#ifdef INDIGO_XDG
namespace {
// modo portátil: o arquivo "indigo-portatil" ao lado do executável faz tudo ficar em <pasta do executável>/dados/
// (pendrive, cartão do Steam Deck). Sem ele o Índigo nunca escreve ao lado do executável.
std::string portableDir() {
  const char* base = SDL_GetBasePath();  // do SDL, termina em '/'; não liberar
  if (!base) return "";
  const std::string dir = base;
  SDL_PathInfo info{};
  if (!SDL_GetPathInfo((dir + "indigo-portatil").c_str(), &info)) return "";
  return dir + "dados/";
}

// pasta base da especificação XDG: a variável, se for um caminho absoluto; senão ~/<padrão>
std::string xdgBase(const char* var, const char* fallback) {
  const char* v = SDL_getenv(var);
  if (v && v[0] == '/') return v;
  std::string home;
  if (const char* h = SDL_getenv("HOME"); h && h[0] == '/') home = h;
  else if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) home = pw->pw_dir;
  if (home.empty()) return "";
  return home + "/" + fallback;
}

std::string ensureDir(std::string dir) {
  if (dir.empty()) return "";
  if (dir.back() != '/') dir += '/';
  return SDL_CreateDirectory(dir.c_str()) ? dir : "";
}
}  // namespace
#endif

std::string dataDir() {
#ifdef INDIGO_XDG
  if (std::string p = ensureDir(portableDir()); !p.empty()) return p;
  if (std::string base = xdgBase("XDG_DATA_HOME", ".local/share"); !base.empty()) return ensureDir(base + "/indigo");
  return "";
#else
  std::string out;
  if (char* pref = SDL_GetPrefPath("Indigo", "Indigo")) { out = pref; SDL_free(pref); }
  return out;
#endif
}

std::string configDir() {
#ifdef INDIGO_XDG
  if (std::string p = ensureDir(portableDir()); !p.empty()) return p;
  if (std::string base = xdgBase("XDG_CONFIG_HOME", ".config"); !base.empty())
    if (std::string dir = ensureDir(base + "/indigo"); !dir.empty()) return dir;
  return dataDir();
#else
  return dataDir();
#endif
}

void haptic() {
#ifdef SDL_PLATFORM_ANDROID
  withActivity([](JNIEnv* env, jobject act, jclass cls) {
    jmethodID m = env->GetMethodID(cls, "haptic", "()V");
    if (m) env->CallVoidMethod(act, m);
  });
#endif
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
}  // namespace
#endif

HttpResult http(const std::string& method, const std::string& url, const std::string& pin, const std::vector<uint8_t>& body,
                int timeoutMs) {
  HttpResult r;
#if defined(_WIN32)
  const std::wstring wurl = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host; uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
  wchar_t extra[2048] = {};
  uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2047;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "Endereço inválido."; return r; }
  HINTERNET ses = WinHttpOpen(L"Indigo", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) ses = WinHttpOpen(L"Indigo", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) { r.error = "Sem acesso à rede."; return r; }
  WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
  HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
  const std::wstring target = std::wstring(path) + extra;
  HINTERNET req = con ? WinHttpOpenRequest(con, widen(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
  if (req) {
    std::wstring hdr = L"Cache-Control: no-store\r\n";
    if (!pin.empty()) hdr += L"x-indigo-pin: " + widen(pin) + L"\r\n";
    if (!body.empty()) hdr += L"Content-Type: application/octet-stream\r\n";
    const BOOL sent = WinHttpSendRequest(req, hdr.c_str(), static_cast<DWORD>(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<uint8_t*>(body.data()),
                                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (sent && WinHttpReceiveResponse(req, nullptr)) {
      DWORD code = 0, len = sizeof code;
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
      r.status = static_cast<int>(code);
      wchar_t ct[256] = {};
      DWORD ctl = sizeof ct;
      if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct, &ctl, WINHTTP_NO_HEADER_INDEX))
        for (const wchar_t* p = ct; *p; p++) r.contentType += static_cast<char>(*p < 128 ? *p : '?');
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        const size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, r.body.data() + at, avail, &got)) { r.body.resize(at); break; }
        r.body.resize(at + got);
        if (r.body.size() > 64u * 1024 * 1024) break;
      }
    } else {
      r.error = "Sem conexão com a nuvem.";
    }
    WinHttpCloseHandle(req);
  } else {
    r.error = "Sem conexão com a nuvem.";
  }
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
#elif defined(SDL_PLATFORM_ANDROID)
  // a IndigoActivity devolve [status (4 bytes, big-endian)] [tipo do conteúdo terminado em \n] [corpo]
  withActivity([&](JNIEnv* env, jobject act, jclass cls) {
    jmethodID m = env->GetMethodID(cls, "httpRequest", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BI)[B");
    if (!m) { r.error = "Sem suporte a rede."; return; }
    jstring jm = env->NewStringUTF(method.c_str()), ju = env->NewStringUTF(url.c_str()), jp = env->NewStringUTF(pin.c_str());
    jbyteArray jb = nullptr;
    if (!body.empty()) {
      jb = env->NewByteArray(static_cast<jsize>(body.size()));
      env->SetByteArrayRegion(jb, 0, static_cast<jsize>(body.size()), reinterpret_cast<const jbyte*>(body.data()));
    }
    auto res = static_cast<jbyteArray>(env->CallObjectMethod(act, m, jm, ju, jp, jb, static_cast<jint>(timeoutMs)));
    if (res && !env->ExceptionCheck()) {
      const jsize n = env->GetArrayLength(res);
      std::vector<uint8_t> all(static_cast<size_t>(n));
      if (n) env->GetByteArrayRegion(res, 0, n, reinterpret_cast<jbyte*>(all.data()));
      if (all.size() >= 4) {
        const int st = static_cast<int>((uint32_t(all[0]) << 24) | (all[1] << 16) | (all[2] << 8) | all[3]);
        size_t p = 4;
        while (p < all.size() && all[p] != '\n') r.contentType += static_cast<char>(all[p++]);
        if (p < all.size()) p++;
        if (st <= 0) r.error.assign(all.begin() + static_cast<long>(p), all.end());
        else { r.status = st; r.body.assign(all.begin() + static_cast<long>(p), all.end()); }
      }
    }
    for (jobject o : {static_cast<jobject>(jm), static_cast<jobject>(ju), static_cast<jobject>(jp), static_cast<jobject>(jb), static_cast<jobject>(res)})
      if (o) env->DeleteLocalRef(o);
  });
  if (!r.status && r.error.empty()) r.error = "Sem conexão com a nuvem.";
#else
  (void)method; (void)url; (void)pin; (void)body; (void)timeoutMs;
  r.error = "Nuvem não disponível neste sistema.";
#endif
  return r;
}

#ifdef _WIN32
namespace {
const wchar_t* const PROGID = L"Indigo.Jogo";
const wchar_t* const EXTS[] = {L".sfc", L".smc", L".nes", L".gb", L".gbc", L".sgb", L".gba", L".gg", L".sms", L".ngp", L".ngc", L".min", L".a26", L".a52", L".mx1", L".mx2", L".col", L".sg", L".ws", L".wsc", L".a78", L".vec", L".int", L".itv", L".lnx", L".vb", L".duck", L".md0", L".md1", L".md2", L".sv", L".pce", L".sgx", L".md", L".gen", L".smd", L".32x", L".fds", L".qd", L".tgc", L".nds", L".j64", L".jag", L".abs", L".z64", L".n64", L".v64", L".cue", L".chd", L".gdi"};

std::wstring exePath() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return std::wstring(buf, n);
}
std::wstring openCommand() { return L"\"" + exePath() + L"\" \"%1\""; }

bool setString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
std::wstring getString(const std::wstring& key, const wchar_t* name) {
  wchar_t buf[2048];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
  return buf;
}
}  // namespace
#endif

bool canAssociate() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

bool filesAssociated() {
#ifdef _WIN32
  return getString(std::wstring(L"Software\\Classes\\") + PROGID + L"\\shell\\open\\command", nullptr) == openCommand();
#else
  return false;
#endif
}

bool associateFiles(bool on) {
#ifdef _WIN32
  const std::wstring cls = std::wstring(L"Software\\Classes\\") + PROGID;
  bool ok = true;
  if (on) {
    ok &= setString(cls, nullptr, L"Jogo do Índigo");
    ok &= setString(cls + L"\\DefaultIcon", nullptr, L"\"" + exePath() + L"\",0");
    ok &= setString(cls + L"\\shell\\open\\command", nullptr, openCommand());
    for (const wchar_t* e : EXTS) {
      const std::wstring ext = std::wstring(L"Software\\Classes\\") + e;
      ok &= RegSetKeyValueW(HKEY_CURRENT_USER, (ext + L"\\OpenWithProgids").c_str(), PROGID, REG_NONE, nullptr, 0) == ERROR_SUCCESS;
      if (getString(ext, nullptr).empty()) ok &= setString(ext, nullptr, PROGID);  // não tira o padrão de outro programa
    }
  } else {
    for (const wchar_t* e : EXTS) {
      const std::wstring ext = std::wstring(L"Software\\Classes\\") + e;
      RegDeleteKeyValueW(HKEY_CURRENT_USER, (ext + L"\\OpenWithProgids").c_str(), PROGID);
      if (getString(ext, nullptr) == PROGID) RegDeleteKeyValueW(HKEY_CURRENT_USER, ext.c_str(), nullptr);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, cls.c_str());
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // o Explorer atualiza os ícones
  return ok;
#else
  (void)on;
  return false;
#endif
}

}  // namespace indigo::platform
