var hosts = {}; // hosts is an associative array of NvHTTP objects, keyed by server UID
var activePolls = {}; // hosts currently being polled.  An associated array of polling IDs, keyed by server UID
var pairingCert;
var myUniqueid = '0123456789ABCDEF'; // Use the same UID as other Moonlight clients to allow them to quit each other's games
var api; // `api` should only be set if we're in a host-specific screen. on the initial screen it should always be null.
var isInGame = false; // flag indicating whether the game stream started
var isDialogOpen = false; // track whether the dialog is open

function loadProductInfos() {
  const modelCodePlaceholder = document.getElementById("modelCodePlaceholder");
  if (modelCodePlaceholder) {
    const model = window.tizen.systeminfo.getCapability('http://tizen.org/system/model_name') || "Not Available";
    const moonlightVersion = window.tizen.application.getAppInfo().version || "Not Available";
    const tizenVersion = window.tizen.systeminfo.getCapability('http://tizen.org/feature/platform.version') || "Not Available";
    modelCodePlaceholder.innerText = `TV Model: ${model} ; Moonlight: v${moonlightVersion} ; Tizen: v${tizenVersion}`;
  }
}

let repeatAction = null;
let lastInvokeTime = 0;
let repeatStartTimeout = null;
const REPEAT_START_DELAY = 350;
const REPEAT_INTERVAL = 100;

// Called by the common.js module.
function attachListeners() {
  changeUiModeForNaClLoad();

  $('.resolutionMenu li').on('click', saveResolution);
  $('.framerateMenu li').on('click', saveFramerate);
  $('#bitrateSlider').on('input', updateBitrateField); // input occurs every notch you slide
  //$('#bitrateSlider').on('change', saveBitrate); //FIXME: it seems not working
  $("#remoteAudioEnabledSwitch").on('click', saveRemoteAudio);
  $('#optimizeGamesSwitch').on('click', saveOptimize);
  $('#framePacingSwitch').on('click', saveFramePacing);
  $('#audioSyncSwitch').on('click', saveAudioSync);
  $('#hdrSwitch').on('click', saveHdr);
  $('.codecVideoMenu li').on('click', saveCodecVideo);
  $('#addHostCell').on('click', addHost);
  $('#backIcon').on('click', showHostsAndSettingsMode);
  $('#quitCurrentApp').on('click', stopGameWithConfirmation);

  const registerMenu = (elementId, view) => {
    $(`#${elementId}`).on('click', () => {
      if (view.isActive())
        Navigation.pop();
      else
        Navigation.push(view);
    });
  }
  registerMenu('selectCodecVideo', Views.SelectCodecVideoMenu);
  registerMenu('selectResolution', Views.SelectResolutionMenu);
  registerMenu('selectFramerate', Views.SelectFramerateMenu);
  registerMenu('bandwidthMenu', Views.SelectBitrateMenu);

  Controller.startWatching();
  window.addEventListener('gamepadbuttonpressed', (e) => {
    const pressed = e.detail.pressed;
    const key = e.detail.key;
    const gamepadMapping = {
      0: () => Navigation.accept(),
      1: () => Navigation.back(),
      8: () => Navigation.selectBtn(),
      9: () => Navigation.startBtn(),
      12: () => Navigation.up(),
      13: () => Navigation.down(),
      14: () => Navigation.left(),
      15: () => Navigation.right(),
    };

    if (pressed) {
      if (gamepadMapping[key]) {
        gamepadMapping[key]();
        repeatAction = gamepadMapping[key];
        lastInvokeTime = Date.now();
        repeatStartTimeout = setTimeout(() => {
          requestAnimationFrame(repeatActionHandler);
        }, REPEAT_START_DELAY);
      }
    } else {
      repeatAction = null;
      clearTimeout(repeatStartTimeout);
    }
  });
}

function sendEscapeToHost() { //FIXME: workaround to send escape key to host
  Module.sendLiSendKeyboardEvent(0x80 << 8 | 0x1B, 0x03, 0);
  Module.sendLiSendKeyboardEvent(0x80 << 8 | 0x1B, 0x04, 0);
}

function repeatActionHandler() {
  if (repeatAction && Date.now() - lastInvokeTime > REPEAT_INTERVAL) {
      repeatAction();
      lastInvokeTime = Date.now();
  }
  if (repeatAction) {
    requestAnimationFrame(repeatActionHandler);
  }
}

function loadWindowState() {
  if (!runningOnChrome()) {
    return;
  }
}

function changeUiModeForNaClLoad() {
  $('#main-navigation').children().hide();
  $("#main-content").children().not("#listener, #naclSpinner").hide();
  $('#naclSpinnerMessage').text('Loading Moonlight plugin...');
  $('#naclSpinner').css('display', 'inline-block');
}

function startPollingHosts() {
  for (var hostUID in hosts) {
    beginBackgroundPollingOfHost(hosts[hostUID]);
  }
}

function stopPollingHosts() {
  for (var hostUID in hosts) {
    stopBackgroundPollingOfHost(hosts[hostUID]);
  }
}

function restoreUiAfterNaClLoad() {
  $('#main-navigation').children().not("#quitCurrentApp").show();
  $("#main-content").children().not("#listener, #naclSpinner, #game-grid").show();
  $('#naclSpinner').hide();
  $('#loadingSpinner').css('display', 'none');
  Navigation.push(Views.Hosts);
  showHostsAndSettingsMode();

  findNvService(function(finder, opt_error) {
    if (finder.byService_['_nvstream._tcp']) {
      var ips = Object.keys(finder.byService_['_nvstream._tcp']);
      for (var i in ips) {
        var ip = ips[i];
        if (finder.byService_['_nvstream._tcp'][ip]) {
          var mDnsDiscoveredHost = new NvHTTP(ip, myUniqueid);
          mDnsDiscoveredHost.pollServer(function(returneMdnsDiscoveredHost) {
            // Just drop this if the host doesn't respond
            if (!returneMdnsDiscoveredHost.online) {
              return;
            }

            if (hosts[returneMdnsDiscoveredHost.serverUid] != null) {
              // if we're seeing a host we've already seen before, update it for the current local IP.
              hosts[returneMdnsDiscoveredHost.serverUid].address = returneMdnsDiscoveredHost.address;
              hosts[returneMdnsDiscoveredHost.serverUid].updateExternalAddressIP4();
            } else {
              // Host must be in the grid before starting background polling
              addHostToGrid(returneMdnsDiscoveredHost, true);
              beginBackgroundPollingOfHost(returneMdnsDiscoveredHost);
            }

            saveHosts();
          });
        }
      }
    }
  });
}

