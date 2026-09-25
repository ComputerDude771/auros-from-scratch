# Trying the no-stick test build on a spare PC

This is for **a PC whose files you do not need**, or a virtual machine.
The installer is unsigned, and this is the first time its Windows half
meets a real computer. Read `docs/RELEASE.md`, "What the test build is,
and is not", before starting.

## What you need

- A PC with **Windows 10 or 11**, started the modern way (UEFI). Almost
  every PC from 2013 on is.
- **About 50 GB free on C:**. AurOS needs 28 GB once it is installed,
  Windows keeps 8 GB, and the 5 GB copy of AurOS stays on C: while it
  installs (plus 1.8 GB of download for a while).
- A wired or reliable internet connection: about **1.8 GB** is
  downloaded. It resumes if it drops.
- **The charger plugged in**, for the whole thing.
- **No USB drives plugged in.** The installer refuses while one is.

## Before you start, in Windows

1. **Turn off Fast Startup.** Control Panel → Power Options → *Choose
   what the power buttons do* → *Change settings that are currently
   unavailable* → untick **Turn on fast startup** → Save. (With it on,
   Windows never really shuts down, and AurOS will not resize a drive
   that is half-asleep.)
2. **If the drive is encrypted, decrypt it.** Settings → Privacy &
   security → **Device encryption** → Off (or BitLocker → Turn off),
   and wait until it says it is finished. AurOS refuses encrypted
   drives; it cannot resize them.
3. **Install any waiting Windows updates and restart once**, so nothing
   is half-installed.

Leave **Secure Boot on**; AurOS starts with it on. On the rare PC that
trusts only Windows (some Secured-core laptops), the installer stops at
*Check this PC* and shows the one firmware setting to switch on.

## Running it

1. Download `AurOS-Installer-test.exe` and double-click it.
2. Windows shows **"Windows protected your PC"**, because the file is
   not signed yet. Click **More info**, then **Run anyway**.
   (If there is no *Run anyway*, the PC has Smart App Control on, and
   an unsigned installer cannot run on it. Use a different PC.)
3. Windows asks **"Do you want to allow this app to make changes?"** →
   **Yes**.
4. Follow the pages. **Check this PC** reads only. If it stops with a
   red card, the card says what to do; nothing has been changed.
5. On **Make it yours**, the language, keyboard, time zone, look and
   (on the page before) desktop you pick are what AurOS starts with.
   The ones Windows already uses are picked for you.
6. On **Ready**, tick the box and press **Start installing**. It
   downloads AurOS (the long part), checks it, and gets the restart
   ready. Nothing on the drive is changed yet.
7. Press **Restart now**. Do not close the window instead: closing it
   takes everything back, on purpose.

## After the restart

- The screen shows **text**, not a desktop. That is the installer. It
  makes the Windows drive smaller (minutes, or longer on an old hard
  drive), saves a copy of the PC's start-up onto the drive, copies
  AurOS, checks it, and starts it. **Do not switch the PC off** while
  it says *Making room on the Windows drive*.
- If it stops with a sentence and `verdict=...`, **take a photo of the
  screen** and send it. A stop before *Making room* has changed
  nothing; switch the PC off and on and Windows starts as before.
- On its first start AurOS applies what you chose in the installer. The
  language sets dates, numbers and the programs that carry their own
  translations; AurOS's own menus are English for now.
- When AurOS starts, it asks **whether it works**. Say **yes** to make
  AurOS what the PC starts from now on; say **no** and the PC goes back
  to starting Windows. Windows is still there either way.

## Getting to Windows afterwards

Windows stays in the PC's start-up menu. Press the menu key when the PC
switches on (often **F12**, **F9** or **Esc**; the maker's logo screen
usually says which) and choose **Windows Boot Manager**. AurOS's own
start menu also has a **Windows** entry.

The `C:\AurOS` folder keeps the 5 GB copy of AurOS. Once AurOS works
you can delete it from Windows.

**There is no "Put Windows back" button yet** (see `docs/RELEASE.md`
§7): the restore that removes AurOS and gives Windows its space back
exists and is tested, but nothing on screen starts it in this build.
