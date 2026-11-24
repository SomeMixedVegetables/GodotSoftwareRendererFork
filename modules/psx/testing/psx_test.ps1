<#
.SYNOPSIS
  Build, run and check the PSX renderer without manual steps.

.DESCRIPTION
  Commands:
    build    Build the editor (incremental). -Dev builds a dev_build with
             debug symbols into a separate binary, for readable crash traces.
    unit     Compile and run the psx_core unit tests (MSVC, no Godot build).
    run      Run a test scene and capture output. -Mode on|off|both.
    check    Run with PSX on and compare against golden images.
    bless    Run with PSX on and save the output as the new golden images.
    review   Run PSX on and off and build one side-by-side sheet to look at.
    all      build, unit, then check every scene.

  Godot's --headless mode uses a dummy renderer, so runs that need the PSX
  path open a short-lived window that closes by itself (Movie Maker mode,
  fixed fps, quits after -Frames frames). Image comparison runs headless.

  Output goes to bin\psx_test_out\<scene>\<mode>\ (git-ignored):
    log.txt, stderr.txt   Godot output
    movie\frameNNNNNNNN.png   Composited window frames (3D + 2D)
    psx\psx_NNNN.png          Raw PSX framebuffer at native resolution
    summary.json              Exit code, crash flag, errors, PSX_ lines

  Every result line starts with "PSX_" so it is easy to grep.

  Per-scene settings: scenes\<name>.override.cfg, if present, adds lines to
  the [rendering] section of override.cfg for that scene's runs.

  External models: -ModelsDir copies a fixed selection of .glb files
  (-ModelsPerCategory from each top-level folder, by sorted name) into
  project\external_models and re-imports the project. Scenes named ext_*
  need them and are skipped when the folder is empty. Models, their import
  files and their golden images (golden_external\) are all git-ignored:
  third-party models must never be committed.

.EXAMPLE
  .\modules\psx\testing\psx_test.ps1 all
  .\modules\psx\testing\psx_test.ps1 review -Scene basic
  .\modules\psx\testing\psx_test.ps1 run -Mode both -Stats
