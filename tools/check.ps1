# Runs every "must measure" line of SPEC.md against the built plugins and prints PASS/FAIL
# with the numbers. Usage: powershell -File tools\check.ps1 [drive|split|both] [-NoValidate]
param([string]$Which = 'both', [switch]$NoValidate)

$root = Split-Path $PSScriptRoot -Parent
$build = "$root\build\Release"
$tool = "$build\basstool_artefacts\Release\basstool.exe"
$plugins = @{
    drive = "$build\NightshiftBassDrive_artefacts\Release\VST3\Nightshift Bass Drive.vst3"
    split = "$build\NightshiftBassSplit_artefacts\Release\VST3\Nightshift Bass Split.vst3"
}
$sig = "$root\testdata\signals"
$tmp = "$root\testdata\out"
New-Item -ItemType Directory -Force $tmp | Out-Null
if (-not (Test-Path "$sig\gatetest.wav")) { & $tool --signals $sig | Out-Null }

$script:fails = 0
function Report([bool]$ok, [string]$what, [string]$numbers) {
    if (-not $ok) { $script:fails++ }
    "{0}  {1}  -  {2}" -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $what, $numbers
}

function Render([string]$vst, [string]$in, [string[]]$sets, [string]$name = 'r') {
    $out = "$tmp\$name.wav"
    $argv = @('--render', $vst, "$sig\$in", $out)
    foreach ($kv in $sets) { $argv += '--set'; $argv += $kv }
    $text = & $tool @argv | Out-String
    if ($text -notmatch 'rendered') { throw "render failed: $text" }
    return $out
}

function Measure-File([string]$file, [string[]]$opts) { return (& $tool --measure $file @opts | Out-String) }
function Get-Num([string]$text, [string]$pattern) { if ($text -match $pattern) { return [double]$Matches[1] } else { return [double]::NaN } }
function Get-Thd([string]$t)   { Get-Num $t 'THD ([\d.]+) %' }
function Get-Fund([string]$t)  { Get-Num $t 'fundamental [\d.]+ Hz at (-?[\d.]+) dBFS' }
function Get-Rms([string]$t)   { Get-Num $t 'rms (-?[\d.]+) dBFS' }
function Get-Spur([string]$t)  { Get-Num $t 'worst spur (-?[\d.]+) dB' }
function Get-Crest([string]$t) { Get-Num $t 'crest:\s+(-?[\d.]+) dB' }
function Get-Response([string]$t) {
    $map = @{}
    if ($t -match 'response:(.*?)\s+\(') {
        foreach ($pair in $Matches[1].Trim().Split(' ')) {
            $k, $v = $pair.Split('=')
            $map[$k] = [double]$v
        }
    }
    return $map
}
function Get-Latency([string]$vst) { Get-Num (& $tool --params $vst | Out-String) 'latency (\d+) samples' }
function Response([string]$vst, [string[]]$sets, [string]$freqs = '') {
    $f = Render $vst 'noise30.wav' $sets
    $opts = @('--response', '--ref', "$sig\noise30.wav")
    if ($freqs) { $opts += '--freqs'; $opts += $freqs }
    return Get-Response (Measure-File $f $opts)
}
function Flatness($resp, [string[]]$keys) {
    $vals = foreach ($k in $keys) { $resp[$k] }
    return (($vals | Measure-Object -Maximum).Maximum, ($vals | Measure-Object -Minimum).Minimum)
}
$flatKeys = @('30', '60', '120', '250', '500', '1.0k', '2.0k', '4.0k', '8.0k', '12k', '16k')

