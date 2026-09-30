# RunMeCpp extended smoke tests: list-window interactions + path prefixes + formats
# (ASCII only, PS 5.1 compatible)
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\run-tests-extra.ps1 [-Exe <path>]
param([string]$Exe = '')
$ErrorActionPreference = 'Stop'

$projRoot = Split-Path -Parent $PSScriptRoot
$exe = $Exe
if (-not $exe) { $exe = Join-Path $projRoot 'bin\Release\RunMeCpp.exe' }
if (-not (Test-Path $exe)) { $exe = Join-Path $projRoot 'bin\Debug\RunMeCpp.exe' }
if (-not (Test-Path $exe)) { throw "Build first: $exe not found" }

$sandbox = Join-Path $env:TEMP 'RunMeCppExtra'
$results = New-Object System.Collections.ArrayList

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left, Top, Right, Bottom; }
public static class W32 {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")] public static extern IntPtr SendMessageI(IntPtr h, int msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public static int WindowWidth(IntPtr h) {
        RECT r;
        if (!GetWindowRect(h, out r)) return 0;
        return r.Right - r.Left;
    }
    public static IntPtr FindListBox(IntPtr parent) {
        IntPtr found = IntPtr.Zero;
        EnumProc cb = (h, l) => {
            var sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            if (sb.ToString().ToUpperInvariant().Contains("LISTBOX")) { found = h; return false; }
            return true;
        };
        EnumChildWindows(parent, cb, IntPtr.Zero);
        GC.KeepAlive(cb);
        return found;
    }
}
'@

function Add-Result([string]$name, [bool]$ok, [string]$info = '') {
    if ($ok) { $res = 'PASS' } else { $res = 'FAIL' }
    [void]$results.Add([pscustomobject]@{ Case = $name; Result = $res; Info = $info })
    Write-Host ("[{0}] {1} {2}" -f $res, $name, $info)
}

function Wait-File([string]$path, [int]$timeoutMs = 6000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if (Test-Path $path) { return $true }
        [System.Threading.Thread]::Sleep(100)
    }
    return (Test-Path $path)
}

# poll until the file exists AND has content (cmd creates the file before writing it)
function Get-FileText([string]$path, [int]$timeoutMs = 8000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if (Test-Path $path) {
            $raw = Get-Content $path -Raw -ErrorAction SilentlyContinue
            if ($raw) { return $raw.Trim() }
        }
        [System.Threading.Thread]::Sleep(100)
    }
    return ''
}

function Get-ListBoxHandle([System.Diagnostics.Process]$p, [int]$timeoutMs = 8000) {
    $lb = [IntPtr]::Zero
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs -and $lb -eq [IntPtr]::Zero) {
        $p.Refresh()
        if ($p.HasExited) { break }
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $lb = [W32]::FindListBox($p.MainWindowHandle) }
        if ($lb -eq [IntPtr]::Zero) { [System.Threading.Thread]::Sleep(100) }
    }
    return $lb
}

# ---------------- prepare sandbox ----------------
Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
New-Item $sandbox -ItemType Directory | Out-Null
New-Item (Join-Path $sandbox 'nested') -ItemType Directory | Out-Null

Copy-Item $exe (Join-Path $sandbox 'Dx.exe')
Copy-Item $exe (Join-Path $sandbox 'Fmt.exe')
Copy-Item $exe (Join-Path $sandbox 'App.exe')
Copy-Item $exe (Join-Path $sandbox 'Pf.exe')
Copy-Item $exe (Join-Path $sandbox 'Runner3.exe')
Copy-Item $exe (Join-Path $sandbox 'nested\Up.exe')

$d1 = Join-Path $sandbox 'd1.txt'
$d2 = Join-Path $sandbox 'd2.txt'
$fmtFile = Join-Path $sandbox 'fmt.txt'
$nestedOk = Join-Path $sandbox 'nested.txt'

# AppData runner (its own folder + own config)
$appDir = Join-Path $env:USERPROFILE 'AppData\Local\Temp\RunMeCppExtraApp'
Remove-Item $appDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item $appDir -ItemType Directory | Out-Null
Copy-Item $exe (Join-Path $appDir 'Runner2.exe')
$appOk = Join-Path $sandbox 'appdata.txt'
@"
[Settings]
RunParentDirectory=$appDir
[Config]
Runner2=cmd echo appdata-ok>$appOk
"@ | Set-Content -Path (Join-Path $appDir 'YanBinCfg.ini') -Encoding UTF8

# nested runner base dir with a TRAILING backslash in RunParentDirectory
@"
[Settings]
RunParentDirectory=$sandbox\nested\
[Config]
Up=..\Runner3.exe
"@ | Set-Content -Path (Join-Path $sandbox 'nested\YanBinCfg.ini') -Encoding UTF8