function beginBackgroundPollingOfHost(host) {
  var el = document.querySelector('#hostgrid-' + host.serverUid)
  if (host.online) {
    el.classList.remove('host-cell-inactive')
    // The host was already online. Just start polling in the background now.
    activePolls[host.serverUid] = window.setInterval(function() {
      // every 5 seconds, poll at the address we know it was live at
      host.pollServer(function() {
        if (host.online) {
          el.classList.remove('host-cell-inactive')
        } else {
          el.classList.add('host-cell-inactive')
        }
      });
    }, 5000);
  } else {
    el.classList.add('host-cell-inactive')
    // The host was offline, so poll immediately.
    host.pollServer(function() {
      if (host.online) {
        el.classList.remove('host-cell-inactive')
      } else {
        el.classList.add('host-cell-inactive')
      }

      // Now start background polling
      activePolls[host.serverUid] = window.setInterval(function() {
        // every 5 seconds, poll at the address we know it was live at
        host.pollServer(function() {
          if (host.online) {
            el.classList.remove('host-cell-inactive')
          } else {
            el.classList.add('host-cell-inactive')
          }
        });
      }, 5000);
    });
  }
}

function stopBackgroundPollingOfHost(host) {
  console.log('%c[index.js, backgroundPolling]', 'color: green;', 'Stopping background polling of host ' + host.serverUid + '\n', host, host.toString()); //Logging both object (for console) and toString-ed object (for text logs)
  window.clearInterval(activePolls[host.serverUid]);
  delete activePolls[host.serverUid];
}

function updateMacAddress(host) { //FIXME(?) : needed to correctly set the stored mac address (indexedDB)
  getData('hosts', function (previousValue) {
    var hosts = previousValue.hosts != null ? previousValue.hosts : {};
    var currentHostUID = host.serverUid;
    if (host.macAddress != "00:00:00:00:00:00") {
      if (hosts[currentHostUID] && hosts[currentHostUID].macAddress != host.macAddress) {
        console.log("Updated MAC address for host " + host.hostname + " from " + hosts[currentHostUID].macAddress + " to " + host.macAddress);
        saveHosts();
      }
    }
  });
}

function snackbarLog(givenMessage) {
  console.log('%c[index.js, snackbarLog]', 'color: green;', givenMessage);
  var data = {
    message: givenMessage,
    timeout: 2000
  };
  document.querySelector('#snackbar').MaterialSnackbar.showSnackbar(data);
}

function snackbarLogLong(givenMessage) {
  console.log('%c[index.js, snackbarLog]', 'color: green;', givenMessage);
  var data = {
    message: givenMessage,
    timeout: 5000
  };
  document.querySelector('#snackbar').MaterialSnackbar.showSnackbar(data);
}

function updateBitrateField() {
  $('#bitrateField').html($('#bitrateSlider').val() + " Mbps");
  saveBitrate();
}

function moduleDidLoad() {
  loadHTTPCerts();
}

// pair to the given NvHTTP host object.  Returns whether pairing was successful.
function pairTo(nvhttpHost, onSuccess, onFailure) {
  if (!onFailure) {
    onFailure = function() {}
  }

  if (!pairingCert) {
    snackbarLog('ERROR: cert has not been generated yet. Is NaCl initialized?');
    console.warn('%c[index.js]', 'color: green;', 'User wants to pair, and we still have no cert. Problem = very yes.');
    onFailure();
    return;
  }

  nvhttpHost.pollServer(function(ret) {
    if (!nvhttpHost.online) {
      snackbarLog('Failed to connect to ' + nvhttpHost.hostname + '! Ensure Sunshine is running on your host PC or GameStream is enabled in GeForce Experience SHIELD settings.');
      console.error('%c[index.js]', 'color: green;', 'Host declared as offline:', nvhttpHost, nvhttpHost.toString()); //Logging both the object and the toString version for text logs
      onFailure();
      return;
    }

    if (nvhttpHost.paired) {
      onSuccess();
      return;
    }

    var randomNumber = String("0000" + (Math.random() * 10000 | 0)).slice(-4);
    var pairingDialog = document.querySelector('#pairingDialog');
    $('#pairingDialogText').html('Please enter the following PIN on the target PC:  ' + randomNumber + '<br><br>If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.<br>Alternatively, navigate to the GeForce Experience (NVIDIA GPUs only) to enter the PIN.<br><br>This dialog will close once the pairing is complete.');
    pairingDialog.close();
    pairingDialog.showModal();
    Navigation.push(Views.PairingDialog);

    $('#cancelPairingDialog').off('click');
    $('#cancelPairingDialog').on('click', function() {
      pairingDialog.close();
      Navigation.pop();
    });

    console.log('%c[index.js]', 'color: green;', 'Sending pairing request to ' + nvhttpHost.hostname + ' with PIN: ' + randomNumber);
    nvhttpHost.pair(randomNumber).then(function() {
      snackbarLog('Pairing successful');
      pairingDialog.close();
      Navigation.pop();
      onSuccess();
    }, function(failedPairing) {
      snackbarLog('Failed pairing to: ' + nvhttpHost.hostname);
      if (nvhttpHost.currentGame != 0) {
        $('#pairingDialogText').html('Error: ' + nvhttpHost.hostname + ' is busy.  Stop streaming to pair.');
      } else {
        $('#pairingDialogText').html('Error: failed to pair with ' + nvhttpHost.hostname + '.');
      }
      console.log('%c[index.js]', 'color: green;', 'Failed API object:', nvhttpHost, nvhttpHost.toString()); //Logging both the object and the toString version for text logs
      onFailure();
    });
  });
}

function hostChosen(host) {
  if (!host.online) {
    return;
  }

  // Avoid delay from other polling during pairing
  stopPollingHosts();

  api = host;
  if (!host.paired) {
    // Still not paired; go to the pairing flow
    pairTo(host, function() {
        showApps(host);
        saveHosts();
        Navigation.push(Views.Apps);
      },
      function() {
        startPollingHosts();
      });
  } else {
    // When we queried again, it was paired, so show apps.
    showApps(host);
    Navigation.push(Views.Apps);
  }
}

