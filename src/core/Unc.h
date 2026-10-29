#pragma once

#include <string>
#include <string_view>
#include <vector>

// Network paths: "\\host\share\folder". Hosts are names or IP addresses
// ("\\192.168.1.10\docs"); "\\?\" and "\\.\" device paths are not network paths.
namespace ws::unc {

bool isUnc(std::wstring_view path) noexcept;
bool isWslHost(std::wstring_view host) noexcept;
// The registered distribution name in a WSL UNC path; empty for SMB paths.
std::wstring_view wslDistribution(std::wstring_view path) noexcept;

// "\\Host\Share\a\" -> "\\Host\Share\a": backslashes only, one between names,
// none at the end. Empty unless it names at least a share ("\\host\share").
std::wstring normalize(std::wstring_view path);
// normalize(), and a WebDAV folder in the one form it is kept in, however it
// was written (GitHub #114: Windows itself writes one folder two ways, and
// addShare() must see a folder added already). The server in lower case,
// "@SSL" for TLS, its port unless TLS on 443 (80 too, which Windows leaves
// out: the path then says it is no SMB one, and WebClient takes it), and
// DavWWWRoot only for the server's root, as Windows lists connections:
// "\\NAS@ssl@443\DavWWWRoot\dav\" ->
// "\\nas@SSL\dav", "\\nas\DavWWWRoot" -> "\\nas@80\DavWWWRoot". A WebDAV
// server alone is its root ("\\nas@SSL@5006" -> "\\nas@SSL@5006\DavWWWRoot").
std::wstring canonical(std::wstring_view path);

std::wstring_view host(std::wstring_view path) noexcept; // "\\host\share\a" -> "host"
std::wstring_view shareRoot(std::wstring_view path) noexcept; // -> "\\host\share"

// Whether `path` is `folder` or lies inside it. Linux components in WSL
// are case sensitive; SMB paths and WSL host/distribution names are not.
bool contains(std::wstring_view folder, std::wstring_view path) noexcept;
// WSL's Linux components are case sensitive; hosts/distributions and SMB are not.
bool equal(std::wstring_view a, std::wstring_view b) noexcept;

// The network folder a drive letter is mapped to ("Z:" -> "\\nas\docs"): as
// Windows remembers it for the user (HKCU\Network, which an elevated process
// sees too), else the letter's connection for this sign-in (net use ...
// /persistent:no). WebDAV drives come in WebClient's UNC form ("R:" ->
// "\\nas@5244\dav", canonical()). "Z:\a\b" keeps "\a\b". Empty when the
// letter is not mapped.
std::wstring fromMappedDrive(std::wstring_view path);

// Shares the user has used: mapped drives (HKCU\Network) and the shares
// Explorer has opened (MountPoints2), newest first as far as Windows says,
// without duplicates. Reads the registry only, never the network.
struct Known {
    std::wstring path; // "\\host\share", canonical()
    std::wstring drive; // "Z:" when mapped, else empty
};
std::vector<Known> knownShares();

} // namespace ws::unc
