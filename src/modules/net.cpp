// Network diagnostics on native APIs (iphlpapi/ws2_32): adapters, gateway ping, DNS, TCP.
#include "../win/app.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace wr {

namespace {
std::string Ip4(const sockaddr* sa) {
    if (!sa || sa->sa_family != AF_INET) return "";
    char b[32];
    const sockaddr_in* in = (const sockaddr_in*)sa;
    InetNtopA(AF_INET, (void*)&in->sin_addr, b, sizeof b);
    return b;
}
}  // namespace

void ModNetwork(Ctx& c) {
    c.info("=== Сеть ===");
    Json rep = Json::object();
    rep.set("Timestamp", IsoNow());
    Section sec;
    sec.title = "Сеть";
    std::vector<std::string> issues;

    ULONG sz = 32768;
    std::vector<uint8_t> buf(sz);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(sz); rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz); }
    Json jad = Json::array();
    std::string gw;
    if (rc == NO_ERROR) {
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            bool up = a->OperStatus == IfOperStatusUp;
            std::string ip, dns, g;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) { std::string s = Ip4(u->Address.lpSockaddr); if (!s.empty()) ip += (ip.empty() ? "" : ", ") + s; }
            for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) { std::string s = Ip4(d->Address.lpSockaddr); if (!s.empty()) dns += (dns.empty() ? "" : ", ") + s; }
            for (auto* x = a->FirstGatewayAddress; x; x = x->Next) { std::string s = Ip4(x->Address.lpSockaddr); if (!s.empty()) { g = s; break; } }
            char mac[32] = "";
            if (a->PhysicalAddressLength == 6)
                snprintf(mac, sizeof mac, "%02X-%02X-%02X-%02X-%02X-%02X", a->PhysicalAddress[0], a->PhysicalAddress[1], a->PhysicalAddress[2], a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
            std::string name = U(a->FriendlyName), desc = U(a->Description);
            unsigned long long speed = a->TransmitLinkSpeed == (ULONG64)-1 ? 0 : a->TransmitLinkSpeed / 1000000ULL;
            c.info(fmt("%s (%s): %s, %llu Мбит/с, MAC %s, IP %s, шлюз %s, DNS %s", name.c_str(), desc.c_str(), up ? "Up" : "Down", speed, mac,
                       ip.empty() ? "-" : ip.c_str(), g.empty() ? "-" : g.c_str(), dns.empty() ? "-" : dns.c_str()));
            jad.add(Json::object().set("Name", name).set("Description", desc).set("Status", up ? "Up" : "Down").set("LinkSpeedMbps", speed)
                        .set("MacAddress", mac).set("IPv4", ip).set("Gateway", g).set("Dns", dns));
            if (gw.empty() && up && !g.empty()) gw = g;
        }
    } else c.warn("GetAdaptersAddresses: " + ErrMsg(rc));
    rep.set("Adapters", jad);
    if (jad.size() == 0) issues.push_back("Сетевые адаптеры не найдены (нет драйвера или не выполнен wpeinit).");

    // gateway ping
    if (!gw.empty()) {
        IPAddr addr = 0;
        InetPtonA(AF_INET, gw.c_str(), &addr);
        HANDLE ic = IcmpCreateFile();
        bool reach = false;
        if (ic != INVALID_HANDLE_VALUE) {
            char data[32] = "WinRepair";
            std::vector<uint8_t> rb(sizeof(ICMP_ECHO_REPLY) + sizeof data + 16);
            for (int i = 0; i < 2 && !reach; ++i) {
                DWORD n = IcmpSendEcho(ic, addr, data, sizeof data, nullptr, rb.data(), (DWORD)rb.size(), 1500);
                if (n > 0 && ((ICMP_ECHO_REPLY*)rb.data())->Status == IP_SUCCESS) reach = true;
            }
            IcmpCloseHandle(ic);
        }
        c.info(fmt("Шлюз %s: %s", gw.c_str(), reach ? "доступен" : "НЕдоступен"));
        rep.set("Gateway", Json::object().set("IP", gw).set("Reachable", reach));
        if (!reach) issues.push_back("Шлюз " + gw + " недостижим.");
    } else {
        issues.push_back("Нет default gateway.");
        rep.set("Gateway", Json());
    }
    if (c.cancelled()) return;

    // DNS + TCP
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int gr = getaddrinfo(c.opt.host.c_str(), nullptr, &hints, &res);
        Json jdns = Json::object();
        jdns.set("Target", c.opt.host);
        sockaddr_in target{};
        bool have = false;
        if (gr == 0 && res) {
            std::string ips;
            for (auto* p = res; p; p = p->ai_next) {
                std::string s = Ip4(p->ai_addr);
                if (!s.empty()) { ips += (ips.empty() ? "" : ", ") + s; if (!have) { target = *(sockaddr_in*)p->ai_addr; have = true; } }
            }
            c.info(fmt("DNS %s -> %s", c.opt.host.c_str(), ips.c_str()));
            jdns.set("Success", true).set("Records", ips);
            freeaddrinfo(res);
        } else {
            c.warn(fmt("DNS: %s не разрешается (код %d).", c.opt.host.c_str(), gr));
            jdns.set("Success", false).set("Error", fmt("getaddrinfo=%d", gr));
            issues.push_back("DNS: " + c.opt.host + " не разрешается.");
        }
        rep.set("Dns", jdns);

        Json jt = Json::object();
        jt.set("Host", c.opt.host).set("Port", c.opt.port);
        bool open = false;
        if (have) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                u_long nb = 1;
                ioctlsocket(s, FIONBIO, &nb);
                target.sin_port = htons((u_short)c.opt.port);
                connect(s, (sockaddr*)&target, sizeof target);
                fd_set wf, ef;
                FD_ZERO(&wf); FD_ZERO(&ef);
                FD_SET(s, &wf); FD_SET(s, &ef);
                timeval tv{5, 0};
                if (select(0, nullptr, &wf, &ef, &tv) > 0 && FD_ISSET(s, &wf)) open = true;
                closesocket(s);
            }
        }
        c.info(fmt("TCP %s:%d — %s", c.opt.host.c_str(), c.opt.port, open ? "открыт" : "недоступен"));
        jt.set("Open", open);
        rep.set("TcpCheck", jt);
        if (!open) issues.push_back(fmt("TCP %s:%d недоступен.", c.opt.host.c_str(), c.opt.port));
        WSACleanup();
    }
    Json ji = Json::array();
    for (const auto& s : issues) { ji.add(s); c.warn("Замечание: " + s); sec.rows.push_back({"Замечание", s}); }
    if (issues.empty()) { c.ok("Сеть: замечаний нет."); sec.rows.push_back({"Итог", "замечаний нет"}); }
    rep.set("Issues", ji);
    c.saveText("network.json", rep.dump());
    if (c.res) c.res->setSection(sec);
}

}  // namespace wr
