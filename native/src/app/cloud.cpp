#include "cloud.h"

#include <cstdio>

#include "core/backup.h"
#include "platform.h"

namespace indigo::cloud {

namespace {
constexpr int TIMEOUT_MS = 15000;

std::string num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.0f", v);
  return b;
}

bool isJson(const platform::HttpResult& r) { return r.contentType.find("json") != std::string::npos; }

// mensagem de erro de uma resposta (o site manda {"error": "..."}), e se foi por causa do PIN
void fillError(const platform::HttpResult& r, Result& res) {
  res.pinError = r.status == 401;
  if (!r.status) { res.error = r.error.empty() ? "Sem conexão com a nuvem." : r.error; return; }
  res.error = "Erro " + std::to_string(r.status) + " na nuvem.";
  backup::Json j;
  if (isJson(r) && backup::parseJson(std::string(r.body.begin(), r.body.end()), j)) {
    const std::string e = j.str("error");
    if (!e.empty()) res.error = e;
  }
}
}  // namespace

std::string urlEncode(const std::string& s) {
  static const char* HEX = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += HEX[c >> 4]; out += HEX[c & 15]; }
  }
  return out;
}

Client::Client(std::string baseUrl) : base_(std::move(baseUrl)) {
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
  mutex_ = SDL_CreateMutex();
  cond_ = SDL_CreateCondition();
  thread_ = SDL_CreateThread(threadMain, "nuvem", this);
}

Client::~Client() {
  SDL_LockMutex(mutex_);
  quit_ = true;
  jobs_.clear();
  SDL_SignalCondition(cond_);
  SDL_UnlockMutex(mutex_);
  // a requisição em andamento termina sozinha pelo tempo limite
  if (thread_) SDL_WaitThread(thread_, nullptr);
  SDL_DestroyCondition(cond_);
  SDL_DestroyMutex(mutex_);
}

void Client::setPin(const std::string& p) { SDL_LockMutex(mutex_); pin_ = p; SDL_UnlockMutex(mutex_); }
std::string Client::pin() const { SDL_LockMutex(mutex_); std::string p = pin_; SDL_UnlockMutex(mutex_); return p; }

int Client::threadMain(void* self) { static_cast<Client*>(self)->run(); return 0; }

void Client::run() {
  for (;;) {
    SDL_LockMutex(mutex_);
    while (!quit_ && jobs_.empty()) SDL_WaitCondition(cond_, mutex_);
    if (quit_) { SDL_UnlockMutex(mutex_); return; }
    Job j = std::move(jobs_.front());
    jobs_.pop_front();
    j.pin = j.kind == Result::Auth ? j.pin : pin_;
    running_++;
    SDL_UnlockMutex(mutex_);
    Result r = execute(j);
    SDL_LockMutex(mutex_);
    running_--;
    results_.push_back(std::move(r));
    SDL_UnlockMutex(mutex_);
  }
}

Result Client::execute(const Job& j) {
  Result res;
  res.kind = j.kind; res.key = j.key; res.at = j.at; res.tag = j.tag;
  platform::HttpResult r;
  switch (j.kind) {
    case Result::Ping: {
      r = platform::http("GET", url("/api/transfer?action=ping"), "", {}, TIMEOUT_MS);
      backup::Json js;
      if (r.status == 200 && isJson(r) && backup::parseJson(std::string(r.body.begin(), r.body.end()), js)) {
        res.ok = true;
        res.storage = js.flag("storage");
      } else fillError(r, res);
      return res;
    }
    case Result::Auth:
      r = platform::http("GET", url("/api/transfer?action=auth"), j.pin, {}, TIMEOUT_MS);
      res.ok = r.status == 200;
      if (!res.ok) fillError(r, res);
      res.tag = j.pin;
      return res;
    case Result::List: {
      r = platform::http("GET", url("/api/cloud?action=list"), j.pin, {}, TIMEOUT_MS);
      backup::Json js;
      if (r.status == 200 && backup::parseJson(std::string(r.body.begin(), r.body.end()), js)) {
        if (const backup::Json* a = js.get("saves"); a && a->type == backup::Json::Arr) {
          for (const auto& e : a->a) {
            RemoteSave s;
            s.key = e.str("key"); s.at = e.num("at"); s.size = static_cast<size_t>(e.num("size"));
            s.from = e.str("from"); s.name = e.str("name"); s.console = e.str("console");
            if (!s.key.empty() && s.at > 0) res.list.push_back(std::move(s));
          }
          res.ok = true;
          return res;
        }
      }
      fillError(r, res);
      if (r.status == 200) res.error = "Resposta estranha da nuvem.";
      return res;
    }
    case Result::Put: {
      const std::string q = "/api/cloud?action=put&key=" + urlEncode(j.key) + "&at=" + num(j.at) + "&from=" + urlEncode(j.from) +
                            "&name=" + urlEncode(j.name) + "&console=" + urlEncode(j.console);
      r = platform::http("POST", url(q), j.pin, j.data, TIMEOUT_MS);
      res.ok = r.status == 200;
      if (!res.ok) fillError(r, res);
      return res;
    }
    case Result::Get:
      r = platform::http("GET", url("/api/cloud?action=get&key=" + urlEncode(j.key) + "&at=" + num(j.at)), j.pin, {}, TIMEOUT_MS);
      res.ok = r.status == 200 && !isJson(r) && !r.body.empty();
      if (res.ok) res.data = std::move(r.body); else fillError(r, res);
      return res;
  }
  return res;
}

