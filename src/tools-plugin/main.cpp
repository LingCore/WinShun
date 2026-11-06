// WinShunTools.exe: the winshun.tools plugin on its own (Tools.cpp), for when
// Win顺 keeps plugins apart instead of running it inside its plugin host.

#include "Tools.h"

int main()
{
    return tools::serve(::GetStdHandle(STD_INPUT_HANDLE), ::GetStdHandle(STD_OUTPUT_HANDLE));
}
