# Checks the --bind <ip> option (and LARECOMP_BIND) on the real programs larecomp_master_server, larecomp_rmr_server and
# larecomp_relay_server: it starts each one on 127.0.0.1, asks Windows which addresses the process listens on (nothing may
# listen on 0.0.0.0 or ::), reaches it from a client on 127.0.0.1, and shows that 127.0.0.2 (also loopback) is refused.
# It also runs the trio together on loopback (a relay announcing itself to a master, the tool reading both), the order
# --bind > LARECOMP_BIND, and every wrong value (the program must stop with an error, never listen on every interface).
#
#   powershell -NoProfile -File server_bind_check.ps1 -BinDir <folder> [-BasePort 43100] [-Work <folder>]
#
# <folder> holds larecomp_master_server.exe, larecomp_rmr_server.exe, larecomp_relay_server.exe and, for the client
# checks, larecomp_online_tool.exe (those checks are skipped without it). Build them with the clang line of each source
# (no CMake or SDK needed), or from CMake: the servers are plain targets.
#
# Loopback only. It never starts a server on every interface (the default), so Windows Firewall has nothing to ask
# about; "the default is every interface" is checked by bind_address_test on the number. Needs ports BasePort..BasePort+60
# free. Exit code 0 = every check held.

param(
    [Parameter(Mandatory = $true)][string]$BinDir,
    [int]$BasePort = 43100,
    [string]$Work = (Join-Path $env:TEMP 'server-bind-check')
)

$ErrorActionPreference = 'Stop'
$script:checks = 0
$script:failed = 0
$script:running = @()
$script:nextPort = $BasePort

function Check([bool]$Ok, [string]$What) {
    $script:checks++
    if (-not $Ok) {
        $script:failed++
        Write-Host "FAILED: $What"
    }
}

function New-Port([int]$Count = 2) {
    $p = $script:nextPort
    $script:nextPort += $Count
    return $p
}

