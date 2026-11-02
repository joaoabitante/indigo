// Serialização simples para save states: o mesmo método sync() serve para gravar e para ler,
// então a ordem dos campos nunca fica diferente entre os dois lados.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace indigo {

class StateIO {
public:
  // gravação
  StateIO() : reading_(false) {}
  // leitura
  explicit StateIO(const std::vector<uint8_t>& src) : reading_(true), src_(&src) {}

  bool reading() const { return reading_; }
  bool atEnd() const { return reading_ && pos_ >= src_->size(); }
  const std::vector<uint8_t>& data() const { return out_; }

  // bool vem de um byte do arquivo: qualquer valor diferente de 0 vira true (um byte inválido num bool é
  // comportamento indefinido em C++)
  void sync(bool& v) {
    uint8_t b = v ? 1 : 0;
    raw(&b, 1);
    if (reading_) v = b != 0;
  }

  template <typename T>
  void sync(T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "sync: tipo precisa ser copiável byte a byte");
    raw(&v, sizeof(T));
  }

  void raw(void* p, size_t n) {
    if (reading_) {
      if (pos_ + n > src_->size()) throw std::runtime_error("Estado salvo truncado.");
      std::memcpy(p, src_->data() + pos_, n);
      pos_ += n;
    } else {
      const auto* b = static_cast<const uint8_t*>(p);
      out_.insert(out_.end(), b, b + n);
    }
  }

  // bloco de bytes com tamanho conhecido dos dois lados (memórias)
  void bytes(std::vector<uint8_t>& v) {
    uint32_t n = static_cast<uint32_t>(v.size());
    sync(n);
    if (reading_ && n != v.size()) throw std::runtime_error("Estado salvo incompatível com este jogo.");
    if (n) raw(v.data(), n);
  }

private:
  bool reading_;
  const std::vector<uint8_t>* src_ = nullptr;
  size_t pos_ = 0;
  std::vector<uint8_t> out_;
};

// Limites aplicados depois de carregar um estado: um arquivo danificado não pode levar a acesso fora da
// memória nem a laços sem fim. Os limites contêm todos os valores que o hardware pode ter de verdade, então
// um estado válido carrega exatamente igual.
template <typename T>
inline void clampTo(T& v, T lo, T hi) { if (v < lo) v = lo; else if (v > hi) v = hi; }
inline void finiteOr(double& v, double fallback) { if (!(v == v) || v > 1e300 || v < -1e300) v = fallback; }

}  // namespace indigo
