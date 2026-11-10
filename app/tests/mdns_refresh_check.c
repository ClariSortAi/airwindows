/* Network-change check for src/mdnsd_win.c.
 *
 * The responder is compiled into this file with its adapter list, change
 * notification and bind address replaced. A scripted adapter on the loopback
 * interface then appears, changes address and disappears, and the check asks
 * the responder for the AirPlay service after each step. No real adapter,
 * address or firewall rule is touched, and nothing leaves the PC.
 */
#include "compat.h"
#include "mdnsd.h"

#define CHECK_INSTANCE "AirWindowsRefreshCheck"
#define CHECK_PORT 7123

static volatile LONG g_adapter_present;
static volatile LONG g_adapter_addr; /* network byte order */
static DWORD g_loopback_index;
static PUNICAST_IPADDRESS_CHANGE_CALLBACK g_change_callback;
static PVOID g_change_context;
static volatile LONG g_change_cancelled;

static ULONG WINAPI ScriptedAdapters(ULONG family, ULONG flags, PVOID reserved,
                                     PIP_ADAPTER_ADDRESSES out, PULONG size)
{
    /* The caller owns `out`; the address records hang off it in the same buffer. */
    const ULONG needed = sizeof(IP_ADAPTER_ADDRESSES) + sizeof(IP_ADAPTER_UNICAST_ADDRESS) +
                         sizeof(struct sockaddr_in) + 32 * sizeof(wchar_t);
    IP_ADAPTER_UNICAST_ADDRESS *unicast;
    struct sockaddr_in *address;
    wchar_t *description;
    (void) family; (void) flags; (void) reserved;
    if (!g_adapter_present) {
        return ERROR_NO_DATA;
    }
    if (*size < needed) {
        *size = needed;
        return ERROR_BUFFER_OVERFLOW;
    }
    memset(out, 0, needed);
    unicast = (IP_ADAPTER_UNICAST_ADDRESS *) (out + 1);
    address = (struct sockaddr_in *) (unicast + 1);
    description = (wchar_t *) (address + 1);
    wcscpy(description, L"Scripted test adapter");
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = (ULONG) g_adapter_addr;
    unicast->Address.lpSockaddr = (LPSOCKADDR) address;
    unicast->Address.iSockaddrLength = sizeof(*address);
    out->IfType = IF_TYPE_ETHERNET_CSMACD;
    out->OperStatus = IfOperStatusUp;
    out->IfIndex = g_loopback_index;
    out->Description = description;
    out->FirstUnicastAddress = unicast;
    return NO_ERROR;
}

static NETIO_STATUS NETIOAPI_API_ ScriptedNotify(ADDRESS_FAMILY family,
                                                 PUNICAST_IPADDRESS_CHANGE_CALLBACK callback,
                                                 PVOID context, BOOLEAN initial, HANDLE *handle)
{
    (void) family; (void) initial;
    g_change_callback = callback;
    g_change_context = context;
    *handle = (HANDLE) (INT_PTR) 1;
    return NO_ERROR;
}

static NETIO_STATUS NETIOAPI_API_ ScriptedCancel(HANDLE handle)
{
    (void) handle;
    InterlockedExchange(&g_change_cancelled, 1);
    return NO_ERROR;
}

/* Listening on every address would raise a Windows Firewall prompt for this
 * test program; the loopback address needs no rule. */
static int LoopbackBind(SOCKET s, const struct sockaddr *name, int length)
{
    struct sockaddr_in local;
    if (name->sa_family != AF_INET || length < (int) sizeof(local)) {
        return bind(s, name, length);
    }
    memcpy(&local, name, sizeof(local));
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return bind(s, (const struct sockaddr *) &local, sizeof(local));
}

#define GetAdaptersAddresses ScriptedAdapters
#define NotifyUnicastIpAddressChange ScriptedNotify
#define CancelMibChangeNotify2 ScriptedCancel
#define bind LoopbackBind
#define MDNS_INTERFACE_POLL_MS 150
#define MDNS_CHANGE_SETTLE_MS 100
#include "../src/mdnsd_win.c"

static void Require(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static int Contains(const unsigned char *bytes, int length, const void *needle, int needle_length)
{
    for (int i = 0; i + needle_length <= length; i++) {
        if (!memcmp(bytes + i, needle, (size_t) needle_length)) {
            return 1;
        }
    }
    return 0;
}

/* True for a response that names the check's service and carries `addr` as its A record. */
static int IsOurAnswer(const unsigned char *bytes, int length, uint32_t addr)
{
    unsigned char record[6] = {0, 4};
    memcpy(record + 2, &addr, 4);
    return length > 12 && (bytes[2] & 0x80) &&
           Contains(bytes, length, CHECK_INSTANCE, (int) strlen(CHECK_INSTANCE)) &&
           Contains(bytes, length, record, sizeof(record));
}

static SOCKET OpenSocket(unsigned short port, int join)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    DWORD timeout = 50;
    struct sockaddr_in local;
    struct in_addr via;
    Require(s != INVALID_SOCKET, "socket");
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    Require(bind(s, (struct sockaddr *) &local, sizeof(local)) == 0, "bind to loopback");
    via.s_addr = htonl(INADDR_LOOPBACK);
    Require(setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *) &via, sizeof(via)) == 0, "multicast interface");
    if (join) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        inet_pton(AF_INET, MDNS_ADDR4, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(g_loopback_index);
        Require(setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &mreq, sizeof(mreq)) == 0, "listener join");
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &timeout, sizeof(timeout));
    return s;
}

