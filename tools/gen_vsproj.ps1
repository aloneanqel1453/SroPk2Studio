<#
  Generates a portable Visual Studio solution (SroPk2Studio.sln / .vcxproj / .vcxproj.filters) for the
  app from the CMake build's compile_commands.json: exactly the translation units CMake compiles for
  SroPk2Studio.exe (test programs excluded), with each file's own defines / warning level / options.
  All paths in the generated project are relative, so the folder can be moved or shared.

  usage: cmake --preset release            (writes obj\cmake\release\compile_commands.json)
         powershell -ExecutionPolicy Bypass -File tools\gen_vsproj.ps1 [-Build <cmake build dir>] [-Out <project root>]
#>
param(
    [string]$Build,
    [string]$Out,
    [string]$Source
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty inside param() defaults: resolve them here.
if (-not $Source) { $Source = Split-Path $PSScriptRoot -Parent }
if (-not $Build) { $Build = Join-Path $Source 'obj\cmake\release' }
if (-not $Out) { $Out = $Source }
$Source = (Resolve-Path $Source).Path.TrimEnd('\')
$cc = Get-Content (Join-Path $Build 'compile_commands.json') -Raw | ConvertFrom-Json

function Rel([string]$p) {
    $full = [IO.Path]::GetFullPath($p)
    if (-not $full.StartsWith($Source + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "outside source: $p" }
    return $full.Substring($Source.Length + 1)
}

# One entry per translation unit of the app (tests and the pk2test console tool are not part of it).
$units = @{}
foreach ($e in $cc) {
    $file = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($e.file)) { $e.file } else { Join-Path $e.directory $e.file }))
    $rel = Rel $file
    $name = [IO.Path]::GetFileName($rel)
    if ($name -match '_test\.cpp$' -or $name -eq 'pk2test.cpp') { continue }
    $cmd = $e.command
    $defs = [regex]::Matches($cmd, '(?:^|\s)[/-]D\s*("?)([^\s"]+)\1') | ForEach-Object { $_.Groups[2].Value }
    $defs = $defs | Where-Object { $_ -notmatch '^(CMAKE_INTDIR|NDEBUG)' } | Sort-Object -Unique
    $incs = [regex]::Matches($cmd, '(?:^|\s)[/-]I\s*("?)([^"\s]+)\1') | ForEach-Object { Rel $_.Groups[2].Value } | Sort-Object -Unique
    $units[$rel] = [pscustomobject]@{
        Rel = $rel; IsC = $rel.EndsWith('.c')
        NoWarn = [bool]($cmd -match '(?:^|\s)[/-]w(\s|$)')
        BigObj = [bool]($cmd -match '/bigobj')
        Defs = @($defs); Incs = @($incs)
    }
}
if ($units.Count -eq 0) { throw 'no translation units found' }

# Project-wide settings = the most common define / include sets; files that differ get per-file values.
$common = $units.Values | Where-Object { -not $_.NoWarn -and -not $_.IsC } | Group-Object { $_.Defs -join ';' } | Sort-Object Count -Descending | Select-Object -First 1
$commonDefs = $common.Group[0].Defs
$allIncs = $units.Values | ForEach-Object { $_.Incs } | Sort-Object -Unique
$x = { param($s) [Security.SecurityElement]::Escape($s) }