#>
[CmdletBinding()]
param(
	[Parameter(Position = 0)]
	[ValidateSet('build', 'unit', 'run', 'check', 'bless', 'review', 'all')]
	[string]$Command = 'all',
	[string]$Scene = '',
	[ValidateSet('on', 'off', 'both')]
	[string]$Mode = 'on',
	[int]$Frames = 40,
	[int]$GoldenFrame = 30,   # must be below -Frames
	[int]$Tolerance = 0,
	[int]$CompositeTolerance = 2,
	[switch]$Stats,
	[switch]$Dev,
	[string]$ModelsDir = '',
	[int]$ModelsPerCategory = 2
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Project = Join-Path $PSScriptRoot 'project'
$GoldenRoot = Join-Path $PSScriptRoot 'golden'
$GoldenExternalRoot = Join-Path $PSScriptRoot 'golden_external'
$ExternalModels = Join-Path $Project 'external_models'
$OutRoot = Join-Path $Repo 'bin\psx_test_out'
$ExeName = if ($Dev) { 'godot.windows.editor.dev.x86_64.console.exe' } else { 'godot.windows.editor.x86_64.console.exe' }
$Exe = Join-Path $Repo "bin\$ExeName"

function Write-Result([string]$Line) { Write-Host $Line }

function Test-ExternalModels {
	return (Test-Path $ExternalModels) -and @(Get-ChildItem $ExternalModels -Recurse -Filter '*.glb').Count -gt 0
}

function Get-Scenes {
	if ($Scene) { return @($Scene) }
	$all = @(Get-ChildItem (Join-Path $Project 'scenes') -Filter '*.tscn' | ForEach-Object { $_.BaseName })
	if (Test-ExternalModels) { return $all }
	$skipped = @($all | Where-Object { $_ -like 'ext_*' })
	foreach ($s in $skipped) { Write-Result "PSX_SKIP scene=$s reason=no_external_models (use -ModelsDir)" }
	return @($all | Where-Object { $_ -notlike 'ext_*' })
}

function Get-GoldenDir([string]$SceneName) {
	$root = if ($SceneName -like 'ext_*') { $GoldenExternalRoot } else { $GoldenRoot }
	return Join-Path $root $SceneName
}

# Copies a deterministic selection of models into the (git-ignored) test project.
function Copy-ExternalModels {
	if (-not $ModelsDir) { return $false }
	if (-not (Test-Path $ModelsDir)) { throw "ModelsDir not found: $ModelsDir" }
	if (Test-Path $ExternalModels) { Remove-Item -Recurse -Force $ExternalModels }
	New-Item -ItemType Directory -Force $ExternalModels | Out-Null
	$count = 0
	foreach ($cat in @(Get-ChildItem $ModelsDir -Directory | Sort-Object Name)) {
		$picked = @(Get-ChildItem $cat.FullName -Recurse -Filter '*.glb' | Sort-Object Name | Select-Object -First $ModelsPerCategory)
		foreach ($f in $picked) {
			Copy-Item $f.FullName (Join-Path $ExternalModels ($cat.Name + '_' + $f.Name))
			$count++
		}
	}
	Write-Result "PSX_MODELS copied=$count per_category=$ModelsPerCategory"
	return $count -gt 0
}

function Quote([string]$s) { if ($s -match '[\s"]') { '"' + ($s -replace '"', '\"') + '"' } else { $s } }

# Runs Godot with an argument list; returns exit code, stdout and stderr.
function Invoke-Godot([string[]]$GodotArgs, [string]$LogPath, [string]$ErrPath) {
	if (-not (Test-Path $Exe)) { throw "Godot binary not found: $Exe (run 'build' first)" }
	$argLine = ($GodotArgs | ForEach-Object { Quote $_ }) -join ' '
	$p = Start-Process -FilePath $Exe -ArgumentList $argLine -NoNewWindow -Wait -PassThru `
		-RedirectStandardOutput $LogPath -RedirectStandardError $ErrPath
	return [pscustomobject]@{
		Code = $p.ExitCode
		Out  = (Get-Content $LogPath -Raw -ErrorAction SilentlyContinue) + ''
		Err  = (Get-Content $ErrPath -Raw -ErrorAction SilentlyContinue) + ''
	}
}

function Invoke-Build {
	Push-Location $Repo
	try {
		$sconsArgs = @('platform=windows', 'target=editor', 'd3d12=no', 'accesskit=no', "-j$env:NUMBER_OF_PROCESSORS")
		if ($Dev) { $sconsArgs += 'dev_build=yes' }
		$log = Join-Path $OutRoot 'build.log'
		New-Item -ItemType Directory -Force $OutRoot | Out-Null
		Write-Result "PSX_BUILD start dev=$([bool]$Dev) log=$log"
		$start = Get-Date
		# Through cmd: Windows PowerShell 5.1 turns a native tool's stderr into
		# terminating errors when ErrorActionPreference is Stop.
		cmd /c "scons $($sconsArgs -join ' ') > `"$log`" 2>&1"
		$code = $LASTEXITCODE
		$secs = [int]((Get-Date) - $start).TotalSeconds
		$errors = @(Select-String -Path $log -Pattern 'error C\d+|fatal error|LNK\d+' | Select-Object -First 20 | ForEach-Object { $_.Line.Trim() })
		foreach ($e in $errors) { Write-Result "PSX_BUILD_ERROR $e" }
		Write-Result "PSX_BUILD result=$(if ($code -eq 0) {'pass'} else {'fail'}) seconds=$secs"
		return $code -eq 0
	} finally { Pop-Location }
}

