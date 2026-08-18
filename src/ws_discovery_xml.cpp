#include "zifi/ws_discovery_xml.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace zifi {
namespace wsd {
namespace {

constexpr const char* kProbeAction =
    "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe";
constexpr const char* kResolveAction =
    "http://schemas.xmlsoap.org/ws/2005/04/discovery/Resolve";
constexpr const char* kGetAction =
    "http://schemas.xmlsoap.org/ws/2004/09/transfer/Get";

bool isNameEnd(char value) {
  return value == '>' || value == '/' ||
         isspace(static_cast<unsigned char>(value)) != 0;
}

bool sameText(const char* left, size_t leftLength, const char* right) {
  return right != nullptr && strlen(right) == leftLength &&
         memcmp(left, right, leftLength) == 0;
}

// Значения имени и рабочей группы попадают внутрь XML. Даже если будущий
// плагин разрешит символы '&' или '<', они не должны ломать SOAP-документ.
bool escapeXml(const char* input, char* output, size_t capacity) {
  if (input == nullptr || output == nullptr || capacity == 0) {
    return false;
  }
  size_t used = 0;
  while (*input != 0) {
    const char* replacement = nullptr;
    switch (*input) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '\"':
        replacement = "&quot;";
        break;
      case '\'':
        replacement = "&apos;";
        break;
      default:
        break;
    }
    const size_t count = replacement == nullptr ? 1 : strlen(replacement);
    if (used + count >= capacity) {
      output[0] = 0;
      return false;
    }
    if (replacement == nullptr) {
      output[used++] = *input;
    } else {
      memcpy(output + used, replacement, count);
      used += count;
    }
    ++input;
  }
  output[used] = 0;
  return true;
}

size_t checkedLength(char* output, size_t capacity, int written) {
  if (output == nullptr || capacity == 0 || written < 0 ||
      static_cast<size_t>(written) >= capacity) {
    if (output != nullptr && capacity != 0) {
      output[0] = 0;
    }
    return 0;
  }
  return static_cast<size_t>(written);
}

bool validIdentity(const Identity& identity) {
  return identity.uuid != nullptr && identity.sequenceUuid != nullptr &&
         identity.hostname != nullptr && identity.workgroup != nullptr &&
         identity.firmwareVersion != nullptr && identity.ipAddress != nullptr;
}

}  // namespace

bool extractElementText(const char* xml, size_t length, const char* localName,
                        char* output, size_t outputSize) {
  if (xml == nullptr || localName == nullptr || output == nullptr ||
      outputSize == 0) {
    return false;
  }
  output[0] = 0;
  const char* const end = xml + length;
  for (const char* cursor = xml; cursor < end; ++cursor) {
    if (*cursor != '<' || cursor + 1 == end || cursor[1] == '/' ||
        cursor[1] == '!' || cursor[1] == '?') {
      continue;
    }
    const char* name = cursor + 1;
    const char* local = name;
    const char* nameEnd = name;
    while (nameEnd < end && !isNameEnd(*nameEnd)) {
      if (*nameEnd == ':') {
        local = nameEnd + 1;
      }
      ++nameEnd;
    }
    if (!sameText(local, static_cast<size_t>(nameEnd - local), localName)) {
      continue;
    }
    const char* openingEnd = nameEnd;
    while (openingEnd < end && *openingEnd != '>') {
      ++openingEnd;
    }
    if (openingEnd == end) {
      return false;
    }
    const char* value = openingEnd + 1;
    const char* valueEnd = value;
    while (valueEnd < end && *valueEnd != '<') {
      ++valueEnd;
    }
    while (value < valueEnd &&
           isspace(static_cast<unsigned char>(*value)) != 0) {
      ++value;
    }
    while (valueEnd > value &&
           isspace(static_cast<unsigned char>(valueEnd[-1])) != 0) {
      --valueEnd;
    }
    const size_t valueLength = static_cast<size_t>(valueEnd - value);
    if (valueLength >= outputSize) {
      return false;
    }
    memcpy(output, value, valueLength);
    output[valueLength] = 0;
    return true;
  }
  return false;
}

RequestKind classifyRequest(const char* xml, size_t length) {
  char action[96] = {};
  if (!extractElementText(xml, length, "Action", action, sizeof(action))) {
    return RequestKind::kUnknown;
  }
  if (strcmp(action, kProbeAction) == 0) {
    return RequestKind::kProbe;
  }
  if (strcmp(action, kResolveAction) == 0) {
    return RequestKind::kResolve;
  }
  if (strcmp(action, kGetAction) == 0) {
    return RequestKind::kGet;
  }
  return RequestKind::kUnknown;
}

