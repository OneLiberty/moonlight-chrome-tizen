/* eslint-disable */

const SyncFunctions = {
  // no parameters
  'makeCert': (...args) => Module.makeCert(...args),
  // cert, privateKey, myUniqueid
  'httpInit': (...args) => Module.httpInit(...args),
  // host, width height fps bitrate rikey rikeyid appversion gfeversion
  'startRequest': (...args) => Module.startStream(...args),
  // no parameters
  'stopRequest': (...args) => Module.stopStream(...args),
};

const AsyncFunctions = {
  // url, ppk, binaryResponse
  'openUrl': (...args) => Module.openUrl(...args),
  // no parameters
  'STUN': (...args) => Module.STUN(...args),
  // serverMajorVersion, address, randomNumber
  'pair': (...args) => Module.pair(...args),
};

var callbacks = {}
var callbacks_ids = 1;

/**
 * var sendMessage - Sends a message with arguments to the NaCl module
 *
 * @param  {String} method A named method
 * @param  {(String|Array)} params An array of options or a signle string
 * @return {void}        The NaCl module calls back trought the handleMessage method
 */
var sendMessage = function(method, params) {
  if (SyncFunctions[method]) {
    return new Promise(function(resolve, reject) {
      const ret = SyncFunctions[method](...params);
      if (ret.type === "resolve") {
        resolve(ret.ret);
      } else {
        reject(ret.ret);
      }
    });
  } else {
    return new Promise(function(resolve, reject) {
      const id = callbacks_ids++;
      callbacks[id] = {
        'resolve': resolve,
        'reject': reject
      };

      AsyncFunctions[method](id, ...params);
    });
  }
}

var handlePromiseMessage = function(callbackId, type, msg) {
  callbacks[callbackId][type](msg);
  delete callbacks[callbackId];
}

/**
 * handleMessage - Handles messages from the NaCl module
 *
 * @param  {Object} msg An object given by the NaCl module
 * @return {void}
 */
function handleMessage(msg) {
  console.log('%c[messages.js, handleMessage]', 'color:gray;', 'Message data: ', msg);
  if (msg.indexOf('streamTerminated: ') === 0) { // if it's a recognized event, notify the appropriate function
    // Release our keep awake request
    if (runningOnChrome()) {
      chrome.power.releaseKeepAwake();
    }

    // Show a termination snackbar message if the termination was unexpected
    var errorCode = parseInt(msg.replace('streamTerminated: ', ''));
    if (errorCode !== 0) {
      snackbarLogLong("Connection terminated");
    }

    api.refreshServerInfo().then(function(ret) {
      // Return to app list with new currentgame
      showApps(api);
    }, function() {
      // Return to app list anyway
      showApps(api);
    });
  } else if (msg === 'Connection Established') {
    $('#loadingSpinner').css('display', 'none');
    $('body').css('backgroundColor', 'transparent');
    $("#nacl_module").css("display", "");
    $("#nacl_module").focus();

    // Keep the display awake while streaming
    if (runningOnChrome()) {
      chrome.power.requestKeepAwake("display");
    }
  } else if (msg.indexOf('ProgressMsg: ') === 0) {
    $('#loadingMessage').text(msg.replace('ProgressMsg: ', ''));
  } else if (msg.indexOf('TransientMsg: ') === 0) {
    snackbarLog(msg.replace('TransientMsg: ', ''));
  } else if (msg.indexOf('DialogMsg: ') === 0) {
    // FIXME: Really use a dialog
    snackbarLogLong(msg.replace('DialogMsg: ', ''));
  } else if (msg === 'displayVideo') {
    $("#listener").addClass("fullscreen");
  } else if (msg.indexOf('controllerRumble: ') === 0) {
	  const eventData = msg.split(' ')[1].split(',');
	  const gamepadIdx = parseInt(eventData[0]);
	  const weakMagnitude = parseFloat(eventData[1]);
	  const strongMagnitude = parseFloat(eventData[2]);
	  console.log("Playing rumble on gamepad " + gamepadIdx + " with weakMagnitude " + weakMagnitude + " and strongMagnitude " + strongMagnitude);

	  const gamepads = navigator.getGamepads();
	  const gamepad = gamepads[gamepadIdx];

	  if (gamepad && gamepad.vibrationActuator) {
	    gamepad.vibrationActuator.playEffect('dual-rumble', {
	      startDelay: 0,
	      duration: 200,
	      weakMagnitude: weakMagnitude,
	      strongMagnitude: strongMagnitude,
	    });
	  }
	}
}
