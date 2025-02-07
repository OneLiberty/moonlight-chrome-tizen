<p align=left>
	<a href="https://discord.gg/zHafSd3bTw">
		<img src="https://discordapp.com/api/guilds/1196915612522393651/widget.png?style=banner2" alt="Discord Banner 2"/> 
	</a>
</p>

## About
[Moonlight for Tizen](https://moonlight-stream.org) is an open-source client for NVIDIA GameStream and [Sunshine](https://github.com/LizardByte/Sunshine). It enables streaming games from a powerful desktop to Samsung Smart TVs running Tizen OS 5.5 or higher. For more details, setup guides, or troubleshooting, visit the [Moonlight wiki](https://github.com/moonlight-stream/moonlight-docs/wiki).

### Note
As a non-developer with limited coding knowledge, I do my best to maintain the repository and address issues. If you encounter problems, please report them in the issue section. While I can't guarantee a solution, I will certainly investigate.

## Getting Started
To install Moonlight on your Samsung Smart TV, start by ensuring your setup meets the [Prerequisites](https://github.com/OneLiberty/moonlight-chrome-tizen#prerequisites) and follow the [Installation](https://github.com/OneLiberty/moonlight-chrome-tizen#installation) guide.

### Prerequisites
You'll need:
- Windows Subsystem for Linux (WSL 2) — [Installation Guide](https://learn.microsoft.com/en-us/windows/wsl/install-manual)
- Docker Desktop — [Installation Guide](https://docs.docker.com/desktop/)
Ensure Docker Desktop is running and close any resource-intensive applications.

### Installation
1. **Enable Developer Mode on Samsung Smart TV**:
   - Navigate to `Apps` panel, enter `12345` on the remote, turn on `Developer mode`, input your PC's IP, and restart the TV.
2. **Launch Docker Image**:
   - Run in Windows PowerShell:
     ```
     docker run -it --rm ghcr.io/oneliberty/moonlight-chrome-tizen:samsung_wasm
     ```
3. **Install the Application**:
   - Connect via Smart Development Bridge (Replace `YOUR_TV_IP` with your TV's IP):
     ```
     sdb connect <YOUR_TV_IP>
     ```
   - (Optional) Uninstall the previous version if you are upgrading:
     ```
     sdb shell 0 vd_appuninstall MoonLightS.MoonlightWasm
     ```
   - Install via Smart Development Bridge:
     ```
     tizen install -n Moonlight.wgt
     exit
     ```
   
   > **Note**: If you have multiple TVs connected to SDB, you need to specify the target TV. Use the `-t <device_id>` option, where `<device_id>` is the ID shown in the last column of the output from the `sdb devices` command.

5. **(Optional) Disable Developer Mode**:
   - Revisit the `Apps` panel to turn off Developer mode and restart the TV.

Moonlight should now be available under `Recent Apps` on your Samsung Smart TV.


>[!NOTE]
> You can now install Moonlight via USB ! Check out the [release tab](https://github.com/OneLiberty/moonlight-chrome-tizen/releases) for more informations.


### Updating
To update Moonlight:
1. Delete the existing app from your TV.
2. Follow the installation instructions to install the latest version.

## Changelogs
View changes and updates in the [CHANGELOG](https://github.com/oneliberty/moonlight-chrome-tizen/blob/samsung_wasm/CHANGELOG.md).

## Contributing
Contributions are welcome! Fork the repo, create pull requests, or open issues. If you find the project useful, consider giving it a star!

## Credits
- Moonlight for Chrome OS is developed and maintained by [Moonlight Developers](https://github.com/moonlight-stream/moonlight-chrome)
- Moonlight for Tizen is based on Chrome OS version which was then adapted and powered by [Samsung Developers](https://github.com/SamsungDForum/moonlight-chrome)
- Dockerfile have been readapted by [pablojrl123](https://github.com/pablojrl123/moonlight-tizen-docker)
- Thanks to [henry2fa](https://github.com/henryfa2) for the discord and more. 
