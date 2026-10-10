param(
    [Parameter(Mandatory = $true)][string]$SchemaDir,
    [Parameter(Mandatory = $true)][string]$MpdList
)

# Validates the MPDs named in $MpdList (one path a line) against ISO/IEC
# 23009-1's schema with .NET's XSD 1.0 validator. The fallback for a machine
# with no xmllint - Windows, in practice; tools/checks/verify_dash_schema.py
# prefers xmllint and explains the rest.
#
# Prints "VALID   <path>" or "INVALID <path>" followed by indented messages,
# one verdict per manifest, and exits 1 if any was invalid.
#
# Three of the schema documents carry a DOCTYPE (the ISO schema declares its
# URI pattern fragments as internal entities), which the default reader
# refuses, so each is read with DTD processing on and no resolver - nothing
# external is fetched - and added to the set as a parsed schema. Imports are
# then answered by namespace from what is already loaded.
$ErrorActionPreference = 'Stop'

function Read-Xsd([string]$path) {
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Parse
    $settings.XmlResolver = $null
    $reader = [System.Xml.XmlReader]::Create($path, $settings)
    try {
        return [System.Xml.Schema.XmlSchema]::Read($reader, $null)
    } finally {
        $reader.Close()
    }
}

$set = New-Object System.Xml.Schema.XmlSchemaSet
$set.XmlResolver = $null
foreach ($name in 'xml.xsd', 'xlink.xsd', 'DASH-MPD-UP.xsd', 'DASH-MPD.xsd') {
    $null = $set.Add((Read-Xsd (Join-Path $SchemaDir $name)))
}
$set.Compile()

$failed = 0
foreach ($path in (Get-Content -LiteralPath $MpdList | Where-Object { $_.Trim() -ne '' })) {
    $messages = New-Object System.Collections.Generic.List[string]
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.ValidationType = [System.Xml.ValidationType]::Schema
    $settings.Schemas = $set
    $settings.ValidationFlags = [System.Xml.Schema.XmlSchemaValidationFlags]::ReportValidationWarnings
    $settings.add_ValidationEventHandler({
            param($sender, $e)
            # The schema defaults xlink:type/show/actuate on Period, AdaptationSet
            # and friends. .NET reports a defaulted attribute as an error when the
            # instance declares no xlink prefix to put it under - a property of the
            # validator, not of the document (libxml2 applies it silently, and an
            # MPD is not required to bind xlink). Anything else is real.
            if ($e.Message -match "^Default attribute '.*' for element '.*' could not be applied as the attribute namespace is not mapped to a prefix") {
                return
            }
            $messages.Add(('{0}: {1} (line {2})' -f $e.Severity, $e.Message, $e.Exception.LineNumber))
        })
    $reader = [System.Xml.XmlReader]::Create($path, $settings)
    try {
        while ($reader.Read()) { }
    } catch {
        $messages.Add('EXCEPTION: ' + $_.Exception.Message)
    } finally {
        $reader.Close()
    }
    if ($messages.Count -eq 0) {
        Write-Output ('VALID   ' + $path)
    } else {
        $failed++
        Write-Output ('INVALID ' + $path)
        $messages | Select-Object -First 8 | ForEach-Object { Write-Output ('    ' + $_) }
    }
}
exit $(if ($failed -eq 0) { 0 } else { 1 })
