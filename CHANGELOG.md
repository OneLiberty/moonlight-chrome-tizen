# Changelog

All notable changes to this project will be documented in this file.

## v1.1.1

### Added.
- HDR toggle in the app. If your TV is not compatible with HDR this might result in a crash of the app or blackscreen
  Also note that your computer need to have HDR enabled in order to stream HDR.

### Changed
- Remote handler, should fix issue in navigation using the tv remote.
- Fix CSS outline (thanks to @AlexPresso)

## v1.1.0

### Added
- Host deletion from the gamepad (press start while hovering over the host to delete it).
- Temporary workaround for the keyboard disappearing issue in Tizen 7.0.
- Mouse support for 2021+ TVs 

### Changed
- Fixed rumble on tizen 5.5 and 6.0
- Improved controller navigation within the app.
- Fixed  IPV6 host resolution (removed [] bracket from the ip in the host resolution sequence)
- Various minor fixes.

## v1.0.8

### Added
- Support for rumble (modified code from the original NaCl moonlight-chrome)
- Now the controller will vibrate on first connection to notify the user
- Controller should vibrate during gameplay (if supported by app and controller)

### Changed
- Updated the project readme to reflect the changes in the building process

### Removed
- Unnecessary files (mostly duplicates)

## v1.0.7 All credits for the changes go to ndriqimlahu (deleted repository)

### Added
- Enhanced card hover styles for better appearance

### Changed
- Aligned 'Add Host' and 'Hosts PC' text to the center
- Increased the font size application-wide for better readability
- Changed the text content in the 'Pairing' dialog for better clarity
- Improved the readability of dialog texts and snack bar logs

## v1.0.6

### Added
- Enhanced button focus styles for better appearance

### Changed
- Adjusted the width of dialogs application-wide
- Adjusted padding for title and content text in dialogs
- Increased the width of the input field in the 'Add Host' dialog
- Adjusted dialog buttons to increase their size

## v1.0.5

### Added
- Introduced a new feature: 'Terminate Moonlight' dialog
- Implemented dynamic handling for 'Terminate Moonlight' dialog
- Show 'Terminate Moonlight' dialog via 'Back' button on 'Hosts' view

## v1.0.4

### Added
- Added comments to improve code clarity and understanding

### Changed
- Updated '480p' resolution setting
- Updated default bitrate setting to '20 Mbps'
- Reordered 'STOP_STREAM_BUTTONS_FLAGS' buttons

## v1.0.3

### Added
- Expanded settings: Introduced '480p' resolution option

### Changed
- Updated default bitrate and resolution settings from '720p' to '1080p'

### Fixed
- Fixed bitrate presets for '480p' resolution option

## v1.0.2

### Changed
- Updated the 'Frame Pacing' icon for better appearance in the Settings menu
- Enhanced dialog and error messages application-wide

### Fixed
- Fixed spelling errors in the code

### Removed
- Removed extra white spaces and code formatting

## v1.0.1

### Changed
- Dockerfile has been updated for the build process
- Updated widget name and description
- Improved app logo icons for enhanced appearance in the Smart Hub