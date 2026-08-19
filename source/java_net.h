/* java_net.h -- java.net.URL / URLConnection stubs.
 *
 * Registers just enough of java.net that a network request resolves and then
 * fails the way it does on a device with no signal, instead of openConnection
 * returning null and the managed null check killing the process. See the
 * comment at the top of java_net.c for the full derivation.
 */
#ifndef PVZU_JAVA_NET_H
#define PVZU_JAVA_NET_H

void java_net_init(void);

#endif /* PVZU_JAVA_NET_H */
