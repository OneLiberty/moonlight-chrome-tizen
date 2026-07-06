/* eslint-disable */
//
// Lightweight internationalization (i18n) layer for Moonlight Tizen.
//
// Phase 1 covers every user-facing string produced by the HTML markup and the
// JavaScript layer (menus, tooltips, dialogs, toasts, status and error
// messages). Connection-stage strings emitted by the C++/WASM module
// (ProgressMsg/DialogMsg via LiGetStageName) are intentionally out of scope
// here because translating them requires rebuilding the WASM binary.
//
// Usage:
//   - In markup, tag elements with data-i18n / data-i18n-html / data-i18n-aria
//     / data-i18n-placeholder and I18n.applyDom() fills them on load.
//   - In JS, call t('some.key', { var: value }) to get a translated string.
//   - Language preference is stored in localStorage (synchronous, so it is
//     available before the first paint and avoids a flash of English on
//     reload). Switching language just saves the choice and reloads the app.
//
const I18N_STRINGS = {
  en: {
    // Common / shared button labels
    'common.ok': 'OK',
    'common.cancel': 'Cancel',
    'common.yes': 'Yes',
    'common.no': 'No',
    'common.close': 'Close',
    'common.continue': 'Continue',

    // Navigation tooltips
    'tooltip.resolution': 'Resolution',
    'tooltip.framerate': 'Framerate',
    'tooltip.bandwidth': 'Bandwidth',
    'tooltip.videoCodec': 'Video Codec',
    'tooltip.playAudioOnHost': 'Play audio on host',
    'tooltip.optimize': 'Allow game optimizations',
    'tooltip.framePacing': 'Enable frame pacing',
    'tooltip.audioSync': 'Enable audio syncing',
    'tooltip.hdr': 'Enable HDR',
    'tooltip.language': 'Language',

    // Navigation buttons / aria labels
    'nav.quitCurrentApp': 'Quit Current App',
    'aria.hostSelection': 'Host selection',
    'aria.quitCurrentApp': 'Quit current app',
    'aria.addHost': 'Add Host',

    // Host grid / host settings
    'host.addHost': 'Add Host',
    'host.wakeWol': 'Wake PC (WOL)',
    'host.refreshBoxArt': 'Refresh box art',
    'host.remove': 'Remove {host}',
    'host.settingsTitle': 'Settings {host}',

    // Pairing dialog
    'dialog.pairing.title': 'Pairing',
    'dialog.pairing.body': 'Please enter the following PIN on the target PC: XXXX<br><br>If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.<br>Alternatively, navigate to the GeForce Experience (NVIDIA GPUs only) to enter the PIN.<br><br>This dialog will close once the pairing is complete.',
    'dialog.pairing.bodyPin': 'Please enter the following PIN on the target PC:  {pin}<br><br>If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.<br>Alternatively, navigate to the GeForce Experience (NVIDIA GPUs only) to enter the PIN.<br><br>This dialog will close once the pairing is complete.',

    // Restart dialog
    'dialog.restart.title': 'Restart Moonlight',
    'dialog.restart.body': 'After changing video codec, you should restart the application',

    // Exit dialog
    'dialog.exit.title': 'Exit Moonlight',
    'dialog.exit.body': 'Are you sure you want to exit Moonlight?',
    'dialog.exit.confirm': 'Exit',

    // Quit running app dialog
    'dialog.quitApp.title': 'Quit Running App?',
    'dialog.quitApp.placeholder': 'An app is already running. Would you like to quit it?',
    'dialog.quitApp.bodyRunning': '{app} is already running. Would you like to quit {app}?',
    'dialog.quitApp.bodyConfirm': ' Are you sure you want to quit {app}?  All unsaved data will be lost.',

    // Delete host dialog
    'dialog.deleteHost.title': 'Delete Host',
    'dialog.deleteHost.body': 'Are you sure you want to delete this host?',
    'dialog.deleteHost.bodyHost': ' Are you sure you want to delete {host}?',

    // Add host dialog
    'dialog.addHost.title': 'Add Host Manually',
    'dialog.addHost.manualToggle': 'Enter IP manually (press Select)',
    'dialog.addHost.ipPlaceholder': 'Enter Host IP address',

    // Status / progress messages
    'status.loadingPlugin': 'Loading Moonlight plugin...',
    'status.loadingApps': 'Loading apps...',
    'status.pairingSuccess': 'Pairing successful',
    'status.startingApp': 'Starting {app}...',
    'status.stoppingApp': 'Stopping {app}',
    'status.wolRequest': 'Sending WOL request to {host} with mac address {mac}',

    // Error messages
    'error.certNotGenerated': 'ERROR: cert has not been generated yet. Is NaCl initialized?',
    'error.failedConnect': 'Failed to connect to {host}! Ensure Sunshine is running on your host PC or GameStream is enabled in GeForce Experience SHIELD settings.',
    'error.pairingFailed': 'Failed pairing to: {host}',
    'error.hostBusy': 'Error: {host} is busy.  Stop streaming to pair.',
    'error.pairFailedHost': 'Error: failed to pair with {host}.',
    'error.emptyGameList': 'Your game list is empty',
    'error.retrieveGames': 'Unable to retrieve your games',
    'error.nothingRunning': 'Nothing was running',
    'error.launchStatus': 'Error {code}: {message}',

    // Stream lifecycle (messages.js)
    'stream.connectionTerminated': 'Connection terminated',
    'stream.interrupted': 'Stream interrupted. Returning to menu.',
    'stream.mouseEmuOn': 'Mouse Emulation is now enabled',
    'stream.mouseEmuOff': 'Mouse Emulation is now disabled',

    // Misc
    'app.runningSuffix': ' (Running)',
  },

  'pt-BR': {
    'common.ok': 'OK',
    'common.cancel': 'Cancelar',
    'common.yes': 'Sim',
    'common.no': 'Não',
    'common.close': 'Fechar',
    'common.continue': 'Continuar',

    'tooltip.resolution': 'Resolução',
    'tooltip.framerate': 'Taxa de quadros',
    'tooltip.bandwidth': 'Largura de banda',
    'tooltip.videoCodec': 'Codec de vídeo',
    'tooltip.playAudioOnHost': 'Reproduzir áudio no host',
    'tooltip.optimize': 'Permitir otimizações de jogo',
    'tooltip.framePacing': 'Ativar ritmo de quadros',
    'tooltip.audioSync': 'Ativar sincronização de áudio',
    'tooltip.hdr': 'Ativar HDR',
    'tooltip.language': 'Idioma',

    'nav.quitCurrentApp': 'Fechar app atual',
    'aria.hostSelection': 'Seleção de host',
    'aria.quitCurrentApp': 'Fechar app atual',
    'aria.addHost': 'Adicionar host',

    'host.addHost': 'Adicionar host',
    'host.wakeWol': 'Ligar PC (WOL)',
    'host.refreshBoxArt': 'Atualizar capas',
    'host.remove': 'Remover {host}',
    'host.settingsTitle': 'Configurações {host}',

    'dialog.pairing.title': 'Emparelhamento',
    'dialog.pairing.body': 'Digite o seguinte PIN no PC de destino: XXXX<br><br>Se o PC host estiver rodando o Sunshine, acesse a interface web do Sunshine para inserir o PIN.<br>Como alternativa, acesse o GeForce Experience (apenas GPUs NVIDIA) para inserir o PIN.<br><br>Esta janela será fechada quando o emparelhamento for concluído.',
    'dialog.pairing.bodyPin': 'Digite o seguinte PIN no PC de destino:  {pin}<br><br>Se o PC host estiver rodando o Sunshine, acesse a interface web do Sunshine para inserir o PIN.<br>Como alternativa, acesse o GeForce Experience (apenas GPUs NVIDIA) para inserir o PIN.<br><br>Esta janela será fechada quando o emparelhamento for concluído.',

    'dialog.restart.title': 'Reiniciar o Moonlight',
    'dialog.restart.body': 'Após alterar o codec de vídeo, você deve reiniciar o aplicativo',

    'dialog.exit.title': 'Sair do Moonlight',
    'dialog.exit.body': 'Tem certeza de que deseja sair do Moonlight?',
    'dialog.exit.confirm': 'Sair',

    'dialog.quitApp.title': 'Fechar app em execução?',
    'dialog.quitApp.placeholder': 'Um app já está em execução. Deseja fechá-lo?',
    'dialog.quitApp.bodyRunning': '{app} já está em execução. Deseja fechar {app}?',
    'dialog.quitApp.bodyConfirm': ' Tem certeza de que deseja fechar {app}?  Todos os dados não salvos serão perdidos.',

    'dialog.deleteHost.title': 'Remover host',
    'dialog.deleteHost.body': 'Tem certeza de que deseja remover este host?',
    'dialog.deleteHost.bodyHost': ' Tem certeza de que deseja remover {host}?',

    'dialog.addHost.title': 'Adicionar host manualmente',
    'dialog.addHost.manualToggle': 'Inserir IP manualmente (pressione Select)',
    'dialog.addHost.ipPlaceholder': 'Insira o endereço IP do host',

    'status.loadingPlugin': 'Carregando o plugin do Moonlight...',
    'status.loadingApps': 'Carregando aplicativos...',
    'status.pairingSuccess': 'Emparelhamento concluído',
    'status.startingApp': 'Iniciando {app}...',
    'status.stoppingApp': 'Fechando {app}',
    'status.wolRequest': 'Enviando requisição WOL para {host} com o endereço MAC {mac}',

    'error.certNotGenerated': 'ERRO: o certificado ainda não foi gerado. O NaCl foi inicializado?',
    'error.failedConnect': 'Falha ao conectar a {host}! Verifique se o Sunshine está em execução no PC host ou se o GameStream está ativado nas configurações SHIELD do GeForce Experience.',
    'error.pairingFailed': 'Falha no emparelhamento com: {host}',
    'error.hostBusy': 'Erro: {host} está ocupado.  Pare a transmissão para emparelhar.',
    'error.pairFailedHost': 'Erro: falha ao emparelhar com {host}.',
    'error.emptyGameList': 'Sua lista de jogos está vazia',
    'error.retrieveGames': 'Não foi possível obter seus jogos',
    'error.nothingRunning': 'Nada estava em execução',
    'error.launchStatus': 'Erro {code}: {message}',

    'stream.connectionTerminated': 'Conexão encerrada',
    'stream.interrupted': 'Transmissão interrompida. Retornando ao menu.',
    'stream.mouseEmuOn': 'Emulação de mouse ativada',
    'stream.mouseEmuOff': 'Emulação de mouse desativada',

    'app.runningSuffix': ' (Em execução)',
  },

  es: {
    'common.ok': 'OK',
    'common.cancel': 'Cancelar',
    'common.yes': 'Sí',
    'common.no': 'No',
    'common.close': 'Cerrar',
    'common.continue': 'Continuar',

    'tooltip.resolution': 'Resolución',
    'tooltip.framerate': 'Fotogramas',
    'tooltip.bandwidth': 'Ancho de banda',
    'tooltip.videoCodec': 'Códec de vídeo',
    'tooltip.playAudioOnHost': 'Reproducir audio en el host',
    'tooltip.optimize': 'Permitir optimizaciones de juego',
    'tooltip.framePacing': 'Activar sincronía de fotogramas',
    'tooltip.audioSync': 'Activar sincronización de audio',
    'tooltip.hdr': 'Activar HDR',
    'tooltip.language': 'Idioma',

    'nav.quitCurrentApp': 'Cerrar app actual',
    'aria.hostSelection': 'Selección de host',
    'aria.quitCurrentApp': 'Cerrar app actual',
    'aria.addHost': 'Añadir host',

    'host.addHost': 'Añadir host',
    'host.wakeWol': 'Encender PC (WOL)',
    'host.refreshBoxArt': 'Actualizar carátulas',
    'host.remove': 'Eliminar {host}',
    'host.settingsTitle': 'Configuración {host}',

    'dialog.pairing.title': 'Emparejamiento',
    'dialog.pairing.body': 'Introduce el siguiente PIN en el PC de destino: XXXX<br><br>Si tu PC host ejecuta Sunshine, abre la interfaz web de Sunshine para introducir el PIN.<br>Como alternativa, abre GeForce Experience (solo GPUs NVIDIA) para introducir el PIN.<br><br>Esta ventana se cerrará cuando finalice el emparejamiento.',
    'dialog.pairing.bodyPin': 'Introduce el siguiente PIN en el PC de destino:  {pin}<br><br>Si tu PC host ejecuta Sunshine, abre la interfaz web de Sunshine para introducir el PIN.<br>Como alternativa, abre GeForce Experience (solo GPUs NVIDIA) para introducir el PIN.<br><br>Esta ventana se cerrará cuando finalice el emparejamiento.',

    'dialog.restart.title': 'Reiniciar Moonlight',
    'dialog.restart.body': 'Tras cambiar el códec de vídeo, debes reiniciar la aplicación',

    'dialog.exit.title': 'Salir de Moonlight',
    'dialog.exit.body': '¿Seguro que quieres salir de Moonlight?',
    'dialog.exit.confirm': 'Salir',

    'dialog.quitApp.title': '¿Cerrar la app en ejecución?',
    'dialog.quitApp.placeholder': 'Ya hay una app en ejecución. ¿Quieres cerrarla?',
    'dialog.quitApp.bodyRunning': '{app} ya está en ejecución. ¿Quieres cerrar {app}?',
    'dialog.quitApp.bodyConfirm': ' ¿Seguro que quieres cerrar {app}?  Se perderán todos los datos no guardados.',

    'dialog.deleteHost.title': 'Eliminar host',
    'dialog.deleteHost.body': '¿Seguro que quieres eliminar este host?',
    'dialog.deleteHost.bodyHost': ' ¿Seguro que quieres eliminar {host}?',

    'dialog.addHost.title': 'Añadir host manualmente',
    'dialog.addHost.manualToggle': 'Introducir IP manualmente (pulsa Select)',
    'dialog.addHost.ipPlaceholder': 'Introduce la dirección IP del host',

    'status.loadingPlugin': 'Cargando el complemento de Moonlight...',
    'status.loadingApps': 'Cargando aplicaciones...',
    'status.pairingSuccess': 'Emparejamiento correcto',
    'status.startingApp': 'Iniciando {app}...',
    'status.stoppingApp': 'Cerrando {app}',
    'status.wolRequest': 'Enviando solicitud WOL a {host} con la dirección MAC {mac}',

    'error.certNotGenerated': 'ERROR: el certificado aún no se ha generado. ¿Se inicializó NaCl?',
    'error.failedConnect': '¡No se pudo conectar a {host}! Asegúrate de que Sunshine esté en ejecución en el PC host o de que GameStream esté activado en los ajustes SHIELD de GeForce Experience.',
    'error.pairingFailed': 'Fallo al emparejar con: {host}',
    'error.hostBusy': 'Error: {host} está ocupado.  Detén la transmisión para emparejar.',
    'error.pairFailedHost': 'Error: fallo al emparejar con {host}.',
    'error.emptyGameList': 'Tu lista de juegos está vacía',
    'error.retrieveGames': 'No se pudieron obtener tus juegos',
    'error.nothingRunning': 'No había nada en ejecución',
    'error.launchStatus': 'Error {code}: {message}',

    'stream.connectionTerminated': 'Conexión terminada',
    'stream.interrupted': 'Transmisión interrumpida. Volviendo al menú.',
    'stream.mouseEmuOn': 'Emulación de ratón activada',
    'stream.mouseEmuOff': 'Emulación de ratón desactivada',

    'app.runningSuffix': ' (En ejecución)',
  },
};