// the `+` was selected on the host grid.
// give the user a dialog to input connection details for the PC
function addHost() {
  var modal = document.querySelector('#addHostDialog');
  modal.showModal();
  Navigation.push(Views.AddHostDialog);

  // drop the dialog if they cancel
  $('#cancelAddHost').off('click');
  $('#cancelAddHost').on('click', function() {
    modal.close();
    Navigation.pop();
  });

  // try to pair if they continue
  $('#continueAddHost').off('click');
  $('#continueAddHost').on('click', function() {
  $(this).prop('disabled', true); // disable the button so users don't send multiple requests
	var inputHost;
	if ($('#manualInputToggle').prop('checked')) {
	      // Manual input is selected
	      inputHost = $('#manualIPAddress').val();
  } else {
	var ipPart1 = $('#ipPart1').val();
	var ipPart2 = $('#ipPart2').val();
	var ipPart3 = $('#ipPart3').val();
	var ipPart4 = $('#ipPart4').val();
	inputHost = ipPart1 + '.' + ipPart2 + '.' + ipPart3 + '.' + ipPart4;
  }
    var _nvhttpHost = new NvHTTP(inputHost, myUniqueid, inputHost);

    _nvhttpHost.refreshServerInfoAtAddress(inputHost).then(function(success) {
      modal.close();
      Navigation.pop();

      // Check if we already have record of this host. If so,
      // we'll need the PPK string to ensure our pairing status is accurate.
      if (hosts[_nvhttpHost.serverUid] != null) {
        // Update the addresses
        hosts[_nvhttpHost.serverUid].address = _nvhttpHost.address;
        hosts[_nvhttpHost.serverUid].userEnteredAddress = _nvhttpHost.userEnteredAddress;

        // Use the host in the array directly to ensure the PPK propagates after pairing
        pairTo(hosts[_nvhttpHost.serverUid], function() {
          saveHosts();
        });
      }
      else {
        pairTo(_nvhttpHost, function() {
          // Host must be in the grid before starting background polling
          addHostToGrid(_nvhttpHost);
          beginBackgroundPollingOfHost(_nvhttpHost);
          saveHosts();
        });
      }
      $('#continueAddHost').prop('disabled', false); // re-enable the button on success
    }.bind(this),
    function(failure) {
      snackbarLog('Failed to connect to ' + _nvhttpHost.hostname + '! Ensure Sunshine is running on your host PC or GameStream is enabled in GeForce Experience SHIELD settings.');
      $('#continueAddHost').prop('disabled', false); // re-enable the button on failure
    }.bind(this));
  });
}


// host is an NvHTTP object
function addHostToGrid(host, ismDNSDiscovered) {
  var outerDiv = $("<div>", {
    class: 'host-container mdl-card mdl-shadow--4dp',
    id: 'host-container-' + host.serverUid,
    role: 'link',
    tabindex: 0,
    'aria-label': host.hostname
  });
  var cell = $("<div>", {
    class: 'mdl-card__title mdl-card--expand',
    id: 'hostgrid-' + host.serverUid
  });
  $(cell).prepend($("<h2>", {
    class: "mdl-card__title-text",
    html: host.hostname
  }));
  var settingsButton = $("<div>", {
    class: "host-settings",
    id: "hostSettingsButton-" + host.serverUid,
    role: 'button',
    tabindex: 0,
    'aria-label': 'Settings ' + host.hostname
  });

  var settingsDialog = $('<dialog>', {
    class: 'mdl-dialog',
    id: "settingsDialog-" + host.serverUid
  }).appendTo(outerDiv);

  $('<h4>', {
    class: 'mdl-dialog__title',
    text: 'Settings ' + host.hostname
  }).appendTo(settingsDialog);

  var dialogContent = $('<div>', {
    class: 'mdl-dialog__content'
  }).appendTo(settingsDialog);

  var options = [ // host settings dialog options, used an array to make it easier to add more options
    { text: 'Wake PC (WOL)', id: "wake-" + host.hostname, action: function () {host.sendWOL(); } },
    // { text: 'Show hidden Apps (WIP)', id: "showHiddenApps-" + host.hostname, action: null }, //TODO: implement this
    { text: 'Refresh box art', id: "refreshBoxArt-" + host.hostname, action: function () {host.purgeBoxArt(); } },
    { text: 'Remove ' + host.hostname, id: "remove-" + host.hostname, action: function () {removeClicked(host); } }
  ];

  options.forEach(function (option) {
    var button = $('<button>', {
      class: 'mdl-button mdl-js-button mdl-button--raised mdl-js-ripple-effect',
      text: option.text,
      id: option.id
    });
    button.click(function() {
      Navigation.pop();
      option.action();
      settingsDialog[0].close();
    });
    button.appendTo(dialogContent);
  });

  $('<button>', {
    type: 'button',
    class: 'mdl-button',
    text: 'Close',
    id: 'closeSettingsDialog'
  }).click(function () {
    settingsDialog[0].close();
    Navigation.pop();
  }).appendTo($('<div>', {
    class: 'mdl-dialog__actions'
  }).appendTo(settingsDialog));

  if (!settingsDialog[0].showModal) {
    dialogPolyfill.registerDialog(settingsDialog[0]);
  }

  settingsButton.click(function () {
    settingsDialog[0].showModal();
    Navigation.push(Views.SettingsDialog, host.hostname);
  });

  cell.off('click');
  cell.click(function() {
    hostChosen(host);
  });
  outerDiv.keypress(function(e) {
    if (e.keyCode == 13) {
      hostChosen(host);
    }
  });
  $(outerDiv).append(cell);
  if (!ismDNSDiscovered) {
    // we don't have the option to delete mDNS hosts.  So don't show it to the user.
    $(outerDiv).append(settingsButton);
  }
  $('#host-grid').append(outerDiv);
  hosts[host.serverUid] = host;
  if (ismDNSDiscovered) {
    hosts[host.serverUid].updateExternalAddressIP4();
  }
}

function removeClicked(host) {
  var deleteHostDialog = document.querySelector('#deleteHostDialog');
  document.getElementById('deleteHostDialogText').innerHTML =
    ' Are you sure you want to delete ' + host.hostname + '?';
  deleteHostDialog.showModal();
  Navigation.push(Views.DeleteHostDialog);

  $('#cancelDeleteHost').off('click');
  $('#cancelDeleteHost').on('click', function() {
    deleteHostDialog.close();
    Navigation.pop();
  });

  // locally remove the hostname/ip from the saved `hosts` array.
  // note: this does not make the host forget the pairing to us.
  // this means we can re-add the host, and will still be paired.
  $('#continueDeleteHost').off('click');
  $('#continueDeleteHost').on('click', function() {
    var deleteHostDialog = document.querySelector('#deleteHostDialog');
    $('#host-container-' + host.serverUid).remove();
    delete hosts[host.serverUid]; // remove the host from the array;
    saveHosts();
    deleteHostDialog.close();
    Navigation.pop();
  });
}

window.removeClicked = removeClicked;