function Invoke-Unit {
	$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
	$out = Join-Path $OutRoot 'unit'
	New-Item -ItemType Directory -Force $out | Out-Null
	$core = Join-Path $PSScriptRoot '..\psx_core'
	$sources = @((Join-Path $PSScriptRoot 'unit\test_psx_core.cpp')) + @(Get-ChildItem $core -Filter *.cpp | ForEach-Object FullName)
	$srcArgs = ($sources | ForEach-Object { "`"$_`"" }) -join ' '
	$bin = Join-Path $out 'test_psx_core.exe'
	$compile = "call `"$vcvars`" >nul 2>nul && cl /nologo /EHsc /std:c++17 /O2 /W4 /WX $srcArgs /Fo`"$out\\`" /Fe`"$bin`""
	$compileOut = cmd /c $compile 2>&1 | Out-String
	if ($LASTEXITCODE -ne 0) {
		Write-Host $compileOut
		Write-Result 'PSX_UNIT result=fail reason=compile'
		return $false
	}
	# Also saves the core demo frames (no Godot, no window) for review.
	$demoDir = Join-Path $out 'demo'
	New-Item -ItemType Directory -Force $demoDir | Out-Null
	& $bin --dump $demoDir | ForEach-Object { Write-Host $_ }
	$ok = $LASTEXITCODE -eq 0
	Write-Result "PSX_UNIT result=$(if ($ok) {'pass'} else {'fail'})"
	return $ok
}

function Set-Override([bool]$Enabled, [string]$DumpDir, [string]$SceneName = '') {
	$lines = @('[rendering]', '', "psx/enabled=$($Enabled.ToString().ToLower())")
	if ($Enabled) {
		$lines += "psx/debug/dump_dir=`"$($DumpDir -replace '\\', '/')`""
		$lines += "psx/debug/dump_frames=$Frames"
		$lines += "psx/debug/print_stats=$($Stats.IsPresent.ToString().ToLower())"
		$extra = Join-Path $Project "scenes\$SceneName.override.cfg"
		if ($SceneName -and (Test-Path $extra)) {
			$lines += @(Get-Content $extra | Where-Object { $_ -match '\S' -and $_ -notmatch '^\s*[;#\[]' })
		}
	}
	[IO.File]::WriteAllText((Join-Path $Project 'override.cfg'), ($lines -join "`n") + "`n")
}

function Initialize-Project([bool]$Force = $false) {
	if (-not $Force -and (Test-Path (Join-Path $Project '.godot'))) { return }
	New-Item -ItemType Directory -Force $OutRoot | Out-Null
	$r = Invoke-Godot @('--headless', '--path', $Project, '--import') (Join-Path $OutRoot 'import.log') (Join-Path $OutRoot 'import.err')
	# On 4.8 beta, --import finishes importing and then exits with an access
	# violation from EditorNode::is_cmdline_mode (Godot editor code, not the PSX
	# module). Judge it by whether the import produced .godot.
	$imported = Test-Path (Join-Path $Project '.godot')
	Write-Result "PSX_IMPORT result=$(if ($imported) {'pass'} else {'fail'}) godot_exit=$($r.Code)"
	if (-not $imported) { throw "Project import failed; see $OutRoot\import.err" }
}

# One captured run of one scene in one mode. Returns a summary object.
function Invoke-Run([string]$SceneName, [string]$RunMode) {
	$out = Join-Path $OutRoot "$SceneName\$RunMode"
	if (Test-Path $out) { Remove-Item -Recurse -Force $out }
	$movieDir = Join-Path $out 'movie'
	$psxDir = Join-Path $out 'psx'
	New-Item -ItemType Directory -Force $movieDir, $psxDir | Out-Null

	Set-Override ($RunMode -eq 'on') $psxDir $SceneName
	$godotArgs = @('--path', $Project, '--write-movie', (Join-Path $movieDir 'frame.png'),
		'--fixed-fps', '30', '--quit-after', "$Frames", "res://scenes/$SceneName.tscn")
	$r = Invoke-Godot $godotArgs (Join-Path $out 'log.txt') (Join-Path $out 'stderr.txt')
	Set-Override $false ''

	$all = $r.Out + "`n" + $r.Err
	$crashed = $all -match 'CrashHandlerException|Program crashed'
	$errors = @($all -split "`r?`n" | Where-Object { $_ -match '^\s*(ERROR|SCRIPT ERROR|USER ERROR):' } | ForEach-Object { $_.Trim() })
	$warnings = @($all -split "`r?`n" | Where-Object { $_ -match '^\s*WARNING:' } | ForEach-Object { $_.Trim() })
	$psxLines = @($all -split "`r?`n" | Where-Object { $_ -match '^PSX' })
	$movie = @(Get-ChildItem $movieDir -Filter '*.png' | Sort-Object Name)
	$dumps = @(Get-ChildItem $psxDir -Filter '*.png' | Sort-Object Name)
	$active = [bool]($psxLines -match 'compositor active')

	$summary = [ordered]@{
		scene = $SceneName; mode = $RunMode; exit_code = $r.Code; crashed = $crashed
		psx_active = $active; movie_frames = $movie.Count; psx_dumps = $dumps.Count
		errors = $errors; warnings = $warnings; psx_lines = $psxLines
		last_movie_frame = $(if ($movie.Count) { $movie[-1].FullName } else { '' })
		out_dir = $out
	}
	$summary | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $out 'summary.json')

	$ok = ($r.Code -eq 0) -and -not $crashed -and ($movie.Count -gt 0) -and ($active -eq ($RunMode -eq 'on'))
	Write-Result ("PSX_RUN scene=$SceneName mode=$RunMode result=$(if ($ok) {'pass'} else {'fail'}) exit=$($r.Code) " +
		"crashed=$crashed psx_active=$active movie_frames=$($movie.Count) psx_dumps=$($dumps.Count) errors=$($errors.Count) warnings=$($warnings.Count)")
	foreach ($e in $errors | Select-Object -First 10) { Write-Result "PSX_RUN_ERROR $e" }
	if ($crashed) {
		$trace = ($all -split "`r?`n" | Select-String -Pattern 'CrashHandlerException' -Context 0, 20 | Out-String)
		Write-Host $trace
		if (-not $Dev) { Write-Result 'PSX_HINT crash trace has no symbols; rebuild with -Dev and rerun with -Dev for function names' }
	}
	Write-Result "PSX_OUT $out"
	return [pscustomobject]$summary
}

