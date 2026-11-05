# PowerShell helper to build Moonlight DX PS4 using Docker

$ImageName = "moonlight-ps4-builder"

# Build builder image if needed
$img = docker images -q $ImageName
if (-not $img) {
    Write-Host "Building Docker build image: $ImageName..."
    docker build -t $ImageName -f Dockerfile.builder .
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to build Docker builder image."
        exit $LASTEXITCODE
    }
}

# Run make inside Docker container
Write-Host "Compiling Moonlight DX PS4 inside Docker..."
docker run --rm -v "${PWD}:/workspace" -w /workspace $ImageName make all
