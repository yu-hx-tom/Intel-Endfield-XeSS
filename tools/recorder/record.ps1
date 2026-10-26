#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [int]$OwnerPid,
    [uint64]$OwnerCreationFileTime,
    [switch]$Simulate
)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 7){throw 'The recorder requires PowerShell 7 (pwsh.exe); Windows PowerShell fallback is not supported.'}
$powerShellRuntime=[ordered]@{executable=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName;version=$PSVersionTable.PSVersion.ToString();edition=$PSVersionTable.PSEdition;processId=$PID}
if([IO.Path]::GetFileName($powerShellRuntime.executable) -ine 'pwsh.exe'){throw 'Run the recorder with the installed PowerShell 7 pwsh.exe.'}
# Keep recorded UTC text intact on PowerShell releases that auto-convert JSON dates.
$recorderJsonOptions=@{}
if((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')){$recorderJsonOptions.DateKind='String'}
. (Join-Path $PSScriptRoot 'RecorderContext.ps1')
. (Join-Path $PSScriptRoot 'RecorderSummary.ps1')

function Write-RecorderJson([string]$Path,$Value) {
    $temporary=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 32),[Text.UTF8Encoding]::new($false))
        for($attempt=0;;$attempt++) {
            try {
                if([IO.File]::Exists($Path)){[IO.File]::Replace($temporary,$Path,[System.Management.Automation.Language.NullString]::Value)}else{[IO.File]::Move($temporary,$Path)}
                break
            } catch [IO.IOException] {if($attempt -ge 20){throw};Start-Sleep -Milliseconds 10}
        }
    } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
}

function Write-RecorderError([string]$Directory,[string]$Phase,[Management.Automation.ErrorRecord]$Record) {
    $detail=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');phase=$Phase;message=$Record.Exception.Message;exception=$Record.Exception.ToString();fullyQualifiedErrorId=$Record.FullyQualifiedErrorId
        scriptName=$Record.InvocationInfo.ScriptName;scriptLineNumber=$Record.InvocationInfo.ScriptLineNumber;offsetInLine=$Record.InvocationInfo.OffsetInLine;line=$Record.InvocationInfo.Line;positionMessage=$Record.InvocationInfo.PositionMessage;scriptStackTrace=$Record.ScriptStackTrace}
    try {[IO.File]::AppendAllText((Join-Path $Directory 'error-details.jsonl'),(($detail|ConvertTo-Json -Depth 8 -Compress)+"`n"),[Text.UTF8Encoding]::new($false))}catch{}
}

function Initialize-RecorderNative {
    if('RecorderNative' -as [type]){return}
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
public static class RecorderNative {
    public sealed class Identity { public string ImagePath; public ulong Created; }
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr p,uint flags,StringBuilder path,ref uint size);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr p,out long c,out long e,out long k,out long u);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr p,uint milliseconds);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr p);
    public static Identity Read(int pid) {
        IntPtr p=OpenProcess(0x00101000,false,pid);
        if(p==IntPtr.Zero)throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            StringBuilder path=new StringBuilder(32768);uint size=(uint)path.Capacity;long c,e,k,u;
            if(!QueryFullProcessImageName(p,0,path,ref size)||!GetProcessTimes(p,out c,out e,out k,out u))throw new Win32Exception(Marshal.GetLastWin32Error());
            if(WaitForSingleObject(p,0)!=0x102)throw new InvalidOperationException("Process identity is no longer live.");
            return new Identity {ImagePath=path.ToString(),Created=unchecked((ulong)c)};
        } finally {CloseHandle(p);}
    }
    public static bool Same(int pid,ulong expected) {try{return Read(pid).Created==expected;}catch{return false;}}
    public static FileStream OpenRead(string path) {return new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete);}
    public static string ReadAllText(string path) {using(FileStream input=OpenRead(path))using(StreamReader reader=new StreamReader(input)){return reader.ReadToEnd();}}
    public static long CsvRows(string path) {long lines=0;using(FileStream input=OpenRead(path))using(StreamReader reader=new StreamReader(input)){string line;while((line=reader.ReadLine())!=null)if(!String.IsNullOrWhiteSpace(line))++lines;}return Math.Max(0,lines-1);}
    public static string ReadText(string path,int limit,bool tail) {
        using(FileStream input=OpenRead(path)) {
            long start=tail?Math.Max(0,input.Length-limit):0;input.Position=start;
            byte[] bytes=new byte[(int)Math.Min(limit,input.Length-start)];int count=0,n;
            while(count<bytes.Length&&(n=input.Read(bytes,count,bytes.Length-count))>0)count+=n;
            int begin=0,end=count;while(end>0&&bytes[end-1]!=10)--end;
            if(start>0){while(begin<end&&bytes[begin++]!=10){}}
            return Encoding.UTF8.GetString(bytes,begin,Math.Max(0,end-begin));
        }
    }
    internal static string Signature(FileStream input) {
        input.Position=0;byte[] bytes=new byte[(int)Math.Min(65536,input.Length)];int count=input.Read(bytes,0,bytes.Length);
        int line=Array.IndexOf(bytes,(byte)10,0,count);if(line<0)return null;
        using(SHA256 sha=SHA256.Create()){return Convert.ToBase64String(sha.ComputeHash(bytes,0,line+1));}
    }
    public sealed class Copy {public long SourceBytes,Bytes,PartialTailBytes;public bool SourceChanged;}
    public static Copy Freeze(string source,string destination) {
        using(FileStream input=OpenRead(source))using(FileStream output=new FileStream(destination,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.Read)) {
            long length=input.Length,lastLine=0;string signature=Signature(input);input.Position=0;
            byte[] bytes=new byte[65536];long remaining=length;
            while(remaining>0) {
                int n=input.Read(bytes,0,(int)Math.Min(remaining,bytes.Length));if(n==0)break;
                for(int j=n-1;j>=0;--j)if(bytes[j]==10){lastLine=output.Position+j+1;break;}
                output.Write(bytes,0,n);remaining-=n;
            }
            bool changed=remaining!=0||input.Length<length||signature!=Signature(input);
            output.SetLength(lastLine);output.Flush();
            return new Copy {SourceBytes=length,Bytes=lastLine,PartialTailBytes=length-lastLine,SourceChanged=changed};
        }
    }
}
public sealed class RecorderLineStream : IDisposable {
    public sealed class Update {
        public long SourceBytes,SourceOffset,OutputOffset,CopiedBytes,ReadBytes,BacklogBytes;
        public int Generation;public bool Rotated;public string RotationReason,LatestStatus;
        public string[] StatusLines;
    }
    readonly FileStream output;long offset;string signature;int generation;
    public RecorderLineStream(string destination) {output=new FileStream(destination,FileMode.CreateNew,FileAccess.Write,FileShare.Read);}
    public Update Poll(string source) {return Poll(source,true);}
    public Update Poll(string source,bool measurementStatus) {return Poll(source,measurementStatus,false);}
    public Update Poll(string source,bool measurementStatus,bool statusOnly) {
        using(FileStream input=RecorderNative.OpenRead(source)) {
            long length=input.Length;string first=RecorderNative.Signature(input);
            Update result=new Update {SourceBytes=length,OutputOffset=output.Position};
            if(length<offset||(signature!=null&&first!=null&&signature!=first)) {
                result.Rotated=true;result.RotationReason=length<offset?"size_reset":"changed_first_complete_line";offset=0;++generation;
            }
            signature=first;input.Position=offset;
            byte[] bytes=new byte[(int)Math.Min(1024*1024,Math.Max(0,length-offset))];int count=0,n;
            while(count<bytes.Length&&(n=input.Read(bytes,count,bytes.Length-count))>0)count+=n;
            if(input.Length<length||first!=RecorderNative.Signature(input))throw new IOException("Source rotated during this poll; next poll retries from the detected generation.");
            int keep=count;while(keep>0&&bytes[keep-1]!=10)--keep;
            if(keep==0&&count==1024*1024)throw new IOException("A measurement line exceeds the 1 MiB streaming read bound; the final snapshot retains available context.");
            result.SourceOffset=offset;result.Generation=generation;
            if(keep>0) {
                if(!statusOnly){output.Write(bytes,0,keep);output.Flush();result.CopiedBytes=keep;}offset+=keep;result.ReadBytes=keep;
                if(measurementStatus){string text=Encoding.UTF8.GetString(bytes,0,keep);int match=text.LastIndexOf("\"event\":\"measurement_status\"",StringComparison.Ordinal);
                    if(match>=0){int begin=text.LastIndexOf('\n',match),end=text.IndexOf('\n',match);if(end>=0)result.LatestStatus=text.Substring(begin+1,end-begin-1);}
                    if(statusOnly){var lines=new System.Collections.Generic.List<string>();foreach(string line in text.Split('\n'))if(line.IndexOf("\"event\":\"measurement_status\"",StringComparison.Ordinal)>=0)lines.Add(line);result.StatusLines=lines.ToArray();}}
            }
            result.BacklogBytes=length-offset;return result;
        }
    }
    public int AppendStatus(string line){byte[] bytes=Encoding.UTF8.GetBytes(line+"\n");output.Write(bytes,0,bytes.Length);output.Flush();return bytes.Length;}
    public void Dispose(){output.Dispose();}
}
'@
}

