Set-StrictMode -Version Latest

function Find-GatewayRepositoryRoot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$StartPath
    )

    $candidate = Get-Item -LiteralPath $StartPath
    if (-not $candidate.PSIsContainer) {
        $candidate = $candidate.Directory
    }

    while ($null -ne $candidate) {
        $cmakeFile = Join-Path $candidate.FullName "CMakeLists.txt"
        $runtimeInclude = Join-Path $candidate.FullName "include\edge_gateway"
        if ((Test-Path -LiteralPath $cmakeFile -PathType Leaf) -and
            (Test-Path -LiteralPath $runtimeInclude -PathType Container)) {
            return $candidate.FullName
        }
        $candidate = $candidate.Parent
    }

    throw "Gateway repository root was not found from: $StartPath"
}
