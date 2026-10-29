#include "Network.h"

#include "Util.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <netlistmgr.h>
#include <objbase.h>

#include <map>
#include <vector>

namespace localsend {

namespace {

constexpr unsigned long kWifiType = 71; // IF_TYPE_IEEE80211

std::string guidText(const GUID& guid)
{
    wchar_t text[64];
    ::StringFromGUID2(guid, text, 64);
    return lower(narrow(text));
}

struct NetworkFacts {
    Interface::Category category = Interface::Category::Unknown;
    std::string id;
    std::string name;
};

// Adapter GUID (lower case, braces) -> its network, from the Network List Manager.
std::map<std::string, NetworkFacts> networks()
{
    std::map<std::string, NetworkFacts> out;
    INetworkListManager* manager = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))))
        return out;
    IEnumNetworkConnections* connections = nullptr;
    if (SUCCEEDED(manager->GetNetworkConnections(&connections))) {
        INetworkConnection* connection = nullptr;
        ULONG got = 0;
        while (connections->Next(1, &connection, &got) == S_OK && got) {
            GUID adapter {};
            INetwork* network = nullptr;
            if (SUCCEEDED(connection->GetAdapterId(&adapter)) && SUCCEEDED(connection->GetNetwork(&network))) {
                NetworkFacts facts;
                NLM_NETWORK_CATEGORY category {};
                if (SUCCEEDED(network->GetCategory(&category))) {
                    facts.category = category == NLM_NETWORK_CATEGORY_PUBLIC ? Interface::Category::Public
                        : category == NLM_NETWORK_CATEGORY_PRIVATE           ? Interface::Category::Private
                                                                              : Interface::Category::Domain;
                }
                GUID id {};
                if (SUCCEEDED(network->GetNetworkId(&id)))
                    facts.id = guidText(id);
                BSTR name = nullptr;
                if (SUCCEEDED(network->GetName(&name)) && name)
                    facts.name = narrow(name);
                ::SysFreeString(name);
                out[guidText(adapter)] = facts;
                network->Release();
            }
            connection->Release();
        }
        connections->Release();
    }
    manager->Release();
    return out;
}

} // namespace

bool linkLocal(std::uint32_t address) noexcept
{
    return (address >> 16) == 0xA9FE;
}

bool hotspot(const Interface& i) noexcept
{
    // Windows' mobile hotspot runs on a Wi-Fi Direct virtual adapter (no
    // hardware flag); without a hotspot it has a link-local address only.
    return !i.hardware && !i.loopback && i.type == kWifiType && i.address != 0 && !linkLocal(i.address);
}

bool usable(const Interface& i) noexcept
{
    if (i.loopback || i.address == 0 || linkLocal(i.address) || (i.address >> 24) == 127)
        return false;
    return i.hardware || hotspot(i);
}

bool receiveAllowed(const Interface& i, ReceiveNetworks networks, const std::set<std::string>& allowedNetworks)
{
    if (!usable(i))
        return false;
    if (networks == ReceiveNetworks::All || hotspot(i))
        return true;
    if (i.category == Interface::Category::Private || i.category == Interface::Category::Domain)
        return true;
    return !i.networkId.empty() && allowedNetworks.count(i.networkId) > 0;
}

bool sameSubnet(std::uint32_t a, std::uint32_t b, int prefix) noexcept
{
    if (prefix <= 0)
        return true;
    if (prefix >= 32)
        return a == b;
    const std::uint32_t mask = ~std::uint32_t {0} << (32 - prefix);
    return (a & mask) == (b & mask);
}

std::uint32_t parseIpv4(const std::string& text)
{
    in_addr a {};
    if (::inet_pton(AF_INET, text.c_str(), &a) != 1)
        return 0;
    return ntohl(a.s_addr);
}

std::string ipv4Text(std::uint32_t address)
{
    in_addr a {};
    a.s_addr = htonl(address);
    char text[INET_ADDRSTRLEN] {};
    ::inet_ntop(AF_INET, &a, text, sizeof text);
    return text;
}

std::vector<Interface> currentInterfaces()
{
    std::vector<Interface> out;
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buffer;
    IP_ADAPTER_ADDRESSES* list = nullptr;
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    for (int tries = 0; tries < 4; ++tries) {
        buffer.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const ULONG result = ::GetAdaptersAddresses(AF_INET, kFlags, nullptr, list, &size);
        if (result == NO_ERROR)
            break;
        list = nullptr;
        if (result != ERROR_BUFFER_OVERFLOW)
            return out;
    }
    if (!list)
        return out;
    const std::map<std::string, NetworkFacts> facts = networks();
    for (const IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp)
            continue;
        MIB_IF_ROW2 row {};
        row.InterfaceLuid = a->Luid;
        const bool haveRow = ::GetIfEntry2(&row) == NO_ERROR;
        for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            Interface i;
            i.index = a->IfIndex;
            i.name = narrow(a->FriendlyName ? a->FriendlyName : L"");
            i.type = a->IfType;
            i.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            i.hardware = haveRow && row.InterfaceAndOperStatusFlags.HardwareInterface;
            i.address = ntohl(reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            i.ip = ipv4Text(i.address);
            i.prefix = u->OnLinkPrefixLength;
            if (const auto it = facts.find(lower(a->AdapterName ? a->AdapterName : "")); it != facts.end()) {
                i.category = it->second.category;
                i.networkId = it->second.id;
                i.networkName = it->second.name;
            }
            out.push_back(std::move(i));
        }
    }
    return out;
}

} // namespace localsend
