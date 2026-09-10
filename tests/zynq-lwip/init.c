/*
 * Bring lwip up on the Zynq A9's Cadence GEM and prove a packet moved.
 *
 * "The stack initialised" is not the claim worth making.  This project has
 * already been caught once by an emulated device that answered plausibly while
 * doing nothing at all, so the test here is a TCP connection accepted from
 * outside the guest and bytes exchanged over it -- which cannot happen unless
 * the GEM, the driver, lwip and QEMU's user-mode network all work.
 *
 * QEMU's user-mode networking puts the guest at 10.0.2.15 behind a NAT, with
 * the gateway at 10.0.2.2, and forwards a host port in with hostfwd.  No DHCP
 * server is needed for a static address, and using one would test QEMU's DHCP
 * rather than anything here.
 */

#include <netstart.h>

#include <lwip/sockets.h>
#include <lwip/netif.h>

#include <rtems.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LISTEN_PORT 5555

static int failures;

static void check(const char *what, bool ok)
{
  printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static struct netif net_interface;

/* Locally administered, so it cannot collide with a real card. */
static unsigned char mac_address[6] = { 0x02, 0x52, 0x54, 0x00, 0x12, 0x34 };

static rtems_task Init(rtems_task_argument arg)
{
  ip_addr_t ipaddr, netmask, gateway;
  int listener;
  int conn;
  struct sockaddr_in addr;
  int rv;

  (void) arg;

  printf("\n*** ZYNQ LWIP TEST ***\n");

  /* IP_ADDR4, not IP4_ADDR: LWIP_IPV6 is on in this build, so ip_addr_t is
   * the dual-stack union and the address needs its type tag set as well as
   * its bytes. */
  IP_ADDR4(&ipaddr, 10, 0, 2, 15);
  IP_ADDR4(&netmask, 255, 255, 255, 0);
  IP_ADDR4(&gateway, 10, 0, 2, 2);

  rv = start_networking(
    &net_interface,
    &ipaddr,
    &netmask,
    &gateway,
    mac_address
  );
  check("start_networking", rv == 0);

  if (rv != 0) {
    goto done;
  }

  check("the interface is up", netif_is_up(&net_interface));
  check("it has the address it was given",
        ip4_addr_get_u32(netif_ip4_addr(&net_interface))
          == ip4_addr_get_u32(ip_2_ip4(&ipaddr)));

  listener = socket(AF_INET, SOCK_STREAM, 0);
  check("socket()", listener >= 0);
  if (listener < 0) {
    goto done;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LISTEN_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  check("bind()", bind(listener, (struct sockaddr *) &addr, sizeof(addr)) == 0);
  check("listen()", listen(listener, 1) == 0);

  printf("listening on %d, waiting for the host\n", LISTEN_PORT);

  /* Blocking accept.  If nothing connects the run times out, which is the
   * correct outcome for a stack that came up but cannot be reached. */
  conn = accept(listener, NULL, NULL);
  check("a connection arrived from outside the guest", conn >= 0);

  if (conn >= 0) {
    char buf[64];
    ssize_t n;

    memset(buf, 0, sizeof(buf));
    n = recv(conn, buf, sizeof(buf) - 1, 0);
    check("bytes arrived over it", n > 0);
    check("they are what the host sent", n > 0 && strncmp(buf, "hello", 5) == 0);
    if (n > 0) {
      printf("       received '%.*s'\n", (int) n, buf);
    }

    /* Reply, so the host can confirm the path works in both directions --
     * a receive alone would pass with a transmit path that never worked. */
    n = send(conn, "RTEMS", 5, 0);
    check("a reply could be sent", n == 5);

    close(conn);
  }

  close(listener);

done:
  printf("\n%d failure(s)\n", failures);
  if (failures == 0) {
    printf("CI-MARKER net ok\n");
  }
  printf("*** END OF ZYNQ LWIP TEST ***\n");

  exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_FILESYSTEM_IMFS
#define CONFIGURE_MAXIMUM_TASKS 16
#define CONFIGURE_MAXIMUM_SEMAPHORES 32
#define CONFIGURE_MAXIMUM_MESSAGE_QUEUES 16
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 32
#define CONFIGURE_MAXIMUM_POSIX_THREADS 16
#define CONFIGURE_MAXIMUM_DRIVERS 8
#define CONFIGURE_UNLIMITED_OBJECTS
#define CONFIGURE_UNIFIED_WORK_AREAS
#define CONFIGURE_INIT_TASK_STACK_SIZE (16 * 1024)
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