$cl = New-Object Text.StringBuilder
$hd = New-Object Text.StringBuilder
foreach ($u in ($units.Values | Sort-Object Rel)) {
    $extra = @()
    $d = $u.Defs -join ';'
    # A file with its own define set gets exactly that set (plus the configuration's NDEBUG / _DEBUG):
    # inheriting the project-wide list would add defines CMake never passes to it.
    if ($d -ne ($commonDefs -join ';')) {
        $pre = if ($d) { "$(& $x $d);" } else { '' }
        $extra += "      <PreprocessorDefinitions Condition=`"'`$(Configuration)'=='Release'`">${pre}NDEBUG</PreprocessorDefinitions>"
        $extra += "      <PreprocessorDefinitions Condition=`"'`$(Configuration)'=='Debug'`">${pre}_DEBUG</PreprocessorDefinitions>"
    }
    if ($u.NoWarn) { $extra += '      <WarningLevel>TurnOffAllWarnings</WarningLevel>'; $extra += '      <TreatWarningAsError>false</TreatWarningAsError>' }
    if ($u.IsC) { $extra += '      <CompileAs>CompileAsC</CompileAs>'; $extra += '      <LanguageStandard_C>stdc17</LanguageStandard_C>' }
    if ($u.BigObj) { $extra += '      <AdditionalOptions>/bigobj %(AdditionalOptions)</AdditionalOptions>' }
    if ($extra.Count) { [void]$cl.AppendLine("    <ClCompile Include=`"$(& $x $u.Rel)`">"); $extra | ForEach-Object { [void]$cl.AppendLine($_) }; [void]$cl.AppendLine('    </ClCompile>') }
    else { [void]$cl.AppendLine("    <ClCompile Include=`"$(& $x $u.Rel)`" />") }
}
Get-ChildItem (Join-Path $Source 'src') -Recurse -File -Include *.h,*.hpp,*.inc | Sort-Object FullName | ForEach-Object {
    [void]$hd.AppendLine("    <ClInclude Include=`"$(& $x (Rel $_.FullName))`" />")
}

$projGuid = '{6A1E4C2B-5D7F-4B8E-9C31-0F2A7D5E8B14}'
$incList = (($allIncs | ForEach-Object { "`$(ProjectDir)$_" }) -join ';')
$defList = (($commonDefs | ForEach-Object { & $x $_ }) -join ';')
$libs = 'd2d1.lib;dwrite.lib;dwmapi.lib;ole32.lib;oleaut32.lib;shell32.lib;shlwapi.lib;user32.lib;gdi32.lib;comdlg32.lib;d3d11.lib;dxgi.lib;d3dcompiler.lib;winmm.lib;bcrypt.lib;crypt32.lib;advapi32.lib;windowscodecs.lib;msimg32.lib;opengl32.lib;glu32.lib;comctl32.lib;version.lib;psapi.lib;dbghelp.lib'

$vcx = @"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>18.0</VCProjectVersion>
    <ProjectGuid>$projGuid</ProjectGuid>
    <RootNamespace>SroPk2Studio</RootNamespace>
    <Keyword>Win32Proj</Keyword>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <PlatformToolset>`$(DefaultPlatformToolset)</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'`$(Configuration)'=='Release'" Label="Configuration">
    <UseDebugLibraries>false</UseDebugLibraries>
    <WholeProgramOptimization>false</WholeProgramOptimization>
  </PropertyGroup>
  <PropertyGroup Condition="'`$(Configuration)'=='Debug'" Label="Configuration">
    <UseDebugLibraries>true</UseDebugLibraries>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <!-- bin\ holds only SroPk2Studio.exe (Debug: bin\Debug\); objects, pdb and logs stay in obj\. -->
  <PropertyGroup>
    <OutDir Condition="'`$(Configuration)'=='Release'">`$(SolutionDir)bin\</OutDir>
    <OutDir Condition="'`$(Configuration)'=='Debug'">`$(SolutionDir)bin\Debug\</OutDir>
    <IntDir>`$(SolutionDir)obj\`$(Configuration)\</IntDir>
    <TargetName>SroPk2Studio</TargetName>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpplatest</LanguageStandard>
      <ConformanceMode>true</ConformanceMode>
      <WarningLevel>Level4</WarningLevel>
      <TreatWarningAsError>true</TreatWarningAsError>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <ScanSourceForModuleDependencies>false</ScanSourceForModuleDependencies>
      <AdditionalOptions>/utf-8 /Zc:__cplusplus %(AdditionalOptions)</AdditionalOptions>
      <ExceptionHandling>Sync</ExceptionHandling>
      <!-- BAK and BAK-2 share file names (Pk2Archive.cpp, KeyFinder.cpp, Efp.cpp ...): one object folder per source folder. -->
      <ObjectFileName>`$(IntDir)%(RelativeDir)</ObjectFileName>
      <PreprocessorDefinitions>$defList;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <AdditionalIncludeDirectories>$incList;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Link>
      <SubSystem>Windows</SubSystem>
      <ProgramDatabaseFile>`$(IntDir)`$(TargetName).pdb</ProgramDatabaseFile>
      <ImportLibrary>`$(IntDir)`$(TargetName).lib</ImportLibrary>
      <AdditionalDependencies>$libs;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
    <Manifest>
      <AdditionalManifestFiles>src\app.manifest;%(AdditionalManifestFiles)</AdditionalManifestFiles>
    </Manifest>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'`$(Configuration)'=='Release'">
    <ClCompile>
      <Optimization>MaxSpeed</Optimization>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <PreprocessorDefinitions>NDEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <IntrinsicFunctions>true</IntrinsicFunctions>
    </ClCompile>
    <Link><OptimizeReferences>true</OptimizeReferences><EnableCOMDATFolding>true</EnableCOMDATFolding></Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'`$(Configuration)'=='Debug'">
    <ClCompile>
      <Optimization>Disabled</Optimization>
      <RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>
      <PreprocessorDefinitions>_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
$($cl.ToString().TrimEnd())
  </ItemGroup>
  <ItemGroup>
$($hd.ToString().TrimEnd())
  </ItemGroup>
  <ItemGroup>
    <None Include="src\app.manifest" />
  </ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@

# Filters: one folder per source directory.
$dirs = @{}; $fl = New-Object Text.StringBuilder
foreach ($kind in 'ClCompile', 'ClInclude') {
    $items = if ($kind -eq 'ClCompile') { $units.Values | ForEach-Object { $_.Rel } } else { Get-ChildItem (Join-Path $Source 'src') -Recurse -File -Include *.h,*.hpp,*.inc | ForEach-Object { Rel $_.FullName } }
    foreach ($i in ($items | Sort-Object)) {
        $d = Split-Path $i -Parent
        $p = $d; while ($p) { $dirs[$p] = 1; $p = Split-Path $p -Parent }
        [void]$fl.AppendLine("    <$kind Include=`"$(& $x $i)`"><Filter>$(& $x $d)</Filter></$kind>")
    }
}
$fx = New-Object Text.StringBuilder
foreach ($d in ($dirs.Keys | Sort-Object)) { [void]$fx.AppendLine("    <Filter Include=`"$(& $x $d)`"><UniqueIdentifier>{$([guid]::NewGuid())}</UniqueIdentifier></Filter>") }
$filters = @"
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
$($fx.ToString().TrimEnd())
  </ItemGroup>
  <ItemGroup>
$($fl.ToString().TrimEnd())
  </ItemGroup>
</Project>
"@

$sln = @"

Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 18
VisualStudioVersion = 18.0.0.0
MinimumVisualStudioVersion = 10.0.40219.1
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "SroPk2Studio", "SroPk2Studio.vcxproj", "$projGuid"
EndProject
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Release|x64 = Release|x64
		Debug|x64 = Debug|x64
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		$projGuid.Release|x64.ActiveCfg = Release|x64
		$projGuid.Release|x64.Build.0 = Release|x64
		$projGuid.Debug|x64.ActiveCfg = Debug|x64
		$projGuid.Debug|x64.Build.0 = Debug|x64
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
EndGlobal
"@

$enc = New-Object Text.UTF8Encoding($true)
[IO.File]::WriteAllText((Join-Path $Out 'SroPk2Studio.vcxproj'), $vcx, $enc)
[IO.File]::WriteAllText((Join-Path $Out 'SroPk2Studio.vcxproj.filters'), $filters, $enc)
[IO.File]::WriteAllText((Join-Path $Out 'SroPk2Studio.sln'), $sln, $enc)
"generated: $($units.Count) translation units, $($allIncs.Count) include dirs"
