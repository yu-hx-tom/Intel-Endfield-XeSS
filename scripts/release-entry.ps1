#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([ValidateSet('Verify','Install','Uninstall','Launch')][string]$Action='Verify')
$ErrorActionPreference='Stop'
try {
    if(-not [Environment]::Is64BitProcess){throw 'Use 64-bit PowerShell 7.'}
    $json=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-manifest.json') -Raw | ConvertFrom-Json
    if($json.schema -ne 1 -or $json.flavor -cne 'release-minimal'){throw 'Invalid release manifest.'}
    foreach($entry in $json.files){
        if($entry.path -cnotmatch '^[A-Za-z0-9._/-]+$' -or $entry.path.StartsWith('/') -or $entry.path.Split('/') -contains '..'){throw 'Unsafe package path.'}
        $file=Join-Path $PSScriptRoot $entry.path
        if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256){throw "Package integrity check failed: $($entry.path)"}
    }
    $manager=Join-Path $PSScriptRoot 'manage-observer-install.ps1'
    if($Action -in @('Verify','Install')){
        $plan=& $manager -Action ReviewPlan -SourceDirectory (Join-Path $PSScriptRoot 'payload/observer') -WithModLauncher -ModPayloadDirectory (Join-Path $PSScriptRoot 'mod') -WithXeFGSdk | ConvertFrom-Json
        if($plan.changesPerformed -or $plan.copiedFiles.Count -ne 12){throw 'Unexpected payload review.'}
        if($Action -eq 'Verify'){
            [ordered]@{passed=$true;flavor=$json.flavor;version=$json.version;files=$json.files.Count;installationChanged=$false}|ConvertTo-Json
            return
        }
        $s=$plan.sourceHashes
        $arguments=@{Action='Install';SourceDirectory=(Join-Path $PSScriptRoot 'payload/observer');WithModLauncher=$true;ModPayloadDirectory=(Join-Path $PSScriptRoot 'mod');WithXeFGSdk=$true
            ExpectedDllSha256=$s.dllSha256;ExpectedManifestSha256=$s.manifestSha256;ExpectedSmokeSha256=$s.smokeSha256;ExpectedScriptSha256=$s.scriptSha256
            ExpectedModLauncherSha256=$s.modLauncher.launcherSha256;ExpectedEfuSha256=$s.modLauncher.efuSha256;ExpectedEfuLicenseSha256=$s.modLauncher.licenseSha256;ExpectedEfuNoticeSha256=$s.modLauncher.noticeSha256
            ExpectedXeFGDllSha256=$s.xeFGSdk.xeFGDllSha256;ExpectedXellDllSha256=$s.xeFGSdk.xellDllSha256;ExpectedIntelLicenseSha256=$s.xeFGSdk.intelLicenseSha256}
        & $manager @arguments
        if($LASTEXITCODE){throw "Installation failed: $LASTEXITCODE"}
        return
    }
    $installed=Join-Path $env:ProgramFiles 'EndfieldXeSSObserver'
    if($Action -eq 'Uninstall'){
        # The protected installed manager validates its own receipt and files.
        & (Join-Path $installed 'manage-observer-install.ps1') -Action Uninstall
        if($LASTEXITCODE){throw "Uninstall failed: $LASTEXITCODE"}
    }else{
        Start-Process -FilePath (Join-Path $installed 'EndfieldXeSSLauncher.exe') -WindowStyle Normal
    }
}catch{
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
