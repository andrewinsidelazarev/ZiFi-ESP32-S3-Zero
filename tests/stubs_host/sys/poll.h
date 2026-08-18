#pragma once

// Windows не имеет sys/poll.h, но WinSock предоставляет WSAPoll с той же
// семантикой. Симулятору этого достаточно: libsmb2 использует poll только для
// ожидания готовности сокетов.

#include <winsock2.h>

#ifndef POLLIN
#define POLLIN POLLRDNORM
#endif
#ifndef POLLOUT
#define POLLOUT POLLWRNORM
#endif

using nfds_t = unsigned long;

inline int poll(struct pollfd* fds, nfds_t count, int timeout) {
  return WSAPoll(fds, static_cast<ULONG>(count), timeout);
}