// Short labels shown on the language menu button (kept intentionally compact).
const I18N_LANG_LABELS = {
  'en': 'EN',
  'pt-BR': 'Pt-BR',
  'es': 'ES',
};

const I18n = (function () {
  const SUPPORTED = ['en', 'pt-BR', 'es'];
  const FALLBACK = 'en';
  const STORAGE_KEY = 'language';
  let current = FALLBACK;

  // Resolve the active language: stored preference first, then the platform
  // locale (Tizen exposes it through navigator.language), then fallback.
  function detect() {
    try {
      const saved = localStorage.getItem(STORAGE_KEY);
      if (saved && SUPPORTED.indexOf(saved) !== -1) {
        return saved;
      }
    } catch (e) { /* localStorage may be unavailable */ }

    const loc = (navigator.language || navigator.userLanguage || '').toLowerCase();
    if (loc.indexOf('pt') === 0) return 'pt-BR';
    if (loc.indexOf('es') === 0) return 'es';
    return FALLBACK;
  }

  function setCurrent(lang) {
    current = (SUPPORTED.indexOf(lang) !== -1) ? lang : FALLBACK;
  }

  function get() {
    return current;
  }

  // Translate a key, interpolating {placeholders} from vars. Falls back to the
  // English table and finally to the raw key so nothing ever renders blank.
  function t(key, vars) {
    const table = I18N_STRINGS[current] || I18N_STRINGS[FALLBACK];
    let str = table[key];
    if (str == null) {
      str = I18N_STRINGS[FALLBACK][key];
    }
    if (str == null) {
      str = key;
    }
    if (vars) {
      Object.keys(vars).forEach(function (name) {
        str = str.split('{' + name + '}').join(vars[name]);
      });
    }
    return str;
  }

  // Fill every tagged element in the document (or a subtree).
  function applyDom(root) {
    root = root || document;

    root.querySelectorAll('[data-i18n]').forEach(function (el) {
      el.textContent = t(el.getAttribute('data-i18n'));
    });
    root.querySelectorAll('[data-i18n-html]').forEach(function (el) {
      el.innerHTML = t(el.getAttribute('data-i18n-html'));
    });
    root.querySelectorAll('[data-i18n-aria]').forEach(function (el) {
      el.setAttribute('aria-label', t(el.getAttribute('data-i18n-aria')));
    });
    root.querySelectorAll('[data-i18n-placeholder]').forEach(function (el) {
      el.setAttribute('placeholder', t(el.getAttribute('data-i18n-placeholder')));
    });

    document.documentElement.setAttribute('lang', current);

    const langButton = document.getElementById('selectLanguage');
    if (langButton) {
      langButton.textContent = I18N_LANG_LABELS[current] || current;
      langButton.setAttribute('data-value', current);
    }
  }

  // Persist a language choice. The caller reloads so everything re-renders.
  function save(lang) {
    try {
      localStorage.setItem(STORAGE_KEY, lang);
    } catch (e) { /* best effort */ }
  }

  return {
    t: t,
    applyDom: applyDom,
    detect: detect,
    setCurrent: setCurrent,
    get: get,
    save: save,
    SUPPORTED: SUPPORTED,
  };
})();

// Global shorthand used throughout the platform scripts.
function t(key, vars) {
  return I18n.t(key, vars);
}

// Resolve the language as early as possible so t() is correct for any code
// that runs during startup, then localize the static markup once it is parsed.
I18n.setCurrent(I18n.detect());
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', function () { I18n.applyDom(); });
} else {
  I18n.applyDom();
}
