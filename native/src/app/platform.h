// Serviços que dependem do sistema (no Android, chamadas ao Java da IndigoActivity).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace indigo::platform {

// true no celular/tablet (controles na tela, menu por toque, biblioteca de jogos)
bool isMobile();

// teste no PC: com a variável INDIGO_MOBILE=LxA o app se comporta como no celular, numa janela LxA
bool simulatedPhone(int* w, int* h);

// nome do arquivo para mostrar e para achar o console pela extensão. No Android o seletor devolve
// "content://...", sem nome: ele é perguntado ao sistema.
std::string displayName(const std::string& path);

// Android: jogo pedido por um frontend (ES-DE, Daijisho, Beacon, "Abrir com" do gerenciador de arquivos)
// que ainda não foi aberto; vazio = nada. Cada pedido é entregue uma vez. Nas outras plataformas: sempre vazio
// (lá o jogo vem pela linha de comando).
std::string takePendingRom();

// vibração curta (botões da tela)
void haptic();

// requisição HTTP(S) simples (Windows: WinHTTP; Android: HttpURLConnection pela IndigoActivity).
// status 0 = sem conexão (a mensagem fica em error). Bloqueia: chamar fora da linha principal quando der.
struct HttpResult {
  int status = 0;
  std::string contentType, error;
  std::vector<uint8_t> body;
};
HttpResult http(const std::string& method, const std::string& url, const std::string& pin,
                const std::vector<uint8_t>& body, int timeoutMs);

// Windows: "abrir com o Índigo" para as extensões de jogos, só para o usuário (HKCU, sem pedir administrador).
// Entra na lista "Abrir com" de cada extensão; vira o padrão só onde nenhum outro programa é o padrão.
bool canAssociate();
bool filesAssociated();
bool associateFiles(bool on);

}  // namespace indigo::platform
