# RunMeCpp end-to-end smoke tests (ASCII only, PS 5.1 compatible)
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\run-tests.ps1 [-Exe <path\to\RunMeCpp.exe>]
param([string]$Exe = '')
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = $Exe
if (-not $exe) { $exe = Join-Path $root 'bin\Release\RunMeCpp.exe' }
if (-not (Test-Path $exe)) { $exe = Join-Path $root 'bin\Debug\RunMeCpp.exe' }
if (-not (Test-Path $exe)) { throw "Build first: $exe not found" }

$sandbox = Join-Path $env:TEMP 'RunMeTest'
$results = New-Object System.Collections.ArrayList

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Win32Native {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageStr(IntPtr hWnd, int msg, IntPtr wParam, StringBuilder lParam);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessageInt(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    public static IntPtr FindListBox(IntPtr parent) {
        IntPtr found = IntPtr.Zero;
        EnumProc cb = (h, l) => {
            var cls = new StringBuilder(256);
            GetClassName(h, cls, 256);
            if (cls.ToString().ToUpperInvariant().Contains("LISTBOX")) { found = h; return false; }
            return true;
        };
        EnumChildWindows(parent, cb, IntPtr.Zero);
        GC.KeepAlive(cb);
        return found;
    }
    public static int CountConsoleWindows() {
        int count = 0;
        EnumProc cb = (h, l) => {
            if (!IsWindowVisible(h)) return true;
            var cls = new StringBuilder(256);
            GetClassName(h, cls, 256);
            var c = cls.ToString();
            if (c == "ConsoleWindowClass" || c == "CASCADIA_HOSTING_WINDOW_CLASS") count++;
            return true;
        };
        EnumWindows(cb, IntPtr.Zero);
        GC.KeepAlive(cb);
        return count;
    }
}
"@

function Add-Result([string]$name, [bool]$ok, [string]$info = '') {
    if ($ok) { $res = 'PASS' } else { $res = 'FAIL' }
    [void]$results.Add([pscustomobject]@{ Case = $name; Result = $res; Info = $info })
    Write-Host ("[{0}] {1} {2}" -f $res, $name, $info)
}

function Wait-File([string]$path, [int]$timeoutMs = 5000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if (Test-Path $path) { return $true }
        [System.Threading.Thread]::Sleep(100)
    }
    return (Test-Path $path)
}

