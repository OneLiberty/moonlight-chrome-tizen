<p align=left>
	<a href="https://discord.gg/zHafSd3bTw">
		<img src="https://discordapp.com/api/guilds/1196915612522393651/widget.png?style=banner2" alt="Discord Banner 2"/> 
	</a>
</p>

## **About**

[Moonlight for Tizen](https://moonlight-stream.org) is an open-source client for NVIDIA GameStream and [Sunshine](https://github.com/LizardByte/Sunshine). It enables streaming games from a powerful desktop to Samsung Smart TVs running Tizen OS 5.5 or higher. For more details, setup guides, or troubleshooting, visit the [Moonlight wiki](https://github.com/moonlight-stream/moonlight-docs/wiki).

### Note

As a non-developer with limited coding knowledge, I do my best to maintain the repository and address issues. If you encounter problems, please report them in the issue section. While I can't guarantee a solution, I will certainly investigate.

## **Getting Started**

To install Moonlight on your Samsung Smart TV, start by ensuring your setup meets the [Prerequisites](https://github.com/OneLiberty/moonlight-chrome-tizen#prerequisites) and follow the [Installation](https://github.com/OneLiberty/moonlight-chrome-tizen#installation) guide.

Make sure to enable **Enable Developer Mode on Samsung Smart TV**:

- Navigate to `Apps` panel, enter `12345` on the remote, turn on `Developer mode`, input your PC's IP, and restart the TV (by long pressing the power button).

## **Installation**

### **Using Samsung-Jellyfin-Installer (Recommended)**

The easiest way to install Moonlight on your Samsung TV is with the [Samsung-Jellyfin-Installer](https://github.com/Jellyfin2Samsung/Samsung-Jellyfin-Installer):

1. **Install the Installer:** Download and open the Samsung-Jellyfin-Installer on your PC.
2. **Sign in:** Most TVs require a Samsung account to install apps. If needed, [create one here](https://account.samsung.com/).
3. **Find Moonlight:** In the installer, go to **Release → Tizen Community**, then select **Moonlight.wgt** (or **Moonlight-NoGame.wgt** for [MrPhaze62](https://github.com/MrPhaze62)'s version).
4. **Connect your TV:** Your TV should appear in the installer. Select it and click **Download and Install**. Log in with your Samsung account if prompted.

> **Tip:** You can also use the **Custom WGT File** option to install a `.wgt` file from your computer if needed.

After installation, Moonlight will appear under `Recent Apps` on your Samsung Smart TV.

#

### **Using Docker (Advanced)**

#### **Prerequisites**

You'll need:

- Windows Subsystem for Linux (WSL 2) — [Installation Guide](https://learn.microsoft.com/en-us/windows/wsl/install-manual)
- Docker Desktop — [Installation Guide](https://docs.docker.com/desktop/)
  Ensure Docker Desktop is running and close any resource-intensive applications.

#### **Installation**

1. **Launch Docker Image**:
   - Run in Windows PowerShell:
     ```
     docker run -it --rm ghcr.io/oneliberty/moonlight-chrome-tizen:samsung_wasm
     ```
2. **Install the Application**:
   - Connect and install via Smart Development Bridge:
     ```
     sdb connect <YOUR_TV_IP>
     tizen install -n Moonlight.wgt
     exit
     ```
   - Replace `YOUR_TV_IP` with your TV's IP.

   > **Note**: If you have multiple TVs connected to SDB, you need to specify the target TV. Use the `-t <device_id>` option, where `<device_id>` is the ID shown in the last column of the output from the `sdb devices` command.

Moonlight should now be available under `Recent Apps` on your Samsung Smart TV.

#

### **Updating**

To update Moonlight:

1. Delete the existing app from your TV.
2. Follow the installation instructions to install the latest version.

## **Changelogs**

View changes and updates in the [CHANGELOG](https://github.com/oneliberty/moonlight-chrome-tizen/blob/samsung_wasm/CHANGELOG.md).

## **Contributing**

Contributions are welcome! Fork the repo, create pull requests, or open issues. If you find the project useful, consider giving it a star!

## **Credits**

- Moonlight for Chrome OS is developed and maintained by [Moonlight Developers](https://github.com/moonlight-stream/moonlight-chrome)
- Moonlight for Tizen is based on Chrome OS version which was then adapted and powered by [Samsung Developers](https://github.com/SamsungDForum/moonlight-chrome)
- Dockerfile have been readapted by [pablojrl123](https://github.com/pablojrl123/moonlight-tizen-docker)
- Thanks to [henry2fa](https://github.com/henryfa2) for the discord and more.
