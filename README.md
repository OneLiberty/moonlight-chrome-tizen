<p align=left>
	<a href="https://discord.gg/zHafSd3bTw">
		<img src="https://discordapp.com/api/guilds/1196915612522393651/widget.png?style=banner2" alt="Discord Banner 2"/> 
	</a>
</p>

## About
[Moonlight for Tizen](https://moonlight-stream.org) is an open source client for NVIDIA GameStream and [Sunshine](https://github.com/LizardByte/Sunshine).

Moonlight for Tizen allows you to stream your full collection of games from your powerful desktop to your Samsung Smart TV running Tizen OS 5.5 or more.

Check out the [Moonlight wiki](https://github.com/moonlight-stream/moonlight-docs/wiki) for more detailed project information, setup guide, or troubleshooting steps.

### Small note
I am not a developer, and my knowledge of coding is limited. I am putting in my best effort to address issues and maintain the repository. If you encounter any problems, please report them in the issue section. While I cannot guarantee a solution, I will certainly investigate.

Please be aware that my fixes may not be optimal, and there might be better ways to resolve issues. I appreciate your understanding as I do my best to improve the application.

## Getting Started

Starting with the project, you should first take a look at the required [Prerequisites](https://github.com/OneLiberty/moonlight-chrome-tizen#prerequisites) and then follow the [Installation](https://github.com/OneLiberty/moonlight-chrome-tizen#installation) instructions in order to successfully install Moonlight on your Samsung Smart TV.

### Prerequisites

Before building this application, you must have [Windows Subsystem for Linux (WSL 2)](https://learn.microsoft.com/en-us/windows/wsl/install-manual) and [Docker Desktop](https://docs.docker.com/desktop/) installed on your computer.

Also, you should run "Docker Desktop" before proceeding further and it is also recommended to close any software or application that requires high CPU and memory resources, because "Docker Desktop" will take high resources during use.

### Installation
1. Enable the "Developer mode" in your "Samsung Smart TV" (If you need more detailed instructions, see the official [Samsung guide](https://developer.samsung.com/smarttv/develop/getting-started/using-sdk/tv-device.html)):
	- Go to the `Apps` panel.
	- Press `12345` on the remote and a dialog should popup.
	- Set `Developer mode` to `On` and fill in the `Host PC IP` field which is the `IP Address` of your PC, then click the `OK` button to close the dialog.
	- Restart the TV by holding the power button for 2 seconds as instructed by the new dialog popup, then again go to the `Apps` panel.
	- Depending on your model, a `DEVELOP MODE` or similar message will appear in the `Apps` panel at the top of the screen.
2. After that, in `Windows PowerShell`, enter the following command to launch the "Docker" image:
	```
	docker run -it --rm ghcr.io/oneliberty/moonlight-chrome-tizen:samsung_wasm
	```
	- This operation may take a while, please be patient.
3. After that, in `Windows PowerShell`, follow the steps below to install the application on your TV:

	- Next, enter the following command to connect to your "Samsung Smart TV" over "Smart Development Bridge":
	 ```
	 sdb connect YOUR_TV_IP
	 ```
 	> Note: Replace `YOUR_TV_IP` with `IP Address` of your TV.
	- Next, enter the following command to confirm that you are connected, then take note of the "Device ID":
	 ```
	 sdb devices
	 ```
 	> Note: Just to clarify "Device ID" will be the last column, something like `UE55AU7172UXXH`.
	- Next, enter the following command to install the package:
	 ```
	 tizen install -n Moonlight.wgt -t YOUR_DEVICE_ID
	 ```
 	> Note: Replace `YOUR_DEVICE_ID` with `Device ID` of your TV.
 	- After that, Moonlight should now appear in your `Recent Apps` or similar page on your "Samsung Smart TV".
	- Next, enter the following command to exit the container:
	 ```
	 exit
	 ```
 	> Note: At the end you can enter the `exit` command to close the `Windows PowerShell` window.
	 
4. (Optional) Disable the "Developer mode" in your "Samsung Smart TV":
	- Go to the `Apps` panel.
	- Press `12345` on the remote and a dialog should popup.
	- Set `Developer mode` to `Off` and then click the `OK` button to close the dialog.
	- Restart the TV by holding the power button for 2 seconds as instructed by the new dialog popup, then again go to the `Apps` panel.
	- Depending on your model, a `DEVELOP MODE` or similar message will disappear from the `Apps` panel at the top of the screen.
5. Now you can launch Moonlight on your TV, then add your host computer and enjoy the high quality streaming experience.

### Updating

1. Before updating the Moonlight app, you must delete the installed Moonlight app that you already have on your Samsung Smart TV to prevent errors during the update.
2. Now, whenever you want to install an updated version of Moonlight on your Samsung Smart TV, you need to follow the [Installation](https://github.com/kyrofrcode/moonlight-chrome-tizen#installation) instructions in order to successfully install the updated version of Moonlight on your Samsung Smart TV.

## Changelogs

See the [CHANGELOG](https://github.com/kyrofrcode/moonlight-chrome-tizen/blob/samsung_wasm/CHANGELOG.md) file for more information about the changes for each version of this project.

## Contributing

Contributions are what make the open source community such an amazing place to learn, inspire, and create. Any contributions you make are **greatly appreciated**.

If you have a suggestion that would make this project better, then feel free to fork the repo, create a pull request or open an issue.

Also, if you liked the project or found it useful, don't forget to give the project a star!


## Credits
- Moonlight for Chrome OS is developed and maintained by [Moonlight Developers](https://github.com/moonlight-stream/moonlight-chrome)
- Moonlight for Tizen is based on Chrome OS version which was then adapted and powered by [Samsung Developers](https://github.com/SamsungDForum/moonlight-chrome)
- Support files and Dockerfile have been adapted by [jellyfin](https://github.com/jellyfin/jellyfin-tizen) and [babagreensheep](https://github.com/babagreensheep/jellyfin-tizen-docker)
- Dockerfile have been readapted by [pablojrl123](https://github.com/pablojrl123/moonlight-tizen-docker)
- Some content files have been readapted by [ndriqimlahu](https://github.com/ndriqimlahu)
- Thanks to [henry2fa](https://github.com/henryfa2) for the discord