# run synchronously; returns $true if process exited within timeout
function Invoke-Sync([string]$file, $arguments, [string]$wd, [int]$timeoutMs = 10000) {
    if ($arguments -and $arguments.Count -gt 0) {
        $p = Start-Process -FilePath $file -ArgumentList $arguments -WorkingDirectory $wd -PassThru
    } else {
        $p = Start-Process -FilePath $file -WorkingDirectory $wd -PassThru
    }
    $exited = $p.WaitForExit($timeoutMs)
    if (-not $exited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    return $exited
}

# run and capture the main window title (for window cases), then kill
function Invoke-CaptureTitle([string]$file, $arguments, [string]$wd, [int]$timeoutMs = 8000) {
    if ($arguments -and $arguments.Count -gt 0) {
        $p = Start-Process -FilePath $file -ArgumentList $arguments -WorkingDirectory $wd -PassThru
    } else {
        $p = Start-Process -FilePath $file -WorkingDirectory $wd -PassThru
    }
    $title = ''
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $p.Refresh()
        if ($p.HasExited) { break }
        if ($p.MainWindowTitle) { $title = $p.MainWindowTitle; break }
        [System.Threading.Thread]::Sleep(100)
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    return $title
}

# run and capture the list window items, then kill
function Get-ListItemsText([System.Diagnostics.Process]$p, [int]$timeoutMs = 8000) {
    $listBox = [IntPtr]::Zero
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $p.Refresh()
        if ($p.HasExited) { break }
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            $listBox = [Win32Native]::FindListBox($p.MainWindowHandle)
            if ($listBox -ne [IntPtr]::Zero) { break }
        }
        [System.Threading.Thread]::Sleep(100)
    }
    $items = @()
    if ($listBox -ne [IntPtr]::Zero) {
        $count = [int][Win32Native]::SendMessageInt($listBox, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
        for ($i = 0; $i -lt $count; $i++) {
            $sb = New-Object System.Text.StringBuilder -ArgumentList 256
            [void][Win32Native]::SendMessageStr($listBox, 0x0189, [IntPtr]$i, $sb)
            $items += $sb.ToString()
        }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    return ($items -join "`n")
}

# ---------------- prepare sandbox ----------------
Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
New-Item $sandbox -ItemType Directory | Out-Null
Copy-Item $exe (Join-Path $sandbox 'RunMe.exe')
Copy-Item $exe (Join-Path $sandbox 'Wx.exe')
Copy-Item $exe (Join-Path $sandbox 'Multi.exe')
Copy-Item $exe (Join-Path $sandbox 'Keys.exe')
Copy-Item $exe (Join-Path $sandbox 'Txt.exe')
Copy-Item $exe (Join-Path $sandbox 'Mx.exe')
Copy-Item $exe (Join-Path $sandbox 'Pk.exe')
Copy-Item $exe (Join-Path $sandbox 'Psx.exe')
Copy-Item $exe (Join-Path $sandbox 'Rc.exe')
Copy-Item $exe (Join-Path $sandbox 'Ra.exe')
Copy-Item $exe (Join-Path $sandbox 'Cc.exe')
Copy-Item $exe (Join-Path $sandbox 'R1.exe')
Copy-Item $exe (Join-Path $sandbox 'Hd.exe')
Copy-Item $exe (Join-Path $sandbox 'Sh.exe')

$cmd = Join-Path $env:WINDIR 'System32\cmd.exe'
$ini = @"
[Settings]
RunParentDirectory=$sandbox
ExcludeExeName=RunMe|MeRun
# countdown off: this suite must keep the window open and drive the list deterministically
ListAutoRunSeconds=0

[Config]
RunMe=$cmd /c echo noargs>$sandbox\noargs.txt
Wx=$cmd /c echo wx>$sandbox\wx.txt
Multi=runme x1|$cmd,x2|$cmd
Keys=$cmd /c echo {guid.id} {time.yyyy} {random.1-9} ok>$sandbox\placeholders.txt
Mx=runme A1|cmd echo a1>$sandbox\a1.txt,B2|cmd echo b2>$sandbox\b2.txt,C3|ps Set-Content -Path $sandbox\c3.txt -Value c3
Pk=cmd echo {0}>$sandbox\pkarg.txt
Psx=ps Set-Content -Path $sandbox\psok.txt -Value psok
Rc=robocopy $sandbox\src $sandbox\dst a.txt
Ra=cmd runadmin
Cc=cmd echo a,b>$sandbox\comma.txt
R1=runme Only|cmd echo r1>$sandbox\r1.txt
Hd=cmd ping -n 3 127.0.0.1 >$sandbox\hd.txt
Sh=show cmd ping -n 3 127.0.0.1 >$sandbox\sh.txt
"@
Set-Content -Path (Join-Path $sandbox 'YanBinCfg.ini') -Value $ini -Encoding UTF8

# ---------------- cases ----------------

# 1) no args, [Config] single entry -> direct launch, no window
$ok = (Invoke-Sync (Join-Path $sandbox 'RunMe.exe') @() $sandbox) -and (Wait-File (Join-Path $sandbox 'noargs.txt'))
Add-Result 'NoArgs: [Config] single entry -> direct launch' $ok

# 2) runme single entry, separate-word form
$a2 = '"single|' + $cmd + ' /c echo runme-single>' + $sandbox + '\runme-single.txt"'
$ok = (Invoke-Sync (Join-Path $sandbox 'RunMe.exe') @('runme', $a2) $sandbox) -and (Wait-File (Join-Path $sandbox 'runme-single.txt'))
Add-Result 'runme single entry (separate word form)' $ok

# 3) runme single entry, quoted whole-string form (old style)
$a3 = '"runme single2|' + $cmd + ' /c echo runme-single2>' + $sandbox + '\runme-single2.txt"'
$ok = (Invoke-Sync (Join-Path $sandbox 'RunMe.exe') @($a3) $sandbox) -and (Wait-File (Join-Path $sandbox 'runme-single2.txt'))
Add-Result 'runme single entry (quoted whole-string form)' $ok

# 4) multi-identity: exe renamed to Wx.exe -> [Config] Wx
$ok = (Invoke-Sync (Join-Path $sandbox 'Wx.exe') @() $sandbox) -and (Wait-File (Join-Path $sandbox 'wx.txt'))
Add-Result 'Multi-identity: Wx.exe -> [Config] Wx' $ok

# 6) runme multi entries -> list window
$title = Invoke-CaptureTitle (Join-Path $sandbox 'RunMe.exe') @('runme', ('"m1|' + $cmd + ',m2|' + $cmd + '"')) $sandbox
Add-Result 'runme multi entries -> list window' ($title.Length -gt 0) ("title='$title'")

# 7) multi-identity + [Config] multi entries -> list window
$title = Invoke-CaptureTitle (Join-Path $sandbox 'Multi.exe') @() $sandbox
Add-Result 'Multi-identity Multi.exe -> list window' ($title.Length -gt 0) ("title='$title'")

# 8) list mode, default directory
Set-Content (Join-Path $sandbox 'aa.txt') 'a'
Set-Content (Join-Path $sandbox 'bb.txt') 'b'
$title = Invoke-CaptureTitle (Join-Path $sandbox 'RunMe.exe') @('list', 'txt') $sandbox
Add-Result 'list txt (default dir) -> list window' ($title.Length -gt 0) ("title='$title'")

# 9) list mode, explicit directory
$title = Invoke-CaptureTitle (Join-Path $sandbox 'RunMe.exe') @('list', 'txt', $sandbox) $sandbox
Add-Result 'list txt <dir> -> list window' ($title.Length -gt 0) ("title='$title'")

# 10) help
$title = Invoke-CaptureTitle (Join-Path $sandbox 'RunMe.exe') @('help') $sandbox
Add-Result 'help -> help window' ($title -match '\u4F7F\u7528\u5E2E\u52A9') ("title='$title'")

# 11) list on missing directory -> should show a message box, not crash
$p = Start-Process -FilePath (Join-Path $sandbox 'RunMe.exe') -ArgumentList 'list', 'txt', 'N:\NoSuchDir' -WorkingDirectory $sandbox -PassThru
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 2000 -and -not $p.HasExited) { [System.Threading.Thread]::Sleep(100) }
if ($p.HasExited) { $info11 = "process exited, exitcode=$($p.ExitCode)" } else { $info11 = 'process alive (waiting on message box)' }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Add-Result 'list missing dir -> no crash' ($info11 -like '*alive*') $info11

