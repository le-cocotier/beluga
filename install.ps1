<#
.SYNOPSIS
    Beluga installer for Windows (PowerShell 5.1 or 7+).

.DESCRIPTION
    The Windows counterpart of install.sh: builds beluga.exe (or installs a
    prebuilt one), puts it on your PATH, and sets up ~/.beluga (config,
    bundled packages) exactly as install.sh does.

    Safe to re-run: an existing build/ is reconfigured, and your config is
    never touched without asking first. Nothing in ~/.beluga changes until
    the build has succeeded. Bundled packages are replaced to
    match this checkout; one with local edits is moved to
    ~/.beluga/packages.bak/ first, never deleted. Packages that aren't
    bundled are never touched.

    Your own changes belong in ~/.beluga/packages/personal-config/init.lisp:
    it's created from a template on the first install and never overwritten
    afterwards (even with -Yes or -SkipPackages), so it survives updates.

    Run it with:
        powershell -ExecutionPolicy Bypass -File .\install.ps1

    It works from a clone of the repository (it builds beluga.exe) and from
    an unzipped release package (it installs the beluga.exe that comes with
    it).

.PARAMETER Yes
    Don't prompt; assume "yes" (and never overwrite an existing config).
.PARAMETER Prefix
    Where to install beluga.exe (default: %LOCALAPPDATA%\Programs\beluga).
.PARAMETER SkipPackages
    Don't install the bundled packages (packages\ in this checkout).
.PARAMETER Dev
    Link the bundled packages to this checkout (directory junctions)
    instead of copying them, for working on them.
.PARAMETER DebugBuild
    Build an unoptimized debug build with APP_DEBUG logging enabled; logs go
    to ~/.beluga/debug.log.
.PARAMETER Binary
    Install this prebuilt beluga.exe (e.g. from a CI artifact) instead of
    building one. No compiler needed then. Run from an unzipped release
    package, the script does this by itself with the beluga.exe next to it.
.PARAMETER Update
    git pull this clone first, then install the new version (with the other
    options given).
.PARAMETER Uninstall
    Remove beluga.exe and its PATH entry. ~/.beluga (your config and
    packages) is kept unless you also pass -Purge.
.PARAMETER Purge
    With -Uninstall: delete ~/.beluga as well.
#>
[CmdletBinding()]
param(
    [switch]$Yes,
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\beluga'),
    [switch]$SkipPackages,
    [switch]$Dev,
    [switch]$DebugBuild,
    [string]$Binary,
    [switch]$Update,
    [switch]$Uninstall,
    [switch]$Purge
)

# This file is deliberately plain ASCII: Windows PowerShell 5.1 reads a
# script without a byte-order mark in the legacy code page, so anything
# else would be garbled.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

# --- output helpers ---------------------------------------------------------

function Info($msg)  { Write-Host "==> $msg" -ForegroundColor Cyan }
function Ok($msg)    { Write-Host "[ok] $msg" -ForegroundColor Green }
function Warn($msg)  { Write-Host "[!] $msg" -ForegroundColor Yellow }
function Die($msg)   { Write-Host "[x] $msg" -ForegroundColor Red; exit 1 }

function Confirm-Step($question) {
    if ($Yes) { return $true }
    $reply = Read-Host "$question (y/N)"
    return $reply -match '^[Yy]$'
}

# A command just installed by winget lands on the machine/user PATH, which
# this session doesn't see yet: reload it from the registry.
function Update-SessionPath {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = (@($machine, $user) | Where-Object { $_ }) -join ';'
}

