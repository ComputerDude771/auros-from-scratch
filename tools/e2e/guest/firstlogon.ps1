# Runs once, at the test account's first logon (autounattend.xml,
# FirstLogonCommands), from the answer CD.
#
# 1. Puts a small bootstrap on C: that, at every logon, fetches the UI
#    helper from the test's host (http://10.0.2.2:8000, QEMU's address
#    for the host). The helper only REPORTS what is on screen (window
#    and button names and where they are, from UI Automation) and runs
#    read-only diagnostics when asked; every press is made by the host,
#    through the virtual keyboard and tablet.
# 2. Tells the host what this Windows is: the facts the AurOS installer
#    will check for itself (Secure Boot, TPM, BitLocker, Fast Startup,
#    partitions), recorded independently of it.
param([string]$Src)
$ErrorActionPreference = 'Continue'
$hub = 'http://10.0.2.2:8000'

New-Item -ItemType Directory -Force C:\e2e | Out-Null
Copy-Item "$Src\boot.ps1", "$Src\boot.vbs" C:\e2e\ -Force

# Per-user settings for the account the test logs in as. None of them
# touch SmartScreen, Defender, UAC or power settings.
#  - the classic console host, so a hidden PowerShell stays hidden
#    (Windows Terminal, the default, opens a window regardless)
#  - no "let's finish setting up your device" screen on a later logon
$con = 'HKCU:\Console\%%Startup'
New-Item -Force $con | Out-Null
Set-ItemProperty $con DelegationConsole '{B23D10C0-E52E-411E-9D5B-C09FDF709C7D}'
Set-ItemProperty $con DelegationTerminal '{B23D10C0-E52E-411E-9D5B-C09FDF709C7D}'
$eng = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\UserProfileEngagement'
New-Item -Force $eng | Out-Null
Set-ItemProperty $eng ScoobeSystemSettingEnabled 0 -Type DWord
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' AurE2E 'wscript.exe "C:\e2e\boot.vbs"'

function Fact($f) { try { & $f 2>&1 | Out-String -Width 200 } catch { "error: $_" } }
$facts = [ordered]@{
  os          = Fact { Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture | Format-List }
  display     = Fact { (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').DisplayVersion }
  secure_boot = Fact { Confirm-SecureBootUEFI }
  tpm         = Fact { Get-Tpm | Select-Object TpmPresent, TpmReady, TpmEnabled, ManufacturerIdTxt, ManufacturerVersion | Format-List }
  bitlocker   = Fact { Get-BitLockerVolume -MountPoint C: | Select-Object VolumeStatus, ProtectionStatus, EncryptionPercentage, EncryptionMethod | Format-List }
  hiberboot   = Fact { (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power').HiberbootEnabled }
  hiberfil    = Fact { Test-Path -LiteralPath C:\hiberfil.sys }
  powercfg_a  = Fact { powercfg /a }
  disk        = Fact { Get-Disk | Select-Object Number, FriendlyName, SerialNumber, Size, PartitionStyle | Format-List }
  partitions  = Fact { Get-Partition -DiskNumber 0 | Select-Object PartitionNumber, Type, GptType, Offset, Size, DriveLetter | Format-Table -AutoSize }
  volume_c    = Fact { Get-Volume -DriveLetter C | Select-Object FileSystem, Size, SizeRemaining | Format-List }
  firmware    = Fact { bcdedit /enum firmware }
  reagentc    = Fact { reagentc /info }
  defender    = Fact { Get-MpComputerStatus | Select-Object AMServiceEnabled, RealTimeProtectionEnabled, AntivirusSignatureVersion | Format-List }
  screen      = Fact { Get-CimInstance Win32_VideoController | Select-Object Name, CurrentHorizontalResolution, CurrentVerticalResolution | Format-List }
}

# The machine this is meant to be (e2e\variant.txt, from mkanswer.sh): the
# variants are the PCs people actually have, made here with Windows' own
# tools, as their owners' Windows made them.
$variant = 'base'
if (Test-Path "$Src\variant.txt") { $variant = (Get-Content "$Src\variant.txt" -Raw).Trim() }
Set-Content C:\e2e\variant.txt $variant
switch ($variant) {
  'bitlocker' {
    # Device encryption as a PC with a TPM has it: BitLocker on C:,
    # TPM protector, used space encrypted. Wait until it is done.
    $facts.variant = Fact {
      Enable-BitLocker -MountPoint C: -EncryptionMethod XtsAes128 -UsedSpaceOnly -TpmProtector -SkipHardwareTest
      for ($i = 0; $i -lt 360; $i++) {
        if ((Get-BitLockerVolume -MountPoint C:).VolumeStatus -eq 'FullyEncrypted') { break }
        Start-Sleep 10
      }
      Get-BitLockerVolume -MountPoint C: | Select-Object VolumeStatus, ProtectionStatus, EncryptionPercentage, KeyProtector | Format-List
    }
  }
  'smalldisk' {
    # A small drive with years of leftovers in Windows' temp folder: too
    # little room until something cleans up (the installer's space fix,
    # which empties that folder). Sized from what is free: 30 GB left
    # free is 22 GB AurBridge can give (it keeps 8 for Windows) against
    # the 28 it needs; with the leftovers gone it has 28 plus theirs.
    $facts.variant = Fact {
      $free = (Get-Volume -DriveLetter C).SizeRemaining
      $junk = $free - 30GB
      if ($junk -lt 10GB) { "TOO SMALL: only $([math]::Round($free / 1GB, 1)) GB free, the variant needs 40" }
      else { fsutil file createnew C:\Windows\Temp\e2e-old-leftovers.tmp $junk }
      Get-Volume -DriveLetter C | Select-Object Size, SizeRemaining | Format-List
    }
  }
  'pending' {
    # A Windows that has finished installing a feature and is waiting for
    # a restart. Made at the test's own logon (a restart here would finish
    # it), by enabling a feature without restarting: real servicing, which
    # the next real restart completes.
    Set-Content C:\e2e\pending.ps1 @'
Unregister-ScheduledTask -TaskName 'E2E pending restart' -Confirm:$false
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /norestart /quiet
$k = Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending'
Invoke-WebRequest -UseBasicParsing -Method Post -Uri 'http://10.0.2.2:8000/e2e/event?name=pending-ready' -Body "dism exit $LASTEXITCODE, RebootPending=$k"
'@
    $facts.variant = Fact {
      $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -File C:\e2e\pending.ps1'
      Register-ScheduledTask -TaskName 'E2E pending restart' -Action $a -Trigger (New-ScheduledTaskTrigger -AtLogOn) -User SYSTEM -RunLevel Highest -Force | Out-Null
      'registered'
    }
  }
}
$facts.variant_name = $variant

# Free space is given back to the host's disk image (the drive is an SSD
# to Windows, and QEMU passes TRIM through), which keeps the cached copy
# of this installed Windows small.
Fact { Optimize-Volume -DriveLetter C -ReTrim } | Out-Null

$body = [Text.Encoding]::UTF8.GetBytes(($facts | ConvertTo-Json -Depth 4))
for ($i = 0; $i -lt 120; $i++) {
  try {
    Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$hub/e2e/event?name=firstlogon" `
      -Body $body -ContentType 'application/json' -TimeoutSec 20 | Out-Null
    break
  } catch { Start-Sleep 5 }
}
Start-Process wscript.exe '"C:\e2e\boot.vbs"'