# 14) placeholders {guid.id} {time.yyyy} {random.1-9}
$ok = (Invoke-Sync (Join-Path $sandbox 'Keys.exe') @() $sandbox)
$txt = ''
$pattern = '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12} \d{4} [1-9] ok$'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 5000) {
    $raw = $null
    if (Test-Path (Join-Path $sandbox 'placeholders.txt')) {
        $raw = Get-Content (Join-Path $sandbox 'placeholders.txt') -Raw
    }
    if ($null -ne $raw) { $candidate = $raw.Trim() } else { $candidate = '' }
    if ($candidate -match $pattern) { $txt = $candidate; break }
    [System.Threading.Thread]::Sleep(100)
}
Add-Result 'placeholders guid.id / time.yyyy / random.1-9' ($ok -and ($txt -match $pattern)) ("value='$txt'")

# 15) {name}run.txt sequential launch
Set-Content -Path (Join-Path $sandbox 'Txtrun.txt') -Value @(
    "$cmd /c echo line1>$sandbox\line1.txt",
    "$cmd /c echo line2>$sandbox\line2.txt"
)
$ok = (Invoke-Sync (Join-Path $sandbox 'Txt.exe') @() $sandbox) -and (Wait-File (Join-Path $sandbox 'line1.txt')) -and (Wait-File (Join-Path $sandbox 'line2.txt'))
Add-Result 'run.txt (Txtrun.txt) sequential launch' $ok