function Get-RecorderInventorySummary($Data) {
    if($Data.readOnly -ne $true -or $Data.sessionsQuery.status -ne 0 -or $Data.sessionsQuery.reportedCount -gt $Data.sessionsQuery.capacity){throw 'Read-only ETW inventory is incomplete.'}
    $kernel=@($Data.providers|Where-Object {$_.name -ceq 'Microsoft-Windows-Kernel-Process'})
    if($kernel.Count -ne 1 -or $kernel[0].status -ne 0){throw 'Read-only Kernel-Process provider query failed.'}
    $loggers=@($kernel[0].instances|ForEach-Object {$_.enables}|Where-Object {$_.enabled -eq 1}|ForEach-Object {[int]$_.loggerId}|Sort-Object -Unique)
    [ordered]@{kernelProcessEnabledLoggerIds=$loggers;kernelProcessHeadroomBelowEight=($loggers.Count -lt 8);readOnly=$true;existingSessionsTouched=$false}
}

function Set-RecorderPresentMonFallback([string]$Reason) {
    $script:status['presentMonSource']=$(if($script:selectedCollectors.pmdp){'existing_pmdp'}else{'unavailable'})
    $script:status['independentPresentMonRaw']=$false
    $script:status['presentMonFallbackReason']=$Reason
    Set-RecorderSource 'presentmonRaw' 'unavailable' $Reason
    $script:warnings.Add($(if($script:selectedCollectors.pmdp){'复用已有PresentMon，缺少独立raw。现有PMDP是否有帧数据以保存结果为准。原因：'}else{'Independent PresentMon raw unavailable; no unselected fallback was enabled. '})+$Reason)
}

function Resolve-RecorderCollectors($Request) {
    $defaults=[ordered]@{presentmonRaw=$true;pmdp=$true;rtss=$true;hardware=$true;foreground=$true;system=$false;threads=$false;measurements=$true;logs=$true}
    $property=@($Request.PSObject.Properties|Where-Object {$_.Name -ieq 'collectors'})
    if($property.Count -eq 0){return $defaults}
    if($property.Count -ne 1 -or $property[0].Name -cne 'collectors'){throw 'request.json collectors must use the exact property name.'}
    $value=$property[0].Value
    if($null -eq $value -or $value -isnot [pscustomobject]){throw 'request.json collectors must be an object of JSON booleans.'}
    $selected=[ordered]@{};foreach($key in $defaults.Keys){$selected[$key]=$false}
    foreach($entry in $value.PSObject.Properties) {
        if($defaults.Keys -cnotcontains $entry.Name){throw ('Unknown collector: '+$entry.Name)}
        if($entry.Value -isnot [bool]){throw ('Collector '+$entry.Name+' must be a JSON boolean.')}
        $selected[$entry.Name]=$entry.Value
    }
    if(-not ($selected.Values -contains $true)){throw 'Select at least one collector.'}
    $selected
}

function Set-RecorderSource([string]$Key,[string]$State,[string]$Message,$Coverage=$null) {
    if(-not $script:sourceStatus.Contains($Key)){return}
    $entry=$script:sourceStatus[$Key];$entry.status=$State;$entry.message=$Message
    if($null -ne $Coverage){$entry.coverage=$Coverage}
}

function Get-RecorderReadySourceState($ReadyData,[string]$Key) {
    if($ReadyData.state -notin @('ready','partial')){return 'unavailable'}
    $detail=$ReadyData.sourceStatus.$Key
    if($detail.status -eq 'partial' -or (-not $detail.status -and ($ReadyData.state -eq 'partial' -or $ReadyData.partialSource))){return 'partial'}
    'collecting'
}

function Get-RecorderHelperPlan($Selected,[int]$TargetPid,[uint64]$Creation,[string]$Directory,[string]$Root,[string]$Shell,[bool]$CaptureOperationKeys=$false) {
    $common=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File')
    $capture=@('-TargetPid',$TargetPid.ToString(),'-TargetCreationFileTime',$Creation.ToString(),'-DurationSeconds','86400','-OutputDirectory',$Directory,'-StopFile','stop.request')
    if($Selected.hardware -or $Selected.foreground) {
        $flags=@();$keys=@();if($Selected.hardware){$flags+='-CaptureHardware';$keys+='hardware'};if($Selected.foreground){$flags+='-CaptureForeground';$keys+='foreground'}
        $flags+='-CaptureOperationKeys:'+$(if($Selected.foreground -and $CaptureOperationKeys){'$true'}else{'$false'})
        [pscustomobject]@{kind='power';keys=$keys;exe=$Shell;arguments=($common+@((Join-Path $Root 'capture-power.ps1'))+$capture+@('-ReadyFile','power.ready.json')+$flags);stdout='power-run.json';stderr='power.stderr.txt';source=(Join-Path $Root 'capture-power.ps1')}
    }
    if($Selected.rtss){[pscustomobject]@{kind='rtss';keys=@('rtss');exe=(Join-Path $Root 'rtss-poll.exe');arguments=@($TargetPid.ToString(),'86400','rtss-present-intervals.csv','--stop-file','stop.request','--ready-file','rtss.ready.json');stdout='rtss-run.json';stderr='rtss.stderr.txt';source=(Join-Path $Root 'rtss-poll.exe')}}
    if($Selected.pmdp){[pscustomobject]@{kind='pmdp';keys=@('pmdp');exe=(Join-Path $Root 'pmdp-reader.exe');arguments=@($TargetPid.ToString(),'86400','--new-only','--stop-file','stop.request','--ready-file','pmdp.ready.json');stdout='frames.csv';stderr='pmdp-run.json';source=(Join-Path $Root 'pmdp-reader.exe')}}
    if($Selected.system -or $Selected.threads) {
        $flags=@();$keys=@();if($Selected.system){$flags+='-CaptureSystem';$keys+='system'};if($Selected.threads){$flags+='-CaptureThreads';$keys+='threads'}
        [pscustomobject]@{kind='system';keys=$keys;exe=$Shell;arguments=($common+@((Join-Path $Root 'capture-system.ps1'))+$capture+@('-ReadyFile','system.ready.json')+$flags);stdout='system-run.json';stderr='system.stderr.txt';source=(Join-Path $Root 'capture-system.ps1')}
    }
}

function Start-RecorderPlannedHelper($Plan,[string]$Directory) {
    if(-not [IO.File]::Exists($Plan.source)){throw ('Required recorder component is missing: '+$Plan.source)}
    $child=Start-RecorderOwned $Plan.kind $Plan.exe $Plan.arguments $Directory $Plan.stdout $Plan.stderr
    $child|Add-Member -NotePropertyName keys -NotePropertyValue @($Plan.keys)
    $child
}

function Get-RecorderSnapshotPlan($Selected,[string]$MeasurementMode='full') {
    if($Selected.measurements -and $MeasurementMode -ne 'summary'){foreach($name in 'measurements','measurements-summary'){foreach($index in 0,1){[pscustomobject]@{key='measurements';source=('observer.jsonl.'+$name+'-'+$index+'.jsonl');file=($name+'-'+$index+'.snapshot.jsonl');tail=$false}}}}
    if($Selected.logs){
        [pscustomobject]@{key='logs';source='observer.jsonl';file='observer-snapshot.jsonl';tail=$true}
        [pscustomobject]@{key='logs';source='efu-runtime.jsonl';file='efu-runtime.jsonl';tail=$true}
        foreach($index in 0,1){[pscustomobject]@{key='logs';source=('observer.jsonl.sr-observation-'+$index+'.jsonl');file=('sr-observation-'+$index+'.snapshot.jsonl');tail=$false}}
    }
}

function Start-RecorderContextStreams($Selected,[string]$SourceDirectory,[string]$Directory) {
    if(-not $Selected.measurements -and -not $Selected.logs){return}
    if($Selected.measurements){$script:streamEvents=[IO.StreamWriter]::new((Join-Path $Directory 'measurement-stream-events.jsonl'),$false,[Text.UTF8Encoding]::new($false))}
    if($Selected.logs){$script:srStreamEvents=[IO.StreamWriter]::new((Join-Path $Directory 'sr-observation-stream-events.jsonl'),$false,[Text.UTF8Encoding]::new($false))}
    $mode=$(if($script:recordingContext){$script:recordingContext.measurementMode}else{'full'})
    $plans=@(Get-RecorderSnapshotPlan $Selected|Where-Object {-not $_.tail})
    foreach($plan in $plans) {
        $source=Join-Path $SourceDirectory $plan.source;$file=$plan.file.Replace('.snapshot.jsonl','.stream.jsonl')
        $compact=$mode -eq 'summary' -and $plan.source -like 'observer.jsonl.measurements-?.jsonl'
        if($compact){$file=$file.Replace('measurements-','measurement-status-').Replace('.stream.jsonl','.compact.jsonl')}
        $script:streams+=[pscustomobject]@{key=$plan.key;source=$source;file=$file;statusOnly=$compact;reader=[RecorderLineStream]::new((Join-Path $Directory $file));bytes=[long]0;backlogBytes=[long]0;rotations=0;lastError=$null}
        if($plan.key -eq 'measurements' -and [IO.File]::Exists($source)){foreach($line in ([RecorderNative]::ReadText($source,2097152,$true) -split "`n")){if($line.Contains('"event":"measurement_status"')){Set-RecorderMode $line}}}
    }
    Update-RecorderStreams
}