// Function to show the Restart Moonlight dialog
function showRestartMoonlightDialog() {
  var restartMoonlightDialog = document.querySelector('#restartMoonlightDialog');

  // Show the dialog and push the view
  restartMoonlightDialog.showModal();
  Navigation.push(Views.RestartMoonlightDialog);

  isDialogOpen = true;

  $('#pressOK').off('click');
  $('#pressOK').on('click', function() {
    restartMoonlightDialog.close();
    isDialogOpen = false;
    Navigation.pop();
  });
}
	
// Function to show the Terminate Moonlight dialog
function showTerminateMoonlightDialog() {
  var terminateMoonlightDialog = document.querySelector('#terminateMoonlightDialog');
  
  terminateMoonlightDialog.showModal();
  Navigation.push(Views.TerminateMoonlightDialog);

  // Set the dialog as open
  isDialogOpen = true;

  // Close the dialog if the Cancel button is pressed
  $('#cancelTerminateMoonlight').off('click');
  $('#cancelTerminateMoonlight').on('click', function() {
    terminateMoonlightDialog.close();
    isDialogOpen = false;
    Navigation.pop();
    Navigation.change(Views.Hosts);
  });

  // Terminate the application if the Exit button is pressed
  $('#exitTerminateMoonlight').off('click');
  $('#exitTerminateMoonlight').on('click', function() {
    terminateMoonlightDialog.close();
    isDialogOpen = false;
    Navigation.pop();
    tizen.application.getCurrentApplication().exit();
  });
}

// puts the CSS style for current app on the app that's currently running
// and puts the CSS style for non-current app apps that aren't running
// this requires a hot-off-the-host `api`, and the appId we're going to stylize
// the function was made like this so that we can remove duplicated code, but
// not do N*N stylizations of the box art, or make the code not flow very well
function stylizeBoxArt(freshApi, appIdToStylize) {
  // If the running game is the good one then style it
  var el = document.querySelector("#game-" + appIdToStylize);
  if(freshApi.currentGame === appIdToStylize) {
    el.classList.add('current-game')
    el.title += ' (Running)'
  } else {
    el.classList.remove('current-game')
    el.title.replace(' (Running)', '') // TODO: Replace with localized string so make it e.title = game_title
  }
}

function sortTitles(list, sortOrder) {
  return list.sort((a, b) => {
    const titleA = a.title.toLowerCase();
    const titleB = b.title.toLowerCase();

    // A - Z
    if (sortOrder === 'ASC') {
      if (titleA < titleB) {
        return -1;
      }
      if (titleA > titleB) {
        return 1;
      }
      return 0;
    }

    // Z - A
    if (sortOrder === 'DESC') {
      if (titleA < titleB) {
        return 1;
      }
      if (titleA > titleB) {
        return -1;
      }
      return 0;
    }
  });
}

// show the app list
function showApps(host) {
  if (!host || !host.paired) { // safety checking. shouldn't happen.
    console.log('%c[index.js, showApps]', 'color: green;', 'Moved into showApps, but `host` did not initialize properly! Failing.');
    return;
  }
  console.log('%c[index.js, showApps]', 'color: green;', 'Current host object:', host, host.toString()); //Logging both object (for console) and toString-ed object (for text logs)
  $('#quitCurrentApp').show();
  $("#gameList .game-container").remove();

  // Show a spinner while the applist loads
  $('#naclSpinnerMessage').text('Loading apps...');
  $('#naclSpinner').css('display', 'inline-block');

  $("div.game-container").remove();

  host.getAppList().then(function(appList) {
    $('#naclSpinner').hide();
    $("#game-grid").show();

    if(appList.length == 0) {
      console.error('%c[index.js, showApps]', 'User\'s applist is empty')
      var img = new Image()
      img.src = 'static/res/applist_empty.svg'
      $('#game-grid').html(img)
      snackbarLog('Your game list is empty')
      return; // We stop the function right here
    }
    var focusSetOnFirstGame = false
    // if game grid is populated, empty it
    const sortedAppList = sortTitles(appList, 'ASC');

    sortedAppList.forEach(function(app) {
      if ($('#game-' + app.id).length === 0) {
        // double clicking the button will cause multiple box arts to appear.
        // to mitigate this we ensure we don't add a duplicate.
        // This isn't perfect: there's lots of RTTs before the logic prevents anything
        var gameCard = document.createElement('div')
        gameCard.id = 'game-' + app.id
        gameCard.className = 'game-container mdl-card mdl-shadow--4dp'
        gameCard.setAttribute('role', 'link')
        gameCard.tabIndex = 0
        gameCard.title = app.title

        gameCard.innerHTML = `<div class="game-title">${app.title}</div>`

        gameCard.addEventListener('click', e => {
          startGame(host, app.id)
        })
        gameCard.addEventListener('mouseover', e => {
          gameCard.focus();
        });
        document.querySelector('#game-grid').appendChild(gameCard);
        // apply CSS stylization to indicate whether the app is active
        stylizeBoxArt(host, app.id);
        if (focusSetOnFirstGame === false) {
          focusSetOnFirstGame = true;
          navigateGameCards(1); //Select the first game in the app list
          Navigation.change(Views.Apps); //Put the focus on the app list
		}
      }
      var img = new Image();
      host.getBoxArt(app.id).then(function(resolvedPromise) {
        img.src = resolvedPromise;
      }, function(failedPromise) {
        console.log('%c[index.js, showApps]', 'color: green;', 'Error! Failed to retrieve box art for app ID: ' + app.id + '. Returned value was: ' + failedPromise, '\n Host object:', host, host.toString());
        img.src = 'static/res/placeholder_error.svg'
      });
      img.onload = e => img.classList.add('fade-in');
      $(gameCard).append(img);
    });
  }, function(failedAppList) {
    $('#naclSpinner').hide();
    var img = new Image();
    img.src = 'static/res/applist_error.svg'
    $("#game-grid").html(img)
    snackbarLog('Unable to retrieve your games')
    console.error('%c[index.js, showApps]', 'Failed to get applist from host: ' + host.hostname, '\n Host object:', host, host.toString());
  });

  showAppsMode();
}

// set the layout to the initial mode you see when you open moonlight
function showHostsAndSettingsMode() {
  console.log('%c[index.js]', 'color: green;', 'Entering "Show apps and hosts" mode');
  $("#main-navigation").show();
  $(".nav-menu-parent").show();
  $("#externalAudioBtn").show();
  $("#main-content").children().not("#listener, #loadingSpinner, #naclSpinner").show();
  $('#game-grid').hide();
  $('#backIcon').hide();
  $('#quitCurrentApp').hide();
  $("#main-content").removeClass("fullscreen");
  $("#listener").removeClass("fullscreen");
  Navigation.start();
  Navigation.pop();

  startPollingHosts();
}

