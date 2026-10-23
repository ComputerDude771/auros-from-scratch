' Starts boot.ps1 with no window at all (0 = hidden), so nothing of the
' test's own appears on the screen it is photographing.
CreateObject("WScript.Shell").Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\e2e\boot.ps1", 0, False
