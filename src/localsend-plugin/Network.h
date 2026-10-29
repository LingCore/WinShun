#pragma once

// Which network adapters LocalSend runs on, and whether files may be
// received there. Usable: up, an IPv4 address that is not link-local, and
// a hardware adapter (Ethernet, Wi-Fi, USB tethering) or Windows' mobile
// hotspot (a Wi-Fi Direct virtual adapter with a routable address; phones
// joined to the PC's hotspot are a common case). Not usable: Hyper-V and
// WSL switches, VPN and proxy tunnels (TUN/Wintun: FlClash and the like),
// loopback. Receiving, by default, only where Windows calls the network
// private or domain, on the hotspot, or on a network the user allowed
// (by its NLM id); "公用" networks need that one click.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace localsend {

struct Interface {
    enum class Category { Unknown, Public, Private, Domain };
    unsigned long index = 0; // IfIndex
    std::string name; // "以太网", "WLAN"
    std::string ip; // "192.168.1.20"
    std::uint32_t address = 0; // host order
    int prefix = 24;
    unsigned long type = 0; // IF_TYPE_*
    bool hardware = false;
    bool loopback = false;
    Category category = Category::Unknown;
    std::string networkId; // NLM's GUID for the network; "" when unknown
    std::string networkName; // "网络 3", the Wi-Fi's name
};

enum class ReceiveNetworks { Trusted, All };

bool linkLocal(std::uint32_t address) noexcept; // 169.254/16
bool hotspot(const Interface& i) noexcept;
bool usable(const Interface& i) noexcept;
// Files may be received on it (see the top).
bool receiveAllowed(const Interface& i, ReceiveNetworks networks, const std::set<std::string>& allowedNetworks);
bool sameSubnet(std::uint32_t a, std::uint32_t b, int prefix) noexcept;
std::uint32_t parseIpv4(const std::string& text); // 0 when it is not one
std::string ipv4Text(std::uint32_t address);

// Up, with IPv4, as Windows has them now; categories and network ids
// from the Network List Manager (COM: call on a thread that initialized it).
std::vector<Interface> currentInterfaces();

} // namespace localsend
