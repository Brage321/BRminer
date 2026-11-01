[CmdletBinding()]
param(
	[string]$BuildDirectory = 'out\build\release-msvc'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
$cmake = Get-Command cmake -ErrorAction Stop
$ctest = Get-Command ctest -ErrorAction Stop

$versionMatch = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(BRminer VERSION ([^ )]+)')
if (-not $versionMatch.Success) {
	throw 'Could not determine BRminer version from CMakeLists.txt.'
}
$version = $versionMatch.Groups[1].Value

$configure = @('-S', $root, '-B', $buildPath, '-G', 'Visual Studio 18 2026', '-A', 'x64')
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot -and (Test-Path 'C:\vcpkg\scripts\buildsystems\vcpkg.cmake')) {
	$vcpkgRoot = 'C:\vcpkg'
}
if ($vcpkgRoot) {
	$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
	if (-not (Test-Path $toolchain)) {
		throw "VCPKG_ROOT does not contain the CMake toolchain: $toolchain"
	}
	$configure += @(('-DCMAKE_TOOLCHAIN_FILE=' + $toolchain), '-DVCPKG_TARGET_TRIPLET=x64-windows')
}

& $cmake.Source @configure
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }
& $cmake.Source --build $buildPath --config Release --target brminer config-tests pow-tests warthog-pow-tests warthog-gpu-tests
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }
& $ctest.Source --test-dir $buildPath -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Release tests failed with exit code $LASTEXITCODE." }

$executable = Join-Path $root 'brminer.exe'
if (-not (Test-Path $executable)) { throw "Expected Release executable was not produced: $executable" }
$coreCheck = (& $executable --embedded-core-check | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $coreCheck -ne 'BRMINER_EMBEDDED_CORE=7') {
	throw 'The executable did not pass the embedded-Core release check.'
}
& $executable --pow-self-test
if ($LASTEXITCODE -ne 0) { throw 'The RandomX v2 known-answer self-test failed.' }

$deps = Join-Path $buildPath '_deps'
$coreSource = Join-Path $deps 'connectcoin_source-src'
$janusSource = Join-Path $deps 'janusminer_source-src'
$mbedtlsSource = Join-Path $deps 'connectcoin_mbedtls-src'
$requiredLicenses = @(
	@{ Source = (Join-Path $coreSource 'COPYING'); Name = 'ConnectCoin-COPYING.txt' },
	@{ Source = (Join-Path $coreSource 'src\crypto\ctaes\COPYING'); Name = 'ctaes-COPYING.txt' },
	@{ Source = (Join-Path $coreSource 'src\crc32c\LICENSE'); Name = 'crc32c-LICENSE.txt' },
	@{ Source = (Join-Path $coreSource 'src\leveldb\LICENSE'); Name = 'LevelDB-LICENSE.txt' },
	@{ Source = (Join-Path $coreSource 'src\minisketch\LICENSE'); Name = 'minisketch-LICENSE.txt' },
	@{ Source = (Join-Path $coreSource 'src\randomx\LICENSE'); Name = 'RandomX-LICENSE.txt' },
	@{ Source = (Join-Path $coreSource 'src\secp256k1\COPYING'); Name = 'secp256k1-COPYING.txt' },
	@{ Source = (Join-Path $mbedtlsSource 'LICENSE'); Name = 'mbedTLS-LICENSE.txt' },
	@{ Source = (Join-Path $janusSource 'LICENSE'); Name = 'JanusMiner-LICENSE.txt' },
	@{ Source = (Join-Path $root 'licenses\JANUSMINER-LICENSES.txt'); Name = 'JanusMiner-and-VerusHash-notices.txt' }
)
foreach ($license in $requiredLicenses) {
	if (-not (Test-Path $license.Source)) {
		throw "Required upstream license/notice was not found: $($license.Source)"
	}
}
$connectcoinLauncher = Get-Content (Join-Path $root 'mining-connectcoin.bat') -Raw
$warthogLauncher = Get-Content (Join-Path $root 'mining-warthog.bat') -Raw
if ($connectcoinLauncher -notmatch 'set "PAYOUT_ADDRESS=ENTER-YOUR-CONNECTCOIN-WALLET"' -or
	$warthogLauncher -notmatch 'set "PAYOUT_ADDRESS=ENTER-YOUR-WARTHOG-WALLET"') {
	throw 'Release launchers must contain only the payout-address placeholders.'
}

