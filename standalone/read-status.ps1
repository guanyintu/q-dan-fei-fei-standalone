param([Parameter(Mandatory)][int]$GameProcessId, [switch]$RequireRunning)
$ErrorActionPreference = 'Stop'
$mappingName = 'Local\QBounceStandalone-' + $GameProcessId
$mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting($mappingName, [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
try {
    $view = $mapping.CreateViewAccessor(0, 600, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
    try {
        $bytes = New-Object byte[] 512
        [void]$view.ReadArray(88, $bytes, 0, 512)
        $state = [ordered]@{
            Stage = $view.ReadInt32(64); Enabled = $view.ReadInt32(12); Processed = $view.ReadInt32(68)
            Tracked = $view.ReadInt32(72); Hooks = $view.ReadInt32(76); Scene = $view.ReadInt32(80)
            Error = [Text.Encoding]::Unicode.GetString($bytes).TrimEnd([char]0)
        }
        $state | ConvertTo-Json
        if ($RequireRunning -and ($state.Stage -ne 2 -or $state.Enabled -ne 1 -or $state.Processed -eq 0)) {
            throw "独立版未实际运行：$($state.Error)"
        }
    } finally { $view.Dispose() }
} finally { $mapping.Dispose() }