/* Waits up to `wait_ms` for a response carrying `addr`. */
static int Await(SOCKET s, DWORD wait_ms, uint32_t addr)
{
    const ULONGLONG deadline = GetTickCount64() + wait_ms;
    while (GetTickCount64() < deadline) {
        unsigned char buffer[MDNS_MAX_PACKET];
        const int received = recv(s, (char *) buffer, sizeof(buffer), 0);
        if (received > 0 && IsOurAnswer(buffer, received, addr)) {
            return 1;
        }
    }
    return 0;
}

static void Drain(SOCKET s)
{
    unsigned char buffer[MDNS_MAX_PACKET];
    while (recv(s, (char *) buffer, sizeof(buffer), 0) > 0) {
    }
}

/* Asks "who offers AirPlay?" the way an iPhone does, repeating until answered. */
static int Ask(SOCKET s, DWORD wait_ms, uint32_t addr)
{
    static const unsigned char question[] = {
        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        8, '_', 'a', 'i', 'r', 'p', 'l', 'a', 'y', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, DNS_TYPE_PTR, 0x80, DNS_CLASS_IN};
    const ULONGLONG deadline = GetTickCount64() + wait_ms;
    struct sockaddr_in group;
    memset(&group, 0, sizeof(group));
    group.sin_family = AF_INET;
    group.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_ADDR4, &group.sin_addr);
    do {
        sendto(s, (const char *) question, sizeof(question), 0, (struct sockaddr *) &group, sizeof(group));
        if (Await(s, 150, addr)) {
            return 1;
        }
    } while (GetTickCount64() < deadline);
    return 0;
}

int main(void)
{
    WSADATA wsa;
    mdnsd_t *mdnsd;
    mdnsd_service_t airplay;
    SOCKET listener, asker;
    uint32_t first, second;

    Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup");
    inet_pton(AF_INET, "127.0.0.1", &first);
    inet_pton(AF_INET, "127.0.0.2", &second);
    Require(GetBestInterface(first, &g_loopback_index) == NO_ERROR && g_loopback_index, "loopback interface index");

    listener = OpenSocket(MDNS_PORT, 1);
    asker = OpenSocket(0, 0);

    /* Launching before the network is up must still start the responder. */
    mdnsd = mdnsd_init("refresh-check.local");
    Require(mdnsd != NULL, "mdnsd_init");
    Require(mdnsd_start(mdnsd) == 0, "responder starts with no network");
    Require(g_change_callback != NULL, "address-change notification registered");
    memset(&airplay, 0, sizeof(airplay));
    snprintf(airplay.type, sizeof(airplay.type), "%s", "_airplay._tcp.local");
    snprintf(airplay.name, sizeof(airplay.name), "%s", CHECK_INSTANCE "._airplay._tcp.local");
    airplay.port = CHECK_PORT;
    airplay.registered = 1;
    Require(mdnsd_txt_add(&airplay.txt, "deviceid", "02:00:00:00:00:01") == 0, "txt record");
    mdnsd_set_services(mdnsd, &airplay, NULL);
    Require(!Ask(asker, 500, first), "no answer while no adapter is present");

    /* The network arrives: announce on it without being asked, then answer. */
    Drain(listener);
    InterlockedExchange(&g_adapter_addr, (LONG) first);
    InterlockedExchange(&g_adapter_present, 1);
    Require(Await(listener, 3000, first), "announcement after an adapter appears");
    Require(Ask(asker, 3000, first), "answer after an adapter appears");

    /* Same adapter, new address (a DHCP change): answers must carry the new one. */
    InterlockedExchange(&g_adapter_addr, (LONG) second);
    Require(Ask(asker, 3000, second), "answer carries the changed address");
    InterlockedExchange(&g_adapter_addr, (LONG) first);
    Require(Ask(asker, 3000, first), "answer follows the address back");

    /* An address-change event with an identical list still rejoins and announces. */
    Sleep(400);
    Drain(listener);
    g_change_callback(g_change_context, NULL, MibParameterNotification);
    Require(Await(listener, 3000, first), "announcement after a change event");

    /* The network goes away: stop answering for it. */
    InterlockedExchange(&g_adapter_present, 0);
    Sleep(600);
    Require(!Ask(asker, 500, first), "no answer after the adapter disappears");

    /* It comes back: the earlier leave must not block the new join. */
    InterlockedExchange(&g_adapter_present, 1);
    Require(Ask(asker, 3000, first), "answer after the adapter returns");

    mdnsd_destroy(mdnsd);
    Require(g_change_cancelled, "address-change notification cancelled on stop");
    closesocket(listener);
    closesocket(asker);
    WSACleanup();
    printf("mDNS network-change checks passed: late network, address change, change event, loss and return.\n");
    return 0;
}