function Test-Command($name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

# --- sanity checks ----------------------------------------------------------

if ($Update -and $Uninstall) { Die '-Update and -Uninstall don''t go together' }
if ($Purge -and -not $Uninstall) { Die '-Purge only makes sense with -Uninstall' }

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

if (-not (Test-Path (Join-Path $ScriptDir 'assets\beluga.txt'))) {
    Die "This doesn't look like beluga ($ScriptDir). Run this script from the cloned repo, or from the unzipped release."
}
# Run from an unzipped release (beluga.exe next to this script, no source
# to build): install that beluga.exe, as -Binary would.
$PackagedExe = Join-Path $ScriptDir 'beluga.exe'
if (-not $Binary -and -not (Test-Path (Join-Path $ScriptDir 'meson.build'))) {
    if (-not (Test-Path $PackagedExe)) {
        Die "Neither beluga.exe nor the sources (meson.build) are in $ScriptDir."
    }
    $Binary = $PackagedExe
}
if ($Binary -and -not (Test-Path $Binary)) {
    Die "No such file: $Binary"
}

# beluga keeps its files in $HOME/.beluga, as on Linux and macOS; on
# Windows that's your profile folder (beluga.exe sets HOME from it too).
$BelugaHome = Join-Path $env:USERPROFILE '.beluga'

function Test-Junction($path) {
    $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    return $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}

function Test-SamePath($a, $b) {
    return $a.TrimEnd('\') -ieq $b.TrimEnd('\')
}

# --- uninstall --------------------------------------------------------------

if ($Uninstall) {
    Write-Host '--- Beluga uninstaller (Windows) ---'
    $installed = Join-Path $Prefix 'beluga.exe'
    if (Test-Path $installed) {
        if (Confirm-Step "Remove ${installed}?") {
            try {
                Remove-Item -Force $installed
            } catch {
                Die "Couldn't remove $installed - is beluga still running? Close it and re-run."
            }
            # Left over from an update while beluga was running.
            Remove-Item -Force -ErrorAction SilentlyContinue "$installed.old"
            if (-not (Get-ChildItem -Force $Prefix)) { Remove-Item $Prefix }
            Ok "Removed $installed"
        }
    } else {
        Warn "No beluga.exe in $Prefix"
    }

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @($userPath -split ';' | Where-Object { $_ })
    $kept = @($entries | Where-Object { -not (Test-SamePath $_ $Prefix) })
    if ($kept.Count -ne $entries.Count -and -not (Test-Path (Join-Path $Prefix 'beluga.exe'))) {
        [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
        Ok "Removed $Prefix from your user PATH"
    }

    if (-not (Test-Path $BelugaHome)) {
        # nothing to keep or delete
    } elseif ($Purge) {
        if (Confirm-Step "Delete $BelugaHome (your config, personal-config and every package) for good?") {
            # -Dev junctions first, on their own: Windows PowerShell 5.1's
            # Remove-Item -Recurse follows a junction and would delete the
            # checkout's packages it points to.
            $pkgDir = Join-Path $BelugaHome 'packages'
            if (Test-Path $pkgDir) {
                foreach ($p in Get-ChildItem -LiteralPath $pkgDir -Force) {
                    if (Test-Junction $p.FullName) { [IO.Directory]::Delete($p.FullName, $false) }
                }
            }
            Remove-Item -LiteralPath $BelugaHome -Recurse -Force
            Ok "Deleted $BelugaHome"
        }
    } else {
        Write-Host "Kept $BelugaHome (your config and packages); -Uninstall -Purge deletes it too."
    }
    exit 0
}

# --- update -----------------------------------------------------------------

if ($Update) {
    if (-not (Test-Path (Join-Path $ScriptDir '.git'))) {
        Die "-Update needs a git clone of beluga, and $ScriptDir isn't one. For a release, download the new zip and run its install.ps1."
    }
    if (-not (Test-Command 'git')) { Die '-Update needs git (winget install Git.Git).' }
    Info "Updating $ScriptDir ..."
    $before = & git rev-parse HEAD
    & git pull --ff-only
    if ($LASTEXITCODE -ne 0) { Die 'git pull failed (local changes or a diverged branch?) - sort it out and re-run.' }
    if ((& git rev-parse HEAD) -eq $before) { Ok 'Already up to date' }
    else { & git --no-pager log --oneline "$before..HEAD" }
    # Re-run the (possibly new) installer, so a changed install.ps1 is the
    # one doing the installing.
    $rerun = @{}
    foreach ($key in $PSBoundParameters.Keys) {
        if ($key -ne 'Update') { $rerun[$key] = $PSBoundParameters[$key] }
    }
    & $PSCommandPath @rerun
    exit $LASTEXITCODE
}

Write-Host '--- Beluga installer (Windows) ---'
if (-not (Confirm-Step "Install beluga from $ScriptDir to ${Prefix}?")) { Write-Host 'Aborted.'; exit 1 }

# --- dependencies -----------------------------------------------------------

# command -> winget package that provides it
$WingetPackages = @{
    'meson' = 'mesonbuild.meson'                    # Meson, with Ninja included
    'ninja' = 'mesonbuild.meson'
    'gcc'   = 'BrechtSanders.WinLibs.POSIX.UCRT'    # MinGW-w64 gcc (WinLibs)
    'git'   = 'Git.Git'
}

function Install-Dependency($command, [bool]$required) {
    if (Test-Command $command) { return }
    $package = $WingetPackages[$command]
    if ($required) { Write-Host "[x] Missing: $command" -ForegroundColor Red }
    else { Warn "Not found: $command (optional - git status, project search and the file finder use it)" }

    if (-not (Test-Command 'winget')) {
        if ($required) { Die "Install $command (e.g. winget install $package) and re-run this script." }
        return
    }
    if (Confirm-Step "Run 'winget install --id $package' now?") {
        & winget install --id $package --exact --accept-source-agreements --accept-package-agreements
        Update-SessionPath
        if ($required -and -not (Test-Command $command)) {
            Die "'$command' still isn't on PATH. Open a new terminal (or log out and back in) and re-run this script."
        }
    } elseif ($required) {
        Die "Install it yourself, e.g.: winget install --id $package"
    }
}

Info 'Checking dependencies ...'
if (-not $Binary) {
    Install-Dependency 'meson' $true
    Install-Dependency 'ninja' $true
    Install-Dependency 'gcc' $true
}
Install-Dependency 'git' $false
Ok 'Dependencies checked'

# --- build ------------------------------------------------------------------

# Build first: if it fails, ~/.beluga stays as it was, rather than holding
# new packages the old beluga.exe can't run.

$Exe = $Binary
if (-not $Exe) {
    if ($DebugBuild) {
        Info 'Building (debug, APP_DEBUG logging enabled) ...'
        $mesonArgs = @('-Dbuildtype=debug', '-Dc_args=-DAPP_DEBUG')
    } else {
        Info 'Building (release) ...'
        # -Dc_args= explicitly: a reconfigure keeps every option it isn't
        # given, so a previous -DebugBuild's -DAPP_DEBUG would otherwise stay.
        $mesonArgs = @('-Dbuildtype=release', '-Dc_args=')
    }
    # MinGW gcc, even when Visual Studio's compiler is also around (it isn't
    # supported: the code uses gcc extensions).
    $env:CC = 'gcc'
    # --reconfigure only on a directory meson has actually set up: on an
    # empty or half-created build\ it fails.
    if (Test-Path (Join-Path $ScriptDir 'build\meson-private')) {
        & meson setup --reconfigure build @mesonArgs
    } else {
        & meson setup build @mesonArgs
    }
    if ($LASTEXITCODE -ne 0) { Die 'meson setup failed' }
    & meson compile -C build
    if ($LASTEXITCODE -ne 0) { Die "Build failed - nothing was installed, $BelugaHome is untouched." }
    $Exe = Join-Path $ScriptDir 'build\beluga.exe'
}

# --- config files -----------------------------------------------------------

Info "Setting up $BelugaHome ..."
foreach ($dir in 'assets', 'packages', 'config') {
    New-Item -ItemType Directory -Force -Path (Join-Path $BelugaHome $dir) | Out-Null
}
Copy-Item -Force (Join-Path $ScriptDir 'assets\beluga.txt') (Join-Path $BelugaHome 'assets\')

$ConfigFile = Join-Path $BelugaHome 'config\init.lisp'
$DefaultConfig = Join-Path $ScriptDir 'config\init.lisp'
if (Test-Path $ConfigFile) {
    if ($Yes) {
        Copy-Item -Force $DefaultConfig "$ConfigFile.new"
        Warn "Kept your existing config; new default written to $ConfigFile.new"
    } elseif (Confirm-Step 'Replace your existing config (old one saved as init.lisp.bak)?') {
        Move-Item -Force $ConfigFile "$ConfigFile.bak"
        Copy-Item -Force $DefaultConfig $ConfigFile
        Ok "Config replaced (backup at $ConfigFile.bak)"
    } else {
        Copy-Item -Force $DefaultConfig "$ConfigFile.new"
        Ok "Kept your config; new default written to $ConfigFile.new"
    }
} else {
    Copy-Item $DefaultConfig $ConfigFile
    Ok "Config installed at $ConfigFile"
}

# personal-config: yours, created once from the template and never
# overwritten afterwards, so edits there survive every update.
$PersonalConfig = Join-Path $BelugaHome 'packages\personal-config\init.lisp'
if (Test-Path $PersonalConfig) {
    Write-Host '  personal-config already exists, leaving it alone'
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $PersonalConfig) | Out-Null
    Copy-Item (Join-Path $ScriptDir 'config\personal-config.lisp') $PersonalConfig
    Ok "Personal config created at $PersonalConfig"
}
if (-not (Select-String -Path $ConfigFile -Pattern '^\s*\(add-package\s*"personal-config"\)' -Quiet)) {
    Warn "$ConfigFile doesn't load personal-config - add (add-package `"personal-config`") after the theme's add-package line"
}
# use-package comes from the package manager: a config from before it
# existed doesn't load it, and every use-package line would fail.
if (-not (Select-String -Path $ConfigFile -Pattern '^\s*\(add-package\s*"package"\)' -Quiet)) {
    Warn "$ConfigFile doesn't load the package manager - add (add-package `"package`") before the other add-package lines (see config\init.lisp)"
}

# --- packages ---------------------------------------------------------------

# Same rules as install.sh: a bundled package in ~/.beluga/packages is
# beluga's to replace on every install, so it stays in step with the
# binary; anything that would be lost doing so is moved to packages.bak.

$PackagesSrc = Join-Path $ScriptDir 'packages'
$PackagesDst = Join-Path $BelugaHome 'packages'
$PackagesBak = Join-Path $BelugaHome 'packages.bak'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

# Same files with the same contents? (lisp_log.err aside: the editor writes
# it into whatever directory it runs from.)
function Test-SameTree($a, $b) {
    $ignore = @('lisp_log.err', 'desktop.ini', '.bundled')
    $filesA = @(Get-ChildItem -LiteralPath $a -Recurse -File -Force | Where-Object { $ignore -notcontains $_.Name })
    $filesB = @(Get-ChildItem -LiteralPath $b -Recurse -File -Force | Where-Object { $ignore -notcontains $_.Name })
    if ($filesA.Count -ne $filesB.Count) { return $false }
    foreach ($f in $filesA) {
        $rel = $f.FullName.Substring($a.Length)
        $other = Join-Path $b $rel
        if (-not (Test-Path -LiteralPath $other)) { return $false }
        if ((Get-FileHash -LiteralPath $f.FullName).Hash -ne (Get-FileHash -LiteralPath $other).Hash) { return $false }
    }
    return $true
}

function Backup-Package($installed, $name, $why) {
    New-Item -ItemType Directory -Force -Path $PackagesBak | Out-Null
    $target = Join-Path $PackagesBak "$name-$Stamp"
    Move-Item -LiteralPath $installed $target
    Warn "'$name' $why - moved to $target"
}

function Install-Package($name) {
    $src = Join-Path $PackagesSrc $name
    $dest = Join-Path $PackagesDst $name

    if (Test-Junction $dest) {
        if ($Dev -and ((Get-Item -LiteralPath $dest -Force).Target -contains $src)) { return }
        # A junction holds nothing of its own: remove just the link.
        [IO.Directory]::Delete($dest, $false)
    } elseif (Test-Path -LiteralPath (Join-Path $dest '.git')) {
        Backup-Package $dest $name 'was a git clone (now bundled)'
    } elseif (Test-Path -LiteralPath $dest -PathType Container) {
        if (Test-SameTree $src $dest) {
            if (-not $Dev) {
                # An install from before the .bundled marker
                New-Item -ItemType File -Force -Path (Join-Path $dest '.bundled') | Out-Null
                return
            }
            Remove-Item -LiteralPath $dest -Recurse -Force
        } else {
            Backup-Package $dest $name 'had local changes'
        }
    } elseif (Test-Path -LiteralPath $dest) {
        Backup-Package $dest $name "wasn't a directory"
    }

    if ($Dev) {
        # A junction rather than a symlink: no administrator rights or
        # Developer Mode needed.
        New-Item -ItemType Junction -Path $dest -Target $src | Out-Null
        Write-Host "  $name -> $src"
    } else {
        Copy-Item -LiteralPath $src -Destination $dest -Recurse
        # Marks it as beluga's own: the *packages* list shows it as "bundled".
        New-Item -ItemType File -Force -Path (Join-Path $dest '.bundled') | Out-Null
        Write-Host "  $name"
    }
}

if ($SkipPackages) {
    Info 'Skipping bundled packages (-SkipPackages)'
} elseif (-not (Test-Path $PackagesSrc)) {
    Warn 'No packages\ directory in this checkout - skipping bundled packages'
} else {
    if ($Dev) { Info "Linking bundled packages to $PackagesSrc (-Dev) ..." }
    else { Info 'Installing bundled packages ...' }
    foreach ($pkg in Get-ChildItem -LiteralPath $PackagesSrc -Directory) {
        Install-Package $pkg.Name
    }
    Ok 'Bundled packages up to date'
}

# --- install the binary -----------------------------------------------------

Info "Installing to $Prefix ..."
New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
$Target = Join-Path $Prefix 'beluga.exe'
# A running beluga.exe can't be overwritten, but it can be renamed: move it
# aside first, so updating doesn't need every beluga closed.
$Old = "$Target.old"
Remove-Item -Force -ErrorAction SilentlyContinue $Old
if (Test-Path $Target) {
    try { Move-Item -Force $Target $Old } catch { }
}
try {
    Copy-Item -Force $Exe $Target
} catch {
    if ((Test-Path $Old) -and -not (Test-Path $Target)) { Move-Item $Old $Target }
    Die "Couldn't write $Target - is beluga still running? Close it and re-run."
}
# Fails, harmlessly, while the old one is still running; the next install
# or -Uninstall cleans it up.
Remove-Item -Force -ErrorAction SilentlyContinue $Old

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$onPath = @($userPath -split ';' | Where-Object { $_ -and (Test-SamePath $_ $Prefix) }).Count -gt 0
if (-not $onPath) {
    if (Confirm-Step "Add $Prefix to your PATH?") {
        $newPath = (@($userPath, $Prefix) | Where-Object { $_ }) -join ';'
        [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
        $env:Path = "$env:Path;$Prefix"
        Ok "Added $Prefix to your user PATH (new terminals will see it)"
    } else {
        Warn "$Prefix isn't on your PATH - run it as $Prefix\beluga.exe, or add it yourself"
    }
}

Write-Host ''
$version = & (Join-Path $Prefix 'beluga.exe') --version
Ok "Installed $version to $Prefix\beluga.exe"
Write-Host "Put your customizations in $PersonalConfig - unlike"
Write-Host "$ConfigFile, it's never replaced by an update."
Write-Host 'Run beluga in Windows Terminal (or any Windows 10/11 console). Terminal panes'
Write-Host 'start PowerShell; set BELUGA_SHELL to use another shell.'