function showAppsMode() {
  console.log('%c[index.js]', 'color: green;', 'Entering "Show apps" mode');
  $('#backIcon').show();
  $("#main-navigation").show();
  $("#main-content").children().not("#listener, #loadingSpinner, #naclSpinner").show();
  $("#streamSettings").hide();
  $(".nav-menu-parent").hide();
  $("#externalAudioBtn").hide();
  $("#host-grid").hide();
  $("#settings").hide();
  $("#main-content").removeClass("fullscreen");
  $("#listener").removeClass("fullscreen");
  $('#loadingSpinner').css('display', 'none');
  $('body').css('backgroundColor', '#282C38');
  $('#nacl_module').css('display', 'none');

  isInGame = false;

  // FIXME: We want to eventually poll on the app screen but we can't now
  // because it slows down box art loading and we don't update the UI live
  // anyway.
  stopPollingHosts();
  Navigation.start();
}


// start the given appID.  if another app is running, offer to quit it.
// if the given app is already running, just resume it.
function startGame(host, appID) {
  if (!host || !host.paired) {
    console.error('%c[index.js, startGame]', 'color: green;', 'Attempted to start a game, but `host` did not initialize properly. Host object: ', host);
    return;
  }

  // refresh the server info, because the user might have quit the game.
  host.refreshServerInfo().then(function(ret) {
    host.getAppById(appID).then(function(appToStart) {

      if (host.currentGame != 0 && host.currentGame != appID) {
        host.getAppById(host.currentGame).then(function(currentApp) {
          var quitAppDialog = document.querySelector('#quitAppDialog');
          document.getElementById('quitAppDialogText').innerHTML =
            currentApp.title + ' is already running. Would you like to quit ' +
            currentApp.title + '?';
          quitAppDialog.showModal();
          Navigation.push(Views.CloseAppDialog);
          $('#cancelQuitApp').off('click');
          $('#cancelQuitApp').on('click', function() {
            quitAppDialog.close();
            Navigation.pop();
            console.log('[index.js, startGame]', 'color: green;', 'Closing app dialog, and returning');
          });
          $('#continueQuitApp').off('click');
          $('#continueQuitApp').on('click', function() {
            console.log('[index.js, startGame]', 'color: green;', 'Stopping game, and closing app dialog, and returning');
            stopGame(host, function() {
              // please oh please don't infinite loop with recursion
              startGame(host, appID);
            });
            quitAppDialog.close();
            Navigation.pop();
          });

          return;
        }, function(failedCurrentApp) {
          console.error('[index.js, startGame]', 'color: green;', 'Failed to get the current running app from host! Returned error was:' + failedCurrentApp, '\n Host object:', host, host.toString());
          return;
        });
        return;
      }

      var frameRate = $('#selectFramerate').data('value').toString();
      var codecVideo = $('#selectCodecVideo').data('value').toString();
      var optimize = $("#optimizeGamesSwitch").parent().hasClass('is-checked') ? 1 : 0;
      var streamWidth = $('#selectResolution').data('value').split(':')[0];
      var streamHeight = $('#selectResolution').data('value').split(':')[1];
      // we told the user it was in Mbps. We're dirty liars and use Kbps behind their back.
      var bitrate = parseInt($("#bitrateSlider").val()) * 1000;
      const framePacingEnabled = $('#framePacingSwitch').parent().hasClass('is-checked') ? 1 : 0;
      const audioSyncEnabled = $('#audioSyncSwitch').parent().hasClass('is-checked') ? 1 : 0;
      const hdrEnabled = $('#hdrSwitch').parent().hasClass('is-checked') ? 1 : 0;
      console.log('%c[index.js, startGame]', 'color:green;',
                  'startRequest:' + host.address +
                  ":" + streamWidth +
                  ":" + streamHeight +
                  ":" + frameRate +
                  ":" + bitrate +
                  ":" + optimize +
                  ":" + framePacingEnabled,
                  ":" + audioSyncEnabled,
                  ":" + hdrEnabled,
                  ":" + codecVideo);

      var rikey = generateRemoteInputKey();
      var rikeyid = generateRemoteInputKeyId();
      var gamepadMask = getConnectedGamepadMask();

      $('#loadingMessage').text('Starting ' + appToStart.title + '...');
      playGameMode();

      if (host.currentGame == appID) { // if user wants to launch the already-running app, then we resume it.
        return host.resumeApp(
          rikey, rikeyid, 0x030002 // Surround channel mask << 16 | Surround channel count
        ).then(function(launchResult) {
          $xml = $($.parseXML(launchResult.toString()));
          $root = $xml.find('root');

          if ($root.attr('status_code') != 200) {
            snackbarLog('Error ' + $root.attr('status_code') + ': ' + $root.attr('status_message'));
            showApps(host);
            return;
          }

          sendMessage('startRequest', [
            host.address,
            streamWidth,
            streamHeight,
            frameRate,
            bitrate.toString(),
            rikey,
            rikeyid.toString(),
            host.appVersion,
            "",
            $root.find('sessionUrl0').text().trim(),
            framePacingEnabled,
            audioSyncEnabled,
            hdrEnabled,
            codecVideo,
            host.serverCodecSupportMode		 
          ]);
        }, function(failedResumeApp) {
          console.error('%c[index.js, startGame]', 'color:green;', 'Failed to resume the app! Returned error was' + failedResumeApp);
          showApps(host);
          return;
        });
      }

      var remote_audio_enabled = $("#remoteAudioEnabledSwitch").parent().hasClass('is-checked') ? 1 : 0;

      host.launchApp(appID,
        streamWidth + "x" + streamHeight + "x" + frameRate,
        optimize, // DON'T Allow GFE (0) to optimize game settings, or ALLOW (1) to optimize game settings
        rikey, rikeyid,
        remote_audio_enabled, // Play audio locally too?
        0x030002, // Surround channel mask << 16 | Surround channel count
        gamepadMask
      ).then(function(launchResult) {
        $xml = $($.parseXML(launchResult.toString()));
        $root = $xml.find('root');

        if ($root.attr('status_code') != 200) {
          snackbarLog('Error ' + $root.attr('status_code') + ': ' + $root.attr('status_message'));
          showApps(host);
          return;
        }

        sendMessage('startRequest', [
          host.address,
          streamWidth,
          streamHeight,
          frameRate,
          bitrate.toString(),
          rikey,
          rikeyid.toString(),
          host.appVersion,
          "",
		      $root.find('sessionUrl0').text().trim(),
          framePacingEnabled,
          audioSyncEnabled,
          hdrEnabled,
          codecVideo,
          host.serverCodecSupportMode			  
        ]);
      }, function(failedLaunchApp) {
        console.error('%c[index.js, launchApp]', 'color: green;', 'Failed to launch app width id: ' + appID + '\nReturned error was: ' + failedLaunchApp);
        showApps(host);
        return;
      });

    });
  });
}

