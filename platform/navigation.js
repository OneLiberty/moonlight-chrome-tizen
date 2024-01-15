const hoveredClassName = 'hovered';

function unmarkElementById(id) {
  unmarkElement(document.getElementById(id));
}

function unmarkElement(element) {
  if (element) {
    element.classList.remove(hoveredClassName);
    element.dispatchEvent(new Event('mouseleave'));
  }
}

function markElementById(id) {
  markElement(document.getElementById(id));
}

function markElement(element) {
  if (element) {
    element.classList.add(hoveredClassName);
    element.dispatchEvent(new Event('mouseenter'));
  }
}

function mark(value) {
  if (typeof value === 'string') {
    markElementById(value);
  } else if (typeof value === 'object') {
    markElement(value);
  }
}

function unmark(value) {
  if (typeof value === 'string') {
    unmarkElementById(value);
  } else if (typeof value === 'object') {
    unmarkElement(value);
  }
}

function isPopupActive(id) {
  return document
      .getElementById(id)
      .parentNode
      .children[1]
      .classList
      .contains('is-visible');
}

class ListView {
	  constructor(func) {
	    this.index = 0;
	    this.func = func;
	  }

	  prev() {
	    if (this.index > 0) {
	      unmark(this.func()[this.index]);
	      --this.index;
	      mark(this.func()[this.index]);
	    }

	    return this.func()[this.index];
	  }

	  next() {
	    const array = this.func();
	    if (this.index < array.length - 1) {
	      unmark(array[this.index]);
	      ++this.index;
	      mark(array[this.index]);
	    }

	    return array[this.index];
	  }

	  current() {
	    return this.func()[this.index];
	  }
	  navigateDigits(direction) {
		    const currentElement = this.func()[this.index];
		    const currentId = currentElement.id;

		    // Check if the current element is a digit select element
		    if (currentId && currentId.startsWith('ipPart')) {
		      const digitIndex = parseInt(currentId.slice(-1), 10);
		      const nextDigitIndex = digitIndex + direction;

		      if (nextDigitIndex >= 1 && nextDigitIndex <= 3) {
		        // Update the id to the next digit and focus on it
		        const nextDigitId = `${currentId.slice(0, -1)}${nextDigitIndex}`;
		        unmark(currentElement);
		        mark(document.getElementById(nextDigitId));
		        return nextDigitId;
		      }
		    }

		    return currentId;
		  }
	}


