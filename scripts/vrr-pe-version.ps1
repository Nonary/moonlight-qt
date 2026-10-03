# Map a GitHub CI_VERSION such as 6.1.0-vrr17 or 6.1.0-vrr17.1 to the
# three-field Windows PE/MSI ProductVersion. MSI compares only major.minor.build,
# so patch tags cannot reuse 6.2.N.
#
#   6.1.0-vrrN     (N <= 17)  ->  6.2.N
#   6.1.0-vrrN.P              ->  6.2.(N * 10 + P)   # vrr17.1 -> 6.2.171
#   6.1.0-vrrN     (N >= 18)  ->  6.2.(N * 10)       # vrr18   -> 6.2.180
param(
    [Parameter(Mandatory = $true)]
    [string]$CiVersion
)

if ($CiVersion -match '-vrr(\d+)\.(\d+)$') {
    Write-Output ('6.2.' + ([int]$Matches[1] * 10 + [int]$Matches[2]))
    exit 0
}

if ($CiVersion -match '-vrr(\d+)$') {
    $n = [int]$Matches[1]
    if ($n -le 17) {
        Write-Output "6.2.$n"
    } else {
        Write-Output ('6.2.' + ($n * 10))
    }
    exit 0
}

Write-Error "Cannot map CI_VERSION '$CiVersion' to a Windows PE/MSI version"
exit 1
