// Виртуальная точка IPv4 для нативного SMB-сервера ZiFi.
//
// Windows занимает 0.0.0.0:445, поэтому второй локальный SMB-сервер не может
// открыть этот порт даже на другом адресе обратной петли. Мост предоставляет
// перенаправителю Windows адрес 192.168.1.222:445 и переносит только этот поток
// к нативному серверу на 127.0.0.1:1445. Ответы преобразуются обратно, поэтому
// клиент видит обычную удалённую точку SMB и использует настоящий стек SMB.

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windivert.h>

namespace {

std::atomic<bool> running{true};
HANDLE divertHandle = INVALID_HANDLE_VALUE;

BOOL WINAPI stopHandler(DWORD) {
  running.store(false);
  if (divertHandle != INVALID_HANDLE_VALUE) {
    WinDivertShutdown(divertHandle, WINDIVERT_SHUTDOWN_BOTH);
  }
  return TRUE;
}

bool parsePort(const char* text, uint16_t& port) {
  if (text == nullptr || *text == 0) {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (*end != 0 || value == 0 || value > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(value);
  return true;
}

void tracePayload(FILE* trace, char direction, const WINDIVERT_TCPHDR* tcp,
                  const unsigned char* data, UINT dataLength) {
  if (trace == nullptr || data == nullptr || dataLength == 0) {
    return;
  }
  std::fprintf(trace, "%c seq=%lu ack=%lu len=%u\n", direction,
               static_cast<unsigned long>(ntohl(tcp->SeqNum)),
               static_cast<unsigned long>(ntohl(tcp->AckNum)), dataLength);
  for (UINT offset = 0; offset < dataLength; offset += 16) {
    std::fprintf(trace, "  %04x:", offset);
    const UINT lineEnd = (offset + 16 < dataLength) ? offset + 16 : dataLength;
    for (UINT index = offset; index < lineEnd; ++index) {
      std::fprintf(trace, " %02x", data[index]);
    }
    std::fputc('\n', trace);
  }
  std::fflush(trace);
}

}  // пространство имён

int main(int argc, char** argv) {
  const char* virtualIpText = argc >= 2 ? argv[1] : "192.168.1.222";
  const char* targetIpText = argc >= 5 ? argv[4] : "127.0.0.1";
  const char* clientAliasText = argc >= 6 ? argv[5] : "127.0.0.2";
  uint16_t targetPort = 1445;
  if (argc >= 3 && !parsePort(argv[2], targetPort)) {
    std::fprintf(stderr, "Invalid target port: %s\n", argv[2]);
    return 2;
  }
  FILE* trace = nullptr;
  if (argc >= 4) {
    trace = std::fopen(argv[3], "wb");
    if (trace == nullptr) {
      std::fprintf(stderr, "Cannot open trace file: %s\n", argv[3]);
      return 2;
    }
  }

  UINT32 virtualIpHost = 0;
  UINT32 targetIpHost = 0;
  UINT32 clientAliasHost = 0;
  if (!WinDivertHelperParseIPv4Address(virtualIpText, &virtualIpHost) ||
      !WinDivertHelperParseIPv4Address(targetIpText, &targetIpHost) ||
      !WinDivertHelperParseIPv4Address(clientAliasText, &clientAliasHost)) {
    std::fprintf(stderr, "Invalid IPv4 address: virtual=%s target=%s alias=%s\n",
                 virtualIpText, targetIpText, clientAliasText);
    return 2;
  }
  const UINT32 virtualIp = WinDivertHelperHtonl(virtualIpHost);
  const UINT32 targetIp = WinDivertHelperHtonl(targetIpHost);
  const UINT32 clientAlias = WinDivertHelperHtonl(clientAliasHost);

  char filter[512] = {};
  std::snprintf(
      filter, sizeof(filter),
      "outbound and ip and tcp and "
      "((ip.DstAddr == %s and tcp.DstPort == 445) or "
      "(ip.SrcAddr == %s and ip.DstAddr == %s and "
      "tcp.SrcPort == %u))",
      virtualIpText, targetIpText, clientAliasText,
      static_cast<unsigned>(targetPort));

  divertHandle = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 100, 0);
  if (divertHandle == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "WinDivertOpen failed: %lu\n", GetLastError());
    std::fprintf(stderr, "Run this bridge as Administrator.\n");
    return 1;
  }
  SetConsoleCtrlHandler(stopHandler, TRUE);