# Starts one server from its own folder (the registry, state and data files land there). $Environment: name -> value.
function Start-Server([string]$Name, [string]$Exe, [string[]]$ServerArgs, [hashtable]$Environment = @{}, [hashtable]$Files = @{}) {
    $dir = Join-Path $Work $Name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $src = Join-Path $BinDir $Exe
    if (-not (Test-Path $src)) { throw "missing $src" }
    Copy-Item $src $dir
    foreach ($f in $Files.Keys) { Set-Content -Path (Join-Path $dir $f) -Value $Files[$f] -Encoding ascii }  # before it starts
    $saved = @{}
    foreach ($k in $Environment.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
        [Environment]::SetEnvironmentVariable($k, $Environment[$k], 'Process')
    }
    try {
        $splat = @{
            FilePath               = (Join-Path $dir $Exe)
            WorkingDirectory       = $dir
            RedirectStandardOutput = (Join-Path $dir 'out.txt')
            RedirectStandardError  = (Join-Path $dir 'err.txt')
            PassThru               = $true
            WindowStyle            = 'Hidden'
        }
        if ($ServerArgs -and $ServerArgs.Count -gt 0) {
            # Start-Process joins the arguments with spaces: an empty one or one with a space needs its quotes.
            $splat['ArgumentList'] = @($ServerArgs | ForEach-Object { if ($_ -eq '' -or $_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } })
        }
        $proc = Start-Process @splat
    } finally {
        foreach ($k in $Environment.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
    $null = $proc.Handle  # without it ExitCode can come back empty for a process that has already stopped
    $srv = [pscustomobject]@{ Name = $Name; Proc = $proc; Dir = $dir }
    $script:running += $srv
    return $srv
}

function Stop-Server($Srv) {
    if ($Srv.Proc -and -not $Srv.Proc.HasExited) { Stop-Process -Id $Srv.Proc.Id -Force -ErrorAction SilentlyContinue }
    $script:running = @($script:running | Where-Object { $_.Name -ne $Srv.Name })
}

function Get-Log($Srv) {
    $text = ''
    foreach ($f in 'out.txt', 'err.txt') {
        $p = Join-Path $Srv.Dir $f
        if (Test-Path $p) { $text += (Get-Content -Raw $p) }
    }
    return $text
}

# Every socket the process listens on, as "tcp 127.0.0.1:43100" / "udp 127.0.0.1:43100".
function Get-Listeners($Srv) {
    $out = @()
    foreach ($c in @(Get-NetTCPConnection -State Listen -OwningProcess $Srv.Proc.Id -ErrorAction SilentlyContinue)) {
        $out += "tcp $($c.LocalAddress):$($c.LocalPort)"
    }
    foreach ($e in @(Get-NetUDPEndpoint -OwningProcess $Srv.Proc.Id -ErrorAction SilentlyContinue)) {
        $out += "udp $($e.LocalAddress):$($e.LocalPort)"
    }
    return $out
}

# Waits until the server listens on the given "tcp ip:port" / "udp ip:port" entries, or has stopped.
function Wait-Listening($Srv, [string[]]$Want) {
    for ($i = 0; $i -lt 60; $i++) {
        if ($Srv.Proc.HasExited) { return $false }
        $have = Get-Listeners $Srv
        $missing = @($Want | Where-Object { $have -notcontains $_ })
        if ($missing.Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

# After a server is up: it listens on these sockets, on no wildcard address, and on no address but theirs (a relay that
# announces to a master has one more socket, on an ephemeral port: it must sit on the same address).
function Check-Only($Srv, [string[]]$Want, [string]$Label) {
    $have = Get-Listeners $Srv
    Check ((@($Want | Where-Object { $have -notcontains $_ })).Count -eq 0) "$Label listens on $($Want -join ', ') (has: $($have -join ', '))"
    $wild = @($have | Where-Object { $_ -match ' (0\.0\.0\.0|::|\[::\]):' })
    Check ($wild.Count -eq 0) "$Label listens on no wildcard address (has: $($wild -join ', '))"
    $addresses = @($Want | ForEach-Object { ($_ -split ' ')[1] -replace ':\d+$', '' } | Select-Object -Unique)
    $other = @($have | Where-Object { $addresses -notcontains (($_ -split ' ')[1] -replace ':\d+$', '') })
    Check ($other.Count -eq 0) "$Label listens only on $($addresses -join ', ') (also: $($other -join ', '))"
}

function Test-TcpConnect([string]$Ip, [int]$Port, [int]$TimeoutMs = 700) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $ar = $c.BeginConnect($Ip, $Port, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne($TimeoutMs)) { return $false }
        try { $c.EndConnect($ar) } catch { return $false }
        return $c.Connected
    } finally {
        $c.Close()
    }
}

function Get-Http([string]$Url, [string]$Method = 'GET') {
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Uri $Url -Method $Method -TimeoutSec 6
        return [pscustomobject]@{ Status = [int]$r.StatusCode; Body = [string]$r.Content }
    } catch {
        $resp = $_.Exception.Response
        if ($resp) { return [pscustomobject]@{ Status = [int]$resp.StatusCode; Body = '' } }
        return [pscustomobject]@{ Status = 0; Body = '' }
    }
}

# Runs a program that must stop by itself (a wrong option): its exit code and everything it printed. A program that is
# still running after 8 s did not refuse; it is killed and counted as a failure by the caller (the exit code is -999).
function Invoke-Refusal([string]$Name, [string]$Exe, [string[]]$ServerArgs, [hashtable]$Environment = @{}) {
    $srv = Start-Server $Name $Exe $ServerArgs $Environment
    $done = $srv.Proc.WaitForExit(8000)
    $code = if ($done) { $srv.Proc.ExitCode } else { -999 }
    $listeners = if ($done) { @() } else { Get-Listeners $srv }
    $log = Get-Log $srv
    Stop-Server $srv
    return [pscustomobject]@{ Code = $code; Log = $log; Listeners = $listeners; Dir = $srv.Dir }
}

function Read-Json([string]$Text) {
    try { return $Text | ConvertFrom-Json } catch { return $null }
}

if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$tool = Join-Path $BinDir 'larecomp_online_tool.exe'
$haveTool = Test-Path $tool
$haveSecondLoopback = $true
try {
    $t = New-Object System.Net.Sockets.TcpListener ([System.Net.IPAddress]::Parse('127.0.0.2')), 0
    $t.Start()
    $t.Stop()
} catch {
    $haveSecondLoopback = $false
    Write-Host 'SKIP: this machine cannot bind 127.0.0.2; the "refuses the other loopback address" checks are not run'
}

try {
    # ---- the Rate My Ride server on 127.0.0.1 -----------------------------------------------------------------------
    $port = New-Port
    $rmr = Start-Server 'rmr' 'larecomp_rmr_server.exe' @('--bind', '127.0.0.1', '--port', "$port", '--data', (Join-Path $Work 'rmr\data'))
    Check (Wait-Listening $rmr @("tcp 127.0.0.1:$port")) 'rmr: starts and listens on 127.0.0.1'
    Check-Only $rmr @("tcp 127.0.0.1:$port") 'rmr (--bind 127.0.0.1)'
    $r = Get-Http "http://127.0.0.1:$port/rmr/v1"
    Check ($r.Status -eq 200 -and $r.Body -like '*Rate My Ride server*') "rmr: a client on 127.0.0.1 reaches it (status $($r.Status))"
    if ($haveSecondLoopback) { Check (-not (Test-TcpConnect '127.0.0.2' $port)) 'rmr: 127.0.0.2 is refused (it would answer on every interface)' }
    Check ((Get-Log $rmr) -like '*bind: 127.0.0.1 (this machine only), from --bind*') 'rmr: the log says what it bound'
    Stop-Server $rmr

    # ---- the master on 127.0.0.1 (UDP and TCP) -------------------------------------------------------------------------
    $mport = New-Port
    $master = Start-Server 'master' 'larecomp_master_server.exe' @('--port', "$mport", '--bind', '127.0.0.1')
    Check (Wait-Listening $master @("tcp 127.0.0.1:$mport", "udp 127.0.0.1:$mport")) 'master: starts and listens on 127.0.0.1, UDP and TCP'
    Check-Only $master @("tcp 127.0.0.1:$mport", "udp 127.0.0.1:$mport") 'master (--bind 127.0.0.1)'
    $r = Get-Http "http://127.0.0.1:$mport/"
    Check ($r.Status -eq 200 -and $r.Body -like '*larecomp master server*') "master: a client on 127.0.0.1 reaches it (status $($r.Status))"
    if ($haveSecondLoopback) { Check (-not (Test-TcpConnect '127.0.0.2' $mport)) 'master: 127.0.0.2 is refused' }
    Check ((Get-Log $master) -like '*bind: 127.0.0.1 (this machine only), from --bind*') 'master: the log says what it bound'

    # ---- the relay on 127.0.0.1, announcing itself to that master: the trio on loopback ----------------------------------
    $rport = New-Port
    $relayDir = Join-Path $Work 'relay'
    $relay = Start-Server 'relay' 'larecomp_relay_server.exe' @('--bind', '127.0.0.1', '--port', "$rport") @{} @{ 'server.toml' = "name = `"bind check`"`nmaster = `"127.0.0.1:$mport`"" }
    Check (Wait-Listening $relay @("udp 127.0.0.1:$rport", "tcp 127.0.0.1:$($rport + 1)")) 'relay: starts and listens on 127.0.0.1 (UDP relay, TCP dashboard)'
    Check-Only $relay @("udp 127.0.0.1:$rport", "tcp 127.0.0.1:$($rport + 1)") 'relay (--bind 127.0.0.1)'
    if ($haveSecondLoopback) { Check (-not (Test-TcpConnect '127.0.0.2' ($rport + 1))) 'relay: the dashboard refuses 127.0.0.2' }
    Check ((Get-Log $relay) -like '*bind: 127.0.0.1 (this machine only), from --bind*') 'relay: the log says what it bound'
    if ($haveTool) {
        $j = Read-Json ((& $tool servers --servers "127.0.0.1:$rport" --timeout-ms 1500 | Out-String))
        Check ($j -and $j.ok -and $j.servers.Count -eq 1 -and $j.servers[0].answered) 'relay: the online tool on 127.0.0.1 gets its answer'
    }
    # The relay announces to the master named in its server.toml (written before it started).
    $listed = $false
    for ($i = 0; $i -lt 40 -and -not $listed; $i++) {
        Start-Sleep -Milliseconds 500
        $r = Get-Http "http://127.0.0.1:$mport/servers" 'POST'
        $listed = $r.Status -eq 200 -and $r.Body -like "*127.0.0.1:$rport*"
    }
    Check $listed 'trio: the master on 127.0.0.1 lists the relay on 127.0.0.1 (announce, probe and proof, all on loopback)'
    # The relay's announcing socket sends from a socket of its own: with the master on loopback it must not sit on a wildcard address.
    $have = Get-Listeners $relay
    $wild = @($have | Where-Object { $_ -match ' (0\.0\.0\.0|::|\[::\]):' })
    Check ($wild.Count -eq 0) "relay while announcing to a master on 127.0.0.1: nothing on a wildcard address (has: $($have -join ', '))"
    if ($haveTool) {
        $j = Read-Json ((& $tool servers --master "127.0.0.1:$mport" --timeout-ms 1500 | Out-String))
        Check ($j -and $j.master.state -eq 'ok' -and $j.summary.servers -ge 1 -and $j.servers[0].listed) 'trio: the online tool reads the list from the master and the relay behind it'
    }
    Stop-Server $relay
    Stop-Server $master

    # ---- the same from the environment, and the order --bind > LARECOMP_BIND --------------------------------------------
    $port = New-Port
    $rmr = Start-Server 'rmr-env' 'larecomp_rmr_server.exe' @('--port', "$port", '--data', (Join-Path $Work 'rmr-env\data')) @{ LARECOMP_BIND = '127.0.0.1' }
    Check (Wait-Listening $rmr @("tcp 127.0.0.1:$port")) 'rmr: LARECOMP_BIND=127.0.0.1 alone puts it on 127.0.0.1'
    Check-Only $rmr @("tcp 127.0.0.1:$port") 'rmr (LARECOMP_BIND)'
    Check ((Get-Log $rmr) -like '*bind: 127.0.0.1 (this machine only), from LARECOMP_BIND*') 'rmr: the log names LARECOMP_BIND as the source'
    Stop-Server $rmr
    if ($haveSecondLoopback) {
        $port = New-Port
        $rmr = Start-Server 'rmr-order' 'larecomp_rmr_server.exe' @('--bind', '127.0.0.2', '--port', "$port", '--data', (Join-Path $Work 'rmr-order\data')) @{ LARECOMP_BIND = '127.0.0.1' }
        Check (Wait-Listening $rmr @("tcp 127.0.0.2:$port")) 'rmr: --bind 127.0.0.2 beats LARECOMP_BIND=127.0.0.1'
        Check-Only $rmr @("tcp 127.0.0.2:$port") 'rmr (--bind over LARECOMP_BIND)'
        Check (-not (Test-TcpConnect '127.0.0.1' $port)) 'rmr: and 127.0.0.1 is refused then'
        Stop-Server $rmr
    }
    $port = New-Port
    $master = Start-Server 'master-env' 'larecomp_master_server.exe' @('--port', "$port") @{ LARECOMP_BIND = '127.0.0.1' }
    Check (Wait-Listening $master @("tcp 127.0.0.1:$port", "udp 127.0.0.1:$port")) 'master: LARECOMP_BIND=127.0.0.1 alone puts it on 127.0.0.1'
    Check-Only $master @("tcp 127.0.0.1:$port", "udp 127.0.0.1:$port") 'master (LARECOMP_BIND)'
    Stop-Server $master
    $port = New-Port
    $relay = Start-Server 'relay-env' 'larecomp_relay_server.exe' @('--port', "$port") @{ LARECOMP_BIND = '127.0.0.1' }
    Check (Wait-Listening $relay @("udp 127.0.0.1:$port", "tcp 127.0.0.1:$($port + 1)")) 'relay: LARECOMP_BIND=127.0.0.1 alone puts it on 127.0.0.1'
    Check-Only $relay @("udp 127.0.0.1:$port", "tcp 127.0.0.1:$($port + 1)") 'relay (LARECOMP_BIND)'
    Stop-Server $relay

    # ---- other ways to write it: --bind=<ip>, and the relay's older positional form ---------------------------------------
    $port = New-Port
    $master = Start-Server 'master-eq' 'larecomp_master_server.exe' @("--bind=127.0.0.1", "--port", "$port")
    Check (Wait-Listening $master @("tcp 127.0.0.1:$port", "udp 127.0.0.1:$port")) 'master: --bind=127.0.0.1 works'
    Stop-Server $master
    $port = New-Port
    $rmr = Start-Server 'rmr-eq' 'larecomp_rmr_server.exe' @("--bind=localhost", "--port", "$port", '--data', (Join-Path $Work 'rmr-eq\data'))
    Check (Wait-Listening $rmr @("tcp 127.0.0.1:$port")) 'rmr: --bind=localhost is 127.0.0.1'
    Stop-Server $rmr
    $port = New-Port
    $relay = Start-Server 'relay-eq' 'larecomp_relay_server.exe' @("--bind=127.0.0.1", "--port=$port")
    Check (Wait-Listening $relay @("udp 127.0.0.1:$port")) 'relay: --bind=127.0.0.1 --port=N works'
    Stop-Server $relay
    $port = New-Port
    $relay = Start-Server 'relay-pos' 'larecomp_relay_server.exe' @('127.0.0.1', "$port")
    Check (Wait-Listening $relay @("udp 127.0.0.1:$port", "tcp 127.0.0.1:$($port + 1)")) 'relay: the older positional form (bind_ip port) still works'
    Check-Only $relay @("udp 127.0.0.1:$port", "tcp 127.0.0.1:$($port + 1)") 'relay (positional)'
    Check ((Get-Log $relay) -like '*bind: 127.0.0.1 (this machine only), from argument*') 'relay: the log names the argument as the source'
    Stop-Server $relay

    # ---- wrong values: the program stops with an error and listens on nothing ---------------------------------------------
    foreach ($bad in 'example.com', '::1', '127.0.0.1:31000', '224.0.0.1', '127.0.0.256', '010.0.0.1', '') {
        $label = if ($bad -eq '') { '<empty>' } else { $bad }
        $port = New-Port
        $x = Invoke-Refusal 'bad-rmr' 'larecomp_rmr_server.exe' @('--bind', $bad, '--port', "$port", '--data', (Join-Path $Work 'bad-rmr\data'))
        Check ($x.Code -eq 2) "rmr --bind '$label' stops with exit code 2 (got $($x.Code); listening: $($x.Listeners -join ', '))"
        Check ($x.Log -like '*--bind*') "rmr --bind '$label' says why: $($x.Log.Trim())"
        Check (-not (Test-Path (Join-Path $x.Dir 'data'))) "rmr --bind '$label' creates no data folder"
        $x = Invoke-Refusal 'bad-master' 'larecomp_master_server.exe' @('--bind', $bad, '--port', "$port")
        Check ($x.Code -eq 2) "master --bind '$label' stops with exit code 2 (got $($x.Code); listening: $($x.Listeners -join ', '))"
        Check ($x.Log -like '*--bind*') "master --bind '$label' says why: $($x.Log.Trim())"
        Check (@(Get-ChildItem $x.Dir -File | Where-Object { $_.Name -notin 'larecomp_master_server.exe', 'out.txt', 'err.txt' }).Count -eq 0) "master --bind '$label' creates no registry files"
        $x = Invoke-Refusal 'bad-relay' 'larecomp_relay_server.exe' @('--bind', $bad, '--port', "$port")
        Check ($x.Code -eq 2) "relay --bind '$label' stops with exit code 2 (got $($x.Code); listening: $($x.Listeners -join ', '))"
        Check ($x.Log -like '*--bind*') "relay --bind '$label' says why: $($x.Log.Trim())"
    }
    foreach ($exe in 'larecomp_rmr_server.exe', 'larecomp_master_server.exe', 'larecomp_relay_server.exe') {
        $port = New-Port
        $x = Invoke-Refusal 'bad-env' $exe @('--port', "$port") @{ LARECOMP_BIND = 'bogus.example' }
        Check ($x.Code -eq 2) "$exe with LARECOMP_BIND=bogus.example stops with exit code 2 (got $($x.Code); listening: $($x.Listeners -join ', '))"
        Check ($x.Log -like '*LARECOMP_BIND*') "$exe with a wrong LARECOMP_BIND says so: $($x.Log.Trim())"
        # --bind is read first, but a wrong LARECOMP_BIND is no reason to stop a server that was told where to listen.
        $port = New-Port
        $srv = Start-Server 'env-over' $exe @('--bind', '127.0.0.1', '--port', "$port") @{ LARECOMP_BIND = 'bogus.example' }
        $kind = if ($exe -like '*rmr*') { "tcp 127.0.0.1:$port" } else { "udp 127.0.0.1:$port" }
        Check (Wait-Listening $srv @($kind)) "$exe with --bind 127.0.0.1 starts although LARECOMP_BIND is wrong"
        Stop-Server $srv
    }
    # The relay's two forms are not mixed, and --port is strict.
    $x = Invoke-Refusal 'mix' 'larecomp_relay_server.exe' @('--bind', '127.0.0.1', '31000')
    Check ($x.Code -eq 2 -and $x.Listeners.Count -eq 0) "relay: --bind with a positional argument is refused (exit $($x.Code))"
    foreach ($badPort in '0', '65536', 'abc', '31000x', '') {
        $x = Invoke-Refusal 'badport' 'larecomp_relay_server.exe' @('--bind', '127.0.0.1', '--port', $badPort)
        Check ($x.Code -eq 2 -and $x.Listeners.Count -eq 0) "relay: --port '$badPort' is refused (exit $($x.Code))"
    }
    $x = Invoke-Refusal 'helprelay' 'larecomp_relay_server.exe' @('--help')
    Check ($x.Code -eq 0 -and $x.Log -like '*--bind*') 'relay: --help prints the usage with --bind and stops'
    $x = Invoke-Refusal 'helprmr' 'larecomp_rmr_server.exe' @('--help')
    Check ($x.Code -eq 0 -and $x.Log -like '*--bind*') 'rmr: --help names --bind'
    $x = Invoke-Refusal 'usagemaster' 'larecomp_master_server.exe' @('--nope')
    Check ($x.Code -eq 2 -and $x.Log -like '*--bind*') 'master: the usage line names --bind'
} finally {
    foreach ($s in @($script:running)) { Stop-Server $s }
}

Write-Host "server_bind_check: $script:checks checks, $script:failed failed"
if ($script:failed -eq 0 -and (Test-Path $Work)) { Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue }
exit $(if ($script:failed -eq 0) { 0 } else { 1 })
