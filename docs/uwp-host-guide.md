# UWP host guide

Press **View + Menu** (Back + Start) together on a connected Xbox controller to
open the Blades host guide, including while a game is running. Release both
buttons before using the shortcut again. The shortcut waits while another
dialog is active.

The guide provides Friends, Profiles, Network Settings, Network Status, and
Gamerpic Browser. Friends opens directly in a Blades panel, including when a
game opens its Friends screen. Search starts highlighted without entering
text-edit mode. Use the direction buttons to
select a friend action; joining requires an available session in the running
title. The Manager, Friends and friend details, Sessions, UPnP ports, profile
deletion, Network Settings and its subpages, Network Status, gamerpic browser,
Achievements, and their confirmations use the same compact guide presentation
on UWP. Played Titles also keeps its cards inside that guide boundary.

Opening an option keeps the host guide as its parent. Close the selected page
with **B** to return to that same guide option; press **B** again to leave the
guide. Held buttons must be released before the restored guide accepts input.

Removing one or all friends opens a confirmation with **Cancel** highlighted.
The opening button must be released before **Remove** becomes available. Press
the direction buttons, then **A**, to deliberately confirm removal; another
**A** while Cancel is selected cancels. Press **B** to close a child popup first,
then Friends; **Escape** also works with a keyboard.

The game stays running, and guest controller input is suppressed while host UI
is active. Closing a dialog keeps held controls and queued button events away
from the game until each controller returns to neutral. This covers all four
controller slots and both state and keystroke input APIs. A notification alone
does not capture game input. The controller that opens the guide also controls
it. This shortcut is specific to UWP and does not change the desktop hotkey
configuration.

Buttons and text fields use the Sign-in guide's right boundary: 32 pixels of
left padding plus a 320-pixel content width at 1024 by 576, scaled with the
display and clamped to the panel. Long button labels are clipped inside their
button. Long popup pages scroll within the grey body, with the scrollbar hidden;
controller navigation still scrolls to the selected control. Netplay guide pages
use spacing and headings without divider lines. Network Settings closes with
Back and omits the desktop Discord Presence option. Profile row highlights
enclose the avatar and all wrapped profile text. Guide textures remain alive
until the closing frame has been submitted to the renderer.

The Paths page supports preset and custom storage locations. **Games directory** picks
the primary search folder; **Add another game folder** preserves additional
locations. Both offer **Development files** (`D:/DevelopmentFiles/Games/xbox360`),
**USB path** (`E:/Games/xbox360`), and **Custom**, which opens the native folder
picker. The USB directory is the default when no game paths are configured.
Changing the primary folder keeps the additional folders and refreshes the
library. The scanner uses these configured paths without an implicit LocalCache
search. A disconnected drive can remain selected; reconnect it and choose
**Refresh List**. Cancelling the picker preserves the current choice.

**Config folders** provides separate Development files (`D:/DevelopmentFiles/Xenia`),
USB (`E:/Xenia`), and Custom locations for cache, content, and storage subfolders.
Changes take effect after restarting Xenia and do not move existing profiles or
saves. Existing configured locations remain unchanged until a new one is chosen.