bool containsEndpoint(const char* xml, size_t length, const char* uuid) {
  if (xml == nullptr || uuid == nullptr) {
    return false;
  }
  char endpoint[64] = {};
  const int written = snprintf(endpoint, sizeof(endpoint), "urn:uuid:%s", uuid);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(endpoint)) {
    return false;
  }
  const size_t wanted = static_cast<size_t>(written);
  if (wanted > length) {
    return false;
  }
  for (size_t offset = 0; offset + wanted <= length; ++offset) {
    if (memcmp(xml + offset, endpoint, wanted) == 0) {
      return true;
    }
  }
  return false;
}

size_t buildHello(char* output, size_t capacity, const Identity& identity,
                  const char* messageId, uint32_t instanceId,
                  uint32_t messageNumber) {
  if (!validIdentity(identity) || messageId == nullptr) {
    return 0;
  }
  const int written = snprintf(
      output, capacity,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
      "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
      "xmlns:dp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\" "
      "xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
      "<s:Header><a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
      "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello</a:Action>"
      "<a:MessageID>%s</a:MessageID>"
      "<d:AppSequence InstanceId=\"%lu\" SequenceId=\"urn:uuid:%s\" "
      "MessageNumber=\"%lu\"/></s:Header>"
      "<s:Body><d:Hello><a:EndpointReference><a:Address>urn:uuid:%s"
      "</a:Address></a:EndpointReference><d:Types>dp:Device pub:Computer"
      "</d:Types><d:XAddrs>http://%s:5357/%s</d:XAddrs>"
      "<d:MetadataVersion>2</d:MetadataVersion></d:Hello></s:Body>"
      "</s:Envelope>",
      messageId, static_cast<unsigned long>(instanceId),
      identity.sequenceUuid, static_cast<unsigned long>(messageNumber),
      identity.uuid, identity.ipAddress, identity.uuid);
  return checkedLength(output, capacity, written);
}

size_t buildBye(char* output, size_t capacity, const Identity& identity,
                const char* messageId, uint32_t instanceId,
                uint32_t messageNumber) {
  if (!validIdentity(identity) || messageId == nullptr) {
    return 0;
  }
  const int written = snprintf(
      output, capacity,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
      "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
      "<s:Header><a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
      "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Bye</a:Action>"
      "<a:MessageID>%s</a:MessageID>"
      "<d:AppSequence InstanceId=\"%lu\" SequenceId=\"urn:uuid:%s\" "
      "MessageNumber=\"%lu\"/></s:Header>"
      "<s:Body><d:Bye><a:EndpointReference><a:Address>urn:uuid:%s"
      "</a:Address></a:EndpointReference></d:Bye></s:Body></s:Envelope>",
      messageId, static_cast<unsigned long>(instanceId),
      identity.sequenceUuid, static_cast<unsigned long>(messageNumber),
      identity.uuid);
  return checkedLength(output, capacity, written);
}

size_t buildProbeMatches(char* output, size_t capacity,
                         const Identity& identity, const char* messageId,
                         const char* relatesTo, uint32_t instanceId,
                         uint32_t messageNumber) {
  if (!validIdentity(identity) || messageId == nullptr || relatesTo == nullptr) {
    return 0;
  }
  const int written = snprintf(
      output, capacity,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
      "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
      "xmlns:dp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\" "
      "xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
      "<s:Header><a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
      "</a:To><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/"
      "ProbeMatches</a:Action><a:MessageID>%s</a:MessageID>"
      "<a:RelatesTo>%s</a:RelatesTo>"
      "<d:AppSequence InstanceId=\"%lu\" SequenceId=\"urn:uuid:%s\" "
      "MessageNumber=\"%lu\"/></s:Header>"
      "<s:Body><d:ProbeMatches><d:ProbeMatch><a:EndpointReference>"
      "<a:Address>urn:uuid:%s</a:Address></a:EndpointReference>"
      "<d:Types>dp:Device pub:Computer</d:Types>"
      "<d:XAddrs>http://%s:5357/%s</d:XAddrs>"
      "<d:MetadataVersion>2</d:MetadataVersion></d:ProbeMatch>"
      "</d:ProbeMatches></s:Body></s:Envelope>",
      messageId, relatesTo, static_cast<unsigned long>(instanceId),
      identity.sequenceUuid, static_cast<unsigned long>(messageNumber),
      identity.uuid, identity.ipAddress, identity.uuid);
  return checkedLength(output, capacity, written);
}