$releaseName = "BRminer-$version-windows-x64"
$releaseDirectory = Join-Path $root 'out\release'
New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
$archivePath = Join-Path $releaseDirectory ($releaseName + '.zip')
$checksumPath = Join-Path $releaseDirectory ($releaseName + '.zip.sha256')
if (Test-Path $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
if (Test-Path $checksumPath) { Remove-Item -LiteralPath $checksumPath -Force }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($archivePath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
	$prefix = $releaseName + '/'
	$packageFiles = @(
		@{ Source = $executable; Name = 'brminer.exe' },
		@{ Source = (Join-Path $root 'mining-connectcoin.bat'); Name = 'mining-connectcoin.bat' },
		@{ Source = (Join-Path $root 'mining-warthog.bat'); Name = 'mining-warthog.bat' },
		@{ Source = (Join-Path $root 'README.md'); Name = 'README.md' }
	)
	foreach ($file in $packageFiles) {
		if (-not (Test-Path $file.Source)) { throw "Release file is missing: $($file.Source)" }
		$entry = $archive.CreateEntry($prefix + $file.Name, [System.IO.Compression.CompressionLevel]::Optimal)
		$input = [System.IO.File]::OpenRead($file.Source)
		$output = $entry.Open()
		try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
	}
	foreach ($license in $requiredLicenses) {
		$entry = $archive.CreateEntry($prefix + 'licenses/' + $license.Name, [System.IO.Compression.CompressionLevel]::Optimal)
		$input = [System.IO.File]::OpenRead($license.Source)
		$output = $entry.Open()
		try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
	}
	$boostDirEntry = Select-String -Path (Join-Path $buildPath 'CMakeCache.txt') -Pattern '^Boost_DIR:PATH=(.+)$' | Select-Object -First 1
	if ($boostDirEntry) {
		$boostLicense = Join-Path (Split-Path $boostDirEntry.Matches[0].Groups[1].Value -Parent) 'boost-headers\copyright'
		if (Test-Path $boostLicense) {
			$entry = $archive.CreateEntry($prefix + 'licenses/Boost-Software-License.txt', [System.IO.Compression.CompressionLevel]::Optimal)
			$input = [System.IO.File]::OpenRead($boostLicense)
			$output = $entry.Open()
			try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
		}
	}
} finally {
	$archive.Dispose()
}

$archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
	$entryNames = @($archive.Entries | ForEach-Object { $_.FullName })
	$requiredEntries = @(
		"$releaseName/brminer.exe",
		"$releaseName/mining-connectcoin.bat",
		"$releaseName/mining-warthog.bat",
		"$releaseName/README.md"
	)
	foreach ($license in $requiredLicenses) {
		$requiredEntries += "$releaseName/licenses/$($license.Name)"
	}
	$missingEntries = @($requiredEntries | Where-Object { $_ -notin $entryNames })
	if ($missingEntries.Count -gt 0) {
		throw "Release ZIP is missing required entries: $($missingEntries -join ', ')"
	}
	if (@($entryNames | Where-Object { $_ -match '\.(pdb|ilk|exp|lib)$' }).Count -gt 0) {
		throw 'Release ZIP unexpectedly contains debug or linker artifacts.'
	}
} finally {
	$archive.Dispose()
}

$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText($checksumPath, "$hash  $([System.IO.Path]::GetFileName($archivePath))`r`n", [System.Text.UTF8Encoding]::new($false))
Write-Output "Release archive: $archivePath"
Write-Output "SHA-256: $hash"
