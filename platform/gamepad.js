const Controller = (function() {
  let pollingInterval = null;

  const gamepads = {};

  class Button {
    constructor(button) {
      this.value = button.value;
      this.pressed = button.pressed;
    }
  }

  class Gamepad {
    constructor(gamepad) {
      this.buttons = gamepad.buttons.map((button) => new Button(button));
    }

    analyzeButtons(newButtons) {
      if (this.buttons.length !== newButtons.length) {
        console.error('New buttons layout does not match saved one');
        return;
      }

      for (let i = 0; i < newButtons.length; ++i) {
        if (this.buttons[i].pressed !== newButtons[i].pressed) {
          window.dispatchEvent(
              new CustomEvent('gamepadbuttonpressed',
                  {
                    detail: {
                      key: i,
                      pressed: newButtons[i].pressed,
                    },
                  }));
        }
      }

      this.buttons = newButtons.map((button) => new Button(button));
    }
  }

  function gamepadConnected(gamepad) {
    gamepads[gamepad.index] = new Gamepad(gamepad);
  }

  function gamepadDisconnected(gamepad) {
    delete gamepad[gamepad.index];
  }

  function analyzeGamepad(gamepad) {
    const index = gamepad.index;
    const pGamepad = gamepads[index];

    if (pGamepad) {
      pGamepad.analyzeButtons(gamepad.buttons);
    }
  }

  function pollGamepads() {
    const gamepads =
        navigator.getGamepads ?
          navigator.getGamepads() :
          (navigator.webkitGetGamepads ? navigator.webkitGetGamepads : []);
    for (const gamepad of gamepads) {
      if (gamepad) {
        analyzeGamepad(gamepad);
      }
    }
  }

  function startWatching() {
    if (!pollingInterval) {
      window.addEventListener('gamepadconnected', function(e) {
        gamepadConnected(e.gamepad);
      });
      window.addEventListener('gamepaddisconnected', function(e) {
        gamepadDisconnected(e.gamepad);
      });
      pollingInterval = setInterval(pollGamepads, 5);
    }
  }

  function stopWatching() {
    if (pollingInterval) {
      clearInterval(pollingInterval);
      pollingInterval = null;
    }
  }

  return {startWatching, stopWatching};
})();