void Client::ping() { SDL_LockMutex(mutex_); jobs_.push_back(Job(Result::Ping)); SDL_SignalCondition(cond_); SDL_UnlockMutex(mutex_); }

void Client::auth(const std::string& p) {
  Job j(Result::Auth);
  j.pin = p;
  SDL_LockMutex(mutex_); jobs_.push_back(std::move(j)); SDL_SignalCondition(cond_); SDL_UnlockMutex(mutex_);
}

void Client::list(const std::string& tag) {
  Job j(Result::List);
  j.tag = tag;
  SDL_LockMutex(mutex_); jobs_.push_back(std::move(j)); SDL_SignalCondition(cond_); SDL_UnlockMutex(mutex_);
}

void Client::put(const std::string& key, double at, std::vector<uint8_t> data, const std::string& from, const std::string& name,
                 const std::string& console) {
  Job j(Result::Put);
  j.key = key; j.at = at; j.data = std::move(data); j.from = from; j.name = name; j.console = console;
  SDL_LockMutex(mutex_); jobs_.push_back(std::move(j)); SDL_SignalCondition(cond_); SDL_UnlockMutex(mutex_);
}

void Client::get(const std::string& key, double at) {
  Job j(Result::Get);
  j.key = key; j.at = at;
  SDL_LockMutex(mutex_); jobs_.push_back(std::move(j)); SDL_SignalCondition(cond_); SDL_UnlockMutex(mutex_);
}

bool Client::poll(Result& out) {
  SDL_LockMutex(mutex_);
  const bool has = !results_.empty();
  if (has) { out = std::move(results_.front()); results_.pop_front(); }
  SDL_UnlockMutex(mutex_);
  return has;
}

bool Client::busy() {
  SDL_LockMutex(mutex_);
  const bool b = !jobs_.empty() || running_ > 0;
  SDL_UnlockMutex(mutex_);
  return b;
}

bool Client::latest(const std::string& key, RemoteSave& out, bool& pinError, int timeoutMs) {
  pinError = false;
  const auto r = platform::http("GET", url("/api/cloud?action=versions&key=" + urlEncode(key)), pin(), {}, timeoutMs);
  if (r.status == 401) { pinError = true; return false; }
  backup::Json js;
  if (r.status != 200 || !backup::parseJson(std::string(r.body.begin(), r.body.end()), js)) return false;
  const backup::Json* v = js.get("versions");
  if (!v || v->type != backup::Json::Arr || v->a.empty()) return false;
  const auto& top = v->a[0];
  out.key = key; out.at = top.num("at"); out.from = top.str("from"); out.size = static_cast<size_t>(top.num("size"));
  return out.at > 0;
}

bool Client::fetch(const std::string& key, double at, std::vector<uint8_t>& out, int timeoutMs) {
  auto r = platform::http("GET", url("/api/cloud?action=get&key=" + urlEncode(key) + "&at=" + num(at)), pin(), {}, timeoutMs);
  if (r.status != 200 || isJson(r) || r.body.empty()) return false;
  out = std::move(r.body);
  return true;
}

}  // namespace indigo::cloud
