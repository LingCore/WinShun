#include "Pipe.h"

#include <sddl.h>

namespace ws::pipe {

PSECURITY_DESCRIPTOR createSecurityDescriptor()
{
    const std::wstring sid = userSid();
    if (sid.empty())
        return nullptr;
    // Everything for the system and administrators (Win顺 itself); reading
    // and writing for the user. Protected: nothing inherited. The label lets
    // medium-integrity processes in (made by an elevated process, the pipe
    // would be high, and they could not open it), and keeps low ones out.
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;FRFW;;;" + sid + L")S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return nullptr;
    return descriptor;
}

} // namespace ws::pipe
