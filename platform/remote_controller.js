function remoteControllerHandler(e) {
    const keyCode = e.keyCode;

    // Prevent the default behavior for specific keys
    switch (keyCode) {
        case tvKey.KEY_UP:
        case tvKey.KEY_DOWN:
        case tvKey.KEY_LEFT:
        case tvKey.KEY_RIGHT:
        case tvKey.KEY_ENTER:
        case tvKey.KEY_REMOTE_ENTER:
        case tvKey.KEY_RETURN:
        case tvKey.KEY_VOLUME_UP:
        case tvKey.KEY_VOLUME_DOWN:
        case tvKey.KEY_VOLUME_MUTE:
            e.preventDefault();
            break;
        default:
            // For other keys, do not prevent the default behavior
    }

    switch (keyCode) {
        case tvKey.KEY_UP:
            Navigation.up();
            break;
        case tvKey.KEY_DOWN:
            Navigation.down();
            break;
        case tvKey.KEY_LEFT:
            Navigation.left();
            break;
        case tvKey.KEY_RIGHT:
            Navigation.right();
            break;
        case tvKey.KEY_ENTER:
        case tvKey.KEY_REMOTE_ENTER:
            Navigation.accept();
            break;
        case tvKey.KEY_RETURN:
            Navigation.back();
            break;
        case tvKey.KEY_VOLUME_UP:
            tizen.tvaudiocontrol.setVolumeUp();
            break;
        case tvKey.KEY_VOLUME_DOWN:
            tizen.tvaudiocontrol.setVolumeDown();
            break;
        case tvKey.KEY_VOLUME_MUTE:
            tizen.tvaudiocontrol.setMute();
            break;
    }
}