# 17) config list: display names without 'runme ' prefix + cmd-prefixed targets
$p17 = Start-Process -FilePath (Join-Path $sandbox 'Mx.exe') -WorkingDirectory $sandbox -PassThru
$items17 = @((Get-ListItemsText $p17) -split "`n" | Where-Object { $_ -ne '' })
$ok17 = ($items17.Count -eq 3) -and ($items17[0] -eq 'A1') -and ($items17[1] -eq 'B2') -and ($items17[2] -eq 'C3')
Add-Result 'Config list: names stripped (A1,B2,C3) + cmd/ps targets' $ok17 ("items='$($items17 -join ',')'")

# 18) runme single target: cmd prefix + placeholder inside command
$a18 = '"X0|cmd echo {time.yyyy} ok>' + $sandbox + '\cmdp.txt"'
$ok18 = (Invoke-Sync (Join-Path $sandbox 'RunMe.exe') @('runme', $a18) $sandbox) -and (Wait-File (Join-Path $sandbox 'cmdp.txt'))
$content18 = ''
if (Test-Path (Join-Path $sandbox 'cmdp.txt')) { $content18 = (Get-Content (Join-Path $sandbox 'cmdp.txt') -Raw).Trim() }
$ok18 = $ok18 -and ($content18 -match '^\d{4} ok$')
Add-Result 'cmd prefix + placeholder in runme target' $ok18 ("content='$content18'")

# 19) {0} filled from command line args (drag-drop style)
$p19 = Start-Process -FilePath (Join-Path $sandbox 'Pk.exe') -ArgumentList 'HELLO' -WorkingDirectory $sandbox -PassThru
$ok19 = $p19.WaitForExit(10000) -and (Wait-File (Join-Path $sandbox 'pkarg.txt'))
$content19 = ''
if (Test-Path (Join-Path $sandbox 'pkarg.txt')) { $content19 = (Get-Content (Join-Path $sandbox 'pkarg.txt') -Raw).Trim() }
$ok19 = $ok19 -and ($content19 -eq 'HELLO')
Add-Result '{0} filled by command line arg (Pk.exe HELLO)' $ok19 ("content='$content19'")

# 20) ps prefix in config
# powershell.exe starts slowly: poll until the file has CONTENT, otherwise we may read it
# right after creation (empty) and fail on a slow/loaded machine
$exited20 = Invoke-Sync (Join-Path $sandbox 'Psx.exe') @() $sandbox
$content20 = ''
$sw20 = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw20.ElapsedMilliseconds -lt 15000) {
    if (Test-Path (Join-Path $sandbox 'psok.txt')) {
        $raw20 = Get-Content (Join-Path $sandbox 'psok.txt') -Raw -ErrorAction SilentlyContinue
        if ($raw20) { $content20 = $raw20.Trim(); break }
    }
    [System.Threading.Thread]::Sleep(200)
}
$ok20 = $exited20 -and ($content20 -eq 'psok')
Add-Result 'ps prefix in config (Psx.exe)' $ok20 ("content='$content20'")

# 21) bare command via PATH in config (robocopy)
New-Item (Join-Path $sandbox 'src') -ItemType Directory -Force | Out-Null
Set-Content (Join-Path $sandbox 'src\a.txt') 'payload'
$ok21 = (Invoke-Sync (Join-Path $sandbox 'Rc.exe') @() $sandbox) -and (Wait-File (Join-Path $sandbox 'dst\a.txt'))
Add-Result 'bare command via PATH (robocopy)' $ok21

# 22) first run generates annotated default config
$fresh = Join-Path $env:TEMP 'RunMeFresh'
Remove-Item $fresh -Recurse -Force -ErrorAction SilentlyContinue
New-Item $fresh -ItemType Directory | Out-Null
Copy-Item $exe (Join-Path $fresh 'RunMe.exe')
$p22 = Start-Process -FilePath (Join-Path $fresh 'RunMe.exe') -WorkingDirectory $fresh -PassThru
$exited22 = $p22.WaitForExit(10000)
$iniPath22 = Join-Path $fresh 'YanBinCfg.ini'
$text22 = ''
if (Test-Path $iniPath22) { $text22 = Get-Content $iniPath22 -Raw }
$ok22 = $exited22 -and (Test-Path $iniPath22) -and ($text22.Contains('[Config]')) -and ($text22.Contains('runadmin')) -and ($text22.Contains('RunParentDirectory')) -and ($text22.Contains('{time.')) -and ($text22.Contains('runmeth')) -and ($text22.Contains('ListAutoRunSeconds=5'))
Add-Result 'first run generates annotated default config' $ok22

