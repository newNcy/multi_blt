$ErrorActionPreference = "Continue"
$log = "G:\multi_blt\winusb\install_log.txt"
Start-Transcript -Path $log -Force

$pfx = "G:\multi_blt\winusb\cert.pfx"
$pwd = ConvertTo-SecureString -String "DualBlt123" -Force -AsPlainText

# 1) 证书导入受信任根 / 受信任发布者
foreach ($store in @("Root", "TrustedPublisher")) {
    $st = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, "LocalMachine")
    $st.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $cert = Import-PfxCertificate -FilePath $pfx -CertStoreLocation ("Cert:\LocalMachine\" + $store) -Password $pwd
    $st.Close()
    Write-Host ("imported into {0}: {1}" -f $store, $cert.Thumbprint)
}

# 2) 安装 WinUSB 驱动包
pnputil /add-driver "G:\multi_blt\winusb\pkg\DualBltWinUSB.inf" /install

Stop-Transcript