size_t buildResolveMatches(char* output, size_t capacity,
                           const Identity& identity, const char* messageId,
                           const char* relatesTo, uint32_t instanceId,
                           uint32_t messageNumber) {
  if (!validIdentity(identity) || messageId == nullptr || relatesTo == nullptr) {
    return 0;
  }
  const int written = snprintf(
      output, capacity,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
      "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
      "xmlns:dp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\" "
      "xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
      "<s:Header><a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
      "</a:To><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/"
      "ResolveMatches</a:Action><a:MessageID>%s</a:MessageID>"
      "<a:RelatesTo>%s</a:RelatesTo>"
      "<d:AppSequence InstanceId=\"%lu\" SequenceId=\"urn:uuid:%s\" "
      "MessageNumber=\"%lu\"/></s:Header>"
      "<s:Body><d:ResolveMatches><d:ResolveMatch><a:EndpointReference>"
      "<a:Address>urn:uuid:%s</a:Address></a:EndpointReference>"
      "<d:Types>dp:Device pub:Computer</d:Types>"
      "<d:XAddrs>http://%s:5357/%s</d:XAddrs>"
      "<d:MetadataVersion>2</d:MetadataVersion></d:ResolveMatch>"
      "</d:ResolveMatches></s:Body></s:Envelope>",
      messageId, relatesTo, static_cast<unsigned long>(instanceId),
      identity.sequenceUuid, static_cast<unsigned long>(messageNumber),
      identity.uuid, identity.ipAddress, identity.uuid);
  return checkedLength(output, capacity, written);
}

size_t buildGetResponse(char* output, size_t capacity,
                        const Identity& identity, const char* messageId,
                        const char* relatesTo) {
  if (!validIdentity(identity) || messageId == nullptr || relatesTo == nullptr) {
    return 0;
  }
  char hostname[128] = {};
  char workgroup[128] = {};
  char version[128] = {};
  if (!escapeXml(identity.hostname, hostname, sizeof(hostname)) ||
      !escapeXml(identity.workgroup, workgroup, sizeof(workgroup)) ||
      !escapeXml(identity.firmwareVersion, version, sizeof(version))) {
    return 0;
  }
  const int written = snprintf(
      output, capacity,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
      "xmlns:wsx=\"http://schemas.xmlsoap.org/ws/2004/09/mex\" "
      "xmlns:dp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\" "
      "xmlns:pnpx=\"http://schemas.microsoft.com/windows/pnpx/2005/10\" "
      "xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
      "<s:Header><a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
      "</a:To><a:Action>http://schemas.xmlsoap.org/ws/2004/09/transfer/"
      "GetResponse</a:Action><a:MessageID>%s</a:MessageID>"
      "<a:RelatesTo>%s</a:RelatesTo></s:Header><s:Body><wsx:Metadata>"
      "<wsx:MetadataSection Dialect=\"http://schemas.xmlsoap.org/ws/2006/02/"
      "devprof/ThisDevice\"><dp:ThisDevice><dp:FriendlyName>%s</dp:FriendlyName>"
      "<dp:FirmwareVersion>%s</dp:FirmwareVersion><dp:SerialNumber>%s"
      "</dp:SerialNumber></dp:ThisDevice></wsx:MetadataSection>"
      "<wsx:MetadataSection Dialect=\"http://schemas.xmlsoap.org/ws/2006/02/"
      "devprof/ThisModel\"><dp:ThisModel><dp:Manufacturer>ZiFi</dp:Manufacturer>"
      "<dp:ModelName>ZX Evolution SMB Server</dp:ModelName>"
      "<pnpx:DeviceCategory>Computers</pnpx:DeviceCategory></dp:ThisModel>"
      "</wsx:MetadataSection>"
      "<wsx:MetadataSection Dialect=\"http://schemas.xmlsoap.org/ws/2006/02/"
      "devprof/Relationship\"><dp:Relationship Type=\"http://schemas.xmlsoap.org/"
      "ws/2006/02/devprof/host\"><dp:Host><a:EndpointReference><a:Address>"
      "urn:uuid:%s</a:Address></a:EndpointReference><dp:Types>pub:Computer"
      "</dp:Types><dp:ServiceId>urn:uuid:%s</dp:ServiceId>"
      // В тексте MS-PBSD для рабочей группы показан обратный слеш, но
      // Function Discovery в актуальной Windows съедает его при разборе:
      // "ZX-Evo\\Workgroup:WORKGROUP" превращается в слитую строку
      // "ZX-EvoWorkgroup:WORKGROUP", после чего раздел «Сеть» отбрасывает
      // устройство. Совместимый с Проводником wsdd передаёт здесь прямой
      // слеш; тогда граница имени сохраняется и компьютер попадает в общий
      // каталог сетевых устройств.
      "<pub:Computer>%s/Workgroup:%s</pub:Computer></dp:Host>"
      "</dp:Relationship></wsx:MetadataSection></wsx:Metadata></s:Body>"
      "</s:Envelope>",
      messageId, relatesTo, hostname, version, identity.uuid, identity.uuid,
      identity.uuid, hostname, workgroup);
  return checkedLength(output, capacity, written);
}

}  // namespace wsd
}  // namespace zifi