function Save-RecorderContextSnapshots($Selected,[string]$SourceDirectory,[string]$Directory) {
    $snapshots=@();$notes=[Collections.Generic.List[string]]::new()
    $mode=$(if($script:recordingContext){$script:recordingContext.measurementMode}else{'full'})
    foreach($plan in @(Get-RecorderSnapshotPlan $Selected $mode)) {
        $source=Join-Path $SourceDirectory $plan.source;$destination=Join-Path $Directory $plan.file
        if([IO.File]::Exists($source)) {
            try {
                if($plan.tail){$text=[RecorderNative]::ReadText($source,2097152,$true);[IO.File]::WriteAllText($destination,$text,[Text.UTF8Encoding]::new($false));$copy=[pscustomobject]@{SourceBytes=([IO.FileInfo]$source).Length;Bytes=([IO.FileInfo]$destination).Length;PartialTailBytes=$null;SourceChanged=$false}}
                else {$copy=[RecorderNative]::Freeze($source,$destination)}
                $snapshots+=[ordered]@{key=$plan.key;source=$source;file=$plan.file;state=$(if($copy.SourceChanged){'changed_during_snapshot'}else{'copied'});tailOnly=$plan.tail;tailByteLimit=$(if($plan.tail){2097152}else{$null});sourceBytes=$copy.SourceBytes;bytes=$copy.Bytes;partialTailBytesOmitted=$copy.PartialTailBytes}
                if($copy.SourceChanged){$notes.Add($plan.source+' changed during final snapshot; retained data may be partial.')}
            }catch{$snapshots+=[ordered]@{key=$plan.key;source=$source;state='copy_failed';error=$_.Exception.Message};$notes.Add($plan.source+': '+$_.Exception.Message);Write-RecorderError $Directory 'context_snapshot' $_}
        }else{$snapshots+=[ordered]@{key=$plan.key;source=$source;state='missing'};$notes.Add($plan.source+' was absent at Stop.')}
    }
    try {[IO.File]::Copy((Join-Path $SourceDirectory 'launch.json'),(Join-Path $Directory 'launch.json'),$false)}catch{$notes.Add('launch.json snapshot: '+$_.Exception.Message)}
    [ordered]@{snapshots=$snapshots;warnings=@($notes)}
}

function Get-RecorderContextSourceSummary([string]$Key,$Snapshots,$Streams) {
    $available=@($Snapshots|Where-Object {$_.key -eq $Key -and $_.bytes -gt 0})
    $selectedStreams=@($Streams|Where-Object {$_.key -eq $Key -or (-not $_.key -and $Key -eq 'measurements')})
    $streamBytes=[long]0;$bytes=[long]0;$rotations=[long]0;$remaining=[long]0
    # Sum dictionary values directly and keep large recording byte totals as Int64.
    foreach($entry in $available){$bytes+=[long]$entry.bytes}
    foreach($entry in $selectedStreams){$streamBytes+=[long]$entry.copiedBytes;$rotations+=[long]$entry.detectedRotations;$remaining+=[long]$entry.remainingBytesAtLastPoll}
    $bytes+=$streamBytes
    $files=@($available|ForEach-Object {$_.file})+@($selectedStreams|ForEach-Object {$_.file})
    $streamPartial=@($selectedStreams|Where-Object {$_.lastError -or $_.remainingBytesAtLastPoll -gt 0 -or $_.detectedRotations -gt 0}).Count -gt 0
    $coverage=[ordered]@{bytes=$bytes;streamBytes=$streamBytes;availableSnapshots=$available.Count;detectedRotations=$rotations;remainingStreamBytes=$remaining}
    if($selectedStreams.Count){$coverage['lossNote']='Only complete lines observed at polls are retained; unread tails and rotations between polls may be missing.'}
    if($Key -eq 'measurements'){$coverage['instrumentationNotEnabledByRecorder']=$true;$coverage['gpuCpuTimingAvailability']='unknown; inspect existing latestMeasurementStatus, recorder never enables instrumentation'}
    [ordered]@{files=$files;state=$(if($bytes -eq 0){'unavailable'}elseif($streamPartial -or @($Snapshots|Where-Object {$_.key -eq $Key -and $_.state -ne 'copied'}).Count -gt 0){'partial'}else{'collected'})
        message=$(if($Key -eq 'measurements'){'Existing detail and 60-second summary fields are saved separately; GPU timestamps and CPU timing instrumentation were not enabled by the recorder.'}else{'Observer and EFU tails are bounded to 2 MiB each; SR observation files are streamed and their retained contents are saved at Stop.'});coverage=$coverage}
}

