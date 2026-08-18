#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {
namespace wsd {

// WS-Discovery передаёт команды как SOAP/XML. Эти функции не зависят от
// Arduino и сети: так их можно проверять обычными тестами на ПК.
enum class RequestKind : uint8_t {
  kUnknown,
  kProbe,
  kResolve,
  kGet,
};

struct Identity {
  const char* uuid;
  const char* sequenceUuid;
  const char* hostname;
  const char* workgroup;
  const char* firmwareVersion;
  const char* ipAddress;
};

RequestKind classifyRequest(const char* xml, size_t length);
bool extractElementText(const char* xml, size_t length, const char* localName,
                        char* output, size_t outputSize);
bool containsEndpoint(const char* xml, size_t length, const char* uuid);

size_t buildHello(char* output, size_t capacity, const Identity& identity,
                  const char* messageId, uint32_t instanceId,
                  uint32_t messageNumber);
size_t buildBye(char* output, size_t capacity, const Identity& identity,
                const char* messageId, uint32_t instanceId,
                uint32_t messageNumber);
size_t buildProbeMatches(char* output, size_t capacity,
                         const Identity& identity, const char* messageId,
                         const char* relatesTo, uint32_t instanceId,
                         uint32_t messageNumber);
size_t buildResolveMatches(char* output, size_t capacity,
                           const Identity& identity, const char* messageId,
                           const char* relatesTo, uint32_t instanceId,
                           uint32_t messageNumber);
size_t buildGetResponse(char* output, size_t capacity,
                        const Identity& identity, const char* messageId,
                        const char* relatesTo);

}  // namespace wsd
}  // namespace zifi