# 23/24/25) run list items with cmd/ps prefixed targets by moving selection down and pressing Enter
function Invoke-ListSelection([string]$file, [string]$wd, [int]$downCount, [string]$expectedFile) {
    $p = Start-Process -FilePath $file -WorkingDirectory $wd -PassThru
    $lb = [IntPtr]::Zero
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 8000) {
        $p.Refresh()
        if ($p.HasExited) { break }
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            $lb = [Win32Native]::FindListBox($p.MainWindowHandle)
            if ($lb -ne [IntPtr]::Zero) { break }
        }
        [System.Threading.Thread]::Sleep(100)
    }
    $ran = $false
    if ($lb -ne [IntPtr]::Zero) {
        for ($i = 0; $i -lt $downCount; $i++) {
            [void][Win32Native]::SendMessageInt($lb, 0x0100, [IntPtr]0x28, [IntPtr]0)   # WM_KEYDOWN VK_DOWN
        }
        [void][Win32Native]::SendMessageInt($lb, 0x0100, [IntPtr]0x0D, [IntPtr]0)       # WM_KEYDOWN VK_RETURN
        $ran = Wait-File (Join-Path $wd $expectedFile)
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    return $ran
}

$ok23 = Invoke-ListSelection (Join-Path $sandbox 'Mx.exe') $sandbox 0 'a1.txt'
Add-Result 'list select: 1st item cmd target runs (a1.txt)' $ok23
$ok24 = Invoke-ListSelection (Join-Path $sandbox 'Mx.exe') $sandbox 1 'b2.txt'
Add-Result 'list select: 2nd item cmd target runs (b2.txt)' $ok24
$ok25 = Invoke-ListSelection (Join-Path $sandbox 'Mx.exe') $sandbox 2 'c3.txt'
Add-Result 'list select: 3rd item ps target runs (c3.txt)' $ok25

# 26) admin marker combination without command body: no launch, clean exit (no UAC)
$exited26 = Invoke-Sync (Join-Path $sandbox 'Ra.exe') @() $sandbox
Add-Result 'admin markers without body: no launch, clean exit' $exited26

# 27) single command containing a comma (no runme marker) must run as-is, not as a list
$exited27 = Invoke-Sync (Join-Path $sandbox 'Cc.exe') @() $sandbox
$file27 = Join-Path $sandbox 'comma.txt'
[void](Wait-File $file27)
$content27 = ''
if (Test-Path $file27) { $content27 = (Get-Content $file27 -Raw).Trim() }
Add-Result 'single command with comma: not treated as list' ($exited27 -and $content27 -eq 'a,b') ("content='$content27'")

# 28) config value with runme marker and a single entry -> runs directly (no list window)
$exited28 = Invoke-Sync (Join-Path $sandbox 'R1.exe') @() $sandbox
$file28 = Join-Path $sandbox 'r1.txt'
[void](Wait-File $file28)
$content28 = ''
if (Test-Path $file28) { $content28 = (Get-Content $file28 -Raw).Trim() }
Add-Result 'runme single entry in config: runs directly, no list' ($exited28 -and $content28 -eq 'r1') ("content='$content28'")

# 29/30) window visibility: default hidden vs show marker
function Measure-ConsoleWindows([string]$file, [string]$wd) {
    $before = [Win32Native]::CountConsoleWindows()
    $p = Start-Process -FilePath $file -WorkingDirectory $wd -PassThru
    $maxDelta = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 2500) {
        $delta = [Win32Native]::CountConsoleWindows() - $before
        if ($delta -gt $maxDelta) { $maxDelta = $delta }
        [System.Threading.Thread]::Sleep(150)
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    return $maxDelta
}
$d29 = Measure-ConsoleWindows (Join-Path $sandbox 'Hd.exe') $sandbox
$ok29 = ($d29 -eq 0) -and (Wait-File (Join-Path $sandbox 'hd.txt'))
Add-Result 'default: console window hidden (delta 0)' $ok29 ("maxDelta=$d29")
$d30 = Measure-ConsoleWindows (Join-Path $sandbox 'Sh.exe') $sandbox
$ok30 = ($d30 -ge 1) -and (Wait-File (Join-Path $sandbox 'sh.txt'))
Add-Result 'show marker: console window visible (delta>=1)' $ok30 ("maxDelta=$d30")

