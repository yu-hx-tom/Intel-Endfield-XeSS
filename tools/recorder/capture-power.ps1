#requires -Version 7.0
#requires -PSEdition Core
# Passive MAHM parser reused from the P2 capture; the recorder owns its stop marker.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$TargetPid,
    [Parameter(Mandatory=$true)][uint64]$TargetCreationFileTime,[string]$OutputDirectory,[switch]$SelfTest,
    [ValidateRange(1,86400)][int]$DurationSeconds=86400,[string]$StopFile='stop.request',[string]$ReadyFile='power.ready.json',
    [switch]$CaptureHardware,[switch]$CaptureForeground,[switch]$CaptureOperationKeys=$true)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 7){throw 'The collector requires PowerShell 7 (pwsh.exe).'}
$powerShellRuntime=[ordered]@{executable=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName;version=$PSVersionTable.PSVersion.ToString();edition=$PSVersionTable.PSEdition;processId=$PID}
$seconds=$DurationSeconds
if(-not $PSBoundParameters.ContainsKey('CaptureHardware') -and -not $PSBoundParameters.ContainsKey('CaptureForeground')){$CaptureHardware=$true;$CaptureForeground=$true}
if(-not $CaptureHardware -and -not $CaptureForeground){throw 'Select hardware or foreground sampling.'}
$wanted=@('CPU GT power','CPU package power','CPU cores power','Platform power','GPU power','GPU usage','CPU temperature','GPU temperature','Core clock','Memory clock','Memory usage','RAM usage','Commit charge','CPU clock','Framerate','Framerate 1% Low','Frametime','CPU usage')
if([IO.Path]::GetFileName($StopFile) -cne $StopFile -or [IO.Path]::GetFileName($ReadyFile) -cne $ReadyFile){throw 'Use relative marker leaf names.'}
if(-not $OutputDirectory){$OutputDirectory=(Get-Location).Path}
$stopPath=Join-Path $OutputDirectory $StopFile
if(-not $SelfTest -and [IO.File]::Exists($stopPath)) {
    [ordered]@{samples=0;powerShellRuntime=$powerShellRuntime;stopMarkerObserved=$true;targetProcessId=$TargetPid;targetCreationFileTime=$TargetCreationFileTime.ToString()}|ConvertTo-Json
    return
}
function Write-PowerReady($Sample) {
    $path=Join-Path $OutputDirectory $ReadyFile;$temporary=$path+'.tmp'
    [IO.File]::WriteAllText($temporary,([ordered]@{state='ready';powerShellRuntime=$powerShellRuntime;message=$(if($Sample.error){'Selected sampler started with unavailable data: '+$Sample.error}else{'Selected hardware/foreground sampler started'});partialSource=[bool]$Sample.error;captureHardware=[bool]$CaptureHardware;captureForeground=[bool]$CaptureForeground;captureOperationKeys=([bool]$CaptureForeground -and [bool]$CaptureOperationKeys);targetPid=$TargetPid;qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp()
        sourceStatus=[ordered]@{hardware=[ordered]@{status=$(if(-not $CaptureHardware){'disabled'}elseif($Sample.error){'partial'}else{'ready'});message=[string]$Sample.error};foreground=[ordered]@{status=$(if($CaptureForeground){'ready'}else{'disabled'});message=''}}}|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary,$path)
}

function Read-MahmSample($View,[string[]]$Names){
    $capacity=[uint64]$View.Capacity
    if($capacity -lt 32){throw 'MAHM mapping is shorter than its v2 header.'}
    $signature=$View.ReadUInt32(0);$version=$View.ReadUInt32(4)
    $header=$View.ReadUInt32(8);$count=$View.ReadUInt32(12);$row=$View.ReadUInt32(16)
    $gpuCount=$View.ReadUInt32(24);$gpuRow=$View.ReadUInt32(28)
    # Installed SDK/Include/MAHMSharedMemory.h: five MAX_PATH strings, then
    # float data at 1300; v2 entries include gpu/source identifiers at 1316/1320.
    if($signature -ne 0x4d41484d -or $version -lt 0x20000 -or $version -ge 0x30000 -or
        $header -lt 32 -or $header -gt 1048576 -or $row -lt 1324 -or $row -gt 65536 -or $count -gt 4096){throw 'Invalid/uninitialized MAHM v2 header.'}
    $end=[uint64]$header+[uint64]$count*[uint64]$row
    if($end -gt $capacity -or $gpuCount -gt 256 -or ($gpuCount -gt 0 -and ($gpuRow -eq 0 -or $gpuRow -gt 65536)) -or
        ($end+[uint64]$gpuCount*[uint64]$gpuRow) -gt $capacity){throw 'MAHM entry arrays exceed mapping bounds.'}
    $readings=@();$found=@{}
    for($i=0;$i -lt $count;$i++){
        $offset=[uint64]$header+[uint64]$i*[uint64]$row
        $bytes=New-Object byte[] 260;[void]$View.ReadArray([long]$offset,$bytes,0,260)
        $zero=[Array]::IndexOf($bytes,[byte]0)
        if($zero -lt 0){throw 'MAHM source name is not terminated.'}
        $name=[Text.Encoding]::ASCII.GetString($bytes,0,$zero)
        if($Names -cnotcontains $name){continue}
        $unitBytes=New-Object byte[] 260;[void]$View.ReadArray([long]($offset+260),$unitBytes,0,260)
        $unitZero=[Array]::IndexOf($unitBytes,[byte]0);if($unitZero -lt 0){throw 'MAHM units are not terminated.'}
        $value=$View.ReadSingle([long]($offset+1300))
        $finite=-not [single]::IsNaN($value) -and -not [single]::IsInfinity($value) -and [Math]::Abs([double]$value) -lt 1e30
        $readings += [ordered]@{name=$name;units=[Text.Encoding]::ASCII.GetString($unitBytes,0,$unitZero)
            value=$(if($finite){[double]$value}else{$null});finiteValid=[bool]$finite
            gpuIndex=$View.ReadUInt32([long]($offset+1316));sourceId=$View.ReadUInt32([long]($offset+1320))}
        $found[$name]=$true
    }
    if($View.ReadUInt32(0) -ne $signature -or $View.ReadUInt32(4) -ne $version -or $View.ReadUInt32(8) -ne $header -or
        $View.ReadUInt32(12) -ne $count -or $View.ReadUInt32(16) -ne $row){throw 'MAHM layout changed during this sample.'}
    [ordered]@{version=$version;headerBytes=$header;rowBytes=$row;rows=$count;mappingBytes=$capacity
        pollUnixSeconds=$View.ReadUInt32(20);readings=$readings;missingNames=@($Names|Where-Object {-not $found.ContainsKey($_)})}
}

function Get-PowerOperationTransitions($State,$Sample,[bool]$IdentityMatches,[scriptblock]$ReadKeys,[bool]$Enabled=$true) {
    if(-not $Enabled -or -not $IdentityMatches -or -not $Sample.targetForeground){$State.known=$false;return}
    $observed=& $ReadKeys
    if($null -eq $observed -or $null -eq $observed.Mask -or $observed.Mask -lt 0 -or $observed.Mask -gt 3 -or $observed.QpcTicks -le 0 -or -not $observed.Utc){$State.known=$false;return}
    $State.polls++
    if($State.known) {
        foreach($key in @(@('M',1),@('Esc',2))) {
            if((([int]$observed.Mask -bxor [int]$State.mask) -band $key[1]) -eq 0){continue}
            $State.transitions++
            [ordered]@{key=$key[0];state=$(if([int]$observed.Mask -band $key[1]){'down'}else{'up'});targetProcessId=$Sample.targetProcessId
                qpcTicks=[long]$observed.QpcTicks;qpcFrequency=$Sample.qpcFrequency;utc=[string]$observed.Utc;previousQpcTicks=[long]$State.qpcTicks;previousUtc=[string]$State.utc}
        }
    }
    $State.known=$true;$State.mask=[int]$observed.Mask;$State.qpcTicks=[long]$observed.QpcTicks;$State.utc=[string]$observed.Utc
}
function Get-PowerOperationKeyObservation([bool]$Enabled,$State,[string]$File) {
    [ordered]@{enabled=$Enabled;method='polled_state_change';keys=@('M','Esc');samplePeriodMs=500;samplePeriodIsNominal=$true;targetForegroundOnly=$true;exactEventTiming=$false;shortPressesMayBeMissed=$true
        baselinePolicy='No edge on first sample, focus regain or an unreadable sample; no synthetic release at focus loss or Stop.';samePollKeyOrder='unknown';validPolls=$State.polls;transitions=$State.transitions;dataFile=$File;sampleField='operationKeyTransitions'}
}
# Dot-sourced mock tests expose only pure parsing/state logic, never native input reads.
if($MyInvocation.InvocationName -eq '.'){return}
if(-not $TargetCreationFileTime){throw 'Exact creation FILETIME is required.'}
$targetStartUtc=[DateTime]::FromFileTimeUtc([long]$TargetCreationFileTime).ToString('o')
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RecorderForeground {
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int virtualKey);
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr p,out long c,out long e,out long k,out long u);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr p,uint ms);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr p);
    public static bool SameProcess(int pid,ulong expected) {
        IntPtr p=OpenProcess(0x00101000,false,pid); if(p==IntPtr.Zero)return false;
        try {long c,e,k,u;return GetProcessTimes(p,out c,out e,out k,out u)&&unchecked((ulong)c)==expected&&WaitForSingleObject(p,0)==0x102;}
        finally {CloseHandle(p);}
    }
    public static uint ProcessId() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }
    public sealed class OperationKeyState { public int Mask; public long QpcTicks; public string Utc; }
    public static OperationKeyState OperationKeys(int targetPid) {
        if(ProcessId()!=targetPid)return null;
        int mask=((GetAsyncKeyState(0x4D)&0x8000)!=0?1:0)|((GetAsyncKeyState(0x1B)&0x8000)!=0?2:0);
        long qpc=System.Diagnostics.Stopwatch.GetTimestamp();string utc=DateTime.UtcNow.ToString("o");
        if(ProcessId()!=targetPid)return null;
        return new OperationKeyState {Mask=mask,QpcTicks=qpc,Utc=utc};
    }
}
'@
if($SelfTest){
    if([RecorderForeground]::SameProcess(0,$TargetCreationFileTime)){throw 'Invalid PID passed limited identity check.'}
    if([DateTime]::FromFileTimeUtc([long]$TargetCreationFileTime).ToFileTimeUtc() -ne $TargetCreationFileTime){throw 'FILETIME precision was lost.'}
    [ordered]@{passed=$true;powerShellRuntime=$powerShellRuntime;checks=2;samplingStarted=$false;newEtwSessions=0}|ConvertTo-Json
    return
}
if(-not [RecorderForeground]::SameProcess($TargetPid,$TargetCreationFileTime)){throw 'Target identity is not live.'}
[IO.Directory]::CreateDirectory($OutputDirectory)|Out-Null
$writer=$null;$samples=0;$errors=0;$foregroundSamples=0
$operationState=[ordered]@{known=$false;mask=0;qpcTicks=0L;utc=$null;polls=0;transitions=0}
$qpcFrequency=[Diagnostics.Stopwatch]::Frequency
$startedQpc=[Diagnostics.Stopwatch]::GetTimestamp();$endQpc=$startedQpc;$stopObserved=$false
try{
    $dataFile=$(if($CaptureHardware){'power.jsonl'}else{'foreground.jsonl'})
    $writer=[IO.StreamWriter]::new([IO.FileStream]::new((Join-Path $OutputDirectory $dataFile),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read),[Text.UTF8Encoding]::new($false))
    $watch=[Diagnostics.Stopwatch]::StartNew()
    for($i=0;$i -lt ($seconds*2);$i++){
        $remaining=$i*500-$watch.Elapsed.TotalMilliseconds
        while($remaining -gt 0 -and -not [IO.File]::Exists($stopPath)) {
            Start-Sleep -Milliseconds ([int][Math]::Min(100,$remaining));$remaining=$i*500-$watch.Elapsed.TotalMilliseconds
        }
        if([IO.File]::Exists($stopPath)){$stopObserved=$true;break}
        if($watch.Elapsed.TotalSeconds -ge $seconds){break}
        $endQpc=[Diagnostics.Stopwatch]::GetTimestamp()
        $sample=[ordered]@{index=$i;qpcTicks=$endQpc;qpcMs=($endQpc*1000.0/$qpcFrequency)
            qpcFrequency=$qpcFrequency;utc=[DateTime]::UtcNow.ToString('o');targetProcessId=$TargetPid;targetStartUtc=$targetStartUtc
            error=$null}
        if($CaptureForeground){$foreground=[RecorderForeground]::ProcessId();$sample['foregroundProcessId']=$foreground;$sample['targetForeground']=[bool]($foreground -eq $TargetPid)}
        $map=$null;$view=$null;$targetStillMatches=$true
        try{
            $targetStillMatches=[RecorderForeground]::SameProcess($TargetPid,$TargetCreationFileTime)
            if($CaptureForeground){$sample['operationKeyTransitions']=@(Get-PowerOperationTransitions $operationState $sample $targetStillMatches {[RecorderForeground]::OperationKeys($TargetPid)} ([bool]$CaptureOperationKeys))}
            if(-not $targetStillMatches){throw 'Target exited or exact creation identity changed.'}
            if($CaptureHardware) {
                $sample['readings']=@()
                $map=[IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('MAHMSharedMemory',[IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
                $view=$map.CreateViewAccessor(0,0,[IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
                $values=Read-MahmSample $view $wanted
                foreach($key in $values.Keys){$sample[$key]=$values[$key]}
                if(@($values.readings|Where-Object {$_.finiteValid}).Count -eq 0){throw 'No selected MAHM readings are available.'}
            }
        }catch{$sample.error=$_.Exception.Message;$errors++}
        finally{if($view){$view.Dispose()};if($map){$map.Dispose()}}
        if($CaptureForeground -and $sample.targetForeground){$foregroundSamples++}
        $writer.WriteLine(($sample|ConvertTo-Json -Depth 6 -Compress));$writer.Flush();$samples++
        if($samples -eq 1){Write-PowerReady $sample}
        if(-not $targetStillMatches){break}
    }
}finally{if($writer){$writer.Dispose()}}
[ordered]@{samples=$samples;powerShellRuntime=$powerShellRuntime;errorSamples=$errors;targetForegroundSamples=$foregroundSamples
    captureHardware=[bool]$CaptureHardware;captureForeground=[bool]$CaptureForeground;captureOperationKeys=([bool]$CaptureForeground -and [bool]$CaptureOperationKeys);files=@($dataFile)
    operationKeyObservation=(Get-PowerOperationKeyObservation ([bool]$CaptureForeground -and [bool]$CaptureOperationKeys) $operationState $dataFile)
    secondsLimit=$seconds;samplePeriodMs=500;targetProcessId=$TargetPid;targetStartUtc=$targetStartUtc;targetCreationFileTime=$TargetCreationFileTime.ToString()
    startQpcTicks=$startedQpc;endQpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=$qpcFrequency;stopMarkerObserved=$stopObserved;configurationChanged=$false}|ConvertTo-Json
