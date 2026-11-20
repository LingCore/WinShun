#pragma once

#include <QString>

#include <memory>

namespace ws::capture {

// Whether the screen and the desktop kept their beat while the capture's own
// drawing stood still (#325: about 2 s inside the AMD driver's Present, with
// the process idle). Two threads of its own, waiting from arm() to disarm():
// one for each vertical blank of the monitor followed (D3DKMT, the kernel's
// count of the screen's refreshes), one for each of the desktop's
// compositions (DwmFlush). take() gives the longest wait of each since the
// last take, so a record of a stall says which of them stopped with it:
//  - the vertical blanks: the screen itself (its mode changed, a reset card, TDR);
//  - only the compositions: the desktop (dwm.exe) and every program with it;
//  - neither: only this program's drawing, in the driver.
// docs/pitfalls.md "截图：遮罩一出来固定卡约 2 秒".
class DisplayPulse {
public:
    struct Gaps {
        int vblankMs = -1; // -1: not measured (disarmed, or the monitor could not be followed)
        int dwmMs = -1;
        QString vblankError; // the NTSTATUS a wait failed with, "0xc01e0001": a reset card or a changed mode
    };

    DisplayPulse();
    ~DisplayPulse(); // returns at once: a thread still waiting ends by itself after its wait
    DisplayPulse(const DisplayPulse&) = delete;
    DisplayPulse& operator=(const DisplayPulse&) = delete;

    // From now on, the monitor by its GDI name (MONITORINFOEX's szDevice).
    void arm(const QString& gdiDeviceName);
    void disarm();
    // The longest gaps since the last take (or arm); one still open counts until now.
    Gaps take();

private:
    struct State;
    static void followVerticalBlanks(std::shared_ptr<State> state);
    static void followCompositions(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
};

} // namespace ws::capture