# 12/13) runmeth / runmefth in a separate sandbox
$sandboxR = Join-Path $env:TEMP 'RunMeTestR'
Remove-Item $sandboxR -Recurse -Force -ErrorAction SilentlyContinue
New-Item $sandboxR -ItemType Directory | Out-Null
Copy-Item $exe (Join-Path $sandboxR 'RunMe.exe')
Copy-Item $exe (Join-Path $sandboxR 'Zz.exe')
$iniR = @"
[Settings]
RunParentDirectory=$sandboxR

[Config]
RunMe=help
Ghost=help
"@
Set-Content -Path (Join-Path $sandboxR 'YanBinCfg.ini') -Value $iniR -Encoding UTF8

$exited = Invoke-Sync (Join-Path $sandboxR 'RunMe.exe') @('runmeth') $sandboxR
$h1 = (Get-FileHash (Join-Path $sandboxR 'RunMe.exe')).Hash
$h2 = ''
if (Test-Path (Join-Path $sandboxR 'Zz.exe')) { $h2 = (Get-FileHash (Join-Path $sandboxR 'Zz.exe')).Hash }
Add-Result 'runmeth: all copies overwritten by self' ($exited -and $h2 -ne '' -and $h2 -eq $h1)

$exited = Invoke-Sync (Join-Path $sandboxR 'RunMe.exe') @('runmefth') $sandboxR
$ghost = Test-Path (Join-Path $sandboxR 'Ghost.exe')
$h3 = ''
if ($ghost) { $h3 = (Get-FileHash (Join-Path $sandboxR 'Ghost.exe')).Hash }
Add-Result 'runmefth: create copies from [Config] keys' ($exited -and $ghost -and $h3 -eq $h1)

# 16) runme display names in list window (a|aaaa.exe,b|bbb.exe)
$p16 = Start-Process -FilePath (Join-Path $sandbox 'RunMe.exe') -ArgumentList 'runme', '"a|aaaa.exe,b|bbb.exe"' -WorkingDirectory $sandbox -PassThru
$items16 = @()
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$listBox = [IntPtr]::Zero
while ($sw.ElapsedMilliseconds -lt 8000) {
    $p16.Refresh()
    if ($p16.HasExited) { break }
    if ($p16.MainWindowHandle -ne [IntPtr]::Zero) {
        $listBox = [Win32Native]::FindListBox($p16.MainWindowHandle)
        if ($listBox -ne [IntPtr]::Zero) { break }
    }
    [System.Threading.Thread]::Sleep(100)
}
if ($listBox -ne [IntPtr]::Zero) {
    $count = [int][Win32Native]::SendMessageInt($listBox, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($i = 0; $i -lt $count; $i++) {
        $sb = New-Object System.Text.StringBuilder -ArgumentList 256
        [void][Win32Native]::SendMessageStr($listBox, 0x0189, [IntPtr]$i, $sb)
        $items16 += $sb.ToString()
    }
}
if (-not $p16.HasExited) { Stop-Process -Id $p16.Id -Force -ErrorAction SilentlyContinue }
$ok16 = ($items16.Count -eq 2) -and ($items16[0] -eq 'a') -and ($items16[1] -eq 'b')
Add-Result 'runme display names (a|aaaa.exe,b|bbb.exe)' $ok16 ("items='$($items16 -join ',' )'")

# ---------------- summary ----------------
Write-Host ''
$results | Format-Table -AutoSize
$failCount = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ("Passed {0} / {1}, failed {2}" -f ($results.Count - $failCount), $results.Count, $failCount)
exit $failCount
