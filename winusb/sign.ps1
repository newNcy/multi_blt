$ErrorActionPreference = "Stop"

$root     = "d:\dual blt\winusb"
$pkg      = Join-Path $root "pkg"
$signtool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.17763.0\x64\signtool.exe"
$pfx      = Join-Path $root "cert.pfx"
$pwdPlain = "DualBlt123"

# 1) 自签代码签名证书
$cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq "CN=DualBlt Test Signing" } |
        Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigning `
        -Subject "CN=DualBlt Test Signing" `
        -CertStoreLocation "Cert:\LocalMachine\My" `
        -KeyExportPolicy Exportable `
        -NotAfter ((Get-Date).AddYears(5)) `
        -HashAlgorithm SHA256
}
"thumbprint = " + $cert.Thumbprint

# 2) 导入到 受信任的根 与 受信任的发布者
foreach ($name in @("Root", "TrustedPublisher")) {
    $st = New-Object System.Security.Cryptography.X509Certificates.X509Store($name, "LocalMachine")
    $st.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $st.Add($cert)
    $st.Close()
    "imported into LocalMachine\$name"
}

# 3) 导出 PFX
$pwd = ConvertTo-SecureString -String $pwdPlain -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pwd | Out-Null
"pfx exported"

# 4) 生成目录文件（此时 pkg 里只有 inf，目录不含自身）
$cat = Join-Path $root "DualBltWinUSB.cat"
if (Test-Path $cat) { Remove-Item $cat -Force }
New-FileCatalog -Path $pkg -CatalogFilePath $cat | Out-Null
"catalog created"

# 5) 签名（必须先签，再复制到 inf 旁边）
& $signtool sign /f $pfx /p $pwdPlain /fd SHA256 $cat
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed: $LASTEXITCODE" }
"catalog signed"

# 6) 把已签名的目录文件放到 inf 旁边
Copy-Item $cat (Join-Path $pkg "DualBltWinUSB.cat") -Force
"signed catalog copied next to inf"

# 7) 校验
& $signtool verify /pa $cat
"verify exit = $LASTEXITCODE"
