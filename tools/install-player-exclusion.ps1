$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskSource = Join-Path $taskRoot 'build/companion/ds3sc_companion.dll'
$taskTarget = 'C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamlessCoop/ds3sc_companion.dll'
$taskExpected = '272A1B39B72BC990BBED16D4D98A38696216BA5CD14745315E663283D19C2938'
if (Get-Process -Name DarkSoulsIII -ErrorAction SilentlyContinue) { throw 'Close DS3 before installing.' }
if ((Get-FileHash -LiteralPath $taskSource).Hash -ne $taskExpected) { throw 'Candidate DLL changed since validation.' }
if (-not (Test-Path -LiteralPath $taskTarget)) { throw 'Installed DLL not found.' }
$taskBefore = (Get-FileHash -LiteralPath $taskTarget).Hash
$taskBackup = Join-Path $taskRoot ('build/backups/outline-player-exclusion/installed-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff') + '.dll')
Copy-Item -LiteralPath $taskTarget -Destination $taskBackup
try {
    Copy-Item -LiteralPath $taskSource -Destination $taskTarget -Force
    if ((Get-FileHash -LiteralPath $taskTarget).Hash -ne $taskExpected) { throw 'Copy verification failed.' }
} catch {
    Copy-Item -LiteralPath $taskBackup -Destination $taskTarget -Force
    throw
}
[ordered]@{
    revision = 'outline-local-player-exclusion'
    installed_at = (Get-Date).ToString('o')
    installed_dll = $taskTarget
    sha256 = $taskExpected
    previous_sha256 = $taskBefore
    backup = $taskBackup
    tests = 'build/outline/player-exclusion-validation.txt'
    native_validation = 'build/outline/player-exclusion-native.txt'
    visual_validation = 'Pending in-game overlap test; game was closed during installation.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskRoot 'analysis/outline-player-exclusion-installation.json') -Encoding UTF8
Write-Output 'DLL installed; SHA-256 verified. Previous version backed up.'
