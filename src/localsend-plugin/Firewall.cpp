#include "Firewall.h"

#include <windows.h>

#include <netfw.h>
#include <oleauto.h>

#include <vector>

namespace localsend {

namespace {

std::wstring expanded(const wchar_t* path)
{
    if (!path || !*path)
        return {};
    const DWORD size = ::ExpandEnvironmentStringsW(path, nullptr, 0);
    std::wstring out(size, L'\0');
    if (size == 0 || ::ExpandEnvironmentStringsW(path, out.data(), size) == 0)
        return path;
    out.resize(size - 1);
    return out;
}

bool samePath(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

FirewallFacts firewallFacts(const std::wstring& program)
{
    FirewallFacts facts;
    INetFwPolicy2* policy = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy))))
        return facts;
    long current = 0;
    if (FAILED(policy->get_CurrentProfileTypes(&current))) {
        policy->Release();
        return facts;
    }
    facts.known = true;
    for (const NET_FW_PROFILE_TYPE2 profile : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
        VARIANT_BOOL on = VARIANT_FALSE;
        if ((current & profile) && SUCCEEDED(policy->get_FirewallEnabled(profile, &on)) && on != VARIANT_FALSE)
            facts.on = true;
    }
    INetFwRules* rules = nullptr;
    if (facts.on && SUCCEEDED(policy->get_Rules(&rules))) {
        IUnknown* unknown = nullptr;
        IEnumVARIANT* each = nullptr;
        if (SUCCEEDED(rules->get__NewEnum(&unknown)) && unknown && SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&each)))) {
            VARIANT item;
            ::VariantInit(&item);
            ULONG fetched = 0;
            while (each->Next(1, &item, &fetched) == S_OK && fetched) {
                INetFwRule* rule = nullptr;
                if (item.vt == VT_DISPATCH && item.pdispVal && SUCCEEDED(item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) {
                    BSTR application = nullptr;
                    NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
                    VARIANT_BOOL enabled = VARIANT_FALSE;
                    NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
                    long profiles = 0;
                    if (SUCCEEDED(rule->get_ApplicationName(&application)) && application && SUCCEEDED(rule->get_Direction(&direction))
                        && direction == NET_FW_RULE_DIR_IN && SUCCEEDED(rule->get_Enabled(&enabled)) && enabled != VARIANT_FALSE
                        && SUCCEEDED(rule->get_Profiles(&profiles)) && (profiles & current) && SUCCEEDED(rule->get_Action(&action))
                        && samePath(expanded(application), program)) {
                        if (action == NET_FW_ACTION_ALLOW)
                            facts.allowed = true;
                        else
                            facts.blocked = true;
                    }
                    ::SysFreeString(application);
                    rule->Release();
                }
                ::VariantClear(&item);
            }
            each->Release();
        }
        if (unknown)
            unknown->Release();
        rules->Release();
    }
    policy->Release();
    return facts;
}

} // namespace localsend