function playGameMode() {
  console.log('%c[index.js, playGameMode]', 'color:green;', 'Entering play game mode');
  isInGame = true;

  $("#main-navigation").hide();
  $("#main-content").children().not("#listener, #loadingSpinner").hide();
  $("#main-content").addClass("fullscreen");
  $("#listener").addClass("fullscreen");

  fullscreenNaclModule();
  $('#loadingSpinner').css('display', 'inline-block');
  Navigation.stop();
}

// Maximize the size of the nacl module by scaling and resizing appropriately
function fullscreenNaclModule() {
  var streamWidth = $('#selectResolution').data('value').split(':')[0];
  var streamHeight = $('#selectResolution').data('value').split(':')[1];
  var screenWidth = window.innerWidth;
  var screenHeight = window.innerHeight;

  var xRatio = screenWidth / streamWidth;
  var yRatio = screenHeight / streamHeight;

  var zoom = Math.min(xRatio, yRatio);

  var module = $("#nacl_module")[0];
  module.width = zoom * streamWidth;
  module.height = zoom * streamHeight;
  module.style.paddingTop = ((screenHeight - module.height) / 2) + "px";
  module.focus();
  module.dispatchEvent(new Event('mousedown'));
}

function stopGameWithConfirmation() {
  if (api.currentGame === 0) {
    snackbarLog('Nothing was running');
  } else {
    api.getAppById(api.currentGame).then(function(currentGame) {
      var quitAppDialog = document.querySelector('#quitAppDialog');
      document.getElementById('quitAppDialogText').innerHTML =
        ' Are you sure you want to quit ' +
        currentGame.title + '?  All unsaved data will be lost.';
      quitAppDialog.showModal();
      Navigation.push(Views.CloseAppDialog);
      $('#cancelQuitApp').off('click');
      $('#cancelQuitApp').on('click', function() {
        console.log('%c[index.js, stopGameWithConfirmation]', 'color:green;', 'Closing app dialog, and returning');
        quitAppDialog.close();
        Navigation.pop();
      });
      $('#continueQuitApp').off('click');
      $('#continueQuitApp').on('click', function() {
        console.log('%c[index.js, stopGameWithConfirmation]', 'color:green;', 'Stopping game, and closing app dialog, and returning');
        stopGame(api);
        quitAppDialog.close();
        Navigation.pop();
      });

    });
  }
}

function stopGame(host, callbackFunction) {
  isInGame = false;

  if (!host.paired) {
    return;
  }

  host.refreshServerInfo().then(function(ret) {
    host.getAppById(host.currentGame).then(function(runningApp) {
      if (!runningApp) {
        snackbarLog('Nothing was running');
        return;
      }
      var appName = runningApp.title;
      snackbarLog('Stopping ' + appName);
      host.quitApp().then(function(ret2) {
        host.refreshServerInfo().then(function(ret3) { // refresh to show no app is currently running.
          showApps(host);
          if (typeof(callbackFunction) === "function") callbackFunction();
        }, function(failedRefreshInfo2) {
          console.error('%c[index.js, stopGame]', 'color:green;', 'Failed to refresh server info! Returned error was:' + failedRefreshInfo + ' and failed server was:', host, host.toString());
        });
      }, function(failedQuitApp) {
        console.error('%c[index.js, stopGame]', 'color:green;', 'Failed to quit app! Returned error was:' + failedQuitApp);
      });
    }, function(failedGetApp) {
      console.error('%c[index.js, stopGame]', 'color:green;', 'Failed to get app ID! Returned error was:' + failedRefreshInfo);
    });
  }, function(failedRefreshInfo) {
    console.error('%c[index.js, stopGame]', 'color:green;', 'Failed to refresh server info! Returned error was:' + failedRefreshInfo);
  });
}

let indexedDB = null;
const dbVersion = 1.0;
let db = null;
const dbName = 'GameStreamingDB';
const storeName = 'GameStreamingStore';

// Based on example from
// https://hacks.mozilla.org/2012/02/storing-images-and-files-in-indexeddb/
function createObjectStore(dataBase) {
  if (!dataBase.objectStoreNames.contains(storeName)) {
    dataBase.createObjectStore(storeName);
  }
}

function openIndexDB(callback) {
  if (db) {
    // Database already opened
    callback();
    return;
  }

  console.log('Opening IndexDB');

  if (!indexedDB) {
    indexedDB = self.indexedDB;
  }

  // Create/open database
  const request = indexedDB.open(dbName, dbVersion);

  request.onerror = function(event) {
    console.error('Error creating/accessing IndexedDB database', event);
  };

  request.onsuccess = function(event) {
    console.log('Success creating/accessing IndexedDB database');
    db = request.result;

    db.onerror = function(event) {
      console.error('Error creating/accessing IndexedDB database', event);
    };

    callback();
  };

  request.onupgradeneeded = function(event) {
    createObjectStore(event.target.result);
  };
}

function callCb(key, value, callbackFunction) {
  let obj = {};
  obj[key] = value;
  callbackFunction(obj);
}

function getData(key, callbackFunction) {
  let cb = function() {
    try {
      const transaction = db.transaction(storeName, 'readonly');
      const readRequest = transaction.objectStore(storeName).get(key);

      // Retrieve the data that was stored
      readRequest.onsuccess = function(event) {
        console.log('Read data from the DB key: ' +
                    key + ' value: '+ readRequest.result);
        let value = null;
        if (readRequest.result) {
          value = JSON.parse(readRequest.result);
        }

        callCb(key, value, callbackFunction);
      };

      transaction.onerror = function(e) {
        console.error('Error reading data at key: "' + key +
                      '" from IndexDB: ' + e);
        callCb(key, value, callbackFunction);
      };
    } catch (e) {
      console.log('getData: caught exception while reading key:' + key);
      console.error(e);

      callCb(key, value, callbackFunction);
    }
  };

  if (db) {
    cb();
  } else {
    openIndexDB(cb);
  }
}