function Invoke-Compare([string]$Expected, [string]$Actual, [string]$DiffOut, [int]$Tol) {
	$tmp = Join-Path $OutRoot 'tool'
	New-Item -ItemType Directory -Force $tmp | Out-Null
	$r = Invoke-Godot @('--headless', '--path', $Project, '--script', 'res://tools/image_tool.gd', '--', 'compare', $Expected, $Actual, $DiffOut, "$Tol") `
		(Join-Path $tmp 'compare.log') (Join-Path $tmp 'compare.err')
	$line = ($r.Out -split "`r?`n" | Where-Object { $_ -match '^PSX_COMPARE' } | Select-Object -First 1)
	if (-not $line) { $line = "PSX_COMPARE result=error code=$($r.Code) $($r.Err.Trim())" }
	return [pscustomobject]@{ Pass = ($r.Code -eq 0); Line = $line }
}

function Invoke-Sheet([string]$OutPath, [string[]]$Images) {
	$tmp = Join-Path $OutRoot 'tool'
	New-Item -ItemType Directory -Force $tmp | Out-Null
	$r = Invoke-Godot (@('--headless', '--path', $Project, '--script', 'res://tools/image_tool.gd', '--', 'sheet', $OutPath) + $Images) `
		(Join-Path $tmp 'sheet.log') (Join-Path $tmp 'sheet.err')
	return $r.Code -eq 0
}

function Get-RunImages($Summary) {
	# Same frame number for both, so the raw PSX frame and the composited
	# window frame show the same moment.
	$raw = Join-Path $Summary.out_dir ('psx\psx_{0:D4}.png' -f $GoldenFrame)
	$composite = Join-Path $Summary.out_dir ('movie\frame{0:D8}.png' -f $GoldenFrame)
	return [pscustomobject]@{ Raw = $raw; Composite = $composite }
}

