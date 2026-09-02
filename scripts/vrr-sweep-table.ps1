<#
.SYNOPSIS
Print a sender-spacing cadence table from a vrrreplay batch JSON.

.EXAMPLE
& .\build\tests-vrr\vrr\release\vrrreplay.exe capture.vrrtrace `
    --config tests\vrr\configs\playout-delay-sweep-lowlatency.json --jobs 0 `
    --output build\sweep.json
.\scripts\vrr-sweep-table.ps1 build\sweep.json
#>
param([Parameter(Mandatory = $true)][string]$BatchJson)

$batch = Get-Content -LiteralPath $BatchJson -Raw | ConvertFrom-Json
$rows = New-Object System.Collections.Generic.List[object]
$first = $null
foreach ($scenario in $batch.scenarios) {
    $s = $scenario.summary
    if ($null -eq $s) { $s = $scenario }
    if ($null -eq $first) { $first = $s }
    $name = $scenario.scenario
    if ($null -eq $name -or $name -eq "") { $name = $scenario.name }
    $rows.Add([pscustomobject]@{
        scenario   = $name
        pjerk_p50  = $s.replay_presented_jerk_p50_us
        pjerk_p90  = $s.replay_presented_jerk_p90_us
        pjerk_gt2  = $s.replay_presented_jerk_over_2ms_per_mille
        err_p50    = $s.replay_sender_spacing_error_p50_us
        err_p90    = $s.replay_sender_spacing_error_p90_us
        err_p99    = $s.replay_sender_spacing_error_p99_us
        jerk_p99   = $s.replay_sender_jerk_p99_us
        hitches    = $s.replay_sender_hitches
        per_sec    = [math]::Round([double]$s.replay_sender_hitches_per_second, 2)
        late       = $s.replay_sender_hitch_late_arrivals
        lat_p50    = $s.replay_decode_to_submission_p50_us
        lat_p95    = $s.replay_decode_to_submission_p95_us
        delay_p50  = $s.replay_playout_delay_p50_us
        delay_max  = $s.replay_playout_delay_max_us
        saturated  = $(if ($s.replay_worker_saturated) { "YES" } else { "" })
    })
}
if ($null -ne $first) {
    $rows.Insert(0, [pscustomobject]@{
        scenario = "stock (present on render)"
        pjerk_p50 = $first.stock_presented_jerk_p50_us; pjerk_p90 = $first.stock_presented_jerk_p90_us; pjerk_gt2 = $first.stock_presented_jerk_over_2ms_per_mille
        err_p50 = $first.stock_sender_spacing_error_p50_us; err_p90 = $first.stock_sender_spacing_error_p90_us; err_p99 = $first.stock_sender_spacing_error_p99_us
        jerk_p99 = $first.stock_sender_jerk_p99_us; hitches = $first.stock_sender_hitches
        per_sec = [math]::Round([double]$first.stock_sender_hitches_per_second, 2); late = ""
        lat_p50 = ""; lat_p95 = ""; delay_p50 = ""; delay_max = ""; saturated = "" })
    $rows.Insert(1, [pscustomobject]@{
        scenario = "recorded session"
        pjerk_p50 = $first.original_presented_jerk_p50_us; pjerk_p90 = $first.original_presented_jerk_p90_us; pjerk_gt2 = $first.original_presented_jerk_over_2ms_per_mille
        err_p50 = $first.original_sender_spacing_error_p50_us; err_p90 = $first.original_sender_spacing_error_p90_us; err_p99 = $first.original_sender_spacing_error_p99_us
        jerk_p99 = $first.original_sender_jerk_p99_us; hitches = $first.original_sender_hitches
        per_sec = [math]::Round([double]$first.original_sender_hitches_per_second, 2); late = ""
        lat_p50 = $first.original_decode_to_submission_p50_us; lat_p95 = $first.original_decode_to_submission_p95_us; delay_p50 = ""; delay_max = ""; saturated = "" })
}
"trace: $($batch.trace)  pairs: $($first.replay_sender_pairs)  jobs: $($batch.parallel_jobs)  elapsed: $($batch.elapsed_ms) ms"
$rows | Format-Table -AutoSize