#==============================================================================
function Check-Drive {
    $v = $plugins.drive
    "===== Nightshift Bass Drive"
    $clean = @('Blend=0', 'Comp=0', 'Cab=Off')

    $t = Measure-File (Render $v 'sine1k.wav' $clean) @('--thd', '1000')
    $thd = Get-Thd $t; $lvl = (Get-Fund $t) + 12.04
    Report ($thd -lt 0.01 -and [math]::Abs($lvl) -le 0.1) 'clean path transparent (sine1k)' ("THD {0:N4} %, level {1:+0.00;-0.00} dB" -f $thd, $lvl)

    $max, $min = Flatness (Response $v $clean) $flatKeys
    Report ($max -le 0.3 -and $min -ge -0.3) 'clean path flat 30 Hz-16 kHz (noise30)' ("{0:N2} .. {1:N2} dB" -f $min, $max)

    $thds = foreach ($d in 0, 50, 100) { Get-Thd (Measure-File (Render $v 'sine82.wav' @('Blend=100', 'Comp=0', 'Cab=Off', 'Tone=100', "Drive=$d")) @('--thd', '82.4069')) }
    Report ($thds[0] -lt $thds[1] -and $thds[1] -lt $thds[2] -and $thds[2] -ge 20) 'THD rises with drive (sine82: 0/50/100)' (($thds | ForEach-Object { "{0:N2} %" -f $_ }) -join ' / ')

    foreach ($c in 'Smooth', 'Tight', 'Fuzz') {
        $t = Measure-File (Render $v 'sine1490.wav' @('Blend=100', 'Comp=0', 'Cab=Off', 'Tone=100', 'Drive=100', "Character=$c")) @('--alias', '1490')
        $spur = Get-Spur $t
        Report ($spur -le -60) "aliasing at drive 100, $c (sine1490)" ("worst spur {0:N1} dB" -f $spur)
    }

    # Touch: the grit follows how hard the string is hit, gradually (not a switch)
    foreach ($c in 'Smooth', 'Tight', 'Fuzz') {
        $curve = foreach ($in in -18, -9, 0, 9) { Get-Thd (Measure-File (Render $v 'sine82.wav' @('Blend=100', 'Comp=0', 'Cab=Off', 'Tone=100', 'Drive=40', "Character=$c", "Input=$in")) @('--thd', '82.4069')) }
        $rising = ($curve[0] -lt $curve[1]) -and ($curve[1] -lt $curve[2]) -and ($curve[2] -lt $curve[3])
        Report ($rising -and $curve[0] -le 0.5 * $curve[3]) "touch: THD follows input level, $c (sine82, input -18/-9/0/+9)" (($curve | ForEach-Object { "{0:N1} %" -f $_ }) -join ' / ')
    }

    # Memory: a soft note right after a loud one starts compressed (the circuit's bias and
    # supply are still recovering) and comes back. Measured as the note's own fundamental
    # (200 ms windows, so the DC shift's thump doesn't count), against the same soft note
    # played with nothing before it
    foreach ($c in 'Smooth', 'Tight', 'Fuzz') {
        $sets = @('Blend=100', 'Comp=0', 'Cab=Off', 'Tone=100', 'Drive=70', "Character=$c")
        $after = Render $v 'burst.wav' $sets 'burst'
        $alone = Render $v 'softalone.wav' $sets 'alone'
        $fund = { param($file, $from, $to) Get-Fund (Measure-File $file @('--thd', '82.4069', '--from', $from, '--to', $to)) }
        $early = (& $fund $after '1.02' '1.22') - (& $fund $alone '1.02' '1.22')
        $late = (& $fund $after '1.80' '2.00') - (& $fund $alone '1.80' '2.00')
        # Valve and fuzz sag audibly; the op-amp character is a regulated 9 V pedal and barely does
        $need = @{ Smooth = -0.5; Tight = 0.0; Fuzz = -0.3 }[$c]
        Report ($early -le $need -and [math]::Abs($late) -le 0.3) "memory: compressed after a loud note, then recovers, $c (burst)" ("{0:+0.00;-0.00} dB 20-220 ms after, {1:+0.00;-0.00} dB at 0.8-1 s (vs the note alone; needs <= {2})" -f $early, $late, $need)
    }

    $rms = foreach ($d in 0, 50, 100) { Get-Rms (Measure-File (Render $v 'bassdi.wav' @('Blend=100', 'Comp=0', 'Cab=Off', "Drive=$d")) @('--stats')) }
    $spread = ($rms | Measure-Object -Maximum).Maximum - ($rms | Measure-Object -Minimum).Minimum
    Report ($spread -le 4) 'loudness steady across drive (bassdi: 0/50/100)' ((($rms | ForEach-Object { "{0:N1}" -f $_ }) -join ' / ') + (" dBFS rms, spread {0:N1} dB" -f $spread))

    $pathRms = foreach ($b in 0, 60, 100) { Get-Rms (Measure-File (Render $v 'bassdi.wav' @('Comp=0', 'Cab=Off', "Blend=$b")) @('--stats')) }
    $louder = [math]::Max($pathRms[0], $pathRms[2])
    Report (($louder - $pathRms[1]) -le 2) 'blend 60 holds level (no cancelling between paths)' ("clean {0:N1}, blend 60 {1:N1}, drive {2:N1} dBFS rms" -f $pathRms[0], $pathRms[1], $pathRms[2])

    $dry = Measure-File "$sig\bassdi.wav" @('--stats', '--bands')
    $def = Measure-File (Render $v 'bassdi.wav' @()) @('--stats', '--bands')
    $dl = (Get-Rms $def) - (Get-Rms $dry); $sub = Get-Num $def 'sub ([\d.]+)'
    Report ([math]::Abs($dl) -le 2 -and $sub -ge 20) 'defaults: level with the input, low end kept (bassdi)' ("{0:+0.0;-0.0} dB vs dry, sub share {1:N1} % (dry {2:N1} %)" -f $dl, $sub, (Get-Num $dry 'sub ([\d.]+)'))

    $thin = Response $v @('Blend=100', 'Comp=0', 'Cab=Off', 'Drive=60', 'Grunt=Thin')
    $fat = Response $v @('Blend=100', 'Comp=0', 'Cab=Off', 'Drive=60', 'Grunt=Fat')
    Report (($fat['60'] - $thin['60']) -ge 10) 'grunt: Fat vs Thin at 60 Hz' ("{0:N1} dB more" -f ($fat['60'] - $thin['60']))

    $cut = Response $v @('Blend=100', 'Comp=0', 'Cab=Off', 'Drive=60', 'Attack=Cut')
    $boost = Response $v @('Blend=100', 'Comp=0', 'Cab=Off', 'Drive=60', 'Attack=Boost')
    Report (($boost['4.0k'] - $cut['4.0k']) -ge 6) 'attack: Boost vs Cut at 4 kHz' ("{0:N1} dB more" -f ($boost['4.0k'] - $cut['4.0k']))

    $m0 = Measure-File (Render $v 'bassdi.wav' @('Blend=0', 'Cab=Off', 'Comp=0')) @('--dynamics', '--stats')
    $m1 = Measure-File (Render $v 'bassdi.wav' @('Blend=0', 'Cab=Off', 'Comp=100')) @('--dynamics', '--stats')
    $r0 = Get-Num $m0 'range ([\d.]+) dB'; $r1 = Get-Num $m1 'range ([\d.]+) dB'; $dl = (Get-Rms $m1) - (Get-Rms $m0)
    Report (($r0 - $r1) -ge 3 -and [math]::Abs($dl) -le 3) 'comp 100 narrows the dynamics, level held (bassdi)' ("range {0:N1} -> {1:N1} dB, level {2:+0.0;-0.0} dB" -f $r0, $r1, $dl)

    $gated = Get-Rms (Measure-File (Render $v 'gatetest.wav' @('Blend=0', 'Comp=0', 'Cab=Off', 'Gate=-50')) @('--stats', '--from', '1.6'))
    $open = Get-Rms (Measure-File (Render $v 'gatetest.wav' @('Blend=0', 'Comp=0', 'Cab=Off')) @('--stats', '--from', '1.6'))
    Report ($gated -le -100 -and $open -gt -75) 'gate -50 shuts -70 dBFS hiss; Off passes it' ("gated {0:N1}, open {1:N1} dBFS rms" -f $gated, $open)

    $lat = Get-Latency $v
    $pos = Get-Num (Measure-File (Render $v 'impulse.wav' $clean) @('--peakpos')) 'sample (\d+)'
    Report (($pos - 4800) -eq $lat) 'latency matches an impulse (clean path)' ("reported {0}, impulse peak +{1}" -f $lat, ($pos - 4800))

    $centres = '60,160,400,1000,2500,6000'
    $keys = @('60', '160', '400', '1.0k', '2.5k', '6.0k')
    $names = @('EQ 60 Hz', 'EQ 160 Hz', 'EQ 400 Hz', 'EQ 1kHz', 'EQ 2.5kHz', 'EQ 6kHz')
    $gains = for ($b = 0; $b -lt 6; $b++) { (Response $v ($clean + ("{0}=12" -f $names[$b])) $centres)[$keys[$b]] }
    $okEq = ($gains | Where-Object { $_ -lt 11 -or $_ -gt 13 }).Count -eq 0
    Report $okEq 'each EQ band +12 at its centre' (($gains | ForEach-Object { "{0:N1}" -f $_ }) -join ' / ')

    $offRms = Get-Rms (Measure-File (Render $v 'bassdi.wav' @('Blend=0', 'Comp=0', 'Cab=Off')) @('--stats'))
    foreach ($cab in '4x10', '8x10', '1x15', '2x12') {
        $resp = Response $v @('Blend=0', 'Comp=0', "Cab=$cab")
        $drop = $resp['1.0k'] - $resp['8.0k']
        $cr = Get-Rms (Measure-File (Render $v 'bassdi.wav' @('Blend=0', 'Comp=0', "Cab=$cab")) @('--stats'))
        Report ($drop -ge 20 -and [math]::Abs($cr - $offRms) -le 2) "cab $cab" ("8k {0:N1} dB under 1k; bassdi {1:+0.0;-0.0} dB vs Off" -f $drop, ($cr - $offRms))
    }

    $cpu = & $tool --cpu $v --block 32 | Out-String
    $pct = Get-Num $cpu '= ([\d.]+) % of one core'; $ms = Get-Num $cpu '\(([\d.]+) ms\)'
    Report ($pct -lt 5 -and $ms -lt 1) 'CPU at 32-sample blocks, latency' ("{0:N2} % of a core, {1:N2} ms" -f $pct, $ms)

    if (-not $NoValidate) { & $tool --validate $v | Select-String 'FAIL|PASSED|FAILED' | ForEach-Object { "validate: " + $_.Line } }
}