function storeData(key, data, callbackFunction) {
  let cb = function () {
    try {
      const transaction = db.transaction(storeName, 'readwrite'); //open a transaction to the database
      const put = transaction.objectStore(storeName).put(
        JSON.stringify(data), key);

      transaction.oncomplete = function (e) {
        console.log('Data at key: ' + key + ' stored as: ' + JSON.stringify(data));
        if (callbackFunction) {
          callbackFunction();
        }
      };

      transaction.onerror = function (e) {
        console.error('Error storing data in IndexDB: ' + e);
      };
    } catch (e) {
      console.log('storeData: caught exception while storing key:' + key);
      console.error(e);
    }
  };

  if (db) {
    cb();
  } else {
    openIndexDB(cb);
  }
}

function saveResolution() {
  var chosenResolution = $(this).data('value');
  $('#selectResolution').text($(this).text()).data('value', chosenResolution);
  storeData('resolution', chosenResolution, null);
  updateDefaultBitrate();
  Navigation.pop();
}

function saveOptimize() {
  // MaterialDesignLight uses the mouseup trigger, so we give it some time to change the class name before
  // checking the new state
  setTimeout(function() {
    var chosenOptimize = $("#optimizeGamesSwitch").parent().hasClass('is-checked');
    console.log('%c[index.js, saveOptimize]', 'color: green;', 'Saving optimize state : ' + chosenOptimize);
    storeData('optimize', chosenOptimize, null);
  }, 100);
}

function saveFramePacing() {
  setTimeout(function() {
    const chosenFramePacing = $("#framePacingSwitch").parent().hasClass('is-checked');
    console.log('%c[index.js, saveFramePacing]', 'color: green;', 'Saving frame pacing state : ' + chosenFramePacing);
    storeData('framePacing', chosenFramePacing, null);
  }, 100);
}

function saveHdr() {
  setTimeout(function() {
    const chosenHDR = $("#hdrSwitch").parent().hasClass('is-checked');
    console.log('%c[index.js, saveHDR]', 'color: green;', 'Saving HDR state : ' + chosenHDR);
    storeData('HDR', chosenHDR, null);
  }, 100);
}

function saveCodecVideo() {
  var chosenCodecVideo = $(this).data('value');
  $('#selectCodecVideo').text($(this).text()).data('value', chosenCodecVideo);
  storeData('codecVideo', chosenCodecVideo, null);
  Navigation.pop();
}

function saveAudioSync() {
  setTimeout(function() {
    const chosenAudioSync = $("#audioSyncSwitch").parent().hasClass('is-checked');
    console.log('%c[index.js, saveAudioSync]', 'color: green;', 'Saving audio sync state : ' + chosenAudioSync);
    storeData('audioSync', chosenAudioSync, null);
  }, 100);
}

function saveFramerate() {
  var chosenFramerate = $(this).data('value');
  $('#selectFramerate').text($(this).text()).data('value', chosenFramerate);
  storeData('frameRate', chosenFramerate, null);
  updateDefaultBitrate();
  Navigation.pop();
}

function saveHosts() {
  storeData('hosts', hosts, null);
}

function saveBitrate() {
  storeData('bitrate', $('#bitrateSlider').val(), null);
}

function saveRemoteAudio() {
  setTimeout(function() {
    var remoteAudioState = $("#remoteAudioEnabledSwitch").parent().hasClass('is-checked');
    console.log('%c[index.js, saveRemoteAudio]', 'color: green;', 'Saving remote audio state : ' + remoteAudioState);
    storeData('remoteAudio', remoteAudioState, null);
  }, 100);
}

function updateDefaultBitrate() {
  var res = $('#selectResolution').data('value');
  var frameRate = $('#selectFramerate').data('value').toString();

  // These quality presets include video resolution like 480p, 720p, 1080p, 1440p, 2160p (4K) and video frame rate like 30 FPS and 60 FPS
  if (res === "858:480") {
    if (frameRate === "30") { // 480p, 30 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('2');
    } else { // 480p, 60 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('4');
    }
  } else if (res === "1280:720") {
    if (frameRate === "30") { // 720p, 30 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('5');
    } else { // 720p, 60 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('10');
    }
  } else if (res === "1920:1080") {
    if (frameRate === "30") { // 1080p, 30 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('10');
    } else { // 1080p, 60 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('20');
    }
  } else if (res === "2560:1440") {
    if (frameRate === "30") { // 1440p, 30 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('20');
    } else { // 1440p, 60 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('40');
    }
  } else if (res === "3840:2160") {
    if (frameRate === "30") { // 2160p (4K), 30 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('40');
    } else { // 2160p (4K), 60 FPS
      $('#bitrateSlider')[0].MaterialSlider.change('80');
    }
  } else { // unrecognized option. In case someone screws with the JS to add custom resolutions
    $('#bitrateSlider')[0].MaterialSlider.change('10');
  }

  updateBitrateField();
  saveBitrate();
}

function initSamsungKeys() {
  console.log('initializing keys');

  var handler = {
    initRemoteController: true,
    buttonsToRegister: [
      // https://developer.samsung.com/tv/develop/guides/user-interaction/keyboardime
      'ColorF0Red',    // F1
      'ColorF1Green',  // F2
      'ColorF2Yellow', // F3
      'ColorF3Blue',   // F4
      // Not working...
      //'SmartHub',      // F5
      'Source',        // F6
      'ChannelList',   // F7
      'ChannelDown',   // F11
      'ChannelUp',     // F12
    ],
    onKeydownListener: remoteControllerHandler
  };

  console.log('Initializing SamsungTV platform');
  platformOnLoad(handler);
}

function loadUserData() {
  console.log('loading stored user data');
  openIndexDB(loadUserDataCb);
}

