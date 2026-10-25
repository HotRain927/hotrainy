## NOTE: paths in this script are absolute (C:\hot_rain\...).
## Adjust them for your machine - see docs/发布流程.md (Release process).
# publish.ps1 — 一键把 hotrainy 发布到 GitHub（建仓库 + 推送 + 发 Release + 传安装包）
#
# 用法（双击同目录的 发布到GitHub.cmd 即可，或手动）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File publish.ps1
#
# 凭据来源：GitHub Desktop 登录后存在 Windows 凭据管理器里的 GitHub token
#          （target: git:https://github.com 或 GitHub - https://api.github.com/<user>）
# 本脚本不会打印 token，也不会把它写进任何文件。

$ErrorActionPreference = "Stop"
$RepoDir = "C:\hot_rain\cpp\hotrainy"
$AssetZip = "C:\hot_rain\hotrainy_setup.zip"
$AssetExe = "C:\hot_rain\hotrainy_installer.exe"
$RelNotesFile = Join-Path $RepoDir "docs\releases\v1.0.md"
$RepoName = "hotrainy"
$RepoDesc = "Windows 11 任务栏真透明 + 岛台面板 + 顶部标签栏 + 音频波动条（C++ / Win32，免管理员免运行库）"
$Tag = "v1.0"
$RelTitle = "hotrainy v1.0 — 第一版 / first release"

function Step($n, $t) { Write-Host "`n[$n] $t" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "    OK  $m" -ForegroundColor Green }
function Warn($m) { Write-Host "    !!  $m" -ForegroundColor Yellow }

# ── 读凭据管理器里的 GitHub token ──────────────────────────────────
function Get-GitHubToken {
    Add-Type -Namespace Win32 -Name Cred -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
public struct CREDENTIAL {
    public uint Flags; public uint Type; public string TargetName; public string Comment;
    public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
    public uint CredentialBlobSize; public IntPtr CredentialBlob;
    public uint Persist; public uint AttributeCount; public IntPtr Attributes;
    public string TargetAlias; public string UserName;
}
[DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
public static extern bool CredRead(string target, uint type, uint flags, out IntPtr credential);
[DllImport("advapi32.dll")] public static extern void CredFree(IntPtr cred);
'@ -ErrorAction SilentlyContinue

    $targets = New-Object System.Collections.Generic.List[string]
    $targets.Add("git:https://github.com")
    $out = & cmdkey /list 2>$null | Select-String "Target:" | ForEach-Object { $_.Line }
    foreach ($line in $out) {
        if ($line -match "target=(.+)$") {
            $t = $Matches[1]
            if ($t -match "github") { $targets.Add($t) }
        }
    }
    foreach ($t in $targets) {
        $ptr = [IntPtr]::Zero
        if ([Win32.Cred]::CredRead($t, 1, 0, [ref]$ptr)) {
            try {
                $c = [System.Runtime.InteropServices.Marshal]::PtrToStructure($ptr, [type][Win32.Cred+CREDENTIAL])
                if ($c.CredentialBlobSize -gt 0) {
                    $bytes = New-Object byte[] $c.CredentialBlobSize
                    [System.Runtime.InteropServices.Marshal]::Copy($c.CredentialBlob, $bytes, 0, $bytes.Length)
                    $cands = @()
                    foreach ($e in @([System.Text.Encoding]::Unicode, [System.Text.Encoding]::UTF8)) {
                        try { $cands += $e.GetString($bytes).Trim([char]0) } catch {}
                    }
                    foreach ($s0 in $cands) {
                        $s = $s0
                        if ($s -match "(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})") { return $Matches[1] }
                        if ($s -match '"accessToken"\s*:\s*"([^"]+)"') { return $Matches[1] }
                        if ($s -match "oauth_token=([^&]+)") { return $Matches[1] }
                        if ($s -match ":") { $s = ($s -split ":")[-1] }
                        if ($s.Length -gt 20 -and $s -notmatch "\s") { return $s.Trim() }
                    }
                }
            } finally { [Win32.Cred]::CredFree($ptr) }
        }
    }
    return $null
}

Step 1 "读取 GitHub 凭据（来自 GitHub Desktop 的登录）"
$token = Get-GitHubToken
if (-not $token) {
    Warn "没读到 token。请先在 GitHub Desktop 里登录，或改用 gh CLI 登录后重跑。"
    exit 1
}
Ok "拿到 token（长度 $($token.Length)，内容不显示）"
$headers = @{ Authorization = "Bearer $token"; Accept = "application/vnd.github+json"; "User-Agent" = "hotrainy-publish" }

Step 2 "确认账号"
$me = Invoke-RestMethod -Uri "https://api.github.com/user" -Headers $headers -Method Get
$owner = $me.login
Ok "账号: $owner"

Step 3 "创建仓库 $owner/$RepoName"
$repoExists = $true
try { $null = Invoke-RestMethod -Uri "https://api.github.com/repos/$owner/$RepoName" -Headers $headers -Method Get }
catch { $repoExists = $false }
if ($repoExists) {
    Ok "仓库已存在，跳过创建"
} else {
    $body = @{ name = $RepoName; description = $RepoDesc; private = $false; has_issues = $true; has_wiki = $false } | ConvertTo-Json
    $null = Invoke-RestMethod -Uri "https://api.github.com/user/repos" -Headers $headers -Method Post -Body ([Text.Encoding]::UTF8.GetBytes($body)) -ContentType "application/json; charset=utf-8"
    Ok "仓库已创建: https://github.com/$owner/$RepoName"
}

Step 4 "推送源码"
Push-Location $RepoDir
$branch = (& git rev-parse --abbrev-ref HEAD).Trim()
if ($branch -ne "main") { & git branch -M main 2>&1 | Out-Null; $branch = "main" }
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$remotes = @(& git remote)
if ($remotes -contains "origin") { & git remote remove origin | Out-Null }
& git remote add origin "https://github.com/$owner/$RepoName.git"
$basic = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("x-access-token:$token"))
& git -c "http.extraheader=Authorization: Basic $basic" push -u origin main --force 2>&1 | ForEach-Object { Write-Host "    $_" }
if ($LASTEXITCODE -ne 0) { Warn "git push 返回 $LASTEXITCODE" } else { Ok "git push 成功" }
$ErrorActionPreference = $prevEAP
Pop-Location
Ok "已推送到 origin/main"

