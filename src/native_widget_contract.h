#pragma once
#include "native_widgets.h"

namespace panel::widget {
inline constexpr unsigned buttonTransient=0x100000, inactive=0x110, checked=0x60;
inline unsigned appearance(unsigned kind) {
    return kind==1 ? 0x20403300 : kind==4 ? 0x00405320 : 0;
}
// An appearance is rendering/style data, NOT an input permission or identity.
// Vanilla type-two checkboxes begin with appearance=0 and 33C90 assigns
// 00C05320 at runtime. GUI focus, exact owned descriptor/action, and thread
// have already been checked by activated(). Do not reject valid native events
// merely because the game's renderer normalized or changed the appearance.
inline bool checkboxAction(const NativeElement& e,int row) {
    return e.kind==2 && e.group==0 && rowInfo(row).checkbox();
}
// Ownership, descriptor identity and thread admission remain the controller's responsibility.
inline bool activation(const NativeElement& e,int row,bool capture,bool controls) {
    if (e.flags & inactive) return false;
    // The root form's buttons (tabs, Defaults, Apply, Cancel): the game calls an action only for a click, and
    // on the widened Hellmode form it no longer carries the transient bit when it does (status: refused 0x1001000).
    if (!controls) return e.kind==1;
    if (capture) return e.kind==5;
    if (rowInfo(row).checkbox()) return checkboxAction(e,row);
    // Native motion/directional and programmatic slider actions omit transient.
    if (rowInfo(row).range.count) return e.kind==4;
    // A value button in the scrolling pane: the game calls its action only for a click, and does so after
    // clearing the transient bit the root form's buttons still carry, so it is not required here.
    return e.kind==1;
}
} // namespace panel::widget