#==============================================================================
function Check-Split {
    $v = $plugins.split
    "===== Nightshift Bass Split"
    $linear = @('Low Comp=0', 'Mid Drive=0', 'High Drive=0', 'Low Level=0', 'Mid Level=0', 'High Level=0', 'Cab=Off')

    $max, $min = Flatness (Response $v $linear) $flatKeys
    $resp = Response $v $linear
    $max, $min = Flatness $resp @('30', '60', '120', '250', '500', '1.0k', '2.0k', '4.0k', '8.0k', '12k')
    Report ($max -le 0.5 -and $min -ge -0.5) 'bands sum flat 30 Hz-12 kHz (noise30, drives 0)' ("{0:N2} .. {1:N2} dB; 16 kHz {2:N2} dB" -f $min, $max, $resp['16k'])

    $t = Measure-File (Render $v 'sine41.wav' @('Low Comp=0', 'Mid Level=-40', 'High Level=-40', 'Cab=Off')) @('--thd', '41.2034')
    Report ((Get-Thd $t) -lt 0.05) 'low band clean (sine41)' ("THD {0:N4} %" -f (Get-Thd $t))

    $low = Response $v @('Low Comp=0', 'Mid Level=-40', 'High Level=-40', 'Cab=Off') '60,640'
    Report ([math]::Abs($low['60']) -le 1 -and $low['640'] -le -24) 'low band alone: 60 Hz passes, 640 Hz down' ("60 Hz {0:N2} dB, 640 Hz {1:N1} dB" -f $low['60'], $low['640'])

    $high = Response $v @('Low Level=-40', 'Mid Level=-40', 'High Drive=0', 'Cab=Off') '120,4000'
    Report (($high['4.0k'] - $high['120']) -ge 30) 'high band alone: 120 Hz under 4 kHz' ("{0:N1} dB under" -f ($high['4.0k'] - $high['120']))

    $t = Measure-File (Render $v 'sine440.wav' @('Low Level=-40', 'High Level=-40', 'Mid Drive=100', 'Cab=Off')) @('--thd', '440')
    Report ((Get-Thd $t) -ge 15) 'mid drive 100 (sine440)' ("THD {0:N1} %" -f (Get-Thd $t))

    foreach ($c in 'Smooth', 'Tight', 'Fuzz') {
        $t = Measure-File (Render $v 'sine1490.wav' @('Low Level=-40', 'Mid Drive=100', 'High Drive=100', 'Cab=Off', "Character=$c")) @('--alias', '1490')
        Report ((Get-Spur $t) -le -60) "aliasing at drive 100, $c (sine1490)" ("worst spur {0:N1} dB" -f (Get-Spur $t))
    }

    $dry = Measure-File "$sig\bassdi.wav" @('--stats')
    $dl = (Get-Rms (Measure-File (Render $v 'bassdi.wav' @()) @('--stats'))) - (Get-Rms $dry)
    Report ([math]::Abs($dl) -le 2) 'defaults: level with the input (bassdi)' ("{0:+0.0;-0.0} dB vs dry" -f $dl)

    $rms = foreach ($d in 0, 50, 100) { Get-Rms (Measure-File (Render $v 'bassdi.wav' @('Cab=Off', "Mid Drive=$d")) @('--stats')) }
    $spread = ($rms | Measure-Object -Maximum).Maximum - ($rms | Measure-Object -Minimum).Minimum
    Report ($spread -le 4) 'loudness steady across mid drive (bassdi: 0/50/100)' ((($rms | ForEach-Object { "{0:N1}" -f $_ }) -join ' / ') + (" dBFS rms, spread {0:N1} dB" -f $spread))

    $lat = Get-Latency $v
    $pos = Get-Num (Measure-File (Render $v 'impulse.wav' $linear) @('--peakpos')) 'sample (\d+)'
    Report ([math]::Abs(($pos - 4800) - $lat) -le 1) 'latency matches an impulse (linear settings)' ("reported {0}, impulse peak +{1}" -f $lat, ($pos - 4800))

    $cpu = & $tool --cpu $v --block 32 | Out-String
    $pct = Get-Num $cpu '= ([\d.]+) % of one core'; $ms = Get-Num $cpu '\(([\d.]+) ms\)'
    Report ($pct -lt 8 -and $ms -lt 1) 'CPU at 32-sample blocks, latency' ("{0:N2} % of a core, {1:N2} ms" -f $pct, $ms)

    if (-not $NoValidate) { & $tool --validate $v | Select-String 'FAIL|PASSED|FAILED' | ForEach-Object { "validate: " + $_.Line } }
}

if ($Which -eq 'drive' -or $Which -eq 'both') { Check-Drive }
if ($Which -eq 'split' -or $Which -eq 'both') { Check-Split }
"{0} spec line(s) failed" -f $script:fails