  std::printf("ZiFi virtual SMB endpoint: \\\\%s\\SD\n", virtualIpText);
  std::printf("Bridge: %s:445 <-> %s:%u (client alias %s)\n", virtualIpText,
              targetIpText, static_cast<unsigned>(targetPort),
              clientAliasText);
  std::fflush(stdout);

  std::uint64_t toServer = 0;
  std::uint64_t toClient = 0;
  UINT32 clientIp = 0;
  UINT32 clientIfIdx = 0;
  UINT32 clientSubIfIdx = 0;
  unsigned char packet[0xFFFF] = {};
  while (running.load()) {
    WINDIVERT_ADDRESS address = {};
    UINT packetLength = 0;
    if (!WinDivertRecv(divertHandle, packet, sizeof(packet), &packetLength,
                       &address)) {
      const DWORD error = GetLastError();
      if (!running.load() || error == ERROR_NO_DATA) {
        break;
      }
      std::fprintf(stderr, "WinDivertRecv failed: %lu\n", error);
      break;
    }

    PWINDIVERT_IPHDR ip = nullptr;
    PWINDIVERT_TCPHDR tcp = nullptr;
    void* payload = nullptr;
    UINT payloadLength = 0;
    if (!WinDivertHelperParsePacket(packet, packetLength, &ip, nullptr,
                                    nullptr, nullptr, nullptr, &tcp, nullptr,
                                    &payload, &payloadLength, nullptr, nullptr) ||
        ip == nullptr || tcp == nullptr) {
      continue;
    }

    if (ip->DstAddr == virtualIp && tcp->DstPort == htons(445)) {
      tracePayload(trace, 'C', tcp,
                   static_cast<const unsigned char*>(payload), payloadLength);
      clientIp = ip->SrcAddr;
      clientIfIdx = address.Network.IfIdx;
      clientSubIfIdx = address.Network.SubIfIdx;
      ip->SrcAddr = clientAlias;
      ip->DstAddr = targetIp;
      tcp->DstPort = htons(targetPort);
      ++toServer;
    } else if (ip->SrcAddr == targetIp && ip->DstAddr == clientAlias &&
               tcp->SrcPort == htons(targetPort) && clientIp != 0 &&
               clientIfIdx != 0) {
      tracePayload(trace, 'S', tcp,
                   static_cast<const unsigned char*>(payload), payloadLength);
      ip->SrcAddr = virtualIp;
      ip->DstAddr = clientIp;
      tcp->SrcPort = htons(445);
      // Ответ должен войти в TCP-стек как пакет с исходного сетевого
      // интерфейса. Повторная отправка его по outbound-пути формально является
      // недокументированной инъекцией inbound-пакета и на многосегментном
      // составном SMB-ответе теряла один из сегментов.
      address.Outbound = 0;
      address.Loopback = 0;
      address.Network.IfIdx = clientIfIdx;
      address.Network.SubIfIdx = clientSubIfIdx;
      ++toClient;
    } else {
      continue;
    }

    WinDivertHelperCalcChecksums(packet, packetLength, &address, 0);
    UINT sentLength = 0;
    if (!WinDivertSend(divertHandle, packet, packetLength, &sentLength,
                       &address) ||
        sentLength != packetLength) {
      std::fprintf(stderr, "WinDivertSend failed: %lu\n", GetLastError());
      break;
    }
  }

  if (divertHandle != INVALID_HANDLE_VALUE) {
    WinDivertClose(divertHandle);
    divertHandle = INVALID_HANDLE_VALUE;
  }
  if (trace != nullptr) {
    std::fclose(trace);
  }
  std::printf("Bridge stopped: to-server=%llu to-client=%llu\n",
              static_cast<unsigned long long>(toServer),
              static_cast<unsigned long long>(toClient));
  return 0;
}
