# Files + FTP (RG35XX Plus)

Two-pane file manager with a built-in FTP server, so files can be copied
between a PC (or phone) and the handheld over Wi-Fi instead of moving the SD
card back and forth.

## Install
Copy `dist/APPS/FilesFTP.sh` and the `dist/APPS/FilesFTP/` folder into the SD
card's `Roms/APPS`. Start **FilesFTP** from App Center.

## Buttons
| Button | Files screen |
|---|---|
| D-pad Up/Down | Move (hold = fast) |
| D-pad Left/Right | Switch between the left and right pane |
| A | Open folder / file actions |
| B | Up one folder (at the top of a card: list of cards) |
| Y | Mark / unmark (for several files at once) |
| X | Actions: copy or move to the other side, delete, rename, new folder, info, mark all, sort, hidden files |
| L1 / R1 | Page up / down |
| L2 / R2 | First / last item |
| Select | Go to: TF1, TF2, USB drives, app folder, whole system |
| Start | Wireless transfer (FTP) screen |
| Menu | Exit (asks) |
| Select + Start | Exit |
| Select + B | Screen off (FTP transfers keep running); any button turns it on |

Copies never overwrite: an existing name becomes `name (2).ext`. Copy / move /
delete run in the background with a progress bar (B stops). Rename and new
folder use an on-screen keyboard (A type, B delete, Y shift, X space, Start done).

## Wireless transfer (FTP)
Start → A starts the server. The screen shows the address, e.g.
`ftp://192.168.254.127:2121`, user name and password. On a Windows PC type the
address into File Explorer's address bar and log in; copy files by dragging.
Also works with FileZilla, WinSCP and phone FTP apps. The PC sees one folder
per card (`TF1`, `TF2`, `USB-...`). X makes a new password. If the server was
running when the app was closed, it starts again next time.

Plain FTP (no encryption): use it on your home Wi-Fi only.

## Build (WSL Ubuntu-22.04)
`make pc rg dist`. PC test aids: `--shot out.bmp secs`, `FB_SIM="down,a,x"`
(key presses), `FB_SIM_MS`. Fonts: `font.ttf` in the app folder, else the
firmware's `/mnt/vendor/bin/default.ttf`, else DejaVu; stb_truetype (public
domain) renders them.
