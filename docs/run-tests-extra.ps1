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
public static class W32 {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")] public static extern IntPtr SendMessageI(IntPtr h, int msg, IntPtr wp, IntPtr lp);
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
    $js = "require('fs').writeFileSync('" + $pfOut + "','pf-ok')"
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
    [void]$p6.WaitForExit(10000)
    $ok6 = $false
    $sw6 = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw6.ElapsedMilliseconds -lt 20000 -and -not $ok6) {
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

# ---------------- summary ----------------
Write-Host ''
$results | Format-Table -AutoSize
$failCount = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ("Passed {0} / {1}, failed {2}" -f ($results.Count - $failCount), $results.Count, $failCount)
exit $failCount