function loadUserDataCb() {
  console.log('load stored VideoCodec prefs');
  getData('codecVideo', function (previousValue) {
    if (previousValue.codecVideo != null) {
      $('.codecVideoMenu li').each(function () {
        if ($(this).data('value') === previousValue.codecVideo) {
          $('#selectCodecVideo').text($(this).text()).data('value', previousValue.codecVideo);
        }
      });
    }
  });

  console.log('load stored resolution prefs');
  getData('resolution', function (previousValue) {
    if (previousValue.resolution != null) {
      $('.resolutionMenu li').each(function () {
        if ($(this).data('value') === previousValue.resolution) {
          $('#selectResolution').text($(this).text()).data('value', previousValue.resolution);
        }
      });
    }
  });

  console.log('Load stored remote audio prefs');
  getData('remoteAudio', function (previousValue) {
    if (previousValue.remoteAudio == null) {
      document.querySelector('#externalAudioBtn').MaterialIconToggle.uncheck();
    } else if (previousValue.remoteAudio == false) {
      document.querySelector('#externalAudioBtn').MaterialIconToggle.uncheck();
    } else {
      document.querySelector('#externalAudioBtn').MaterialIconToggle.check();
    }
  });

  console.log('load stored framerate prefs');
  getData('frameRate', function(previousValue) {
    if (previousValue.frameRate != null) {
      $('.framerateMenu li').each(function() {
        if ($(this).data('value') === previousValue.frameRate) {
          $('#selectFramerate').text($(this).text()).data('value', previousValue.frameRate);
        }
      });
    }
  });

  console.log('load stored optimization prefs');
  getData('optimize', function (previousValue) {
    if (previousValue.optimize == null) {
      document.querySelector('#optimizeGamesBtn').MaterialIconToggle.check();
    } else if (previousValue.optimize == false) {
      document.querySelector('#optimizeGamesBtn').MaterialIconToggle.uncheck();
    } else {
      document.querySelector('#optimizeGamesBtn').MaterialIconToggle.check();
    }
  });

  console.log('load stored framePacing prefs');
  getData('framePacing', function(previousValue) {
    if (previousValue.framePacing == null) {
      document.querySelector('#framePacingBtn').MaterialIconToggle.check();
    } else if (previousValue.framePacing == false) {
      document.querySelector('#framePacingBtn').MaterialIconToggle.uncheck();
    } else {
      document.querySelector('#framePacingBtn').MaterialIconToggle.check();
    }
  });

  console.log('load stored HDR prefs');
  getData('HDR', function(previousValue) {
    if (previousValue.HDR == null) {
      document.querySelector('#hdrBtn').MaterialIconToggle.check();
    } else if (previousValue.HDR == false) {
      document.querySelector('#hdrBtn').MaterialIconToggle.uncheck();
    } else {
      document.querySelector('#hdrBtn').MaterialIconToggle.check();
    }
  });

  console.log('load stored audioSync prefs');
  getData('audioSync', function(previousValue) {
    if (previousValue.audioSync == null) {
      document.querySelector('#audioSyncBtn').MaterialIconToggle.check();
    } else if (previousValue.audioSync == false) {
      document.querySelector('#audioSyncBtn').MaterialIconToggle.uncheck();
    } else {
      document.querySelector('#audioSyncBtn').MaterialIconToggle.check();
    }
  });

  console.log('load stored bitrate prefs');
  getData('bitrate', function(previousValue) {
    $('#bitrateSlider')[0].MaterialSlider.change(previousValue.bitrate != null ? previousValue.bitrate : '20');
    updateBitrateField();
  });
}

function loadHTTPCerts() {
  openIndexDB(loadHTTPCertsCb);
}

function loadHTTPCertsCb() {
  console.log('load the HTTP cert and unique ID if we have one.');
  getData('cert', function(savedCert) {
    if (savedCert.cert != null) { // we have a saved cert
      pairingCert = savedCert.cert;
    }

    getData('uniqueid', function(savedUniqueid) {
      // See comment on myUniqueid
      /*if (savedUniqueid.uniqueid != null) { // we have a saved uniqueid
        myUniqueid = savedUniqueid.uniqueid;
      } else {
        myUniqueid = uniqueid();
        storeData('uniqueid', myUniqueid, null);
      }*/

      if (!pairingCert) { // we couldn't load a cert. Make one.
        console.warn('%c[index.js, moduleDidLoad]', 'color: green;', 'Failed to load local cert. Generating new one');
        sendMessage('makeCert', []).then(function(cert) {
          storeData('cert', cert, null);
          pairingCert = cert;
          console.info('%c[index.js, moduleDidLoad]', 'color: green;', 'Generated new cert:', cert);
        }, function(failedCert) {
          console.error('%c[index.js, moduleDidLoad]', 'color: green;', 'Failed to generate new cert! Returned error was: \n', failedCert);
        }).then(function(ret) {
          sendMessage('httpInit', [pairingCert.cert, pairingCert.privateKey, myUniqueid]).then(function(ret) {
            restoreUiAfterNaClLoad();
          }, function(failedInit) {
            console.error('%c[index.js, moduleDidLoad]', 'color: green;', 'Failed httpInit! Returned error was: ', failedInit);
          });
        });
      } else {
        sendMessage('httpInit', [pairingCert.cert, pairingCert.privateKey, myUniqueid]).then(function(ret) {
          restoreUiAfterNaClLoad();
        }, function(failedInit) {
          console.error('%c[index.js, moduleDidLoad]', 'color: green;', 'Failed httpInit! Returned error was: ', failedInit);
        });
      }

      // load previously connected hosts, which have been killed into an object, and revive them back into a class
      getData('hosts', function(previousValue) {
        hosts = previousValue.hosts != null ? previousValue.hosts : {};
        for (var hostUID in hosts) { // programmatically add each new host.
        var revivedHost = new NvHTTP(hosts[hostUID].address, myUniqueid, hosts[hostUID].userEnteredAddress, hosts[hostUID].macAddress);
          revivedHost.serverUid = hosts[hostUID].serverUid;
          revivedHost.externalIP = hosts[hostUID].externalIP;
          revivedHost.hostname = hosts[hostUID].hostname;
          revivedHost.ppkstr = hosts[hostUID].ppkstr;
          addHostToGrid(revivedHost);
        }
        startPollingHosts();
        console.log('%c[index.js]', 'color: green;', 'Loaded previously connected hosts');
      });
    });
  });
}

function onWindowLoad() {
  console.log('%c[index.js]', 'color: green;', 'Moonlight\'s main window loaded');
  // don't show the game selection div
  $('#gameSelection').css('display', 'none');

  initSamsungKeys();
  loadWindowState();
  loadProductInfos();
  loadUserData();

  var videoElement = document.getElementById('nacl_module'); //FIXME: workaround to send escape key to host
  videoElement.addEventListener('keydown', function (event) {
    if (event.key === 'XF86Back') {
      if (isInGame) {
        sendEscapeToHost();
        videoElement.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true, view: window, clientX: 0, clientY: 0 }));
      }
    }
  });
}

window.onload = onWindowLoad;

// Required on TizenTV, to get gamepad events.
window.addEventListener('gamepadconnected', function (event) {
  const connectedGamepad = event.gamepad;
  console.log('%c[index.js, gamepadconnected] gamepad connected: ', 'color: green;', connectedGamepad);

  if (connectedGamepad.vibrationActuator) { // Check if the gamepad supports rumble
    console.log('Gamepad supports vibration.');
  } else {
    console.log('Gamepad does not support vibration.');
  }
});

window.addEventListener('gamepaddisconnected', function (event) {
  console.log('%c[index.js, gamepaddisconnected] gamepad disconnected: ' +
    JSON.stringify(event.gamepad),
    event.gamepad);
});