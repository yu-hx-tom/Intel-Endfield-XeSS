#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('Enable','Disable','Status')][string]$Action)
$ErrorActionPreference='Stop'
if($Action -eq 'Disable'){throw 'The FSR + XeSS overlay route has been removed. To use the game without XeSS SR, close it and launch without --xess-sr.'}
[ordered]@{policy='XeSS replacement or engine native resolution';requiresXessSrArgument=$true;nativeFsrCorePermitted=$false;markerRequired=$false;shadowEligibleFrames=0}|ConvertTo-Json
