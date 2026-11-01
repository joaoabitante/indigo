// Saves na nuvem do Índigo: a mesma API do site (api/cloud.js, protegida pelo PIN do cabeçalho x-indigo-pin).
// A rede roda numa linha de execução própria; o app pede tarefas e recolhe os resultados a cada quadro (poll),
// então a tela nunca trava esperando a internet.
#pragma once
#include <SDL3/SDL.h>

#include <deque>
#include <string>
#include <vector>

namespace indigo::cloud {

struct RemoteSave {
  std::string key, from, name, console;
  double at = 0;
  size_t size = 0;
};

struct Result {
  enum Kind { Ping, Auth, List, Put, Get } kind = Ping;
  bool ok = false;
  bool pinError = false;     // o servidor pediu ou recusou o PIN
  bool storage = false;      // Ping: a nuvem está configurada no site
  std::string error, key;
  double at = 0;
  std::vector<RemoteSave> list;
  std::vector<uint8_t> data;
  std::string tag;           // texto livre de quem pediu (ex.: o motivo da sincronização)
};

class Client {
public:
  explicit Client(std::string baseUrl);
  ~Client();
  void setPin(const std::string& pin);
  std::string pin() const;

  void ping();
  void auth(const std::string& pin);
  void list(const std::string& tag);
  void put(const std::string& key, double at, std::vector<uint8_t> data, const std::string& from, const std::string& name,
           const std::string& console);
  void get(const std::string& key, double at);
  bool poll(Result& out);
  bool busy();

  // chamadas diretas (bloqueiam até timeoutMs): usadas ao abrir um jogo, antes de ler o save
  bool latest(const std::string& key, RemoteSave& out, bool& pinError, int timeoutMs);
  bool fetch(const std::string& key, double at, std::vector<uint8_t>& out, int timeoutMs);

private:
  struct Job {
    explicit Job(Result::Kind k) : kind(k) {}
    Result::Kind kind;
    std::string key, from, name, console, tag, pin;
    double at = 0;
    std::vector<uint8_t> data;
  };
  static int threadMain(void* self);
  void run();
  Result execute(const Job& j);
  std::string url(const std::string& path) const { return base_ + path; }

  std::string base_, pin_;
  SDL_Mutex* mutex_ = nullptr;
  SDL_Condition* cond_ = nullptr;
  SDL_Thread* thread_ = nullptr;
  std::deque<Job> jobs_;
  std::deque<Result> results_;
  bool quit_ = false;
  int running_ = 0;
};

// texto de parâmetro de URL (%XX)
std::string urlEncode(const std::string& s);

}  // namespace indigo::cloud
