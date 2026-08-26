param(
    [Parameter(Mandatory = $true)]
    [string]$BuildPath,

    [Parameter(Mandatory = $true)]
    [string]$IncludePath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $BuildPath -PathType Container)) {
    throw "Wz import directory does not exist: $BuildPath"
}

New-Item -ItemType Directory -Force -Path $IncludePath | Out-Null

$importMap = @{
    "IWzSeekableArchive" = @("IWzArchive")
    "IWzSerialize" = @("IWzArchive")
    "IWzProperty" = @("IWzSerialize")
    "IWzWritableNameSpace" = @("IWzNameSpace")
    "IWzPackage" = @("IWzSeekableArchive", "IWzNameSpace")
    "IWzFileSystem" = @("IWzWritableNameSpace")
    "IWzResMan" = @("IWzArchive")
    "IWzUOL" = @("IWzSerialize")
    "IWzCanvas" = @("IWzSerialize", "IWzProperty")
    "IWzSound" = @("IWzSerialize")
    "IWzShape2D" = @("IWzSerialize")
    "IWzConvex2D" = @("IWzShape2D")
    "IWzVector2D" = @("IWzShape2D")
    "IWzGr2DLayer" = @("IWzCanvas", "IWzVector2D")
    "IWzGr2D" = @("IWzGr2DLayer")
    "IWzGr2DLayer_DX9" = @("IWzGr2DLayer")
    "IWzGr2D_DX9" = @("IWzGr2DLayer_DX9")
}

$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
$commentPattern = '(?m)^[^\r\n]*//.*(?:\r?\n)+'
$libIdPattern = 'struct\s+__declspec\(uuid\("d15c6042-5770-4007-8608-ebf3a1ed8083"\)\)\s*/\*\s*LIBID\s*\*/\s*__WzLib;\r?\n'
$tliPattern = '#include\s+"([^"]+\.tli)"'

Get-ChildItem -LiteralPath $BuildPath -Filter *.tlh -File | ForEach-Object {
    $interfaceName = $_.BaseName
    $content = [System.IO.File]::ReadAllText($_.FullName)
    $pragmaIndex = $content.IndexOf("#pragma once")
    if ($pragmaIndex -lt 0) {
        throw "Generated import has no #pragma once: $($_.FullName)"
    }

    $content = $content.Substring($pragmaIndex)
    $content = $content.Replace("      virtual", "    virtual")
    $content = [regex]::Replace($content, $commentPattern, "")
    $content = [regex]::Replace($content, $libIdPattern, "")

    $sourceDirectory = $_.DirectoryName
    $content = [regex]::Replace($content, $tliPattern, {
        param($match)

        $tliPath = $match.Groups[1].Value
        if (-not [System.IO.Path]::IsPathRooted($tliPath)) {
            $tliPath = Join-Path $sourceDirectory $tliPath
        }
        $tliContent = [System.IO.File]::ReadAllText($tliPath)
        $tliPragmaIndex = $tliContent.IndexOf("#pragma once")
        if ($tliPragmaIndex -ge 0) {
            $tliContent = $tliContent.Substring($tliPragmaIndex + "#pragma once".Length).TrimStart("`r", "`n")
        }
        return [regex]::Replace($tliContent, $commentPattern, "")
    })

    $includes = @()
    if ($importMap.ContainsKey($interfaceName)) {
        $includes += $importMap[$interfaceName] | ForEach-Object { '#include "' + $_ + '.h"' }
    }
    $includes += '#include "zcomdef.h"'
    $content = $content.Replace("#include <comdef.h>", ($includes -join "`r`n"))

    $content = $content.Replace("raw_GetObjectA", "raw_GetObject")
    $content = $content.Replace("raw_DrawTextA", "raw_DrawText")
    $content = $content.Replace("raw_screenResolution", "put_screenResolution")
    $content = $content.Replace("_bstr_t", "Ztl_bstr_t")
    $content = $content.Replace("_variant_t", "Ztl_variant_t")
    $content = $content.Replace("vtMissing", "vtEmpty")

    $outputPath = Join-Path $IncludePath ($interfaceName + ".h")
    [System.IO.File]::WriteAllText($outputPath, $content, $utf8WithoutBom)
}

Copy-Item -LiteralPath (Join-Path $PSScriptRoot "zcomdef.h") -Destination (Join-Path $IncludePath "zcomdef.h") -Force
