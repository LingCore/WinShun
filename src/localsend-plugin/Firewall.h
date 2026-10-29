#pragma once

// What Windows Firewall will do to devices connecting to this program, read
// only (INetFwPolicy2; a medium-integrity process may read it): whether it
// is on for the networks this PC is on now, and whether a rule for the
// program lets connections in or keeps them out. The plugin never changes
// a rule: the installer adds one (admin installs); otherwise Windows asks
// the user the first time the program listens, and the 传 list says what
// to do when the answer was no.

#include <string>

namespace localsend {

struct FirewallFacts {
    bool known = false; // read at all
    bool on = false; // for one of the current profiles
    bool allowed = false; // an enabled inbound allow rule for the program, for a current profile
    bool blocked = false; // an enabled inbound block rule for it (a prompt answered with 取消)
};

// `program`: the plugin's own exe, full path. COM must be initialized on
// the calling thread. Enumerating the rules takes tens of milliseconds:
// call it now and then, not for every row.
FirewallFacts firewallFacts(const std::wstring& program);

} // namespace localsend