function ConvertTo-RecorderArgument([string]$Value) {
    '"'+[regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'
}

function Start-RecorderOwned([string]$Kind,[string]$Exe,[string[]]$Arguments,[string]$Directory,[string]$Stdout,[string]$Stderr) {
    $si=[Diagnostics.ProcessStartInfo]::new();$si.FileName=$Exe;$si.WorkingDirectory=$Directory
    $si.Arguments=(@($Arguments|ForEach-Object {ConvertTo-RecorderArgument $_}) -join ' ')
    $si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
    if([IO.Path]::GetFileName($Exe) -ieq 'pwsh.exe'){$si.EnvironmentVariables.Remove('PSModulePath')}
    $p=[Diagnostics.Process]::new();$p.StartInfo=$si;$out=$null;$err=$null
    try {
        $out=[IO.FileStream]::new((Join-Path $Directory $Stdout),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $err=[IO.FileStream]::new((Join-Path $Directory $Stderr),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        if(-not $p.Start()){throw ('Could not start '+$Kind)}
        [pscustomobject]@{kind=$Kind;process=$p;out=$out;err=$err;outTask=$p.StandardOutput.BaseStream.CopyToAsync($out);errTask=$p.StandardError.BaseStream.CopyToAsync($err)
            startQpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();executable=$Exe;arguments=$si.Arguments;forced=$false;result=$null}
    } catch {if($out){$out.Dispose()};if($err){$err.Dispose()};$p.Dispose();throw}
}

function Complete-RecorderOwned($Child,[int]$WaitMilliseconds=5000) {
    if($null -ne $Child.result){return $Child.result}
    if(-not $Child.process.WaitForExit($WaitMilliseconds)){$Child.forced=$true;$Child.process.Kill();$Child.process.WaitForExit()}
    [void]$Child.outTask.GetAwaiter().GetResult();[void]$Child.errTask.GetAwaiter().GetResult();$Child.out.Dispose();$Child.err.Dispose()
    $Child.result=[ordered]@{kind=$Child.kind;pid=$Child.process.Id;executable=$Child.executable;startQpcTicks=$Child.startQpcTicks;exitCode=$Child.process.ExitCode;forcedTermination=$Child.forced;arguments=$Child.arguments}
    $Child.process.Dispose();$Child.result
}

function Test-RecorderOwnSessionName([string]$Name,[string]$Expected) {
    $Name -cmatch '^EndfieldRecorder-[0-9a-f]{32}$' -and $Name -ceq $Expected
}

function Get-RecorderEtwSession([string]$Name) {
    $si=[Diagnostics.ProcessStartInfo]::new();$si.FileName=(Join-Path $env:SystemRoot 'System32/logman.exe');$si.Arguments='query '+(ConvertTo-RecorderArgument $Name)+' -ets'
    $si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
    $p=[Diagnostics.Process]::new();$p.StartInfo=$si
    try {
        if(-not $p.Start()){throw 'Could not query the recording ETW session.'}
        $stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
        if(-not $p.WaitForExit(5000)){$p.Kill();$p.WaitForExit();throw 'Recording ETW session query timed out.'}
        [pscustomobject]@{exists=($p.ExitCode -eq 0);exitCode=$p.ExitCode;text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()}
    } finally {$p.Dispose()}
}

function Stop-RecorderPresentMon([string]$Exe,[string]$Name,[string]$Expected,[string]$Directory) {
    if(-not (Test-RecorderOwnSessionName $Name $Expected)){throw 'Refused ETW cleanup outside this invocation GUID.'}
    $before=Get-RecorderEtwSession $Name
    $result=[ordered]@{sessionName=$Name;onlyOwnGuid=$true;sessionExisted=$before.exists;stopRequested=$false;stopExitCode=$null;forcedTermination=$false;sessionReleased=$false}
    [IO.File]::WriteAllText((Join-Path $Directory 'presentmon-session-before-stop.txt'),$before.text)
    if($before.exists) {
        $result.stopRequested=$true
        $stop=Start-RecorderOwned 'presentmon-session-stop' $Exe @('--session_name',$Name,'--terminate_existing_session') $Directory 'presentmon-stop.stdout.txt' 'presentmon-stop.stderr.txt'
        $stopped=Complete-RecorderOwned $stop 5000;$result.stopExitCode=$stopped.exitCode;$result.forcedTermination=$stopped.forcedTermination
    }
    $after=Get-RecorderEtwSession $Name;$result.sessionReleased=-not $after.exists
    [IO.File]::WriteAllText((Join-Path $Directory 'presentmon-session-after-stop.txt'),$after.text)
    $result
}

function Find-RecorderSession([string]$Root,[int]$TargetPid,$Identity) {
    $matchedSessions=@();$candidates=@();$created=[DateTime]::FromFileTimeUtc([long]$Identity.Created)
    if(Test-Path -LiteralPath $Root -PathType Container) {
        foreach($directory in @(Get-ChildItem -LiteralPath $Root -Directory)) {
            if($directory.Name -cnotmatch '^[0-9a-f]{32}$'){continue}
            $launchPath=Join-Path $directory.FullName 'launch.json';$observer=Join-Path $directory.FullName 'observer.jsonl'
            if(-not [IO.File]::Exists($launchPath) -or -not [IO.File]::Exists($observer)){continue}
            try {
                $launch=[IO.File]::ReadAllText($launchPath)|ConvertFrom-Json @recorderJsonOptions
                $difference=($created-[DateTimeOffset]::Parse($launch.startedAt).UtcDateTime).TotalSeconds
                if($difference -lt -5 -or $difference -gt 120){continue}
                $pathMatches=$true
                if($launch.exactGamePath){$pathMatches=[IO.Path]::GetFullPath($launch.exactGamePath).Equals($Identity.ImagePath,[StringComparison]::OrdinalIgnoreCase)}
                elseif($launch.gameDirectory){$pathMatches=[IO.Path]::GetFullPath((Join-Path $launch.gameDirectory 'Endfield.exe')).Equals($Identity.ImagePath,[StringComparison]::OrdinalIgnoreCase)}
                $pidMatches=$false
                foreach($line in ([RecorderNative]::ReadText($observer,262144,$false) -split "`n")) {
                    if(-not $line.Trim()){continue}
                    try{$row=$line|ConvertFrom-Json @recorderJsonOptions;if($row.pid -eq $TargetPid){$pidMatches=$true;break}}catch{}
                }
                $candidate=[ordered]@{sessionId=$directory.Name;directory=$directory.FullName;launchToGameSeconds=$difference;observerPidMatches=$pidMatches;launchPathMatches=$pathMatches}
                $candidates+=$candidate
                if($pidMatches -and $pathMatches){$matchedSessions+=$candidate}
            } catch {$candidates+=[ordered]@{sessionId=$directory.Name;readError=$_.Exception.Message}}
        }
    }
    if($matchedSessions.Count -eq 1){$state='matched';$match=$matchedSessions[0]}elseif($matchedSessions.Count -gt 1){$state='ambiguous';$match=$null}else{$state='absent';$match=$null}
    [ordered]@{state=$state;match=$match;candidates=$candidates;requiredLaunchToGameSeconds=@(-5,120);requiresObserverPidMatch=$true;selectionUsesDirectoryRecency=$false}
}

function Get-RecorderStop([string]$Path,[int]$GuiPid,[uint64]$GuiCreation,[int]$GameProcessId,[uint64]$GameCreation,[long]$SafetyQpc) {
    if([IO.File]::Exists($Path)) {
        $observed=[Diagnostics.Stopwatch]::GetTimestamp()
        try {
            $data=[IO.File]::ReadAllText($Path)|ConvertFrom-Json @recorderJsonOptions
            if([long]$data.qpcFrequency -ne [Diagnostics.Stopwatch]::Frequency -or [long]$data.qpcTicks -le 0 -or [long]$data.qpcTicks -gt $observed+[Diagnostics.Stopwatch]::Frequency){throw 'Stop QPC is invalid.'}
            [void][DateTimeOffset]::Parse($data.utc)
            return [ordered]@{reason=[string]$data.reason;utc=[string]$data.utc;qpcTicks=[long]$data.qpcTicks;qpcFrequency=[long]$data.qpcFrequency;observedQpcTicks=$observed;source='stop.request';validRequest=$true}
        } catch {return [ordered]@{reason='stop_requested';utc=[DateTime]::UtcNow.ToString('o');qpcTicks=$observed;qpcFrequency=[Diagnostics.Stopwatch]::Frequency;observedQpcTicks=$observed;source='stop.request';validRequest=$false;requestError=$_.Exception.Message}}
    }
    $reason=$null
    if(-not [RecorderNative]::Same($GuiPid,$GuiCreation)){$reason='recorder_closed'}
    elseif($GameProcessId -gt 0 -and -not [RecorderNative]::Same($GameProcessId,$GameCreation)){$reason='game_exited'}
    elseif($SafetyQpc -gt 0 -and [Diagnostics.Stopwatch]::GetTimestamp() -ge $SafetyQpc){$reason='24_hour_safety_limit'}
    if($reason) {
        $data=[ordered]@{reason=$reason;utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency}
        Write-RecorderJson $Path $data
        $data['observedQpcTicks']=$data.qpcTicks;$data['source']='automatic';$data['validRequest']=$true
        return $data
    }
    $null
}

function Set-RecorderMode([string]$Json) {
    if(-not $Json){return}
    try {
        $row=$Json|ConvertFrom-Json @recorderJsonOptions
        if($row.event -cne 'measurement_status' -or $row.pid -ne $script:gamePid){return}
        if($null -ne $script:latestMeasurement -and [double]$row.qpc_ns -le [double]$script:latestMeasurement.qpc_ns){return}
        $script:latestMeasurement=$row
        $script:status.mode='Observer: epoch '+$row.control_epoch+', SDK '+$row.sdk_frames+' frames, inflight '+$row.inflight_limit+', async '+$row.async+', shared copy '+$row.shared_copy_path
        Write-RecorderJson (Join-Path $script:OutputDirectory 'measurement-status-latest.json') $row
    } catch {$script:warnings.Add('A measurement_status line could not be parsed: '+$_.Exception.Message)}
}

function Update-RecorderStreams([switch]$Drain) {
    foreach($entry in $script:streams) {
        $events=$(if($entry.key -eq 'logs'){$script:srStreamEvents}else{$script:streamEvents})
        $limit=$(if($Drain){64}else{1})
        for($attempt=0;$attempt -lt $limit;$attempt++) {
            try {
                $update=$entry.reader.Poll($entry.source,($entry.key -ne 'logs'),[bool]$entry.statusOnly)
                if($entry.statusOnly){foreach($line in $update.StatusLines){$compact=ConvertTo-RecorderCompactStatus $line;if($compact){$update.CopiedBytes+=$entry.reader.AppendStatus($compact)}}}
                if($entry.lastError){$events.WriteLine(([ordered]@{utc=[DateTime]::UtcNow.ToString('o');key=$entry.key;source=$entry.source;state='available_again'}|ConvertTo-Json -Compress));$entry.lastError=$null}
                if($update.CopiedBytes -gt 0 -or $update.Rotated) {
                    $event=[ordered]@{qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();key=$entry.key;source=$entry.source;file=$entry.file;generation=$update.Generation;sourceOffset=$update.SourceOffset;outputOffset=$update.OutputOffset;copiedBytes=$update.CopiedBytes;sourceBytes=$update.SourceBytes;backlogBytes=$update.BacklogBytes;rotation=$update.Rotated;rotationReason=$update.RotationReason}
                    $events.WriteLine(($event|ConvertTo-Json -Compress));$events.Flush()
                    if($update.Rotated){$entry.rotations++;$script:warnings.Add('Observer stream '+$entry.file+' rotated; an unread tail or rotations between polls may be missing.')}
                }
                $entry.bytes+=$update.CopiedBytes;$entry.backlogBytes=$update.BacklogBytes;if($entry.key -ne 'logs'){Set-RecorderMode $update.LatestStatus}
                if($update.ReadBytes -eq 0){break}
            } catch {
                $message=$_.Exception.Message
                if($message -cne $entry.lastError){$entry.lastError=$message;$events.WriteLine(([ordered]@{qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();key=$entry.key;source=$entry.source;state='unavailable';error=$message}|ConvertTo-Json -Compress));$events.Flush()}
                break
            }
        }
    }
}

function Save-RecorderStatus([string]$State,[string]$Message) {
    if($State -eq 'recording' -and -not $Simulate -and $script:warnings.Count -gt 0){$Message='Recording. Click Stop when finished. Some sources have partial coverage.'}
    if($script:selectedCollectors.presentmonRaw -and $script:status.presentMonSource -eq 'existing_pmdp'){$Message+=' 复用已有PresentMon，缺少独立raw。'}
    $script:status.state=$State;$script:status.message=$Message;$script:status.updatedUtc=[DateTime]::UtcNow.ToString('o')
    $script:status['warningCount']=@($script:warnings|Select-Object -Unique).Count
    if($script:status.startQpcTicks){$end=$(if($script:stop){[long]$script:stop.qpcTicks}else{[Diagnostics.Stopwatch]::GetTimestamp()});$script:status.durationSeconds=[Math]::Max(0,($end-[long]$script:status.startQpcTicks)/[double][Diagnostics.Stopwatch]::Frequency)}
    Write-RecorderJson (Join-Path $script:OutputDirectory 'status.json') $script:status
}

function New-RecorderZip([string]$Directory,[string]$Path) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $file=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    $archive=$null
    try {
        $archive=[IO.Compression.ZipArchive]::new($file,[IO.Compression.ZipArchiveMode]::Create,$true)
        foreach($item in @(Get-ChildItem -LiteralPath $Directory -File -Recurse|Where-Object {Test-RecorderArtifact $_.Name})) {
            $relative=$item.FullName.Substring($Directory.Length+1).Replace('\','/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$item.FullName,$relative,[IO.Compression.CompressionLevel]::Fastest)|Out-Null
        }
    } finally {if($archive){$archive.Dispose()};$file.Dispose()}
}

function Test-RecorderArtifact([string]$Name) {
    $Name -ne 'status.json' -and $Name -notlike '*.tmp' -and $Name -notlike 'worker.*' -and $Name -notlike 'backend.*' -and $Name -notlike 'recorder-worker.*'
}

function Get-RecorderSha256([string]$Path) {
    $inputFile=[IO.File]::OpenRead($Path);$hash=[Security.Cryptography.SHA256]::Create()
    try {([BitConverter]::ToString($hash.ComputeHash($inputFile))).Replace('-','').ToLowerInvariant()}
    finally {$hash.Dispose();$inputFile.Dispose()}
}

# Dot sourcing exposes the same pure checks and owned-process helpers to the CPU tests.
if($MyInvocation.InvocationName -eq '.'){return}
if(-not $OutputDirectory){throw 'OutputDirectory is required.'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
if(-not [IO.Directory]::Exists($OutputDirectory) -or -not [IO.File]::Exists((Join-Path $OutputDirectory 'request.json'))){throw 'Use the new recording folder containing request.json created by the recorder.'}
if([IO.File]::Exists((Join-Path $OutputDirectory 'status.json'))){throw 'This folder already has recording status; existing evidence was left alone.'}
$script:status=[ordered]@{state='preparing';message='Preparing collectors';outputDirectory=$OutputDirectory;startedUtc=$null;startQpcTicks=$null;qpcFrequency=[Diagnostics.Stopwatch]::Frequency;durationSeconds=0;gamePid=$null;sessionId=$(if($Simulate){'simulated-'+[guid]::NewGuid().ToString('N')}else{$null});mode=$(if($Simulate){'TEST ONLY: simulated recording'}else{'Preparing'});zipPath=$null}
$script:warnings=[Collections.Generic.List[string]]::new();$script:streams=@();$script:streamEvents=$null;$script:srStreamEvents=$null;$script:latestMeasurement=$null
$script:recordingContext=$null;$powerStateWriter=$null;$powerStateSamples=0;$markerClose=$null;$configurationStarted=$false
$script:selectedCollectors=[ordered]@{};$script:sourceStatus=[ordered]@{}
$script:status['selectedCollectors']=$script:selectedCollectors;$script:status['sourceStatus']=$script:sourceStatus
$script:status['powerShellRuntime']=$powerShellRuntime
$children=@();$collectorResults=@();$helperPlan=@();$script:gamePid=0;$gameCreation=[uint64]0;$gameIdentity=$null;$session=$null;$request=$null;$script:stop=$null
$failure=$null;$pmStarted=$false;$pmSession=$null;$pmStop=$null;$pm=$null;$inventory=$null;$inventorySummary=$null;$recordingStarted=$false;$synthetic=$null;$snapshots=@();$safety=0
$script:status['presentMonSource']='disabled';$script:status['independentPresentMonRaw']=$false;$script:status['presentMonFallbackReason']=$null
$pmExe='D:/Programfiles/RivaTuner Statistics Server/Plugins/Client/PresentMonDataProvider/PresentMon-2.3.1-x64.exe'
$stopPath=Join-Path $OutputDirectory 'stop.request'
Save-RecorderStatus 'preparing' $(if($Simulate){'Preparing TEST ONLY recording'}else{'Preparing collectors'})
try {
    Initialize-RecorderNative
    $workerIdentity=[RecorderNative]::Read($PID)
    $script:status['workerPid']=$PID;$script:status['workerCreationFileTime']=$workerIdentity.Created.ToString();$script:status['backendPid']=$PID;$script:status['backendCreationFileTime']=$workerIdentity.Created.ToString();$script:status['gameCreationFileTime']=$null
    if($OwnerPid -le 0 -or -not $OwnerCreationFileTime -or -not [RecorderNative]::Same($OwnerPid,$OwnerCreationFileTime)){throw 'Recorder owner PID/creation identity is not live. No collectors were started.'}
    $request=[IO.File]::ReadAllText((Join-Path $OutputDirectory 'request.json'))|ConvertFrom-Json @recorderJsonOptions
    $script:recordingContext=Resolve-RecorderRequestContext $request
    $script:status['recordingContext']=$script:recordingContext
    $script:selectedCollectors=Resolve-RecorderCollectors $request
    foreach($key in $script:selectedCollectors.Keys){$script:sourceStatus[$key]=[ordered]@{selected=$script:selectedCollectors[$key];status=$(if($script:selectedCollectors[$key]){'pending'}else{'disabled'});message=$(if($script:selectedCollectors[$key]){'Not started'}else{'Not selected'});files=@();coverage=$null}}
    $script:status.selectedCollectors=$script:selectedCollectors
    $script:status.presentMonSource=$(if($script:selectedCollectors.presentmonRaw){'pending'}elseif($script:selectedCollectors.pmdp){'existing_pmdp'}else{'disabled'})
    if(-not $request.utc -and $request.startedUtc){$request|Add-Member -NotePropertyName utc -NotePropertyValue $request.startedUtc}
    if(-not $request.qpcTicks -and $request.startQpcTicks){$request|Add-Member -NotePropertyName qpcTicks -NotePropertyValue $request.startQpcTicks}
    if([long]$request.qpcFrequency -ne [Diagnostics.Stopwatch]::Frequency -or [long]$request.qpcTicks -le 0){throw 'request.json has an invalid QPC clock.'}
    [void][DateTimeOffset]::Parse($request.utc)
    Save-RecorderStatus 'preparing' 'Preparing selected collectors'
    $script:stop=Get-RecorderStop $stopPath $OwnerPid $OwnerCreationFileTime 0 0 0
    if(-not $script:stop -and -not $Simulate) {
        $games=@(Get-Process -Name Endfield -ErrorAction SilentlyContinue)
        if($games.Count -ne 1){throw ('Start the game yourself, then click Start. Expected one live Endfield.exe; found '+$games.Count+'.')}
        $script:gamePid=$games[0].Id;$gameIdentity=[RecorderNative]::Read($script:gamePid);$gameCreation=$gameIdentity.Created
        if(-not [IO.Path]::GetFileName($gameIdentity.ImagePath).Equals('Endfield.exe',[StringComparison]::OrdinalIgnoreCase)){throw 'Detected process path is not Endfield.exe.'}
        $script:status.gamePid=$script:gamePid
        $script:status.gameCreationFileTime=$gameCreation.ToString()
        Write-RecorderJson (Join-Path $OutputDirectory 'game-identity.json') ([ordered]@{pid=$script:gamePid;path=$gameIdentity.ImagePath;creationFileTime=$gameCreation.ToString([Globalization.CultureInfo]::InvariantCulture);creationUtc=[DateTime]::FromFileTimeUtc([long]$gameCreation).ToString('o')})
        $session=Find-RecorderSession 'C:/ProgramData/EndfieldXeSSObserver/Sessions' $script:gamePid $gameIdentity
        $session['retentionPinRegistered']=Protect-RecorderSession $session
        Write-RecorderJson (Join-Path $OutputDirectory 'session-discovery.json') $session
        if($session.state -eq 'matched') {
            $script:status.sessionId=$session.match.sessionId;$script:status.mode='Observer present; selected sources only'
            Start-RecorderContextStreams $script:selectedCollectors $session.match.directory $OutputDirectory
            if($script:selectedCollectors.measurements) {
                Set-RecorderSource 'measurements' 'collecting' 'Copying existing measurement fields; instrumentation settings are unchanged.'
            }
            if($script:selectedCollectors.logs){Set-RecorderSource 'logs' 'collecting' 'Streaming existing SR observations; Observer/EFU tails and retained SR files will also be saved at Stop.'}
        } else {
            $script:status.mode='Observer '+$session.state+'; selected sources only'
            $availability=Get-RecorderMissingSessionStatus $session.state
            foreach($key in @('measurements','logs')){if($script:selectedCollectors[$key]){
                Set-RecorderSource $key 'unavailable' $availability.message
                if(-not $availability.expected){$script:warnings.Add($key+': '+$availability.message)}
            }}
        }
        Write-RecorderJson (Join-Path $OutputDirectory 'configuration-start.json') (Get-RecorderConfigurationSnapshot 'start' $false $script:latestMeasurement);$configurationStarted=$true
        $script:stop=Get-RecorderStop $stopPath $OwnerPid $OwnerCreationFileTime $script:gamePid $gameCreation 0
        if(-not $script:stop -and $script:selectedCollectors.presentmonRaw) {
            $inventory=Join-Path $PSScriptRoot 'etw-readonly-inventory.exe'
            if(-not [IO.File]::Exists($inventory)){$inventory=Join-Path $PSScriptRoot '../../out/20261016-low-stutter/etw-readonly-inventory.exe'}
            try {
                $inventoryChild=Start-RecorderOwned 'etw-readonly-inventory' $inventory @() $OutputDirectory 'etw-before.json' 'etw-inventory.stderr.txt'
                $inventoryResult=Complete-RecorderOwned $inventoryChild 5000;$collectorResults+=$inventoryResult
                if($inventoryResult.exitCode -ne 0 -or $inventoryResult.forcedTermination){throw 'Read-only ETW inventory failed.'}
                $inventorySummary=Get-RecorderInventorySummary ([IO.File]::ReadAllText((Join-Path $OutputDirectory 'etw-before.json'))|ConvertFrom-Json @recorderJsonOptions)
                Write-RecorderJson (Join-Path $OutputDirectory 'etw-summary.json') $inventorySummary
                if(-not $inventorySummary.kernelProcessHeadroomBelowEight){Set-RecorderPresentMonFallback 'Kernel-Process already has eight or more enabled logger IDs; no new ETW session was started.'}
            } catch {Set-RecorderPresentMonFallback ('ETW inventory unavailable; no new session was started: '+$_.Exception.Message)}
            $script:stop=Get-RecorderStop $stopPath $OwnerPid $OwnerCreationFileTime $script:gamePid $gameCreation 0
        }
        if(-not $script:stop) {
            $script:status.startedUtc=[DateTime]::UtcNow.ToString('o');$script:status.startQpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();$safety=[long]$script:status.startQpcTicks+86400L*[Diagnostics.Stopwatch]::Frequency
            $shell=$powerShellRuntime.executable
            $helperPlan=@(Get-RecorderHelperPlan $script:selectedCollectors $script:gamePid $gameCreation $OutputDirectory $PSScriptRoot $shell ($request.recordMenuKeys -eq $true))
            foreach($plan in $helperPlan) {
                $script:stop=Get-RecorderStop $stopPath $OwnerPid $OwnerCreationFileTime $script:gamePid $gameCreation $safety
                if($script:stop){break}
                try {$children+=Start-RecorderPlannedHelper $plan $OutputDirectory;foreach($key in $plan.keys){Set-RecorderSource $key 'starting' ('Waiting for '+$plan.kind+' readiness')}}
                catch {foreach($key in $plan.keys){Set-RecorderSource $key 'unavailable' $_.Exception.Message};$script:warnings.Add($plan.kind+': '+$_.Exception.Message)}
            }
            if(-not $script:stop -and $script:selectedCollectors.presentmonRaw -and $script:status.presentMonSource -eq 'pending') {
                try {
                    $pmSession='EndfieldRecorder-'+[guid]::NewGuid().ToString('N')
                    if((Get-RecorderEtwSession $pmSession).exists){$pmSession=$null;throw 'Fresh recording GUID already exists; no session was stopped.'}
                    $pmArgs=@('--process_id',$script:gamePid.ToString(),'--output_stdout','--timed','86400','--terminate_after_timed','--terminate_on_proc_exit','--v2_metrics','--track_frame_type','--qpc_time_ms','--no_console_stats','--set_circular_buffer_size','8192','--session_name',$pmSession)
                    $pm=Start-RecorderOwned 'presentmon' $pmExe $pmArgs $OutputDirectory 'presentmon.csv' 'presentmon.stderr.txt';$children+=$pm
                } catch {Set-RecorderPresentMonFallback ('Independent PresentMon could not start: '+$_.Exception.Message)
                    if([IO.File]::Exists((Join-Path $OutputDirectory 'presentmon.csv'))){[IO.File]::Move((Join-Path $OutputDirectory 'presentmon.csv'),(Join-Path $OutputDirectory 'presentmon-not-ready.stdout.txt'))}}
            }
            $readyDeadline=[Diagnostics.Stopwatch]::GetTimestamp()+15L*[Diagnostics.Stopwatch]::Frequency;$ready=$false
            while(-not $ready -and -not $script:stop) {
                $script:stop=Get-RecorderStop $stopPath $OwnerPid $OwnerCreationFileTime $script:gamePid $gameCreation $safety
                if($script:stop){break}
                $expired=[Diagnostics.Stopwatch]::GetTimestamp() -gt $readyDeadline
                if($pm -and $script:status.presentMonSource -eq 'pending') {
                    $pmProblem=$null
                    try {
                        if($pm.process.HasExited){$pmProblem='PresentMon exited before readiness; see presentmon.stderr.txt.'}
                        elseif((Get-RecorderEtwSession $pmSession).exists){$pmStarted=$true;$script:status.independentPresentMonRaw=$true;$script:status.presentMonSource='independent';Set-RecorderSource 'presentmonRaw' 'collecting' 'Independent raw frame capture ready.'}
                        elseif($expired){$pmProblem='Independent PresentMon session did not become ready.'}
                    } catch {$pmProblem='Independent PresentMon readiness query failed: '+$_.Exception.Message}
                    if($pmProblem) {
                        Set-RecorderPresentMonFallback $pmProblem
                        if((Get-RecorderEtwSession $pmSession).exists){$pmStarted=$true;$pmStop=Stop-RecorderPresentMon $pmExe $pmSession $pmSession $OutputDirectory;if(-not $pmStop.sessionReleased){throw 'Own unready PresentMon session could not be released.'};$pmStarted=$false}
                        $null=Complete-RecorderOwned $pm 0
                        if([IO.File]::Exists((Join-Path $OutputDirectory 'presentmon.csv'))){[IO.File]::Move((Join-Path $OutputDirectory 'presentmon.csv'),(Join-Path $OutputDirectory 'presentmon-not-ready.stdout.txt'))}
                    }
                }
                $pending=0
                foreach($child in @($children|Where-Object {$_.kind -ne 'presentmon' -and -not $_.result})) {
                    $keys=@($child.keys|Where-Object {$script:sourceStatus[$_].status -eq 'starting'})
                    if($keys.Count -eq 0){continue}
                    $readyPath=Join-Path $OutputDirectory ($child.kind+'.ready.json')
                    try {
                        if([IO.File]::Exists($readyPath)) {
                            $readyData=[IO.File]::ReadAllText($readyPath)|ConvertFrom-Json @recorderJsonOptions
                            foreach($key in $keys){$detail=$readyData.sourceStatus.$key;Set-RecorderSource $key (Get-RecorderReadySourceState $readyData $key) $(if($detail.message){[string]$detail.message}else{[string]$readyData.message})}
                            if($readyData.state -ne 'ready' -or $readyData.partialSource){$script:warnings.Add($child.kind+': '+$readyData.state+'; '+$readyData.message)}
                        } elseif($child.process.HasExited -or $expired) {
                            foreach($key in $keys){Set-RecorderSource $key 'unavailable' ($child.kind+' exited or did not report readiness.')}
                            $script:warnings.Add($child.kind+' exited or did not report readiness; other selected sources continue.');$null=Complete-RecorderOwned $child 0
                        } else {$pending++}
                    } catch {if($expired){foreach($key in $keys){Set-RecorderSource $key 'unavailable' $_.Exception.Message};$script:warnings.Add($child.kind+' readiness: '+$_.Exception.Message);$null=Complete-RecorderOwned $child 0}else{$pending++}}
                }
                $ready=$pending -eq 0 -and $script:status.presentMonSource -ne 'pending'
                if(-not $ready){Start-Sleep -Milliseconds 100}
            }
            if($ready) {
                $recordingStarted=$true;$script:status['recordingReadyQpcTicks']=[Diagnostics.Stopwatch]::GetTimestamp()
                Save-RecorderStatus 'recording' 'Recording. Click Stop when finished.'
            }
        }
    }
    elseif(-not $script:stop) {
        Write-RecorderJson (Join-Path $OutputDirectory 'configuration-start.json') (Get-RecorderConfigurationSnapshot 'start' $true $null);$configurationStarted=$true
        if($script:selectedCollectors.presentmonRaw -and $request.testOnlyPresentMonFallback){Set-RecorderPresentMonFallback ('TEST ONLY injected '+[string]$request.testOnlyPresentMonFallback)}
        $helperPlan=@(Get-RecorderHelperPlan $script:selectedCollectors 0 0 $OutputDirectory $PSScriptRoot 'TEST-ONLY-no-process' ($request.recordMenuKeys -eq $true))
        Write-RecorderJson (Join-Path $OutputDirectory 'TEST-ONLY-helper-plan.json') ([ordered]@{testOnly=$true;helpers=$helperPlan;inventoryWouldRun=[bool]$script:selectedCollectors.presentmonRaw;newEtwSessions=0})
        $script:status.startedUtc=[DateTime]::UtcNow.ToString('o');$script:status.startQpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();$safety=[long]$script:status.startQpcTicks+86400L*[Diagnostics.Stopwatch]::Frequency
        $synthetic=[Collections.Generic.List[object]]::new()
        foreach($key in $script:selectedCollectors.Keys) {
            if(-not $script:selectedCollectors[$key] -or $script:sourceStatus[$key].status -eq 'unavailable'){continue}
            $file='TEST-ONLY-'+$key+'.jsonl';$writer=[IO.StreamWriter]::new((Join-Path $OutputDirectory $file),$false,[Text.UTF8Encoding]::new($false))
            $writer.WriteLine('{"TEST_ONLY":true,"source":"'+$key+'"}');$writer.Flush();$synthetic.Add([pscustomobject]@{key=$key;writer=$writer})
            $script:sourceStatus[$key].files=@($file);Set-RecorderSource $key 'test_only' 'Simulated source; no game, ETW or real helper was started.'
        }
        if($script:selectedCollectors.presentmonRaw -and $script:status.presentMonSource -eq 'pending'){$script:status.presentMonSource='test_only'}
        $recordingStarted=$true;$script:status['recordingReadyQpcTicks']=[Diagnostics.Stopwatch]::GetTimestamp()
        Save-RecorderStatus 'recording' 'TEST ONLY: simulated recording. Click Stop when finished.'
    }
    if($recordingStarted -and $script:recordingContext.capturePowerState){$powerStateWriter=[IO.StreamWriter]::new((Join-Path $OutputDirectory 'power-state.jsonl'),$false,[Text.UTF8Encoding]::new($false))}
    $lastUpdate=[long]0;$lastStream=[long]0;$lastPowerState=[long]0
    while($recordingStarted -and -not $script:stop) {
        $script:stop=Get-RecorderStop $stopPath $OwnerPid $OwnerCreationFileTime $script:gamePid $gameCreation $safety
        if($script:stop){break}
        $now=[Diagnostics.Stopwatch]::GetTimestamp()
        if($powerStateWriter -and $now-$lastPowerState -ge 10*[Diagnostics.Stopwatch]::Frequency){$powerStateWriter.WriteLine(((Get-RecorderPowerState ([bool]$Simulate))|ConvertTo-Json -Depth 5 -Compress));$powerStateWriter.Flush();$powerStateSamples++;$lastPowerState=$now}
        if($Simulate){foreach($entry in $synthetic){$entry.writer.WriteLine('{"TEST_ONLY":true,"qpcTicks":'+$now+'}');$entry.writer.Flush()}}
        elseif($now-$lastStream -ge [Diagnostics.Stopwatch]::Frequency/2){Update-RecorderStreams;$lastStream=$now}
        if(-not $Simulate) {
            foreach($child in $children) {
                if($child.result -or -not $child.process.HasExited){continue}
                if($child.kind -eq 'presentmon') {
                    $script:stop=Get-RecorderStop $stopPath $OwnerPid $OwnerCreationFileTime $script:gamePid $gameCreation $safety
                    if($script:stop){break}
                    Set-RecorderPresentMonFallback 'PresentMon exited during recording; any collected raw rows are retained.'
                    if((Get-RecorderEtwSession $pmSession).exists){$pmStop=Stop-RecorderPresentMon $pmExe $pmSession $pmSession $OutputDirectory;if(-not $pmStop.sessionReleased){throw 'Own exited PresentMon session could not be released.'}}
                    $pmStarted=$false
                } else {foreach($key in $child.keys){Set-RecorderSource $key 'partial' ($child.kind+' exited during recording.')};$script:warnings.Add($child.kind+' exited during recording; that source has partial coverage.')}
                $null=Complete-RecorderOwned $child 0
            }
        }
        if($now-$lastUpdate -ge [Diagnostics.Stopwatch]::Frequency){Save-RecorderStatus 'recording' $(if($Simulate){'TEST ONLY: simulated recording. Click Stop when finished.'}else{'Recording. Click Stop when finished.'});$lastUpdate=$now}
        Start-Sleep -Milliseconds 100
    }
} catch {$failure=$_.Exception.Message;Write-RecorderError $OutputDirectory 'capture' $_}
finally {
    if(-not $script:stop) {
        $script:stop=[ordered]@{reason='capture_failed';utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;source='worker';validRequest=$true}
        try {if(-not [IO.File]::Exists($stopPath)){Write-RecorderJson $stopPath $script:stop}}catch{}
    }
    try {Save-RecorderStatus 'saving' 'Saving recording data'}catch{Write-RecorderError $OutputDirectory 'saving_status' $_;if(-not $failure){$failure=$_.Exception.Message}}
    if($powerStateWriter){$powerStateWriter.Dispose()}
    if($synthetic){foreach($entry in $synthetic){$entry.writer.Dispose()}}
    if($pm -and -not $pmStarted){try{$pmStarted=(Get-RecorderEtwSession $pmSession).exists}catch{if(-not $failure){$failure=$_.Exception.Message}}}
    if($pmStarted) {
        try {$pmStop=Stop-RecorderPresentMon $pmExe $pmSession $pmSession $OutputDirectory
            if(-not $pmStop.sessionReleased -or $pmStop.forcedTermination -or ($pmStop.stopRequested -and $pmStop.stopExitCode -ne 0)){throw 'PresentMon GUID session stop did not complete cleanly.'}
        } catch {if(-not $failure){$failure=$_.Exception.Message}}
    }
    foreach($child in $children) {
        try {$result=Complete-RecorderOwned $child 5000;$collectorResults+=$result
            if($result.forcedTermination -or $result.exitCode -ne 0){$script:warnings.Add($child.kind+' exit code '+$result.exitCode+', forced termination '+$result.forcedTermination+'.');if($child.kind -eq 'presentmon'){Set-RecorderSource 'presentmonRaw' 'partial' 'PresentMon did not exit cleanly.'}else{foreach($key in $child.keys){Set-RecorderSource $key 'partial' ($child.kind+' did not exit cleanly.')}}}
        }catch{if(-not $failure){$failure='Collector shutdown: '+$_.Exception.Message}}
    }
    if($pm -and -not $pmStarted) {
        try {if((Get-RecorderEtwSession $pmSession).exists){$pmStop=Stop-RecorderPresentMon $pmExe $pmSession $pmSession $OutputDirectory;if(-not $pmStop.sessionReleased){throw 'Own late-starting PresentMon session could not be released.'}}}
        catch{if(-not $failure){$failure=$_.Exception.Message}}
    }
    try {Update-RecorderStreams -Drain}catch{$script:warnings.Add('Final observer stream drain: '+$_.Exception.Message)}
    foreach($entry in $script:streams){$entry.reader.Dispose();if($entry.lastError){$script:warnings.Add($entry.file+': '+$entry.lastError)};if($entry.backlogBytes -gt 0){$script:warnings.Add($entry.file+' has '+$entry.backlogBytes+' uncopied trailing bytes (partial line or bounded final drain).')}}
    if($script:streamEvents){$script:streamEvents.Dispose()}
    if($script:srStreamEvents){$script:srStreamEvents.Dispose()}
    if($configurationStarted){try{Write-RecorderJson (Join-Path $OutputDirectory 'configuration-stop.json') (Get-RecorderConfigurationSnapshot 'stop' ([bool]$Simulate) $script:latestMeasurement)}catch{$script:warnings.Add('Configuration stop snapshot: '+$_.Exception.Message)}}
}

try {
    $markerClose=Wait-RecorderMarkersClosed $OutputDirectory $request $OwnerPid $OwnerCreationFileTime
    $frameSources=[ordered]@{}
    if(-not $Simulate) {
        foreach($source in @(@('pmdp','pmdp-run.json','frames.csv'),@('rtss','rtss-run.json','rtss-present-intervals.csv'),@('presentmonRaw',$null,'presentmon.csv'))) {
            $key=$source[0];if(-not $script:selectedCollectors[$key]){continue}
            $rows=$null
            try {if($key -eq 'presentmonRaw'){$rows=[RecorderNative]::CsvRows((Join-Path $OutputDirectory $source[2]))}else{$stats=[IO.File]::ReadAllText((Join-Path $OutputDirectory $source[1]))|ConvertFrom-Json @recorderJsonOptions;if($null -ne $stats.rows){$rows=[long]$stats.rows}}}catch{}
            $frameSources[$key]=[ordered]@{file=$source[2];rows=$rows;state=$(if($null -eq $rows){'missing'}elseif($rows -eq 0){'zero_rows'}else{'collected'})}
            Set-RecorderSource $key $(if($rows -gt 0){if($script:sourceStatus[$key].status -in @('partial','unavailable')){'partial'}else{'collected'}}else{'unavailable'}) $(if($rows -gt 0){'Captured '+$rows+' rows.'}else{'No frame data; source missing or zero rows.'}) ([ordered]@{rows=$rows})
            if($null -eq $rows -or $rows -eq 0){$script:warnings.Add($key+' frame data is missing or has zero rows; no frames were fabricated.')}
            if([IO.File]::Exists((Join-Path $OutputDirectory $source[2]))){$script:sourceStatus[$key].files=@($source[2])}
        }
        foreach($key in @('hardware','foreground')) {
            if(-not $script:selectedCollectors[$key]){continue}
            $samples=0;$bad=0;$stats=$null
            try {$stats=[IO.File]::ReadAllText((Join-Path $OutputDirectory 'power-run.json'))|ConvertFrom-Json @recorderJsonOptions;$samples=[long]$stats.samples;if($key -eq 'hardware'){$bad=[long]$stats.errorSamples}}catch{}
            $file=$(if($script:selectedCollectors.hardware){'power.jsonl'}else{'foreground.jsonl'})
            if([IO.File]::Exists((Join-Path $OutputDirectory $file))){$script:sourceStatus[$key].files=@($file)}
            $coverage=[ordered]@{samples=$samples;errorSamples=$bad;usableSamples=[Math]::Max(0,$samples-$bad)}
            if($key -eq 'foreground' -and $stats.operationKeyObservation){$coverage['operationKeyObservation']=$stats.operationKeyObservation}
            $usable=$coverage.usableSamples;Set-RecorderSource $key $(if($usable -eq 0){'unavailable'}elseif($bad -gt 0 -or $script:sourceStatus[$key].status -eq 'partial'){'partial'}else{'collected'}) ('Samples '+$samples+', unavailable '+$bad+'.') $coverage
            if($usable -eq 0){$script:warnings.Add($key+' has no usable samples.')}
        }
        $systemStats=$null
        if($script:selectedCollectors.system -or $script:selectedCollectors.threads){try {$systemStats=[IO.File]::ReadAllText((Join-Path $OutputDirectory 'system-run.json'))|ConvertFrom-Json @recorderJsonOptions}catch{}}
        foreach($key in @('system','threads')) {
            if(-not $script:selectedCollectors[$key]){continue}
            $pattern=$(if($key -eq 'system'){'system*.jsonl'}else{'thread*.json*'})
            $evidence=@(Get-ChildItem -LiteralPath $OutputDirectory -File -Filter $pattern|Where-Object {$_.Length -gt 0 -and $_.Name -ne 'system.ready.json'})
            if($key -eq 'system'){$evidence+=@(Get-ChildItem -LiteralPath $OutputDirectory -File -Filter 'services-*.json'|Where-Object {$_.Length -gt 0})}
            $script:sourceStatus[$key].files=@($evidence|ForEach-Object {$_.Name})
            $count=$(if($key -eq 'system'){$systemStats.samples}else{$systemStats.threadSnapshots})
            $coverage=$(if($systemStats.coverage.$key){$systemStats.coverage.$key}else{[ordered]@{dataFiles=$evidence.Count}})
            $unavailable=$evidence.Count -eq 0 -or ($null -ne $count -and [long]$count -eq 0)
            $detail=$systemStats.sourceStatus.$key
            Set-RecorderSource $key $(if($unavailable){'unavailable'}elseif($script:sourceStatus[$key].status -eq 'partial' -or $detail.status -eq 'partial'){'partial'}else{'collected'}) $(if($unavailable){'No source samples were produced.'}elseif($detail.message){[string]$detail.message}else{'Saved passive source data; consult per-sample availability fields.'}) $coverage
            if($unavailable -or $detail.status -eq 'partial'){$script:warnings.Add($key+': '+$script:sourceStatus[$key].message)}
        }
    }
    if($session -and $session.state -eq 'matched') {
        $saved=Save-RecorderContextSnapshots $script:selectedCollectors $session.match.directory $OutputDirectory
        $snapshots=$saved.snapshots;foreach($note in $saved.warnings){$script:warnings.Add($note)}
    }
    $streamSummary=@($script:streams|ForEach-Object {[ordered]@{key=$_.key;source=$_.source;file=$_.file;copiedBytes=$_.bytes;remainingBytesAtLastPoll=$_.backlogBytes;detectedRotations=$_.rotations;lastError=$_.lastError}})
    if(-not $Simulate) {
        foreach($key in @('measurements','logs')) {
            if(-not $script:selectedCollectors[$key]){continue}
            $context=Get-RecorderContextSourceSummary $key $snapshots $streamSummary
            $script:sourceStatus[$key].files=$context.files
            Set-RecorderSource $key $context.state $context.message $context.coverage
        }
    }
    foreach($key in $script:sourceStatus.Keys){if($script:sourceStatus[$key].status -in @('pending','starting','collecting')){Set-RecorderSource $key 'stopped_before_start' 'Stopped before source data was ready.'}}
    $hasData=@($script:sourceStatus.Values|Where-Object {$_.status -in @('collected','partial','test_only') -and $_.files.Count -gt 0}).Count -gt 0
    $script:status['noData']=-not $hasData
    if($recordingStarted -and -not $hasData){$script:warnings.Add('No data: none of the selected sources produced usable recording data.')}
    $window=[ordered]@{testOnly=[bool]$Simulate;powerShellRuntime=$powerShellRuntime;recordingStarted=$recordingStarted;selectedCollectors=$script:selectedCollectors;sourceStatus=$script:sourceStatus;requestedStartUtc=$request.utc;requestedStartQpcTicks=$request.qpcTicks;startQpcTicks=$script:status.startQpcTicks;recordingReadyQpcTicks=$script:status.recordingReadyQpcTicks
        endQpcTicks=$script:stop.qpcTicks;qpcFrequency=[Diagnostics.Stopwatch]::Frequency;stop=$script:stop;gamePid=$script:gamePid;gameCreationFileTime=$gameCreation.ToString();sessionId=$script:status.sessionId;mode=$script:status.mode;durationSeconds=$script:status.durationSeconds
        measurementStreams=@($streamSummary|Where-Object key -eq 'measurements');srObservationStreams=@($streamSummary|Where-Object key -eq 'logs');finalSnapshots=$snapshots;coverageNote='Only selected collectors run. Collector startup and shutdown have distinct QPC timestamps; data is never padded. Measurement and SR observation streams contain existing context, with no instrumentation enabled by the recorder; use this QPC window. Rotations between polls and overwritten tails cannot be recovered. RTSS ring overruns are heuristic, not lossless.'}
    $window['recordingContext']=$script:recordingContext;$window['measurementMode']=$script:recordingContext.measurementMode;$window['detailIncluded']=$script:recordingContext.detailIncluded;$window['markerClose']=$markerClose
    Write-RecorderJson (Join-Path $OutputDirectory 'window.json') $window
    $finalState=$(if($failure){'failed'}else{'complete'})
    $summary=[ordered]@{state=$finalState;testOnly=[bool]$Simulate;powerShellRuntime=$powerShellRuntime;recordingStarted=$recordingStarted;selectedCollectors=$script:selectedCollectors;sourceStatus=$script:sourceStatus;noData=(-not $hasData);error=$failure;stopReason=$script:stop.reason;outputDirectory=$OutputDirectory;gamePid=$script:gamePid;gameIdentity=$gameIdentity;sessionId=$script:status.sessionId
        mode=$script:status.mode;latestMeasurementStatus=$script:latestMeasurement;measurementStreams=@($streamSummary|Where-Object key -eq 'measurements');srObservationStreams=@($streamSummary|Where-Object key -eq 'logs');startedUtc=$script:status.startedUtc;durationSeconds=$script:status.durationSeconds;collectors=$collectorResults;presentMonSession=$pmSession;presentMonStop=$pmStop;presentMonSource=$script:status.presentMonSource;independentPresentMonRaw=$script:status.independentPresentMonRaw;presentMonFallbackReason=$script:status.presentMonFallbackReason;frameSources=$frameSources;warnings=@($script:warnings|Select-Object -Unique)
        gameLaunched=$false;gameOrInstallModified=$false;rtssConfigurationModified=$false;preexistingEtwSessionsStopped=$false;finishedUtc=[DateTime]::UtcNow.ToString('o')}
    if(-not $script:recordingContext){$script:recordingContext=Resolve-RecorderRequestContext ([pscustomobject]@{})}
    foreach($key in @('label','labelCode','notes','xiaomiPerformanceMode','xiaomiPerformanceModeSource','measurementMode','detailIncluded')){$summary[$key]=$script:recordingContext[$key]}
    $summary['markerClose']=$markerClose;$summary['configurationSnapshots']=@('configuration-start.json','configuration-stop.json'|Where-Object {[IO.File]::Exists((Join-Path $OutputDirectory $_))});$summary['powerState']=[ordered]@{selected=$script:recordingContext.capturePowerState;samples=$powerStateSamples;intervalSeconds=10;file=$(if($powerStateSamples -gt 0){'power-state.jsonl'}else{$null})}
    $analysis=Write-RecorderReadableSummary $OutputDirectory $window $summary $script:recordingContext
    $summary['markerCount']=$analysis.markers.validCount;$summary['markerInvalidRows']=$analysis.markers.invalidRows;$summary['markerIntegrity']=$analysis.markerIntegrity;$summary['steadyVerified']=$analysis.steadyVerified;$summary['analysisFile']='summary.analysis.json';$summary['readableSummaryFile']='summary.md';$summary['sizeBudget']=$analysis.sizeBudget
    foreach($note in $analysis.markerIntegrity.warnings){$script:warnings.Add($note)};$summary.warnings=@($script:warnings|Select-Object -Unique)
    Write-RecorderJson (Join-Path $OutputDirectory 'summary.json') $summary
    $files=@(Get-ChildItem -LiteralPath $OutputDirectory -File -Recurse|Where-Object {$_.Name -ne 'manifest.json' -and (Test-RecorderArtifact $_.Name)}|ForEach-Object {[ordered]@{file=$_.FullName.Substring($OutputDirectory.Length+1).Replace('\','/');bytes=$_.Length;sha256=Get-RecorderSha256 $_.FullName}})
    $sources=@($PSCommandPath,(Join-Path $PSScriptRoot 'RecorderContext.ps1'),(Join-Path $PSScriptRoot 'RecorderSummary.ps1'))+@($helperPlan|ForEach-Object {$_.source})
    if(-not $Simulate -and $script:selectedCollectors.presentmonRaw){$sources+=@($pmExe,$inventory)}
    $sourceHashes=@($sources|Where-Object {$_ -and [IO.File]::Exists($_)}|Sort-Object -Unique|ForEach-Object {[ordered]@{path=$_;sha256=Get-RecorderSha256 $_}})
    Write-RecorderJson (Join-Path $OutputDirectory 'manifest.json') ([ordered]@{schema=2;testOnly=[bool]$Simulate;powerShellRuntime=$powerShellRuntime;state=$finalState;selectedCollectors=$script:selectedCollectors;sourceStatus=$script:sourceStatus;noData=(-not $hasData);files=$files;collectorSources=$sourceHashes;mutableFilesExcludedFromHashesAndZip=@('status.json','*.tmp','worker.*','backend.*','recorder-worker.*');hashAlgorithm='SHA256';createdUtc=[DateTime]::UtcNow.ToString('o')})
    $zip=$OutputDirectory+'.zip';if([IO.File]::Exists($zip)){$zip=$OutputDirectory+'.'+[guid]::NewGuid().ToString('N')+'.zip'}
    try {New-RecorderZip $OutputDirectory $zip;$script:status.zipPath=$zip}catch{$script:warnings.Add('ZIP unavailable; the recording folder is saved: '+$_.Exception.Message);$script:status['zipError']=$_.Exception.Message}
    $script:status['warningCount']=@($script:warnings|Select-Object -Unique).Count
    if($failure){Save-RecorderStatus 'failed' ($failure+' Data saved in '+$OutputDirectory)}
    elseif(-not $recordingStarted){Save-RecorderStatus 'complete' 'Stopped before collectors started. Data saved.'}
    elseif(-not $hasData){Save-RecorderStatus 'complete' 'No data: selected sources were unavailable. Diagnostics and selection were saved.'}
    elseif($Simulate){Save-RecorderStatus 'complete' 'TEST ONLY recording saved.'}
    else {Save-RecorderStatus 'complete' $(if($script:status.warningCount){'Recording saved with partial-source notes. See summary.json.'}else{'Recording saved.'})}
} catch {
    Write-RecorderError $OutputDirectory 'save' $_
    $script:status['saveError']=$_.Exception.Message
    Save-RecorderStatus 'failed' ('Saving could not finish: '+$_.Exception.Message+'. Raw data remains in '+$OutputDirectory)
}
if($script:status.state -eq 'failed'){exit 1}