Step 5 "创建 Release $Tag"
$relBody = Get-Content $RelNotesFile -Raw -Encoding UTF8
# 去掉文件开头的归档说明，从 "# 中文" 开始作为发布正文
$idx = $relBody.IndexOf("# 中文")
if ($idx -gt 0) { $relBody = $relBody.Substring($idx) }
$relPayload = @{ tag_name = $Tag; name = $RelTitle; body = $relBody; draft = $false; prerelease = $false } | ConvertTo-Json -Depth 4
$rel = $null
try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$owner/$RepoName/releases/tags/$Tag" -Headers $headers -Method Get
    Warn "Release $Tag 已存在，将复用并覆盖附件"
} catch {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$owner/$RepoName/releases" -Headers $headers -Method Post -Body ([Text.Encoding]::UTF8.GetBytes($relPayload)) -ContentType "application/json; charset=utf-8"
    Ok "Release 已创建"
}

Step 6 "上传安装包"
foreach ($pair in @(@{ f = $AssetZip; n = "hotrainy_setup.zip" }, @{ f = $AssetExe; n = "hotrainy_installer.exe" })) {
    if (-not (Test-Path $pair.f)) { Warn "跳过（不存在）: $($pair.f)"; continue }
    # 已存在同名附件则先删掉
    foreach ($a in $rel.assets) { if ($a.name -eq $pair.n) { try { Invoke-RestMethod -Uri $a.url -Headers $headers -Method Delete | Out-Null } catch {} } }
    $up = "https://uploads.github.com/repos/$owner/$RepoName/releases/$($rel.id)/assets?name=$($pair.n)"
    $null = Invoke-RestMethod -Uri $up -Headers $headers -Method Post -InFile $pair.f -ContentType "application/octet-stream"
    Ok "已上传 $($pair.n)  ($([math]::Round((Get-Item $pair.f).Length/1KB,1)) KB)"
}

Write-Host "`n============================================" -ForegroundColor Green
Write-Host " 完成！" -ForegroundColor Green
Write-Host " 仓库   : https://github.com/$owner/$RepoName" -ForegroundColor Green
Write-Host " Release: https://github.com/$owner/$RepoName/releases/tag/$Tag" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green