const Views = {
  Hosts: {
    view: new ListView(() => document.getElementById('host-grid').children),
    up: function() {
      Navigation.change(Views.HostsNav);
    },
    left: function() {
      this.view.prev();
    },
    right: function() {
      this.view.next();
    },
    accept: function() {
      const element = this.view.current();
      if (element.id === 'addHostCell') {
        element.click();
      } else {
        element.children[0].click();
      }
    },
    back: function() {
      showTerminateMoonlightDialog(); /* Show the dialog and push the view */
    },
    startBtn: function() {
      const element = this.view.current();
      if (element.id != 'addHostCell') {
          element.children[1].click();
        }
    },
    selectBtn: function() { //for future use
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  HostsNav: {
    view: new ListView(() => [
      'selectResolution',
      'selectFramerate',
      'bitrateField',
      'externalAudioBtn',
      'optimizeGamesBtn',
      'framePacingBtn',
      'audioSyncBtn',
      'hdrBtn']),
    left: function() {
      this.view.prev();
    },
    right: function() {
      this.view.next();
    },
    down: function() {
      Navigation.change(Views.Hosts);
    },
    accept: function() {
      document.getElementById(this.view.current()).click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  AddHostDialog: {
	  view: new ListView(() => {
	  	if (document.getElementById('manualInputToggle').checked) {
	  		return ['manualInputToggle', 'manualIPAddress', 'continueAddHost', 'cancelAddHost'];
	  	  } else {
	  	  	return ['manualInputToggle','ipPart1', 'ipPart2', 'ipPart3', 'ipPart4', 'continueAddHost', 'cancelAddHost'];
          }
	  }),
	  left: function() {
	      document.getElementById(this.view.prev()).focus();
	  },
	  right: function() {
		  document.getElementById(this.view.next()).focus();
	  },
	  up: function () {
	    const currentId = this.view.current();
	    if (currentId.startsWith('ipPart')) {
	      const digitElement = document.getElementById(currentId);
	      const currentValue = parseInt(digitElement.value, 10);
	      if (currentValue < 255) {
	        digitElement.value = currentValue + 1;
	      }
	    }
	  },
	  down: function () {
	    const currentId = this.view.current();
	    if (currentId.startsWith('ipPart')) {
	      const digitElement = document.getElementById(currentId);
	      const currentValue = parseInt(digitElement.value, 10);
	      if (currentValue > 0) {
	        digitElement.value = currentValue - 1;
	      }
	    }
	  },
	  accept: function () {
	    document.getElementById(this.view.current()).click();
	  },
	  selectBtn: function () {
		document.getElementById('manualInputToggle').click();
	  },
	  back: function () {
	    document.getElementById('cancelAddHost').click();
	  },
	  enter: function () {
	    mark(this.view.current());
	  },
	  leave: function () {
	    unmark(this.view.current());
	  },
	},
  DeleteHostDialog: {
    view: new ListView(() => [
      'continueDeleteHost',
      'cancelDeleteHost']),
    left: function() {
      this.view.prev();
      document.getElementById(this.view.current()).focus();
    },
    right: function() {
      this.view.next();
      document.getElementById(this.view.current()).focus();
    },
    down: function() {
        document.getElementById('continueDeleteHost').click();
    },
    accept: function() {
      document.getElementById(this.view.current()).click();
    },
    back: function() {
      document.getElementById('cancelDeleteHost').click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  SelectResolutionMenu: {
    isActive: () => isPopupActive('resolutionMenu'),
    view: new ListView(
        () => document
            .getElementById('resolutionMenu')
            .parentNode
            .children[1]
            .children[1]
            .children),
    up: function() {
      this.view.prev();
    },
    down: function() {
      this.view.next();
    },
    accept: function() {
      this.view.current().click();
    },
    back: function() {
      document.getElementById('selectResolution').click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  SelectFramerateMenu: {
    isActive: () => isPopupActive('framerateMenu'),
    view: new ListView(
        () => document
            .getElementById('framerateMenu')
            .parentNode
            .children[1]
            .children[1]
            .children),
    up: function() {
      this.view.prev();
    },
    down: function() {
      this.view.next();
    },
    accept: function() {
      this.view.current().click();
    },
    back: function() {
      document.getElementById('selectFramerate').click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  SelectBitrateMenu: {
    isActive: () => isPopupActive('bandwidthMenu'),
    left: function() {
      bitrateSlider.stepDown();
      bitrateSlider.dispatchEvent(new Event('input'));
    },
    right: function() {
      bitrateSlider.stepUp();
      bitrateSlider.dispatchEvent(new Event('input'));
    },
    accept: function() {
      document.getElementById('bandwidthMenu').click();
    },
    back: function() {
      document.getElementById('bandwidthMenu').click();
    },
    enter: function() {},
    leave: function() {},
  },
  PairingDialog: {
    view: new ListView(() => ['cancelPairingDialog']),
    accept: function() {
      document.getElementById(this.view.current()).click();
    },
    back: function() {
      document.getElementById('cancelPairingDialog').click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    }
  },
  Apps: {
    view: new ListView(() => document.getElementById('game-grid').children),
    up: function() {
      Navigation.change(Views.AppsNav);
    },
    left: function() {
      this.view.prev();
    },
    right: function() {
      this.view.next();
    },
    accept: function() {
      this.view.current().click();
    },
    back: function() {
      document.getElementById('backIcon').click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  AppsNav: {
    view: new ListView(() => [
      'backIcon',
      'quitCurrentApp']),
    down: function() {
      Navigation.change(Views.Apps);
    },
    left: function() {
      this.view.prev();
      document.getElementById(this.view.current()).focus();
    },
    right: function() {
      this.view.next();
      document.getElementById(this.view.current()).focus();
    },
    accept: function() {
      document.getElementById(this.view.current()).click();
    },
    back: function() {
      document.getElementById('backIcon').click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  CloseAppDialog: {
    isActive: () => isDialogActive('quitAppDialog'),
    view: new ListView(() => [
      'continueQuitApp',
      'cancelQuitApp']),
    down: function() {
      Navigation.change(Views.Apps);
    },
    left: function() {
      this.view.prev();
      document.getElementById(this.view.current()).focus();
    },
    right: function() {
      this.view.next();
      document.getElementById(this.view.current()).focus();
    },
    accept: function() {
      document.getElementById(this.view.current()).click();
    },
    back: function() {
      document.getElementById('cancelQuitApp').click();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
  TerminateMoonlightDialog: {
    isActive: () => isDialogActive('TerminateMoonlightDialog'),
    view: new ListView(() => [
      'exitTerminateMoonlight',
      'cancelTerminateMoonlight']),
    left: function() {
      this.view.prev();
      document.getElementById(this.view.current()).focus();
    },
    right: function() {
      this.view.next();
      document.getElementById(this.view.current()).focus();
    },
    down: function() {
      document.getElementById('exitTerminateMoonlight').focus();
    },
    accept: function() {
      document.getElementById(this.view.current()).click();
    },
    back: function() {
      document.getElementById('cancelTerminateMoonlight').focus();
    },
    enter: function() {
      mark(this.view.current());
    },
    leave: function() {
      unmark(this.view.current());
    },
  },
};

const Navigation = (function() {
  let hasFocus = false;

  function loseFocus() {
    if (hasFocus) {
      hasFocus = false;
      Stack.get().leave();
    }
  }

  function focus() {
    if (!hasFocus) {
      hasFocus = true;
      Stack.get().enter();
    }
  }
  function runOp(name) {
    return () => {
      if (!State.isRunning()) {
        return;
      }

      if (!hasFocus) {
        focus();
        return;
      }
      const view = Stack.get();
      if (view[name]) {
        view[name]();
      }
    };
  }

  const Stack = (function() {
    const viewStack = [];

    function push(view) {
      if (get()) {
        get().leave();
      }
      viewStack.push(view);
      get().enter();
    }

    function change(view) {
      get().leave();
      viewStack[viewStack.length - 1] = view;
      get().enter();
    }

    function pop() {
      if (viewStack.length > 1) {
        get().leave();
        viewStack.pop();
        get().enter();
      }
    }

    function get() {
      return viewStack[viewStack.length - 1];
    }

    return {get, push, change, pop};
  })();

  const State = (function() {
    let running = false;

    function start() {
      if (!running) {
        running = true;
        window.addEventListener('mousemove', loseFocus);
      }
    }

    function stop() {
      if (running) {
        running = false;
        window.removeEventListener('mousemove', loseFocus);
      }
    }

    function isRunning() {
      return running;
    }

    return {start, stop, isRunning};
  })();

  return {
    accept: runOp('accept'),
    back: runOp('back'),
    left: runOp('left'),
    right: runOp('right'),
    up: runOp('up'),
    down: runOp('down'),
    startBtn: runOp('startBtn'),
    selectBtn: runOp('selectBtn'),
    push: Stack.push,
    change: Stack.change,
    pop: Stack.pop,
    start: State.start,
    stop: State.stop,
  };
})();