function Invoke-Check([string]$SceneName) {
	$s = Invoke-Run $SceneName 'on'
	$golden = Get-GoldenDir $SceneName
	$img = Get-RunImages $s
	$results = @()
	$pairs = @(
		@{ Name = 'raw'; Expected = (Join-Path $golden ('psx_{0:D4}.png' -f $GoldenFrame)); Actual = $img.Raw; Tol = $Tolerance },
		@{ Name = 'composite'; Expected = (Join-Path $golden 'composite.png'); Actual = $img.Composite; Tol = $CompositeTolerance }
	)
	foreach ($p in $pairs) {
		if (-not (Test-Path $p.Expected)) { Write-Result "PSX_CHECK scene=$SceneName image=$($p.Name) result=missing_golden (run 'bless')"; $results += $false; continue }
		if (-not $p.Actual -or -not (Test-Path $p.Actual)) { Write-Result "PSX_CHECK scene=$SceneName image=$($p.Name) result=fail reason=no_output"; $results += $false; continue }
		$diff = Join-Path $s.out_dir "diff_$($p.Name).png"
		$c = Invoke-Compare $p.Expected $p.Actual $diff $p.Tol
		Write-Result "PSX_CHECK scene=$SceneName image=$($p.Name) $($c.Line -replace '^PSX_COMPARE ', '')"
		if (-not $c.Pass -and (Test-Path $diff)) { Write-Result "PSX_DIFF $diff" }
		$results += $c.Pass
	}
	return (-not ($results -contains $false)) -and ($s.exit_code -eq 0) -and -not $s.crashed
}

function Invoke-Bless([string]$SceneName) {
	$s = Invoke-Run $SceneName 'on'
	$img = Get-RunImages $s
	if (-not (Test-Path $img.Raw) -or -not (Test-Path $img.Composite)) { Write-Result "PSX_BLESS scene=$SceneName result=fail reason=no_output"; return $false }
	$golden = Get-GoldenDir $SceneName
	New-Item -ItemType Directory -Force $golden | Out-Null
	Copy-Item $img.Raw (Join-Path $golden ('psx_{0:D4}.png' -f $GoldenFrame)) -Force
	Copy-Item $img.Composite (Join-Path $golden 'composite.png') -Force
	Write-Result "PSX_BLESS scene=$SceneName result=pass golden=$golden"
	return $true
}

function Invoke-Review([string]$SceneName) {
	$on = Invoke-Run $SceneName 'on'
	$off = Invoke-Run $SceneName 'off'
	$img = Get-RunImages $on
	$offFrame = Join-Path $off.out_dir ('movie\frame{0:D8}.png' -f $GoldenFrame)
	$images = @($img.Raw, $img.Composite, $offFrame) | Where-Object { $_ -and (Test-Path $_) }
	$sheet = Join-Path $OutRoot "$SceneName\review.png"
	$ok = Invoke-Sheet $sheet $images
	Write-Result "PSX_REVIEW scene=$SceneName result=$(if ($ok) {'pass'} else {'fail'}) sheet=$sheet (left to right: raw PSX frame $GoldenFrame, PSX composite, Godot with PSX off)"
	return $ok
}

# --- main ---
New-Item -ItemType Directory -Force $OutRoot | Out-Null
$ok = $true
if ($ModelsDir -and $Command -ne 'build' -and $Command -ne 'unit') {
	if (Copy-ExternalModels) { Initialize-Project $true }
}
switch ($Command) {
	'build' { $ok = Invoke-Build }
	'unit' { $ok = Invoke-Unit }
	'run' {
		Initialize-Project
		$modes = if ($Mode -eq 'both') { @('on', 'off') } else { @($Mode) }
		foreach ($sc in Get-Scenes) { foreach ($m in $modes) { $s = Invoke-Run $sc $m; $ok = $ok -and ($s.exit_code -eq 0) -and -not $s.crashed } }
	}
	'check' { Initialize-Project; foreach ($sc in Get-Scenes) { $ok = (Invoke-Check $sc) -and $ok } }
	'bless' { Initialize-Project; foreach ($sc in Get-Scenes) { $ok = (Invoke-Bless $sc) -and $ok } }
	'review' { Initialize-Project; foreach ($sc in Get-Scenes) { $ok = (Invoke-Review $sc) -and $ok } }
	'all' {
		$ok = Invoke-Build
		if ($ok) { $ok = Invoke-Unit }
		if ($ok) { Initialize-Project; foreach ($sc in Get-Scenes) { $ok = (Invoke-Check $sc) -and $ok } }
	}
}
Write-Result "PSX_DONE command=$Command result=$(if ($ok) {'pass'} else {'fail'})"
exit $(if ($ok) { 0 } else { 1 })