# pf\ test program (pick a console app under Program Files that writes a file)
$pfOut = Join-Path $sandbox 'pf.txt'
$pfLine = ''
$pfCheck = ''
$pfIsDir = $false
if (Test-Path 'C:\Program Files\nodejs\node.exe') {
    # NOTE: backslashes inside the JS string literal are escapes ('\r' becomes CR,
    # '\t' a tab...), which mangles a Windows path - pass forward slashes instead.
    $jsPath = $pfOut.Replace('\', '/')
    $js = "require('fs').writeFileSync('" + $jsPath + "','pf-ok')"
    $pfLine = 'Pf=pf\nodejs\node.exe -e "' + $js + '"'
    $pfCheck = $pfOut
} elseif (Test-Path 'C:\Program Files\dotnet\dotnet.exe') {
    $pfProjDir = Join-Path $sandbox 'pfproj'
    $pfLine = 'Pf=pf\dotnet\dotnet.exe new sln -o ' + $pfProjDir
    $pfCheck = $pfProjDir
    $pfIsDir = $true
} elseif (Test-Path 'C:\Program Files\PowerShell\7\pwsh.exe') {
    $pfLine = 'Pf=pf\PowerShell\7\pwsh.exe -NoProfile -Command "Set-Content -Path ''' + $pfOut + ''' -Value pf-ok"'
    $pfCheck = $pfOut
}

# main sandbox config
$iniLines = New-Object System.Collections.ArrayList
[void]$iniLines.Add('[Settings]')
[void]$iniLines.Add("RunParentDirectory=$sandbox")
[void]$iniLines.Add('ExcludeExeName=RunMe|MeRun')
$iniLines.Add('# countdown off: these cases must keep the window open while driving the list')
[void]$iniLines.Add('ListAutoRunSeconds=0')
[void]$iniLines.Add('')
[void]$iniLines.Add('[Config]')
[void]$iniLines.Add("Dx=runme D1|cmd echo d1>$d1,D2|cmd echo d2>$d2")
# NOTE: wrap placeholders in brackets - cmd treats "digit>" (e.g. "5>file") as a handle redirect
[void]$iniLines.Add("Fmt=cmd echo [{time.yyyyMMdd}] [{random.-3-3}]>$fmtFile")
[void]$iniLines.Add('App=AppData\Local\Temp\RunMeCppExtraApp\Runner2.exe')
[void]$iniLines.Add("Runner3=cmd echo nested-ok>$nestedOk")
if ($pfLine -ne '') { [void]$iniLines.Add($pfLine) }
Set-Content -Path (Join-Path $sandbox 'YanBinCfg.ini') -Value $iniLines -Encoding UTF8

# ---------------- cases ----------------

# 1) double-click in list window runs the first item
$p = Start-Process -FilePath (Join-Path $sandbox 'Dx.exe') -WorkingDirectory $sandbox -PassThru
$lb = Get-ListBoxHandle $p
if ($lb -ne [IntPtr]::Zero) {
    # WM_COMMAND, wParam = (LBN_DBLCLK << 16) | controlId(1), lParam = listbox hwnd
    [void][W32]::SendMessageI($p.MainWindowHandle, 0x0111, [IntPtr]((2 -shl 16) -bor 1), $lb)
}
$c1 = Get-FileText $d1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Add-Result 'list double-click runs 1st item (d1.txt)' ($c1 -eq 'd1') ("value='$c1'")

# 2) mouse wheel down + Enter runs the second item
$p2 = Start-Process -FilePath (Join-Path $sandbox 'Dx.exe') -WorkingDirectory $sandbox -PassThru
$lb2 = Get-ListBoxHandle $p2
if ($lb2 -ne [IntPtr]::Zero) {
    [void][W32]::SendMessageI($lb2, 0x020A, [IntPtr](-7864320), [IntPtr]::Zero)   # WM_MOUSEWHEEL delta -120
    [void][W32]::SendMessageI($lb2, 0x0100, [IntPtr]0x0D, [IntPtr]::Zero)         # WM_KEYDOWN VK_RETURN
}
$c2 = Get-FileText $d2
if (-not $p2.HasExited) { Stop-Process -Id $p2.Id -Force -ErrorAction SilentlyContinue }
Add-Result 'wheel down + Enter runs 2nd item (d2.txt)' ($c2 -eq 'd2') ("value='$c2'")

# 3) {time.yyyyMMdd} + {random.-3-3}
$p3 = Start-Process -FilePath (Join-Path $sandbox 'Fmt.exe') -WorkingDirectory $sandbox -PassThru
[void]$p3.WaitForExit(10000)
$c3 = Get-FileText $fmtFile
Add-Result 'time yyyyMMdd + random -3..3' ($c3 -match '^\[\d{8}\] \[-?\d\]$') ("value='$c3'")

# 4) AppData path prefix
$p4 = Start-Process -FilePath (Join-Path $sandbox 'App.exe') -WorkingDirectory $sandbox -PassThru
[void]$p4.WaitForExit(10000)
$c4 = Get-FileText $appOk
Add-Result 'AppData path prefix launch' ($c4 -eq 'appdata-ok') ("value='$c4'")

# 5) ..\ relative path when RunParentDirectory has a trailing backslash
$p5 = Start-Process -FilePath (Join-Path $sandbox 'nested\Up.exe') -WorkingDirectory (Join-Path $sandbox 'nested') -PassThru
[void]$p5.WaitForExit(10000)
$c5 = Get-FileText $nestedOk
Add-Result '..\ with trailing-backslash base dir' ($c5 -eq 'nested-ok') ("value='$c5'")

# 6) pf\ path prefix (Program Files)
if ($pfLine -ne '') {
    $p6 = Start-Process -FilePath (Join-Path $sandbox 'Pf.exe') -WorkingDirectory $sandbox -PassThru
    # dotnet/node first run after a cold start can be slow on CI - allow more time
    [void]$p6.WaitForExit(30000)
    $ok6 = $false
    $sw6 = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw6.ElapsedMilliseconds -lt 45000 -and -not $ok6) {
        if ($pfIsDir) {
            if ((Test-Path $pfCheck) -and (@(Get-ChildItem $pfCheck -File -Recurse -ErrorAction SilentlyContinue).Count -gt 0)) { $ok6 = $true }
        } else {
            if ((Test-Path $pfCheck) -and ((Get-Item $pfCheck).Length -gt 0)) { $ok6 = $true }
        }
        if (-not $ok6) { [System.Threading.Thread]::Sleep(200) }
    }
    Add-Result 'pf prefix resolves under Program Files' $ok6 ("check='$pfCheck'")
} else {
    Add-Result 'pf prefix resolves under Program Files' $false 'no suitable console app found under Program Files'
}

# ---------------- list countdown / arrow-key cycle ----------------

# send WM_KEYDOWN to the listbox (VK_DOWN = 0x28 / VK_UP = 0x26 / VK_RETURN = 0x0D)
function Send-ListKey([IntPtr]$lb, [int]$vk, [int]$times = 1) {
    for ($i = 0; $i -lt $times; $i++) { [void][W32]::SendMessageI($lb, 0x0100, [IntPtr]$vk, [IntPtr]0) }
}

function New-AutoSandbox([string]$dir, [string]$exeName, [string]$configLines) {
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item $dir -ItemType Directory | Out-Null
    Copy-Item $exe (Join-Path $dir $exeName)
    $configLines | Set-Content -Path (Join-Path $dir 'YanBinCfg.ini') -Encoding UTF8
}

# 7) countdown: remaining seconds shown in the title bar + first item auto-runs when idle
#    title suffix: "(N <sec-word><after-word><start-word><run-word> <name>)" - matched via \u escapes
$autoDir = Join-Path $env:TEMP 'RunMeCppAuto'
$au1 = Join-Path $autoDir 'au1.txt'
$au2 = Join-Path $autoDir 'au2.txt'
$au3 = Join-Path $autoDir 'au3.txt'
New-AutoSandbox $autoDir 'Auto.exe' @"
[Settings]
RunParentDirectory=$autoDir
[Config]
Auto=runme A1|cmd echo au1>$au1,A2|cmd echo au2>$au2,A3|cmd echo au3>$au3
"@

$pa = Start-Process -FilePath (Join-Path $autoDir 'Auto.exe') -WorkingDirectory $autoDir -PassThru
$title7 = ''
$width7 = 0
$sw7 = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw7.ElapsedMilliseconds -lt 3000 -and $title7 -eq '' -and -not $pa.HasExited) {
    $pa.Refresh()
    if ($pa.MainWindowTitle) {
        $title7 = $pa.MainWindowTitle
        $width7 = [W32]::WindowWidth($pa.MainWindowHandle)
    }
    [System.Threading.Thread]::Sleep(100)
}
Add-Result 'countdown shown in title bar' ($title7 -match '\u79D2\u540E\u542F\u52A8') ("title='$title7'")
Add-Result 'list window width not below the classic 402' ($width7 -ge 402) ("width=$width7")

$exited7 = $pa.WaitForExit(12000)
$c7 = Get-FileText $au1
if (-not $pa.HasExited) { Stop-Process -Id $pa.Id -Force -ErrorAction SilentlyContinue }
$ok7 = $exited7 -and ($c7 -eq 'au1') -and (-not (Test-Path $au2)) -and (-not (Test-Path $au3))
Add-Result 'countdown auto-runs the first item after timeout' $ok7 ("exited=$exited7 value='$c7'")

# 8) ListAutoRunSeconds=0 -> window stays open until the user acts
$offDir = Join-Path $env:TEMP 'RunMeCppAutoOff'
$of1 = Join-Path $offDir 'of1.txt'
$of2 = Join-Path $offDir 'of2.txt'
$of3 = Join-Path $offDir 'of3.txt'
New-AutoSandbox $offDir 'Off.exe' @"
[Settings]
RunParentDirectory=$offDir
ListAutoRunSeconds=0
[Config]
Off=runme B1|cmd echo of1>$of1,B2|cmd echo of2>$of2,B3|cmd echo of3>$of3
"@

$po = Start-Process -FilePath (Join-Path $offDir 'Off.exe') -WorkingDirectory $offDir -PassThru
$lbo = Get-ListBoxHandle $po
[System.Threading.Thread]::Sleep(6500)
$po.Refresh()
$na = -not $po.HasExited; $nf = -not (Test-Path $of1)
$title8 = $po.MainWindowTitle
if (-not $po.HasExited) { Stop-Process -Id $po.Id -Force -ErrorAction SilentlyContinue }
Add-Result 'ListAutoRunSeconds=0 disables auto start' ($na -and $nf -and ($title8 -notmatch '\u79D2\u540E\u542F\u52A8') -and ($lbo -ne [IntPtr]::Zero)) ("alive=$na nofile=$nf title='$title8'")

# 9) arrow down past the last item wraps to the first
$p9 = Start-Process -FilePath (Join-Path $offDir 'Off.exe') -WorkingDirectory $offDir -PassThru
$lb9 = Get-ListBoxHandle $p9
if ($lb9 -ne [IntPtr]::Zero) {
    Send-ListKey $lb9 0x28 3          # 3 items: down x3 must wrap back to item 1
    Send-ListKey $lb9 0x0D
}
$c9 = Get-FileText $of1
if (-not $p9.HasExited) { Stop-Process -Id $p9.Id -Force -ErrorAction SilentlyContinue }
Add-Result 'arrow down wraps last -> first item' ($c9 -eq 'of1') ("value='$c9'")

# 10) arrow up from the first item wraps to the last
$p10 = Start-Process -FilePath (Join-Path $offDir 'Off.exe') -WorkingDirectory $offDir -PassThru
$lb10 = Get-ListBoxHandle $p10
if ($lb10 -ne [IntPtr]::Zero) {
    Send-ListKey $lb10 0x26          # up x1: item 1 -> item 3 (last)
    Send-ListKey $lb10 0x0D
}
$c10 = Get-FileText $of3
if (-not $p10.HasExited) { Stop-Process -Id $p10.Id -Force -ErrorAction SilentlyContinue }
Add-Result 'arrow up wraps first -> last item' ($c10 -eq 'of3') ("value='$c10'")

# 11) any user action cancels the countdown (nothing auto-runs afterwards)
Remove-Item $au1, $au2, $au3 -Force -ErrorAction SilentlyContinue
$pb = Start-Process -FilePath (Join-Path $autoDir 'Auto.exe') -WorkingDirectory $autoDir -PassThru
$lbb = Get-ListBoxHandle $pb
$titleB = ''
if ($lbb -ne [IntPtr]::Zero) {
    $pb.Refresh()
    $titleB = $pb.MainWindowTitle
    Send-ListKey $lbb 0x28 1                 # VK_DOWN -> user takes over, countdown must go away
    [System.Threading.Thread]::Sleep(600)
    $pb.Refresh()
}
$titleC = $pb.MainWindowTitle
[System.Threading.Thread]::Sleep(6000)       # longer than the 5s default
$pb.Refresh()
$aliveB = -not $pb.HasExited
$nofilesB = (-not (Test-Path $au1)) -and (-not (Test-Path $au2)) -and (-not (Test-Path $au3))
if (-not $pb.HasExited) { Stop-Process -Id $pb.Id -Force -ErrorAction SilentlyContinue }
$okB = $aliveB -and $nofilesB -and ($titleB -match '\u79D2\u540E\u542F\u52A8') -and ($titleC -notmatch '\u79D2\u540E\u542F\u52A8')
Add-Result 'user action cancels the countdown' $okB ("alive=$aliveB nofiles=$nofilesB title='$titleC'")

# ---------------- summary ----------------
Write-Host ''
$results | Format-Table -AutoSize
$failCount = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ("Passed {0} / {1}, failed {2}" -f ($results.Count - $failCount), $results.Count, $failCount)
exit $failCount
