# BadUSB scripts

Scripts for the Flipper Zero's BadUSB app. Copy them to `SD Card/badusb/`.

| Script | Use |
|---|---|
| `usb_path_test.txt` | Proves keyboard and mouse pass through an extender, switcher, dock or hub. Click into a text field on the far-end PC first. |
| `usb_path_test_windows_notepad.txt` | The same test, but opens Notepad first (Windows). |
| `keep_awake_30min.txt` | Nudges the pointer one pixel every 30 seconds for 30 minutes, to keep a room PC awake during burn-in. |

A path test passes when all three text lines arrive intact and the pointer draws a square.

These follow the firmware's documented script commands and have not yet been run on a
Flipper. The mouse commands are recent additions to the firmware.
